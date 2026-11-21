"""Check manual checkpoint publication and recovery across process recreation."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ANDROID = Path(__file__).resolve().parents[1] / 'nxnative/dm/android'

HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "checkpoint_android.h"
#include "saves_android.h"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static unsigned arrived, released, attempts;
static int fail_attempt;
uint64_t dm_fs_commit_count(void) { return 0; }
void dm_overlay_redraw(void) {}
int capture_impl(const char *stage, const char *files);

int dm_saves_checkpoint_capture(const char *stage, const char *files) {
    int result = capture_impl(stage, files);
    pthread_mutex_lock(&mu);
    unsigned attempt = ++attempts;
    arrived = attempt;
    pthread_cond_broadcast(&cv);
    while (released < attempt) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    return attempt == (unsigned)fail_attempt ? -EIO : result;
}

static void wait_attempt(unsigned n) {
    pthread_mutex_lock(&mu);
    while (arrived < n) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

static void release(unsigned n) {
    pthread_mutex_lock(&mu);
    released = n;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static int exists(const char *root, const char *name) {
    char path[2048]; snprintf(path, sizeof path, "%s/background-resume/%s", root, name);
    return !access(path, F_OK);
}

static void wait_state(int wanted) {
    for (int tries = 0; tries < 1000; tries++) {
        if (dm_checkpoint_state() == wanted) return;
        usleep(1000);
    }
    assert(0 && "checkpoint files did not reach the expected state");
}

static void set_docked(const char *root, int docked) {
    char path[2048]; snprintf(path, sizeof path, "%s/settings.txt", root);
    FILE *out = fopen(path, "w"); assert(out);
    assert(fprintf(out, "docked=%d\n", docked) > 0 && !fclose(out));
}

int main(int argc, char **argv) {
    assert(argc == 5);
    const char *base = argv[1], *files = argv[2], *save = argv[3], *mode = argv[4];
    assert(!setenv("DM_SAVE", save, 1));
    dm_saves_paths(files);
    int ready = dm_checkpoint_init(base, files);
    assert(dm_checkpoint_state() == DM_CHECKPOINT_IDLE);
    if (!strcmp(mode, "available")) {
        assert(ready);
        FILE *settings = fopen(dm_checkpoint_settings(), "r"); assert(settings);
        char text[32]; assert(fgets(text, sizeof text, settings) && !strcmp(text, "docked=0\n"));
        fclose(settings);
        return 0;
    }
    if (!strcmp(mode, "unavailable")) { assert(!ready); return 0; }

    if (!strcmp(mode, "consume")) { assert(ready); dm_checkpoint_consume(); return 0; }

    set_docked(files, 0);
    dm_checkpoint_request();
    wait_attempt(1);
    assert(dm_checkpoint_state() == DM_CHECKPOINT_SAVING && !exists(base, "ready"));
    dm_checkpoint_request();   // a tap while saving does not queue another save
    release(1);
    wait_state(DM_CHECKPOINT_SAVED);
    assert(exists(base, "ready"));

    if (!strcmp(mode, "saved")) return 0;
    set_docked(files, 1);
    fail_attempt = 2;
    dm_checkpoint_request();
    wait_attempt(2);
    assert(!exists(base, "ready"));
    if (!strcmp(mode, "killed-latest")) _exit(0);
    release(2);
    wait_state(DM_CHECKPOINT_FAILED);
    assert(!exists(base, "ready") && !exists(base, ".snapshot-part"));
    return 0;
}
'''


class ManualCheckpoint(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc')
        if not cc or sys.platform not in ('darwin', 'linux'):
            raise unittest.SkipTest('a POSIX C compiler is needed')
        temp = tempfile.TemporaryDirectory(prefix='nx2a_manual_checkpoint_test_')
        cls.addClassCleanup(temp.cleanup)
        root = Path(temp.name)
        cls.executable = root / 'checkpoint'
        (root / 'dm.h').write_text('#include <stdio.h>\n#define LOG(...) ((void)0)\n')
        (root / 'saves.c').write_text('#define dm_saves_checkpoint_capture capture_impl\n#include "saves_android.c"\n')
        harness = root / 'test.c'
        harness.write_text(HARNESS)
        flags = ['-Dst_ctim=st_ctimespec', '-DSYS_renameat2=-1', '-Wno-deprecated-declarations'] if sys.platform == 'darwin' else []
        result = subprocess.run([cc, '-D_GNU_SOURCE', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                 '-Wno-unused-but-set-variable',
                                 '-pthread', *flags,
                                 f'-I{root}', f'-I{ANDROID}', str(harness), str(root / 'saves.c'),
                                 str(ANDROID / 'checkpoint_android.c'), '-o', str(cls.executable)],
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError(result.stderr)

    def run_case(self, mode, check, change=None):
        with tempfile.TemporaryDirectory(prefix='nx2a_checkpoint_fixture_') as name:
            root = Path(name)
            internal, files = root / 'private', root / 'files'
            save = internal / 'save'
            save.mkdir(parents=True)
            files.mkdir()
            (save / 'slot.sav').write_bytes(b'committed game data')
            (files / 'nx2a-data.json').write_text('{"version":"fixture"}\n')
            args = [self.executable, internal, files, save]
            subprocess.run([*args, mode], check=True, timeout=8)
            if change:
                change(files, save)
            for verification in check.split():
                subprocess.run([*args, verification], check=True, timeout=8)
            self.assertEqual((save / 'slot.sav').read_bytes(), b'committed game data')

    def test_manual_publication_is_consumed_after_a_cold_start_choice(self):
        self.run_case('saved', 'available consume unavailable')

    def test_failure_disqualifies_older_checkpoint(self):
        self.run_case('failed-latest', 'unavailable')

    def test_interrupted_latest_checkpoint_disqualifies_older_published_copy(self):
        self.run_case('killed-latest', 'unavailable')

    def test_changed_data_pack_disqualifies_checkpoint(self):
        self.run_case('saved', 'unavailable',
                      lambda files, save: (files / 'nx2a-data.json').write_text('{"version":"changed"}\n'))
