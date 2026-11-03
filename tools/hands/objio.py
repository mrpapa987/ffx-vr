"""objio.py - minimal Wavefront OBJ/MTL reader for the hand importer (triangulates n-gons; keeps groups/materials).

SPDX-License-Identifier: MIT (part of the FFX VR repository; see LICENSE). Reads the file as text only: nothing in it is
executed. Faces naming a vertex, UV or normal that does not exist are dropped (counted in "bad_faces").
"""
import os
import numpy as np

def _floats(parts, n):
    try:
        v = [float(x) for x in parts[:n]]
    except ValueError:
        return None
    return v if len(v) == n and all(np.isfinite(v)) else None

def load_obj(path):
    V, T, N = [], [], []
    faces = []   # (group, material, [(vi, ti, ni)...])
    grp, mtl, mtllib = None, None, None
    bad = 0
    with open(path, encoding="latin-1") as f:
        for ln in f:
            if ln.startswith("v "):
                v = _floats(ln.split()[1:4], 3); V.append(v if v else [0.0, 0.0, 0.0])
            elif ln.startswith("vt "):
                v = _floats(ln.split()[1:3], 2); T.append(v if v else [0.0, 0.0])
            elif ln.startswith("vn "):
                v = _floats(ln.split()[1:4], 3); N.append(v if v else [0.0, 0.0, 0.0])
            elif ln.startswith(("g ", "o ")): grp = ln.split(None, 1)[1].strip() if len(ln.split()) > 1 else None
            elif ln.startswith("usemtl "): mtl = ln.split(None, 1)[1].strip()
            elif ln.startswith("mtllib ") and mtllib is None: mtllib = ln.split(None, 1)[1].strip()
            elif ln.startswith("f "):
                cs = []; ok = True
                for w in ln.split()[1:]:
                    p = (w.split("/") + ["", ""])[:3]
                    def ix(s, n):
                        if not s: return -1
                        i = int(s); i = i - 1 if i > 0 else n + i
                        if not (0 <= i < n): raise ValueError
                        return i
                    try: cs.append((ix(p[0], len(V)), ix(p[1], len(T)), ix(p[2], len(N))))
                    except ValueError: ok = False; break
                if not ok or len(cs) < 3 or cs[0][0] < 0: bad += 1; continue
                for k in range(1, len(cs) - 1):
                    faces.append((grp, mtl, [cs[0], cs[k], cs[k + 1]]))
    mats = {}
    if mtllib:
        mp = os.path.join(os.path.dirname(os.path.abspath(path)), mtllib.replace("\\", os.sep))
        if os.path.exists(mp):
            cur = None
            for ln in open(mp, encoding="latin-1"):
                s = ln.strip()
                if s.startswith("newmtl "): cur = s.split(None, 1)[1]; mats[cur] = {}
                elif s.startswith("map_Kd ") and cur: mats[cur]["map_Kd"] = s.split(None, 1)[1]
                elif s.startswith("Kd ") and cur:
                    v = _floats(s.split()[1:4], 3)
                    if v: mats[cur]["Kd"] = [min(max(x, 0.0), 1.0) for x in v]
    return dict(V=np.array(V, float).reshape(-1, 3), T=np.array(T, float).reshape(-1, 2), N=np.array(N, float).reshape(-1, 3),
                faces=faces, mats=mats, dir=os.path.dirname(os.path.abspath(path)), bad_faces=bad)
