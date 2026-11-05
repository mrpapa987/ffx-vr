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


def soft_step(w=120, h=40, x0=60.3, sigma=2.5, near=1.0, far=0.3, slope=0.0):
    """A vertical depth step (near on the left) blurred like a model's edge, on a background with an optional slope."""
    x = np.arange(w, dtype=np.float64)
    from math import erf, sqrt
    t = np.array([0.5 * (1 - erf((xi - x0) / (sigma * sqrt(2)))) for xi in x]) if sigma > 0 else (x < x0) * 1.0
    row = far + (near - far) * t + slope * x
    return np.tile(row.astype(np.float32), (h, 1))


class EdgeTest(unittest.TestCase):
    def test_box_minmax(self):
        a = np.random.default_rng(0).random((13, 17)).astype(np.float32)
        mn, mx = dp.box_minmax(a, 2, 3)
        p = np.pad(a, ((2, 2), (3, 3)), mode='edge')
        for y in range(13):
            for x in range(17):
                win = p[y:y + 5, x:x + 7]
                self.assertEqual(mn[y, x], win.min())
                self.assertEqual(mx[y, x], win.max())

    def test_soft_step_becomes_a_step(self):
        for sigma in (1.0, 2.5, 4.0):
            d = soft_step(sigma=sigma)
            s = dp.sharpen_edges(d, rng=0.7)
            row = s[20]
            inner = (row > 0.3 + 0.1 * 0.7) & (row < 1.0 - 0.1 * 0.7)
            self.assertEqual(int(inner.sum()), 0, (sigma, np.round(row[50:70], 2)))
            cross = int(np.argmax(row < 0.65))
            self.assertLessEqual(abs(cross - 60.3), 1.0, (sigma, cross))      # the step at the 50 % crossing

    def test_sharp_step_slope_and_flat_untouched(self):
        d = soft_step(sigma=0)
        np.testing.assert_array_equal(dp.sharpen_edges(d, rng=0.7), d)
        ramp = np.tile(np.linspace(0, 1, 120, dtype=np.float32) * 3.0, (40, 1))      # a steep slope: 0.025/px
        np.testing.assert_array_equal(dp.sharpen_edges(ramp), ramp)
        yy, xx = np.mgrid[0:80, 0:80]
        bump = np.exp(-((xx - 40) ** 2 + (yy - 40) ** 2) / (2 * 12.0 ** 2)).astype(np.float32)   # a round surface
        np.testing.assert_array_equal(dp.sharpen_edges(bump), bump)
        noisy = (0.5 + np.random.default_rng(1).normal(0, 0.01, (60, 60))).astype(np.float32)
        np.testing.assert_array_equal(dp.sharpen_edges(noisy), noisy)

    def test_grow_near(self):
        d = soft_step(sigma=0)
        g = dp.grow_near(d, 2, 1, thr=0.05)
        self.assertEqual(int(np.argmax(g[20] < 0.65)), int(np.argmax(d[20] < 0.65)) + 2)
        v = d.T.copy()                                    # a horizontal step: near on top
        gv = dp.grow_near(v, 2, 1, thr=0.05)
        self.assertEqual(int(np.argmax(gv[:, 20] < 0.65)), int(np.argmax(v[:, 20] < 0.65)) + 1)
        ramp = np.tile(np.linspace(0, 1, 120, dtype=np.float32), (10, 1))
        np.testing.assert_array_equal(dp.grow_near(ramp, 2, 1, thr=0.05), ramp)

    def test_edge_shrink(self):
        d = soft_step(w=924, h=36, x0=402.6, sigma=0)                 # a step inside output texel 139
        o = dp.edge_shrink(d, 320, 12, thr=0.05)
        u = np.unique(o)
        self.assertTrue(np.all(np.isclose(u, 0.3) | np.isclose(u, 1.0)), u)            # no in-between texel
        a = dp.area_resize(d, 320, 12)
        self.assertGreater(len(np.unique(np.round(a, 5))), 2)
        ramp = np.tile(np.linspace(0, 3, 924, dtype=np.float32), (36, 1))
        np.testing.assert_allclose(dp.edge_shrink(ramp, 320, 12, thr=0.005), dp.area_resize(ramp, 320, 12), atol=1e-6)

    def test_refine_and_shrink(self):
        d = soft_step(w=924, h=518, x0=400.6, sigma=2.5)
        o = dp.refine_and_shrink(d, 320, 180)
        self.assertEqual(o.shape, (180, 320))
        row = o[90]
        self.assertEqual(int(((row > 0.37) & (row < 0.93)).sum()), 0, np.round(row[135:145], 2))
        # the near side grew ~2 model px: the step moves right by under one output texel (2.9 model px)
        self.assertIn(int(np.argmax(row < 0.65)), (139, 140))


if __name__ == '__main__':
    unittest.main()
