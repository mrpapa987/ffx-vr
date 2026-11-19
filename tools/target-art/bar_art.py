#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cuts the overdrive timing bar sprite sheet into the mod's bar art pack (Tidus's Swordplay bar, Lulu's Fury countdown).

The pack is local art, like the Bushido target art: nothing this makes belongs in the repository or the mod's zip (the
project's own pack is a separate, optional download; docs/design-notes.md, "Overdrive bar art (local pack)").

usage:
    python bar_art.py SHEET OUT_DIR [--no-trim] [--all] [--force]

    SHEET    the sheet (JPEG or PNG; pieces on pure black, the layout of the table below)
    OUT_DIR  the folder for the PNGs; the mod loads <the mod folder>\\art\\odbar\\ (for example
             <game>\\fahrenheit\\mods\\ffxvr\\art\\odbar), so that folder can be given directly
    --no-trim  cut each piece at its box in the table (default: trimmed to its visible content, with a 4 px margin)
    --all      also write the five pieces the mod does not use (the arrow cursors, the two studs, the corner cap), as-is
    --force    write the pack even when a measured proportion is outside what the mod assumes (below; it would be misdrawn)
OUT_DIR must not be inside this repository's work tree (the art never belongs in it).

Writes seven RGBA PNGs at the exact sizes the mod packs into its 1024 x 1024 atlas (it refuses any other size):
    frame.png        1024 x 112  bar_frame_outer
    track_fill.png   1024 x 96   bar_fill_track's lit part: its round left cap, the body, the bright leading edge
    track_empty.png  1024 x 96   bar_fill_track's dark part, made full length (its right end mirrored on the left)
    plate.png        1024 x 160  plate_ornate_base
    zone.png          512 x 224  target_zone_hit: its two end bars at x 24 and 488, its glow outside them
    cursor_down.png   256 x 208  cursor_tri_down (the tip at the bottom)
    cursor_up.png     256 x 208  cursor_tri_up (the tip at the top)

How each piece gets its alpha (the sheet has none; the pieces sit on black). Two numbers per piece: a black floor f (JPEG
noise below it is cut) and an opaque level t, on the brightness L (the brightest channel, 0..1):
    alpha = clamp((L - f) / (t - f), 0, 1), colour = the pixel / alpha (clamped), so that the piece drawn over black is
    the sheet again (premultiplied, it is the pixel itself).
  - a key (t low): the metal (frame, plate) and the cursors: everything brighter than t is opaque, the dark interior and
    the background go transparent, the glows and the anti-aliased edges fade over f..t;
  - light (t = 1): the zone and the track: alpha follows the brightness, so the glow stays soft (the mod draws them blended
    normally, over a dark backing behind the track, the look of the sheet's black).
Each piece is also masked to its own connected content (the brightness above f, joined across gaps under 9 px), so a
neighbour's glow inside its box (the plate's box holds part of cursor_tri_up's) is left out.

The mod assumes the proportions measured on the owner's sheet (ffxvr_odbar.h): the frame's opening, the cursors' tips, the
zone's golden body. The script measures them on what it cut and, when one is outside the mod's tolerance (below), says so and
writes nothing (exit 1) unless --force: such a pack would be drawn with the zone and the cursors off the opening.

Memory: the sheet is held once as 8-bit RGB (3 bytes a pixel); the content is labelled on a reduced copy (a sheet larger than
the owner's 2576 px is pooled to about that width first) and counted in strips, and each piece is cut, masked and shrunk to at most
4x its cell before it becomes floating point. A 4x upscale (about 59 MP) needs well under 1.5 GB.

Needs Python 3 with numpy and Pillow (as target_art.py).
"""
import argparse
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

Image.MAX_IMAGE_PIXELS = 150_000_000   # Pillow's decompression-bomb guard, raised to a sane cap (review N4): a 4x upscale of a 4K sheet is about 130 MP

# The atlas of the owner's sheet: (left, top, right, bottom) in % of the sheet, from its own table (docs/design-notes.md).
BOXES = {
    "bar_frame_outer":   (3.8, 4.2, 96.0, 23.5),
    "bar_fill_track":    (8.2, 27.8, 91.6, 40.0),
    "target_zone_hit":   (7.8, 44.5, 37.0, 66.8),
    "cursor_tri_down":   (68.5, 46.0, 79.8, 61.5),
    "cursor_tri_up":     (68.5, 64.2, 79.8, 80.2),
    "cursor_arrow_up":   (83.0, 46.0, 93.2, 63.8),
    "cursor_arrow_down": (83.0, 64.0, 93.2, 81.5),
    "plate_ornate_base": (2.8, 75.5, 78.4, 96.2),
    "rivet_smooth":      (79.2, 87.5, 82.5, 93.5),
    "rivet_filigree":    (84.4, 87.5, 87.8, 93.5),
    "corner_bracket_r":  (89.5, 84.0, 96.2, 96.0),
}
# The cells (the mod's ffxvr_odbar.h odb::cell: the same sizes) and the composition's fixed columns
CAP_PX, EDGE_PX = 48, 40           # the track pieces' round cap, the fill's leading edge (cell px)
ZONE_M_PX = 24                     # the zone's glow margin outside its end bars (cell px)
# each piece's alpha: (black floor f, opaque level t), on 0..255
ALPHA = {
    "frame": (12, 60), "plate": (12, 55), "cursor_down": (12, 110), "cursor_up": (12, 110),
    "zone": (8, 255), "track_fill": (8, 200), "track_empty": (8, 200),
    "cursor_arrow_up": (12, 110), "cursor_arrow_down": (12, 110), "rivet_smooth": (12, 60), "rivet_filigree": (12, 60), "corner_bracket_r": (12, 60),
}
FLOOR = 10      # the content mask's brightness floor (0..255)
GAP = 9         # gaps under this (px, at the owner's sheet's scale) join a piece's content
REF_W = 2576    # the owner's sheet's width: a wider sheet is pooled to about this for the labelling
CELL_W = {"bar_frame_outer": 1024, "bar_fill_track": 1024, "target_zone_hit": 512, "cursor_tri_down": 256, "cursor_tri_up": 256, "plate_ornate_base": 1024}
# what the mod assumes (ffxvr_odbar.h k_open_*, k_tip_*, k_zone_h's body) and the tolerance it draws them right within
ASSUME = {"opening": ((0.0488, 0.9512, 0.2857, 0.7143), 0.02), "tip_down": (0.933, 0.05), "tip_up": (0.029, 0.05), "zone_body": ((0.223, 0.786), 0.05)}


def to_px(box, w, h):
    x0, y0, x1, y1 = box
    return int(round(x0 / 100 * w)), int(round(y0 / 100 * h)), int(round(x1 / 100 * w)), int(round(y1 / 100 * h))


def components(L):
    """The content's connected parts, labelled on a 4:1 grid of L: the grid's labels (0 = background), their count, and the
    mask. The mask is the brightness above FLOOR, widened by GAP so sparks and a glow's soft gaps join their piece; a grid
    cell is in it when any of its 16 pixels is; the smallest index spreads to the 8 neighbours until nothing changes."""
    m = np.asarray(Image.fromarray(((L > FLOOR) * 255).astype(np.uint8)).filter(ImageFilter.MaxFilter(GAP))) > 0
    h, w = m.shape
    gh, gw = (h + 3) // 4, (w + 3) // 4
    g = np.zeros((gh * 4, gw * 4), bool)
    g[:h, :w] = m
    g = g.reshape(gh, 4, gw, 4).any(axis=(1, 3))
    big = np.int64(gh * gw + 1)
    lab = np.where(g, np.arange(1, gh * gw + 1, dtype=np.int64).reshape(gh, gw), big)
    while True:
        p = np.pad(lab, 1, constant_values=big)
        nb = lab.copy()
        for dy in (0, 1, 2):
            for dx in (0, 1, 2):
                nb = np.minimum(nb, p[dy:dy + gh, dx:dx + gw])
        nb = np.where(g, nb, big)
        if np.array_equal(nb, lab):
            break
        lab = nb
    ids, inv = np.unique(np.where(g, lab, 0), return_inverse=True)
    small = inv.reshape(gh, gw).astype(np.int32)   # 0 stays the background (the smallest value)
    if ids[0] != 0:
        small += 1
    return small, int(small.max()), m


class Labels:
    """The labels at the sheet's own size, a region at a time (never a full-size label image): the grid's label of each pixel
    (the pooled sheet's 4:1 grid, f sheet pixels a pooled one), where the pooled sheet's widened mask holds (at 1:1 exactly
    the sheet's own mask; one byte a pooled pixel)."""
    def __init__(self, L, f):
        self.L, self.f = L, f
        H, W = L.shape
        if f > 1:   # the pooled sheet: the brightest pixel of each f x f block
            Hp, Wp = (H + f - 1) // f, (W + f - 1) // f
            P = np.zeros((Hp * f, Wp * f), np.uint8)
            P[:H, :W] = L
            P = P.reshape(Hp, f, Wp, f).max(axis=(1, 3))
        else:
            P = L
        self.small, self.n, self.m = components(P)

    def region(self, x0, y0, x1, y1):
        """The labels of the pixels x0..x1 x y0..y1 (int32)."""
        f = self.f
        if f == 1:
            m = self.m[y0:y1, x0:x1]
        else:
            m = self.m[np.arange(y0, y1) // f][:, np.arange(x0, x1) // f]
        ys = np.arange(y0, y1) // (4 * f)
        xs = np.arange(x0, x1) // (4 * f)
        lab = self.small[np.minimum(ys, self.small.shape[0] - 1)][:, np.minimum(xs, self.small.shape[1] - 1)]
        return np.where(m, lab, 0).astype(np.int32)


def assign(lb, boxes):
    """Each part's owner: the box holding most of its pixels (a part outside every box: the box its centre is within 40 px of,
    else none: dropped), counted over the sheet in strips of rows. Returns the owner of each label as an index into the
    boxes' order (-1 none; label 0 the background) and each label's pixel bounding box (x0, y0, x1, y1)."""
    names = list(boxes)
    n = lb.n
    H, W = lb.L.shape
    cnt = np.zeros((len(names), n + 1), np.int64)
    tot = np.zeros(n + 1, np.int64); sx = np.zeros(n + 1); sy = np.zeros(n + 1)
    bb = np.array([[W, H, -1, -1]] * (n + 1), np.int64)
    step = 256
    for y0 in range(0, H, step):
        y1 = min(y0 + step, H)
        lab = lb.region(0, y0, W, y1)
        ys, xs = np.nonzero(lab)
        if ys.size == 0:
            continue
        v = lab[ys, xs]
        ys = ys + y0
        tot += np.bincount(v, minlength=n + 1); sx += np.bincount(v, xs, minlength=n + 1); sy += np.bincount(v, ys, minlength=n + 1)
        np.minimum.at(bb[:, 0], v, xs); np.minimum.at(bb[:, 1], v, ys); np.maximum.at(bb[:, 2], v, xs + 1); np.maximum.at(bb[:, 3], v, ys + 1)
        for i, name in enumerate(names):
            x0, b0, x1, b1 = boxes[name]
            sel = (xs >= x0) & (xs < x1) & (ys >= b0) & (ys < b1)
            if sel.any():
                cnt[i] += np.bincount(v[sel], minlength=n + 1)
    own = np.full(n + 1, -1, np.int32)
    for k in range(1, n + 1):
        if tot[k] == 0:
            continue
        best, bi = 0, -1
        for i in range(len(names)):   # (the first box with the most pixels, as before)
            if cnt[i, k] > best:
                best, bi = int(cnt[i, k]), i
        if bi < 0:
            cx, cy = sx[k] / tot[k], sy[k] / tot[k]
            for i, name in enumerate(names):
                x0, y0, x1, y1 = boxes[name]
                if x0 - 40 <= cx < x1 + 40 and y0 - 40 <= cy < y1 + 40:
                    bi = i
                    break
        own[k] = bi
    return own, bb


def piece_mask(lb, own, bb, boxes, me):
    """The pixels of piece `me` (an index into the boxes' order), in its region (its box and its own parts' boxes): its own
    parts' pixels but those outside its box and inside another's, and the pixels inside its box of a part owned by another box
    that are outside that box (two pieces whose glows touch are split at their boxes; where boxes overlap, the part's owner
    keeps it). Returns the mask and the region (x0, y0, x1, y1)."""
    names = list(boxes)
    bx0, by0, bx1, by1 = boxes[names[me]]
    mine = np.nonzero(own == me)[0]
    rx0, ry0, rx1, ry1 = bx0, by0, bx1, by1
    for k in mine:
        if bb[k, 2] > bb[k, 0]:
            rx0, ry0, rx1, ry1 = min(rx0, int(bb[k, 0])), min(ry0, int(bb[k, 1])), max(rx1, int(bb[k, 2])), max(ry1, int(bb[k, 3]))
    lab = lb.region(rx0, ry0, rx1, ry1)
    o = np.where(lab > 0, own[lab], -2)
    def local(b):   # a sheet box as a boolean mask of the region
        z = np.zeros(lab.shape, bool)
        x0, y0, x1, y1 = b
        z[max(y0 - ry0, 0):max(y1 - ry0, 0), max(x0 - rx0, 0):max(x1 - rx0, 0)] = True
        return z
    inside = local(boxes[names[me]])
    other = np.zeros(lab.shape, bool)
    in_owner = np.zeros(lab.shape, bool)
    for i, k in enumerate(names):
        if i == me:
            continue
        li = local(boxes[k])
        other |= li
        in_owner |= (o == i) & li
    return (lab > 0) & (((o == me) & (inside | ~other)) | (inside & (o != me) & ~in_owner)), (rx0, ry0, rx1, ry1)


def cut(a, mine, region, box, trim, cell_w):
    """The piece: its crop (trimmed to its own pixels with a 4 px margin, or the box), its pixels not its own set to black (the
    sheet's background), shrunk by a whole factor to at most 4x its cell's width (box filter), as float; its mask; the crop."""
    rx0, ry0, rx1, ry1 = region
    H, W = a.shape[:2]
    if trim:
        L = a[ry0:ry1, rx0:rx1].max(2)
        ys, xs = np.nonzero(mine & (L > FLOOR))
        x0, x1, y0, y1 = xs.min() - 4 + rx0, xs.max() + 5 + rx0, ys.min() - 4 + ry0, ys.max() + 5 + ry0
    else:
        x0, y0, x1, y1 = box
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, W), min(y1, H)
    img = a[y0:y1, x0:x1].copy()
    m = np.zeros((y1 - y0, x1 - x0), bool)   # the mask over the crop (its part outside the region is not the piece's)
    sy0, sx0 = max(y0, ry0), max(x0, rx0)
    sy1, sx1 = min(y1, ry1), min(x1, rx1)
    if sy1 > sy0 and sx1 > sx0:
        m[sy0 - y0:sy1 - y0, sx0 - x0:sx1 - x0] = mine[sy0 - ry0:sy1 - ry0, sx0 - rx0:sx1 - rx0]
    img[~m] = 0
    k = max(1, img.shape[1] // (2 * cell_w))
    if k > 1:
        img = np.asarray(Image.fromarray(img, "RGB").reduce(k))
        m = np.asarray(Image.fromarray((m * 255).astype(np.uint8), "L").reduce(k)) > 0
    return img.astype(np.float32), m, (x0, y0, x1, y1)


def rgba(img, mask, f, t, solid=0):
    """Straight-alpha RGBA (0..1) of a piece on black: alpha = clamp((L - f) / (t - f)), colour = pixel / alpha. solid: the
    metal's dark grooves made opaque too: k > 0 closes gaps under k px inside the opaque part (the frame, whose opening must
    stay open); -1 makes everything inside the outline opaque (the plate: only what the sheet's black reaches from outside
    stays transparent)."""
    L = img.max(2)
    al = np.clip((L - f) / float(t - f), 0, 1) * mask
    if solid:
        on = Image.fromarray(((al > 0.5) * 255).astype(np.uint8))
        if solid > 0:
            fill = np.asarray(on.filter(ImageFilter.MaxFilter(solid)).filter(ImageFilter.MinFilter(solid))) > 0
        else:   # the outside: the transparent pixels joined to the border (flood from a 1 px frame around the piece)
            pad = Image.new("L", (on.width + 2, on.height + 2), 0)
            pad.paste(on.filter(ImageFilter.MaxFilter(3)).filter(ImageFilter.MinFilter(3)), (1, 1))
            ImageDraw.floodfill(pad, (0, 0), 128)
            fill = np.asarray(pad)[1:-1, 1:-1] != 128
        al = np.maximum(al, fill * mask * 1.0)
    rgb = np.where(al[..., None] > 1e-4, np.clip(img / 255.0 / np.maximum(al[..., None], 1e-4), 0, 1), 0.0)
    return np.dstack([rgb, al])


def resize(x, w, h):
    """Premultiplied resize (Lanczos) of straight RGBA, back to straight: no dark or light fringes at soft edges."""
    if x.shape[1] == w and x.shape[0] == h:
        return x
    p = np.dstack([x[..., :3] * x[..., 3:4], x[..., 3]])
    ch = [np.asarray(Image.fromarray(p[..., k].astype(np.float32), "F").resize((w, h), Image.LANCZOS)) for k in range(4)]
    al = np.clip(ch[3], 0, 1)
    rgb = np.stack([np.clip(ch[k], 0, None) for k in range(3)], 2)
    rgb = np.where(al[..., None] > 1e-4, np.clip(rgb / np.maximum(al[..., None], 1e-4), 0, 1), 0.0)
    return np.dstack([rgb, al])


def fit(x, w, h):
    """x stretched to exactly w x h (the cells' aspects are the sheet's pieces', within a few %)."""
    return resize(x, w, h)


def hcat(parts):
    return np.concatenate(parts, 1)


def save(path, x):
    Image.fromarray((np.clip(x, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA").save(path, optimize=True)


def track_split(img):
    """The track's lit / dark boundary: the column where the middle rows' brightness falls by a third within 24 px."""
    h = img.shape[0]
    prof = img[h // 3:2 * h // 3].max(2).mean(0)
    best, bx = 0.0, None
    for x in range(img.shape[1] // 3, img.shape[1] - h):
        d = prof[x - 12] - prof[x + 12]
        if d > best:
            best, bx = d, x
    if bx is None or best < prof.max() / 3:
        return None
    return bx


def opening(fr):
    """The frame's opening (x0, x1, y0, y1, of the cell): the transparent run around the centre row and column."""
    h, w = fr.shape[:2]
    a = fr[..., 3]
    cy, cx = h // 2, w // 2
    row, col = a[cy], a[:, cx]
    x0 = cx
    while x0 > 0 and row[x0 - 1] < 0.5:
        x0 -= 1
    x1 = cx
    while x1 < w - 1 and row[x1 + 1] < 0.5:
        x1 += 1
    y0 = cy
    while y0 > 0 and col[y0 - 1] < 0.5:
        y0 -= 1
    y1 = cy
    while y1 < h - 1 and col[y1 + 1] < 0.5:
        y1 += 1
    return x0 / w, (x1 + 1) / w, y0 / h, (y1 + 1) / h


def tip(c, down):
    """A cursor's tip row (of its cell height): the lowest (down) or highest (up) row whose alpha passes 0.5 at the centre column band."""
    h, w = c.shape[:2]
    a = c[:, w // 2 - 3:w // 2 + 4, 3].max(1)
    rows = np.nonzero(a > 0.5)[0]
    if rows.size == 0:
        return float("nan")
    return (rows.max() + 1) / h if down else rows.min() / h


def inside_repo(path):
    """The work tree this script is in (its .git two folders up), when `path` is inside it, else None (review N5)."""
    root = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    if not os.path.exists(os.path.join(root, ".git")):
        return None
    p = os.path.realpath(path)
    return root if p == root or p.startswith(root + os.sep) else None


def main():
    ap = argparse.ArgumentParser(description="Cut the overdrive timing bar sheet into the mod's bar art pack (art\\odbar).")
    ap.add_argument("sheet", help="the sheet (JPEG or PNG)")
    ap.add_argument("out", help="the folder for the PNGs (the mod loads <mod>\\art\\odbar); not inside this repository")
    ap.add_argument("--no-trim", action="store_true", help="cut each piece at its table box instead of its visible content")
    ap.add_argument("--all", action="store_true", help="also write the five pieces the mod does not use")
    ap.add_argument("--force", action="store_true", help="write the pack even when a measured proportion is outside what the mod assumes")
    o = ap.parse_args()
    root = inside_repo(o.out)
    if root:
        raise SystemExit(f"refused: {o.out} is inside the repository ({root}); the art never goes in it. Give the mod's art\\odbar folder, or a folder outside the repository.")
    im = Image.open(o.sheet).convert("RGB")
    a = np.asarray(im)   # 8-bit RGB, held once
    del im
    H, W = a.shape[:2]
    boxes = {k: to_px(b, W, H) for k, b in BOXES.items()}
    f = max(1, int(round(W / REF_W)))
    lb = Labels(a.max(2), f)
    owner, bb = assign(lb, boxes)
    rep = [f"sheet {W} x {H}: {lb.n} content parts" + (f" (labelled at 1:{f})" if f > 1 else "")]
    piece = {}
    for i, (name, box) in enumerate(boxes.items()):
        mine, region = piece_mask(lb, owner, bb, boxes, i)
        if not (mine & (a[region[1]:region[3], region[0]:region[2]].max(2) > FLOOR)).any():
            raise SystemExit(f"{name}: nothing found in its box {box}: is this the bar sheet?")
        img, m, crop = cut(a, mine, region, box, not o.no_trim, CELL_W.get(name, 1024))
        del mine
        piece[name] = (img, m)
        clip = [s for s, v in (("left", crop[0] < box[0]), ("top", crop[1] < box[1]), ("right", crop[2] > box[2]), ("bottom", crop[3] > box[3])) if v]
        rep.append(f"{name}: cut {crop[0]}..{crop[2]} x {crop[1]}..{crop[3]}" + (f" (past its table box on the {', '.join(clip)}: the box clipped it)" if clip else ""))
    del a, lb
    out = {}   # file -> RGBA; written at the end, when the proportions are right (or --force)
    bad = []
    def within(v, want, tol): return abs(v - want) <= tol
    # frame
    img, m = piece["bar_frame_outer"]
    fr = fit(rgba(img, m, *ALPHA["frame"], solid=7), 1024, 112)
    out["frame.png"] = fr
    ox = opening(fr)
    want, tol = ASSUME["opening"]
    rep.append(f"frame opening x {ox[0]:.4f}..{ox[1]:.4f}, y {ox[2]:.4f}..{ox[3]:.4f} (the mod assumes {want[0]:.4f}..{want[1]:.4f}, {want[2]:.4f}..{want[3]:.4f} within {tol})")
    if not all(within(ox[j], want[j], tol) for j in range(4)):
        bad.append("the frame's opening")
    # the track: the lit part (left cap, body, leading edge) and the dark part made full length
    img, m = piece["bar_fill_track"]
    bx = track_split(img)
    th = img.shape[0]
    if bx is None:   # one look only: both pieces from the whole track
        rep.append("track: no lit / dark boundary found: track_fill and track_empty are the same (the countdown shows only its length)")
        lit, dark = rgba(img, m, *ALPHA["track_fill"]), rgba(img, m, *ALPHA["track_empty"])
    else:
        rep.append(f"track: lit / dark boundary at {bx} of {img.shape[1]} px")
        lit = rgba(img[:, :bx + 3], m[:, :bx + 3], *ALPHA["track_fill"])
        d = rgba(img[:, bx + 30:], m[:, bx + 30:], *ALPHA["track_empty"])
        r = d[:, d.shape[1] // 3:]            # its right two thirds (the round right end; the part next to the lit one is greener)
        dark = hcat([r[:, ::-1], r])          # mirrored: a round cap at both ends, the body's mirror seam in the middle is smooth
    def three(x, cl, cr, w, h, l_px, r_px):
        """x into w x h with its left cl and right cr source columns made exactly l_px and r_px wide (the 3-slice the mod draws)."""
        return hcat([fit(x[:, :cl], l_px, h), fit(x[:, cl:x.shape[1] - cr], w - l_px - r_px, h), fit(x[:, x.shape[1] - cr:], r_px, h)])
    capsrc = int(round(CAP_PX * th / 96.0))
    edgesrc = int(round(EDGE_PX * th / 96.0))
    out["track_fill.png"] = three(lit, capsrc, edgesrc, 1024, 96, CAP_PX, EDGE_PX)
    out["track_empty.png"] = three(dark, capsrc, capsrc, 1024, 96, CAP_PX, CAP_PX)
    # plate
    img, m = piece["plate_ornate_base"]
    out["plate.png"] = fit(rgba(img, m, *ALPHA["plate"], solid=-1), 1024, 160)
    # the zone: its end bars (the brightest columns in each outer fifth) to x 24 and 488, its glow outside them
    img, m = piece["target_zone_hit"]
    z = rgba(img, m, *ALPHA["zone"])
    zw = z.shape[1]
    prof = img.max(2).mean(0)
    lb_ = int(np.argmax(prof[:zw // 5]))
    rb = zw - zw // 5 + int(np.argmax(prof[zw - zw // 5:]))
    rep.append(f"zone end bars at {lb_} and {rb} of {zw} px")
    zone = hcat([fit(z[:, :lb_], ZONE_M_PX, 224), fit(z[:, lb_:rb], 512 - 2 * ZONE_M_PX, 224), fit(z[:, rb:], ZONE_M_PX, 224)])
    out["zone.png"] = zone
    zr = np.nonzero(zone[:, 256, 3] > 0.5)[0]
    want, tol = ASSUME["zone_body"]
    if zr.size:
        z0, z1 = zr.min() / 224, (zr.max() + 1) / 224
        rep.append(f"zone body rows {z0:.3f}..{z1:.3f} of its height at the centre column (the mod assumes {want[0]:.3f}..{want[1]:.3f} within {tol})")
        if not (within(z0, want[0], tol) and within(z1, want[1], tol)):
            bad.append("the zone's body")
    else:
        rep.append("zone: no opaque body at the centre column")
        bad.append("the zone's body")
    # the cursors
    for src, dst, down in (("cursor_tri_down", "cursor_down", True), ("cursor_tri_up", "cursor_up", False)):
        img, m = piece[src]
        c = fit(rgba(img, m, *ALPHA[dst], solid=7), 256, 208)
        out[f"{dst}.png"] = c
        tv = tip(c, down)
        want, tol = ASSUME["tip_down" if down else "tip_up"]
        rep.append(f"{dst} tip at {tv:.3f} of its height (the mod assumes {want:.3f} within {tol})")
        if not within(tv, want, tol):   # (NaN: not within)
            bad.append(f"the {dst} tip")
    if o.all:
        for k in ("cursor_arrow_up", "cursor_arrow_down", "rivet_smooth", "rivet_filigree", "corner_bracket_r"):
            img, m = piece[k]
            out[f"{k}.png"] = rgba(img, m, *ALPHA[k])
        rep.append("--all: the five unused pieces written as cut (the mod does not load them)")
    print("\n".join(rep))
    if bad and not o.force:
        print("ERROR: outside what the mod assumes: " + ", ".join(bad) + ". The mod would draw the zone and the cursors off the opening. "
              "Nothing was written (--force writes it anyway).")
        raise SystemExit(1)
    if bad:
        print("WARNING: outside what the mod assumes: " + ", ".join(bad) + " (written anyway: --force)")
    os.makedirs(o.out, exist_ok=True)
    for name, x in out.items():
        save(os.path.join(o.out, name), x)


if __name__ == "__main__":
    main()
