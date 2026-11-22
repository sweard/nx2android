#include "dm.h"
#include "slot.h"
#include "checkpoint_android.h"
#include "saves_android.h"
#include "sections.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *survey_private;
static char chosen_scene[64], discarded_scene[sizeof chosen_scene];
static int private_dir = -1;
static const char current[] = "runtime-checkpoint.current";
static const char pending[] = "runtime-checkpoint.pending";
static const char core_prefix[] = ".runtime-survey-core.";
int dm_checkpoint_slot_valid_name(const char *name) {
    size_t n = sizeof core_prefix - 1;
    return strlen(name) == n + 6 && !memcmp(name, core_prefix, n) &&
        strspn(name + n, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") == 6;
}

// Publish the directory name only after every section and its directory
// have reached storage. A failed replacement leaves the last slot usable.
int dm_checkpoint_slot_publish(const char *dir) {
    const char *name = strrchr(dir, '/');
    if (private_dir < 0 || !name || !dm_checkpoint_slot_valid_name(++name)) return -EINVAL;
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

void dm_checkpoint_field_choose(int choice) {
    if (choice == DM_CHECKPOINT_RESTORE) return;
    if (choice == DM_CHECKPOINT_DISCARD && dm_checkpoint_slot_valid_name(chosen_scene)) {
        // Retire the marker before starting the guest. A cold start can finish
        // the cleanup if the process dies before the host thread removes the files.
        if (!unlinkat(private_dir, current, 0)) {
            if (fsync(private_dir)) LOG("[snapshot] could not sync discarded checkpoint: %s", strerror(errno));
            memcpy(discarded_scene, chosen_scene, sizeof discarded_scene);
        } else LOG("[snapshot] could not discard current checkpoint: %s", strerror(errno));
    }
    chosen_scene[0] = 0;
}

void dm_checkpoint_slot_remove_discarded(void) {
    if (!discarded_scene[0]) return;
    char path[1024];
    if (snprintf(path, sizeof path, "%s/%s", survey_private, discarded_scene) < (int)sizeof path)
        dm_saves_checkpoint_remove(path);
    discarded_scene[0] = 0;
}

// These private mkdtemp directories belong to checkpoint capture even if the
// process died before creating its first section. Skip the live session's source.
void dm_checkpoint_slot_prune(const char *keep) {
    DIR *dir = opendir(survey_private);
    if (!dir) return;
    struct dirent *e;
    unsigned removed = 0;
    while ((e = readdir(dir))) {
        if (!dm_checkpoint_slot_valid_name(e->d_name) || !strcmp(keep, e->d_name) || !strcmp(chosen_scene, e->d_name)) continue;
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
// A missing marker is normal after an interrupted first save. Any other
// read/validation error leaves all scenes intact until the slot is identifiable.
static int read_current_scene(void) {
    int fd = openat(private_dir, current, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    ssize_t n = read(fd, chosen_scene, sizeof chosen_scene - 1);
    close(fd);
    if (n != sizeof core_prefix - 1 + 6 || !dm_checkpoint_slot_valid_name(chosen_scene)) return -1;
    int dir = openat(private_dir, chosen_scene, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return -1;
    for (unsigned i = 0; i < SECTION_COUNT; ++i) {
        struct stat st;
        if (fstatat(dir, checkpoint_filenames[i], &st, AT_SYMLINK_NOFOLLOW) || !S_ISREG(st.st_mode) || st.st_size == 0) {
            close(dir);
            return -1;
        }
    }
    close(dir);
    return 2;
}

int dm_checkpoint_slot_init(const char *private_root, const char *files_root) {
    survey_private = private_root;
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
            if (n != sizeof core_prefix - 1 + 6 || !dm_checkpoint_slot_valid_name(replay)) replay[0] = 0;
            close(fd);
        }
        close(files);
    }
    if (kind < 0) {
        chosen_scene[0] = 0;
        LOG("[snapshot] current checkpoint could not be verified; keeping scene directories");
        return 0;
    }
    dm_checkpoint_slot_prune(replay);
    return kind;
}

const char *dm_checkpoint_slot_name(void) { return chosen_scene; }
