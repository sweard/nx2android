// Included by hle_audio.c for access to its private runtime state.
#include "services.h"
// Called by the single checkpoint coordinator after all guest producers
// and the SDK renderer have parked. A held pull also pins the output
// against au_close until the coordinator releases it.
static aout *checkpoint_pulls[AOMAX];
void dm_checkpoint_audio_end(void) {
    for (int i = 0; i < AOMAX; ++i) {
        if (checkpoint_pulls[i]) pull_end(checkpoint_pulls[i]);
        checkpoint_pulls[i] = NULL;
    }
}
int dm_checkpoint_audio_begin(void) {
    const uint64_t deadline = now_ns() + 500000000;
    for (;;) {
        int ready = 1;
        pthread_mutex_lock(&g_aos_lock);
        for (int i = 0; i < AOMAX; ++i) {
            aout *a = g_aos[i];
            if (!a) continue;
            if (!checkpoint_pulls[i] && pull_begin(a)) checkpoint_pulls[i] = a;
            if (checkpoint_pulls[i] != a || atomic_load(&a->notify_armed) ||
                atomic_load(&a->notify_pending)) ready = 0;
        }
        pthread_mutex_unlock(&g_aos_lock);
        if (ready) return 0;
        if (now_ns() >= deadline) break;
        usleep(1000);
    }
    dm_checkpoint_audio_end();
    return -EBUSY;
}
#include "../checkpoint_io.h"
#include "../checkpoint_memory.h"
#include <sys/stat.h>

// Guest-owned AudioOuts only. The renderer owns a separate PCM sink with
// process-local buffers. It must create that sink with its own worker.
typedef struct {
    uint64_t key, event, released[RELMAX];
    uint32_t channels, volume, state, count;
} checkpoint_audio_out;

// Capture the descriptor identity and the PCM range latched at Append, which
// can differ from the guest descriptor's current fields. PCM is in guest memory.
typedef struct {
    uint32_t count, reserved;
    uint64_t position;
    struct { uint64_t buffer, pcm, size; } blocks[QMAX];
} checkpoint_audio_queue;

static int audio_range(uint64_t address, size_t bytes) {
    if (dm_checkpoint_memory_committed(address, bytes)) return 1;
    module_t *m = module_of((void *)(uintptr_t)address);
    if (!m) return 0;
    uintptr_t off = address - (uintptr_t)m->base;
    return off >= m->data_off && off < m->size && bytes <= m->size - off;
}

static int valid_checkpoint_queue(const checkpoint_audio_queue *q, uint32_t released) {
    if (q->count > QMAX || q->count + released > RELMAX || q->position > q->blocks[0].size) return 0;
    for (unsigned j = 0; j < q->count; ++j) {
        if (!audio_range(q->blocks[j].buffer, sizeof(gbuf)) ||
            (q->blocks[j].pcm && q->blocks[j].size && !audio_range(q->blocks[j].pcm, q->blocks[j].size)))
            return 0;
    }
    return 1;
}

int dm_checkpoint_audio_outputs(int fd, int restore) {
    struct {
        char magic[8];
        uint32_t count, crc;
        checkpoint_audio_out out[AOMAX];
    } table = {.magic = "NX2AAU1"};
    checkpoint_stream io = {.fd = fd, .loading = restore};
    checkpoint_audio_queue queues[AOMAX] = {0};
    int has_queues = 0;
    if (!restore) {
        for (int i = 0; i < AOMAX; ++i) {
            aout *a = checkpoint_pulls[i];
            if (!a || a->rel_sem) continue;
            checkpoint_audio_queue *q = &queues[table.count];
            uint32_t first = atomic_load(&a->q_head);
            q->count = atomic_load(&a->q_tail) - first;
            q->position = a->pos;
            if (q->count > QMAX) return -ERANGE;
            for (unsigned j = 0; j < q->count; ++j) {
                unsigned idx = (first + j) % QMAX;
                q->blocks[j].buffer = (uintptr_t)a->q[idx].b;
                q->blocks[j].pcm = (uintptr_t)a->q[idx].ptr;
                q->blocks[j].size = a->q[idx].size;
            }
            has_queues |= q->count != 0;
            checkpoint_audio_out *r = &table.out[table.count++];
            r->key = (uintptr_t)a->checkpoint_key;
            r->event = (uintptr_t)a->event;
            r->channels = a->ch;
            r->state = atomic_load(&a->state);
            r->volume = atomic_load(&a->vol);
            uint32_t head = atomic_load(&a->rel_head);
            r->count = atomic_load(&a->rel_tail) - head;
            if (r->count > RELMAX || !valid_checkpoint_queue(q, r->count)) return -ERANGE;
            for (unsigned j = 0; j < r->count; ++j) r->released[j] = (uintptr_t)a->rel[(head+j) % RELMAX];
        }
        if (has_queues) memcpy(table.magic, "NX2AAU2", 8);
        table.crc = crc32(0, (const Bytef *)table.out, sizeof table.out);
        if (has_queues) table.crc = crc32(table.crc, (const Bytef *)queues, table.count * sizeof *queues);
    }
    int rc = checkpoint_io(&io, &table, sizeof table);
    if (rc || table.count > AOMAX) return rc ? rc : -EBADMSG;
    has_queues = !memcmp(table.magic, "NX2AAU2", 8);
    if (has_queues) rc = checkpoint_io(&io, queues, table.count * sizeof *queues);
    if (rc || !restore) return rc;
    struct stat st;
    uLong crc = crc32(0, (const Bytef *)table.out, sizeof table.out);
    if (has_queues) crc = crc32(crc, (const Bytef *)queues, table.count * sizeof *queues);
    if (fstat(fd, &st) || st.st_size != io.off || (!has_queues && memcmp(table.magic, "NX2AAU1", 8)) ||
        table.crc != crc) return -EBADMSG;
    for (unsigned i = 0; i < table.count; ++i) {
        const checkpoint_audio_out *r = &table.out[i];
        if (!audio_range(r->key, 4) || (r->event && !audio_range(r->event, 0x30)) ||
            (r->channels != 2 && r->channels != 6) || r->state > STATE_STOPPED || r->count > RELMAX) return -ERANGE;
        void *key = (void *)(uintptr_t)r->key;
        if (objmap_get(key)) return -EEXIST;
        for (unsigned j = 0; j < r->count; ++j)
            if (!audio_range(r->released[j], sizeof(gbuf))) return -ERANGE;
        const checkpoint_audio_queue *q = &queues[i];
        if (!valid_checkpoint_queue(q, r->count)) return -ERANGE;
        uint32_t param[2] = {RATE, r->channels};
        rc = au_open_at(key, (void *)(uintptr_t)r->event, NULL, (const uint8_t *)param, NULL);
        if (rc) return -EIO;
        aout *a = ao(key);
        for (unsigned j = 0; j < q->count; ++j) {
            a->q[j].b = (gbuf *)(uintptr_t)q->blocks[j].buffer;
            a->q[j].ptr = (void *)(uintptr_t)q->blocks[j].pcm;
            a->q[j].size = q->blocks[j].size;
        }
        a->pos = q->position;
        atomic_store(&a->q_tail, q->count);
        for (unsigned j = 0; j < r->count; ++j) a->rel[j] = (gbuf *)(uintptr_t)r->released[j];
        atomic_store(&a->rel_tail, r->count);
        atomic_store(&a->vol, r->volume);
        if (r->state == STATE_STARTED) au_start_at(key, NULL);
        LOG("[snapshot] restored guest AudioOut channels=%u released=%u queued=%u position=%llu",
            r->channels, r->count, q->count, (unsigned long long)q->position);
    }
    return 0;
}
void dm_checkpoint_report_audio(void) {
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; ++i) {
        aout *a = g_aos[i];
        if (!a) continue;
        LOG("[snapshot] audio output #%d channels=%d pending=%u released=%u position=%llu host_sink=%d",
            a->id, a->ch, atomic_load(&a->q_tail) - atomic_load(&a->q_head),
            atomic_load(&a->rel_tail) - atomic_load(&a->rel_head), (unsigned long long)a->pos, !!a->rel_sem);
    }
    pthread_mutex_unlock(&g_aos_lock);
}
