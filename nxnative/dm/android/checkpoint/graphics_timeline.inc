// Included by hle_nvn.c for access to its private runtime state.
#include "services.h"
int dm_checkpoint_present_begin(int report_busy);
void dm_checkpoint_present_end(void);
int dm_checkpoint_present_ticket(uint64_t *value, int restore);

// ProgramSetShaders owns the only reliable lifetime for the control section.
// Persist keys captured at registration, plus the allocator high-water mark
// so later programs and textures cannot collide with restored serials.
static int checkpoint_program_keys(checkpoint_stream *io) {
    uint64_t serial = g_nvn_serial;
    int rc = checkpoint_io(io, &serial, sizeof serial);
    if (rc || serial == UINT64_MAX) return rc ? rc : -EBADMSG;
    uint64_t count = g_pk ? CFDictionaryGetCount(g_pk) : 0;
    if (io->loading && count) return -EBUSY;
    if ((rc = checkpoint_io(io, &count, sizeof count))) return rc;
    if (count > 1048576) return -EFBIG;
    const void **programs = NULL;
    if (!io->loading && count) {
        programs = calloc(count, sizeof *programs);
        if (!programs) return -ENOMEM;
        CFDictionaryGetKeysAndValues(g_pk, programs, NULL);
    }
    for (uint64_t i = 0; !rc && i < count; ++i) {
        uintptr_t addr = programs ? (uintptr_t)programs[i] : 0;
        prog_keys_t keys = {0};
        if (programs) keys = *(const prog_keys_t *)CFDictionaryGetValue(g_pk, programs[i]);
        if ((rc = checkpoint_io(io, &addr, sizeof addr)) ||
            (rc = checkpoint_io(io, &keys, sizeof keys))) break;
        // The address is an opaque dictionary key, never dereferenced here.
        if (!addr || (keys.ok & ~63u)) { rc = -EBADMSG; break; }
        if (io->loading) {
            if (g_pk && CFDictionaryGetValue(g_pk, (void *)addr)) { rc = -EBADMSG; break; }
            prog_keys_t *slot = pk_get((void *)addr, 1);
            if (!slot) { rc = -ENOMEM; break; }
            *slot = keys;
        }
    }
    free(programs);
    if (!rc && io->loading) g_nvn_serial = serial;
    if (!rc) LOG("[snapshot] %s shader keys for %llu programs", io->loading ? "restored" : "captured", (unsigned long long)count);
    return rc;
}

// The capture gate has drained all jobs, Vulkan completions and presents.
// Keep their guest-visible numbering while using fresh physical Vulkan fences.
int dm_checkpoint_graphics_timeline(int fd, int restore) {
    struct { char magic[8]; uint64_t frame, jobs, device, ticket; } t = {
        .magic = "NX2AGT3", .frame = g_frame, .jobs = g_job_done,
        .device = NVN_FENCE_BASE + mtl_last_fence()
    };
    checkpoint_stream io = {.fd = fd, .loading = restore};
    int rc = restore ? 0 : dm_checkpoint_present_ticket(&t.ticket, 0);
    if (!rc) rc = checkpoint_io(&io, &t, sizeof t);
    int has_modes = !memcmp(t.magic, "NX2AGT3", 8);
    uint32_t dm_checkpoint_clip_modes(void);
    uint32_t modes = dm_checkpoint_clip_modes();
    if (!rc && has_modes) rc = checkpoint_io(&io, &modes, sizeof modes);
    if (!rc && (has_modes || !memcmp(t.magic, "NX2AGT2", 8))) rc = checkpoint_program_keys(&io);
    else if (!rc && memcmp(t.magic, "NX2AGT1", 8)) rc = -EBADMSG;
    uint32_t expected = io.crc, sum = expected;
    if (!rc) rc = checkpoint_io(&io, &sum, sizeof sum);
    if (rc || !restore) return rc;
    if (sum != expected || modes > 3 ||
        ((t.frame | t.jobs | t.device | t.ticket) & ASYNC_FENCE) ||
        g_frame || g_job_enq || g_job_done) return -EBADMSG;
    if ((rc = dm_checkpoint_present_ticket(&t.ticket, 1))) return rc;
    if (has_modes) {
        mtl_set_origin_lower_left((modes >> 1) & 1);
        mtl_set_depth_zero(modes & 1);
    }
    g_frame = t.frame;
    g_job_enq = g_job_done = checkpoint_done_job = t.jobs;
    checkpoint_device_fence = t.device;
    LOG("[snapshot] restored graphics timeline: frame=%llu jobs=%llu device=%llu present=%llu",
        (unsigned long long)t.frame, (unsigned long long)t.jobs, (unsigned long long)t.device, (unsigned long long)t.ticket);
    return 0;
}

int dm_checkpoint_graphics_begin(int report_busy) {
    pthread_mutex_lock(&g_susp_mu);
    if (g_susp || g_inflight_api) {
        if (report_busy) LOG("[snapshot] graphics busy: lifecycle=%d in-flight API=%d", g_susp, g_inflight_api);
        pthread_mutex_unlock(&g_susp_mu);
        return -EBUSY;
    }
    g_susp |= 2;
    pthread_mutex_unlock(&g_susp_mu);

    int rc = -EBUSY;
    if (!pthread_mutex_trylock(&g_sub_mu)) {
        pthread_mutex_lock(&g_job_mu);
        int idle = !g_njob && g_job_done == g_job_enq;
        if (!idle && report_busy) LOG("[snapshot] replay pending: queued=%d", g_njob);
        pthread_mutex_unlock(&g_job_mu);
        if (idle) rc = dm_checkpoint_present_begin(report_busy);
        if (!rc) {
            // GPU idle precedes the completion worker's writes into guest
            // memory (reports, events and shadow-pool copies).
            if (mtl_wait(mtl_last_fence(), 1000000000ull)) return 0;
            dm_checkpoint_present_end();
            rc = -ETIMEDOUT;
        }
        pthread_mutex_unlock(&g_sub_mu);
    } else if (report_busy) LOG("[snapshot] submission mutex held");
    pthread_mutex_lock(&g_susp_mu);
    g_susp &= ~2;
    pthread_cond_broadcast(&g_susp_cv);
    pthread_mutex_unlock(&g_susp_mu);
    return rc;
}

void dm_checkpoint_graphics_end(void) {
    dm_checkpoint_present_end();
    pthread_mutex_unlock(&g_sub_mu);
    pthread_mutex_lock(&g_susp_mu);
    g_susp &= ~2;
    pthread_cond_broadcast(&g_susp_cv);
    pthread_mutex_unlock(&g_susp_mu);
}
