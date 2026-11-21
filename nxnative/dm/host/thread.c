// Guest thread environment: gate slot = host gate entry; x18 slot cleared; tpidr_el0 slot = ELF TLS block; tpidrro_el0 slot = Horizon ThreadLocalRegion (0x200).
// The SDK's internal critical section takes the current thread object pointer from TLR[0x1f8]; here each thread gets a zero-initialized object allocated and written in.
#include "dm.h"
#define TLS_SIZE 8192
#define TLR_SIZE 0x200
#define THREAD_OBJ_SIZE 0x400
extern void dm_gate(void);            // entry.S: common gate entry
static pthread_key_t g_keyown;   // raw pointers of the TLS block / thread-local region this thread allocated (see the end of guest_thread_init)
static pthread_once_t g_keyown_once = PTHREAD_ONCE_INIT;
#if defined(__ANDROID__) && defined(DM_MANUAL_CHECKPOINT)
#include "../android/checkpoint_tls.h"
uintptr_t dm_guest_tls_owner(void) {
    void **own = pthread_getspecific(g_keyown);
    return own ? (uintptr_t)own[0] : 0;
}

int dm_guest_tls_adopt(uintptr_t original) {
    void **own = pthread_getspecific(g_keyown);
    if (!own) return -1;
    if (original == (uintptr_t)own[0]) return 0;
    int rc = dm_checkpoint_tls_claim(original);
    if (rc) return rc;
    if (!dm_checkpoint_tls_release(own[0])) { free(own[0]); free(own[1]); }
    own[0] = (void *)original; own[1] = (void *)(original + TLS_SIZE);
    return 0;
}
#endif
static void own_free(void *p) {
    void **own = p;
#if defined(__ANDROID__) && defined(DM_MANUAL_CHECKPOINT)
    if (dm_checkpoint_tls_release(own[0])) { free(own); return; }
#endif
    free(own[0]); free(own[1]); free(own);
}
static void own_key_init(void) { if (pthread_key_create(&g_keyown, own_free)) die("cannot create the free key for thread-local blocks"); }
void guest_thread_init(void) {
    dm_tsd_set(g_keygate, (void *)dm_gate);
    dm_tsd_set(g_key18, 0);
    // The TLS block and the thread-local region are freed when the thread exits. The guest overwrites the slots of the TLS / thread-local-region keys (it sets its own thread pointer), so they cannot be freed by the current value in the key:
    // the raw pointers that were allocated are recorded separately on this file's own key g_keyown and freed by its destructor (key destructors run at pthread_exit, like crash.c's signal alternate stack; the destructor only calls free, which is safe on a guest thread's small stack too).
    // They used not to be freed: every short-lived thread the game started leaked 8192 + 0x200 + 0x400 = 9728 bytes. The thread-local region and the current thread object are one block (the SDK may also overwrite the TLR[0x1f8] slot)
    pthread_once(&g_keyown_once, own_key_init);
    void **own = calloc(2, sizeof *own);
    if (!own) die("guest thread-local ownership allocation failed");
    uint8_t *tlr;
#if defined(__ANDROID__) && defined(DM_MANUAL_CHECKPOINT)
    int arena = dm_checkpoint_tls_alloc(&own[0]);
    if (arena < 0) die("checkpoint TLS allocation failed (%d)", arena);
    if (arena) tlr = (uint8_t *)own[0] + TLS_SIZE;
    else
#endif
    {
        own[0] = calloc(1, TLS_SIZE);
        tlr = calloc(1, TLR_SIZE + THREAD_OBJ_SIZE);
        if (!own[0] || !tlr) die("guest thread-local block allocation failed");
    }
    own[1] = tlr;
    *(void **)(tlr + 0x1f8) = tlr + TLR_SIZE;               // current thread object
    dm_tsd_set(g_keytls, own[0]); dm_tsd_set(g_keytlr, tlr);
    void **prev = pthread_getspecific(g_keyown); if (prev) own_free(prev);   // re-initialised on the same thread: free the previous set first (the new one is what the slots now point to)
    pthread_setspecific(g_keyown, own);
}
