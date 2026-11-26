"""A guest can change either console-variable copy while Custom is active."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

import csrc


class InputOverride(unittest.TestCase):
    def test_restore_last_guest_values(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("requires a C compiler")
        source = (csrc.REPO / "nxnative/dm/android/gsr2_ue_resolution.inc").read_text(encoding="utf-8")
        state = csrc.between(source, "typedef struct {\n    uintptr_t data;", "static ue_override")
        test = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
static int readable = 1, writable = 1;
static int read_field(uintptr_t a, void *p, size_t n) {
    if (readable) memcpy(p, (void *)a, n);
    return readable;
}
static int write_field(uintptr_t a, void *p, size_t n) {
    if (writable) memcpy((void *)a, p, n);
    return writable;
}
""" + state + csrc.function(source, "static int ue_set_cvar(") + r"""
int main(void) {
    ue_override state = {0};
    uint32_t original[] = {80, 79}, replacement[] = {60, 59};
    uintptr_t a = (uintptr_t)original, b = (uintptr_t)replacement;
    assert(ue_set_cvar(&state, a, 50, 0));
    assert(original[0] == 50 && original[1] == 50);
    original[1] = 78; // the game updates the render thread's copy
    assert(ue_set_cvar(&state, a, 75, 0));
    assert(!ue_set_cvar(&state, 0, 0, 1) && state.data == a);
    writable = 0;
    assert(!ue_set_cvar(&state, a, 0, 1) && state.data == a);
    writable = 1;
    assert(ue_set_cvar(&state, a, 0, 1));
    assert(original[0] == 80 && original[1] == 78);
    assert(ue_set_cvar(&state, a, 50, 0));
    assert(ue_set_cvar(&state, b, 55, 0));
    readable = 0;
    assert(!ue_set_cvar(&state, b, 0, 1));
    readable = 1;
    assert(ue_set_cvar(&state, b, 0, 1));
    assert(replacement[0] == 60 && replacement[1] == 59);
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "override"
            subprocess.run([cc, "-std=c11", "-x", "c", "-", "-o", exe], input=test, text=True, check=True)
            subprocess.run([exe], check=True)

    def test_ktgl_docked_scene_table(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("requires a C compiler")
        android = csrc.REPO / "nxnative/dm/android"
        test = r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "gsr2_profile.h"
#define LOG(...) ((void)0)
typedef struct { const char *name, *build_id; unsigned char *base; size_t data_off, size; } module_t;
static module_t g_mod[1];
static int g_nmod = 1, writable = 1;
static int read_field(uintptr_t a, void *p, size_t n) { memcpy(p, (void *)a, n); return 1; }
static int write_field(uintptr_t a, void *p, size_t n) {
    if (writable) memcpy((void *)a, p, n);
    return writable;
}
static int preference = 50;
static int dm_opmode(void) { return 1; }
static int dm_gsr2_custom_percent(void) { return preference; }
#include "gsr2_fe3h_resolution.inc"
int main(void) {
    size_t size = KT_CURRENT + sizeof(uint32_t);
    unsigned char *image = calloc(1, size);
    assert(image);
    g_mod[0] = (module_t){"main", "other build", image, 0x19e4000, size};
    kt_scene_extent *rows = (void *)(image + KT_EXTENTS);
    uint32_t *modes = (void *)(image + KT_MODE), *steps = (void *)(image + KT_DRS);
    uint32_t *current = (void *)(image + KT_CURRENT);
    const kt_scene_extent stock[] = {{1920,1080},{1600,900},{1440,810}};
    const uint32_t native_modes[] = {0,5,5,7,8,9};
    memcpy(rows, stock, sizeof stock);
    memcpy(modes, native_modes, sizeof native_modes);
    memcpy(steps, kt_native_steps, sizeof kt_native_steps);
    dm_gsr2_init_ktgl_resolution();
    assert(!kt_base && !memcmp(rows, stock, sizeof stock));
    g_mod[0].build_id = GSR2_FE3H_BUILD;
    for (preference = 50; preference <= 100; preference += 5) {
        kt_base = 0;
        kt_active = kt_last_percent = -1;
        memcpy(rows, stock, sizeof stock);
        dm_gsr2_init_ktgl_resolution();
        uint32_t bank = preference == 100 ? 0 : preference == 75 ? 2 : 1;
        assert(*modes == bank && *current == bank);
        for (int i = 0; i < 6; ++i) assert(steps[i] == bank);
        assert(rows[bank].width == 1920u * preference / 100 && rows[bank].height == 1080u * preference / 100);
        assert(!memcmp(rows, stock, sizeof *rows) && !memcmp(rows+2, stock+2, sizeof *rows));
        set_resolution(75);
        assert(*modes == 2 && *current == 2);
        set_resolution(100);
        assert(!*modes && !*current);
        writable = 0;
        assert(!dm_gsr2_release_resolution());
        writable = 1;
        assert(dm_gsr2_release_resolution());
        assert(!memcmp(steps, kt_native_steps, sizeof kt_native_steps) && !memcmp(modes, native_modes, sizeof native_modes));
    }
    free(image);
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "ktgl"
            subprocess.run([cc, "-std=c11", "-I", android, "-x", "c", "-", "-o", exe],
                           input=test, text=True, check=True)
            subprocess.run([exe], check=True)
