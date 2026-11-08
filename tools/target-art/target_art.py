#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cuts a 3x3 medallion sprite sheet into the Bushido target art pack (strike the targets).

The pack is local art: nothing it makes belongs in the repository or the mod's zip (the project's own pack is a separate,
optional download; docs/design-notes.md, "Bushido target art (local pack)", has the sheet prompt and the steps).

usage:
    python target_art.py SHEET.png OUT_DIR [--size 1024]
    python target_art.py --arrow ARROW.png OUT_DIR       (the optional arrow.png: one image of a glowing arrow pointing UP
                                                          on pure black; written 1024 x 1024 as OUT_DIR/arrow.png)

then copy OUT_DIR's eight PNGs to <the mod folder>\\art\\targets\\ (the mod's own build never touches that folder).

The sheet: 3 x 3 cells on pure black, straight-on, no text (row-major):
    1 the frame (no glow, an empty black centre)   2 the same frame with a white glow channel   3 a soft white halo ring
    4 a white / grey swirling energy disc           5 a second swirl (unused)                    6 the back face
    7 a seamless side strip of the rim             8 a white burst                              9 white sparks

Writes RGBA PNGs at --size (the mod expects 1024): frame, glow (white; alpha = the glow's light), halo, swirl, back, burst,
sparks (size x size) and edge (size x its own aspect). The metal pieces (frame, back) get a hard circular mask measured from the
image (the frame's outer edge and hole); the light pieces (glow, halo, burst, sparks) are white with alpha from their
brightness, so the mod can tint them; the swirl keeps its own grey inside an opaque disc. Prints the measured radii: the mod
assumes the frame's hole at 0.465 and its edge at 0.960 of the half-size, the swirl's disc at 0.925.

Needs Python 3 with numpy and Pillow.
"""
import argparse
import os
import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None


def cells(a):
    """The nine cells of the sheet, row-major."""
    h, w = a.shape[:2]
    return [a[r * h // 3:(r + 1) * h // 3, c * w // 3:(c + 1) * w // 3] for r in range(3) for c in range(3)]


def bbox(c, thr=14):
    """The lit part of a cell (x0, x1, y0, y1)."""
    L = c.mean(2)
    ys, xs = np.where(L > thr)
    return xs.min(), xs.max() + 1, ys.min(), ys.max() + 1


def square_crop(c, box, pad=0.02):
    """A square around the box (centred), padded, black outside the cell."""
    x0, x1, y0, y1 = box
    cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
    s = max(x1 - x0, y1 - y0) * (1 + 2 * pad) / 2
    X0, Y0 = int(round(cx - s)), int(round(cy - s))
    n = int(round(2 * s))
    out = np.zeros((n, n, 3), np.float32)
    sx0, sy0 = max(X0, 0), max(Y0, 0)
    sx1, sy1 = min(X0 + n, c.shape[1]), min(Y0 + n, c.shape[0])
    out[sy0 - Y0:sy1 - Y0, sx0 - X0:sx1 - X0] = c[sy0:sy1, sx0:sx1]
    return out


def resize(a, n):
    """Each channel resized to n x n (Lanczos, float)."""
    return np.stack([np.asarray(Image.fromarray(a[..., k].astype(np.float32), "F").resize((n, n), Image.LANCZOS)) for k in range(a.shape[2])], 2)


def radial(n):
    """Each pixel's distance from the centre, in half-sizes."""
    yy, xx = np.mgrid[0:n, 0:n]
    c = (n - 1) / 2
    return np.hypot(yy - c, xx - c) / (n / 2)


def ring_radii(img):
    """A ring's or disc's hole and outer edge, from the radial profile of brightness."""
    r = radial(img.shape[0])
    L = img.mean(2)
    bins = np.linspace(0, 1.05, 211)
    idx = np.digitize(r, bins)
    prof = np.array([L[idx == i].mean() if (idx == i).any() else 0 for i in range(1, len(bins))])
    ks = np.where(prof > 10)[0]
    return bins[ks.min()], bins[ks.max() + 1]


def soft_mask(n, inner, outer, feather=0.006):
    """1 between inner and outer (no hole when inner is ~0), feathered over a few pixels."""
    r = radial(n)
    m = np.clip((outer - r) / feather + 0.5, 0, 1)
    if inner > 0.02:
        m *= np.clip((r - inner) / feather + 0.5, 0, 1)
    return m


def light_rgba(img):
    """White light on black: white RGB, alpha = the brightness (the max channel)."""
    a = np.clip(img.max(2) / 255.0, 0, 1)
    return np.dstack([np.ones(img.shape[:2] + (3,)), a])


def save(path, rgba):
    Image.fromarray((np.clip(rgba, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").save(path, optimize=True)


def arrow(src, out, n):
    """The optional arrow: one image of a glowing arrow pointing up on black (a metal outline, a white glowing fill) -> a square
    on its content's box with a 4 % margin, Lanczos to n x n, alpha = the brightness (max channel) and RGB = the colour / alpha
    (so the metal outline keeps its grey); the mod multiplies it by the symbol's colour and turns it to each cut direction."""
    a = np.asarray(Image.open(src).convert("RGB")).astype(np.float32)
    img = np.clip(resize(square_crop(a, bbox(a), 0.04), n), 0, 255)
    al = np.clip(img.max(2) / 255.0, 0, 1)
    rgb = np.where(al[..., None] > 1e-4, np.clip(img / 255.0 / np.maximum(al[..., None], 1e-4), 0, 1), 1.0)
    save(f"{out}/arrow.png", np.dstack([rgb, al]))
    ys, xs = np.where(img.max(2) > 128)
    up = "up" if ys.size and ys.mean() < n / 2 else "NOT up? (its bright part is in the lower half: check the image points up)"
    print(f"arrow {n} x {n}, its bright part {xs.min() if xs.size else 0}..{xs.max() if xs.size else 0} x {ys.min() if ys.size else 0}..{ys.max() if ys.size else 0}: {up}")


def main():
    ap = argparse.ArgumentParser(description="Cut a 3x3 medallion sheet into the Bushido target art pack (or, with --arrow, one arrow image).")
    ap.add_argument("sheet", help="the 3x3 sheet (PNG), or with --arrow the arrow image")
    ap.add_argument("out", help="the folder for the PNGs")
    ap.add_argument("--size", type=int, default=1024, help="the pieces' side (the mod expects 1024)")
    ap.add_argument("--arrow", action="store_true", help="cut the optional arrow.png from one image of an arrow pointing up")
    o = ap.parse_args()
    n = o.size
    os.makedirs(o.out, exist_ok=True)
    if o.arrow:
        arrow(o.sheet, o.out, n)
        return
    a = np.asarray(Image.open(o.sheet).convert("RGB")).astype(np.float32)
    C = cells(a)
    rep = []
    # 1 + 2, the frames: cropped with the same box (their union) so they stay aligned; the glow = frame 2 - frame 1
    b1, b2 = bbox(C[0]), bbox(C[1])
    ub = (min(b1[0], b2[0]), max(b1[1], b2[1]), min(b1[2], b2[2]), max(b1[3], b2[3]))
    f1, f2 = resize(square_crop(C[0], ub), n), resize(square_crop(C[1], ub), n)
    best = (1e18, 0, 0)   # fine alignment: the whole-pixel shift of frame 2 that best matches frame 1's metal (+-6 px)
    g1 = f1.mean(2)
    for dy in range(-6, 7):
        for dx in range(-6, 7):
            g2 = np.roll(np.roll(f2.mean(2), dy, 0), dx, 1)
            d = np.abs(np.minimum(g1, 120) - np.minimum(g2, 120)).mean()
            if d < best[0]:
                best = (d, dy, dx)
    f2 = np.roll(np.roll(f2, best[1], 0), best[2], 1)
    rep.append(f"frame align shift dy {best[1]} dx {best[2]} (mean diff {best[0]:.2f})")
    inner, outer = ring_radii(f1)
    m = soft_mask(n, inner, outer)
    rep.append(f"frame hole {inner:.3f}, outer {outer:.3f} (of the half-size; the mod assumes 0.465 / 0.960)")
    save(f"{o.out}/frame.png", np.dstack([f1 / 255.0, m]))
    glow = np.clip(f2 - f1, 0, 255)
    glow[glow.max(2) < 18] = 0
    g = light_rgba(glow)
    g[..., 3] *= m
    save(f"{o.out}/glow.png", g)
    # 3 halo, 4 swirl, 8 burst, 9 sparks: light pieces
    for k, name in ((2, "halo"), (3, "swirl"), (7, "burst"), (8, "sparks")):
        img = resize(square_crop(C[k], bbox(C[k], 6), 0.04), n)
        if name == "swirl":   # an opaque disc in its own grey (the mod tints it)
            _, o2 = ring_radii(img)
            L = np.dstack([np.clip(img / 255.0, 0, 1), soft_mask(n, 0, o2, 0.01)])
            rep.append(f"swirl outer {o2:.3f} (the mod assumes it opaque inside 0.925)")
        else:
            L = light_rgba(img)
        save(f"{o.out}/{name}.png", L)
    # 6 the back face: an opaque disc
    bk = resize(square_crop(C[5], bbox(C[5])), n)
    _, ob = ring_radii(bk)
    save(f"{o.out}/back.png", np.dstack([bk / 255.0, soft_mask(n, 0, ob)]))
    # 7 the edge strip: its own box, at its aspect, n wide
    x0, x1, y0, y1 = bbox(C[6])
    e = C[6][y0:y1, x0:x1]
    eh = max(8, int(round(n * (y1 - y0) / (x1 - x0))))
    E = np.stack([np.asarray(Image.fromarray(e[..., k], "F").resize((n, eh), Image.LANCZOS)) for k in range(3)], 2)
    save(f"{o.out}/edge.png", np.dstack([np.clip(E / 255.0, 0, 1), np.ones(E.shape[:2])]))
    rep.append(f"edge {n} x {eh} (the mod accepts 1024 x 32..512)")
    print("\n".join(rep))


if __name__ == "__main__":
    main()
