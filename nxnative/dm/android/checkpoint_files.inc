// Caller stops all guest I/O. Reopen immutable game assets under the current
// installation's trusted roots. Save transactions require a separate section.
#include "checkpoint_io.h"
typedef struct {
    uint32_t guest_id, root, packed, reserved;
    uint64_t bytes;
    uint32_t first_crc, last_crc;
    char relative[1024];
} checkpoint_file;

static const char *asset_root(unsigned id) {
    const char *vars[] = {"DM_ROMFS", "DM_AOC_DIR", "DM_ROMFS_OVERLAY"};
    return id < 3 ? getenv(vars[id]) : NULL;
}
static int asset_fingerprint(int fd, checkpoint_file *r, int compare) {
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0) return -EBADMSG;
    uint8_t data[4096];
    size_t n = st.st_size < (off_t)sizeof data ? (size_t)st.st_size : sizeof data;
    uint32_t crc[2];
    for (unsigned i = 0; i < 2; ++i) {
        if (pread(fd, data, n, i ? st.st_size - n : 0) != (ssize_t)n) return -EIO;
        crc[i] = crc32(0, data, (uInt)n);
    }
    if (compare) return r->bytes == (uint64_t)st.st_size && r->first_crc == crc[0] && r->last_crc == crc[1] ? 0 : -ESTALE;
    r->bytes = st.st_size; r->first_crc = crc[0]; r->last_crc = crc[1]; return 0;
}
static int capture_file_path(int fd, checkpoint_file *r) {
    char proc[64], path[PATH_MAX], resolved[PATH_MAX], base[PATH_MAX];
    snprintf(proc, sizeof proc, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(proc, path, sizeof path - 1);
    if (n <= 0 || n == sizeof path-1) return -ENOTSUP;
    path[n] = 0;
    if (!realpath(path, resolved)) return -errno;
    for (unsigned i = 0; i < 3; ++i) {
        const char *root = asset_root(i);
        if (!root || !realpath(root, base)) continue;
        size_t len = strlen(base);
        if (!strncmp(resolved, base, len) && resolved[len] == '/' && resolved[len+1] &&
            strlcpy(r->relative, resolved + len + 1, sizeof r->relative) < sizeof r->relative) {
            r->root = i; return 0;
        }
    }
    return -ENOTSUP;
}
// openat each component. A link or a path that leaves the asset root is
// invalid even when a checkpoint or the extracted data pack was edited.
static int open_asset(const checkpoint_file *r) {
    const char *root = asset_root(r->root);
    if (!root || !memchr(r->relative, 0, sizeof r->relative)) return -EINVAL;
    char path[sizeof r->relative]; strlcpy(path, r->relative, sizeof path);
    int dir = open(root, O_DIRECTORY | O_RDONLY | O_CLOEXEC);
    if (dir < 0) return -errno;
    char *p = path;
    for (;;) {
        char *slash = strchr(p, '/');
        if (slash) *slash = 0;
        if (!*p || !strcmp(p, ".") || !strcmp(p, "..")) { close(dir); return -EINVAL; }
        int fd = openat(dir, p, O_NOFOLLOW | O_RDONLY | O_CLOEXEC | (slash ? O_DIRECTORY : O_NONBLOCK));
        int err = errno; close(dir);
        if (fd < 0) return -err;
        if (!slash) return fd;
        p = slash + 1; dir = fd;
    }
}

int dm_checkpoint_files(int section, int restore) {
    typedef struct { char magic[8]; uint32_t count, reserved; } file_header;
    file_header h = {.magic = "NX2AFS1"};
    for (unsigned i = 0; i < 65536; ++i) if (g_fmode[i]) {
        if (restore || g_fmode[i] != MODE_READ || g_fsave[i]) return -ENOTSUP;
        h.count++;
    }
    checkpoint_stream io = {.fd = section, .loading = restore};
    struct stat st;
    if (fstat(section, &st) || !S_ISREG(st.st_mode) || (!restore && st.st_size)) return -EINVAL;
    int rc = checkpoint_io(&io, &h, sizeof h);
    if (rc || memcmp(h.magic, "NX2AFS1", 8) || h.count > 65535 || h.reserved ||
        (restore && st.st_size != (off_t)(sizeof h + h.count * sizeof(checkpoint_file) + 4))) return -EBADMSG;
    checkpoint_file *files = calloc(h.count ? h.count : 1, sizeof *files);
    int *fds = calloc(h.count ? h.count : 1, sizeof *fds);
    if (!files || !fds) { free(files); free(fds); return -ENOMEM; }
    unsigned pending = 0, next = 1;
    for (unsigned i = 0; !rc && i < h.count; ++i) {
        checkpoint_file *r = &files[i];
        if (!restore) {
            while (next < 65536 && !g_fmode[next]) ++next;
            if (next == 65536) { rc = -EBADMSG; break; }
            r->guest_id = next;
            for (unsigned id = 1; id < 65536; ++id) if (atomic_load(&restored_fd[id]) == (int)next + 1) {
                r->guest_id = id; break;
            }
            r->packed = !!ZF(next);
            rc = capture_file_path(next, r);
            if (!rc) rc = asset_fingerprint(next, r, 0);
            ++next;
        }
        if (!rc) rc = checkpoint_io(&io, r, sizeof *r);
        if (rc || !restore) continue;
        if (!r->guest_id || r->guest_id >= 65536 || r->reserved || r->packed > 1) { rc = -EBADMSG; break; }
        for (unsigned j = 0; j < i; ++j) if (files[j].guest_id == r->guest_id) { rc = -EBADMSG; break; }
        if (rc) break;
        int fd = open_asset(r);
        if (fd < 0) { rc = fd; break; }
        fds[pending++] = fd;
        if (!fd || fd >= 65536) { rc = -EMFILE; break; }
        rc = asset_fingerprint(fd, r, 1);
        if (!rc && r->packed && !(ZF(fd) = zf_open(fd))) rc = -EBADMSG;
    }
    uint32_t expected = io.crc, footer = expected;
    if (!rc && (checkpoint_io(&io, &footer, sizeof footer) || footer != expected)) rc = -EBADMSG;
    for (unsigned i = 0; i < pending; ++i) {
        int fd = fds[i];
        if (rc) { if (fd < 65536) zf_close(fd); close(fd); }
        else {
            atomic_store(&restored_fd[files[i].guest_id], fd + 1);
            g_fmode[fd] = MODE_READ;
        }
    }
    if (!rc && restore) {
        for (unsigned i = 0; i < h.count; ++i) {
            int64_t bytes = -1;
            if (h_fs_GetFileSize(&bytes, files[i].guest_id) ||
                bytes != (int64_t)(files[i].packed ? ZF(fds[i])->raw : files[i].bytes)) { rc = -EIO; break; }
        }
    }
    LOG("[snapshot] %s %u game asset handles, result=%d", restore ? "cold reopened" : "captured", h.count, rc);
    free(files); free(fds);
    return rc;
}

// The field trial restores into its own app-private transaction. Archive the
// working tree separately: it may differ from the game's last Commit.
typedef struct {
    char name[32];
    int32_t kind, index;
} checkpoint_mount;
enum { field_rom = 1, field_aoc, field_save };

int dm_checkpoint_mounts(int fd, const char *scene, int restore) {
    struct {
        char magic[8];
        unsigned count, poisoned;
        uint64_t game_ns, frame;
    } h = {.magic = "NX2AMT1"};
    checkpoint_stream io = {.fd = fd, .loading = restore};
    checkpoint_mount entries[MAX_MOUNT] = {0};
    unsigned save_count = 0;
    char committed[1200], working[1200], clone[1200], clone_work[1200];
    if (snprintf(committed, sizeof committed, "%s/committed", scene) >= (int)sizeof committed ||
        snprintf(working, sizeof working, "%s/working", scene) >= (int)sizeof working) return -ENAMETOOLONG;
    if (!restore) {
        h.count = g_nmnt;
        h.poisoned = atomic_load(&g_save_poison);
        h.game_ns = guest_ns();
        h.frame = dm_guest_frame();
        for (unsigned i = 0; i < h.count; ++i) {
            mount_t *m = &g_mnt[i];
            checkpoint_mount *r = &entries[i];
            strlcpy(r->name, m->name, sizeof r->name);
            const char *rom = asset_root(0), *aoc = asset_root(1), *save = getenv("DM_SAVE");
            char work[1100];
            if (rom && m->ro && !strcmp(m->dir, rom)) r->kind = field_rom;
            else if (aoc && m->ro && !strncmp(m->dir, aoc, strlen(aoc)) && m->dir[strlen(aoc)] == '/') {
                char *end;
                const char *suffix = m->dir + strlen(aoc) + 1;
                long index = strtol(suffix, &end, 10);
                if (end == suffix || *end || index < 0 || index > INT32_MAX) return -ENOTSUP;
                r->kind = field_aoc;
                r->index = (int)index;
            } else if (save && !m->ro && !strcmp(m->commit, save) &&
                       snprintf(work, sizeof work, "%s.work", save) < (int)sizeof work && !strcmp(m->dir, work)) {
                if (++save_count > 1) return -ENOTSUP;
                r->kind = field_save;
                if (clonefile(save, committed, CLONE_NOFOLLOW) || clonefile(work, working, CLONE_NOFOLLOW)) return -errno;
            } else return -ENOTSUP;
        }
    }
    int rc = checkpoint_io(&io, &h, sizeof h);
    if (rc || memcmp(h.magic, "NX2AMT1", 8) || !h.count || h.count > MAX_MOUNT || h.poisoned || !h.game_ns)
        return rc ? rc : -ENOTSUP;
    rc = checkpoint_io(&io, entries, h.count * sizeof *entries);
    uint32_t sum = io.crc, footer = sum;
    if (!rc) rc = checkpoint_io(&io, &footer, sizeof footer);
    if (rc || footer != sum) return rc ? rc : -EBADMSG;
    if (!restore) return save_count == 1 ? 0 : -ENOTSUP;
    if (g_nmnt) return -EBUSY;
    if (snprintf(clone, sizeof clone, "%s/trial-save", scene) >= (int)sizeof clone ||
        snprintf(clone_work, sizeof clone_work, "%s.work", clone) >= (int)sizeof clone_work) return -ENAMETOOLONG;
    // Both paths are children of the validated private sample directory.
    if (rm_tree(clone) && errno != ENOENT) return -errno;
    if (rm_tree(clone_work) && errno != ENOENT) return -errno;
    if (clonefile(committed, clone, CLONE_NOFOLLOW) || setenv("DM_SAVE", clone, 1)) return -errno;
    for (unsigned i = 0; i < h.count; ++i) {
        const checkpoint_mount *m = &entries[i];
        if (!memchr(m->name, 0, sizeof m->name) || !m->name[0] || strchr(m->name, ':') || strchr(m->name, '/')) return -EBADMSG;
        Result result;
        if (m->kind == field_rom) result = h_fs_MountRom(m->name, NULL, 0);
        else if (m->kind == field_aoc) result = h_fs_MountAoc(m->name, m->index, NULL, 0);
        else if (m->kind == field_save && !save_count++) {
            result = h_fs_MountSave(m->name, NULL);
            if (result || strcmp(g_mnt[g_nmnt-1].dir, clone_work)) return -EIO;
            if (rm_tree(clone_work) || clonefile(working, clone_work, CLONE_NOFOLLOW)) return -errno;
        } else return -ENOTSUP;
        if (result || g_nmnt != (int)i+1) return -EIO;
    }
    if (save_count != 1) return -EBADMSG;
    int dm_checkpoint_clock_rebase(uint64_t ns, uint64_t frame);
    rc = dm_checkpoint_clock_rebase(h.game_ns, h.frame);
    if (!rc) LOG("[snapshot] rebuilt %u mounts, isolated save transaction, guest clock at frame %llu", h.count,
                 (unsigned long long)h.frame);
    return rc;
}
