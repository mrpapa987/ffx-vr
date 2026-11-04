"""depthproc.py - per-frame helpers of the FMV depth converter (numpy only; no torch).

SPDX-License-Identifier: MIT (part of the FFX VR repository).

    model_input_size   the size Video-Depth-Anything's own Resize transform would give (multiples of 14)
    output_size        the stored depth size: 180 lines high (320x180 for 16:9, 240x180 for 4:3)
    area_resize        box-filter resize (exact bin means)
    CutDetector        shot cuts from the mean absolute luma change of 64x36 thumbnails
    ShotNormalizer     8-bit quantisation by p2/p98, EMA-smoothed within a shot, reset at a cut
    edge_snap          the optional light edge filter
"""
import numpy as np

OUT_H = 180


def _round_multiple(x, m, min_val):
    """Video-Depth-Anything's Resize.constrain_to_multiple_of (lower_bound, no max)."""
    y = int(np.round(x / m) * m)
    if y < min_val:
        y = int(np.ceil(x / m) * m)
    return y


def model_input_size(src_w, src_h, input_size=518, multiple=14):
    """(w, h) the model sees, as video_depth.py computes it: shorter side to input_size (keep aspect, lower_bound),
    both multiples of 14, and the input_size shrink for frames wider than 16:9 (ratio > 1.78)."""
    ratio = max(src_w, src_h) / min(src_w, src_h)
    if ratio > 1.78:
        input_size = int(input_size * 1.777 / ratio)
        input_size = round(input_size / 14) * 14
    sh, sw = input_size / src_h, input_size / src_w
    s = sw if sw > sh else sh
    return (_round_multiple(s * src_w, multiple, input_size), _round_multiple(s * src_h, multiple, input_size))


def output_size(src_w, src_h, out_h=OUT_H):
    """Stored depth size: out_h lines, width from the aspect, even."""
    w = int(round(src_w * out_h / src_h / 2.0)) * 2
    return max(w, 2), out_h


def thumb_size(src_w, src_h):
    """Luma thumbnail size: 9 lines (16x9 for 16:9, 12x9 for 4:3)."""
    return max(int(round(src_w * 9 / src_h)), 1), 9


def _bins(n_in, n_out):
    return (np.arange(n_out) * n_in) // n_out


def area_resize(img, w, h):
    """Box-filter a 2-D array to h x w: each output pixel is the mean of its input bin (bins as even as possible).
    Downscaling only (w <= width, h <= height)."""
    a = np.asarray(img, dtype=np.float32)
    H, W = a.shape[:2]
    if w > W or h > H:
        raise ValueError('area_resize only shrinks')
    ry, rx = _bins(H, h), _bins(W, w)
    s = np.add.reduceat(np.add.reduceat(a, ry, axis=0), rx, axis=1)
    cy = np.diff(np.append(ry, H)).astype(np.float32)
    cx = np.diff(np.append(rx, W)).astype(np.float32)
    return s / cy[:, None] / cx[None, :]


def luma(rgb, step=1):
    """BT.601 luma (0..255 float32) of an HxWx3 uint8 RGB frame, optionally subsampled by `step` first."""
    f = rgb[::step, ::step]
    return f[..., 0] * np.float32(0.299) + f[..., 1] * np.float32(0.587) + f[..., 2] * np.float32(0.114)


def resize_bilinear(img, w, h):
    """Bilinear resize of a 2-D (or HxWxC) array, pixel centres aligned (for previews; any direction)."""
    a = np.asarray(img, dtype=np.float32)
    H, W = a.shape[:2]
    ys = np.clip((np.arange(h) + 0.5) * H / h - 0.5, 0, H - 1)
    xs = np.clip((np.arange(w) + 0.5) * W / w - 0.5, 0, W - 1)
    y0 = np.floor(ys).astype(int); x0 = np.floor(xs).astype(int)
    y1 = np.minimum(y0 + 1, H - 1); x1 = np.minimum(x0 + 1, W - 1)
    fy = (ys - y0).astype(np.float32); fx = (xs - x0).astype(np.float32)
    if a.ndim == 3:
        fy = fy[:, None, None]; fx = fx[None, :, None]
    else:
        fy = fy[:, None]; fx = fx[None, :]
    top = a[y0][:, x0] * (1 - fx) + a[y0][:, x1] * fx
    bot = a[y1][:, x0] * (1 - fx) + a[y1][:, x1] * fx
    return top * (1 - fy) + bot * fy


class CutDetector:
    """A frame starts a new shot when the mean absolute luma difference of 64x36 thumbnails from the previous frame is
    at least `threshold` (0..255) AND at least `ratio` x the recent average difference (+1), so a fast pan or a flash
    sequence does not count every frame. Frame 0 is always a cut. Also returns the frame's tw x th luma thumbnail."""

    def __init__(self, src_w, src_h, threshold=28.0, ratio=3.0, history=15):
        self.threshold, self.ratio = float(threshold), float(ratio)
        self.cw, self.ch = (64, 36) if src_w * 9 >= src_h * 16 - 1 else (48, 36)
        self.tw, self.th = thumb_size(src_w, src_h)
        self.prev = None
        self.hist = []
        self.history = history
        self.last_diff = 0.0

    def __call__(self, rgb):
        y = luma(rgb, 2 if min(rgb.shape[:2]) >= 144 else 1)
        small = area_resize(y, self.cw, self.ch)
        thumb = np.clip(area_resize(small, self.tw, self.th) + 0.5, 0, 255).astype(np.uint8)
        if self.prev is None:
            cut, d = True, 0.0
        else:
            d = float(np.mean(np.abs(small - self.prev)))
            avg = (sum(self.hist) / len(self.hist)) if self.hist else 0.0
            cut = d >= self.threshold and d >= self.ratio * (avg + 1.0)
        self.prev = small
        self.last_diff = d
        if cut:
            self.hist = []
        else:
            self.hist.append(d)
            if len(self.hist) > self.history:
                self.hist.pop(0)
        return cut, thumb


class ShotNormalizer:
    """Quantise relative disparity to 8 bits: q = 255 * clip((d - lo) / (hi - lo)), with lo/hi the frame's 2nd/98th
    percentiles smoothed by an EMA (weight `alpha` on the past) within a shot and reset at a cut.
    Returns (q uint8, p2, p98, lo, hi): p2/p98 raw for this frame, lo/hi the values used (so d = lo + q/255*(hi-lo))."""

    def __init__(self, alpha=0.9, lo_pct=2.0, hi_pct=98.0):
        if not 0.0 <= alpha < 1.0:
            raise ValueError('alpha must be in [0, 1)')
        self.alpha, self.pcts = float(alpha), (float(lo_pct), float(hi_pct))
        self.lo = self.hi = None

    def __call__(self, depth, cut):
        d = np.asarray(depth, dtype=np.float32)
        p2, p98 = (float(v) for v in np.percentile(d, self.pcts))
        if cut or self.lo is None:
            self.lo, self.hi = p2, p98
        else:
            a = self.alpha
            self.lo = a * self.lo + (1.0 - a) * p2
            self.hi = a * self.hi + (1.0 - a) * p98
        lo, hi = self.lo, self.hi
        rng = hi - lo
        eps = max(1e-6, abs(hi) * 1e-4)
        if rng < eps:
            rng = eps
        q = np.clip((d - lo) * (255.0 / rng) + 0.5, 0, 255).astype(np.uint8)
        return q, p2, p98, lo, hi


def edge_snap(depth, strength=0.08):
    """Light edge filter: where a 3x3 neighbourhood spans more than `strength` x the frame's p2..p98 range (a depth
    edge), move the pixel to whichever of the local min / max it is closer to. That removes the in-between values a
    downscale leaves on an edge (which would become a stretched "rubber sheet" in the relief); flat areas are untouched."""
    d = np.asarray(depth, dtype=np.float32)
    p = np.pad(d, 1, mode='edge')
    H, W = d.shape
    views = [p[y:y + H, x:x + W] for y in range(3) for x in range(3)]
    mn = np.minimum.reduce(views)
    mx = np.maximum.reduce(views)
    p2, p98 = np.percentile(d, (2.0, 98.0))
    edge = (mx - mn) > strength * max(float(p98 - p2), 1e-6)
    snapped = np.where(d - mn < mx - d, mn, mx)
    return np.where(edge, snapped, d).astype(np.float32)
