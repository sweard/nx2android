// Publish the difference between two private guest transactions onto the current real save.
// Treat file contents as opaque. If both histories changed one file differently, fail the
// whole commit: the file may be a shared index or contain several save slots.
#include "save_commit.h"
#include "saves_android.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/clonefile.h>
#include <sys/stat.h>
#include <unistd.h>

static char live_save[1024], staging[1100], baseline[2][1100];
static unsigned active;
static char observed[1100], work_path[1100];
static uint64_t generation;
static _Atomic uint64_t pending;

int dm_checkpoint_save_session(const char *live, const char *scene) {
    if (!live || !*live || !scene) return -EINVAL;
    if (snprintf(live_save, sizeof live_save, "%s", live) >= (int)sizeof live_save ||
        snprintf(staging, sizeof staging, "%s.new", live) >= (int)sizeof staging ||
        snprintf(observed, sizeof observed, "%s/conflict-original", scene) >= (int)sizeof observed ||
        snprintf(work_path, sizeof work_path, "%s/trial-save.work", scene) >= (int)sizeof work_path ||
        snprintf(baseline[0], sizeof baseline[0], "%s/trial-save", scene) >= (int)sizeof baseline[0] ||
        snprintf(baseline[1], sizeof baseline[1], "%s/trial-save.next", scene) >= (int)sizeof baseline[1]) return -ENAMETOOLONG;
    active = 0;
    return 0;
}

int dm_checkpoint_save_mount(const char *commit) {
    return commit && *baseline[0] && !strcmp(baseline[0], commit);
}
const char *dm_checkpoint_save_baseline(const char *commit) {
    return dm_checkpoint_save_mount(commit) ? baseline[active] : commit;
}

typedef struct {
    char *path;
    const char *relative;
    mode_t mode;
    off_t size;
} save_entry;

typedef struct {
    save_entry *entries;
    size_t count, capacity, cursor;
} save_tree;

static int index_tree(save_tree *tree, const char *path, size_t prefix) {
    DIR *dir = opendir(path);
    if (!dir) return -errno;
    int rc = 0;
    while (!rc) {
        errno = 0;
        struct dirent *e = readdir(dir);
        if (!e) { rc = -errno; break; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char *child;
        if (asprintf(&child, "%s/%s", path, e->d_name) < 0) { rc = -ENOMEM; break; }
        struct stat st;
        if (lstat(child, &st)) rc = -errno;
        else if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) rc = -ENOTSUP;
        if (!rc && tree->count == tree->capacity) {
            size_t capacity = tree->capacity ? tree->capacity * 2 : 32;
            void *p = realloc(tree->entries, capacity * sizeof(save_entry));
            if (!p) rc = -ENOMEM;
            else { tree->entries = p; tree->capacity = capacity; }
        }
        if (rc) { free(child); break; }
        tree->entries[tree->count++] = (save_entry){child, child + prefix, st.st_mode, st.st_size};
        if (S_ISDIR(st.st_mode)) rc = index_tree(tree, child, prefix);
    }
    closedir(dir);
    return rc;
}

static int by_path(const void *a, const void *b) {
    return strcmp(((const save_entry *)a)->relative, ((const save_entry *)b)->relative);
}

static int read_tree(save_tree *tree, const char *root) {
    int rc = index_tree(tree, root, strlen(root) + 1);
    if (!rc && tree->count) qsort(tree->entries, tree->count, sizeof(save_entry), by_path);
    return rc;
}

static void free_tree(save_tree *tree) {
    for (size_t i = 0; i < tree->count; i++) free(tree->entries[i].path);
    free(tree->entries);
}

// 1 = equal, 0 = different, negative errno = cannot safely compare.
static int equal_entry(const save_entry *a, const save_entry *b) {
    if (!a || !b) return a == b;
    if (S_ISDIR(a->mode) || S_ISDIR(b->mode)) return !!S_ISDIR(a->mode) == !!S_ISDIR(b->mode);
    if (a->size != b->size) return 0;
    FILE *left = fopen(a->path, "rb");
    if (!left) return -errno;
    FILE *right = fopen(b->path, "rb");
    int result = right ? 1 : -errno;
    while (result == 1) {
        char x[4096], y[sizeof x];
        size_t n = fread(x, 1, sizeof x, left), m = fread(y, 1, sizeof y, right);
        if (ferror(left) || ferror(right)) result = -EIO;
        else if (n != m || memcmp(x, y, n)) result = 0;
        else if (n < sizeof x) break;
    }
    if (right) fclose(right);
    fclose(left);
    return result;
}

static int verify_copy(const char *original, const char *copy) {
    save_tree a = {0}, b = {0};
    int rc = read_tree(&a, original);
    if (!rc) rc = read_tree(&b, copy);
    if (!rc && a.count != b.count) rc = -ESTALE;
    for (size_t i = 0; !rc && i < a.count; i++) {
        int equal = !strcmp(a.entries[i].relative, b.entries[i].relative)
            ? equal_entry(&a.entries[i], &b.entries[i]) : 0;
        if (equal != 1) rc = equal ? equal : -ESTALE;
    }
    free_tree(&a);
    free_tree(&b);
    return rc;
}

// Android storage policies may prohibit hard links even within one filesystem.
static int write_entry(const char *dir, const save_entry *e) {
    if (!e) return 0;
    char *dest;
    if (asprintf(&dest, "%s/%s", dir, e->relative) < 0) return -ENOMEM;
    int rc;
    if (S_ISDIR(e->mode)) rc = mkdir(dest, e->mode & 0777);
    else rc = clonefile(e->path, dest, CLONE_NOFOLLOW);
    rc = rc ? -(errno ? errno : EIO) : 0;
    free(dest);
    return rc;
}

static int stage_merge(save_tree trees[3], const char *next, int *changed, int *conflict) {
    for (;;) {
        const char *name = NULL;
        const save_entry *entry[3] = {0};
        for (int i = 0; i < 3; i++) {
            save_tree *t = &trees[i];
            if (t->cursor == t->count) continue;
            const char *candidate = t->entries[t->cursor].relative;
            if (!name || strcmp(candidate, name) < 0) name = candidate;
        }
        if (!name) return 0;
        for (int i = 0; i < 3; i++) {
            save_tree *t = &trees[i];
            if (t->cursor < t->count && !strcmp(t->entries[t->cursor].relative, name))
                entry[i] = &t->entries[t->cursor++];
        }
        const save_entry *base = entry[0], *work = entry[1], *live = entry[2];
        int same = equal_entry(base, work);
        if (same < 0) return same;
        const save_entry *chosen = live;
        if (!same) {
            same = equal_entry(work, live);
            if (same < 0) return same;
            if (!same) {
                int safe = equal_entry(base, live);
                if (safe < 0) return safe;
                if (!safe) *conflict = 1;
                *changed = 1;
                chosen = work;
            }
        }
        int rc = write_entry(next, work);
        if (!rc) rc = write_entry(staging, chosen);
        // A surviving child under a deleted or replaced parent is a structural conflict.
        if (rc) return rc == -ENOENT || rc == -ENOTDIR ? -ESTALE : rc;
    }
}

static void clear_candidate(void) {
    dm_saves_checkpoint_remove(baseline[1 - active]);
    dm_saves_checkpoint_remove(staging);
    dm_saves_checkpoint_remove(observed);
}

uint64_t dm_checkpoint_conflict_id(void) { return atomic_load(&pending); }

int dm_checkpoint_discard_conflict(void) {
    if (!atomic_exchange(&pending, 0)) return 0;
    clear_candidate();
    return 1;
}

int dm_checkpoint_resolve_conflict(uint64_t ticket, int overwrite) {
    if (!ticket || ticket != dm_checkpoint_conflict_id()) return -ESTALE;
    atomic_store(&pending, 0);
    int rc = 0;
    if (overwrite) {
        rc = verify_copy(observed, live_save);
        if (!rc) rc = verify_copy(baseline[1 - active], work_path);
        if (!rc) {
            rc = dm_saves_backup_before_overwrite(verify_copy);
            if (rc == -ESTALE) rc = -EIO; // The backup failed verification.
        }
        if (!rc) rc = verify_copy(observed, live_save);
        if (!rc && renamex_np(staging, live_save, RENAME_SWAP)) rc = -errno;
        if (!rc) active = 1 - active;
    }
    clear_candidate();
    return rc;
}

int dm_checkpoint_save_commit(const char *work) {
    const char *next = baseline[1 - active];
    const char *roots[] = {baseline[active], work, live_save};
    save_tree trees[3] = {0};
    int rc = 0, changed = 0, conflict = 0;
    for (int i = 0; !rc && i < 3; i++) rc = read_tree(&trees[i], roots[i]);
    clear_candidate();
    if (!rc && (mkdir(next, 0700) || mkdir(staging, 0700))) rc = -errno;
    if (!rc) rc = stage_merge(trees, next, &changed, &conflict);
    for (int i = 0; i < 3; i++) free_tree(&trees[i]);
    if (!rc && conflict) {
        // Keep both outputs immutable while the game continues using its work tree.
        if (!clonefile(live_save, observed, CLONE_NOFOLLOW)) {
            atomic_store(&pending, ++generation);
            return -ESTALE;
        }
        rc = -errno;
    }
    if (!rc && changed && renamex_np(staging, live_save, RENAME_SWAP)) rc = -errno;
    if (!rc) active = 1 - active;
    clear_candidate();
    return rc;
}
