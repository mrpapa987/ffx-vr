#!/usr/bin/env python3
"""import-hands.py - make an FFX VR hand pack from a character model YOU OWN (Wavefront OBJ + its textures).

SPDX-License-Identifier: MIT (the script; part of the FFX VR repository). What it writes is made from your model: it is
for your own use on your own PC. It is marked private and must not be shared or uploaded (most character models,
game rips above all, are someone else's copyright).

Usually run through tools\\import-hands.ps1, which finds Python and the game's mod folder:
    powershell -ExecutionPolicy Bypass -File tools\\import-hands.ps1 "D:\\models\\Tidus - HP.obj" -Name tidus
Directly:
    python import-hands.py "Tidus - HP.obj" --out "<game>/fahrenheit/mods/ffxvr/hands/tidus" [options]

The model: one OBJ (with its .mtl and textures beside it), a full body in a T or A pose, Y up, arms out so the hands are
the two ends of the model along X (else pick them with --tip). What it does, per hand:
  1. finds the arm end and its axis, and cuts the mesh at the wrist / cuff (the narrowest section a hand-length back
     from the fingertips; --cut-right / --cut-left move it), clipping triangles with their UVs and capping the opening;
  2. finds the five fingers from the mesh itself (geodesic distance from the cut: the five branches that merge last),
     orders them from the thumb, and places the 26 OpenXR hand joints on their centrelines;
  3. turns the hand into the pack's space (metres, the OpenXR wrist frame: -Z to the fingertips, +Y out of the back of
     the hand), one scale for both hands from the middle finger (--hand-length, wrist to middle tip, default 0.19 m);
  4. skins it (fingers by their bones, the palm by distance, loose pieces such as straps follow the nearest vertex);
  5. writes right.glb and left.glb (each material's texture embedded), hand.json ("private": true) and
     PRIVATE-do-not-share.txt into --out.
Options:
  --name TEXT          the name shown in F8 (default: "<obj name> hands")
  --cut-right F, --cut-left F, --cut F   the wrist cut, as a fraction of the model height back from the fingertips
                       (--debug prints the cross-section profile; e.g. 0.095 cuts at the bare wrist instead of the cuff)
  --flip-right, --flip-left   the palm and the back of that hand were detected the wrong way round
  --tip=X,Y,Z          pick a hand by hand: a point (model units) near its fingertips; give it twice for both hands.
                       Instead of the two ends along X (arms down at the sides, a model not in a T pose). Write it
                       with "=" (the numbers may start with a minus)
  --height H           the model height in model units (default: its Y extent; set it for a hands-only model: about
                       9 to 11 times the hand's length)
  --z-up               the model is Z up (turned to Y up first)
  --only right|left    use one hand, mirrored for the other
  --hand-length M      wrist to middle fingertip in metres (default 0.19), for both hands: the wrist joints are moved
                       along each hand to match (one scale for both, from the middle fingers)
  --keep-proportions   keep each hand's own wrist estimate instead (the hands may then differ in length)
  --debug              print the cut profile, the finger search and the palm side cues; writes debug-*.png into --out
  --preview FILE.png   render open / fist / point / grip previews (needs matplotlib). A bare file name goes into the
                       pack folder; like the pack, never inside a git checkout
It refuses to write inside a git checkout (a pack must never end up in a repository) and never overwrites a pack
folder that is not one of its own (the shipped generic packs). Needs Python 3.8+ and numpy (pip install numpy).
"""
import argparse, heapq, io, json, os, struct, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import handlib as H
import objio

FINGER_ORDER = ["thumb", "index", "middle", "ring", "little"]
LICENCE = "private \u2014 made from your own copy, do not share"
PRIVATE_TXT = """This hand pack was made by FFX VR's tools/hands/import-hands.py from a 3D model you own.

It is for your own use on this PC. Do not share it, upload it, put it in a mod, a release or a repository: the model it
came from is almost certainly someone else's copyright (a game's character above all). FFX VR never ships, packages or
uploads it: tools/package-release.ps1 refuses any pack marked like this one, and install-mod.ps1 leaves it in place.

Made from: {src}
Made on:   {date}
To remove it: delete this folder ({folder}).
"""

class ImportError_(Exception):
    pass

def fail(msg):
    raise ImportError_(msg)

# canonical phalanx length ratios (proximal, intermediate, distal->tip) and MCP set-back (fraction of visible length)
RATIOS = {"index": (0.47, 0.28, 0.25), "middle": (0.47, 0.29, 0.24), "ring": (0.46, 0.30, 0.24),
          "little": (0.46, 0.27, 0.27), "thumb": (0.54, 0.46)}
MCP_BACK = 0.12
MIDDLE_FRAC = 0.505  # middle finger (knuckle to tip) / hand length (wrist to middle tip), generic hand
THUMB_META = 0.21  # thumb metacarpal length / hand length
RING_JUMP = 1.7    # a ring this much wider than the finger so far = the finger base
WRIST_SNAP = 0.006  # snap the wrist estimate to the narrowest section within this (x height)
WRIST_BACK = 1.0   # wrist-to-knuckles = WRIST_BACK x middle finger (knuckle to tip)

def log(*a):
    print(*a, flush=True)

# ---------------------------------------------------------------------------------------------- geometry
def weld(P, tol):
    key = {}; rep = np.zeros(len(P), int)
    for i, p in enumerate(np.round(P / tol).astype(np.int64)):
        rep[i] = key.setdefault(tuple(p), len(key))
    return rep, len(key)

def dijkstra(adj, sources, n):
    d = np.full(n, np.inf); h = []
    for s, d0 in sources:
        if d0 < d[s]: d[s] = d0; heapq.heappush(h, (d0, s))
    while h:
        dv, v = heapq.heappop(h)
        if dv > d[v]: continue
        for u, w in adj[v]:
            if dv + w < d[u]: d[u] = dv + w; heapq.heappush(h, (dv + w, u))
    return d

def section(tris_pos, origin, axis):
    """Plane/triangle-soup intersection: returns (segments (K,2,3))."""
    s = (tris_pos - origin) @ axis                      # (M,3)
    segs = []
    for t in range(len(tris_pos)):
        pts = []
        for a, b in ((0, 1), (1, 2), (2, 0)):
            if (s[t, a] >= 0) != (s[t, b] >= 0):
                u = s[t, a] / (s[t, a] - s[t, b]); pts.append(tris_pos[t, a] + u * (tris_pos[t, b] - tris_pos[t, a]))
        if len(pts) == 2: segs.append(pts)
    return np.array(segs).reshape(-1, 2, 3)

def clip_tris(tri_attr, origin, axis):
    """Keep the part of each triangle on the +axis side. tri_attr: list of per-triangle (3, D) arrays whose
    first 3 columns are positions. Returns new triangles (list of (3,D)) and cut segments (pairs of rows)."""
    out, cuts = [], []
    for T in tri_attr:
        s = (T[:, :3] - origin) @ axis
        inside = s >= 0
        if inside.all(): out.append(T); continue
        if not inside.any(): continue
        poly = []; cut = []
        for i in range(3):
            a, b = T[i], T[(i + 1) % 3]; sa, sb = s[i], s[(i + 1) % 3]
            if sa >= 0: poly.append(a)
            if (sa >= 0) != (sb >= 0):
                u = sa / (sa - sb); q = a + u * (b - a); poly.append(q); cut.append(q)
        for k in range(1, len(poly) - 1): out.append(np.array([poly[0], poly[k], poly[k + 1]]))
        if len(cut) == 2: cuts.append(cut)
    return out, cuts

# ---------------------------------------------------------------------------------------------- per-hand
def extract_hand(o, side, cut_override, height, debug, tip_pt=None):
    V = o["V"]; used = np.unique([c[0] for f in o["faces"] for c in f[2]])
    U = V[used]
    if tip_pt is None:
        tip = U[np.argmax(U[:, 0] * side)]
    else:   # --tip: the vertex near the given point that is furthest from the model's centre (the fingertips)
        cand = U[np.linalg.norm(U - tip_pt, axis=1) < 0.15 * height]
        if len(cand) < 20: fail(f"--tip ({', '.join(f'{x:.4g}' for x in tip_pt)}): fewer than 20 vertices within 0.15 x the height of it (model units?)")
        tip = cand[np.argmax(np.linalg.norm(cand - U.mean(0), axis=1))]
    near = U[np.linalg.norm(U - tip, axis=1) < 0.25 * height]
    if len(near) < 50: fail("too few vertices near the hand: is the model Y up (else --z-up) and in a T or A pose (else --tip)?")
    c = near.mean(0); _, vec = np.linalg.eigh(np.cov((near - c).T)); ax = vec[:, -1]
    if np.dot(tip - c, ax) < 0: ax = -ax
    # candidate triangles: everything within 0.25*height of the tip
    cand = [f for f in o["faces"] if all(np.linalg.norm(V[x[0]] - tip) < 0.25 * height for x in f[2])]
    tp = np.array([[V[x[0]] for x in f[2]] for f in cand])
    # wrist: narrowest section between 0.085 and 0.14 body heights back from the tip (fraction of height)
    prof = []
    for d in np.linspace(0.07, 0.15, 33):
        segs = section(tp, tip - ax * d * height, ax)
        if len(segs) < 3: continue
        pts = segs.reshape(-1, 3); cc = pts.mean(0)
        r = np.linalg.norm(pts - cc, axis=1)
        prof.append((d, float(np.percentile(r, 90)), cc))
    if cut_override is not None:
        dcut = cut_override
    else:
        win = [p for p in prof if 0.085 <= p[0] <= 0.14]
        if not win: fail("no wrist found (no closed cross-section 0.085 to 0.14 x the height back from the fingertips): "
                         "give the cut with --cut-right / --cut-left, or the model height with --height")
        dcut = min(win, key=lambda p: p[1])[0]
    if debug:
        log("   section profile (d/height: r90):", " ".join(f"{p[0]:.3f}:{p[1]:.0f}" for p in prof))
    origin = tip - ax * dcut * height
    # build per-triangle attribute rows: pos(3) uv(2) nrm(3); material per triangle
    rows, mats = [], []
    T = o["T"]; N = o["N"]
    for f in cand:
        r = []
        for vi, ti, ni in f[2]:
            uv = T[ti] if ti >= 0 else (0, 0)
            nn = N[ni] if ni >= 0 else (0, 0, 0)
            r.append(np.r_[V[vi], uv, nn])
        rows.append(np.array(r)); mats.append(f[1])
    kept, kmats, cuts_by = [], [], []
    for r, m in zip(rows, mats):
        out, cuts = clip_tris([r], origin, ax)
        kept += out; kmats += [m] * len(out); cuts_by += [cuts]
    if len(kept) < 50: fail(f"the hand has {len(kept)} triangles after the cut: --cut too short, or not a hand at that end")
    kept = np.array(kept); kmats = np.array(kmats, dtype=object)
    return dict(tip=tip, axis=ax, origin=origin, dcut=dcut, tris=kept, mats=kmats, prof=prof,
                cut_segs=[c for cs in cuts_by for c in cs])

def rig_hand(hd, height, debug, flip=False):
    tris = hd["tris"]; P = tris[:, :, :3].reshape(-1, 3)
    tol = 1e-4 * height
    rep, nw = weld(P, tol)
    W = np.zeros((nw, 3)); W[rep] = P
    F = rep.reshape(-1, 3)
    # components
    par = list(range(nw))
    def find(a):
        while par[a] != a: par[a] = par[par[a]]; a = par[a]
        return a
    for a, b, c in F:
        for x, y in ((a, b), (b, c)):
            rx, ry = find(x), find(y)
            if rx != ry: par[rx] = ry
    roots = np.array([find(i) for i in range(nw)])
    main = np.bincount(roots).argmax()
    inmain = roots == main
    adj = [[] for _ in range(nw)]
    for tri in F:
        for x, y in ((tri[0], tri[1]), (tri[1], tri[2]), (tri[2], tri[0])):
            w = float(np.linalg.norm(W[x] - W[y])); adj[x].append((y, w)); adj[y].append((x, w))
    ax = hd["axis"]; origin = hd["origin"]
    s = (W - origin) @ ax
    smin = s[inmain].min()        # the hand may end before the cut plane (e.g. under a bracelet)
    ring = [i for i in range(nw) if inmain[i] and s[i] < smin + 0.004 * height]
    wrist_c = W[ring].mean(0) if ring else origin
    g = dijkstra(adj, [(i, 0.0) for i in ring], nw)
    g[~inmain] = -1
    # sweep: superlevel-set components, record the level at which each tip's branch first merges
    order = np.argsort(-g); seen = np.zeros(nw, bool); up = list(range(nw)); members = {}
    tipof = {}; branch = {}
    def f2(a):
        while up[a] != a: up[a] = up[up[a]]; a = up[a]
        return a
    for v in order:
        if g[v] < 0: break
        seen[v] = True; members[v] = [v]; tipof[v] = v      # a new local maximum starts its own branch
        for u, _ in adj[v]:
            if not seen[u]: continue
            a, b = f2(v), f2(u)
            if a == b: continue
            # merging two components. Only a merge of two significant branches (each reaching well above
            # this level) ends a finger: record each such branch at its first such merge, members as of now.
            ta, tb = tipof[a], tipof[b]
            thr = 0.08 * g.max()
            if g[ta] - g[v] > thr and g[tb] - g[v] > thr:
                for t, comp in ((ta, a), (tb, b)):
                    if t not in branch:
                        branch[t] = (g[v], list(members[comp]))
            keep, drop = (a, b) if g[ta] >= g[tb] else (b, a)
            up[drop] = keep; members[keep] += members[drop]; del members[drop]
            tipof[keep] = tipof[keep] if g[tipof[keep]] >= g[tipof[drop]] else tipof[drop]
    Lg = g.max()
    # real branches: long ones (length = tip g - merge g)
    cands = sorted(((g[t] - lev, t, lev, mem) for t, (lev, mem) in branch.items()), reverse=True)
    # the longest branch overall (the one never merged) - give it the merge level of its last partner
    fingers = [c for c in cands if c[0] > 0.12 * Lg][:5]
    if len(fingers) < 5:
        fail(f"found {len(fingers)} finger branches (need 5): fingers fused or mitten-like? "
             "Try --cut-right / --cut-left, or rig the hand in Blender instead.")
    if debug:
        log("   branches (len/Lg):", [round(c[0] / Lg, 2) for c in cands[:8]])
    # centrelines and finger extents: walk geodesic rings outward from each tip; the finger ends where the
    # ring suddenly widens (it has reached the palm / the web).
    tips = [t for _, t, _, _ in fingers]
    D = np.stack([dijkstra(adj, [(t, 0.0)], nw) for t in tips])
    nearest = D.argmin(0); step = 0.004 * height
    finfo = []
    for k, t in enumerate(tips):
        dk = D[k]; cents, radii, ds, gl = [], [], [], []
        d = step
        while d < 0.09 * height:
            m = np.where(inmain & (np.abs(dk - d) < 0.75 * step))[0]
            if len(m) >= 3:
                c = W[m].mean(0); cents.append(c); ds.append(d); gl.append(g[m].mean())
                radii.append(np.percentile(np.linalg.norm(W[m] - c, axis=1), 90))
            d += step
        if len(radii) < 4: fail("a finger is too short to follow (fewer than 4 rings): fused fingers? Try --cut-right / --cut-left")
        radii = np.array(radii); bi = len(radii) - 1
        for i in range(3, len(radii)):
            if radii[i] > RING_JUMP * np.median(radii[1:i]): bi = i; break
        cl = np.array(cents[:bi])[::-1]                       # base ... tip
        mem = np.where((nearest == k) & inmain & (dk < ds[bi]))[0]
        finfo.append(dict(tip=W[t], base=cl[0], cl=cl, mem=mem, lev=gl[bi], radius=float(np.median(radii[1:bi]))))
        if debug:
            log(f"   finger {k}: {bi} rings, base at {ds[bi] / height:.3f} x height from tip, "
                f"radius {finfo[-1]['radius'] / height * 1000:.1f} per-mille")
    # thumb = the branch whose base is nearest the wrist (lowest merge level)
    th = min(range(5), key=lambda i: finfo[i]["lev"])
    thumb = finfo[th]; rest = [finfo[i] for i in range(5) if i != th]
    # order the other four by distance of their base from the thumb base
    rest.sort(key=lambda f: np.linalg.norm(f["base"] - thumb["base"]))
    named = dict(zip(FINGER_ORDER, [thumb] + rest))
    # MCPs: extend each centreline back from its base, along the line fitted through the whole centreline (hands-tap build:
    # the direction of its first two rings bent toward a neighbour at the web, so the Tidus model's right middle knuckle sat
    # 12 mm from the index and 36 mm from the ring, and its fist showed a gap between middle and ring; fitted: 20 / 23 / 21 mm)
    for n in FINGER_ORDER[1:]:
        f = named[n]; c = f["cl"]; d = np.linalg.svd(c - c.mean(0), full_matrices=False)[2][0]
        if np.dot(d, c[0] - c[-1]) < 0: d = -d                # toward the base
        vis = np.linalg.norm(f["tip"] - f["cl"][0])
        f["mcp"] = f["cl"][0] + d * MCP_BACK * vis
    K = np.array([named[n]["mcp"] for n in FINGER_ORDER[1:]])
    # anatomical wrist: one middle-finger length (MCP to tip) back from the knuckles, along the arm axis,
    # centred in the cross-section there. The mesh cut (hd["origin"]) may be further back (cuffs, sleeves).
    mlen = np.linalg.norm(named["middle"]["tip"] - named["middle"]["mcp"])
    wpt = K.mean(0) - ax * WRIST_BACK * mlen
    # snap to the narrowest section within +-0.015 x height of that estimate (the real wrist, if visible)
    d_est = np.dot(hd["tip"] - wpt, ax) / height
    near = [p for p in hd["prof"] if abs(p[0] - d_est) <= WRIST_SNAP]
    if near:
        best = min(near, key=lambda p: p[1]); wpt = hd["tip"] - ax * best[0] * height
    if np.dot(wpt - origin, ax) > 0:
        tp = W[F]; segs = section(tp, wpt, ax)
        if len(segs) >= 3: wpt = segs.reshape(-1, 3).mean(0)
        wrist_c = wpt
    if debug:
        log(f"   wrist joint {np.dot(hd['tip'] - wrist_c, ax) / height:.3f} x height back from the tip "
            f"(cut at {hd['dcut']:.3f})")
    fwd = K.mean(0) - wrist_c; fwd /= np.linalg.norm(fwd)
    lat = named["index"]["mcp"] - named["little"]["mcp"]
    n_ = np.cross(lat, fwd); n_ /= np.linalg.norm(n_)
    # dorsal sign: the thumb sits on the palm side; fingers curl to the palm side
    th_mid = thumb["cl"].mean(0)
    cue_thumb = float(np.dot(th_mid - wrist_c, n_))
    curl = np.mean([np.dot(named[n]["tip"] - (named[n]["mcp"] + fwd * np.dot(named[n]["tip"] - named[n]["mcp"], fwd)), n_)
                    for n in FINGER_ORDER[1:]])
    score = -cue_thumb - curl
    dorsal = n_ if score > 0 else -n_
    if flip: dorsal = -dorsal
    Zb = -fwd; Yb = dorsal - Zb * np.dot(dorsal, Zb); Yb /= np.linalg.norm(Yb); Xb = np.cross(Yb, Zb)
    Rb = np.stack([Xb, Yb, Zb], 1)
    thumb_x = float(np.dot(thumb["base"] - wrist_c, Xb))
    handed = "right" if thumb_x < 0 else "left"
    if debug:
        log(f"   dorsal cues: thumb {cue_thumb/height*1000:.1f}, curl {curl/height*1000:.1f} (per-mille height) -> "
            f"back of the hand on the {'+' if score > 0 else '-'} side of the palm normal{' (then --flip)' if flip else ''}; thumb on {'-X' if thumb_x < 0 else '+X'} => {handed} hand")
    # joints (model space)
    # joints (model space), from a wrist position (run() may move the wrist to normalise the hand length)
    def place(wrist_c):
        J = np.zeros((26, 3)); J[H.J["wrist"]] = wrist_c
        def along(points, fracs):
            seg = np.linalg.norm(np.diff(points, axis=0), axis=1); cum = np.r_[0, np.cumsum(seg)]
            out = []
            for fr in np.cumsum(fracs)[:-1]:
                t = fr * cum[-1]; k = min(np.searchsorted(cum, t) - 1, len(seg) - 1); k = max(k, 0)
                u = (t - cum[k]) / max(seg[k], 1e-9); out.append(points[k] + u * (points[k + 1] - points[k]))
            return out
        for n in FINGER_ORDER[1:]:
            f = named[n]; ch = H.FINGERS[n]
            tipj = f["tip"] - (f["tip"] - f["cl"][-1]) * 0.25
            line = np.vstack([f["mcp"], f["cl"], tipj])
            inter, dist = along(line, RATIOS[n])
            J[ch[0]] = wrist_c + 0.27 * (f["mcp"] - wrist_c)
            J[ch[1]] = f["mcp"]; J[ch[2]] = inter; J[ch[3]] = dist; J[ch[4]] = tipj
        f = thumb; ch = H.FINGERS["thumb"]
        tipj = f["tip"] - (f["tip"] - f["cl"][-1]) * 0.25
        line = np.vstack([f["cl"], tipj]); vis = np.sum(np.linalg.norm(np.diff(line, axis=0), axis=1))
        (dj,) = along(line, RATIOS["thumb"])
        # thumb CMC: one thumb-metacarpal length (0.21 x hand length) from the thumb MCP, toward a point on the
        # thumb side of the wrist (anatomical proportions; the metacarpal is buried in the thenar, not visible)
        Lh = np.linalg.norm(named["middle"]["tip"] - wrist_c)
        tgt = wrist_c + Xb * np.dot(f["cl"][0] - wrist_c, Xb) * 0.45
        u = tgt - f["cl"][0]; u /= np.linalg.norm(u)
        J[ch[0]] = f["cl"][0] + u * THUMB_META * Lh; J[ch[1]] = f["cl"][0]; J[ch[2]] = dj; J[ch[3]] = tipj
        J[H.J["palm"]] = 0.5 * (J[H.J["middle_metacarpal"]] + J[H.J["middle_proximal"]])
        return J
    J = place(wrist_c)
    # region per welded vertex: finger index or palm (-1)
    region = np.full(nw, -1)
    for fi, n in enumerate(FINGER_ORDER):
        region[named[n]["mem"]] = fi
    return dict(W=W, rep=rep, F=F, J=J, Rb=Rb, wrist=wrist_c, region=region, inmain=inmain, handed=handed, adj=adj,
                place=place, kc=K.mean(0))

def normalise_wrist(rig, L):
    """Moves the wrist along its line from the knuckles so that wrist -> middle tip joint is L (model units): the two
    hands' wrist estimates can differ by a centimetre or more (a bracelet or cuff changes the cross-sections the wrist
    is snapped to), which would make one hand longer and sit further along its controller. Returns the move (units)."""
    tip = rig["J"][H.J["middle_tip"]]; kc = rig["kc"]; w0 = rig["wrist"]
    u = w0 - kc; n = np.linalg.norm(u)
    if n < 1e-9: return 0.0
    u /= n; d = kc - tip
    b = np.dot(u, d); c = np.dot(d, d) - L * L      # |kc + t u - tip|^2 = L^2  ->  t^2 + 2 b t + c = 0
    disc = b * b - c
    if disc <= 0: return 0.0
    t = -b + np.sqrt(disc)
    if t <= 0: return 0.0
    w = kc + u * t
    rig["J"] = rig["place"](w); rig["wrist"] = w
    return float(t - n)

def seg_dist(p, a, b):
    ab = b - a; t = np.clip(((p - a) @ ab) / max(ab @ ab, 1e-12), 0, 1)
    return np.linalg.norm(p - (a + t[:, None] * ab), axis=1), t

def skin(rig, Jc):
    """Weights per welded vertex (canonical-space joints Jc, vertices in canonical space)."""
    Wc = rig["Wc"]; nw = len(Wc); Wt = np.zeros((nw, 26)); region = rig["region"]
    for fi, n in enumerate(FINGER_ORDER):
        ch = H.FINGERS[n]; m = np.where(region == fi)[0]
        if not len(m): continue
        bones = list(zip(ch[:-1], ch[1:]))          # bone k is driven by joint ch[k]
        D = np.stack([seg_dist(Wc[m], Jc[a], Jc[b])[0] for a, b in bones], 1)
        Tt = np.stack([seg_dist(Wc[m], Jc[a], Jc[b])[1] for a, b in bones], 1)
        k = D.argmin(1); t = Tt[np.arange(len(m)), k]
        for i, v in enumerate(m):
            a = bones[k[i]][0]; w = {a: 1.0}
            if t[i] < 0.2 and k[i] > 0:
                prev = bones[k[i] - 1][0]; bl = 0.5 + 0.5 * t[i] / 0.2; w = {a: bl, prev: 1 - bl}
            elif t[i] > 0.85 and k[i] + 1 < len(bones):
                nxt = bones[k[i] + 1][0]; bl = 0.5 + 0.5 * (1 - t[i]) / 0.15; w = {a: bl, nxt: 1 - bl}
            for j, x in w.items(): Wt[v, j] += x
    # palm: inverse distance^4 to wrist (short stub), metacarpal bones, and proximal bones (blend at knuckles)
    m = np.where((region < 0) & rig["inmain"])[0]
    cands = [(H.J["wrist"], Jc[H.J["wrist"]] + np.array([0, 0, 0.02]), Jc[H.J["wrist"]])]
    for n in FINGER_ORDER:
        ch = H.FINGERS[n]; cands.append((ch[0], Jc[ch[0]], Jc[ch[1]]))
        if n != "thumb": cands.append((ch[1], Jc[ch[1]], Jc[ch[2]]))
    D = np.stack([seg_dist(Wc[m], a, b)[0] for _, a, b in cands], 1) + 0.004
    w = 1 / D ** 4; w /= w.sum(1, keepdims=True)
    for c, (j, _, _) in enumerate(cands): Wt[m, j] += w[:, c]
    # loose pieces: copy from the nearest main-component vertex
    lo = np.where(~rig["inmain"])[0]; mm = np.where(rig["inmain"])[0]
    for v in lo:
        Wt[v] = Wt[mm[np.argmin(np.linalg.norm(Wc[mm] - Wc[v], axis=1))]]
    return Wt

def finger_scale(rig, hand_len):
    """metres per model unit from the middle finger (knuckle to tip), the most reliably detected length."""
    J = rig["J"]; L = sum(np.linalg.norm(J[b] - J[a]) for a, b in zip(H.FINGERS["middle"][1:-1], H.FINGERS["middle"][2:]))
    return hand_len * MIDDLE_FRAC / L

MAX_VERTS, MAX_INDICES, MAX_IMAGES, MAX_TEX, MAX_GLB = 65535, 393216, 4, 2048, 32 << 20   # = the runtime's caps per hand

def image_size(b):
    """(width, height) from a PNG's IHDR or a JPEG's SOF marker; None if unknown."""
    if b[:8] == b"\x89PNG\r\n\x1a\n" and len(b) >= 24: return struct.unpack(">II", b[16:24])
    if b[:2] == b"\xff\xd8":
        i = 2
        while i + 9 < len(b):
            if b[i] != 0xFF: i += 1; continue
            m = b[i + 1]
            if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7: i += 2; continue
            L = struct.unpack(">H", b[i + 2:i + 4])[0]
            if m in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
                h, w = struct.unpack(">HH", b[i + 5:i + 9]); return (w, h)
            i += 2 + L
    return None

def load_texture(tp):
    """A texture's bytes for the glb (PNG / JPEG), or None (that part is drawn in its flat colour; warned). One over the
    game's 2048 px is shrunk to 1024 px when Pillow is installed (the game would draw a 1024 px copy anyway), else warned."""
    nm = os.path.basename(tp)
    try: data = open(tp, "rb").read()
    except OSError as e: log(f"   warning: texture {nm} could not be read ({e}): that part is drawn in its flat colour"); return None
    if data[:8] != b"\x89PNG\r\n\x1a\n" and data[:2] != b"\xff\xd8":
        log(f"   warning: texture {nm} is not a PNG or JPEG (convert it): that part is drawn in its flat colour"); return None
    wh = image_size(data)
    if wh is None or max(wh) > MAX_TEX or len(data) > 16 << 20:
        try:
            from PIL import Image
            im = Image.open(io.BytesIO(data)); im.load(); im.thumbnail((1024, 1024))
            buf = io.BytesIO()
            if data[:2] == b"\xff\xd8": im.convert("RGB").save(buf, "JPEG", quality=92)
            else: im.save(buf, "PNG")
            log(f"   note: texture {nm} ({wh[0] if wh else '?'} x {wh[1] if wh else '?'} px) shrunk to {im.size[0]} x {im.size[1]} px (the game reads at most {MAX_TEX})")
            return buf.getvalue()
        except ImportError:
            log(f"   warning: texture {nm} is {wh[0] if wh else '?'} x {wh[1] if wh else '?'} px, over the game's {MAX_TEX} (or over 16 MB): that part is drawn "
                f"in its flat colour. Resize it to 2048 px or less, or install Pillow (pip install pillow) and run this again")
            return None
        except Exception as e:
            log(f"   warning: texture {nm} could not be shrunk ({type(e).__name__}: {e}): that part is drawn in its flat colour"); return None
    return data

def weld_prim(pr):
    """Shares identical corners (position, normal, uv, joints, weights) of a primitive: an OBJ's triangles come unshared,
    3 vertices each, and the game reads at most 65535 vertices a hand."""
    uv = pr["uv"] if pr["uv"] is not None else np.zeros((len(pr["pos"]), 2))
    rows = np.c_[pr["pos"], pr["nrm"], uv, pr["joints"].astype(float), pr["weights"].astype(float)]
    u, first, inv = np.unique(rows, axis=0, return_index=True, return_inverse=True)
    order = np.argsort(first); rank = np.empty_like(order); rank[order] = np.arange(len(order))   # keep first-use order
    keep = first[order]
    q = dict(pr); q["pos"] = pr["pos"][keep]; q["nrm"] = pr["nrm"][keep]; q["uv"] = pr["uv"][keep] if pr["uv"] is not None else None
    q["joints"] = pr["joints"][keep]; q["weights"] = pr["weights"][keep]; q["idx"] = rank[inv.reshape(-1)][pr["idx"]]
    return q

def find_texture(d, name):
    """A map_Kd name (maybe with options, a sub-folder or Windows separators) -> an existing file, also by name alone."""
    t = name.strip().strip('"')
    if t.startswith("-"): t = t.split()[-1]
    t = t.replace("\\", os.sep).replace("/", os.sep)
    for c in (os.path.join(d, t), os.path.join(d, os.path.basename(t))):
        if os.path.isfile(c): return c
    low = os.path.basename(t).lower()   # a case mismatch (models made on Windows, read elsewhere)
    try:
        for f in os.listdir(d):
            if f.lower() == low: return os.path.join(d, f)
    except OSError: pass
    return None

def build_pack_hand(o, hd, rig, s, out_dir, slot, texcache, debug):
    J = rig["J"]; Rb = rig["Rb"]; wc = rig["wrist"]
    to_c = lambda p: ((p - wc) @ Rb) * s
    Jc = to_c(J); rig["Wc"] = to_c(rig["W"])
    left = rig["handed"] == "left"
    Rj = H.rest_frames(Jc, left=left)
    Wt = skin(rig, Jc)
    j4, w4 = H.top4(Wt)
    tris = hd["tris"]; rep = rig["rep"].reshape(-1, 3)
    prims = []
    for m in sorted(set(hd["mats"]), key=lambda x: str(x)):
        sel = np.where(hd["mats"] == m)[0]
        if not len(sel): continue
        T = tris[sel]; pos = to_c(T[:, :, :3].reshape(-1, 3))
        uv = T[:, :, 3:5].reshape(-1, 2) * [1, -1] + [0, 1]          # OBJ v-up -> glTF v-down
        nrm = T[:, :, 5:8].reshape(-1, 3) @ Rb
        nl = np.linalg.norm(nrm, axis=1)
        idx = np.arange(len(pos)).reshape(-1, 3)
        if (nl < 0.5).any(): nrm = H.vertex_normals(pos, idx)
        else: nrm /= nl[:, None]
        wi = rep[sel].reshape(-1)
        tn = o["mats"].get(m, {}).get("map_Kd"); png = None; col = tuple(o["mats"].get(m, {}).get("Kd", [0.6] * 3)) + (1.0,)
        if tn:
            tp = find_texture(o["dir"], tn)
            if tp is None: log(f"   warning: texture {tn} (material {m}) not found beside the OBJ: that part is drawn in its flat colour {tuple(round(c, 2) for c in col[:3])}")
            else:
                if tp not in texcache: texcache[tp] = load_texture(tp)
                data = texcache[tp]
                if data is not None: png = data; col = (1, 1, 1, 1)
        prims.append(weld_prim(dict(name=m, pos=pos, nrm=nrm, uv=uv, joints=j4[wi], weights=w4[wi], idx=idx.reshape(-1),
                               color=col, texture_png=png)))
    # cap the wrist opening (fan from the cut centroid), weighted to the wrist
    cs = np.array(hd["cut_segs"])
    if len(cs):
        segs = to_c(cs[:, :, :3].reshape(-1, 3)).reshape(-1, 2, 3)
        # only segments of the main section: near the wrist centre
        r = np.linalg.norm(segs.mean(1)[:, :2], axis=1); segs = segs[r < np.percentile(r, 90) * 1.5]
        cen = segs.reshape(-1, 3).mean(0)
        pos = []; idx = []
        for a, b in segs:
            nrm_ = np.cross(a - cen, b - cen)
            if nrm_[2] < 0: a, b = b, a          # cap faces +Z (back toward the elbow)
            pos += [cen, a, b]; idx += [len(pos) - 3, len(pos) - 2, len(pos) - 1]
        pos = np.array(pos); idx = np.array(idx)
        jw = np.zeros((len(pos), 4), np.uint8); jw[:, 0] = H.J["wrist"]
        ww = np.zeros((len(pos), 4), np.float32); ww[:, 0] = 1
        prims.append(weld_prim(dict(name="wrist_cap", pos=pos, nrm=np.tile([0, 0, 1.0], (len(pos), 1)), uv=None,
                               joints=jw, weights=ww, idx=idx, color=(0.05, 0.05, 0.06, 1), texture_png=None)))
    # the runtime's limits (src/native/ffxvr_hands.h, hand_pack.cs): fail here rather than print "Done" for a pack the game refuses
    nv = sum(len(p["pos"]) for p in prims); nt = sum(len(p["idx"]) // 3 for p in prims)
    if nv > MAX_VERTS: fail(f"the {slot} hand has {nv} vertices after welding (the game reads at most {MAX_VERTS}): decimate the hand in Blender first")
    if nt * 3 > MAX_INDICES: fail(f"the {slot} hand has {nt} triangles (the game reads at most {MAX_INDICES // 3})")
    imgs = []
    for p in prims:
        if p["texture_png"] is not None and not any(p["texture_png"] is q for q in imgs): imgs.append(p["texture_png"])
    if len(imgs) > MAX_IMAGES:
        late = [p["name"] for p in prims if p["texture_png"] is not None and all(p["texture_png"] is not q for q in imgs[:MAX_IMAGES])]
        log(f"   warning: the {slot} hand uses {len(imgs)} textures; the game draws the first {MAX_IMAGES}, the rest in their flat colour ({', '.join(map(str, late))})")
    path = os.path.join(out_dir, f"{slot}.glb")
    H.write_glb(path, prims, Jc, Rj, name=f"{slot}_hand", generator="ffxvr import-hands.py")
    size = os.path.getsize(path)
    if size > MAX_GLB:
        os.remove(path); fail(f"{slot}.glb would be {size >> 20} MB, over the {MAX_GLB >> 20} MB the game reads: use smaller textures")
    log(f"   wrote {path}: {nv} verts, {nt} tris, {len(prims)} primitives, "
        f"{os.path.getsize(path)//1024} KiB; wrist to middle tip {np.linalg.norm(Jc[H.J['middle_tip']] - Jc[H.J['wrist']]):.3f} m")
    return Jc

def in_git_checkout(path):
    """The folder (or the nearest existing parent) is inside a git work tree: a .git folder, or a .git file (a worktree or
    a submodule), in it or any folder above it."""
    p = os.path.abspath(path)
    while True:
        if os.path.exists(os.path.join(p, ".git")): return p
        up = os.path.dirname(p)
        if up == p: return None
        p = up

def check_out_dir(out):
    """Refuses a folder in a git checkout, a shipped pack's folder, and a folder holding something that is not our own pack."""
    repo = in_git_checkout(out)
    if repo: fail(f"--out {out} is inside a git checkout ({repo}): a pack made from your model must never be committed. "
                  "Write it into the game's mod folder (tools\\import-hands.ps1 does), or another folder outside any repository.")
    name = os.path.basename(os.path.normpath(out)).lower().rstrip(" .")   # Windows drops trailing spaces and dots (review N7)
    if name != os.path.basename(os.path.normpath(out)).lower(): fail(f"--out {out}: a folder name may not end in a space or a dot")
    if name in ("generic", "generic-glove"): fail(f"--out {out}: that is a shipped pack's folder name; pick another (e.g. -Name tidus)")
    if os.path.isdir(out):
        man = os.path.join(out, "hand.json"); mine = os.path.exists(os.path.join(out, "PRIVATE-do-not-share.txt"))
        if os.path.exists(man):
            try: mine = mine or json.load(open(man, encoding="utf-8")).get("private") is True
            except (ValueError, OSError, AttributeError): pass
        others = [f for f in os.listdir(out) if not (f in ("hand.json", "right.glb", "left.glb", "PRIVATE-do-not-share.txt", "preview.png") or f.startswith("debug-"))]
        if (os.path.exists(man) and not mine) or (others and not mine):
            fail(f"--out {out} already holds something that is not a pack made by this tool ({', '.join((['hand.json'] if os.path.exists(man) else []) + others[:4])}); "
                 "pick another folder or -Name")

def parse_xyz(t):
    try:
        v = np.array([float(x) for x in t.split(",")])
        if v.shape == (3,) and np.isfinite(v).all(): return v
    except ValueError: pass
    raise argparse.ArgumentTypeError(f"'{t}' is not x,y,z")

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("obj", help="the model (.obj, its .mtl and textures beside it)")
    ap.add_argument("--out", required=True, help="the pack folder to write (normally <game>/fahrenheit/mods/ffxvr/hands/<name>)")
    ap.add_argument("--name", default=None, help="the name shown in F8")
    ap.add_argument("--hand-length", type=float, default=0.19, help="wrist to middle fingertip, metres (0.12 .. 0.30)")
    ap.add_argument("--keep-proportions", action="store_true", help="keep each hand's own wrist estimate (default: both hands' wrist -> middle tip = --hand-length)")
    ap.add_argument("--cut", type=float, default=None, help="the wrist cut for both hands (fraction of the model height back from the fingertips)")
    ap.add_argument("--cut-right", type=float, default=None); ap.add_argument("--cut-left", type=float, default=None)
    ap.add_argument("--flip-right", action="store_true", help="palm/back of hand detected the wrong way round")
    ap.add_argument("--flip-left", action="store_true")
    ap.add_argument("--tip", type=parse_xyz, action="append", default=None, help="x,y,z near a hand's fingertips (model units); up to twice")
    ap.add_argument("--height", type=float, default=None, help="model height in model units (default: its Y extent)")
    ap.add_argument("--z-up", action="store_true", help="the model is Z up")
    ap.add_argument("--only", choices=["right", "left"], default=None, help="use one hand, mirror it for the other")
    ap.add_argument("--debug", action="store_true")
    ap.add_argument("--preview", default=None, help="write open/fist/point/grip previews to this PNG (needs matplotlib)")
    a = ap.parse_args()
    np.seterr(all="ignore")
    try:
        run(a)
    except ImportError_ as e:
        log(f"error: {e}"); log("Nothing usable was written (an earlier pack in that folder, if any, is unchanged unless listed above)."); sys.exit(1)
    except OSError as e:   # review N9: --out names a file, no write permission, a disk full ...
        log(f"error: {e.strerror or e} ({e.filename or ''})"); sys.exit(1)
    except (np.linalg.LinAlgError, ValueError, IndexError, ZeroDivisionError) as e:
        log(f"error: the model could not be rigged ({type(e).__name__}: {e}). Try --cut-right / --cut-left, --tip or --height, "
            "or rig the hand in Blender (the pack format: tools/hands/handlib.py)."); sys.exit(1)

def run(a):
    if not (0.12 <= a.hand_length <= 0.30): fail("--hand-length is metres, wrist to middle fingertip: 0.12 .. 0.30 (default 0.19)")
    for k in ("cut", "cut_right", "cut_left"):
        v = getattr(a, k)
        if v is not None and not (0.03 <= v <= 0.25): fail(f"--{k.replace('_', '-')} {v}: a fraction of the model height, 0.03 .. 0.25 (try 0.09 .. 0.13)")
    if a.tip and len(a.tip) > 2: fail("--tip at most twice (one per hand)")
    out = os.path.abspath(a.out)
    check_out_dir(out)
    if a.preview:   # review B2: a render of the model follows the same rules as the pack: never into a git checkout
        if not os.path.dirname(a.preview): a.preview = os.path.join(out, a.preview)   # a bare name: into the pack folder
        a.preview = os.path.abspath(a.preview)
        if not a.preview.lower().endswith(".png"): fail(f"--preview {a.preview}: a .png file name")
        repo = in_git_checkout(os.path.dirname(a.preview))
        if repo: fail(f"--preview {a.preview} is inside a git checkout ({repo}): it is a picture of your model. Give a bare file name "
                      "(it goes into the pack folder) or a folder outside any repository")
        pdir = os.path.dirname(a.preview)
        if os.path.normcase(pdir) != os.path.normcase(out) and os.path.exists(os.path.join(pdir, "hand.json")):
            try: other_private = json.load(open(os.path.join(pdir, "hand.json"), encoding="utf-8")).get("private") is True
            except (ValueError, OSError, AttributeError): other_private = False
            if not other_private: fail(f"--preview {a.preview}: that folder is another hand pack (a shipped one?); give a bare file name")
    if not os.path.isfile(a.obj): fail(f"no such file: {a.obj}")
    if not a.obj.lower().endswith(".obj"): fail(f"{a.obj}: not a Wavefront .obj (export one from Blender: File > Export > Wavefront, Y up)")
    try: o = objio.load_obj(a.obj)
    except (OSError, ValueError, IndexError) as e: fail(f"{a.obj} could not be read as an OBJ: {e}")
    V = o["V"]
    if len(V) < 100 or len(o["faces"]) < 100: fail(f"{a.obj}: {len(V)} vertices, {len(o['faces'])} faces: not a full model")
    if a.z_up: V = o["V"] = V[:, [0, 2, 1]] * np.array([1, 1, -1]); o["N"] = o["N"][:, [0, 2, 1]] * np.array([1, 1, -1]) if len(o["N"]) else o["N"]
    ext = np.ptp(V, axis=0)
    height = float(a.height) if a.height else float(ext[1])
    if not height > 0: fail("the model has no height (--height)")
    if not a.height and not a.z_up and ext[2] > 1.3 * ext[1] and ext[2] > 0.6 * ext[0]: fail(f"the model is {ext[2]:.3g} deep but {ext[1]:.3g} tall: Z up? (--z-up)")
    os.makedirs(out, exist_ok=True)
    log(f"model: {len(V)} verts, {len(o['faces'])} tris, height {height:.4g} units (Y up), {len(o['mats'])} materials"
        + (f", {o['bad_faces']} broken faces dropped" if o.get("bad_faces") else ""))
    texcache = {}; got = {}
    ends = [(None, t) for t in a.tip] if a.tip else [(-1, None), (1, None)]
    for side, tip in ends:
        lab = (f"tip ({', '.join(f'{x:.4g}' for x in tip)})" if tip is not None else ("-X end" if side < 0 else "+X end"))
        log(f"hand at the {lab}:")
        hd = extract_hand(o, side or 1, a.cut, height, a.debug, tip)
        rig = rig_hand(hd, height, a.debug)
        slot = rig["handed"]
        cut = a.cut_right if slot == "right" else a.cut_left
        if cut is None: cut = a.cut
        flip = a.flip_right if slot == "right" else a.flip_left
        if (cut is not None and cut != hd["dcut"]) or flip:
            hd = extract_hand(o, side or 1, cut, height, a.debug, tip); rig = rig_hand(hd, height, a.debug, flip=flip); slot = rig["handed"]
        log(f"   wrist cut {hd['dcut']:.3f} x height back from the tip; a {slot} hand{' (palm side flipped)' if flip else ''}")
        if a.only and slot != a.only: log("   skipped (--only)"); continue
        if slot in got: log(f"   warning: two {slot} hands detected; keeping the first (--flip-right / --flip-left if a palm side is wrong)"); continue
        got[slot] = (hd, rig)
    if not got: fail("no hand kept")
    # one scale for both hands (else a mis-estimated wrist would make one hand bigger than the other)
    sc = float(np.mean([finger_scale(r, a.hand_length) for _, r in got.values()]))
    log(f"scale {sc:.4g} m per model unit (middle finger fitted to a {a.hand_length} m hand)")
    if not a.keep_proportions:   # both hands wrist -> middle tip = --hand-length (the model's own wrists: --keep-proportions)
        for slot, (hd, rig) in got.items():
            mv = normalise_wrist(rig, a.hand_length / sc)
            log(f"   {slot} wrist moved {mv * sc * 1000:+.1f} mm along the hand (wrist -> middle tip {a.hand_length} m; --keep-proportions keeps the model's)")
    for f in ("right.glb", "left.glb"):   # a stale hand of an earlier run must not be picked up
        if os.path.exists(os.path.join(out, f)) and f[:-4] not in got: os.remove(os.path.join(out, f))
    for slot, (hd, rig) in got.items():
        build_pack_hand(o, hd, rig, sc, out, slot, texcache, a.debug)
    src = os.path.basename(a.obj)
    import datetime
    with open(os.path.join(out, "PRIVATE-do-not-share.txt"), "w", encoding="utf-8", newline="\r\n") as f:
        f.write(PRIVATE_TXT.format(src=src, date=datetime.date.today().isoformat(), folder=out))
    man = {"format": 1, "name": (a.name or os.path.splitext(src)[0] + " hands")[:48], "author": "(your own model)",
           "licence": LICENCE, "private": True, "source": src[:80],
           "right": "right.glb" if "right" in got else None, "left": "left.glb" if "left" in got else None,
           "mirror": len(got) == 1, "scale": 1.0, "wrist_offset_m": [0, 0, 0], "wrist_rotation_deg": [0, 0, 0],
           "tint": [1, 1, 1], "rest_curl": {"thumb": 0, "index": 0, "middle": 0, "ring": 0, "little": 0}}
    with open(os.path.join(out, "hand.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(man, f, indent=2, ensure_ascii=False); f.write("\n")
    log(f"wrote {os.path.join(out, 'hand.json')} and PRIVATE-do-not-share.txt")
    previews = ([(a.preview, False)] if a.preview else []) + ([(os.path.join(out, "debug-preview.png"), True)] if a.debug else [])
    for path, bones in previews:
        try:
            import importlib.util
            spec = importlib.util.spec_from_file_location("preview_pack", os.path.join(os.path.dirname(os.path.abspath(__file__)), "preview-pack.py"))
            pp = importlib.util.module_from_spec(spec); spec.loader.exec_module(pp)
            sys.argv = ["preview-pack.py", out, path] + (["--bones"] if bones else []); pp.main()
        except ImportError as e: log(f"   (no preview: {e}; pip install matplotlib)")
        except Exception as e: log(f"   (no preview: {type(e).__name__}: {e})")   # review SF-2: a preview never fails a good pack
    log("Done. In game: F8 > VR hands > hand model > rescan, then pick it. Keep this pack on your PC: do not share it.")

if __name__ == "__main__":
    main()
