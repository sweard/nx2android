// Android/AArch64 runtime checkpoint probe with two guest threads, a guest
// address range and a Vulkan buffer mapped into the guest. It runs without a
// game or root.
#define _GNU_SOURCE
#include "../../nxnative/dm/android/checkpoint_cpu.h"
#include "probe_gpu.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

enum { WORKERS = 2, STACK_BYTES = 64 * 1024, HEAP_BYTES = 128 * 1024 * 1024 };
static const uintptr_t guest_base = UINT64_C(0x1800000000);
static const uint64_t nzcv_mask = UINT64_C(0xf0000000);
static const char snapshot_name[] = "runtime.nx2c";

extern const unsigned char probe_guest_start[], probe_guest_wait[], probe_guest_wait_end[], probe_guest_end[];
extern _Noreturn void probe_guest_enter(void *code, void *stack_top, void *mailbox);

typedef struct {
    uint64_t seed;
    _Atomic uint32_t waiting, resume;
    uint64_t result_x19, result_stack, result_simd;
    uint32_t result_z;
    _Atomic uint32_t done;
    uint64_t gpu_address;
    uint32_t result_gpu;
} mailbox;

_Static_assert(offsetof(mailbox, waiting) == 8 && offsetof(mailbox, resume) == 12 &&
               offsetof(mailbox, result_x19) == 16 && offsetof(mailbox, result_stack) == 24 &&
               offsetof(mailbox, result_simd) == 32 && offsetof(mailbox, result_z) == 40 &&
               offsetof(mailbox, done) == 44 && offsetof(mailbox, gpu_address) == 48 &&
               offsetof(mailbox, result_gpu) == 56, "guest assembly mailbox offsets");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "signal communication must be lock-free");

typedef struct {
    char magic[8];
    uint32_t page_bytes, code_crc, payload_crc, save_pid;
    uint32_t vendor, device, driver;
    uint8_t driver_uuid[VK_UUID_SIZE];
    uint32_t gpu[PROBE_GPU_WORDS];
    dm_checkpoint_cpu thread[WORKERS];
} snapshot;

typedef struct {
    mailbox *box;
    unsigned char *stack_top;
    dm_checkpoint_cpu *saved;
    pthread_t tid;
    int notice[2], park[2], restore;
} worker;

static const char magic[8] = {'N','X','2','C','T','X','2','\0'};
static _Thread_local worker *current;
static unsigned char *code, *data;
static size_t page_bytes;
static const size_t data_bytes = WORKERS * STACK_BYTES + HEAP_BYTES;

static _Noreturn void error(const char *message) {
    fprintf(stderr, "runtime checkpoint probe: %s\n", message);
    exit(1);
}

static uint64_t now_ms(void) {
    struct timespec time;
    if (clock_gettime(CLOCK_MONOTONIC, &time)) error("monotonic clock unavailable");
    return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static uintptr_t guest_address(const unsigned char *symbol) {
    return guest_base + (uintptr_t)symbol - (uintptr_t)probe_guest_start;
}

static int valid_context(const worker *w, const dm_checkpoint_cpu *ctx) {
    return ctx->pc >= guest_address(probe_guest_wait) && ctx->pc < guest_address(probe_guest_wait_end) &&
           !(ctx->pc & 3) && ctx->sp == (uintptr_t)w->stack_top - 16 &&
           ctx->x[0] == (uintptr_t)w->box && ctx->x[14] == (uintptr_t)&w->box->resume &&
           ctx->x[9] == 1 && ctx->x[23] == (uintptr_t)(data + data_bytes) && !(ctx->nzcv & ~nzcv_mask);
}

static void signal_context(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)info;
    worker *w = current;
    if (!w) _exit(2);
    ucontext_t *uc = context;
    dm_checkpoint_cpu *saved = w->saved;
    if (dm_checkpoint_cpu_transfer(uc, saved, w->restore)) _exit(3);
    if (w->restore) return;
    char reply = valid_context(w, saved) ? '+' : '-';
    int sent;
    do { sent = (int)write(w->notice[1], &reply, 1); } while (sent < 0 && errno == EINTR);
    if (sent != 1) _exit(4);
    // Acknowledging a capture parks this guest while the host copies its stack
    // and heap. The handler and its kernel signal frame live on a host alt stack.
    char release;
    ssize_t n;
    do { n = read(w->park[0], &release, 1); } while (n < 0 && errno == EINTR);
    if (n != 1) _exit(5);
}

static void *run_worker(void *argument) {
    current = argument;
    void *alternate = mmap(NULL, STACK_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (alternate == MAP_FAILED) error("cannot allocate signal stack");
    stack_t stack = { .ss_sp = alternate, .ss_size = STACK_BYTES };
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGUSR1);
    if (sigaltstack(&stack, NULL) || pthread_sigmask(SIG_UNBLOCK, &signals, NULL)) error("cannot install worker signal stack");
    if (current->restore) {
        raise(SIGUSR1);
        error("signal return did not resume the saved guest instruction");
    }
    probe_guest_enter(code, current->stack_top, current->box);
}

static void wait_flag(const _Atomic uint32_t *value, const char *what) {
    uint64_t deadline = now_ms() + 5000;
    while (!atomic_load_explicit(value, memory_order_acquire)) {
        if (now_ms() >= deadline) error(what);
        usleep(1000);
    }
}

static void setup_guest(void) {
    long page = sysconf(_SC_PAGESIZE);
    if (page < 4096 || page > STACK_BYTES || STACK_BYTES % page != 0) error("unexpected Android page size");
    page_bytes = (size_t)page;
    size_t code_size = (uintptr_t)probe_guest_end - (uintptr_t)probe_guest_start;
    if (code_size > page_bytes) error("guest code does not fit");
    size_t total = page_bytes + data_bytes + PROBE_GPU_BYTES;
    code = mmap((void *)guest_base, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((uintptr_t)code != guest_base) error("guest address is occupied in the new process");
    memcpy(code, probe_guest_start, code_size);
    __builtin___clear_cache((char *)code, (char *)code + code_size);
    if (mprotect(code, page_bytes, PROT_READ | PROT_EXEC)) error("cannot activate guest code");
    data = code + page_bytes;
    struct sigaction action = { .sa_sigaction = signal_context, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL)) error("cannot install checkpoint signal");
}

static void transfer_file(int fd, void *data_to_move, size_t length, int writing) {
    unsigned char *p = data_to_move;
    while (length) {
        ssize_t n = writing ? write(fd, p, length) : read(fd, p, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) error(writing ? "snapshot write failed" : "snapshot is truncated");
        p += n;
        length -= (size_t)n;
    }
}

static uint32_t payload_crc(snapshot *s) {
    s->payload_crc = 0;
    uLong crc = crc32(0, (const Bytef *)s, sizeof *s);
    return (uint32_t)crc32(crc, data, data_bytes);
}

static void write_snapshot(int root, snapshot *s) {
    s->payload_crc = payload_crc(s);
    char staging_name[64];
    snprintf(staging_name, sizeof staging_name, ".runtime.nx2c.%" PRIu32 ".part", s->save_pid);
    int fd = openat(root, staging_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) error("cannot create snapshot staging file");
    transfer_file(fd, s, sizeof *s, 1);
    transfer_file(fd, data, data_bytes, 1);
    if (fsync(fd) || close(fd)) error("snapshot flush failed");
    if (renameat(root, staging_name, root, snapshot_name) || fsync(root)) error("cannot publish snapshot");
}

static void read_snapshot(int root, snapshot *s, uint32_t code_crc) {
    int fd = openat(root, snapshot_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != (off_t)(sizeof *s + data_bytes)) {
        error("snapshot missing or has the wrong size");
    }
    transfer_file(fd, s, sizeof *s, 0);
    if (memcmp(s->magic, magic, sizeof magic) || s->page_bytes != page_bytes || s->code_crc != code_crc) {
        error("snapshot is incompatible with this guest code or page layout");
    }
    transfer_file(fd, data, data_bytes, 0);
    if (close(fd)) error("cannot close snapshot");
    uint32_t checksum = s->payload_crc;
    if (payload_crc(s) != checksum) error("snapshot checksum does not match");
}

static void start_workers(worker w[WORKERS], snapshot *s, int restore) {
    for (int i = 0; i < WORKERS; ++i) {
        w[i].box = (mailbox *)(data + WORKERS * STACK_BYTES) + i;
        w[i].stack_top = data + (i + 1) * STACK_BYTES;
        w[i].saved = &s->thread[i];
        w[i].restore = restore;
        if (restore) {
            if (!valid_context(&w[i], w[i].saved) || atomic_load(&w[i].box->waiting) != 1 ||
                atomic_load(&w[i].box->resume) || atomic_load(&w[i].box->done)) error("invalid saved guest thread");
            atomic_store(&w[i].box->resume, 1);
        } else {
            w[i].box->seed = (uint64_t)s->save_pid * 1024 + (uint64_t)(i + 1);
            w[i].box->gpu_address = (uintptr_t)(data + data_bytes);
            if (pipe(w[i].notice) || pipe(w[i].park)) error("cannot create checkpoint rendezvous");
        }
        if (pthread_create(&w[i].tid, NULL, run_worker, &w[i])) error("cannot start guest worker");
    }
}

static void capture_threads(worker w[WORKERS]) {
    for (int i = 0; i < WORKERS; ++i) wait_flag(&w[i].box->waiting, "guest worker never entered its checkpoint loop");
    for (int i = 0; i < WORKERS; ++i) {
        if (pthread_kill(w[i].tid, SIGUSR1)) error("cannot request guest context");
    }
    for (int i = 0; i < WORKERS; ++i) {
        struct pollfd event = { .fd = w[i].notice[0], .events = POLLIN };
        char reply = 0;
        if (poll(&event, 1, 5000) != 1 || read(event.fd, &reply, 1) != 1 || reply != '+') {
            error("guest worker did not park at a supported instruction");
        }
    }
}

static void verify_workers(worker w[WORKERS], uint32_t gpu_word) {
    for (int i = 0; i < WORKERS; ++i) {
        mailbox *box = w[i].box;
        wait_flag(&box->done, "restored guest worker did not finish");
        uint64_t expected = box->seed + 37;
        if (box->result_x19 != expected || box->result_stack != 2 * expected ||
            box->result_simd != UINT64_C(0x5a5a5a5a5a5a5a5a) || box->result_z != 1 ||
            box->result_gpu != gpu_word) {
            error("restored guest register, stack, vector, condition flag, or GPU-backed memory mismatch");
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 3 || (strcmp(argv[1], "save") && strcmp(argv[1], "restore"))) {
        fprintf(stderr, "usage: %s save|restore <existing scratch directory>\n", argv[0]);
        return 2;
    }
    int restore = !strcmp(argv[1], "restore");
    int root = open(argv[2], O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0) error("cannot open scratch directory");
    setup_guest();
    size_t code_size = (uintptr_t)probe_guest_end - (uintptr_t)probe_guest_start;
    uint32_t code_crc = (uint32_t)crc32(0, code, code_size);
    snapshot s;
    memset(&s, 0, sizeof s);
    worker w[WORKERS] = {0};
    probe_gpu gpu = {0};
    uint64_t start = now_ms();
    if (restore) {
        read_snapshot(root, &s, code_crc);
        if (s.save_pid == (uint32_t)getpid()) error("restore must use a new process");
        probe_gpu_open(&gpu, data + data_bytes);
        if (s.vendor != gpu.props.vendorID || s.device != gpu.props.deviceID || s.driver != gpu.props.driverVersion ||
            memcmp(s.driver_uuid, gpu.props.pipelineCacheUUID, sizeof s.driver_uuid)) {
            error("Vulkan device or driver changed");
        }
        probe_gpu_restore(&gpu, s.gpu);
        uint32_t readback[PROBE_GPU_WORDS];
        probe_gpu_capture(&gpu, readback);
        if (memcmp(readback, s.gpu, sizeof readback) || memcmp(gpu.guest, s.gpu, sizeof s.gpu)) {
            error("restored guest mapping or Vulkan buffer contents differ");
        }
        start_workers(w, &s, 1);
        verify_workers(w, s.gpu[0]);
        printf("restored pid=%d old_pid=%" PRIu32 " threads=%d gpu=%s bytes=%zu restore_ms=%" PRIu64 "\n",
               getpid(), s.save_pid, WORKERS, gpu.props.deviceName, sizeof s + data_bytes, now_ms() - start);
        probe_gpu_close(&gpu);
        close(root);
        return 0;
    }

    memcpy(s.magic, magic, sizeof s.magic);
    s.page_bytes = (uint32_t)page_bytes;
    s.code_crc = code_crc;
    s.save_pid = (uint32_t)getpid();
    // Commit the fixture's heap pages so the timings include real data I/O.
    memset(data + WORKERS * STACK_BYTES, 0xa5, HEAP_BYTES);
    memset(data + WORKERS * STACK_BYTES, 0, WORKERS * sizeof(mailbox));
    probe_gpu_open(&gpu, data + data_bytes);
    s.vendor = gpu.props.vendorID;
    s.device = gpu.props.deviceID;
    s.driver = gpu.props.driverVersion;
    memcpy(s.driver_uuid, gpu.props.pipelineCacheUUID, sizeof s.driver_uuid);
    probe_gpu_seed(&gpu, s.save_pid ^ UINT32_C(0xaa55aa55));
    start_workers(w, &s, 0);
    start = now_ms();
    capture_threads(w);
    probe_gpu_capture(&gpu, s.gpu);
    for (size_t i = 0; i < PROBE_GPU_WORDS; ++i) {
        uint32_t expected = s.save_pid ^ UINT32_C(0xaa55aa55);
        if (i >= PROBE_GPU_WORDS / 2) expected = ~expected;
        if (s.gpu[i] != expected) error("capture of GPU-generated buffer contents failed");
    }
    if (memcmp(gpu.guest, s.gpu, sizeof s.gpu)) error("GPU writes are not visible in the guest mapping");
    write_snapshot(root, &s);
    printf("saved pid=%" PRIu32 " threads=%d gpu=%s bytes=%zu save_ms=%" PRIu64 "\n",
           s.save_pid, WORKERS, gpu.props.deviceName, sizeof s + data_bytes, now_ms() - start);
    fflush(stdout);
    // Leave the original process alive so the tester can explicitly kill it.
    for (;;) pause();
}
