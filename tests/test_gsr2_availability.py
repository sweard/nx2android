"""GSR2 eligibility at pack time and across a cold start with a different driver or data pack."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

import csrc
from porter.upstream import gsr2_package as package


class PackageAvailability(unittest.TestCase):
    def test_build_and_required_stages(self):
        title = '0100A3501946E000'
        build = '0d9649011312f62e93f6a5673a09d7e9'
        keys = package.required(title, build)
        self.assertEqual(len(keys), 3)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'shaders.dmsv'
            def pack(stages):
                entries = [(key, stage, bytes(20), bytes(20)) for key, stage in stages.items()]
                package.dmsv.write(path, entries, level=0)
            pack(keys)
            self.assertEqual(package.verify(title, build, path), build)
            self.assertFalse(package.verify(title, '0' * 32, path))
            self.assertFalse(package.verify('0' * 16, build, path))
            key, stage = keys.popitem()
            pack(keys)
            self.assertFalse(package.verify(title, build, path))
            pack({**keys, key: (stage + 1) % 6})
            self.assertFalse(package.verify(title, build, path))
            path.write_bytes(path.read_bytes()[:-1])
            self.assertFalse(package.verify(title, build, path))


@unittest.skipUnless(shutil.which('cc'), 'requires a host C compiler')
class RuntimeAvailability(unittest.TestCase):
    def test_gate_and_saved_preference(self):
        replay = csrc.read('nxnative/dm/android/gfx_vk_exec.c')
        front = csrc.read('nxnative/dm/android/front_android.c')
        code = r"""
#include <assert.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#define DM_HAS_GSR2_ADAPTER 1
#define DM_GAME_PROGRAM_ID 1
#define DM_GSR2_PACKAGED_BUILD "approved"
#define VKLOG(...) ((void)0)
enum {GSR2_OFF, GSR2_AUTO, GSR2_CUSTOM};
typedef struct { int multisample_hud; } gsr2_profile;
static const gsr2_profile digimon_profile = {1};
static const gsr2_profile *gs_profile;
static const char *build = "";
static int bundle_ok, driver_ok, probes;
static struct {int dev;} V;
static const char *dm_main_build(void) { return build; }
static const gsr2_profile *gsr2_profile_for_build(int title, const char *b) { return &digimon_profile; }
static int vk_gsr2_shader_stage(int k) { return 1; }
static int gsr2_bundle_matches(int t, int (*stage)(int)) { return bundle_ok; }
static int vk_gsr2_prepare(int ms) { assert(ms); probes++; return driver_ok; }
"""
        code += csrc.between(replay, 'static _Atomic int gsr2_available;', '#include "gfx_vk_exec_state.inc"')
        code += csrc.between(front, 'static _Atomic int g_gsr2_input =', 'static void menu_gsr2_set(')
        code += r"""
int main(int argc, char **argv) {
    int scenario = atoi(argv[1]);
    g_gsr2_input = GSR2_CUSTOM; g_gsr2_percent = 50; g_gsr2_output = 1440;
    assert(!gsr2_current_profile() && !vk_gsr2_available() && !dm_gsr2_custom_percent());
    V.dev = 1;
    assert(!gsr2_current_profile()); // Wait for main instead of caching an early negative.
    build = scenario == 1 ? "different" : "approved";
    bundle_ok = scenario != 2; driver_ok = scenario != 3;
    gs_profile = gsr2_current_profile();
    assert(gsr2_current_profile() == gs_profile && probes == (scenario == 0 || scenario == 3));
    assert(vk_gsr2_available() == (scenario == 0));
    assert(dm_gsr2_custom_percent() == (scenario == 0 ? 50 : 0));
    assert(gsr2_preference() == GSR2_CUSTOM && g_gsr2_percent == 50 && dm_gsr2_output_height() == 1440);
    g_gsr2_input = GSR2_OFF;
    assert(dm_gsr2_input_mode() == GSR2_OFF && vk_gsr2_available() == (scenario == 0));
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / 'gate'
            subprocess.run(['cc', '-std=c11', '-x', 'c', '-', '-o', str(exe)], input=code, text=True, check=True)
            for scenario in range(4):
                subprocess.run([str(exe), str(scenario)], check=True)
