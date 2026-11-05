"""depthproc.py - per-frame helpers of the FMV depth converter (numpy only; no torch).

SPDX-License-Identifier: MIT (part of the FFX VR repository).

    model_input_size   the size Video-Depth-Anything's own Resize transform would give (multiples of 14)
    output_size        the stored depth size: 180 lines high (320x180 for 16:9, 240x180 for 4:3)
    area_resize        box-filter resize (exact bin means)
    CutDetector        shot cuts from the mean absolute luma change of 64x36 thumbnails
    tag_frames         per-frame tags: cuts with the flash test, blank frames (streaming, a few frames late)
    find_transitions   dissolves, fades and dips through black inside a shot
    ShotBuffer         a shot's float depth maps (RAM, then a temporary file)
    shot_ranges        one depth range per shot segment (robust percentiles over all its frames), as lo/hi per frame
    quantise           the 8-bit rule of FORMAT.md
    temporal_bilateral a centred, edge-preserving temporal filter (per shot)
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
        self.small = small
        self.last_diff = d
        if cut:
            self.hist = []
        else:
            self.hist.append(d)
            if len(self.hist) > self.history:
                self.hist.pop(0)
        return cut, thumb


# ==================================================================== frame tags: cuts, flashes, blank frames
class Tag:
    """What the converter knows about one frame before depth: cut (a shot starts here: FF_CUT, the model's windows
    restart), raw_cut (the detector's own verdict, before the flash test), blank (a near-uniform picture: black,
    white, a flat colour), flash (a short bright / changed run inside a shot that returns to the picture before it),
    small (the detector's 64x36 luma), thumb (the index thumbnail), guide (a uint8 luma image at the depth's size, for
    the temporal filter; None when not asked for)."""
    __slots__ = ('cut', 'raw_cut', 'blank', 'flash', 'small', 'pre', 'diff', 'thumb', 'guide')

    def __init__(self, **kw):
        self.guide = None
        self.pre = None
        for k, v in kw.items():
            setattr(self, k, v)


def _corr(a, b):
    x = np.asarray(a, np.float64).ravel()
    y = np.asarray(b, np.float64).ravel()
    x = x - x.mean()
    y = y - y.mean()
    den = float(np.sqrt((x * x).sum() * (y * y).sum()))
    return float((x * y).sum()) / den if den > 0 else 1.0


def tag_frames(frames, detector, flash_len=4, blank_std=2.5, guide_size=None, same_corr=0.9):
    """Yield (frame, Tag) for every frame, `flash_len` + 1 frames late (the flash test looks ahead).

    blank: the 64x36 luma's standard deviation < blank_std levels (nothing a depth model can read).
    flash: a raw cut at t whose picture comes back within flash_len frames (some u in t+1..t+flash_len has
    mean |small[u] - small[t-1]| < half the cut threshold and < half the cut's own step): frames t..u-1 are a flash,
    and the cuts at t..u are dropped (the shot goes on; the model's windows are not restarted; the frames are left out
    of the shot's depth range). A cut into or out of a run of blank frames that does not come back stays a cut.
    A detector cut between two non-blank frames whose 64x36 lumas correlate >= same_corr is the same picture
    brighter or darker (a fast fade steps past the threshold every frame): not a cut.
    guide_size (w, h): also make each frame's luma at that size (Tag.guide, uint8)."""
    pend = []                     # [frame, Tag], oldest first
    prev_small = None
    prev_blank = True
    half = detector.threshold * 0.5
    for f in frames:
        cut, thumb = detector(f)
        small = detector.small
        blank = float(small.std()) < blank_std
        if cut and prev_small is not None and not blank and not prev_blank and _corr(small, prev_small) >= same_corr:
            cut = False           # the same picture, brighter or darker (a quick fade): not a new shot
        t = Tag(cut=cut, raw_cut=cut, blank=blank, flash=False, small=small, pre=prev_small,
                diff=detector.last_diff, thumb=thumb)
        prev_blank = blank
        if guide_size is not None:
            g = area_resize(luma(f, 2 if min(f.shape[:2]) >= 2 * guide_size[1] else 1), guide_size[0], guide_size[1])
            t.guide = np.clip(g + 0.5, 0, 255).astype(np.uint8)
        prev_small = small
        pend.append([f, t])
        # does this frame close a flash that started at a pending raw cut?
        u = len(pend) - 1
        for k in range(max(0, u - flash_len), u):
            c = pend[k][1]
            if c.raw_cut and c.pre is not None and not c.flash:
                back = float(np.mean(np.abs(small - c.pre)))
                if back < half and back < 0.5 * c.diff:
                    for j in range(k, u):
                        pend[j][1].flash = True
                        pend[j][1].cut = False
                    t.cut = False
                    break
        while len(pend) > flash_len + 1:
            yield tuple(pend.pop(0))
    for p in pend:
        yield tuple(p)


# ==================================================================== transitions inside a shot: dissolves, fades, dips
def _runs(mask):
    """[(start, end_exclusive)] of the True runs of a 1-D bool array."""
    m = np.concatenate([[False], np.asarray(mask, bool), [False]])
    d = np.flatnonzero(m[1:] != m[:-1])
    return list(zip(d[::2].tolist(), d[1::2].tolist()))


def find_transitions(smalls, blank, min_len=6, d1_min=0.5, curv=0.5, min_change=20.0, max_resid=0.25, gap=2,
                     dip_corr=0.6):
    """Gradual transitions inside one shot (between two cuts), from the 64x36 luma of its frames.

    A cross-dissolve from picture A to picture B is, frame by frame, (1 - a) A + a B with a rising: the luma keeps
    changing (mean |step| >= d1_min) while its second difference stays small (<= curv x the step: a linear blend has
    none; motion has). Runs of such frames (gaps up to `gap` frames bridged, at least min_len long) are then checked:
    the end pictures must differ (mean |B - A| >= min_change levels) and every frame between must be such a blend
    (least-squares a per frame, residual <= max_resid x |B - A|, a not falling). A blend with a blank end is a fade
    (to or from black / white): one shot, no boundary. A blend between two pictures is a dissolve: a new
    normalisation segment starts where a crosses 0.5.
    A dip: a run of blank frames inside the shot with different pictures on either side (correlation of the nearest
    non-blank frames < dip_corr): a fade out and in through black. A new segment starts in the middle of the run.

    Returns a list of dicts: kind ('dissolve' | 'fade' | 'dip'), a, b (the frames just outside the transition: frames
    a+1 .. b-1 are its blend / blank frames), boundary (the first frame of the new segment, or None), alpha (per
    frame a+1 .. b-1, the blend weight of B; dips: 0 before the boundary, 1 after)."""
    n = len(smalls)
    out = []
    blank = np.asarray(blank, bool)
    if n >= 3:
        S = np.stack([np.asarray(s, np.float32) for s in smalls])
        d1 = np.zeros(n)
        d1[1:] = np.abs(S[1:] - S[:-1]).mean(axis=(1, 2))
        d2 = np.full(n, np.inf)
        d2[1:-1] = np.abs(S[2:] - 2 * S[1:-1] + S[:-2]).mean(axis=(1, 2))
        cand = (d1 >= d1_min) & (d2 <= curv * d1)
        cand[0] = cand[-1] = False
        runs = _runs(cand)
        merged = []
        for s, e in runs:
            if merged and s - merged[-1][1] <= gap:
                merged[-1] = (merged[-1][0], e)
            else:
                merged.append((s, e))
        for s, e in merged:
            if e - s < min_len:
                continue
            a, b = s - 1, min(e, n - 1)
            A, B = S[a], S[b]
            dAB = B - A
            change = float(np.abs(dAB).mean())
            if change < min_change:
                continue
            den = float((dAB * dAB).sum())
            mid = S[a + 1:b]
            al = ((mid - A) * dAB).sum(axis=(1, 2)) / den
            res = np.abs(mid - A - al[:, None, None] * dAB).mean(axis=(1, 2)) / change
            if res.max() > max_resid or al.min() < -0.15 or al.max() > 1.15 or np.any(np.diff(al) < -0.1):
                continue
            al = np.clip(al, 0.0, 1.0)
            if blank[a] or blank[b]:
                out.append({'kind': 'fade', 'a': a, 'b': b, 'boundary': None, 'alpha': al})
            else:
                k = int(np.argmax(al >= 0.5)) if (al >= 0.5).any() else len(al)
                out.append({'kind': 'dissolve', 'a': a, 'b': b, 'boundary': a + 1 + k, 'alpha': al})
        for p, q in _runs(blank):
            if p == 0 or q >= n:
                continue
            if _corr(S[p - 1], S[q]) < dip_corr:
                bnd = (p + q) // 2
                al = (np.arange(p, q) >= bnd).astype(np.float64)
                out.append({'kind': 'dip', 'a': p - 1, 'b': q, 'boundary': bnd, 'alpha': al})
    out.sort(key=lambda t: t['a'])
    return out


# ==================================================================== the depth range of a shot
class ShotBuffer:
    """The float depth maps of one shot, in order: in RAM up to ram_frames maps, then in a temporary file (in spill_dir,
    deleted when closed), so a long shot cannot run the PC out of memory."""

    def __init__(self, ram_frames=2400, spill_dir=None):
        self.ram, self.ram_frames, self.spill_dir = [], int(ram_frames), spill_dir
        self.file = None
        self.n = 0
        self.shape = None

    def append(self, d):
        d = np.ascontiguousarray(d, dtype=np.float32)
        if self.shape is None:
            self.shape = d.shape
        elif d.shape != self.shape:
            raise ValueError(f'depth map {d.shape}, the shot has {self.shape}')
        if len(self.ram) < self.ram_frames:
            self.ram.append(d)
        else:
            if self.file is None:
                import tempfile
                self.file = tempfile.TemporaryFile(prefix='fvdshot-', dir=self.spill_dir)
            self.file.seek((self.n - self.ram_frames) * d.nbytes)
            self.file.write(d.tobytes())
        self.n += 1

    def __len__(self):
        return self.n

    def __getitem__(self, k):
        if not 0 <= k < self.n:
            raise IndexError(k)
        if k < len(self.ram):
            return self.ram[k]
        nb = int(np.prod(self.shape)) * 4
        self.file.seek((k - self.ram_frames) * nb)
        return np.frombuffer(self.file.read(nb), dtype=np.float32).reshape(self.shape)

    def close(self):
        self.ram = []
        if self.file is not None:
            self.file.close()
            self.file = None


def range_eps(hi):
    """FORMAT.md "The values": a range below this is a flat frame."""
    return max(1e-6, abs(hi) * 1e-4)


def quantise(d, lo, hi):
    """8-bit depth against (lo, hi), exactly as FORMAT.md "The values" says: q = 255 (d - lo) / (hi - lo) rounded
    half up, clamped to 0..255, the range floored at range_eps (a flat frame)."""
    rng = max(hi - lo, range_eps(hi))
    return np.clip((np.asarray(d, np.float32) - np.float32(lo)) * np.float32(255.0 / rng) + np.float32(0.5),
                   0, 255).astype(np.uint8)


def shot_ranges(buf, tags, pcts=(0.1, 99.9), headroom=0.02, sample=4, transitions=None):
    """One depth range per normalisation segment of a shot, written as every frame's lo/hi (FORMAT.md v1 meaning:
    d ~ lo + q/255 (hi - lo)).

    Segments: the shot, split at each dissolve / dip boundary (find_transitions). A segment's range: the pcts
    percentiles of all its reliable frames' pixels together (every `sample`-th pixel each way; reliable = not blank,
    not a flash, not a transition frame; if none, its non-blank frames; if none, the segment is flat: lo = hi), widened
    by `headroom` x the range at both ends. So nothing but true outliers clips (no plateau at the nearest 2 % of every
    frame), and the mapping is the same for the whole segment (nothing breathes or sinks when something enters).
    Dissolve / dip frames get lo/hi interpolated between the two segments by their blend weight; fade frames keep
    their segment's.

    Returns (lo, hi, info): float arrays per frame, and a per-frame list of 'blend' flags plus segment starts."""
    n = len(buf)
    if transitions is None:
        transitions = find_transitions([t.small for t in tags], [t.blank for t in tags])
    blend = np.zeros(n, bool)
    alpha = {}
    starts = [0]
    for tr in transitions:
        blend[tr['a'] + 1:tr['b']] = True
        if tr['boundary'] is not None:
            starts.append(tr['boundary'])
            for j, al in enumerate(tr['alpha']):
                alpha[tr['a'] + 1 + j] = (tr['boundary'], float(al))
    starts = sorted(set(starts))
    seg_of = np.zeros(n, int)
    for k, s in enumerate(starts):
        seg_of[s:] = k
    rel = np.array([not (t.blank or t.flash) for t in tags]) & ~blend
    nonblank = np.array([not t.blank for t in tags])
    rngs = []
    for k, s in enumerate(starts):
        e = starts[k + 1] if k + 1 < len(starts) else n
        idx = [i for i in range(s, e) if rel[i]] or [i for i in range(s, e) if nonblank[i]]
        if not idx:
            rngs.append(None)
            continue
        pool = np.concatenate([buf[i][::sample, ::sample].ravel() for i in idx])
        L, H = (float(v) for v in np.percentile(pool, pcts))
        w = (H - L) * headroom
        rngs.append((L - w, H + w))
    # a flat segment takes a neighbour's range if it has one (its frames are blank: they show nothing anyway)
    lo = np.zeros(n)
    hi = np.zeros(n)
    flat = np.zeros(n, bool)
    for i in range(n):
        r = rngs[seg_of[i]]
        if r is None:
            d = buf[i]
            m = float(np.median(d))
            lo[i] = hi[i] = m
            flat[i] = True
        else:
            lo[i], hi[i] = r
    for i, (bnd, al) in alpha.items():
        k = starts.index(bnd)
        a_r, b_r = rngs[k - 1], rngs[k]
        if a_r is not None and b_r is not None:
            lo[i] = (1 - al) * a_r[0] + al * b_r[0]
            hi[i] = (1 - al) * a_r[1] + al * b_r[1]
    return lo, hi, {'blend': blend, 'starts': starts, 'flat': flat, 'transitions': transitions}


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


# ==================================================================== temporal filter (centred, edge-preserving)
def _guide01(g):
    if g is None:
        return None
    g = np.asarray(g)
    return g.astype(np.float32) * np.float32(1.0 / 255.0) if g.dtype == np.uint8 else g.astype(np.float32, copy=False)


def _tbil_pass(items, radius, sigma_t, sigma_n, sigma_y):
    """One pass of temporal_bilateral over a stream of (n, guide, reliable); yields the same triples, filtered."""
    a_n = np.float32(0.5 / (sigma_n * sigma_n))
    a_y = np.float32(0.5 / (sigma_y * sigma_y)) if sigma_y > 0 else None
    tw = {d: float(0.5 * d * d / (sigma_t * sigma_t)) for d in range(-radius, radius + 1)}
    buf = []                  # (n, guide float or None, reliable)
    base = 0                  # stream index of buf[0]
    t = 0                     # next output

    def out(t):
        c_n, c_g, c_rel = buf[t - base]
        acc = np.zeros_like(c_n)
        wsum = np.zeros_like(c_n)
        for s in range(max(base, t - radius), min(base + len(buf), t + radius + 1)):
            n, g, rel = buf[s - base]
            if s == t:
                w = np.float32(1.0 if c_rel else 1e-3)
                acc += w * n
                wsum += w
                continue
            if not rel:
                continue
            if not c_rel:             # this frame's own value is junk: its reliable neighbours by time alone
                w = np.float32(np.exp(-tw[s - t]))
                acc += w * n
                wsum += w
                continue
            e = (n - c_n) ** 2 * a_n + np.float32(tw[s - t])
            if a_y is not None and g is not None and c_g is not None:
                e += (g - c_g) ** 2 * a_y
            w = np.exp(-e)
            acc += w * n
            wsum += w
        return acc / wsum, c_g, c_rel

    for n, g, rel in items:
        buf.append((np.asarray(n, np.float32), _guide01(g), bool(rel)))
        while t + radius < base + len(buf):
            yield out(t)
            t += 1
            while base < t - radius:
                buf.pop(0)
                base += 1
    while t < base + len(buf):
        yield out(t)
        t += 1


def temporal_bilateral(items, radius=3, passes=2, sigma_t=2.0, sigma_n=8.0 / 255, sigma_y=0.05):
    """A centred (zero-phase, no lag) edge-preserving temporal filter over one shot.

    items: (n, guide, reliable) per frame in order: n the normalised depth (float, 1 = the segment's range), guide the
    frame's luma at the same size (uint8 or 0..1 float; None to filter by depth alone), reliable False for a flash or
    blank frame. Each output pixel is the weighted mean of the same pixel in frames t-radius .. t+radius, the weight
    exp(-dt^2 / 2 sigma_t^2 - dn^2 / 2 sigma_n^2 - dY^2 / 2 sigma_y^2): steady depth averages, while a pixel whose depth
    or luma changes (something moving across it) keeps its own value, so motion passes without a trail. Unreliable
    frames give no weight to others and take theirs from their neighbours. `passes` passes (each one needs `radius`
    frames more on each side). Run it per shot: nothing crosses a cut. Yields the filtered n per frame, in order,
    radius x passes frames late."""
    it = ((np.asarray(n, np.float32), g, rel) for n, g, rel in items)
    for _ in range(passes):
        it = _tbil_pass(it, radius, sigma_t, sigma_n, sigma_y)
    for n, _g, _rel in it:
        yield n
