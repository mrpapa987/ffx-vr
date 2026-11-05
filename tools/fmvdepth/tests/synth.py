"""synth.py - a synthetic FMV with known depth, and a stub depth model with Video-Depth-Anything-like flaws (numpy only).

SPDX-License-Identifier: MIT (part of the FFX VR repository). For the converter's tests and its before/after metrics;
nothing here is game data.

Video: a textured far plane (disparity 0.30 .. 0.45 top to bottom) with a textured disc in front (1.0 + a 0.2 dome)
that moves; shots joined by hard cuts, a cross-dissolve and a fade out / in through black, plus a two-frame white
flash, a slow dolly of the plane and an object entering from the right (disparity 1.5). Frames are RGB uint8 at
(w, h), the model's input size; frame i's exact disparity is truth(i). The frame index is written into two pixels of
the top row (left: index, marker 77 in blue; right: marker 0), so a model call can find the truth of the frame it is
given, mirrored or not.

StubModel: depth = truth (mirrored with the frame), blurred (soft edges: sigma_edge model px), then per call (one
call = one 32-frame window) a random scale and shift (affine ambiguity), a little of the window's mean disparity
leaking into every frame (context, like temporal attention: a window that spans two shots shifts its frames), a
left/right bias ramp (the same sign in every call, so a mirrored call's runs the other way on the picture: flip
averaging cancels it), and per frame i.i.d. noise plus a
smooth flicker field. Returns [T, h, w] float32, non-negative."""
import numpy as np

FLASH_WHITE = 1.0       # the truth a model "sees" in a white flash frame: a flat wall
BLACK_TRUTH = 0.5


def _texture(seed, w, h, cells=(24, 14), lo=60.0, hi=180.0, fine=12.0):
    rng = np.random.default_rng(seed)
    cw, ch = cells
    base = rng.uniform(lo, hi, (ch + 1, cw + 1, 3)).astype(np.float32)
    ys = np.linspace(0, ch, h, dtype=np.float32)
    xs = np.linspace(0, cw, w, dtype=np.float32)
    y0 = np.minimum(ys.astype(int), ch - 1)
    x0 = np.minimum(xs.astype(int), cw - 1)
    fy = (ys - y0)[:, None, None]
    fx = (xs - x0)[None, :, None]
    t = (base[y0][:, x0] * (1 - fx) + base[y0][:, x0 + 1] * fx) * (1 - fy) + \
        (base[y0 + 1][:, x0] * (1 - fx) + base[y0 + 1][:, x0 + 1] * fx) * fy
    return t + rng.normal(0, fine, (h, w, 3)).astype(np.float32)


class Scene:
    """One shot's content: plane + discs (+ an entering object), as a function of the shot-local time t."""

    def __init__(self, w, h, seed, discs, dolly_from=None, dolly_rate=0.002, enter=None, disc_luma=50.0,
                 plane=(60.0, 180.0)):
        self.w, self.h = w, h
        self.plane_tex = _texture(seed, w, h, lo=plane[0], hi=plane[1])
        self.discs = discs                  # [(cx0, cy0, vx, vy, r)] in px and px/frame
        self.disc_tex = [_texture(seed * 31 + k + 7, int(2 * d[4]) + 3, int(2 * d[4]) + 3, cells=(6, 6),
                                  lo=60 + disc_luma, hi=180 + disc_luma) for k, d in enumerate(discs)]
        self.dolly_from, self.dolly_rate, self.enter = dolly_from, dolly_rate, enter
        yy, xx = np.mgrid[0:h, 0:w]
        self.yy, self.xx = yy.astype(np.float32), xx.astype(np.float32)
        self.plane = (0.30 + 0.15 * self.yy / h).astype(np.float32)

    def disc_at(self, k, t):
        cx0, cy0, vx, vy, r = self.discs[k]
        return cx0 + vx * t, cy0 + vy * t, r

    def enter_width(self, t):
        if not self.enter:
            return 0
        a, b, wmax = self.enter
        if not a <= t < b:
            return 0
        ramp = min(1.0, (t - a) / 10.0, (b - t) / 10.0)
        return int(round(wmax * self.w * ramp))

    def render(self, t):
        """(rgb float32 0..255, truth float32) at shot-local time t."""
        rgb = self.plane_tex.copy()
        tr = self.plane.copy()
        if self.dolly_from is not None and t > self.dolly_from:
            tr += np.float32(self.dolly_rate * (t - self.dolly_from))
        for k in range(len(self.discs)):
            cx, cy, r = self.disc_at(k, t)
            rho2 = ((self.xx - cx) ** 2 + (self.yy - cy) ** 2) / (r * r)
            m = rho2 < 1.0
            tr[m] = 1.0 + 0.2 * np.sqrt(1.0 - rho2[m])
            tex = self.disc_tex[k]
            ty = np.clip((self.yy[m] - cy + r + 1).astype(int), 0, tex.shape[0] - 1)
            tx = np.clip((self.xx[m] - cx + r + 1).astype(int), 0, tex.shape[1] - 1)
            rgb[m] = tex[ty, tx]
        ew = self.enter_width(t)
        if ew > 0:
            y0 = int(0.55 * self.h)
            tr[y0:, self.w - ew:] = 1.5
            rgb[y0:, self.w - ew:] = (200.0, 90.0, 60.0)
        return rgb, tr


class Video:
    """The test FMV. scale shortens it for unit tests (frame counts are multiplied by it)."""

    def __init__(self, w=924, h=518, scale=1.0, noise=1.5, seed=0):
        self.w, self.h = w, h
        s = lambda n: max(2, int(round(n * scale)))  # noqa: E731
        px = w / 924.0                                  # motions are given at 924 px wide
        self.noise, self.seed = noise, seed
        r1 = 0.15 * h
        self.scenes = [
            Scene(w, h, 1, [(0.25 * w, 0.45 * h, 3.0 * px, 0.0, r1)], dolly_from=s(60), enter=(s(70), s(100), 0.22)),
            Scene(w, h, 2, [(0.35 * w, 0.5 * h, 0.0, 0.6 * px, 0.2 * h)], plane=(10.0, 90.0)),
            Scene(w, h, 3, [(0.7 * w, 0.4 * h, -2.0 * px, 0.0, 0.17 * h)], disc_luma=-60.0, plane=(140.0, 230.0)),
            Scene(w, h, 4, [(0.3 * w, 0.5 * h, 1.0 * px, 0.0, 0.12 * h), (0.7 * w, 0.55 * h, -1.0 * px, 0.0, 0.1 * h)],
                  plane=(30.0, 130.0)),
            Scene(w, h, 5, [(0.5 * w, 0.5 * h, 0.0, 0.0, 0.18 * h)], plane=(110.0, 200.0)),
        ]
        # timeline: (first frame, scene, local t0) runs; transitions listed separately
        a, b, c, d, e = s(120), s(90), s(30), s(90), s(20)
        self.flash = [s(40), s(40) + 1]                         # two white frames inside shot 1
        self.cut_at = [0, a, a + b + c + d]                      # hard cuts (first frames of shots)
        self.dissolve = (a + b, a + b + c)                       # frames [i0, i1) blend scene 1 -> scene 2
        fo, bl, fi = s(8), s(6), s(8)
        self.dip_start = a + b + c + d + e                       # fade out of scene 3, black, fade in of scene 4
        self.dip = (self.dip_start, self.dip_start + fo + bl + fi)
        self.fo, self.bl, self.fi = fo, bl, fi
        self.n = self.dip[1] + s(48)
        # the true normalisation segments: starts (hard cuts, the dissolve middle, the dip middle)
        self.segments = sorted(self.cut_at + [a + b + c // 2, self.dip_start + fo + bl // 2])

    def where(self, i):
        """[(scene index, local t, weight)] and a brightness factor for frame i."""
        a = self.cut_at[1]
        if i < a:
            return [(0, i, 1.0)], 1.0
        d0, d1 = self.dissolve
        if i < d0:
            return [(1, i - a, 1.0)], 1.0
        if i < d1:
            al = (i - d0 + 1) / (d1 - d0 + 1)
            return [(1, i - a, 1.0 - al), (2, i - d0, al)], 1.0
        c2 = self.cut_at[2]
        if i < c2:
            return [(2, i - d0, 1.0)], 1.0
        if i < self.dip_start:
            return [(3, i - c2, 1.0)], 1.0
        k = i - self.dip_start
        if k < self.fo:
            return [(3, i - c2, 1.0)], 1.0 - (k + 1) / (self.fo + 1)
        if k < self.fo + self.bl:
            return [], 0.0
        if k < self.fo + self.bl + self.fi:
            j = k - self.fo - self.bl
            return [(4, j, 1.0)], (j + 1) / (self.fi + 1)
        return [(4, k - self.fo - self.bl, 1.0)], 1.0

    def render(self, i):
        parts, bright = self.where(i)
        rgb = np.zeros((self.h, self.w, 3), np.float32)
        tr = np.full((self.h, self.w), BLACK_TRUTH, np.float32)
        if parts:
            tr[:] = 0
            for sc, t, wgt in parts:
                c, d = self.scenes[sc].render(t)
                rgb += c * np.float32(wgt)
                tr += d * np.float32(wgt)
            rgb *= np.float32(bright)
        if i in self.flash:
            rgb[:] = 250.0
            tr[:] = FLASH_WHITE
        return rgb, tr

    def frame(self, i):
        rgb, _tr = self.render(i)
        rng = np.random.default_rng((self.seed, i))
        f = np.clip(rgb + rng.normal(0, self.noise, rgb.shape).astype(np.float32) + 0.5, 0, 255).astype(np.uint8)
        f[0, 0] = (i >> 8, i & 255, 77)
        f[0, -1] = (0, 0, 0)
        return f

    def truth(self, i):
        return self.render(i)[1]

    def frames(self):
        for i in range(self.n):
            yield self.frame(i)

    def disc_edge(self, i):
        """(row y, x of the left edge, x of the right edge, vx) of the first disc in frame i, in px, or None when the
        frame is not a single scene."""
        parts, bright = self.where(i)
        if len(parts) != 1 or bright < 1.0 or i in self.flash:
            return None
        sc, t, _w = parts[0]
        S = self.scenes[sc]
        cx, cy, r = S.disc_at(0, t)
        return cy, cx - r, cx + r, S.discs[0][2]


def frame_id(f):
    """(index, mirrored) from the marker pixels."""
    if f[0, 0, 2] == 77:
        return int(f[0, 0, 0]) * 256 + int(f[0, 0, 1]), False
    if f[0, -1, 2] == 77:
        return int(f[0, -1, 0]) * 256 + int(f[0, -1, 1]), True
    raise ValueError('not a synth frame')


def gauss_blur(a, sigma):
    if sigma <= 0:
        return a
    r = int(3 * sigma + 0.5)
    k = np.exp(-0.5 * (np.arange(-r, r + 1) / sigma) ** 2).astype(np.float32)
    k /= k.sum()
    p = np.pad(a, r, mode='edge')
    h, w = a.shape
    t = np.zeros((h + 2 * r, w), np.float32)
    for j in range(2 * r + 1):
        t += k[j] * p[:, j:j + w]
    o = np.zeros((h, w), np.float32)
    for j in range(2 * r + 1):
        o += k[j] * t[j:j + h]
    return o


class StubModel:
    """See the module docstring. calls counts model calls (windows); out=(w, h) shrinks the output by area (the
    converter 0.1 interface: depth at 320x180)."""

    model_name = 'stub'

    def __init__(self, video, sigma_edge=2.0, scale_jitter=0.3, shift_jitter=0.2, leak=0.15, bias=0.03, noise=0.01,
                 flicker=0.006, seed=5, out=None):
        self.v = video
        self.sigma_edge, self.sj, self.tj, self.leak, self.bias = sigma_edge, scale_jitter, shift_jitter, leak, bias
        self.noise, self.flicker, self.seed, self.out = noise, flicker, seed, out
        self.calls = 0
        self.frames_seen = 0

    def prepare(self, *a):
        pass

    def reset_peak(self):
        pass

    def peak_vram(self):
        return 0

    def __call__(self, frames):
        k = self.calls
        self.calls += 1
        rng = np.random.default_rng((self.seed, k))
        s = 1.0 + self.sj * float(rng.uniform(-1, 1))
        t = self.tj * float(rng.uniform(-1, 1))
        beta = self.bias * (1.0 + 0.5 * float(rng.normal()))   # a fixed left/right bias (flip averaging cancels it)
        h, w = self.v.h, self.v.w
        base = []
        for f in frames:
            i, mir = frame_id(f)
            d = self.v.truth(i)
            if mir:                         # the model sees the mirrored picture: its bias is mirrored too
                d = d[:, ::-1]
            base.append(gauss_blur(np.ascontiguousarray(d), self.sigma_edge))
        means = np.array([float(b.mean()) for b in base])
        wmean = float(means.mean())
        ramp = (np.arange(w, dtype=np.float32) / w - 0.5)[None, :] * np.float32(beta)
        out = []
        for b, m in zip(base, means):
            low = rng.normal(0, self.flicker, (6, 9)).astype(np.float32)
            fl = np.kron(low, np.ones((h // 6 + 1, w // 9 + 1), np.float32))[:h, :w]
            d = b + np.float32(self.leak * (wmean - m)) + ramp + fl + rng.normal(0, self.noise, (h, w)).astype(np.float32)
            d = np.maximum(d * np.float32(s) + np.float32(t + 0.5), 0)
            out.append(d)
        self.frames_seen += len(frames)
        o = np.stack(out)
        if self.out:
            import depthproc
            o = np.stack([depthproc.area_resize(x, self.out[0], self.out[1]) for x in o])
        return o
