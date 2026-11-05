import unittest

import numpy as np

import helpers  # noqa: F401
import depthproc as dp


class SizesTest(unittest.TestCase):
    def test_model_input_size(self):
        # what VDA's Resize(lower_bound, multiple of 14) gives for the game's two frame sizes
        self.assertEqual(dp.model_input_size(1280, 720, 518), (924, 518))
        self.assertEqual(dp.model_input_size(1280, 720, 392), (700, 392))
        self.assertEqual(dp.model_input_size(640, 480, 518), (686, 518))
        self.assertEqual(dp.model_input_size(640, 480, 392), (518, 392))
        w, h = dp.model_input_size(1920, 800, 518)       # wider than 16:9: upstream shrinks the input size
        self.assertTrue(h < 518 and h % 14 == 0 and w % 14 == 0)

    def test_output_size(self):
        self.assertEqual(dp.output_size(1280, 720), (320, 180))
        self.assertEqual(dp.output_size(640, 480), (240, 180))
        self.assertEqual(dp.thumb_size(1280, 720), (16, 9))
        self.assertEqual(dp.thumb_size(640, 480), (12, 9))

    def test_area_resize(self):
        a = np.arange(16, dtype=np.float32).reshape(4, 4)
        np.testing.assert_allclose(dp.area_resize(a, 2, 2), [[2.5, 4.5], [10.5, 12.5]])
        b = np.random.default_rng(0).random((518, 924)).astype(np.float32)
        r = dp.area_resize(b, 320, 180)
        self.assertEqual(r.shape, (180, 320))
        self.assertAlmostEqual(float(r.mean()), float(b.mean()), places=2)
        with self.assertRaises(ValueError):
            dp.area_resize(a, 8, 8)

    def test_resize_bilinear(self):
        a = np.full((180, 320), 7, np.float32)
        self.assertTrue(np.allclose(dp.resize_bilinear(a, 640, 360), 7))
        ramp = np.tile(np.arange(4, dtype=np.float32), (2, 1))
        self.assertEqual(dp.resize_bilinear(ramp, 8, 4).shape, (4, 8))


class QuantiseTest(unittest.TestCase):
    def test_rule(self):
        d = np.linspace(0, 10, 10000, dtype=np.float32).reshape(100, 100)
        q = dp.quantise(d, 1.0, 9.0)
        self.assertEqual(q.dtype, np.uint8)
        self.assertEqual(int(q[d < 1].max()), 0)
        self.assertEqual(int(q[d > 9].min()), 255)
        inside = (d > 1) & (d < 9)
        rec = 1.0 + q.astype(np.float32) / 255 * 8.0
        self.assertLessEqual(float(np.abs(rec - d)[inside].max()), 8.0 / 255 / 2 + 1e-5)   # half a step

    def test_flat(self):
        q = dp.quantise(np.full((10, 10), 3.0, np.float32), 3.0, 3.0)
        self.assertEqual(q.dtype, np.uint8)
        self.assertLess(dp.range_eps(3.0), 1e-3)


class CutTest(unittest.TestCase):
    def _scene(self, seed, i, w=320, h=180):
        rng = np.random.default_rng(seed)
        base = rng.integers(0, 256, (h // 20, w // 20, 3)).astype(np.uint8)
        img = np.kron(base, np.ones((20, 20, 1), np.uint8))
        noise = np.random.default_rng(1000 + i).integers(-3, 4, img.shape)
        return np.clip(img.astype(int) + noise + i % 3, 0, 255).astype(np.uint8)

    def test_cuts(self):
        c = dp.CutDetector(1280, 720)
        flags = []
        for i in range(30):
            scene = 1 if i < 10 else (2 if i < 20 else 3)
            cut, thumb = c(self._scene(scene, i))
            flags.append(cut)
            self.assertEqual(thumb.shape, (9, 16))
        self.assertEqual([i for i, f in enumerate(flags) if f], [0, 10, 20])

    def test_fade_is_not_a_cut_per_frame(self):
        c = dp.CutDetector(1280, 720)
        img = self._scene(5, 0).astype(np.float32)
        flags = [c(np.clip(img * (1 - i / 40), 0, 255).astype(np.uint8))[0] for i in range(40)]
        self.assertEqual(sum(flags), 1)                   # only frame 0

    def test_4x3_thumb(self):
        c = dp.CutDetector(640, 480)
        _cut, thumb = c(np.zeros((518, 686, 3), np.uint8))
        self.assertEqual(thumb.shape, (9, 12))


class EdgeSnapTest(unittest.TestCase):
    def test_snap(self):
        d = np.zeros((20, 20), np.float32)
        d[:, 11:] = 1.0
        d[:5, 10] = 0.3                                    # in-between values a downscale leaves on an edge
        d[5:, 10] = 0.8
        s = dp.edge_snap(d)
        self.assertTrue(set(np.unique(s).tolist()) <= {0.0, 1.0})
        self.assertEqual(float(s[0, 10]), 0.0)
        self.assertEqual(float(s[10, 10]), 1.0)

    def test_slope_untouched(self):
        g = np.tile(np.linspace(0, 1, 200, dtype=np.float32), (20, 1))
        np.testing.assert_array_equal(dp.edge_snap(g), g)


if __name__ == '__main__':
    unittest.main()
