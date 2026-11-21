// Texture/sampler registrations include the versions seen by previously
// recorded NVN binds. Actual VkImageView/VkSampler handles are process-local.
// The replay and registration threads must be stopped by the caller.
#include "checkpoint_memory.h"
typedef struct {
    char magic[8];
    uint32_t textures, samplers, texture_slots, sampler_slots;
    uint64_t sampler_sequence;
} binding_header;
typedef struct {
    uint32_t id, reserved;
    treg_ver version[1 + TREG_OLD];
} texture_binding;
typedef struct {
    uint32_t id, reserved;
    sreg_ver version[1 + SREG_OLD];
} sampler_binding;

static int checkpoint_binding_range(uintptr_t ptr, uint64_t n) {
    if (dm_checkpoint_memory_committed(ptr, n)) return 1;
    module_t *m = module_of((void *)ptr);
    if (!m) return 0;
    uint64_t off = ptr - (uintptr_t)m->base;
    return off >= m->data_off && off < m->size && n <= m->size - off;
}

static int checkpoint_binding_valid(const treg_ver *v) {
    if (!v->ok) return 1;
    const tex_t *t = &v->t;
    return v->ok == 1 && v->has_v <= 1 && t->magic == MAGIC('T','E','X',0) && t->serial &&
        (!t->mem || checkpoint_binding_range((uintptr_t)t->mem, t->L.size ? t->L.size : 1)) &&
        (!t->pool || checkpoint_binding_range((uintptr_t)t->pool, sizeof *t->pool));
}

int dm_checkpoint_bindings_save(int fd) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size) return -EINVAL;
    checkpoint_stream io = {.fd = fd};
    pthread_mutex_lock(&g_rmu);
    binding_header h = {.magic = "NX2ABN1", .texture_slots = g_ntreg, .sampler_slots = g_nsreg, .sampler_sequence = g_sregs};
    for (int i = 0; i < g_ntreg; ++i) h.textures += g_treg[i].cur.ok || g_treg[i].old;
    for (int i = 0; i < g_nsreg; ++i) h.samplers += !!g_sreg_ok[i];
    int rc = checkpoint_io(&io, &h, sizeof h);
    for (int i = 0; !rc && i < g_ntreg; ++i) {
        const treg_t *binding = &g_treg[i];
        if (!binding->cur.ok && !binding->old) continue;
        texture_binding b = {.id = i};
        b.version[0] = binding->cur;
        if (binding->old) memcpy(b.version + 1, binding->old, TREG_OLD * sizeof(treg_ver));
        for (unsigned k = 0; k <= TREG_OLD; ++k) {
            if (!b.version[k].ok) memset(&b.version[k], 0, sizeof(treg_ver));
            else if (!checkpoint_binding_valid(&b.version[k])) { rc = -ENOTSUP; break; }
        }
        if (!rc) rc = checkpoint_io(&io, &b, sizeof b);
    }
    for (int i = 0; !rc && i < g_nsreg; ++i) if (g_sreg_ok[i]) {
        sampler_binding b = {.id = i, .version = {{.p = g_sreg[i], .seq = g_sseq[i]}}};
        memcpy(b.version + 1, g_sold + i * SREG_OLD, sizeof(sreg_ver) * SREG_OLD);
        rc = checkpoint_io(&io, &b, sizeof b);
    }
    pthread_mutex_unlock(&g_rmu);
    uint32_t crc = io.crc;
    return rc ? rc : checkpoint_io(&io, &crc, sizeof crc);
}

int dm_checkpoint_bindings_restore(int fd) {
    struct stat st;
    binding_header h;
    checkpoint_stream io = {.fd = fd, .loading = 1};
    int rc = checkpoint_io(&io, &h, sizeof h);
    if (rc) return rc;
    // NVN texture and sampler handles encode 20 and 12 bits. Allow the
    // backend's extra allocation step at the top of each table.
    if (fstat(fd, &st) || memcmp(h.magic, "NX2ABN1", 8) ||
        h.texture_slots > (1u << 20) + 1024 || h.sampler_slots > (1u << 12) + 256 ||
        h.textures > h.texture_slots || h.samplers > h.sampler_slots ||
        st.st_size != (off_t)(sizeof h + sizeof(texture_binding) * h.textures +
                                    sizeof(sampler_binding) * h.samplers + 4)) return -EBADMSG;
    unsigned nt = h.texture_slots, ns = h.sampler_slots;
    treg_t *textures = nt ? calloc(nt, sizeof *textures) : NULL;
    samp_par *samplers = ns ? calloc(ns, sizeof *samplers) : NULL;
    VkSampler *handles = ns ? calloc(ns, sizeof *handles) : NULL;
    uint8_t *valid = ns ? calloc(ns, 1) : NULL;
    uint64_t *sequence = ns ? calloc(ns, sizeof *sequence) : NULL;
    sreg_ver *older = ns ? calloc((size_t)ns * SREG_OLD, sizeof *older) : NULL;
    if ((nt && !textures) || (ns && (!samplers || !handles || !valid || !sequence || !older))) rc = -ENOMEM;
    uint32_t last = 0;
    for (unsigned i = 0; !rc && i < h.textures; ++i) {
        texture_binding b;
        if ((rc = checkpoint_io(&io, &b, sizeof b))) break;
        if (b.id >= nt || (i && b.id <= last) || b.reserved) { rc = -EBADMSG; break; }
        last = b.id;
        for (unsigned k = 0; k <= TREG_OLD; ++k)
            if (!checkpoint_binding_valid(&b.version[k])) { rc = -EBADMSG; break; }
        if (rc) break;
        textures[b.id].cur = b.version[0];
        if (b.version[1].ok) {
            textures[b.id].old = malloc(TREG_OLD * sizeof(treg_ver));
            if (!textures[b.id].old) { rc = -ENOMEM; break; }
            memcpy(textures[b.id].old, b.version + 1, TREG_OLD * sizeof(treg_ver));
        }
    }
    for (unsigned i = 0; !rc && i < h.samplers; ++i) {
        sampler_binding b;
        if ((rc = checkpoint_io(&io, &b, sizeof b))) break;
        if (b.id >= ns || (i && b.id <= last) || b.reserved || !b.version[0].seq ||
            b.version[0].seq > h.sampler_sequence) { rc = -EBADMSG; break; }
        last = b.id;
        for (unsigned k = 1; k <= SREG_OLD; ++k)
            if (b.version[k].seq && b.version[k].seq >= b.version[k-1].seq) { rc = -EBADMSG; break; }
        samplers[b.id] = b.version[0].p; sequence[b.id] = b.version[0].seq; valid[b.id] = 1;
        memcpy(older + b.id * SREG_OLD, b.version + 1, sizeof(sreg_ver) * SREG_OLD);
    }
    uint32_t crc = io.crc, footer;
    if (!rc) rc = checkpoint_io(&io, &footer, sizeof footer);
    if (!rc && crc != footer) rc = -EBADMSG;
    pthread_mutex_lock(&g_rmu);
    if (!rc && (g_ntreg || g_nsreg)) rc = -EBUSY;
    if (!rc) {
        g_treg = textures; g_ntreg = nt;
        g_sreg = samplers; g_samp = handles; g_sreg_ok = valid; g_sseq = sequence; g_sold = older; g_nsreg = ns;
        __atomic_store_n(&g_sregs, h.sampler_sequence, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_tex_gen, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&g_samp_gen, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_rmu);
    if (rc) {
        if (textures) for (unsigned i = 0; i < nt; ++i) free(textures[i].old);
        free(textures); free(samplers); free(handles); free(valid); free(sequence); free(older);
    }
    return rc;
}
