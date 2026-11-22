// Run each phase in a fresh process in the verifier's empty shell directory.
#include "checkpoint/save_commit.h"
#include "checkpoint/services.h"
#include <assert.h>
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
    } else return 2;
    return 0;
}
