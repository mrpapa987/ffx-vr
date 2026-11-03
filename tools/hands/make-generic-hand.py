#!/usr/bin/env python3
"""make-generic-hand.py - generates the stylised low-poly generic hand packs shipped with FFX VR.

SPDX-License-Identifier: MIT. Everything here is procedural (no third-party art): the script and the packs it writes are
the FFX VR contributors' own work, under the repository's MIT licence (LICENSE).

  python tools/hands/make-generic-hand.py                  both shipped packs: assets/hands/generic (bare, skin tone)
                                                           and assets/hands/generic-glove (dark glove, the default look)
  python tools/hands/make-generic-hand.py --glove [out]    one pack, the glove colourway (default out: assets/hands/generic-glove)
  python tools/hands/make-generic-hand.py --skin  [out]    one pack, the skin colourway  (default out: assets/hands/generic)
  options: --name "shown name", --color r,g,b, --cuff-color r,g,b (0..1, gamma space like the game)
Each pack is <out>/hand.json and <out>/right.glb (the left hand is the runtime's X mirror). Needs Python 3 + numpy.
The committed glb files are what ships; numpy versions may differ in the last bits of a regenerated file, which is fine.
"""
import argparse, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import handlib as H

SIDES = 12
# S3 (the owner on S2: "a little floppy / long fingers"): the joints bend over a shorter zone (S2: 0.35 of the bone either
# side, a 90 deg PIP bend spread over ~25 mm, rubbery), the fingers are 5 % fuller (S2: radii 10.0 / 10.4 / 9.8 / 8.6 mm),
# and the palm reaches past the knuckles to the finger webs (S2 stopped 6 mm short of them, so the fingers showed their
# whole length from the knuckles: 0.55 of the hand length for the middle finger, a real one shows ~0.43 from its web).
BLEND = 0.22
FINGER_R = {"index": 0.0105, "middle": 0.0109, "ring": 0.0103, "little": 0.0090}
WEB = {"index": 0.010, "middle": 0.010, "ring": 0.010, "little": 0.013}   # palm skin past each knuckle (MCP), metres

def superellipse(n, a, b, e=2.6, phase=0.0):
    t = np.linspace(0, 2 * np.pi, n, endpoint=False) + phase
    c, s = np.cos(t), np.sin(t)
    return np.c_[a * np.sign(c) * np.abs(c) ** (2 / e), b * np.sign(s) * np.abs(s) ** (2 / e)]

class MeshB:
    def __init__(s): s.p, s.w, s.t = [], [], []
    def nv(s): return sum(len(x) for x in s.p)
    def add_rings(s, rings, weights, cap_start=None, cap_end=None):
        """rings: list of (n,3) arrays; weights: list of (26,) or (n,26). Connects consecutive rings."""
        base = s.nv(); n = len(rings[0])
        for r, w in zip(rings, weights):
            s.p.append(r); s.w.append(np.broadcast_to(w, (n, 26)).copy())
        for k in range(len(rings) - 1):
            a0 = base + k * n; b0 = a0 + n
            for i in range(n):
                i2 = (i + 1) % n
                s.t += [(a0 + i, b0 + i, b0 + i2), (a0 + i, b0 + i2, a0 + i2)]
        for which, cap in ((0, cap_start), (len(rings) - 1, cap_end)):
            if cap is None: continue
            pt, w = cap; ci = s.nv(); s.p.append(pt[None]); s.w.append(np.asarray(w)[None])
            r0 = base + which * n
            for i in range(n):
                i2 = (i + 1) % n
                s.t.append((r0 + i, ci, r0 + i2) if which == 0 else (r0 + i, r0 + i2, ci))
    def arrays(s):
        return np.concatenate(s.p), np.concatenate(s.w), np.array(s.t, dtype=np.int64)

def onehot(*pairs):
    w = np.zeros(26)
    for j, v in pairs: w[j] += v
    return w / w.sum()

def finger(mb, P, R, ch, radius, flat=0.82, back=0.010):
    """Tube along a finger chain [meta, prox, inter, dist, tip]: rings at knuckle-back, joints, mids, tip cap."""
    meta, joints = ch[0], ch[1:]
    rings, ws = [], []
    def ring_at(pt, Rj, r):
        xy = superellipse(SIDES, r, r * flat, e=2.2)
        return pt + xy[:, :1] * Rj[:, 0] + xy[:, 1:] * Rj[:, 1]
    blend = BLEND  # fraction of each bone either side of a joint over which the two bones' weights blend
    k0 = joints[0]
    rings.append(ring_at(P[k0] + R[k0][:, 2] * back, R[k0], radius * 1.08)); ws.append(onehot((meta, 1)))
    segs = list(zip(joints[:-1], joints[1:]))
    taper = np.linspace(1.0, 0.86, len(segs) + 1)
    for si, (a, b) in enumerate(segs):
        Lb = np.linalg.norm(P[b] - P[a])
        prev = meta if si == 0 else segs[si - 1][0]
        # joint ring: blended between previous bone and this one
        rings.append(ring_at(P[a], R[a], radius * taper[si] * 1.04)); ws.append(onehot((prev, 0.5), (a, 0.5)))
        rings.append(ring_at(P[a] - R[a][:, 2] * Lb * blend, R[a], radius * taper[si] * 0.97)); ws.append(onehot((a, 1)))
        rings.append(ring_at(P[a] - R[a][:, 2] * Lb * (1 - blend), R[a], radius * (taper[si] + taper[si + 1]) / 2 * 0.96)); ws.append(onehot((a, 1)))
    # fingertip: rounded cap around the tip joint, weighted to the distal bone
    d = segs[-1][0]; tip = joints[-1]; Rd = R[d]; rt = radius * taper[-1]
    for f in (0.0, 0.45, 0.8):
        rings.append(ring_at(P[tip] + Rd[:, 2] * rt * (1 - f) * 0.9, Rd, rt * np.sqrt(max(1 - f * f, 0.05))))
        ws.append(onehot((d, 1)))
    mb.add_rings(rings, ws, cap_start=None, cap_end=(P[tip] - Rd[:, 2] * rt * 0.15, onehot((d, 1))))

def build(P, R):
    mb = MeshB(); cuff = MeshB()
    # --- palm: lofted superellipse rings from the wrist to the knuckle line; front edge follows the knuckles
    kn = {f: P[H.FINGERS[f][1]] for f in ("index", "middle", "ring", "little")}
    kx = np.array([kn[f][0] for f in ("index", "middle", "ring", "little")])
    kz = np.array([kn[f][2] for f in ("index", "middle", "ring", "little")])
    metas = [H.FINGERS[f][0] for f in ("index", "middle", "ring", "little")]
    def palm_weight(pt):
        # wrist near the back, metacarpals by lateral proximity, thumb metacarpal on the thenar side
        t = np.clip(-pt[2] / 0.07, 0, 1)
        w = np.zeros(26); w[H.J["wrist"]] = (1 - t) ** 2 * 1.5
        g = np.exp(-((pt[0] - kx) / 0.012) ** 2) * t
        for m, gi in zip(metas, g): w[m] += gi
        th = np.exp(-((pt[0] + 0.035) / 0.012) ** 2) * np.exp(-((pt[2] + 0.03) / 0.02) ** 2) * (pt[1] < 0.004)
        w[H.J["thumb_metacarpal"]] += 1.2 * th
        return w / w.sum()
    n = 28
    order = np.argsort(kx); webz = np.array([WEB[f] for f in ("index", "middle", "ring", "little")])
    prox = [H.FINGERS[f][1] for f in ("index", "middle", "ring", "little")]
    def web_weight(pt, s):
        # past the knuckles the palm skin (the webs between the fingers) follows the proximal bones: s 0 at the knuckle
        # line, 1 at the web; shared between the nearest fingers by lateral distance
        w = palm_weight(pt); wp = s * s * (3 - 2 * s)
        g = np.exp(-((pt[0] - kx) / 0.010) ** 2); g /= g.sum()
        w = w * (1 - wp)
        for j, gi in zip(prox, g): w[j] += wp * gi
        return w / w.sum()
    # (t along wrist -> knuckles, s past the knuckles to the web, half width, half thickness, centre lowered by): the webs
    # are on the palm side, thin, below the fingers' axes, so from the back the knuckles and fingers still read separately
    spec = [(0.0, 0, 0.027, 0.016, 0), (0.18, 0, 0.034, 0.017, 0), (0.4, 0, 0.041, 0.018, 0), (0.62, 0, 0.044, 0.017, 0),
            (0.82, 0, 0.045, 0.0155, 0), (0.94, 0, 0.044, 0.0145, 0), (1.0, 0.0, 0.043, 0.0135, 0.0),
            (1.0, 0.55, 0.0415, 0.0085, 0.0045), (1.0, 1.0, 0.0395, 0.0048, 0.0075)]
    rings, ws = [], []
    for t, sw, hw, ht, low in spec:
        xy = superellipse(n, hw, ht, e=3.0, phase=np.pi / n)
        xc = 0.004 * t - 0.002
        x = xy[:, 0] + xc
        zk = np.interp(x, kx[order], kz[order]); zw = np.interp(x, kx[order], webz[order])
        z = 0.012 * (1 - t) + (zk + 0.004) * t if t < 1 else zk - zw * sw   # up to the knuckles, then on to the webs
        y = xy[:, 1] + 0.004 - 0.003 * t - low
        r = np.c_[x, y, z]; rings.append(r)
        ws.append(np.array([web_weight(p, sw) if t >= 1 else palm_weight(p) for p in r]))
    front = rings[-1].mean(0) + np.array([0, 0, -0.002])
    mb.add_rings(rings, ws, cap_start=None, cap_end=(front, web_weight(front, 1.0)))
    # wrist / forearm stub (separate primitive "cuff") behind the palm, weighted to the wrist
    crings = []
    for z, hw, ht in ((0.030, 0.029, 0.021), (0.016, 0.031, 0.022), (0.004, 0.030, 0.021), (-0.006, 0.027, 0.018)):
        xy = superellipse(n, hw, ht, e=2.4)
        crings.append(np.c_[xy[:, 0] - 0.001, xy[:, 1] + 0.003, np.full(n, z)])
    cuff.add_rings(crings, [onehot((H.J["wrist"], 1))] * len(crings),
                   cap_start=(np.array([-0.001, 0.003, 0.030]), onehot((H.J["wrist"], 1))))
    # --- fingers
    for f in ("index", "middle", "ring", "little"):
        finger(mb, P, R, H.FINGERS[f], FINGER_R[f])
    # --- thumb: thick at the metacarpal (thenar), tapering
    ch = H.FINGERS["thumb"]; rings, ws = [], []
    def tring(pt, Rj, r, flat=0.85):
        xy = superellipse(SIDES, r, r * flat, e=2.2)
        return pt + xy[:, :1] * Rj[:, 0] + xy[:, 1:] * Rj[:, 1]
    m, pj, dj, tip = ch
    Lm = np.linalg.norm(P[pj] - P[m]); Lp = np.linalg.norm(P[dj] - P[pj])
    spec = [(P[m] + R[m][:, 2] * 0.006, R[m], 0.015, onehot((m, 1))),
            (P[m] - R[m][:, 2] * Lm * 0.45, R[m], 0.0145, onehot((m, 1))),
            (P[pj] + R[m][:, 2] * Lm * 0.15, R[m], 0.0118, onehot((m, 0.8), (pj, 0.2))),
            (P[pj], R[pj], 0.0112, onehot((m, 0.5), (pj, 0.5))),
            (P[pj] - R[pj][:, 2] * Lp * 0.35, R[pj], 0.0108, onehot((pj, 1))),
            (P[dj] + R[pj][:, 2] * Lp * 0.2, R[pj], 0.0104, onehot((pj, 1))),
            (P[dj], R[dj], 0.0104, onehot((pj, 0.5), (dj, 0.5))),
            (P[dj] - R[dj][:, 2] * 0.008, R[dj], 0.0100, onehot((dj, 1)))]
    rt = 0.0094
    for f in (0.0, 0.45, 0.8):
        spec.append((P[tip] + R[dj][:, 2] * rt * (1 - f) * 0.9, R[dj], rt * np.sqrt(max(1 - f * f, 0.05)), onehot((dj, 1))))
    for pt, Rj, r, w in spec: rings.append(tring(pt, Rj, r)); ws.append(w)
    mb.add_rings(rings, ws, cap_start=(P[m] + R[m][:, 2] * 0.012, onehot((m, 1))),
                 cap_end=(P[tip] - R[dj][:, 2] * rt * 0.15, onehot((dj, 1))))
    return mb.arrays(), cuff.arrays()

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
LOOKS = {   # folder, shown name, hand colour, cuff colour (gamma space, like the game's R8G8B8A8_UNORM target)
    "glove": ("generic-glove", "Generic glove", "0.22,0.22,0.25", "0.46,0.40,0.32"),   # dark glove, tan leather cuff
    "skin":  ("generic",       "Generic hand",  "0.80,0.74,0.68", "0.20,0.22,0.27"),   # skin-neutral mannequin, dark cuff
}

def write_pack(out, name, color, cuff_color):
    os.makedirs(out, exist_ok=True)
    P = H.default_rest_positions(); R = H.rest_frames(P)
    (hp, hw, ht), (cp, cw, ct) = build(P, R)
    prims = []
    for pname, pos, W, tris, col in (("hand", hp, hw, ht, color), ("cuff", cp, cw, ct, cuff_color)):
        j4, w4 = H.top4(W)
        prims.append(dict(name=pname, pos=pos, nrm=H.vertex_normals(pos, tris), uv=None, joints=j4, weights=w4,
                          idx=tris.reshape(-1), color=tuple(float(x) for x in col.split(",")) + (1.0,), texture_png=None))
    H.write_glb(os.path.join(out, "right.glb"), prims, P, R, name="generic_hand", generator="ffxvr make-generic-hand.py",
                copyright="FFX VR contributors, MIT licence")
    manifest = {
        "format": 1, "name": name, "author": "FFX VR contributors", "licence": "MIT",
        "source": "tools/hands/make-generic-hand.py (procedural, no third-party art)",
        "right": "right.glb", "left": None, "mirror": True,
        "scale": 1.0, "wrist_offset_m": [0, 0, 0], "wrist_rotation_deg": [0, 0, 0],
    }
    with open(os.path.join(out, "hand.json"), "w", newline="\n") as f:
        json.dump(manifest, f, indent=2); f.write("\n")
    nt = sum(len(p["idx"]) // 3 for p in prims); nv = sum(len(p["pos"]) for p in prims)
    print(f"wrote {out}/right.glb: {nv} verts, {nt} tris, {len(prims)} primitives, "
          f"{os.path.getsize(os.path.join(out, 'right.glb'))} bytes")

def main():
    ap = argparse.ArgumentParser(description="Generate FFX VR's generic hand packs (MIT).")
    ap.add_argument("out", nargs="?", default=None, help="output folder (one pack); default: assets/hands/<look>")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--glove", action="store_true", help="the dark glove colourway (the default look in game)")
    g.add_argument("--skin", action="store_true", help="the skin-tone colourway")
    ap.add_argument("--name", default=None, help="the name shown in the F8 menu")
    ap.add_argument("--color", default=None, help="hand colour r,g,b (0..1)")
    ap.add_argument("--cuff-color", default=None, help="cuff colour r,g,b (0..1)")
    a = ap.parse_args()
    looks = ["glove"] if a.glove else ["skin"] if a.skin else ["glove", "skin"]
    if len(looks) > 1 and (a.out or a.name or a.color or a.cuff_color):
        ap.error("an output folder, --name or colours need --glove or --skin (one pack)")
    for look in looks:
        folder, name, color, cuff = LOOKS[look]
        write_pack(a.out or os.path.join(REPO, "assets", "hands", folder), a.name or name, a.color or color, a.cuff_color or cuff)

if __name__ == "__main__":
    main()
