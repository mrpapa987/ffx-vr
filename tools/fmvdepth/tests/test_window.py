import gc
import unittest

import numpy as np

import helpers  # noqa: F401
import window as W

H, WD = 6, 8


def reference(frames, model):
    """Upstream VideoDepthAnything.infer_video_depth's windowing and alignment, batch style (all frames and all depth
    maps in lists), written from video_depth.py at 4f5ae23 so the streaming generator can be checked against it."""
    INFER_LEN, OVERLAP, KEYFRAMES, INTERP_LEN = 32, 10, [0, 12, 24, 25, 26, 27, 28, 29, 30, 31], 8
    frame_list = list(frames)
    step = INFER_LEN - OVERLAP
    org_len = len(frame_list)
    append = (step - (org_len % step)) % step + (INFER_LEN - step)
    frame_list = frame_list + [frame_list[-1]] * append
    depth_list, pre = [], None
    for frame_id in range(0, org_len, step):
        cur = [frame_list[frame_id + i] for i in range(INFER_LEN)]
        if pre is not None:
            cur[:OVERLAP] = [pre[k] for k in KEYFRAMES]
        depth_list += list(np.asarray(model(cur), np.float32))
        pre = cur
    aligned, ref = [], []
    align_len = OVERLAP - INTERP_LEN
    kf_align = KEYFRAMES[:align_len]
    for frame_id in range(0, len(depth_list), INFER_LEN):
        if not aligned:
            aligned += depth_list[:INFER_LEN]
            for k in kf_align:
                ref.append(depth_list[frame_id + k])
        else:
            curr = [depth_list[frame_id + i] for i in range(len(kf_align))]
            s, t = W.scale_and_shift(np.concatenate([c.ravel() for c in curr]), np.concatenate([r.ravel() for r in ref]))
            pre_d = aligned[-INTERP_LEN:]
            post = [np.maximum(depth_list[frame_id + i] * s + t, 0) for i in range(align_len, OVERLAP)]
            ws = W.interp_weights(INTERP_LEN)
            aligned[-INTERP_LEN:] = [pre_d[i] * (1 - ws[i]) + post[i] * ws[i] for i in range(INTERP_LEN)]
            for i in range(OVERLAP, INFER_LEN):
                aligned.append(np.maximum(depth_list[frame_id + i] * s + t, 0))
            ref = ref[:1] + [np.maximum(depth_list[frame_id + k] * s + t, 0) for k in kf_align[1:]]
    return aligned[:org_len]


class IdModel:
    """Depth of frame i = i everywhere: alignment is then the identity, so the output must be the frame ids."""

    def __init__(self):
        self.calls = 0

    def __call__(self, frames):
        self.calls += 1
        return np.stack([np.full((H, WD), float(f), np.float32) for f in frames])


class WobblyModel:
    """Each call has its own scale / shift and a frame-dependent pattern: a real test of the fit and the cross-fade."""

    def __init__(self):
        self.k = 0

    def __call__(self, frames):
        self.k += 1
        s, t = 1.0 + 0.3 * np.sin(self.k), 0.5 * np.cos(self.k * 1.7)
        yy, xx = np.mgrid[0:H, 0:WD]
        out = []
        for f in frames:
            base = 1.0 + 0.5 * np.sin(0.1 * f + xx * 0.7) + 0.2 * np.cos(0.05 * f * yy)
            out.append((base * s + t + 0.01 * ((f * 7 + self.k) % 5)).astype(np.float32))
        return np.stack(out)


class WindowTest(unittest.TestCase):
    LENGTHS = [1, 2, 9, 21, 22, 23, 31, 32, 33, 43, 44, 53, 54, 55, 75, 76, 77, 100, 333]

    def test_order_and_count(self):
        for n in [0] + self.LENGTHS:
            m = IdModel()
            out = list(W.windowed_depth(range(n), m))
            self.assertEqual([i for i, _d in out], list(range(n)), n)
            for i, d in out:
                self.assertTrue(np.allclose(d, i), (n, i, float(d.mean())))
            # upstream runs ceil(n/22) windows
            self.assertEqual(m.calls, -(-n // 22) if n else 0, n)

    def test_matches_upstream(self):
        for n in self.LENGTHS:
            ref = reference(range(n), WobblyModel())
            got = list(W.windowed_depth(range(n), WobblyModel()))
            self.assertEqual(len(got), len(ref), n)
            for (i, d), r in zip(got, ref):
                np.testing.assert_allclose(d, r, rtol=2e-5, atol=2e-5, err_msg=f'n={n} frame={i}')

    def test_keyframe_inputs(self):
        """Window k's input: the previous window's KEYFRAMES slots, then 22 new frames (slot 0 is always frame 0)."""
        seen = []

        def m(frames):
            seen.append(list(frames))
            return IdModel()(frames)
        list(W.windowed_depth(range(80), m))
        self.assertEqual(seen[0], list(range(32)))
        self.assertEqual(seen[1], [0, 12] + list(range(24, 32)) + list(range(32, 54)))
        self.assertEqual(seen[2], [0, 34] + list(range(46, 54)) + list(range(54, 76)))
        self.assertEqual(seen[3], [0, 56] + list(range(68, 76)) + [76, 77, 78, 79] + [79] * 18)   # padded with the last

    def test_latency(self):
        """A frame leaves at most 8 + 22 frames after it was read (constant lag, no buffering of the video)."""
        state = {'read': 0}

        def frames():
            for i in range(500):
                state['read'] = i + 1
                yield i
        for i, _d in W.windowed_depth(frames(), IdModel()):
            self.assertLessEqual(state['read'] - i, 32 + 22)

    def test_constant_memory(self):
        live = {'n': 0, 'max': 0}

        class Frame:
            def __init__(self, i):
                self.i = i
                self.pixels = bytearray(1000)
                live['n'] += 1
                live['max'] = max(live['max'], live['n'])

            def __del__(self):
                live['n'] -= 1

            def __float__(self):
                return float(self.i)

        def frames(n):
            for i in range(n):
                yield Frame(i)

        depth_live = {'max': 0}
        for n in (200, 3000):
            live['max'] = 0
            held = []
            for i, d in W.windowed_depth(frames(n), IdModel()):
                held.append(i)          # keep only the index, as the converter does (it writes the depth out)
                gc.collect(0) if i % 500 == 0 else None
                depth_live['max'] = max(depth_live['max'], live['n'])
            self.assertEqual(held, list(range(n)))
            self.assertLessEqual(live['max'], 2 * 32 + 2, n)    # at most two windows of frames, whatever the length
        gc.collect()
        self.assertEqual(live['n'], 0)

    def test_model_must_return_32(self):
        with self.assertRaises(ValueError):
            list(W.windowed_depth(range(40), lambda fr: np.zeros((31, 2, 2), np.float32)))

    def test_scale_and_shift(self):
        p = np.array([1.0, 2.0, 3.0, 4.0])
        s, t = W.scale_and_shift(p, 2 * p + 1)
        self.assertAlmostEqual(s, 2.0)
        self.assertAlmostEqual(t, 1.0)
        self.assertEqual(W.scale_and_shift(np.ones(4), np.arange(4.0)), (1.0, 0.0))   # singular


if __name__ == '__main__':
    unittest.main()
