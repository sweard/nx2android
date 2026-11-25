// Opt-in Android runtime checkpoint capture and startup.
#include "dm.h"
#include "gsr2_profile.h"
#include "checkpoint_memory.h"
#include "checkpoint_android.h"
#include "saves_android.h"
#include "checkpoint_wait.h"
#include "checkpoint_pause.h"
#include <pthread.h>
#include "checkpoint_wait_cpu.h"
#include <errno.h>
#include <dlfcn.h>
#include "survey_threads.h"
#include "slot.h"
#include "save_commit.h"
#include "sections.h"
#include "checkpoint_pools.h"
#include "checkpoint_modules.h"
#include <fcntl.h>
#include "checkpoint_image.h"
#include "restore.h"
#include "services.h"
#include <stdatomic.h>
#include <unistd.h>
#include <sys/stat.h>
static const char *survey_files, *survey_private;
static int diagnostic_samples;
static _Atomic int checkpoint_state = DM_CHECKPOINT_IDLE;
static _Atomic int sample_image;
void dm_overlay_redraw(void);

int dm_checkpoint_state(void) { return atomic_load(&checkpoint_state); }
static void survey_callers(const dm_checkpoint_wait_cpu *thread) {
    unsigned depth;
    uintptr_t end = survey_thread_root(thread, &depth);
    void *root = dm_checkpoint_thread_root(thread->handle == 0x1001);
    if (root && end == (uintptr_t)root) return;
    Dl_info info = {0};
    if (end && dladdr((void *)end, &info) && info.dli_fname) {
        const char *file = strrchr(info.dli_fname, '/');
        LOG("[snapshot] thread %#x caller depth=%u native=%s+%#lx", thread->handle,
            depth, file ? file + 1 : info.dli_fname, (unsigned long)(end - (uintptr_t)info.dli_fbase));
    } else LOG("[snapshot] thread %#x caller depth=%u unresolved", thread->handle, depth);
}

// Private diagnostic file only. These sections can be compared after process
// death without presenting a partial snapshot in the recovery dialog.
static int survey_cpu_section(const dm_checkpoint_wait *waits, unsigned n,
                              const dm_checkpoint_wait_cpu *cpus, unsigned count) {
    char path[1024], staging[1024];
    if (snprintf(path, sizeof path, "%s/.runtime-survey-threads", survey_files) >= (int)sizeof path ||
        snprintf(staging, sizeof staging, "%s.part", path) >= (int)sizeof staging) return -ENAMETOOLONG;
    int fd = open(staging, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -errno;
    int rc = survey_write_threads(fd, waits, n, cpus, count);
    if (!rc && fsync(fd)) rc = -errno;
    if (close(fd) && !rc) rc = -errno;
    if (!rc && rename(staging, path)) rc = -errno;
    if (rc) unlink(staging);
    return rc;
}

// Each manual capture has its own private directory. Only a completed
// capture can replace the slot offered at the next launch.
static int survey_core(const dm_checkpoint_wait *waits, unsigned n,
                       const dm_checkpoint_wait_cpu *cpus, unsigned count) {
    char dir[1024];
    if (snprintf(dir, sizeof dir, "%s/.runtime-survey-core.XXXXXX", survey_private) >= (int)sizeof dir) return -ENAMETOOLONG;
    int rc = 0;
    if (!mkdtemp(dir)) return -errno;
    int fd[SECTION_COUNT];
    for (unsigned i = 0; i < SECTION_COUNT; ++i) fd[i] = -1;
    int root = open(dir, O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0) rc = -errno;
    for (unsigned i = 0; !rc && i < SECTION_COUNT; ++i) {
        fd[i] = openat(root, checkpoint_filenames[i], O_CREAT | O_EXCL | O_CLOEXEC | O_RDWR, 0600);
        if (fd[i] < 0) rc = -errno;
    }
    if (!rc) rc = survey_write_threads(fd[SECTION_THREADS], waits, n, cpus, count);
    if (!rc) rc = dm_checkpoint_files(fd[SECTION_FILES], 0);
    if (!rc) rc = dm_checkpoint_mounts(fd[SECTION_MOUNTS], dir, 0);
    if (!rc) rc = dm_checkpoint_audio_outputs(fd[SECTION_AUDIO], 0);
    if (!rc) rc = dm_checkpoint_graphics_timeline(fd[SECTION_TIMELINE], 0);
    if (!rc) rc = dm_checkpoint_renderer(fd[SECTION_RENDERER], 0, NULL);
    if (!rc) rc = dm_checkpoint_handles_save(fd[SECTION_HANDLES]);
    if (!rc) rc = dm_checkpoint_events_save(fd[SECTION_EVENTS]);
    LOG("[snapshot] scene events result=%d", rc);
    if (!rc) rc = dm_checkpoint_bindings_save(fd[SECTION_BINDINGS]);
    LOG("[snapshot] scene through bindings result=%d", rc);
    if (!rc) rc = dm_checkpoint_modules_save(fd[SECTION_MODULES]);
    LOG("[snapshot] scene through modules result=%d", rc);
    if (!rc) rc = dm_checkpoint_pools_save(fd[SECTION_MEMORY], fd[SECTION_POOLS]);
    if (!rc) rc = dm_checkpoint_scene_images(fd[SECTION_IMAGES], 0);
    for (unsigned i = 0; i < SECTION_COUNT; ++i) {
        if (fd[i] < 0) continue;
        if (!rc && fsync(fd[i])) rc = -errno;
        if (close(fd[i]) && !rc) rc = -errno;
    }
    if (root >= 0) {
        if (!rc && fsync(root)) rc = -errno;
        if (rc) for (unsigned i = 0; i < SECTION_COUNT; ++i) unlinkat(root, checkpoint_filenames[i], 0);
        close(root);
    }
    if (rc) dm_saves_checkpoint_remove(dir);
    else if (!(rc = dm_checkpoint_slot_publish(dir))) dm_checkpoint_slot_prune(strrchr(dir, '/') + 1);
    LOG("[snapshot] scene core result=%d directory=%s", rc, dir);
    return rc;
}

static void *survey_threads(void *unused) {
    (void)unused;
    int paused = dm_checkpoint_pause_begin();
    dm_checkpoint_wait waits[256];
    unsigned n = 0, count = 0, clocks = 0;
    dm_checkpoint_wait_cpu *cpus = calloc(256, sizeof *cpus);
    int rc = paused ? paused : !cpus ? -ENOMEM : dm_checkpoint_audio_begin();
    int audio_held = !rc, graphics_held = 0;
    if (!rc) {
        for (unsigned i = 0; i < 100; ++i) {
            rc = dm_checkpoint_graphics_begin(i == 99);
            if (rc != -EBUSY) break;
            usleep(2000);
        }
        graphics_held = !rc;
    }
    if (!rc) rc = dm_checkpoint_waits_capture(waits, 256, &n);
    for (unsigned i = 0; !rc && i < n; ++i) {
        rc = dm_checkpoint_wait_cpu_capture(waits[i].thread, &cpus[count]);
        if (!rc) rc = dm_checkpoint_wait_cpu_supported(&cpus[count]);
        if (!rc) ++count;
    }
    if (!rc) rc = dm_checkpoint_clock_cpus(cpus + count, 256 - count, &clocks);
    count += clocks;
    if (!rc && count != dm_checkpoint_pause_count()) rc = -ENOTSUP;
    if (!rc) {
        if (diagnostic_samples) {
            for (unsigned i = 0; i < count; ++i) survey_callers(&cpus[i]);
            rc = survey_cpu_section(waits, n, cpus, count);
        }
    }
    LOG("[snapshot] thread sample: handles=%u kernel waits=%u clock readers=%u result=%d", count, n, clocks, rc);
#if DM_HAS_GSR2_ADAPTER
    if (!rc && !dm_gsr2_release_resolution()) rc = -EAGAIN;
#endif
    if (!rc) {
        dm_checkpoint_report_files();
        dm_checkpoint_report_audio();
        rc = survey_core(waits, n, cpus, count);
    }
    free(cpus);
    if (graphics_held) dm_checkpoint_graphics_end();
    if (audio_held) dm_checkpoint_audio_end();
    dm_checkpoint_report_threads();
    if (!paused) dm_checkpoint_pause_end();
    LOG("[snapshot] rendezvous released: result=%d", rc);
    // Sample one live resource on the replay thread after releasing the guest.
    atomic_store(&sample_image, diagnostic_samples);
    int status = rc ? DM_CHECKPOINT_FAILED : DM_CHECKPOINT_SAVED;
    atomic_store(&checkpoint_state, status);
    dm_checkpoint_notify(status);
    dm_overlay_redraw();
    return NULL;
}

void dm_checkpoint_request(void) {
    int previous = atomic_exchange(&checkpoint_state, DM_CHECKPOINT_SAVING);
    if (previous == DM_CHECKPOINT_SAVING) return;
    dm_checkpoint_notify(DM_CHECKPOINT_SAVING);
    pthread_t worker;
    if (!pthread_create(&worker, NULL, survey_threads, NULL)) pthread_detach(worker);
    else {
        atomic_store(&checkpoint_state, DM_CHECKPOINT_FAILED);
        dm_checkpoint_notify(DM_CHECKPOINT_FAILED);
    }
    dm_overlay_redraw();
}

void dm_checkpoint_load_modules(const char *dir, const char **mods, int count) {
    // An adb-shell process cannot enter the app-private capture directory on
    // some devices. Run the same read-only check in a fresh app process.
    if (!survey_private) { // Standalone dmhost has no Activity checkpoint slot.
        load_modules(dir, mods, count);
        return;
    }
    char request[1024], scene[1024], basename[40] = {0};
    dm_checkpoint_slot_remove_discarded();
    const char *chosen_scene = dm_checkpoint_slot_name();
    if (chosen_scene[0]) {
        int fits = snprintf(scene, sizeof scene, "%s/%s", survey_private, chosen_scene) < (int)sizeof scene;
        int rc = fits ? dm_checkpoint_save_session(getenv("DM_SAVE"), scene) : -ENAMETOOLONG;
        if (!rc) rc = dm_checkpoint_replay_scene(dir, scene, load_modules, CHECKPOINT_PLAY, NULL);
        LOG("[snapshot] restored session ended, result=%d", rc);
        // The failure dialog closes the activity. Never return to a partially restored guest.
        if (rc && dm_checkpoint_notify(DM_CHECKPOINT_RESTORE_FAILED)) for (;;) pause();
        _exit(rc ? 1 : 0);
    }
    if (survey_files && snprintf(request, sizeof request, "%s/.runtime-survey-replay-once", survey_files) < (int)sizeof request) {
        int fd = open(request, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            ssize_t len = read(fd, basename, sizeof basename - 1);
            close(fd);
            int rc = 2;
            if (!unlink(request) && len > 0 && (size_t)len == strlen(basename) && dm_checkpoint_slot_valid_name(basename) &&
                snprintf(scene, sizeof scene, "%s/%s", survey_private, basename) < (int)sizeof scene) {
                rc = dm_checkpoint_replay_scene(dir, scene, load_modules, CHECKPOINT_PROBE_SESSION, NULL);
            }
            LOG("[snapshot] cold scene inspection complete pid=%d result=%d", getpid(), rc);
            fflush(NULL);
            _exit(rc);
        }
    }
    int rc = dm_checkpoint_modules_begin(dir, mods, count, -1);
    if (!rc) rc = dm_checkpoint_memory_begin();
    if (rc) die("snapshot layout setup failed (%d)", rc);
    load_modules(dir, mods, count);
    rc = dm_checkpoint_modules_baseline();
    if (rc) die("snapshot module baseline failed (%d)", rc);
    LOG("[snapshot] cold layout ready for %s", dm_main_build());
}

void dm_checkpoint_after_texture_sweep(void) {
    static unsigned frames;
    if (atomic_exchange(&sample_image, 0)) {
        char path[1024], staging[1024];
        snprintf(path, sizeof path, "%s/.runtime-survey-field-image", survey_files);
        snprintf(staging, sizeof staging, "%s/.runtime-survey-field-image.part", survey_files);
        int fd = open(staging, O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        int id = 0, rc = fd < 0 ? -errno : dm_checkpoint_sample_image(fd, &id);
        if (fd >= 0) {
            if (!rc && (fchmod(fd, 0644) || fsync(fd))) rc = -errno;
            if (close(fd) && !rc) rc = -errno;
            if (!rc && rename(staging, path)) rc = -errno;
            if (rc) unlink(staging);
        }
        LOG("[snapshot] manual image sample: NVN texture id=%d result=%d", id, rc);
        dm_checkpoint_report_images();
    }
    if (++frames != 1 && frames % 300) return;
    LOG("[snapshot] inventory at replay frame %u", frames);
    dm_checkpoint_report_threads();
    dm_checkpoint_report_pools();
    dm_checkpoint_report_images();
}

int dm_checkpoint_init(const char *private_root, const char *files_root) {
    survey_private = private_root;
    survey_files = files_root;
    char opt[1024];
    diagnostic_samples = snprintf(opt, sizeof opt, "%s/.runtime-survey-enable-core", files_root) < (int)sizeof opt && !access(opt, F_OK);
    return dm_checkpoint_slot_init(private_root, files_root);
}
