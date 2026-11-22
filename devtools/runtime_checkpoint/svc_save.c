// Run each phase in a fresh process in the verifier's empty shell directory.
#include "checkpoint/save_commit.h"
#include "checkpoint/services.h"
#include <assert.h>
#include <errno.h>
#include "saves_android.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

unsigned h_fs_MountSave_bs(const char *, const void *);
unsigned h_fs_Commit_bs(const char *);
unsigned h_fs_CommitList_bs(const char *const *, int);
unsigned h_fs_OpenFile_bs(uint64_t *, const char *, int);
unsigned h_fs_WriteFile_bs(uint64_t, int64_t, const void *, size_t, const void *);
void h_fs_CloseFile_bs(uint64_t);

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && !fclose(f));
}
static void expect(const char *path, const char *text) {
    char buf[64] = {0};
    FILE *f = fopen(path, "r");
    assert(f && fread(buf, 1, sizeof buf, f) == strlen(text) && !strcmp(buf, text));
    fclose(f);
}
static void save_a(const char *value) {
    uint64_t h;
    assert(!h_fs_OpenFile_bs(&h, "save:/A", 2));
    assert(!h_fs_WriteFile_bs(h, 0, value, strlen(value), NULL));
    h_fs_CloseFile_bs(h);
}
static void mounts(const char *scene, int restore) {
    char path[80];
    snprintf(path, sizeof path, "%s/mounts", scene);
    int fd = open(path, restore ? O_RDONLY : O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0 && !dm_checkpoint_mounts(fd, scene, restore));
    close(fd);
}

int svc_save_test(const char *root, const char *phase) {
    assert(!chdir(root) && !setenv("DM_SAVE", "live", 1));
    if (!strcmp(phase, "capture")) {
        assert(!mkdir("live", 0700) && !mkdir("old", 0700));
        assert(!mkdir("live/slots", 0700));
        put("live/slots/deleted", "09"); put("live/slots/obsolete", "09");
        put("live/A", "09"); put("live/B", "09"); put("live/index", "old");
        assert(!h_fs_MountSave_bs("save", NULL));
        mounts("old", 0);
        save_a("10");
        assert(!h_fs_Commit_bs("save"));
        expect("live/A", "10");
    } else if (!strcmp(phase, "normal")) {
        assert(!h_fs_MountSave_bs("save", NULL));
        expect("live.work/A", "10");
        // A separate history: A is as at capture, B is newer, C is new and
        // the shared index reflects that. All three must survive restoring A.
        put("live.work/A", "09"); put("live.work/B", "11");
        assert(!unlink("live.work/slots/deleted"));
        put("live.work/C", "new"); put("live.work/index", "C added");
        assert(!h_fs_Commit_bs("save"));
    } else if (!strcmp(phase, "restore")) {
        assert(!dm_checkpoint_save_session("live", "old"));
        mounts("old", 1);
        expect("live/B", "11");
        assert(!h_fs_Commit_bs("save")); // Restoring or an empty commit cannot roll back B.
        save_a("10");
        put("old/trial-save.work/slots/new", "created");
        assert(!unlink("old/trial-save.work/slots/obsolete"));
        expect("live/A", "09");
        const char *names[] = {"save"};
        assert(!h_fs_CommitList_bs(names, 1));
        expect("live/A", "10"); expect("live/B", "11"); expect("live/C", "new");
        expect("live/index", "C added");
        save_a("12"); // An uncommitted write must not mutate a published inode.
        expect("live/A", "10");
        assert(!mkdir("again", 0700));
        mounts("again", 0);
        expect("again/committed/A", "10"); expect("again/working/A", "12");
        put("old/trial-save.work/index", "diverged");
        assert(h_fs_Commit_bs("save")); // Same opaque file changed on both sides.
        expect("live/A", "10"); expect("live/index", "C added");
        put("old/trial-save.work/index", "old");
        // Fail a staging file write, then retry the same transaction.
        struct rlimit original, limit;
        assert(!getrlimit(RLIMIT_FSIZE, &original));
        limit = original; limit.rlim_cur = 1;
        signal(SIGXFSZ, SIG_IGN);
        assert(!setrlimit(RLIMIT_FSIZE, &limit));
        assert(h_fs_Commit_bs("save"));
        assert(!setrlimit(RLIMIT_FSIZE, &original));
        expect("live/A", "10"); expect("live/B", "11"); expect("live/index", "C added");
        assert(!h_fs_Commit_bs("save"));
        expect("live/A", "12");
        save_a("13"); // Simulate process death before Commit.
        _exit(0);
    } else if (!strcmp(phase, "restart")) {
        assert(!h_fs_MountSave_bs("save", NULL));
        expect("live.work/A", "12"); expect("live.work/B", "11");
        assert(access("live.work/slots/deleted", F_OK) && access("live.work/slots/obsolete", F_OK));
        expect("live.work/slots/new", "created");
        expect("live.work/C", "new"); expect("live.work/index", "C added");
    } else if (!strcmp(phase, "overwrite")) {
        assert(!mkdir("external", 0700) && !mkdir("live_device", 0700));
        put("live_device/settings", "device");
        dm_saves_paths("external");
        assert(!dm_checkpoint_save_session("live", "old"));
        mounts("old", 1);
        save_a("15");
        assert(h_fs_Commit_bs("save"));
        uint64_t first = dm_checkpoint_conflict_id();
        assert(first);
        save_a("16");
        assert(!dm_checkpoint_conflict_id());
        assert(dm_fs_resolve_checkpoint_conflict(first, 1) == -ESTALE);
        assert(h_fs_Commit_bs("save"));
        first = dm_checkpoint_conflict_id();
        assert(h_fs_Commit_bs("save")); // Even an identical later save supersedes the prompt.
        uint64_t second = dm_checkpoint_conflict_id();
        assert(first && second && first != second);
        assert(dm_fs_resolve_checkpoint_conflict(first, 1) == -ESTALE);
        assert(!dm_fs_resolve_checkpoint_conflict(second, 0)); // Keep playing.
        assert(dm_fs_resolve_checkpoint_conflict(second, 1) == -ESTALE);
        expect("live/A", "12");
        assert(h_fs_Commit_bs("save"));
        put("live/B", "22"); // A live change after capture must not be overwritten.
        assert(dm_fs_resolve_checkpoint_conflict(dm_checkpoint_conflict_id(), 1) == -ESTALE);
        assert(!rename("external/saves", "external/exports"));
        put("external/saves", "unavailable");
        assert(h_fs_Commit_bs("save"));
        assert(dm_fs_resolve_checkpoint_conflict(dm_checkpoint_conflict_id(), 1));
        expect("live/A", "12"); expect("live/B", "22"); expect("live/C", "new");
        assert(!unlink("external/saves") && !rename("external/exports", "external/saves"));
        dm_save_entry backups[3];
        assert(dm_saves_list(backups, 3) == 0);
        put("old/trial-save.work/D", "new slot");
        put("old/trial-save.work/index", "new index");
        assert(h_fs_Commit_bs("save"));
        first = dm_checkpoint_conflict_id();
        assert(!dm_fs_resolve_checkpoint_conflict(first, 1));
        assert(dm_fs_resolve_checkpoint_conflict(first, 1) == -ESTALE);
        assert(dm_saves_list(backups, 3) == 1 && backups[0].device);
        assert(strstr(backups[0].name, "-before-overwrite"));
        char exported[160];
        snprintf(exported, sizeof exported, "external/saves/%s", backups[0].name);
        assert(!chdir(exported));
        expect("A", "12"); expect("B", "22"); expect("C", "new"); expect("index", "C added");
        expect("slots/new", "created"); assert(access("D", F_OK));
        assert(!chdir(root));
        snprintf(exported, sizeof exported, "external/saves/%s.device/settings", backups[0].name);
        expect(exported, "device");
        save_a("17"); assert(!h_fs_Commit_bs("save")); // Against the new private baseline.
    } else if (!strcmp(phase, "overwrite-restart")) {
        assert(!h_fs_MountSave_bs("save", NULL));
        expect("live.work/A", "17"); expect("live.work/B", "22"); expect("live.work/C", "new");
        expect("live.work/D", "new slot"); expect("live.work/index", "new index");
        expect("live.work/slots/new", "created"); expect("live_device/settings", "device");
    } else return 2;
    return 0;
}
