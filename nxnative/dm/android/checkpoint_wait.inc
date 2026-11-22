#include "checkpoint_wait.h"
#include "checkpoint_wait_cpu.h"
#include "checkpoint_tls.h"
#include "checkpoint_memory.h"
#include "checkpoint_io.h"
#include <sys/stat.h>

static uint64_t checkpoint_wait_now(int guest) {
    if (guest) return guest_ns();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// The synchronization event has already woken this waiter, but the pause gate
// still holds its native thread inside block(). Recheck the saved handle set
// on resume, just as svcThread's WaitSynchronization loop does after wake().
enum { CP_WOKEN_SYNC = 5 };
static int checkpoint_woken_sync(const thr_t *t) {
    uint32_t svc = t->checkpoint_retry_cpu ? t->checkpoint_retry_cpu->svc : t->last_svc;
    return t->checkpoint_stop == CP_KERNEL_WAIT && !t->wkind && t->woke && t->wres == -1 && svc == 0x18;
}

int dm_checkpoint_waits_capture(dm_checkpoint_wait *items, unsigned capacity, unsigned *count) {
    *count = 0;
    G_lock();
    int rc = 0;
    for (thr_t *t = g_all; t; t = t->all) {
        int kind = t->wkind;
        if (checkpoint_woken_sync(t)) kind = CP_WOKEN_SYNC;
        if (!kind) continue;
        if (*count >= capacity) { rc = -ENOSPC; break; }
        if (t->host_owned || t->closed || (kind != CP_WOKEN_SYNC && t->woke)) { rc = -ENOTSUP; break; }
        uint64_t now = checkpoint_wait_now(t->checkpoint_guest_clock);
        items[(*count)++] = (dm_checkpoint_wait){
            .thread = t->handle, .kind = kind, .sequence = t->wseq,
            .address = kind >= 4 ? 0 : t->waddr, .key = kind == 2 ? t->wkey : 0,
            .tag = kind <= 2 ? t->wtag : 0, .priority = t->prio, .cancelled = t->cancel,
            .handles = (uintptr_t)t->whs, .count = t->whn,
            .remaining_ns = !t->checkpoint_deadline ? -1 :
                t->checkpoint_deadline > now ? (int64_t)(t->checkpoint_deadline - now) : 0};
    }
    pthread_mutex_unlock(&G);
    if (rc) *count = 0;
    return rc;
}

static int checkpoint_guest_bytes(uint64_t address, uint64_t bytes) {
    if (dm_checkpoint_memory_committed(address, bytes)) return 1;
    module_t *m = module_of((void *)(uintptr_t)address);
    if (!m) return 0;
    uint64_t off = address - (uintptr_t)m->base;
    return off >= m->data_off && off < m->size && bytes <= m->size - off;
}

static int checkpoint_wait_word(uint64_t address) {
    return !(address & 3) && checkpoint_guest_bytes(address, 4);
}

int dm_checkpoint_waits_install(const dm_checkpoint_wait *items, unsigned count) {
    if (count >= MAX_H) return -EINVAL;
    int rc = 0;
    G_lock();
    if (g_wait) { rc = -EBUSY; goto out; }
    for (unsigned i = 0; i < count; ++i) {
        const dm_checkpoint_wait *w = &items[i];
        thr_t *t = h_peek(w->thread, O_THREAD);
        if (!t || t->handle != w->thread || t->started || t->closed || t->prio != w->priority ||
            w->kind < 1 || w->kind > CP_WOKEN_SYNC || !w->sequence || w->sequence >= UINT64_MAX - MAX_H ||
            w->remaining_ns < -1 || (w->cancelled != 0 && w->cancelled != 1) ||
            (w->kind < 4 && (!checkpoint_wait_word(w->address) || w->count || w->handles)) ||
            (w->kind == 2 && !checkpoint_wait_word(w->key)) ||
            (w->kind >= 4 && (w->count > 64 || (w->count &&
                ((w->handles & 3) || !checkpoint_guest_bytes(w->handles, 4ull * w->count)))))) {
            rc = -EINVAL; goto out;
        }
        for (unsigned j = 0; j < i; ++j) if (w->sequence == items[j].sequence || w->thread == items[j].thread) {
            rc = -EINVAL; goto out;
        }
    }
    for (unsigned i = 0; i < count; ++i) {
        const dm_checkpoint_wait *w = &items[i];
        thr_t *t = h_peek(w->thread, O_THREAD);
        t->wkind = w->kind; t->waddr = w->address; t->wkey = w->key; t->wtag = w->tag;
        t->whs = (void *)(uintptr_t)w->handles; t->whn = w->count; t->wseq = w->sequence;
        t->cancel = w->cancelled; t->woke = 0; t->checkpoint_installed = w->kind;
        t->checkpoint_guest_clock = dm_detclock_enabled();
        t->checkpoint_deadline = w->remaining_ns < 0 ? 0 :
            checkpoint_wait_now(t->checkpoint_guest_clock) + (uint64_t)w->remaining_ns;
        if (g_wseq < w->sequence) g_wseq = w->sequence;
        if (w->kind == CP_WOKEN_SYNC) {
            t->checkpoint_installed = 4;
            t->wkind = 0; t->woke = 1; t->wres = -1;
        } else { t->next = g_wait; g_wait = t; }
    }
out:
    pthread_mutex_unlock(&G);
    return rc;
}

int dm_checkpoint_wait_finish(uint64_t *regs) {
    thr_t *t = cur();
    G_lock();
    int kind = t->checkpoint_installed;
    if (!kind) { pthread_mutex_unlock(&G); return -EINVAL; }
    t->checkpoint_installed = 0;
    // An event may have become signaled between capturing the waiting thread
    // and capturing its event table. Use the restored predicate before parking.
    if (kind == 4 && !t->woke) {
        int bad = 0, ready = t->cancel;
        for (uint32_t i = 0; !ready && !bad && i < t->whn; ++i)
            ready = sig_obj(t->whs[i], &bad);
        if (ready || bad) wake(t, -1);
    }
    while (!t->woke) {
        uint64_t end = t->checkpoint_deadline;
        if (end && checkpoint_wait_now(t->checkpoint_guest_clock) >= end) {
            wake(t, R_TIMEOUT); break;
        }
        if (t->checkpoint_retry_cpu) t->checkpoint_stop = CP_KERNEL_WAIT;
        if (!end || t->checkpoint_guest_clock) pthread_cond_wait(&t->cv, &G);
        else {
            struct timespec dl = {(time_t)(end / 1000000000), (long)(end % 1000000000)};
            pthread_cond_timedwait(&t->cv, &G, &dl);
        }
        if (t->checkpoint_retry_cpu) checkpoint_leave_stop(t);
    }
    int64_t result = t->wres;
    if (kind == 4 && result != R_TIMEOUT) {
        regs[1] = (uintptr_t)t->whs; regs[2] = t->whn;
        uint64_t now = checkpoint_wait_now(t->checkpoint_guest_clock);
        regs[3] = !t->checkpoint_deadline ? UINT64_MAX :
            t->checkpoint_deadline > now ? t->checkpoint_deadline - now : 0;
        t->whs = NULL; t->whn = 0;
        pthread_mutex_unlock(&G);
        svc_thread(0x18, regs);
    } else {
        if (kind == 4) { regs[1] = 0; t->whs = NULL; t->whn = 0; }
        regs[0] = result;
        pthread_mutex_unlock(&G);
    }
    return 0;
}

typedef struct { uint32_t owner, type, signaled; } checkpoint_event_slot;
typedef struct { uint64_t object; uint32_t write, read; } checkpoint_event_binding;
typedef struct {
    char magic[8];
    uint32_t bindings, reserved;
    checkpoint_event_slot slots[MAX_H];
    checkpoint_event_binding system[512];
} checkpoint_events;

int dm_checkpoint_events_save(int fd) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size) return -EINVAL;
    checkpoint_events *table = calloc(1, sizeof *table);
    if (!table) return -ENOMEM;
    memcpy(table->magic, "NX2AEV1", 8);
    G_lock();
    pthread_mutex_lock(&g_se_lock);
    for (unsigned i = 1; i < MAX_H; ++i) {
        if (g_h[i].type != O_EVENT_R && g_h[i].type != O_EVENT_W) continue;
        event_t *e = g_h[i].obj;
        unsigned first = 1;
        while (first < i && g_h[first].obj != e) ++first;
        table->slots[i] = (checkpoint_event_slot){first, g_h[i].type, e->signaled};
    }
    table->bindings = g_nse;
    for (int i = 0; i < g_nse; ++i)
        table->system[i] = (checkpoint_event_binding){(uintptr_t)g_se[i].ev, g_se[i].wh, g_se[i].rh};
    pthread_mutex_unlock(&g_se_lock);
    pthread_mutex_unlock(&G);
    checkpoint_stream stream = {.fd = fd};
    int rc = checkpoint_io(&stream, table, sizeof *table);
    uint32_t crc = stream.crc;
    if (!rc) rc = checkpoint_io(&stream, &crc, sizeof crc);
    free(table);
    return rc;
}

int dm_checkpoint_events_restore(int fd) {
    struct stat st;
    if (fstat(fd, &st) || st.st_size != sizeof(checkpoint_events) + sizeof(uint32_t)) return -EBADMSG;
    checkpoint_events *table = malloc(sizeof *table);
    event_t **owners = calloc(MAX_H, sizeof *owners);
    if (!table || !owners) { free(table); free(owners); return -ENOMEM; }
    checkpoint_stream stream = {.fd = fd, .loading = 1};
    int rc = checkpoint_io(&stream, table, sizeof *table);
    uint32_t expected = stream.crc, footer;
    if (!rc) rc = checkpoint_io(&stream, &footer, sizeof footer);
    if (!rc && (footer != expected || memcmp(table->magic, "NX2AEV1", 8) ||
                table->reserved || table->bindings > 512 || table->slots[0].type)) rc = -EBADMSG;
    G_lock();
    pthread_mutex_lock(&g_se_lock);
    if (!rc && g_nse) rc = -EBUSY;
    for (unsigned i = 1; !rc && i < MAX_H; ++i) {
        checkpoint_event_slot s = table->slots[i];
        if (g_h[i].type == O_EVENT_W || g_h[i].type == O_EVENT_R) { rc = -EBUSY; break; }
        if (!s.type) continue;
        if (g_h[i].type || !s.owner || s.owner > i || (s.type != O_EVENT_R && s.type != O_EVENT_W) ||
            s.signaled > 1 || table->slots[s.owner].signaled != s.signaled) { rc = -EBADMSG; break; }
        if (i == s.owner) {
            owners[i] = calloc(1, sizeof(event_t));
            if (!owners[i]) { rc = -ENOMEM; break; }
            owners[i]->signaled = s.signaled;
        }
        if (!owners[s.owner]) { rc = -EBADMSG; break; }
        owners[s.owner]->refs++;
    }
    for (unsigned i = 0; !rc && i < table->bindings; ++i) {
        checkpoint_event_binding b = table->system[i];
        unsigned w = b.write - 0x1000, r = b.read - 0x1000;
        if (!checkpoint_guest_bytes(b.object, 1) || w >= MAX_H || table->slots[w].type != O_EVENT_W ||
            (b.read && (r >= MAX_H || table->slots[r].type != O_EVENT_R || table->slots[r].owner != table->slots[w].owner)))
            rc = -EBADMSG;
    }
    if (!rc) {
        for (unsigned i = 1; i < MAX_H; ++i) if (table->slots[i].type) {
            g_h[i].type = table->slots[i].type; g_h[i].obj = owners[table->slots[i].owner];
        }
        for (unsigned i = 0; i < table->bindings; ++i) {
            checkpoint_event_binding b = table->system[i];
            g_se[g_nse++] = (typeof(g_se[0])){(void *)(uintptr_t)b.object, b.write, b.read};
        }
    }
    pthread_mutex_unlock(&g_se_lock);
    pthread_mutex_unlock(&G);
    if (rc) for (unsigned i = 1; i < MAX_H; ++i) free(owners[i]);
    free(owners); free(table);
    return rc;
}

static int checkpoint_guest_pc(uint64_t pc) {
    module_t *m = module_of((void *)(uintptr_t)pc);
    return !(pc & 3) && m && pc - (uintptr_t)m->base < m->text_end;
}

static void checkpoint_gate_cpu(const thr_t *t, const uint64_t *r, uintptr_t next,
                                uint64_t x18, uint32_t svc, dm_checkpoint_wait_cpu *out) {
    // patch_nso.site_gate saves guest FP/LR in a 16-byte record. dm_gate
    // pushes its 800-byte register file beneath that record. Returning
    // directly to orig+4 consumes both frames, as the rewritten stub would.
    *out = (dm_checkpoint_wait_cpu){.logical_x18 = x18, .tls = t->checkpoint_tls,
        .tlr = t->checkpoint_tlr, .owner = t->checkpoint_tls_owner, .handle = t->handle, .svc = svc};
    memcpy(out->cpu.x, r, sizeof out->cpu.x);
    out->cpu.x[18] = 0;
    out->cpu.x[29] = r[100]; out->cpu.x[30] = r[101];
    out->cpu.pc = next; out->cpu.sp = (uintptr_t)(r + 102);
    out->cpu.nzcv = r[96]; out->cpu.fpsr = r[97]; out->cpu.fpcr = r[98];
    memcpy(out->cpu.v, r + 32, sizeof out->cpu.v);
}

int dm_checkpoint_clock_cpus(dm_checkpoint_wait_cpu *out, unsigned capacity, unsigned *count) {
    *count = 0;
    int rc = 0;
    G_lock();
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->host_owned || t->exited || t->checkpoint_stop != CP_CLOCK_WAIT) continue;
        if (*count == capacity) { rc = -ENOSPC; break; }
        const uint64_t *r = t->clock_regs;
        if (!r || !dm_checkpoint_memory_committed((uintptr_t)r, 816)) { rc = -ENOTSUP; break; }
        module_t *m = module_of((void *)(uintptr_t)r[30]);
        if (!m || !t->clock_orig || t->clock_orig >= m->text_end) { rc = -ENOTSUP; break; }
        // Re-execute the original patched clock instruction in the new PID:
        // it chooses the proper destination register and observes the new clock.
        checkpoint_gate_cpu(t, r, (uintptr_t)m->base + t->clock_orig,
                            t->clock_x18, DM_CHECKPOINT_RETRY_CLOCK, &out[*count]);
        rc = dm_checkpoint_wait_cpu_supported(&out[*count]);
        if (rc) break;
        ++*count;
    }
    pthread_mutex_unlock(&G);
    if (rc) *count = 0;
    return rc;
}

int dm_checkpoint_wait_cpu_capture(uint32_t handle, dm_checkpoint_wait_cpu *out) {
    int rc = -ENOTSUP;
    G_lock();
    thr_t *t = h_peek(handle, O_THREAD);
    if (!t || t->handle != handle || (!t->wkind && !checkpoint_woken_sync(t)) || t->host_owned) {
        LOG("[snapshot] wait CPU %#x: stale wait (thread=%#x kind=%d)", handle,
            t ? t->handle : 0, t ? t->wkind : 0);
        goto done;
    }
    // A thread still in its original reconstructed wait has not returned
    // through the SVC gate in this PID yet. Its guest registers remain valid.
    if (t->checkpoint_retry_cpu) { *out = *t->checkpoint_retry_cpu; rc = 0; goto done; }
    const uint64_t *r = atomic_load(&t->svc_regs);
    if (!r || !dm_checkpoint_memory_committed((uintptr_t)r, 816)) {
        LOG("[snapshot] wait CPU %#x: %s (wait=%d, SVC=%#x)", handle,
            r ? "frame outside checkpoint memory" : "no SVC gate frame", t->wkind, t->last_svc);
        goto done;
    }
    // gates.tsv records orig as an NSO offset; LR identifies the owning NSO.
    module_t *owner = module_of((const void *)(uintptr_t)r[30]);
    if (!owner || t->svc_pc >= owner->text_end || owner->text_end - t->svc_pc <= 4) {
        LOG("[snapshot] wait CPU %#x: gate outside identified module (wait=%d, SVC=%#x)", handle, t->wkind, t->last_svc);
        goto done;
    }
    uint64_t next = (uintptr_t)owner->base + t->svc_pc + 4;
    checkpoint_gate_cpu(t, r, next, t->svc_x18, t->last_svc, out);
    rc = 0;
done:
    pthread_mutex_unlock(&G);
    return rc;
}

int dm_checkpoint_wait_cpu_supported(const dm_checkpoint_wait_cpu *s) {
    const char *field = NULL;
    if (!checkpoint_guest_pc(s->cpu.pc)) field = "pc";
    else if ((s->cpu.sp & 15) || s->cpu.sp < 816 ||
             !dm_checkpoint_memory_committed(s->cpu.sp - 816, 816)) field = "stack";
    else if (s->tls && !checkpoint_guest_bytes(s->tls, 1)) field = "TLS";
    else if (s->tlr && !checkpoint_guest_bytes(s->tlr, 512)) field = "TLR";
    if (field) {
        LOG("[snapshot] thread %#x: unsupported %s (svc=%#x pc=%p sp=%p tls=%p tlr=%p)",
            s->handle, field, s->svc, (void *)(uintptr_t)s->cpu.pc, (void *)(uintptr_t)s->cpu.sp,
            (void *)(uintptr_t)s->tls, (void *)(uintptr_t)s->tlr);
        return -ENOTSUP;
    }
    return 0;
}

static int checkpoint_attach_thread(dm_checkpoint_wait_cpu *s) {
    if (s->handle != cur()->handle) return -EINVAL;
    int rc = dm_checkpoint_wait_cpu_supported(s);
    if (rc) return rc;
    if ((rc = dm_guest_tls_adopt(s->owner))) return rc;
    // Keep this signal thread's bionic TLS and platform x18. Only rewrite the
    // Horizon slots supplied by the rewriter (guest TLS/TLR belong to it).
    dm_tsd_set(g_key18, (void *)(uintptr_t)s->logical_x18);
    dm_tsd_set(g_keytls, (void *)(uintptr_t)s->tls);
    dm_tsd_set(g_keytlr, (void *)(uintptr_t)s->tlr);
    return 0;
}

void dm_checkpoint_helper_identity(dm_checkpoint_wait_cpu *s) {
    s->handle = cur()->handle;
    s->owner = dm_guest_tls_owner();
    s->tls = (uintptr_t)dm_tsd_get(g_keytls);
    s->tlr = (uintptr_t)dm_tsd_get(g_keytlr);
    s->logical_x18 = (uintptr_t)dm_tsd_get(g_key18);
}
static int attach_restored_handle(uint32_t handle, int service) {
    if (pthread_getspecific(g_keythr)) return -EBUSY;
    G_lock();
    thr_t *t = h_peek(handle, O_THREAD);
    int rc = -EINVAL;
    if (t && t->handle == handle && t->checkpoint_was_started && !t->started &&
        !t->exited && !!t->host_owned == service) {
        rc = -pthread_setspecific(g_keythr, t);
        if (!rc) {
            t->pt = pthread_self();
            t->started = 1;
            t->mport = pthread_mach_thread_np(t->pt);
            t->host_work = &dm_host_work;
        }
    }
    pthread_mutex_unlock(&G);
    if (!rc) {
        crash_thread_init();
        guest_thread_init();
    }
    return rc;
}

int dm_checkpoint_attach_handle(uint32_t handle) { return attach_restored_handle(handle, 0); }
int dm_checkpoint_attach_audio(dm_checkpoint_wait_cpu *s) {
    int rc = attach_restored_handle(s->handle, 1);
    if (rc) return rc;
    // The saved PC is the verified SDK return address during validation.
    return checkpoint_attach_thread(s);
}

int dm_checkpoint_clock_cpu_finish(dm_checkpoint_wait_cpu *s) {
    return s->svc == DM_CHECKPOINT_RETRY_CLOCK ? checkpoint_attach_thread(s) : -EINVAL;
}

int dm_checkpoint_wait_cpu_finish(dm_checkpoint_wait_cpu *s) {
    if (s->svc == DM_CHECKPOINT_RETRY_CLOCK) return -EINVAL;
    int rc = checkpoint_attach_thread(s);
    if (rc) return rc;
    thr_t *t = cur();
    G_lock(); t->checkpoint_retry_cpu = s; pthread_mutex_unlock(&G);
    rc = dm_checkpoint_wait_finish(s->cpu.x);
    G_lock(); t->checkpoint_retry_cpu = NULL; pthread_mutex_unlock(&G);
    return rc;
}

typedef struct {
    uint64_t id, entry, arg, stack_top, mask;
    uint32_t handle, started, exited, host, cancelled;
    int32_t priority, core;
    uint32_t reserved;
} snapshot_thread_handle;
typedef struct {
    char magic[8];
    uint64_t next_id, wait_sequence;
    uint32_t count, reserved;
} snapshot_thread_table;

int dm_checkpoint_handles_save(int fd) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size) return -EINVAL;
    checkpoint_stream io = {.fd = fd};
    G_lock();
    snapshot_thread_table h = {.magic = "NX2ATH1", .next_id = g_next_id, .wait_sequence = g_wseq};
    int rc = 0;
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->closed || h_peek(t->handle, O_THREAD) != t) { rc = -ENOTSUP; break; }
        ++h.count;
    }
    if (!rc) rc = checkpoint_io(&io, &h, sizeof h);
    for (unsigned i = 1; !rc && i < MAX_H; ++i) {
        if (g_h[i].type != O_THREAD) continue;
        thr_t *t = g_h[i].obj;
        snapshot_thread_handle r = {.id = t->id, .entry = t->entry, .arg = t->arg,
            .stack_top = t->stack_top, .mask = t->mask, .priority = t->prio, .core = t->core,
            .started = t->started, .exited = t->exited, .host = t->host_owned, .handle = t->handle,
            .cancelled = t->cancel};
        rc = checkpoint_io(&io, &r, sizeof r);
    }
    pthread_mutex_unlock(&G);
    uint32_t sum = io.crc;
    return rc ? rc : checkpoint_io(&io, &sum, sizeof sum);
}

int dm_checkpoint_handles_restore(int fd) {
    pthread_once(&g_thread_key_once, make_thread_key);
    struct stat st;
    snapshot_thread_table h;
    checkpoint_stream io = {.fd = fd, .loading = 1};
    if (fstat(fd, &st) || checkpoint_io(&io, &h, sizeof h) || memcmp(h.magic, "NX2ATH1", 8) ||
        !h.count || h.count >= MAX_H || h.next_id < 0x52 || h.reserved ||
        st.st_size != (off_t)(sizeof h + h.count * sizeof(snapshot_thread_handle) + 4)) return -EBADMSG;
    snapshot_thread_handle *records = calloc(h.count, sizeof *records);
    thr_t **threads = calloc(h.count, sizeof *threads);
    int rc = (!records || !threads) ? -ENOMEM : checkpoint_io(&io, records, h.count * sizeof *records);
    uint32_t sum = io.crc, footer;
    if (!rc && (checkpoint_io(&io, &footer, sizeof footer) || sum != footer)) rc = -EBADMSG;
    G_lock();
    if (!rc && g_all) rc = -EBUSY;
    for (unsigned i = 0; !rc && i < h.count; ++i) {
        const snapshot_thread_handle *r = &records[i];
        unsigned slot = r->handle - 0x1000;
        if (!slot || slot >= MAX_H || g_h[slot].type || (i ? r->handle <= records[i-1].handle : slot != 1) ||
            r->id < 0x51 || r->id >= h.next_id || r->exited > 1 || r->started > 1 || r->host > 1 ||
            r->cancelled > 1 || r->priority < 0 || r->priority > 63 || r->core < 0 || r->core > 63 || r->reserved ||
            (i && !r->exited && (!checkpoint_guest_pc(r->entry) || !r->stack_top || (r->stack_top & 15) ||
             !dm_checkpoint_memory_committed(r->stack_top - 16, 16)))) { rc = -EBADMSG; break; }
        for (unsigned j = 0; j < i; ++j) if (records[j].id == r->id) { rc = -EBADMSG; break; }
        if (rc) break;
        thr_t *t = calloc(1, sizeof *t);
        if (!t) { rc = -ENOMEM; break; }
        if (pthread_cond_init(&t->cv, NULL)) { free(t); rc = -ENOMEM; break; }
        if (pthread_cond_init(&t->pcv, NULL)) { pthread_cond_destroy(&t->cv); free(t); rc = -ENOMEM; break; }
        t->handle = r->handle; t->id = r->id; t->entry = r->entry; t->arg = r->arg; t->stack_top = r->stack_top;
        t->mask = r->mask; t->core = r->core; t->prio = r->priority; t->host_owned = r->host;
        // An exited thread needs no host attachment. Older recaptures stored
        // started=0 for these handles after their first cold restore.
        t->started = r->exited;
        t->exited = r->exited; t->cancel = r->cancelled; t->checkpoint_was_started = r->started;
        threads[i] = t;
    }
    if (!rc) {
        for (unsigned i = 0; i < h.count; ++i) {
            thr_t *t = threads[i];
            unsigned slot = t->handle - 0x1000;
            g_h[slot].type = O_THREAD; g_h[slot].obj = t;
            t->all = g_all; g_all = t;
        }
        g_next_id = h.next_id; g_wseq = h.wait_sequence;
    }
    pthread_mutex_unlock(&G);
    if (rc && threads) for (unsigned i = 0; i < h.count; ++i) if (threads[i]) {
        pthread_cond_destroy(&threads[i]->cv); pthread_cond_destroy(&threads[i]->pcv); free(threads[i]);
    }
    free(threads); free(records);
    return rc;
}
