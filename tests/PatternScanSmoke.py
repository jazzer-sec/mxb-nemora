import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from scan_offsets import scan


class PatternScanSmoke(unittest.TestCase):
    def test_exact_wildcard_and_overlapping_matches(self):
        self.assertEqual(scan(b'\x10\x20\x10\x20', '10 ?? 10'), [0])
        self.assertEqual(scan(b'\xaa\xaa\xaa', 'AA AA'), [0, 1])
        self.assertEqual(scan(b'\x10\x20', '00 FF'), [])

    def test_rejects_empty_patterns(self):
        for pattern in ('', '?? ??'):
            with self.assertRaises(ValueError):
                scan(b'\x00', pattern)


if __name__ == '__main__':
    unittest.main()
