#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Cuts the overdrive timing bar sprite sheet into the mod's bar art pack (Tidus's Swordplay bar, Lulu's Fury countdown).

The pack is local art, like the Bushido target art: nothing this makes belongs in the repository or the mod's zip (the
project's own pack is a separate, optional download; docs/design-notes.md, "Overdrive bar art (local pack)").

usage:
    python bar_art.py SHEET OUT_DIR [--no-trim] [--all]

    SHEET    the sheet (JPEG or PNG; pieces on pure black, the layout of the table below)
    OUT_DIR  the folder for the PNGs; the mod loads <the mod folder>\\art\\odbar\\ (for example
             <game>\\fahrenheit\\mods\\ffxvr\\art\\odbar), so that folder can be given directly
    --no-trim  cut each piece at its box in the table (default: trimmed to its visible content, with a 4 px margin)
    --all      also write the five pieces the mod does not use (the arrow cursors, the two studs, the corner cap), as-is

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
  - light (t = 1): the zone and the track: alpha follows the brightness, so the glow stays soft and adds light (the mod
    draws the zone additively, over a dark backing behind the track, the look of the sheet's black).
Each piece is also masked to its own connected content (the brightness above f, joined across gaps under 9 px), so a
neighbour's glow inside its box (the plate's box holds part of cursor_tri_up's) is left out.

The mod assumes the measured proportions printed at the end (the frame's opening, the cursors' tips); a sheet of your own
should come close (the bars are drawn at those proportions; a piece of another shape is stretched to its cell).

Needs Python 3 with numpy and Pillow (as target_art.py).
"""
import argparse
import os
import numpy as np
from PIL import Image, ImageDraw, ImageFilter

Image.MAX_IMAGE_PIXELS = None

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
GAP = 9         # gaps under this (px) join a piece's content


def to_px(box, w, h):
    x0, y0, x1, y1 = box
    return int(round(x0 / 100 * w)), int(round(y0 / 100 * h)), int(round(x1 / 100 * w)), int(round(y1 / 100 * h))


def components(L):
    """The content's connected parts: a label image (0 = background) and their count. The mask is the brightness above
    FLOOR, widened by GAP so sparks and a glow's soft gaps join their piece. Labelled on a 4:1 grid (each cell: any of its
    16 pixels in the mask) by spreading the smallest index to the 8 neighbours until nothing changes, then back to full size."""
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
    full = np.repeat(np.repeat(small, 4, 0), 4, 1)[:h, :w]
    return np.where(m, full, 0), int(small.max())


def assign(lab, n, boxes):
    """Each part's owner: the box holding most of it (a part outside every box: the box its centre is within 40 px of, else
    none: dropped). Returns the owner of each label as an index into the boxes' order (-1 none; label 0 the background)."""
    names = list(boxes)
    own = np.full(n + 1, -1, np.int32)
    ys, xs = np.nonzero(lab)
    ls = lab[ys, xs]
    order = np.argsort(ls, kind="stable")
    ys, xs, ls = ys[order], xs[order], ls[order]
    starts = np.searchsorted(ls, np.arange(1, n + 2))
    for k in range(1, n + 1):
        py, px = ys[starts[k - 1]:starts[k]], xs[starts[k - 1]:starts[k]]
        if py.size == 0:
            continue
        best, bi = 0, -1
        for i, name in enumerate(names):
            x0, y0, x1, y1 = boxes[name]
            c = int(((px >= x0) & (px < x1) & (py >= y0) & (py < y1)).sum())
            if c > best:
                best, bi = c, i
        if bi < 0:
            cx, cy = px.mean(), py.mean()
            for i, name in enumerate(names):
                x0, y0, x1, y1 = boxes[name]
                if x0 - 40 <= cx < x1 + 40 and y0 - 40 <= cy < y1 + 40:
                    bi = i
                    break
        own[k] = bi
    return own


def piece_mask(lab, own, boxes, me):
    """The pixels of piece `me` (an index into the boxes' order): its own parts' pixels but those outside its box and inside
    another's, and the pixels inside its box of a part owned by another box that are outside that box (two pieces whose
    glows touch are split at their boxes; where boxes overlap, the part's owner keeps it)."""
    names = list(boxes)
    o = own[lab]
    x0, y0, x1, y1 = boxes[names[me]]
    H, W = lab.shape
    yy, xx = np.mgrid[0:H, 0:W]
    inside = (xx >= x0) & (xx < x1) & (yy >= y0) & (yy < y1)
    bx = np.array([boxes[k] for k in names] + [(0, 0, 0, 0)], np.int64)   # (-1 -> the empty box)
    ob = bx[np.where(o < 0, len(names), o)]
    in_owner = (xx >= ob[..., 0]) & (xx < ob[..., 2]) & (yy >= ob[..., 1]) & (yy < ob[..., 3])
    other = np.zeros(lab.shape, bool)
    for i, k in enumerate(names):
        if i != me:
            a0, b0, a1, b1 = boxes[k]
            other[b0:b1, a0:a1] = True
    return (lab > 0) & (((o == me) & (inside | ~other)) | (inside & (o != me) & ~in_owner))


def cut(a, own, box, trim):
    """The piece: its crop (trimmed to its own pixels with a 4 px margin, or the box) and its mask there."""
    if trim:
        ys, xs = np.nonzero(own & (a.max(2) > FLOOR))
        x0, x1, y0, y1 = xs.min() - 4, xs.max() + 5, ys.min() - 4, ys.max() + 5
    else:
        x0, y0, x1, y1 = box
    x0, y0 = max(x0, 0), max(y0, 0)
    x1, y1 = min(x1, a.shape[1]), min(y1, a.shape[0])
    return a[y0:y1, x0:x1].copy(), own[y0:y1, x0:x1], (x0, y0, x1, y1)


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


def main():
    ap = argparse.ArgumentParser(description="Cut the overdrive timing bar sheet into the mod's bar art pack (art\\odbar).")
    ap.add_argument("sheet", help="the sheet (JPEG or PNG)")
    ap.add_argument("out", help="the folder for the PNGs (the mod loads <mod>\\art\\odbar)")
    ap.add_argument("--no-trim", action="store_true", help="cut each piece at its table box instead of its visible content")
    ap.add_argument("--all", action="store_true", help="also write the five pieces the mod does not use")
    o = ap.parse_args()
    os.makedirs(o.out, exist_ok=True)
    a = np.asarray(Image.open(o.sheet).convert("RGB")).astype(np.float32)
    H, W = a.shape[:2]
    boxes = {k: to_px(b, W, H) for k, b in BOXES.items()}
    lab, n = components(a.max(2))
    owner = assign(lab, n, boxes)
    rep = [f"sheet {W} x {H}: {n} content parts"]
    piece = {}
    for i, (name, box) in enumerate(boxes.items()):
        mine = piece_mask(lab, owner, boxes, i)
        if not (mine & (a.max(2) > FLOOR)).any():
            raise SystemExit(f"{name}: nothing found in its box {box}: is this the bar sheet?")
        img, m, crop = cut(a, mine, box, not o.no_trim)
        piece[name] = (img, m)
        clip = [s for s, v in (("left", crop[0] < box[0]), ("top", crop[1] < box[1]), ("right", crop[2] > box[2]), ("bottom", crop[3] > box[3])) if v]
        rep.append(f"{name}: cut {crop[0]}..{crop[2]} x {crop[1]}..{crop[3]}" + (f" (past its table box on the {', '.join(clip)}: the box clipped it)" if clip else ""))
    # frame
    img, m = piece["bar_frame_outer"]
    fr = fit(rgba(img, m, *ALPHA["frame"], solid=7), 1024, 112)
    save(f"{o.out}/frame.png", fr)
    ox0, ox1, oy0, oy1 = opening(fr)
    rep.append(f"frame opening x {ox0:.4f}..{ox1:.4f}, y {oy0:.4f}..{oy1:.4f} (the mod assumes 0.0488..0.9512, 0.2857..0.7143 within 0.02)")
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
        s = h / x.shape[0]
        return hcat([fit(x[:, :cl], l_px, h), fit(x[:, cl:x.shape[1] - cr], w - l_px - r_px, h), fit(x[:, x.shape[1] - cr:], r_px, h)])
    capsrc = int(round(CAP_PX * th / 96.0))
    edgesrc = int(round(EDGE_PX * th / 96.0))
    save(f"{o.out}/track_fill.png", three(lit, capsrc, edgesrc, 1024, 96, CAP_PX, EDGE_PX))
    save(f"{o.out}/track_empty.png", three(dark, capsrc, capsrc, 1024, 96, CAP_PX, CAP_PX))
    # plate
    img, m = piece["plate_ornate_base"]
    save(f"{o.out}/plate.png", fit(rgba(img, m, *ALPHA["plate"], solid=-1), 1024, 160))
    # the zone: its end bars (the brightest columns in each outer fifth) to x 24 and 488, its glow outside them
    img, m = piece["target_zone_hit"]
    z = rgba(img, m, *ALPHA["zone"])
    zw = z.shape[1]
    prof = img.max(2).mean(0)
    lb = int(np.argmax(prof[:zw // 5]))
    rb = zw - zw // 5 + int(np.argmax(prof[zw - zw // 5:]))
    rep.append(f"zone end bars at {lb} and {rb} of {zw} px")
    zone = hcat([fit(z[:, :lb], ZONE_M_PX, 224), fit(z[:, lb:rb], 512 - 2 * ZONE_M_PX, 224), fit(z[:, rb:], ZONE_M_PX, 224)])
    save(f"{o.out}/zone.png", zone)
    zr = np.nonzero(zone[:, 256, 3] > 0.5)[0]
    rep.append(f"zone body rows {zr.min() / 224:.3f}..{(zr.max() + 1) / 224:.3f} of its height at the centre column" if zr.size else "zone: no opaque body at the centre column")
    # the cursors
    for src, dst, down in (("cursor_tri_down", "cursor_down", True), ("cursor_tri_up", "cursor_up", False)):
        img, m = piece[src]
        c = fit(rgba(img, m, *ALPHA[dst], solid=7), 256, 208)
        save(f"{o.out}/{dst}.png", c)
        rep.append(f"{dst} tip at {tip(c, down):.3f} of its height (the mod assumes {'0.933' if down else '0.029'} within 0.05)")
    if o.all:
        for k in ("cursor_arrow_up", "cursor_arrow_down", "rivet_smooth", "rivet_filigree", "corner_bracket_r"):
            img, m = piece[k]
            save(f"{o.out}/{k}.png", rgba(img, m, *ALPHA[k]))
        rep.append("--all: the five unused pieces written as cut (the mod does not load them)")
    print("\n".join(rep))


if __name__ == "__main__":
    main()
