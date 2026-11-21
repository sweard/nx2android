// Field checkpoint prototype for the opt-in Octopath II NativeActivity.
#include "dm.h"
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
#include "checkpoint_pools.h"
#include "checkpoint_modules.h"
int dm_checkpoint_bindings_save(int fd);
int dm_checkpoint_scene_images(int fd, int restore);
int dm_checkpoint_files(int fd, int restore);
#include <fcntl.h>
#include <dirent.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/stat.h>
void dm_checkpoint_report_threads(void);
void dm_checkpoint_report_files(void);
void dm_checkpoint_report_audio(void);
int dm_checkpoint_audio_outputs(int fd, int restore);
int dm_checkpoint_renderer(int, int, dm_checkpoint_wait_cpu *);
int dm_checkpoint_graphics_timeline(int fd, int restore);
int dm_checkpoint_audio_begin(void);
void dm_checkpoint_audio_end(void);
static const char *survey_files, *survey_private;
static int diagnostic_samples;
int dm_checkpoint_mounts(int fd, const char *scene, int restore);
static char chosen_scene[64];
static int private_dir = -1;
static const char current[] = "runtime-checkpoint.current";
static const char pending[] = "runtime-checkpoint.pending";
static const char core_prefix[] = ".runtime-survey-core.";
static const char *const names[] = {"events", "bindings", "modules", "memory", "pools", "threads", "images", "handles", "files", "mounts", "audio", "timeline", "renderer"};
enum { section_count = sizeof names / sizeof names[0] };

static int is_trial(const char *name) {
    size_t n = sizeof core_prefix - 1;
    return strlen(name) == n + 6 && !memcmp(name, core_prefix, n) &&
        strspn(name + n, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == 6;
}

// Publish the directory name only after every section and its directory
// have reached storage. A failed replacement leaves the last slot usable.
static int publish_scene(const char *dir) {
    const char *name = strrchr(dir, '/');
    if (private_dir < 0 || !name || !is_trial(++name)) return -EINVAL;
    int fd = openat(private_dir, pending, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -errno;
    ssize_t n = (ssize_t)strlen(name);
    int rc = write(fd, name, n) == n && !fsync(fd) ? 0 : -EIO;
    if (close(fd) && !rc) rc = -errno;
    if (!rc && renameat(private_dir, pending, private_dir, current)) rc = -errno;
    if (!rc && fsync(private_dir)) rc = -errno;
    if (rc) unlinkat(private_dir, pending, 0);
    return rc;
}

void dm_checkpoint_field_choose(int restore) {
    if (!restore) chosen_scene[0] = 0;
}

// These private mkdtemp directories belong to checkpoint capture even if the
// process died before creating its first section. Skip the live session's source.
static void prune_trials(const char *keep) {
    DIR *dir = opendir(survey_private);
    if (!dir) return;
    struct dirent *e;
    unsigned removed = 0;
    while ((e = readdir(dir))) {
        if (!is_trial(e->d_name) || !strcmp(keep, e->d_name) || !strcmp(chosen_scene, e->d_name)) continue;
        struct stat st;
        if (fstatat(dirfd(dir), e->d_name, &st, AT_SYMLINK_NOFOLLOW) || !S_ISDIR(st.st_mode)) continue;
        char trial[1400];
        if (snprintf(trial, sizeof trial, "%s/%s", survey_private, e->d_name) >= (int)sizeof trial) continue;
        dm_saves_checkpoint_remove(trial);
        if (fstatat(dirfd(dir), e->d_name, &st, AT_SYMLINK_NOFOLLOW) && errno == ENOENT) ++removed;
    }
    closedir(dir);
    LOG("[snapshot] removed %u old or interrupted checkpoint directories", removed);
}
static _Atomic int diagnostic_state = DM_CHECKPOINT_IDLE;
static _Atomic int sample_image;
int dm_checkpoint_sample_image(int fd, int *id);
void dm_overlay_redraw(void);

int __wrap_dm_checkpoint_state(void) { return atomic_load(&diagnostic_state); }
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
    int fd[section_count];
    for (unsigned i = 0; i < section_count; ++i) fd[i] = -1;
    int root = open(dir, O_DIRECTORY | O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (root < 0) rc = -errno;
    for (unsigned i = 0; !rc && i < section_count; ++i) {
        fd[i] = openat(root, names[i], O_CREAT | O_EXCL | O_CLOEXEC | O_RDWR, 0600);
        if (fd[i] < 0) rc = -errno;
    }
    if (!rc) rc = survey_write_threads(fd[5], waits, n, cpus, count);
    if (!rc) rc = dm_checkpoint_files(fd[8], 0);
    if (!rc) rc = dm_checkpoint_mounts(fd[9], dir, 0);
    if (!rc) rc = dm_checkpoint_audio_outputs(fd[10], 0);
    if (!rc) rc = dm_checkpoint_graphics_timeline(fd[11], 0);
    if (!rc) rc = dm_checkpoint_renderer(fd[12], 0, NULL);
    if (!rc) rc = dm_checkpoint_handles_save(fd[7]);
    if (!rc) rc = dm_checkpoint_events_save(fd[0]);
    LOG("[snapshot] scene events result=%d", rc);
    if (!rc) rc = dm_checkpoint_bindings_save(fd[1]);
    LOG("[snapshot] scene through bindings result=%d", rc);
    if (!rc) rc = dm_checkpoint_modules_save(fd[2]);
    LOG("[snapshot] scene through modules result=%d", rc);
    if (!rc) rc = dm_checkpoint_pools_save(fd[3], fd[4]);
    if (!rc) rc = dm_checkpoint_scene_images(fd[6], 0);
    for (unsigned i = 0; i < section_count; ++i) {
        if (fd[i] < 0) continue;
        if (!rc && fsync(fd[i])) rc = -errno;
        if (close(fd[i]) && !rc) rc = -errno;
    }
    if (root >= 0) {
        if (!rc && fsync(root)) rc = -errno;
        if (rc) for (unsigned i = 0; i < section_count; ++i) unlinkat(root, names[i], 0);
        close(root);
    }
    if (rc) dm_saves_checkpoint_remove(dir);
    else if (!(rc = publish_scene(dir))) prune_trials(strrchr(dir, '/') + 1);
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
    atomic_store(&diagnostic_state, status);
    dm_checkpoint_notify(status);
    dm_overlay_redraw();
    return NULL;
}

void __wrap_dm_checkpoint_request(void) {
    int previous = atomic_exchange(&diagnostic_state, DM_CHECKPOINT_SAVING);
    if (previous == DM_CHECKPOINT_SAVING) return;
    dm_checkpoint_notify(DM_CHECKPOINT_SAVING);
    pthread_t worker;
    if (!pthread_create(&worker, NULL, survey_threads, NULL)) pthread_detach(worker);
    else {
        atomic_store(&diagnostic_state, DM_CHECKPOINT_FAILED);
        dm_checkpoint_notify(DM_CHECKPOINT_FAILED);
    }
    dm_overlay_redraw();
}
#include "checkpoint_modules.h"
void __real_load_modules(const char *, const char **, int);
void __real_vk_tex_sweep(void);
void dm_checkpoint_report_pools(void);
void dm_checkpoint_report_images(void);

void __wrap_load_modules(const char *dir, const char **mods, int count) {
    // An adb-shell process cannot enter the app-private capture directory on
    // some devices. Run the same read-only check in a fresh app process.
    char request[1024], scene[1024], basename[40] = {0};
    if (chosen_scene[0]) {
        int fits = snprintf(scene, sizeof scene, "%s/%s", survey_private, chosen_scene) < (int)sizeof scene;
        int svc_resume_private_scene(const char *, const char *, void (*)(const char *, const char **, int));
        int rc = fits ? svc_resume_private_scene(dir, scene, __real_load_modules) : -ENAMETOOLONG;
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
            if (!unlink(request) && len == (ssize_t)sizeof core_prefix - 1 + 6 && is_trial(basename) &&
                snprintf(scene, sizeof scene, "%s/%s", survey_private, basename) < (int)sizeof scene) {
                int svc_replay_private_scene(const char *, const char *, void (*)(const char *, const char **, int));
                rc = svc_replay_private_scene(dir, scene, __real_load_modules);
            }
            LOG("[snapshot] cold scene inspection complete pid=%d result=%d", getpid(), rc);
            fflush(NULL);
            _exit(rc);
        }
    }
    int rc = dm_checkpoint_modules_begin(dir, mods, count, -1);
    if (!rc) rc = dm_checkpoint_memory_begin();
    if (rc) die("snapshot layout setup failed (%d)", rc);
    __real_load_modules(dir, mods, count);
    rc = dm_checkpoint_modules_baseline();
    if (rc) die("snapshot module baseline failed (%d)", rc);
    LOG("[snapshot] cold layout ready for %s", dm_main_build());
}

void __wrap_vk_tex_sweep(void) {
    static unsigned frames;
    __real_vk_tex_sweep();
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

// A missing marker is normal after an interrupted first save. Any other
// read/validation error leaves all scenes intact until the slot is identifiable.
static int read_current_scene(void) {
    int fd = openat(private_dir, current, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    ssize_t n = read(fd, chosen_scene, sizeof chosen_scene - 1);
    close(fd);
    if (n != sizeof core_prefix - 1 + 6 || !is_trial(chosen_scene)) return -1;
    int dir = openat(private_dir, chosen_scene, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return -1;
    for (unsigned i = 0; i < section_count; ++i) {
        struct stat st;
        if (fstatat(dir, names[i], &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_size == 0) {
            close(dir);
            return -1;
        }
    }
    close(dir);
    return 2;
}

int __wrap_dm_checkpoint_init(const char *private_root, const char *files_root) {
    survey_private = private_root;
    survey_files = files_root;
    char opt[1024];
    diagnostic_samples = snprintf(opt, sizeof opt, "%s/.runtime-survey-enable-core", files_root) < (int)sizeof opt && !access(opt, F_OK);
    private_dir = open(private_root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (private_dir < 0) return 0;
    int kind = read_current_scene();
    unlinkat(private_dir, pending, 0);
    char replay[sizeof chosen_scene] = {0};
    int files = open(files_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (files >= 0) {
        unlinkat(files, ".runtime-survey-threads.part", 0);
        unlinkat(files, ".runtime-survey-field-image.part", 0);
        // A one-shot diagnostic can explicitly request an older scene.
        int fd = openat(files, ".runtime-survey-replay-once", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd >= 0) {
            ssize_t n = read(fd, replay, sizeof replay - 1);
            if (n != sizeof core_prefix - 1 + 6 || !is_trial(replay)) replay[0] = 0;
            close(fd);
        }
        close(files);
    }
    if (kind < 0) {
        chosen_scene[0] = 0;
        LOG("[snapshot] current checkpoint could not be verified; keeping scene directories");
        return 0;
    }
    prune_trials(replay);
    return kind;
}
