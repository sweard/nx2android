// Included by hle_audren.c for access to its private runtime state.
#include "services.h"
typedef struct {
    char magic[8];
    uint64_t handle[2], thread_type, owned;
    uint32_t open, event_layout, head, tail, phase;
    int32_t lengths[CQ], history[2 * (RS_T - 1)];
    int16_t pcm[CQ][FRAME_MAX * 2];
    dm_checkpoint_wait_cpu entry;
} checkpoint_renderer;

int dm_checkpoint_renderer(int fd, int restore, dm_checkpoint_wait_cpu *helper) {
    checkpoint_renderer r = {.magic = "NX2AAR1"};
    if (!restore && (r.open = atomic_load(&g_open))) {
        r.handle[0] = (uintptr_t)g_handle[0]; r.handle[1] = (uintptr_t)g_handle[1];
        r.thread_type = (uintptr_t)g_render_tt; r.owned = (uintptr_t)objmap_get(g_handle[0]);
        r.event_layout = g_ev_layout; r.entry = audio_entry;
        r.head = atomic_load(&g_cq_head); r.tail = atomic_load(&g_cq_tail);
        memcpy(r.lengths, g_cq_n, sizeof r.lengths); memcpy(r.pcm, g_cq, sizeof r.pcm);
        r.phase = rs_pos; memcpy(r.history, rs_buf, sizeof r.history);
        if (r.owned && !dm_checkpoint_memory_committed(r.owned, 1)) {
            LOG("[snapshot] renderer work buffer %p outside checkpoint memory", (void *)r.owned);
            return -ENOTSUP;
        }
        if (!r.entry.handle) return -ENOTSUP;
    }
    checkpoint_stream io = {.fd = fd, .loading = restore};
    int rc = checkpoint_io(&io, &r, sizeof r);
    uint32_t expected = io.crc, sum = expected;
    if (!rc) rc = checkpoint_io(&io, &sum, sizeof sum);
    if (rc || !restore) return rc;
    struct stat st;
    if (fstat(fd, &st) || st.st_size != io.off || expected != sum || memcmp(r.magic, "NX2AAR1", 8) ||
        r.open > 1 || r.event_layout > 1 || r.tail - r.head > CQ || r.phase > 2) return -EBADMSG;
    if (!r.open) return 0;
    if (!dm_checkpoint_memory_committed(r.thread_type, 0x200) ||
        (r.owned && !dm_checkpoint_memory_committed(r.owned, 1)) ||
        r.entry.cpu.pc != r.entry.cpu.x[30] || dm_checkpoint_wait_cpu_supported(&r.entry)) return -ESTALE;
    for (int i = 0; i < 2; ++i)
        if (r.handle[i] && !dm_checkpoint_memory_committed(r.handle[i], 8) && !module_of((void *)r.handle[i])) return -ESTALE;
    for (int i = 0; i < CQ; ++i) if (r.lengths[i] < 0 || r.lengths[i] > FRAME_MAX) return -ERANGE;
    g_handle[0] = (void *)r.handle[0]; g_handle[1] = (void *)r.handle[1]; g_render_tt = (void *)r.thread_type;
    if (r.owned) objmap_put(g_handle[0], (void *)r.owned);
    g_ev_layout = r.event_layout; audio_entry = r.entry; *helper = r.entry;
    memcpy(g_cq_n, r.lengths, sizeof r.lengths); memcpy(g_cq, r.pcm, sizeof r.pcm);
    rs_pos = r.phase; memcpy(rs_buf, r.history, sizeof r.history);
    atomic_store(&g_cq_head, r.head); atomic_store(&g_cq_tail, r.tail); atomic_store(&g_open, 1);
    return 0;
}
void *dm_checkpoint_audio_function(void) { return render_main; }
