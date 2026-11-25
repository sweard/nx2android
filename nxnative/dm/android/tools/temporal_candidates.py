"""Find possible reconstruction passes from exported shader reflection.

Each group shares a binding layout. Matching a layout alone cannot identify
scene depth, previous-frame data, a camera buffer, or the order of HUD draws.
"""
import argparse
import json
import struct

import dmsv

HEADER = struct.Struct('<5I')
BIND = struct.Struct('<BBBBHHhBBii')
SOURCES = {1: 'uniform', 7: 'texture', 9: 'image', 11: 'texture_array',
           12: 'texture_in_uniform', 14: 'image_in_uniform', 15: 'dynamic_texture'}


def scan(entries):
    groups = {}
    for key, stage, reflection in entries:
        if stage not in (1, 5):
            continue
        count, constants, x, y, z = HEADER.unpack_from(reflection)
        end = HEADER.size + count * BIND.size
        if end + constants != len(reflection):
            raise ValueError(f'invalid reflection size for shader {key:016x}')
        if stage == 1 and (x, y, z) != (1, 0, 0):  # One non-integer color output, location 0.
            continue
        sampled, storage, uniforms = [], [], []
        for kind, descriptor, dim, flags, binding, n, reg, _, _, offset, _ in BIND.iter_unpack(reflection[HEADER.size:end]):
            if kind == 1 and descriptor == 0:
                uniforms.append(reg)
                continue
            if dim != 1 or flags & 3 or kind not in SOURCES:  # plain 2D, single sample
                continue
            slot = (binding, SOURCES[kind], reg, offset, n)
            if descriptor in (2, 6):
                sampled.append(slot)
            elif descriptor == 3:
                storage.append(slot)
        if not sampled or (stage == 5 and not storage):
            continue
        shape = ('fragment' if stage == 1 else 'compute', (x, y, z) if stage == 5 else (),
                 tuple(sorted(sampled)), tuple(sorted(storage)), tuple(sorted(uniforms)))
        groups.setdefault(shape, []).append(f'{key:016x}')
    return [dict(stage=stage, workgroup=workgroup, sampled=sampled, storage=storage,
                 uniforms=uniforms, keys=sorted(keys))
            for (stage, workgroup, sampled, storage, uniforms), keys in sorted(groups.items())]


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('bundle')
    args = parser.parse_args()
    with dmsv.Bundle(args.bundle) as bundle:
        result = scan((entry[0], entry[1], bundle.refl(entry)) for entry in bundle.entries)
    print(json.dumps(result, indent=2))
