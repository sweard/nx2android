// Host-initiated guest threads (host background work that calls the game's own allocator / locks, such as the movie player's, needs a complete SDK thread context).
// Approach: on a guest thread (e.g. before guest_main enters nnMain) call the SDK's nn::os::CreateThread / StartThread directly, with a host function as the entry --
// the thread is created by the SDK, so ThreadType, thread handle and TLS slots are all in place, and the game's malloc / nn::os locks work as usual (the lock tag word holds this thread's guest handle).
// Constraints: must be called on a guest thread (SDK thread creation goes through the SVC gate); not exposed to the game.
// Self-test DM_GUEST_SPAWN_TEST=1: before guest_main enters nnMain, start one thread that does "SDK malloc/free + lock an nn::os mutex and check that the owner is itself", and report the result after WaitThread finishes.
#include "hle.h"
#if defined(__ANDROID__) && defined(DM_MANUAL_CHECKPOINT)
#include "../android/checkpoint_memory.h"
#endif
#include <stdatomic.h>
#include <unistd.h>
typedef int (*create_fn)(void *tt, void (*fn)(void *), void *arg, void *stack, size_t size, int prio);
typedef void (*tt_fn)(void *tt);
static void *need(const char *n) { void *p = guest_sym(n); if (!p) die("host guest thread: missing symbol %s", n); return p; }
// an entry shim: first register the thread as host-created (hle_thread.c dm_yield_to_core_peers does not wait for them), then run fn
typedef struct { void (*fn)(void *); void *arg; } spawn_arg;
void dm_thread_mark_host(void);
#if defined(__ANDROID__) && defined(DM_MANUAL_CHECKPOINT)
// Entered directly by the SDK. Keep the AAPCS64 nonvolatile registers at
// this boundary so a rebuilt host service can return to the original SDK
// caller without unwinding any frames from the old Android process.
void dm_checkpoint_audio_entry(void (*fn)(void *), const uint64_t *regs);
__attribute__((used,noinline)) static void checkpoint_spawn_body(void *p, const uint64_t *regs) {
    spawn_arg a = *(spawn_arg *)p;
    free(p);
    dm_thread_mark_host();
    dm_checkpoint_audio_entry(a.fn, regs);
    a.fn(a.arg);
}
__attribute__((naked)) static void spawn_tramp(void *p) {
    __asm__ volatile(
        "sub sp, sp, #160\n"
        "stp x19,x20,[sp]\n stp x21,x22,[sp,#16]\n stp x23,x24,[sp,#32]\n"
        "stp x25,x26,[sp,#48]\n stp x27,x28,[sp,#64]\n stp x29,x30,[sp,#80]\n"
        "stp d8,d9,[sp,#96]\n stp d10,d11,[sp,#112]\n"
        "stp d12,d13,[sp,#128]\n stp d14,d15,[sp,#144]\n"
        "mov x1,sp\n bl checkpoint_spawn_body\n"
        "ldp x29,x30,[sp,#80]\n add sp,sp,#160\n ret");
}
#else
static void spawn_tramp(void *p) { spawn_arg a = *(spawn_arg *)p; free(p); dm_thread_mark_host(); a.fn(a.arg); }
#endif
// What one spawn makes: the ThreadType first (its address is the handle the caller keeps), then its stack. A joined thread's go to a free list for the next spawn of that stack size
// rather than to free(): the thread exits on that stack (hle_thread.c thread_exit, pthread_exit after WaitThread can return), and reuse only happens at a later spawn. Freed never,
// every OpenAudioRenderer after a CloseAudioRenderer (hle_audren.c: the render thread) left a ThreadType and a 64 KB stack behind
typedef struct { uint8_t tt[0x200]; void *stack; size_t stack_size; uint64_t joined_ns; } gthr_t;   // tt: nn::os::ThreadType (0x1c0 bytes, with headroom); joined_ns: when its thread was joined
enum { GTHR_REUSE_NS = 100 * 1000000 };   // WaitThread returns when the thread marks itself exited, still on its stack (hle_thread.c thread_exit: pthread_exit and the key destructors run there): a stack is
                                          // taken again only this long after the join. (A game frees its own threads' stacks right after WaitThread, with the same window: not this file's)
static gthr_t *g_gfree[8]; static int g_ngfree; static pthread_mutex_t g_gmu = PTHREAD_MUTEX_INITIALIZER;
void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size) {
    spawn_arg *sa = malloc(sizeof *sa); if (!sa) die("host guest thread: allocation failed"); sa->fn = fn; sa->arg = arg; fn = spawn_tramp; arg = sa;
    stack_size = (stack_size + 0xfff) & ~(size_t)0xfff;
    gthr_t *g = NULL;
    uint64_t now = now_ns();
    pthread_mutex_lock(&g_gmu); for (int i = 0; i < g_ngfree && !g; i++) if (g_gfree[i]->stack_size == stack_size && now - g_gfree[i]->joined_ns > GTHR_REUSE_NS) { g = g_gfree[i]; g_gfree[i] = g_gfree[--g_ngfree]; } pthread_mutex_unlock(&g_gmu);
    if (g) memset(g->tt, 0, sizeof g->tt);
    else {
#if defined(__ANDROID__) && defined(DM_MANUAL_CHECKPOINT)
        void *block = NULL;
        int rc = stack_size <= SIZE_MAX - 16384 ? dm_checkpoint_service_alloc(16384 + stack_size, &block) : -1;
        if (rc < 0) die("checkpoint helper allocation failed (%d)", rc);
        if (rc) {
            _Static_assert(sizeof(gthr_t) <= 16384, "helper metadata prefix");
            g = block;
            g->stack = (uint8_t *)block + 16384;
            g->stack_size = stack_size;
        } else
#endif
        {
            g = calloc(1, sizeof *g);
            if (g) { g->stack = aligned_alloc(0x1000, stack_size); g->stack_size = stack_size; }   // the SDK requires the stack to be 4 KiB aligned and its size a multiple of 4 KiB
        }
        if (!g || !g->stack) die("host guest thread: allocation failed");
    }
    void *tt = g->tt;
    int rc = ((create_fn)need("_ZN2nn2os12CreateThreadEPNS0_10ThreadTypeEPFvPvES3_S3_mi"))(tt, fn, arg, g->stack, stack_size, prio);
    if (rc) die("host guest thread: CreateThread failed %#x", rc);
    ((tt_fn)need("_ZN2nn2os11StartThreadEPNS0_10ThreadTypeE"))(tt);
    return tt;
}
void dm_guest_thread_join(void *tt) {
    ((tt_fn)need("_ZN2nn2os10WaitThreadEPNS0_10ThreadTypeE"))(tt); ((tt_fn)need("_ZN2nn2os13DestroyThreadEPNS0_10ThreadTypeE"))(tt);
    gthr_t *g = (gthr_t *)tt; int kept = 0; g->joined_ns = now_ns();
    pthread_mutex_lock(&g_gmu); if (g_ngfree < (int)(sizeof g_gfree / sizeof *g_gfree)) { g_gfree[g_ngfree++] = g; kept = 1; } pthread_mutex_unlock(&g_gmu);
    if (!kept) {
#if defined(__ANDROID__) && defined(DM_MANUAL_CHECKPOINT)
        if (dm_checkpoint_service_free(g)) return;
#endif
        free(g->stack); free(g);
    }   // (the list is full: more joined threads than ever spawned at once; the stack goes as a game's would after its WaitThread)
}

static struct { int malloc_ok, lock_owned, handle; } g_st;
static void selftest_body(void *arg) {
    (void)arg;
    void *(*gmalloc)(size_t) = need("malloc"); void (*gfree)(void *) = need("free");
    void *p = gmalloc(4096); if (p) { memset(p, 0x5a, 4096); gfree(p); g_st.malloc_ok = 1; }
    uint8_t mtx[0x40] __attribute__((aligned(16))) = {0};           // nn::os::MutexType (0x20 bytes, with headroom)
    ((void (*)(void *, int, int))need("_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi"))(mtx, 0, 0);
    ((void (*)(void *))need("_ZN2nn2os9LockMutexEPNS0_9MutexTypeE"))(mtx);
    g_st.lock_owned = ((int (*)(const void *))need("_ZN2nn2os28IsMutexLockedByCurrentThreadEPKNS0_9MutexTypeE"))(mtx) & 1;
    ((void (*)(void *))need("_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE"))(mtx);
    uint32_t thr_cur_handle(void); g_st.handle = (int)thr_cur_handle();
}
static void park_body(void *arg) { (void)arg; for (;;) sleep(3600); }   // resident (=2): simulates the pre-decode thread staying alive for the whole run, occupying the handle
void dm_guest_spawn_selftest(void) {
    const char *e = getenv("DM_GUEST_SPAWN_TEST"); if (!e) return;
    if (atoi(e) == 2) { dm_guest_thread_spawn(park_body, NULL, 16, 16 << 10); LOG("host guest thread self-check: start a resident thread (later game thread handles shift up by one)"); }
    void *tt = dm_guest_thread_spawn(selftest_body, NULL, 16, 64 << 10);
    dm_guest_thread_join(tt);
    uint32_t thr_cur_handle(void); int mine = (int)thr_cur_handle();
    int ok = g_st.malloc_ok && g_st.lock_owned && g_st.handle && g_st.handle != mine;
    LOG("%shost guest thread self-check: SDK malloc/free %s; mutex owner is this thread %s; thread handle %#x (main thread %#x)", ok ? "" : "[RED] ",
        g_st.malloc_ok ? "ok" : "broken", g_st.lock_owned ? "yes" : "no", g_st.handle, mine);
}
