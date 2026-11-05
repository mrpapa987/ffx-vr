"""Per-shot processing: flash / blank tags, dissolves / fades / dips, the per-segment depth range, process_video on the
synthetic FMV with the stub model (no torch, no ffmpeg)."""
import tempfile
import unittest

import numpy as np

import helpers  # noqa: F401
import convert
import depthproc as dp
import fvd
import synth

W, H = 192, 108          # small model frames for speed (the depth is 64x36)
OW, OH = 64, 36


def scene_img(seed, i=0, w=W, h=H, bright=1.0):
    rng = np.random.default_rng(seed)
    base = rng.integers(30, 220, (h // 12, w // 12, 3)).astype(np.float32)
    img = np.kron(base, np.ones((12, 12, 1), np.float32))
    img += np.random.default_rng(1000 + i).normal(0, 1.0, img.shape)
    return np.clip(img * bright, 0, 255).astype(np.uint8)


def tags_of(frames, **kw):
    det = dp.CutDetector(1280, 720)
    return [t for _f, t in dp.tag_frames(frames, det, **kw)]


class TagTest(unittest.TestCase):
    def test_flash_is_not_a_cut(self):
        fr = [scene_img(1, i) for i in range(10)] + [np.full((H, W, 3), 250, np.uint8)] * 2 + \
             [scene_img(1, i) for i in range(12, 20)] + [scene_img(2, i) for i in range(20, 30)]
        t = tags_of(fr)
        self.assertEqual(len(t), 30)
        self.assertEqual([i for i, x in enumerate(t) if x.cut], [0, 20])
        self.assertEqual([i for i, x in enumerate(t) if x.raw_cut], [0, 10, 12, 20])
        self.assertEqual([i for i, x in enumerate(t) if x.flash], [10, 11])
        self.assertTrue(t[10].blank and t[11].blank and not t[9].blank)

    def test_cut_through_black_stays_a_cut(self):
        fr = [scene_img(1, i) for i in range(10)] + [np.zeros((H, W, 3), np.uint8)] * 3 + \
             [scene_img(2, i) for i in range(13, 25)]
        t = tags_of(fr)
        self.assertEqual([i for i, x in enumerate(t) if x.cut], [0, 10, 13])
        self.assertFalse(any(x.flash for x in t))
        self.assertEqual([i for i, x in enumerate(t) if x.blank], [10, 11, 12])

    def test_long_bright_run_is_a_cut(self):
        fr = [scene_img(1, i) for i in range(10)] + [scene_img(3, i) for i in range(10, 16)] + \
             [scene_img(1, i) for i in range(16, 24)]
        t = tags_of(fr, flash_len=4)
        self.assertEqual([i for i, x in enumerate(t) if x.cut], [0, 10, 16])   # 6 frames: longer than a flash

    def test_guide(self):
        t = tags_of([scene_img(1)], guide_size=(OW, OH))
        self.assertEqual(t[0].guide.shape, (OH, OW))
        self.assertEqual(t[0].guide.dtype, np.uint8)


class TransitionTest(unittest.TestCase):
    def smalls(self, imgs):
        det = dp.CutDetector(1280, 720)
        out = []
        for f in imgs:
            det(f)
            out.append(det.small)
        return out, [float(s.std()) < 2.5 for s in out]

    def test_dissolve(self):
        a, b = scene_img(1).astype(np.float32), scene_img(2).astype(np.float32)
        imgs = [a] * 12 + [(1 - k / 21) * a + k / 21 * b for k in range(1, 21)] + [b] * 12
        imgs = [np.clip(x + np.random.default_rng(i).normal(0, 0.8, x.shape), 0, 255).astype(np.uint8)
                for i, x in enumerate(imgs)]
        sm, bl = self.smalls(imgs)
        tr = dp.find_transitions(sm, bl)
        self.assertEqual(len(tr), 1, tr)
        t = tr[0]
        self.assertEqual(t['kind'], 'dissolve')
        self.assertTrue(10 <= t['a'] <= 13 and 30 <= t['b'] <= 33, (t['a'], t['b']))
        self.assertTrue(abs(t['boundary'] - 22) <= 1, t['boundary'])
        self.assertTrue(np.all(np.diff(t['alpha']) > -0.05))

    def test_fade_and_dip(self):
        a, b = scene_img(1).astype(np.float32), scene_img(2).astype(np.float32)
        imgs = [a] * 10 + [a * (1 - k / 9) for k in range(1, 9)] + [a * 0] * 4 + [b * k / 9 for k in range(1, 9)] + \
            [b] * 10
        imgs = [np.clip(x, 0, 255).astype(np.uint8) for x in imgs]
        sm, bl = self.smalls(imgs)
        tr = dp.find_transitions(sm, bl)
        kinds = [t['kind'] for t in tr]
        self.assertIn('dip', kinds)
        dip = [t for t in tr if t['kind'] == 'dip'][0]
        self.assertTrue(18 <= dip['boundary'] <= 22, dip)
        self.assertTrue(all(t['boundary'] is None for t in tr if t['kind'] == 'fade'))

    def test_same_picture_after_black_is_not_a_dip(self):
        a = scene_img(1).astype(np.float32)
        imgs = [a] * 10 + [a * 0] * 3 + [a] * 10
        sm, bl = self.smalls([np.clip(x, 0, 255).astype(np.uint8) for x in imgs])
        self.assertEqual(dp.find_transitions(sm, bl), [])

    def test_motion_is_not_a_dissolve(self):
        big = scene_img(4, w=W * 2)
        imgs = [np.ascontiguousarray(big[:, 3 * i:3 * i + W]) for i in range(40)]      # a pan
        sm, bl = self.smalls(imgs)
        self.assertEqual([t for t in dp.find_transitions(sm, bl) if t['kind'] == 'dissolve'], [])


def _tag(blank=False, flash=False, small=None):
    return dp.Tag(cut=False, raw_cut=False, blank=blank, flash=flash, small=small, diff=0.0, thumb=None)


class RangeTest(unittest.TestCase):
    def test_one_range_for_the_shot_and_no_2pct_clip(self):
        """A subject in front of a background; a near object enters for a while. Per shot: one lo/hi for every frame,
        the subject's value does not move when the object enters, the subject's front is not clipped."""
        buf = dp.ShotBuffer()
        yy, xx = np.mgrid[0:90, 0:160]
        tags = []
        for t in range(100):
            d = 0.7 + 1.3 * yy / 90.0
            m = (xx - 60) ** 2 / 225 + (yy - 50) ** 2 / 625 < 1
            d = np.where(m, 3.0 + 0.4 * (1 - ((xx - 60) ** 2 / 225 + (yy - 50) ** 2 / 625)), d)
            if 40 <= t < 70:
                d[50:, 140:] = 4.5
            buf.append(d.astype(np.float32))
            tags.append(_tag(small=np.full((36, 64), 100.0, np.float32) + np.random.default_rng(t).normal(0, 4, (36, 64))))
        lo, hi, info = dp.shot_ranges(buf, tags)
        self.assertEqual(len(set(lo.tolist())), 1)
        self.assertEqual(len(set(hi.tolist())), 1)
        self.assertEqual(info['starts'], [0])
        q = [dp.quantise(buf[t], lo[t], hi[t]) for t in range(100)]
        centre = [int(x[50, 60]) for x in q]
        self.assertEqual(len(set(centre)), 1)                     # the subject stays where it is
        self.assertLess(centre[0], 255)                           # and its front is not clipped
        self.assertEqual(int(q[50][60:, 145].max()), 255 if hi[0] <= 4.5 else int(q[50][60:, 145].max()))

    def test_flash_frames_do_not_set_the_range(self):
        buf = dp.ShotBuffer()
        tags = []
        for t in range(30):
            d = np.linspace(1, 2, 400, dtype=np.float32).reshape(20, 20)
            fl = t in (10, 11)
            buf.append(d * (10 if fl else 1))
            tags.append(_tag(blank=fl, flash=fl, small=np.random.default_rng(t).normal(100, 9, (36, 64))))
        lo, hi, _info = dp.shot_ranges(buf, tags)
        self.assertLess(hi[0], 2.1)

    def test_dissolve_segments_and_crossfade(self):
        buf = dp.ShotBuffer()
        tags = []
        for t in range(60):
            buf.append(np.linspace(0, 1, 400, dtype=np.float32).reshape(20, 20) * (1 if t < 30 else 3) + (0 if t < 30 else 5))
            tags.append(_tag(small=np.zeros((36, 64), np.float32)))
        tr = [{'kind': 'dissolve', 'a': 24, 'b': 36, 'boundary': 30, 'alpha': np.linspace(0, 1, 13)[1:-1]}]
        lo, hi, info = dp.shot_ranges(buf, tags, transitions=tr)
        self.assertEqual(info['starts'], [0, 30])
        self.assertEqual(len(set(lo[:25].tolist())), 1)
        self.assertEqual(len(set(lo[36:].tolist())), 1)
        self.assertGreater(lo[36], 4.5)
        self.assertTrue(np.all(np.diff(lo[24:37]) >= 0))          # cross-faded, no jump
        self.assertTrue(info['blend'][25:36].all() and not info['blend'][24] and not info['blend'][36])

    def test_all_blank_segment_is_flat(self):
        buf = dp.ShotBuffer()
        tags = []
        for t in range(5):
            buf.append(np.random.default_rng(t).random((10, 10)).astype(np.float32))
            tags.append(_tag(blank=True, small=np.zeros((36, 64), np.float32)))
        lo, hi, info = dp.shot_ranges(buf, tags)
        self.assertTrue(np.all(hi - lo == 0) and info['flat'].all())

    def test_buffer_spills_to_a_file(self):
        with tempfile.TemporaryDirectory() as d:
            b = dp.ShotBuffer(ram_frames=3, spill_dir=d)
            maps = [np.full((4, 5), i, np.float32) for i in range(8)]
            for m in maps:
                b.append(m)
            self.assertEqual(len(b), 8)
            self.assertIsNotNone(b.file)
            for i in (7, 0, 4, 3):
                np.testing.assert_array_equal(b[i], maps[i])
            b.close()


class TemporalTest(unittest.TestCase):
    def run_filter(self, maps, guides=None, rel=None, **kw):
        n = len(maps)
        items = [(maps[i], None if guides is None else guides[i], True if rel is None else rel[i]) for i in range(n)]
        return list(dp.temporal_bilateral(iter(items), **kw))

    def test_count_and_static_noise(self):
        rng = np.random.default_rng(0)
        maps = [np.float32(0.4) + rng.normal(0, 3 / 255, (20, 30)).astype(np.float32) for _ in range(40)]
        out = self.run_filter(maps)
        self.assertEqual(len(out), 40)
        step_in = np.mean([np.abs(maps[i] - maps[i - 1]).mean() for i in range(1, 40)])
        step_out = np.mean([np.abs(out[i] - out[i - 1]).mean() for i in range(5, 35)])
        self.assertLess(step_out, 0.4 * step_in)

    def test_linear_change_has_no_lag(self):
        maps = [np.full((4, 4), 0.2 + 0.002 * t, np.float32) for t in range(30)]
        out = self.run_filter(maps)
        for t in range(6, 24):                     # away from the ends: symmetric weights keep a ramp exactly
            np.testing.assert_allclose(out[t], maps[t], atol=1e-6)

    def test_moving_edge_passes(self):
        """A near block moving 3 px a frame: no trail (the depth difference stops the averaging)."""
        maps, guides = [], []
        for t in range(30):
            d = np.full((4, 120), 0.2, np.float32)
            d[:, 10 + 3 * t:40 + 3 * t] = 0.8
            maps.append(d)
            guides.append(np.where(d > 0.5, 200, 80).astype(np.uint8))
        out = self.run_filter(maps, guides)
        err = max(float(np.abs(out[t] - maps[t]).max()) for t in range(30))
        self.assertLess(err, 0.01)
        out2 = self.run_filter(maps)               # depth alone (no guide) also stops at a 150-level step
        self.assertLess(max(float(np.abs(out2[t] - maps[t]).max()) for t in range(30)), 0.01)

    def test_unreliable_frame_takes_its_neighbours(self):
        maps = [np.full((3, 3), 0.5, np.float32) for _ in range(9)]
        maps[4] = np.full((3, 3), 0.95, np.float32)     # a flash: the model saw a white wall
        rel = [i != 4 for i in range(9)]
        out = self.run_filter(maps, rel=rel, passes=1)
        self.assertAlmostEqual(float(out[4].mean()), 0.5, places=2)
        for i in (3, 5):
            self.assertAlmostEqual(float(out[i].mean()), 0.5, places=5)   # the flash leaks into nobody

    def test_short_shots(self):
        for n in (1, 2, 5):
            out = self.run_filter([np.full((2, 2), i, np.float32) for i in range(n)])
            self.assertEqual(len(out), n)


class FlipTest(unittest.TestCase):
    def test_flip_merge(self):
        rng = np.random.default_rng(0)
        base = rng.random((4, 10, 16)).astype(np.float32) + 1
        ramp = np.linspace(-0.1, 0.1, 16, dtype=np.float32)[None, None, :]
        a = base + ramp                       # a model with a left/right bias...
        b = (base[:, :, ::-1] + ramp)[:, :, ::-1] * 2.0 + 0.5    # ...run on the mirror (other scale), mirrored back
        m = dp.flip_merge(a, b)
        # up to one scale and shift (relative depth), the merge is the unbiased depth: the ramps cancelled
        A = np.vstack([base.ravel(), np.ones(base.size)]).T
        k, c = np.linalg.lstsq(A, m.ravel().astype(np.float64), rcond=None)[0]
        self.assertLess(float(np.abs(m - (k * base + c)).max()), 0.01)
        self.assertGreater(float(np.abs(a - base).max()), 0.09)
        np.testing.assert_allclose(dp.flip_merge(a, a), a, atol=1e-5)

    def test_flip_in_process_video(self):
        v = synth.Video(W, H, scale=0.25)
        out = {}
        for flip in (False, True):
            m = synth.StubModel(v, out=(OW, OH), noise=0.0, flicker=0.0, leak=0.0, bias=0.2, bias_jitter=0.0)
            cfg = convert.PipeConfig(1280, 720, OW, OH, flip=flip)
            out[flip] = (m.calls, [o[0] for o in convert.process_video(v.frames(), m, cfg)])
        self.assertEqual(out[True][0], 2 * out[False][0])        # every window twice
        # the left/right bias (0.2 across the frame) is gone: the plane's left and right columns agree again
        def lr(qs):
            return float(np.mean([q[2:10, 2].astype(float).mean() - q[2:10, -3].astype(float).mean() for q in qs[5:30]]))
        self.assertGreater(abs(lr(out[False][1])), 3 * abs(lr(out[True][1])) + 1)


class ProcessVideoTest(unittest.TestCase):
    """The whole per-video pipeline on the synthetic FMV (cuts, a flash, a dissolve, a dip), with the stub model."""

    @classmethod
    def setUpClass(cls):
        cls.v = synth.Video(W, H, scale=0.5)
        cls.model = synth.StubModel(cls.v, sigma_edge=1.0, out=(OW, OH))
        cfg = convert.PipeConfig(1280, 720, OW, OH, temporal=dict(convert.TEMPORAL))
        cls.stats = {}
        cls.out = list(convert.process_video(cls.v.frames(), cls.model, cfg, cls.stats))

    def test_frames_and_flags(self):
        v, out = self.v, self.out
        self.assertEqual(len(out), v.n)
        cuts = [i for i, o in enumerate(out) if o[1] & fvd.FF_CUT]
        # the flash is not a cut; at this scale the fade in after the black frames is fast enough to be one cut
        extra = [i for i in cuts if i not in v.cut_at]
        self.assertTrue(set(v.cut_at) <= set(cuts), cuts)
        self.assertTrue(len(extra) <= 1 and all(v.dip[0] <= i < v.dip[1] for i in extra), cuts)
        self.cuts = cuts
        flash = [i for i, o in enumerate(out) if o[1] & fvd.FF_FLASH]
        self.assertTrue(set(v.flash) <= set(flash), flash)
        blend = [i for i, o in enumerate(out) if o[1] & fvd.FF_BLEND]
        d0, d1 = v.dissolve
        self.assertGreater(len(set(blend) & set(range(d0, d1))), (d1 - d0) // 2)
        self.assertEqual(self.stats['frames'], v.n)
        self.assertEqual(self.stats.get('dissolves'), 1)
        self.assertEqual(self.stats.get('dips', 0) + len(extra), 1)       # the dip: a segment boundary either way

    def test_one_range_per_segment(self):
        v, out = self.v, self.out
        lohi = [(o[4], o[5]) for o in out]
        starts = [0] + [i for i in range(1, v.n) if lohi[i] != lohi[i - 1] and not out[i][1] & fvd.FF_BLEND
                        and not out[i - 1][1] & fvd.FF_BLEND]
        # constant inside each true segment, away from the transitions
        segs = v.segments + [v.n]
        for a, b in zip(segs[:-1], segs[1:]):
            inner = [lohi[i] for i in range(a, b) if not out[i][1] & (fvd.FF_BLEND | fvd.FF_FLASH)
                     and not (v.dip[0] <= i < v.dip[1])]
            self.assertLessEqual(len(set(inner)), 1, (a, b, sorted(set(inner))[:3]))
        self.assertTrue(set(v.cut_at) <= set(starts))

    def test_model_windows_restart(self):
        # ceil(len/22) windows per shot
        bounds = [i for i, o in enumerate(self.out) if o[1] & fvd.FF_CUT] + [self.v.n]
        self.assertEqual(self.model.calls, sum(-(-(b - a) // 22) for a, b in zip(bounds[:-1], bounds[1:])))


if __name__ == '__main__':
    unittest.main()
