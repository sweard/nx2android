// An actual Horizon CreateThread runs the title's own nn::crypto SHA-256 on
// its mapped stack. Capture in the middle of that call, while its source
// pointer is already inside the input. A fresh HLE thread returns through a
// signal frame onto that original stack and completes the in-flight hash.
#include "dm.h"
#include "checkpoint_cpu.h"
#include <CommonCrypto/CommonDigest.h>
#include <errno.h>
#include <stddef.h>
#include <sys/mman.h>
#include <unistd.h>

void guest_thread_init(void);
uint32_t svc_thread_main_init(void);
uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr);
int thr_list(pthread_t *out, int max);
extern const uint8_t sdk_caller_begin[], sdk_caller_end[];
enum { source_bytes = 16 << 20, stack_bytes = 64 << 10 };

typedef struct {
    uintptr_t function, source;
    uint64_t size;
    _Atomic uint32_t started, done;
    unsigned char digest[32];
    dm_checkpoint_cpu state;
    uint32_t handle;
} hash_job;
_Static_assert(offsetof(hash_job, state) == 64, "SDK caller layout");
static hash_job *job;
static uintptr_t sdk_base, sdk_end, stack_top;
uintptr_t sdk_cv_entry, sdk_clock_entry;
extern const uint8_t sdk_clock_caller[];
extern const uint8_t sdk_cv_caller[];
static int loading, parked[2];
static _Atomic int captured;

static int inside_hash(const dm_checkpoint_cpu *r) {
    if (r->pc < sdk_base || r->pc >= sdk_end || (r->pc & 3) ||
        r->sp < stack_top - stack_bytes || r->sp >= stack_top || (r->sp & 15)) return 0;
    // A source cursor past the first page makes overwriting the first cache
    // line after restore a test of continuation vs. starting the hash again.
    for (int i = 0; i < 31; ++i) {
        if (r->x[i] > job->source + 4096 && r->x[i] < job->source + source_bytes / 2) return 1;
    }
    return 0;
}

static void sample(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    if (loading) {
        if (dm_checkpoint_cpu_transfer(context, &job->state, 1)) _exit(71);
        return;
    }
    dm_checkpoint_cpu state;
    if (dm_checkpoint_cpu_transfer(context, &state, 0)) _exit(72);
    if (!inside_hash(&state)) return;
    job->state = state;
    atomic_store(&captured, 1);
    char release;
    while (read(parked[0], &release, 1) < 0 && errno == EINTR) {}
}

static void init_restore(void *arg) {
    (void)arg;
    raise(SIGUSR1);
    _exit(73);
}

void sdk_worker_checkpoint(uintptr_t heap, uintptr_t stack, int restore) {
    job = (hash_job *)(heap + 0x4000);
    stack_top = stack + 0x20000;
    loading = restore;
    const size_t page = getpagesize();
    uint8_t *wish = (void *)UINT64_C(0x290000000);
    uint8_t *caller = mmap(wish, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (caller != wish || (uintptr_t)sdk_caller_end - (uintptr_t)sdk_caller_begin > page) die("SDK caller reservation");
    sdk_clock_entry = (uintptr_t)caller + ((uintptr_t)sdk_clock_caller - (uintptr_t)sdk_caller_begin);
    sdk_cv_entry = (uintptr_t)caller + ((uintptr_t)sdk_cv_caller - (uintptr_t)sdk_caller_begin);
    memcpy(caller, sdk_caller_begin, (uintptr_t)sdk_caller_end - (uintptr_t)sdk_caller_begin);
    __builtin___clear_cache((char *)caller, (char *)caller + page);
    if (mprotect(caller, page, PROT_READ | PROT_EXEC)) die("SDK caller code");
    guest_thread_init();
    svc_thread_main_init();
    uintptr_t sha = (uintptr_t)guest_sym("_ZN2nn6crypto18GenerateSha256HashEPvmPKvm");
    module_t *sdk = module_of((void *)sha);
    if (!sdk || strcmp(sdk->name, "sdk")) die("SDK hash unavailable");
    sdk_base = (uintptr_t)sdk->base;
    sdk_end = sdk_base + sdk->text_end;
    struct sigaction sa = {.sa_sigaction = sample, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGUSR1, &sa, NULL) || pipe(parked)) die("worker signal setup");
    uint8_t expected[32];
    uintptr_t src = heap + (1 << 20);
    if (!restore) {
        memset((void *)src, 0x5a, source_bytes);
        job->function = sha;
        job->source = src;
        job->size = source_bytes;
    } else {
        if (job->source != src || job->size != source_bytes || job->function != sha ||
            !job->started || job->done || !inside_hash(&job->state)) die("incompatible SDK CPU state");
        CC_SHA256((const void *)src, source_bytes, expected);
        memset((void *)src, 0x7c, 64);
    }
    uint8_t *bootstrap = restore ? malloc(stack_bytes) : NULL;
    if (restore && !bootstrap) die("bootstrap stack");
    uint64_t regs[32] = {0, restore ? (uintptr_t)init_restore : (uintptr_t)caller, (uintptr_t)job,
                         restore ? (uintptr_t)(bootstrap + stack_bytes) : stack_top, 44, 0};
    svc_dispatch(0x08, regs, 0);
    if (regs[0]) die("CreateThread");
    if (restore && regs[1] != job->handle) die("different Horizon handle");
    job->handle = (uint32_t)regs[1];
    regs[0] = job->handle;
    svc_dispatch(0x09, regs, 0);
    if (regs[0]) die("StartThread");
    uint64_t deadline = now_ns() + 5ull * 1000000000;
    if (restore) {
        while (!job->done && now_ns() < deadline) usleep(100);
        if (!job->done || memcmp(expected, job->digest, sizeof expected)) die("resumed hash did not match");
        printf("SDK worker %#x resumed at sdk+%#lx; SHA-256 matched after changing consumed input.\n",
               job->handle, (unsigned long)(job->state.pc - sdk_base));
        return;
    }
    while (!job->started && now_ns() < deadline) usleep(10);
    pthread_t threads[2]; int n = thr_list(threads, 2);
    if (!job->started || n != 2) die("worker did not start");
    pthread_t worker = pthread_equal(threads[0], pthread_self()) ? threads[1] : threads[0];
    while (!atomic_load(&captured) && !job->done && now_ns() < deadline) {
        if (pthread_kill(worker, SIGUSR1)) die("cannot signal SDK worker");
        usleep(50);
    }
    if (!atomic_load(&captured)) die("could not stop in the original SDK hash loop");
}
