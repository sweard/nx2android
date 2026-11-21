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

int dm_checkpoint_waits_capture(dm_checkpoint_wait *items, unsigned capacity, unsigned *count) {
    *count = 0;
    G_lock();
    int rc = 0;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (*count >= capacity) { rc = -ENOSPC; break; }
        if (t->host_owned || t->closed || t->woke) { rc = -ENOTSUP; break; }
        uint64_t now = checkpoint_wait_now(t->checkpoint_guest_clock);
        items[(*count)++] = (dm_checkpoint_wait){
            .thread = t->handle, .kind = t->wkind, .sequence = t->wseq,
            .address = t->wkind == 4 ? 0 : t->waddr, .key = t->wkind == 2 ? t->wkey : 0,
            .tag = t->wkind <= 2 ? t->wtag : 0, .priority = t->prio, .cancelled = t->cancel,
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
            w->kind < 1 || w->kind > 4 || !w->sequence || w->sequence >= UINT64_MAX - MAX_H ||
            w->remaining_ns < -1 || (w->cancelled != 0 && w->cancelled != 1) ||
            (w->kind < 4 && (!checkpoint_wait_word(w->address) || w->count || w->handles)) ||
            (w->kind == 2 && !checkpoint_wait_word(w->key)) ||
            (w->kind == 4 && (w->count > 64 || (w->count &&
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
        t->next = g_wait; g_wait = t;
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
    while (!t->woke) {
        uint64_t end = t->checkpoint_deadline;
        if (end && checkpoint_wait_now(t->checkpoint_guest_clock) >= end) {
            wake(t, R_TIMEOUT); break;
        }
        if (!end || t->checkpoint_guest_clock) pthread_cond_wait(&t->cv, &G);
        else {
            struct timespec dl = {(time_t)(end / 1000000000), (long)(end % 1000000000)};
            pthread_cond_timedwait(&t->cv, &G, &dl);
        }
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

int dm_checkpoint_wait_cpu_capture(uint32_t handle, dm_checkpoint_wait_cpu *out) {
    int rc = -ENOTSUP;
    G_lock();
    thr_t *t = h_peek(handle, O_THREAD);
    if (!t || t->handle != handle || !t->wkind || t->host_owned) goto done;
    const uint64_t *r = atomic_load(&t->svc_regs);
    if (!r || !dm_checkpoint_memory_committed((uintptr_t)r, 816)) goto done;
    // gates.tsv records orig as an NSO offset; LR identifies the owning NSO.
    module_t *owner = module_of((const void *)(uintptr_t)r[30]);
    if (!owner || t->svc_pc >= owner->text_end || owner->text_end - t->svc_pc <= 4) goto done;
    uint64_t next = (uintptr_t)owner->base + t->svc_pc + 4;
    // patch_nso.site_gate saves guest FP/LR in a 16-byte record. dm_gate
    // pushes its 800-byte register file beneath that record. Returning
    // directly to orig+4 consumes both frames, as the rewritten stub would.
    *out = (dm_checkpoint_wait_cpu){.logical_x18 = t->svc_x18, .tls = t->checkpoint_tls,
        .tlr = t->checkpoint_tlr, .owner = t->checkpoint_tls_owner, .handle = handle, .svc = t->last_svc};
    memcpy(out->cpu.x, r, sizeof out->cpu.x);
    out->cpu.x[18] = 0;
    out->cpu.x[29] = r[100]; out->cpu.x[30] = r[101];
    out->cpu.pc = next; out->cpu.sp = (uintptr_t)(r + 102);
    out->cpu.nzcv = r[96]; out->cpu.fpsr = r[97]; out->cpu.fpcr = r[98];
    memcpy(out->cpu.v, r + 32, sizeof out->cpu.v);
    rc = 0;
done:
    pthread_mutex_unlock(&G);
    return rc;
}

int dm_checkpoint_wait_cpu_supported(const dm_checkpoint_wait_cpu *s) {
    if (!checkpoint_guest_pc(s->cpu.pc) || (s->cpu.sp & 15) ||
        s->cpu.sp < 816 || !dm_checkpoint_memory_committed(s->cpu.sp - 816, 816) ||
        (s->tls && !checkpoint_guest_bytes(s->tls, 1)) || (s->tlr && !checkpoint_guest_bytes(s->tlr, 512))) return -ENOTSUP;
    return 0;
}

int dm_checkpoint_wait_cpu_finish(dm_checkpoint_wait_cpu *s) {
    if (s->handle != cur()->handle) return -EINVAL;
    int rc = dm_checkpoint_wait_cpu_supported(s);
    if (rc) return rc;
    if ((rc = dm_guest_tls_adopt(s->owner))) return rc;
    // Keep this signal thread's bionic TLS and platform x18. Only rewrite the
    // Horizon slots supplied by the rewriter (guest TLS/TLR belong to it).
    dm_tsd_set(g_key18, (void *)(uintptr_t)s->logical_x18);
    dm_tsd_set(g_keytls, (void *)(uintptr_t)s->tls);
    dm_tsd_set(g_keytlr, (void *)(uintptr_t)s->tlr);
    return dm_checkpoint_wait_finish(s->cpu.x);
}
