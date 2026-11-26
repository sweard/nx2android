"""Verify a port against the same build IDs and shader keys used by Vulkan replay."""
from pathlib import Path
import re

import dmsv

GAMES = Path(__file__).resolve().parents[2] / 'games'
STAGES = {'VS': 0, 'FS': 1, 'CS': 5}


def required(title, build):
    builds = (GAMES / 'gsr2_builds.h').read_text(encoding='utf-8')
    for name, game in re.findall(r'#define GSR2_(\w+)_ID 0x([0-9a-f]+)ull', builds):
        if game == title.lower() and f'#define GSR2_{name}_BUILD "{build}"' in builds.splitlines():
            rows = (GAMES / 'gsr2_shaders.inc').read_text(encoding='utf-8').splitlines()
            keys = {}
            for row in rows:
                if row.startswith(f'GSR2_SHADER({name},'):
                    _, _, stage, key = row.removeprefix('GSR2_SHADER(').removesuffix(')').split(', ')
                    keys[int(key, 16)] = STAGES[stage]
            return keys
    return {}


def main_build(fixups):
    match = re.search(r'^# BUILD_ID\s+([0-9a-f]{32})\s*$', fixups, re.MULTILINE)
    return match[1] if match else ''


def verified_index(title, build, size, read_at):
    """Read only the header and sorted index, including for a pack accessed through ADB."""
    keys = required(title, build)
    if not keys or size < dmsv.HDR.size:
        return ''
    header = read_at(0, dmsv.HDR.size)
    if len(header) != dmsv.HDR.size:
        return ''
    magic, version, count = dmsv.HDR.unpack(header)
    start = dmsv.HDR.size
    end = start + count * dmsv.ENT.size
    if magic != dmsv.MAGIC or version not in (1, 2) or end > size:
        return ''
    previous = -1
    while start < end:
        length = min(end - start, 16384 * dmsv.ENT.size)
        data = read_at(start, length)
        if len(data) != length:
            return ''
        for key, stage, off, length_spv, refl, length_refl, raw in dmsv.ENT.iter_unpack(data):
            if (key <= previous or stage > 5 or (version == 1 and raw) or off < end or off + length_spv > size
                    or refl < end or length_refl < 20 or refl + length_refl > size):
                return ''
            previous = key
            if key in keys:
                if stage != keys.pop(key) or not length_spv:
                    return ''
        start += length
    return '' if keys else build


def verify(title, build, bundle):
    if not bundle:
        return ''
    try:
        with Path(bundle).open('rb') as f:
            def read_at(offset, length):
                f.seek(offset)
                return f.read(length)
            return verified_index(title, build, f.seek(0, 2), read_at)
    except OSError:
        return ''
