"""fvd_stats.py on synthetic .fvd files (zlib; MIT, part of the FFX VR repository)."""
import io
import os
import tempfile
import unittest
from contextlib import redirect_stdout

import numpy as np

import helpers  # noqa: F401
import fvd
import fvd_stats


class FvdStatsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()

    def tearDown(self):
        self.tmp.cleanup()

    def _write(self, n, noise, cuts=(0,), pump=0):
        p = os.path.join(self.tmp.name, 'x.fvd')
        rng = np.random.default_rng(3)
        wr = fvd.FvdWriter(p, 320, 180, 30000, 1001, codec=fvd.CODEC_ZLIB, model_id=fvd.MODEL_VDA_SMALL, src_name='opn_us.webm')
        for i in range(n):
            base = 100 + (pump if i % 2 else 0)
            q = np.clip(base + rng.normal(0, noise, (180, 320)), 0, 255).round().astype(np.uint8) if noise else np.full((180, 320), base, np.uint8)
            q[0, :8] = 255
            wr.add_frame(q, fvd.FF_CUT if i in cuts else 0, p2=1.0, p98=2.0 + 0.1 * (i % 2), lo=1.0, hi=2.0 + i * 0.01)
        wr.close()
        return p

    def test_still(self):
        s = fvd_stats.stats(self._write(30, 0))
        self.assertEqual((s['frames'], s['shots']), (30, 1))
        self.assertEqual(s['noise'], 0.0)
        self.assertEqual(s['pump'], 0.0)
        self.assertAlmostEqual(s['clip255'], 8 / (320 * 180))
        self.assertGreater(s['p98_jump'], 0.04)

    def test_noise_pump_shots(self):
        s = fvd_stats.stats(self._write(100, 3.0, cuts=(0, 40), pump=4))
        self.assertEqual(s['shots'], 2)
        self.assertGreater(s['noise'], 1.0)
        self.assertGreater(s['pump'], 3.0)
        self.assertEqual([(a, b) for a, b, _, _ in s['shot_list']], [(0, 40), (40, 100)])
        self.assertTrue(np.isfinite(s['w22']))

    def test_main(self):
        p = self._write(10, 0)
        out = io.StringIO()
        with redirect_stdout(out):
            self.assertEqual(fvd_stats.main([p, '--shots', os.path.join(self.tmp.name, 'missing.fvd')]), 0)
        text = out.getvalue()
        self.assertIn('x.fvd: 10 frames, 1 shots', text)
        self.assertIn('shot      0-     9', text)
        self.assertIn('missing.fvd: cannot read', text)


if __name__ == '__main__':
    unittest.main()
