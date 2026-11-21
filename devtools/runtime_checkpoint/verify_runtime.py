"""Link and exercise the runtime sections using build.py --lib --release
--adrenotools --manual-checkpoint output and an installed game's mods folder.
Only the private shell scratch directory created by this run is written on the
device. The app and its saves are never opened for writing.
"""
import argparse
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import tempfile
import struct
import zlib

HERE = Path(__file__).resolve().parent
DM = HERE.parents[1] / 'nxnative/dm'


def link(args, exe):
    gen = args.build / 'gen' / args.title
    includes = [gen, DM / 'android', DM / 'android/compat/include', DM / 'host', DM / 'gfx/nvn',
                DM / 'gfx/nvn/include', DM / 'gfx/tex', DM / 'third_party/vulkan-headers/include']
    src = [HERE / name for name in ('svc_probe.c', 'sdk_worker.c', 'sdk_worker.S', 'svc_gpu.c', 'svc_wait.c', 'svc_image.c', 'svc_scene.c')]
    objects = sorted((args.build / 'obj').glob('*.o'))
    if not objects or not (gen / 'game_profile.h').is_file():
        raise ValueError('Expected an Android host build directory for this title')
    subprocess.run([args.cc, '-O2', '-ffixed-x18', '-DDM_MANUAL_CHECKPOINT', '-Wall', '-Werror', '-Wno-format',
                    *['-I' + str(p) for p in includes], *src, *objects,
                    '-Wl,--wrap=main', '-Wl,-z,max-page-size=16384', '-Wl,-z,nostart-stop-gc',
                    '-landroid', '-llog', '-lvulkan', '-laaudio', '-lnativewindow', '-lmediandk',
                    '-lz', '-lm', '-ldl', '-o', exe], check=True)


def verify(args, exe):
    adb = [args.adb] + (['-s', args.serial] if args.serial else [])

    def shell(*words, check=True):
        return subprocess.run([*adb, 'shell', shlex.join(words)], check=check,
                              capture_output=True, text=True, timeout=90)

    scratch = shell('mktemp', '-d', '/data/local/tmp/nx2a-svc.XXXXXX').stdout.strip()
    if not re.fullmatch(r'/data/local/tmp/nx2a-svc\.[a-zA-Z0-9]{6}', scratch):
        raise RuntimeError('Unexpected scratch directory from Android mktemp')

    def path(name):
        return scratch + '/' + name

    def probe(mode, memory='memory', modules='modules', pools='gpu', events='events', images='images', bindings='bindings', result=0, message=None):
        res = shell('env', 'LD_LIBRARY_PATH=' + scratch, 'DM_ROOT=' + scratch,
                    'DM_VK_DRIVER=libvulkan_freedreno.so', path('probe'), mode, args.mods,
                    path(memory), path(modules), path(pools), path(events), path(images), path(bindings), check=False)
        if res.returncode != result or (message and message not in res.stdout + res.stderr):
            raise RuntimeError(f'{mode} returned {res.returncode}\n{res.stdout}\n{res.stderr[-2500:]}')
        print(res.stdout.strip() or message, flush=True)

    try:
        hooks = ['libadrenotools.so', 'libhook_impl.so', 'libmain_hook.so',
                 'libgsl_alloc_hook.so', 'libfile_redirect_hook.so']
        files = [(exe, 'probe'), (args.driver, 'libvulkan_freedreno.so')]
        files.extend((args.build / name, name) for name in hooks)
        for src, name in files:
            subprocess.run([*adb, 'push', str(src), path(name)], check=True, capture_output=True)
        shell('chmod', '700', path('probe'))
        print(shell(path('probe'), 'pause-test').stdout.strip(), flush=True)
        if args.scene_core:
            data = PurePosixPath(args.mods).parent
            res = shell('env', 'LD_LIBRARY_PATH=' + scratch, 'DM_ROOT=' + scratch,
                        f'DM_ROMFS={data / "RomFS"}', f'DM_AOC_DIR={data / "aoc"}',
                        'DM_VK_DRIVER=libvulkan_freedreno.so', path('probe'), 'replay-core',
                        args.mods, args.scene_core, check=False)
            if res.returncode: raise RuntimeError(f'Cold scene probe ({res.returncode}): {res.stderr[-2500:]}')
            print(res.stdout.strip(), flush=True)
        if getattr(args, 'scene_image', None):
            section = bytearray(args.scene_image.read_bytes())
            if (len(section) < 124 or section[:8] != b'NX2AIM1\0' or
                    struct.unpack_from('<I', section, len(section)-4)[0] != zlib.crc32(memoryview(section)[:-4]) or
                    struct.unpack_from('<Q', section, 72)[0] + 124 != len(section)):
                raise ValueError('Invalid scene image length or CRC')
            # Only the source pool and source CPU address are process-specific.
            struct.pack_into('<QQ', section, 56, 0, 0)
            struct.pack_into('<I', section, len(section)-4, zlib.crc32(memoryview(section)[:-4]))
            with tempfile.TemporaryDirectory() as temp:
                local = Path(temp) / 'scene-image'
                local.write_bytes(section)
                subprocess.run([*adb, 'push', str(local), path('scene-image')], check=True, capture_output=True)
                result = shell('env', 'LD_LIBRARY_PATH=' + scratch, 'DM_ROOT=' + scratch,
                               'DM_VK_DRIVER=libvulkan_freedreno.so', path('probe'), 'replay-image',
                               path('scene-image'), path('readback'), check=False)
                if result.returncode: raise RuntimeError(result.stderr[-1000:])
                subprocess.run([*adb, 'pull', path('readback'), str(local)], check=True, capture_output=True)
                if local.read_bytes() != section: raise RuntimeError('Scene pixels or bindings changed')
                print(result.stdout.strip(), 'All pixel and binding bytes matched.', flush=True)
        probe('save', result=137, message='GPU fill verified')
        probe('restore', message='SHA-256 matched')
        probe('collide', message='sentinel intact')
        shell('cp', path('memory'), path('short'))
        shell('truncate', '-s', '4096', path('short'))
        probe('restore', memory='short', result=1, message='published=0')
        # A truncated module tests validation before
        # any changes to the freshly relocated SDK have been applied.
        for section in ['memory', 'modules', 'gpu']:
            bad = path('bad-' + section)
            shell('cp', path(section), bad)
            size = int(shell('stat', '-c', '%s', bad).stdout)
            shell('truncate', '-s', str(size - 1), bad)
            if section == 'modules':
                probe('restore', modules='bad-modules', result=1, message='sdk_untouched=1')
            elif section == 'gpu':
                probe('restore', pools='bad-gpu', result=1, message='gpu_unpublished=1')
            else:
                probe('restore', memory='bad-memory', result=1, message='published=0')
        for section, reason in [('events', 'event table restore refused'), ('images', 'image section refused'), ('bindings', 'descriptor table restore refused')]:
            bad = path('bad-' + section)
            shell('cp', path(section), bad)
            size = int(shell('stat', '-c', '%s', bad).stdout)
            # Change only the CRC, leaving the section readable at its native size.
            byte = int(shell('od', '-An', '-tu1', '-j', str(size-1), '-N1', bad).stdout)
            value = '\\%03o' % (byte ^ 1)
            cmd = f'printf {shlex.quote(value)} | dd of={shlex.quote(bad)} bs=1 seek={size-1} conv=notrunc status=none'
            subprocess.run([*adb, 'shell', cmd], check=True, capture_output=True, timeout=30)
            probe('restore', **{section: 'bad-' + section}, result=1, message=reason)
        # Flip a bit in a data page. Only the checksum reveals this error.
        bad = path('bad-crc')
        shell('cp', path('memory'), bad)
        pos = int(shell('stat', '-c', '%s', bad).stdout) - 1024
        original = int(shell('od', '-An', '-tu1', '-j', str(pos), '-N1', bad).stdout)
        byte = '\\%03o' % (original ^ 1)
        cmd = f'printf {shlex.quote(byte)} | dd of={shlex.quote(bad)} bs=1 seek={pos} conv=notrunc status=none'
        subprocess.run([*adb, 'shell', cmd], check=True, capture_output=True, timeout=30)
        probe('restore', memory='bad-crc', result=1, message='published=0')
    finally:
        shell('rm', '-r', scratch)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True, help='NDK aarch64-linux-android31-clang executable')
    parser.add_argument('--build', type=Path, required=True, help='dedicated output of build.py for this title')
    parser.add_argument('--title', required=True)
    parser.add_argument('--mods', required=True, help='on-device path to the title’s rewritten modules')
    parser.add_argument('--driver', required=True, type=Path, help='Turnip shared library')
    parser.add_argument('--scene-core', help='device directory from a completed manual game survey')
    parser.add_argument('--scene-image', type=Path, help='optional bound image captured by the gameplay survey')
    parser.add_argument('--adb', default='adb')
    parser.add_argument('--serial')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / 'probe'
        link(args, binary)
        verify(args, binary)
