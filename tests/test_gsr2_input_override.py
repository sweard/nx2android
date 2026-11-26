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
