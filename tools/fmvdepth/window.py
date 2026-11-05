"""window.py - Video-Depth-Anything's windowed video inference as a streaming generator (numpy only; no torch).

SPDX-License-Identifier: MIT (part of the FFX VR repository). A re-implementation of the windowing and the scale/shift
alignment of VideoDepthAnything.infer_video_depth (video_depth_anything/video_depth.py at commit 4f5ae23, Apache-2.0:
github.com/DepthAnything/Video-Depth-Anything), rewritten so that memory does not grow with the video's length:
upstream keeps every frame and every full-size depth map of the video in RAM (END_us: ~160 GB), this keeps one window.

What upstream does, and so this does (INFER_LEN 32, OVERLAP 10, KEYFRAMES [0,12,24..31], INTERP_LEN 8):
  - window 0 is frames 0..31; window k >= 1 is the previous window's inputs at KEYFRAMES (10 frames: so slot 0 is
    always video frame 0) followed by the next 22 new frames. Windows run while 22*k < video length; frames past the
    end are copies of the last frame.
  - window 0's depth is taken as it is; each later window is fitted to the previous ones by one least-squares scale and
    shift (its slots 0 and 1 against the reference depths of the same two frames), clamped at 0;
  - the 8 frames both windows saw (previous window's slots 24..31 = this window's slots 2..9) are cross-faded from the
    previous window's depth to this one's (weights 0, 1/7, ..., 1);
  - the 22 new frames are appended; the reference for KEYFRAMES[1] becomes this window's slot 12 (aligned).
The last 8 aligned frames can still be rewritten by the next window, so a frame leaves the generator 8 frames after it
was first computed (or at the end of the video).

One deliberate difference: the caller's model function returns depth already shrunk to the output size (320x180), so
the fit and the cross-fade run on those maps, not on full-resolution ones. Both are linear per pixel, so this only
changes which pixels the least-squares fit samples.

Cut-aware windows (converter 0.2): upstream runs one chain of windows over the whole video, so a window that spans a
cut feeds the model frames of two shots (its temporal attention mixes them), and every later window keeps video frame
0 and an earlier window's slot 12 as keyframes and alignment reference. shot_windows() instead splits the stream at
each shot start and runs windowed_depth() on every shot on its own: slot 0 is the shot's first frame, no window holds
frames of two shots, no fit uses another shot, and a short shot is padded with its own last frame (as upstream pads the
end of a video). Cost: each shot rounds up to whole windows, ~10-25 % more model calls on FFX's cut rate.
"""
import numpy as np

INFER_LEN = 32
OVERLAP = 10
KEYFRAMES = [0, 12, 24, 25, 26, 27, 28, 29, 30, 31]
INTERP_LEN = 8
STEP = INFER_LEN - OVERLAP            # 22 new frames per window after the first
ALIGN_LEN = OVERLAP - INTERP_LEN      # 2: KEYFRAMES[:2] = slots 0 and 12 are the fit's reference frames


def scale_and_shift(pred, target):
    """Least squares s, t with s*pred + t ~ target (upstream compute_scale_and_shift_full with an all-true mask);
    (1, 0) when the system is singular."""
    p = np.asarray(pred, dtype=np.float64).ravel()
    t = np.asarray(target, dtype=np.float64).ravel()
    a00, a01, a11 = float(p @ p), float(p.sum()), float(p.size)
    b0, b1 = float(p @ t), float(t.sum())
    det = a00 * a11 - a01 * a01
    if det == 0:
        return 1.0, 0.0
    return (a11 * b0 - a01 * b1) / det, (-a01 * b0 + a00 * b1) / det


def interp_weights(n=INTERP_LEN):
    """Upstream get_interpolate_frames' weights for the later window: 0, 1/(n-1), ..., 1."""
    step = 1.0 / (n - 1)
    return [0.0] + [i * step for i in range(1, n - 1)] + [1.0]


def _aligned(d, s, t):
    out = d * np.float32(s) + np.float32(t)
    out[out < 0] = 0
    return out


def windowed_depth(frames, model, on_window=None):
    """Yield (frame_index, depth) for every frame of the iterable `frames`, in order, exactly once.

    model(list_of_32_frames) -> array-like [32, h, w] float32 (non-negative relative disparity) for those inputs.
    Frames are opaque to this function (numpy images, ids in tests); it keeps at most two windows of them.
    on_window(k, scale, shift), if given, is called after each window (k = 0, 1, ...) for logging."""
    it = iter(frames)
    exhausted = False
    n_read = 0
    last = None

    def take(n):
        nonlocal exhausted, n_read, last
        got = []
        while len(got) < n and not exhausted:
            try:
                f = next(it)
            except StopIteration:
                exhausted = True
                break
            got.append(f)
            last = f
            n_read += 1
        return got

    first = take(INFER_LEN)
    if not first:
        return
    cur = first + [last] * (INFER_LEN - len(first))
    del first
    d = np.asarray(model(cur), dtype=np.float32)
    if d.shape[0] != INFER_LEN:
        raise ValueError(f'model returned {d.shape[0]} depth maps for {INFER_LEN} frames')
    ref = [d[0].copy(), d[KEYFRAMES[1]].copy()]
    pending = [d[i].copy() for i in range(INFER_LEN)]   # aligned depth of frames base .. base+len-1
    base = 0                                           # frame index of pending[0]
    if on_window:
        on_window(0, 1.0, 0.0)
    w8 = interp_weights()

    def emit(upto):
        """Yield pending frames until only `upto` remain, never past the real video length."""
        nonlocal base
        while len(pending) > upto:
            dm = pending.pop(0)
            if base < n_read:
                yield base, dm
            base += 1

    yield from emit(INTERP_LEN)
    prev = cur
    k = 1
    while True:
        new = take(STEP)
        # upstream runs window k while its frame_id 22*k < length. n_read is now min(length, 22*k + 32), so the test
        # below is exact (a window may have no new real frame at all: then only its cross-fade changes anything).
        if STEP * k >= n_read:
            break
        cur = [prev[i] for i in KEYFRAMES] + new + [last] * (STEP - len(new))
        d = np.asarray(model(cur), dtype=np.float32)
        if d.shape[0] != INFER_LEN:
            raise ValueError(f'model returned {d.shape[0]} depth maps for {INFER_LEN} frames')
        s, t = scale_and_shift(np.concatenate([d[i].ravel() for i in range(ALIGN_LEN)]),
                               np.concatenate([r.ravel() for r in ref]))
        # the 8 frames both windows saw: cross-fade from the pending (earlier) depth to this window's
        assert len(pending) == INTERP_LEN
        for i in range(INTERP_LEN):
            post = _aligned(d[ALIGN_LEN + i], s, t)
            pending[i] = pending[i] * np.float32(1.0 - w8[i]) + post * np.float32(w8[i])
        for i in range(OVERLAP, INFER_LEN):
            pending.append(_aligned(d[i], s, t))
        ref = [ref[0], _aligned(d[KEYFRAMES[1]], s, t)]
        if on_window:
            on_window(k, s, t)
        yield from emit(INTERP_LEN)
        prev = cur
        k += 1
    yield from emit(0)


_END = object()


def split_shots(items, is_start):
    """Split the iterable `items` into consecutive runs, a new run at every item after the first for which
    is_start(item) is true. Yields one iterator per run, lazily: it reads one item ahead (to see where the run ends) and
    never holds more than that one. A run the caller does not finish is skipped over before the next one starts."""
    it = iter(items)
    nxt = [next(it, _END)]

    def run():
        first = True
        while nxt[0] is not _END:
            x = nxt[0]
            if not first and is_start(x):
                return
            first = False
            nxt[0] = next(it, _END)
            yield x

    while nxt[0] is not _END:
        g = run()
        yield g
        for _x in g:
            pass


def shot_windows(items, model, is_start, frame_of=lambda x: x, on_window=None):
    """Cut-aware windowed depth. `items` are the video's frames in order (or tuples carrying them: frame_of(item) gives
    the frame for the model); a shot starts at every item for which is_start(item) is true (the first item always
    starts one). Yields (start, depths) per shot: `start` the shot's first frame index in the video, `depths` a
    generator of (frame_index, depth) for that shot's frames, in order, each exactly once (windowed_depth restarted on
    the shot alone). A shot's generator left unfinished is run to its end before the next shot starts."""
    base = 0
    for shot in split_shots(items, is_start):
        count = [0]

        def frames(shot=shot, count=count):
            for x in shot:
                count[0] += 1
                yield frame_of(x)

        def depths(frames=frames, base=base):
            for j, d in windowed_depth(frames(), model, on_window):
                yield base + j, d
        g = depths()
        yield base, g
        for _x in g:            # (normally already consumed: then this does nothing)
            pass
        base += count[0]
