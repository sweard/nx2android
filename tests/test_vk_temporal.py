import pathlib
import shutil
import subprocess
import tempfile
import unittest


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class TemporalInputTest(unittest.TestCase):
    def test_title_adapter_and_replay_bindings(self):
        android = pathlib.Path(__file__).resolve().parents[1] / 'nxnative/dm/android'
        with tempfile.TemporaryDirectory() as tmp:
            work = pathlib.Path(tmp)
            # Compile the replay code against small native type doubles without an Android SDK.
            shutil.copy(android / 'gfx_vk_temporal.h', work)
            shutil.copy(android / 'gfx_vk_gsr2_profile.h', work)
            (work / 'gfx_vk.h').write_text("""
#include <stdint.h>
typedef struct { uint64_t serial; struct { unsigned width, height; } d; } tex_t;
typedef struct { unsigned layer; } view_t;
""")
            source = pathlib.Path(__file__).with_name('vk_temporal_harness.c')
            exe = work / 'test'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I', str(work),
                            '-I', str(android), str(source), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, capture_output=True)
