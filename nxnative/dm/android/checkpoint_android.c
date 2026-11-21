#include "dm.h"
#include "checkpoint_android.h"
#include "saves_android.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char marker[] = "NX2A manual committed-save checkpoint 1\n";
static char slot[768], staging[768], saved_settings[800];
static const char *files;
static int directory = -1;
static _Atomic int state;
void dm_overlay_redraw(void);

static int clear_marker(void) {
    if (unlinkat(directory, "ready", 0)) return errno == ENOENT ? 0 : -1;
    return fsync(directory);
}

static int ready(void) {
    char value[sizeof marker];
    int fd = openat(directory, "ready", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    ssize_t n = read(fd, value, sizeof value);
    close(fd);
    return n == sizeof marker - 1 && !memcmp(value, marker, sizeof marker - 1);
}

static int publish(void) {
    if (rename(staging, slot) || fsync(directory)) return -1;
    int fd = openat(directory, ".ready.new", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, marker, sizeof marker - 1) == sizeof marker - 1 && !fsync(fd);
    if (close(fd)) ok = 0;
    if (ok && !renameat(directory, ".ready.new", directory, "ready")) {
        if (!fsync(directory)) return 0;
        clear_marker();
    }
    unlinkat(directory, ".ready.new", 0);
    return -1;
}

static void *save_worker(void *arg) {
    (void)arg;
    struct timespec start = {0}, end = {0};
    clock_gettime(CLOCK_MONOTONIC, &start);
    dm_saves_checkpoint_remove(slot);
    dm_saves_checkpoint_remove(staging);
    int result = dm_saves_checkpoint_capture(staging, files);
    if (!result) result = publish();
    dm_saves_checkpoint_remove(staging);
    clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = (end.tv_sec - start.tv_sec) * 1000.0 + (end.tv_nsec - start.tv_nsec) / 1e6;
    LOG("[resume] manual checkpoint %s in %.1f ms", result ? "unavailable" : "ready", ms);
    (void)ms;
    int status = !result ? DM_CHECKPOINT_SAVED : result == -ENOENT ? DM_CHECKPOINT_NO_SAVE : DM_CHECKPOINT_FAILED;
    atomic_store(&state, status);
    dm_checkpoint_notify(status);
    dm_overlay_redraw();
    return NULL;
}

int dm_checkpoint_init(const char *private_root, const char *files_root) {
    char path[720];
    if (snprintf(path, sizeof path, "%s/background-resume", private_root) >= (int)sizeof path) return 0;
    if (mkdir(path, 0700) && errno != EEXIST) return 0;
    directory = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0) return 0;
    files = files_root;
    snprintf(slot, sizeof slot, "%s/snapshot", path);
    snprintf(staging, sizeof staging, "%s/.snapshot-part", path);
    snprintf(saved_settings, sizeof saved_settings, "%s/settings.txt", slot);
    atomic_store(&state, DM_CHECKPOINT_IDLE);
    return ready() && dm_saves_checkpoint_matches(slot, files);
}

const char *dm_checkpoint_settings(void) { return saved_settings; }
int dm_checkpoint_state(void) { return atomic_load(&state); }

void dm_checkpoint_consume(void) {   // startup only, after the player has made a choice
    if (directory >= 0 && clear_marker()) LOG("[RED] [resume] could not consume the checkpoint");
}

void dm_checkpoint_request(void) {
    int previous = dm_checkpoint_state();
    if (previous == DM_CHECKPOINT_DISABLED || previous == DM_CHECKPOINT_SAVING ||
        !atomic_compare_exchange_strong(&state, &previous, DM_CHECKPOINT_SAVING)) return;
    dm_checkpoint_notify(DM_CHECKPOINT_SAVING);
    pthread_t thread;
    if (clear_marker() || pthread_create(&thread, NULL, save_worker, NULL)) {
        atomic_store(&state, DM_CHECKPOINT_FAILED);
        dm_checkpoint_notify(DM_CHECKPOINT_FAILED);
        LOG("[RED] [resume] could not start a manual checkpoint");
    } else {
        pthread_detach(thread);
    }
    dm_overlay_redraw();
}
