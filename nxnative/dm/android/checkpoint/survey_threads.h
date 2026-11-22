// Diagnostic format. Never accepted as a runnable checkpoint.
#pragma once
#include "checkpoint_io.h"
#include "checkpoint_wait.h"
#include "checkpoint_wait_cpu.h"
#include <sys/stat.h>
typedef struct { char magic[8]; uint32_t waits, cpus; } survey_thread_header;

// Follow only ABI frame records from the saved x29, in the current thread's
// committed guest memory. A frameless function can legitimately stop the walk.
static inline uintptr_t survey_thread_root(const dm_checkpoint_wait_cpu *thread, unsigned *depth) {
    uintptr_t sp = thread->cpu.sp, fp = thread->cpu.x[29], lr = thread->cpu.x[30];
    *depth = 0;
    while (lr && module_of((void *)lr)) {
        // The saved FP/LR pair needs pointer alignment. SP remains 16-byte
        // aligned, but a compiler can put this pair at SP+0x28.
        if (++*depth > 128 || (fp & 7) || fp < sp || fp - sp > DM_CHECKPOINT_MAIN_STACK_BYTES ||
            !dm_checkpoint_memory_committed(fp, 16)) return 0;
        uint64_t frame[2];
        memcpy(frame, (const void *)fp, sizeof frame);
        sp = fp + 16;
        fp = frame[0];
        lr = frame[1];
    }
    return lr;
}

static inline int survey_write_threads(int fd, const dm_checkpoint_wait *waits, unsigned n,
                                       const dm_checkpoint_wait_cpu *cpus, unsigned count) {
    survey_thread_header h = {"NX2ATD1", n, count};
    checkpoint_stream io = {.fd = fd};
    int rc = checkpoint_io(&io, &h, sizeof h);
    if (!rc) rc = checkpoint_io(&io, (void *)waits, n * sizeof *waits);
    if (!rc) rc = checkpoint_io(&io, (void *)cpus, count * sizeof *cpus);
    uint32_t crc = io.crc;
    return rc ? rc : checkpoint_io(&io, &crc, sizeof crc);
}

// Only inspect addresses after restoring the module and guest-memory sections.
// Install all waits together, before any guest or helper thread starts.
typedef struct {
    unsigned count;
    dm_checkpoint_wait_cpu cpus[256];
} survey_roster;

static inline int survey_validate_threads(int fd, dm_checkpoint_wait_cpu *selected, survey_roster *all) {
    checkpoint_stream io = {.fd = fd, .loading = 1};
    survey_thread_header h;
    struct stat st;
    if (checkpoint_io(&io, &h, sizeof h) || memcmp(h.magic, "NX2ATD1", 8) ||
        !h.cpus || h.cpus > 256 || h.waits > h.cpus || fstat(fd, &st) ||
        st.st_size != (off_t)(sizeof h + h.waits * sizeof(dm_checkpoint_wait) +
            h.cpus * sizeof(dm_checkpoint_wait_cpu) + sizeof(uint32_t))) return -EBADMSG;
    dm_checkpoint_wait waits[256];
    int rc = checkpoint_io(&io, waits, h.waits * sizeof *waits);
    uint32_t handles[256];
    unsigned roots = 0, unresolved = 0;
    for (unsigned i = 0; !rc && i < h.cpus; ++i) {
        dm_checkpoint_wait_cpu cpu;
        if ((rc = checkpoint_io(&io, &cpu, sizeof cpu))) break;
        if ((i < h.waits && cpu.handle != waits[i].thread) ||
            ((i >= h.waits) != (cpu.svc == DM_CHECKPOINT_RETRY_CLOCK))) return -EBADMSG;
        if ((rc = dm_checkpoint_wait_cpu_supported(&cpu))) break;
        if (dm_checkpoint_thread_root(0)) {
            unsigned depth;
            uintptr_t lr = survey_thread_root(&cpu, &depth);
            if (lr == (uintptr_t)dm_checkpoint_thread_root(cpu.handle == 0x1001)) ++roots;
            else if (!lr) {
                module_t *pc = module_of((void *)(uintptr_t)cpu.cpu.pc);
                module_t *caller = module_of((void *)(uintptr_t)cpu.cpu.x[30]);
                LOG("[snapshot] incomplete frame thread %#x depth=%u PC=%s+%#lx LR=%s+%#lx FP=%#lx SP=%#lx",
                    cpu.handle, depth, pc->name, (unsigned long)(cpu.cpu.pc - (uintptr_t)pc->base),
                    caller ? caller->name : "unknown", (unsigned long)(cpu.cpu.x[30] - (caller ? (uintptr_t)caller->base : 0)),
                    (unsigned long)cpu.cpu.x[29], (unsigned long)cpu.cpu.sp);
                ++unresolved;
            }
            else return -ESTALE;
        }
        for (unsigned j = 0; j < i; ++j) if (handles[j] == cpu.handle) return -EBADMSG;
        handles[i] = cpu.handle;
        if (all) all->cpus[i] = cpu;
    }
    uint32_t sum = io.crc, footer;
    if (!rc) rc = checkpoint_io(&io, &footer, sizeof footer);
    if (rc || sum != footer) return rc ? rc : -EBADMSG;
    if (dm_checkpoint_thread_root(0))
        LOG("[snapshot] cold thread roots: %u/%u stable, %u incomplete frame chains", roots, h.cpus, unresolved);
    rc = dm_checkpoint_waits_install(waits, h.waits);
    if (rc) return rc;
    LOG("[snapshot] installed %u field waits for %u CPU contexts", h.waits, h.cpus);
    if (all) {
        if (roots != h.cpus) return -ENOTSUP;
        all->count = h.cpus;
        return 0;
    }
    // Exercise one actual signal against disposable restored memory only.
    // Choose a free mutex so the expected outcome is a handoff, and compute
    // the predicted recipient independently from the runtime's wait list.
    for (unsigned i = 0; i < h.waits; ++i) {
        if (waits[i].kind != 2) continue;
        const dm_checkpoint_wait *best = &waits[i];
        for (unsigned j = 0; j < h.waits; ++j) {
            const dm_checkpoint_wait *w = &waits[j];
            if (w->kind == 2 && w->key == best->key &&
                (w->priority < best->priority || (w->priority == best->priority && w->sequence < best->sequence))) best = w;
        }
        uint32_t *word = (uint32_t *)(uintptr_t)best->address;
        if (*word || !best->tag) continue;
        unsigned pos = (unsigned)(best - waits);
        off_t offset = sizeof h + h.waits * sizeof *waits + pos * sizeof *selected;
        if (pread(fd, selected, sizeof *selected, offset) != sizeof *selected) return -EIO;
        module_t *caller = module_of((void *)(uintptr_t)selected->cpu.x[30]);
        size_t page = (size_t)getpagesize();
        if (selected->svc != 0x1c || *(uint32_t *)(uintptr_t)selected->cpu.pc != 0xd65f03c0 ||
            !caller || selected->cpu.x[30] - (uintptr_t)caller->base >= caller->text_end ||
            (selected->cpu.x[30] & 3) || selected->cpu.pc / page == selected->cpu.x[30] / page) continue;
        int svc_thread(int, uint64_t *);
        uint64_t regs[32] = {best->key, 1};
        if (!svc_thread(0x1d, regs) || regs[0] || *word != best->tag) return -EIO;
        LOG("[snapshot] cold CV signal handed the mutex to recorded thread %#x", best->thread);
        return 0;
    }
    return -ENOTSUP;
}
