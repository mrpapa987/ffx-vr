#!/usr/bin/env python3
"""preview-pack.py - validate a hand pack and render rest/fist/point/grip previews (handlib's reference poses).
  python preview-pack.py <pack_dir> <out.png> [--size 300] [--bones]
SPDX-License-Identifier: MIT (part of the FFX VR repository). Needs Python 3, numpy and matplotlib.
"""
import argparse, json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import handlib as H

def load_pack(d):
    man = json.load(open(os.path.join(d, "hand.json")))
    hands = {}
    for slot in ("right", "left"):
        if man.get(slot): hands[slot] = H.read_glb(os.path.join(d, man[slot]))
    if man.get("mirror"):
        src = "right" if "right" in hands else "left"; dst = "left" if src == "right" else "right"
        if dst not in hands: hands[dst] = H.mirror_glb(hands[src])
    return man, hands

def posed(g, pose, left=False):
    """Poses are defined for the right hand; a left hand is mirrored into right space, posed, mirrored back."""
    if left: g = H.mirror_glb(g)
    bindW = g["bindW"]; P = bindW[:, :3, 3]; R = bindW[:, :3, :3]
    W = H.pose_world(P, R, **H.POSES[pose])
    out = []
    for pr in g["prims"]:
        v = H.skin_vertices(pr["pos"], pr["joints"], pr["weights"], bindW, W)
        tris = pr["idx"].reshape(-1, 3)
        if left: v = v * np.array([-1, 1, 1]); tris = tris[:, ::-1]
        tex = H.decode_png(pr["texture_png"]) if pr["texture_png"] is not None else None
        out.append((v, tris, pr["uv"], tex, pr["color"]))
    bones = W[:, :3, 3] * (np.array([-1, 1, 1]) if left else 1)
    return out, bones

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack"); ap.add_argument("out"); ap.add_argument("--size", type=int, default=300)
    ap.add_argument("--bones", action="store_true")
    a = ap.parse_args()
    man, hands = load_pack(a.pack)
    for k, g in hands.items():
        print(k, "| issues:", g["issues"] or "none", "| prims:",
              [(len(p["pos"]), len(p["idx"]) // 3, p["texture_png"] is not None) for p in g["prims"]])
    imgs = []; ext = None
    for slot in ("right", "left"):
        g = hands[slot]; lf = slot == "left"; sg = -1 if lf else 1
        views = [H.look_rot(0, np.pi / 2), H.look_rot(sg * np.pi / 2, 0.25), H.look_rot(sg * -0.6, -0.9)]
        for pose in ("rest", "fist", "point", "grip"):
            for VR in views:
                meshes, bones = posed(g, pose, left=lf)
                if ext is None:
                    allp = np.concatenate([m[0] for m in meshes]); cen = 0.5 * (allp.min(0) + allp.max(0))
                    ext = np.linalg.norm(allp - cen, axis=1).max() * 1.02
                imgs.append(H.render(meshes, VR, size=(a.size, a.size), bones=bones if a.bones else None,
                                     extent=ext, center=cen))
    R_, L_ = imgs[:12], imgs[12:]
    rows = [R_[i*3:(i+1)*3] + L_[i*3:(i+1)*3] for i in range(4)]
    H.save_png(a.out, H.grid([x for r in rows for x in r], 6))
    print("wrote", a.out, "(left half: RIGHT hand, right half: LEFT hand; rows rest/fist/point/grip; "
          "cols per hand: top (back of hand), thumb side, 3/4 from the palm)")

if __name__ == "__main__":
    main()
