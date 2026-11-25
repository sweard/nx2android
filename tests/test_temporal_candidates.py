import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'nxnative/dm/android/tools'))
from temporal_candidates import BIND, HEADER, scan


class TemporalCandidates(unittest.TestCase):
    def test_group_matching_layouts_and_preserve_indirect_sources(self):
        texture = BIND.pack(12, 2, 1, 0, 1, 1, 3, 0, 0, 80, 0)
        storage = BIND.pack(9, 3, 1, 4, 2, 1, 1, 0, 0, -1, 0)
        compute = HEADER.pack(2, 0, 64, 1, 1) + texture + storage
        fragment = HEADER.pack(1, 0, 1, 0, 0) + texture
        groups = scan([(4, 5, compute), (2, 1, fragment), (3, 5, compute),
                       (5, 1, HEADER.pack(1, 0, 3, 0, 0) + texture), (6, 5, fragment)])
        self.assertEqual([g['keys'] for g in groups], [[f'{3:016x}', f'{4:016x}'], [f'{2:016x}']])
        self.assertEqual(groups[0]['workgroup'], (64, 1, 1))
        self.assertEqual(groups[0]['sampled'], ((1, 'texture_in_uniform', 3, 80, 1),))
        self.assertEqual(groups[0]['storage'], ((2, 'image', 1, -1, 1),))
