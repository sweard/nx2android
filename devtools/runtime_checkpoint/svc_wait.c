// Original and restored pthreads must coordinate through the normal Horizon
// SVCs even if an event was signalled before a restored worker started.
#include "dm.h"
#include "checkpoint_wait.h"
#include "checkpoint_memory.h"
#include "checkpoint_wait_cpu.h"
#include <signal.h>
#include <stdatomic.h>
#include <unistd.h>

uint64_t svc_dispatch(int, uint64_t *, uint64_t);
typedef struct {
    dm_checkpoint_wait records[4];
    uint32_t count, lock, key, handles[2], writers[2], workers[3];
    uint64_t cv_job[6];
    dm_checkpoint_wait_cpu cpu;
} wait_case;
static wait_case *state;
static int loading;
extern uintptr_t sdk_cv_entry;
void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);
static uintptr_t cv_sp;
static void resume_gate(int sig, siginfo_t *si, void *u) {
    (void)sig; (void)si;
    if (dm_checkpoint_cpu_transfer(u, &state->cpu.cpu, 1)) _exit(74);
}
static uintptr_t sdk_leaf_site(const char *mods, const char *expected, unsigned arg) {
    char path[1024], line[256], kind[24];
    snprintf(path, sizeof path, "%s/sdk.gates.tsv", mods);
    FILE *fp = fopen(path, "r");
    module_t *sdk = module_of(guest_sym("_ZN2nn6crypto18GenerateSha256HashEPvmPKvm"));
    if (!fp || !sdk) die("SDK gate table missing");
    unsigned long ret, orig; unsigned svc;
    while (fgets(line, sizeof line, fp)) {
        if (sscanf(line, "%lx %23s %u %lx", &ret, kind, &svc, &orig) != 4 ||
            strcmp(kind, expected) || svc != arg || orig + 8 > sdk->text_end) continue;
        // A genuine SDK WaitProcessWideKeyAtomic leaf which returns at orig+4.
        if (*(uint32_t *)(sdk->base + orig + 4) == 0xd65f03c0) {
            fclose(fp); return (uintptr_t)sdk->base + orig;
        }
    }
    die("SDK %s %u leaf not found", expected, arg);
}

static _Atomic unsigned done;
static int call(int num, uint64_t *regs) {
    svc_dispatch(num, regs, 0);
    return (int)regs[0];
}

static void waiter(uintptr_t index) {
    uint64_t regs[32] = {0};
    if (!index) {
        if (loading) {
            int rc = dm_checkpoint_wait_cpu_finish(&state->cpu);
            if (rc) die("SDK wait continuation refused %d", rc);
            uint64_t *tls = dm_tsd_get(g_keytls);
            uint8_t *tlr = dm_tsd_get(g_keytlr);
            if (tls[2] != UINT64_C(0xfedcb12365) || *(void **)(tlr + 0x1f8) != tlr + 0x200) die("guest TLS/TLR contents lost");
            raise(SIGUSR1);
        } else {
            uint64_t *tls = dm_tsd_get(g_keytls);
            tls[2] = UINT64_C(0xfedcb12365);
            dm_thread_enter(sdk_cv_entry, (uintptr_t)state->cv_job, cv_sp);
        }
        die("SDK wait trampoline returned");
    }
    if (loading) {
        if (dm_checkpoint_wait_finish(regs)) die("could not resume waiter");
    } else if (index < 2) {
        regs[0] = (uintptr_t)&state->lock; regs[1] = (uintptr_t)&state->key;
        regs[2] = state->workers[index]; regs[3] = UINT64_MAX;
        call(0x1c, regs);
    } else {
        regs[1] = (uintptr_t)state->handles; regs[2] = 2; regs[3] = UINT64_MAX;
        call(0x18, regs);
    }
    if (regs[0] || (index == 2 && regs[1] != 1) ||
        (index < 2 && __atomic_load_n(&state->lock, __ATOMIC_SEQ_CST) != state->workers[index])) die("incorrect wait result");
    atomic_fetch_or(&done, 1u << index);
    call(0x0a, regs);
}

static void start_worker(unsigned index) {
    uint64_t r[32] = {state->workers[index]};
    if (call(0x09, r)) die("StartThread waiter");
}
static unsigned completed(void) {
    return atomic_load(&done) | (__atomic_load_n(&state->cv_job[5], __ATOMIC_ACQUIRE) ? 1u : 0u);
}
static void until_done(unsigned mask) {
    uint64_t until = now_ns() + 2000000000;
    while ((completed() & mask) != mask && now_ns() < until) usleep(100);
    if ((completed() & mask) != mask) die("lost wake");
}
void svc_wait_checkpoint(uintptr_t heap, uintptr_t stack, const char *mods, int events_fd, int restore) {
    state = (void *)(heap + 0x8000); loading = restore;
    cv_sp = stack + 0x40000;
    if (restore) {
        struct sigaction sa = {.sa_flags = SA_SIGINFO | SA_ONSTACK, .sa_sigaction = resume_gate};
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGUSR1, &sa, NULL)) die("wait signal");
    }
    for (unsigned i = 0; i < 3; ++i) {
        void *stack = malloc(1 << 16);
        if (!stack) die("thread stack");
        uint64_t r[32] = {0, (uintptr_t)waiter, i, (uintptr_t)stack + (1 << 16), 44, 0};
        if (call(0x08, r)) die("CreateThread waiter");
        if (restore && state->workers[i] != r[1]) die("thread handle mismatch");
        state->workers[i] = r[1];
    }
    if (restore) {
        if (dm_checkpoint_events_restore(events_fd)) {
            fprintf(stderr, "event table restore refused\n"); _exit(1);
        }
        int rc = dm_checkpoint_waits_install(state->records, state->count);
        if (rc) {
            for (unsigned i = 0; i < state->count && i < 4; ++i) {
                dm_checkpoint_wait *w = &state->records[i];
                fprintf(stderr, "handle=%x kind=%u prio=%d count=%u address=%lx key=%lx handles=%lx seq=%lu remaining=%ld\n",
                    w->thread, w->kind, w->priority, w->count, w->address, w->key, w->handles, w->sequence, w->remaining_ns);
            }
            die("wait install refused (%d, count %u)", rc, state->count);
        }
        uint64_t r[32] = {state->writers[1]};
        if (call(0x11, r)) die("event signal");
        // The first CV waiter was already signalled under contention when saved.
        if (state->lock != (0x1001 | 0x40000000)) die("missing saved owner");
        r[0] = (uintptr_t)&state->lock;
        if (call(0x1b, r) || state->lock != state->workers[0]) die("wrong handoff");
        for (unsigned i = 0; i < 3; ++i) start_worker(i);
        until_done(5);
        if (state->cv_job[4] || completed() != 5) die("second CV waiter woke early");
        r[0] = (uintptr_t)&state->key; r[1] = 1;
        if (call(0x1d, r)) die("CV signal");
        r[0] = (uintptr_t)&state->lock;
        if (call(0x1b, r)) die("unlock");
        until_done(7);
        puts("Original SDK WaitProcessWideKeyAtomic returned to its guest caller on the restored stack.");
        puts("Restored CV-to-mutex handoff, FIFO wait and event handle index 1 after an early signal.");
    } else {
        state->cv_job[0] = sdk_leaf_site(mods, "svc", 0x1c);
        state->cv_job[1] = (uintptr_t)&state->lock;
        state->cv_job[2] = (uintptr_t)&state->key;
        state->cv_job[3] = state->workers[0];
        state->cv_job[4] = UINT64_MAX;
        for (unsigned i = 0; i < 2; ++i) {
            uint64_t r[32] = {0};
            if (call(0x45, r)) die("CreateEvent");
            state->writers[i] = r[1]; state->handles[i] = r[2];
        }
        for (unsigned i = 0; i < 3; ++i) {
            if (i < 2) state->lock = state->workers[i];
            start_worker(i);
            uint64_t until = now_ns() + 2000000000;
            do {
                if (dm_checkpoint_waits_capture(state->records, 4, &state->count)) die("wait capture");
                if (state->count == i + 1) break;
                usleep(100);
            } while (now_ns() < until);
            if (state->count != i + 1) die("worker did not park");
        }
        state->lock = 0x1001;
        uint64_t r[32] = {(uintptr_t)&state->key, 1};
        if (call(0x1d, r) || dm_checkpoint_waits_capture(state->records, 4, &state->count) ||
            state->count != 3 || dm_checkpoint_wait_cpu_capture(state->workers[0], &state->cpu) ||
            state->cpu.cpu.pc != state->cv_job[0] + 4 || state->cpu.tls != state->cpu.owner || state->cpu.tlr != state->cpu.owner + 8192 ||
            dm_checkpoint_events_save(events_fd)) die("wait/event save");
    }
}


// Run an SDK hardware-counter leaf on the checkpoint's main-stack region.
// Only the saved guest continuation can finish this caller in the new PID.
static struct {
    uint64_t site, ticks;
    uint32_t entries;
    _Atomic uint32_t done;
    dm_checkpoint_wait_cpu thread;
} *clock_job;
extern uintptr_t sdk_clock_entry;
static void clock_signal(int sig, siginfo_t *info, void *uc) {
    (void)sig; (void)info;
    if (dm_checkpoint_cpu_transfer(uc, &clock_job->thread.cpu, 1)) _exit(76);
}
static void restore_clock(void *unused) {
    (void)unused;
    if (dm_checkpoint_clock_cpu_finish(&clock_job->thread)) die("clock thread attach");
    raise(SIGUSR1);
    die("clock transfer returned");
}
void svc_clock_checkpoint(uintptr_t heap, const char *mods, int restore) {
    clock_job = (void *)(heap + 0xc000);
    uintptr_t stack = dm_checkpoint_main_stack();
    if (!stack) die("checkpoint main stack unavailable");
    if (!restore) clock_job->site = sdk_leaf_site(mods, "cntpct", 0);
    else {
        struct sigaction sa = {.sa_flags = SA_SIGINFO | SA_ONSTACK, .sa_sigaction = clock_signal};
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGUSR1, &sa, NULL)) die("clock signal");
    }
    guest_clock_freeze_for(1, 1);
    uintptr_t bootstrap = restore ? (uintptr_t)malloc(65536) : 0;
    if (restore && !bootstrap) die("clock bootstrap stack");
    uint64_t r[32] = {0, restore ? (uintptr_t)restore_clock : sdk_clock_entry,
                      (uintptr_t)clock_job, restore ? bootstrap + 65536 : stack + DM_CHECKPOINT_MAIN_STACK_BYTES, 44, 0};
    if (call(0x08, r) || (restore && r[1] != clock_job->thread.handle)) die("clock thread handle");
    r[0] = r[1];
    if (call(0x09, r)) die("clock thread start");
    uint64_t until = now_ns() + 2000000000;
    if (restore) {
        guest_clock_freeze_for(1, 0);
        while (!clock_job->done && now_ns() < until) usleep(100);
        if (!clock_job->done || clock_job->entries != 1 || !clock_job->ticks) die("clock caller did not continue: done=%u entries=%u ticks=%llu", atomic_load(&clock_job->done), clock_job->entries, (unsigned long long)clock_job->ticks);
        puts("SDK counter read resumed once on the restored 16 MiB main-stack region.");
    } else {
        unsigned count = 0;
        do {
            if (dm_checkpoint_clock_cpus(&clock_job->thread, 1, &count)) die("clock frame unavailable");
            if (!count) usleep(100);
        } while (!count && now_ns() < until);
        if (count != 1 || clock_job->thread.cpu.pc != clock_job->site || clock_job->done ||
            clock_job->entries != 1 || clock_job->thread.cpu.sp <= stack ||
            clock_job->thread.cpu.sp > stack + DM_CHECKPOINT_MAIN_STACK_BYTES) die("incorrect clock continuation");
    }
}
