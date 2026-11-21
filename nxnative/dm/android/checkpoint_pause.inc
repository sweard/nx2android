// Included by hle_thread.c. All stop markers and the live roster are protected
// by G. Sleeping threads must pass the same release gate before returning to
// guest work, even when their original wait times out or is signalled.
enum { CP_RUNNING, CP_GATE, CP_KERNEL_WAIT, CP_CLOCK_WAIT, CP_HELPER_IDLE };
_Atomic int dm_checkpoint_pausing;
static unsigned checkpoint_guest_count;
unsigned dm_checkpoint_pause_count(void) { return checkpoint_guest_count; }
static pthread_cond_t checkpoint_release = PTHREAD_COND_INITIALIZER;

static void checkpoint_leave_stop(thr_t *t) {
    while (atomic_load(&dm_checkpoint_pausing) && (!t->host_owned || t->checkpoint_stop == CP_HELPER_IDLE))
        pthread_cond_wait(&checkpoint_release, &G);
    t->checkpoint_stop = CP_RUNNING;
}

void dm_checkpoint_helper_idle(int entering) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t || !t->host_owned) return;
    G_lock();
    if (entering) t->checkpoint_stop = CP_HELPER_IDLE;
    else checkpoint_leave_stop(t);
    pthread_mutex_unlock(&G);
}

void dm_checkpoint_gate_pause(void) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t || atomic_load(&dm_host_work)) return;
    G_lock();
    t->checkpoint_stop = CP_GATE;
    checkpoint_leave_stop(t);
    pthread_mutex_unlock(&G);
}

void dm_checkpoint_clock_wait(int entering, uint64_t *regs, uint64_t orig) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) return;
    G_lock();
    if (entering) {
        extern uintptr_t dm_guest_tls_owner(void);
        t->clock_regs = regs; t->clock_orig = orig;
        t->clock_x18 = (uintptr_t)dm_tsd_get(g_key18);
        t->checkpoint_tls_owner = dm_guest_tls_owner();
        t->checkpoint_tls = (uintptr_t)dm_tsd_get(g_keytls);
        t->checkpoint_tlr = (uintptr_t)dm_tsd_get(g_keytlr);
        t->checkpoint_stop = CP_CLOCK_WAIT;
    } else {
        checkpoint_leave_stop(t);
        t->clock_regs = NULL;
    }
    pthread_mutex_unlock(&G);
}

void dm_checkpoint_pause_end(void) {
    G_lock();
    atomic_store(&dm_checkpoint_pausing, 0);
    pthread_cond_broadcast(&checkpoint_release);
    pthread_mutex_unlock(&G);
}

int dm_checkpoint_pause_begin(void) {
    G_lock();
    if (atomic_exchange(&dm_checkpoint_pausing, 1)) { pthread_mutex_unlock(&G); return -EBUSY; }
    // The Android menu freezes guest time. Use host monotonic time here.
    uint64_t deadline = now_ns() + 750000000;
    for (;;) {
        unsigned stops[5] = {0}, pending_helpers = 0;
        for (thr_t *t = g_all; t; t = t->all) {
            if (!t->started || t->exited) continue;
            if (t->host_owned && t->checkpoint_stop != CP_HELPER_IDLE) ++pending_helpers;
            else ++stops[t->checkpoint_stop];
        }
        unsigned pending = stops[CP_RUNNING] + pending_helpers;
        if (!pending || now_ns() >= deadline) {
            LOG("[snapshot] rendezvous: gate=%u kernel_wait=%u clock_wait=%u pending=%u idle_helpers=%u pending_helpers=%u",
                stops[CP_GATE], stops[CP_KERNEL_WAIT], stops[CP_CLOCK_WAIT], stops[CP_RUNNING], stops[CP_HELPER_IDLE], pending_helpers);
            checkpoint_guest_count = stops[CP_GATE] + stops[CP_KERNEL_WAIT] + stops[CP_CLOCK_WAIT];
            if (pending) {
                for (thr_t *t = g_all; t; t = t->all) {
                    if (t->started && !t->exited && (t->host_owned ? t->checkpoint_stop != CP_HELPER_IDLE : !t->checkpoint_stop))
                        LOG("[snapshot] pending thread=%#x tid=%u in_svc=%d sleeping=%d helper=%d stop=%d", t->handle,
                            (unsigned)t->mport, atomic_load(&t->svc_state) & 1, atomic_load(&t->sleeping), t->host_owned, t->checkpoint_stop);
                }
                atomic_store(&dm_checkpoint_pausing, 0);
                pthread_cond_broadcast(&checkpoint_release);
            }
            pthread_mutex_unlock(&G);
            return pending ? -ETIMEDOUT : 0;
        }
        pthread_mutex_unlock(&G);
        struct timespec delay = {.tv_nsec = 2000000};
        nanosleep(&delay, NULL);
        G_lock();
    }
}
