// Link into a temporary NativeActivity with --wrap for the three entry points.
// Uses the real save and renderer, but never publishes/consumes a checkpoint.
#include "dm.h"
#include "checkpoint_memory.h"
#include "checkpoint_android.h"
#include "checkpoint_wait.h"
#include "checkpoint_wait_cpu.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <unistd.h>
#include <sys/stat.h>
void dm_checkpoint_report_threads(void);
static const char *survey_files;
static _Atomic int diagnostic_state = DM_CHECKPOINT_IDLE;
static _Atomic int sample_image;
int dm_checkpoint_sample_image(int fd, int *id);
void dm_overlay_redraw(void);

int __wrap_dm_checkpoint_state(void) { return atomic_load(&diagnostic_state); }
void __wrap_dm_checkpoint_request(void) {
    dm_checkpoint_wait waits[256];
    unsigned n;
    int rc = dm_checkpoint_waits_capture(waits, 256, &n);
    LOG("[snapshot] manual checkpoint: parked waiters=%u result=%d", n, rc);
    unsigned captured = 0, portable = 0;
    for (unsigned i = 0; !rc && i < n; ++i) {
        dm_checkpoint_wait_cpu cpu;
        if (!dm_checkpoint_wait_cpu_capture(waits[i].thread, &cpu)) {
            ++captured;
            if (!dm_checkpoint_wait_cpu_supported(&cpu)) ++portable;
        }
    }
    LOG("[snapshot] sampled SVC continuations with guest stack=%u, with mapped TLS/TLR=%u of %u waits", captured, portable, n);
    dm_checkpoint_report_threads();
    // No complete CPU/HLE continuation exists for every gameplay thread. Do
    // not publish a ready slot. Capture one live resource on the replay thread.
    atomic_store(&sample_image, 1);
    atomic_store(&diagnostic_state, DM_CHECKPOINT_FAILED);
    dm_overlay_redraw();
}
#include "checkpoint_modules.h"
void __real_load_modules(const char *, const char **, int);
void __real_vk_tex_sweep(void);
void dm_checkpoint_report_pools(void);
void dm_checkpoint_report_images(void);

void __wrap_load_modules(const char *dir, const char **mods, int count) {
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
        char path[1024];
        snprintf(path, sizeof path, "%s/.runtime-survey-field-image", survey_files);
        int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        int id = 0, rc = fd < 0 ? -errno : dm_checkpoint_sample_image(fd, &id);
        if (fd >= 0) { if (!rc && (fchmod(fd, 0644) || fsync(fd))) rc = -errno; close(fd); if (rc) unlink(path); }
        LOG("[snapshot] manual image sample: NVN texture id=%d result=%d", id, rc);
        dm_checkpoint_report_images();
    }
    if (++frames != 1 && frames % 300) return;
    LOG("[snapshot] inventory at replay frame %u", frames);
    dm_checkpoint_report_threads();
    dm_checkpoint_report_pools();
    dm_checkpoint_report_images();
}

int __wrap_dm_checkpoint_init(const char *private_root, const char *files_root) {
    (void)private_root;
    survey_files = files_root;
    return 0;
}
