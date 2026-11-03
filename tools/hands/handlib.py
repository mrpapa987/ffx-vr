"""handlib.py - shared code for FFX VR hand packs (canonical skeleton, glb I/O, posing, preview).

Pure Python 3 + numpy. Preview rendering additionally uses matplotlib (only to decode/encode PNG).
SPDX-License-Identifier: MIT (part of the FFX VR repository; see LICENSE).

Conventions (the pack contract the native renderer relies on):
  * Units: metres.  Space: the RIGHT hand's OpenXR wrist-joint space at the bind pose:
      origin = wrist joint, -Z = toward the fingertips, +Y = out of the back of the hand,
      +X = Y x Z (so for a right hand the thumb sits on -X).  The left hand is the X-mirror.
  * Skeleton: the 26 OpenXR XR_EXT_hand_tracking joints, in XrHandJointEXT order, with the
    joint frames OpenXR defines (+Z points away from the fingertip along the bone, +Y dorsal).
  * Bind pose: flat, fully open hand (OpenXR's "fully opened hand pose").
"""
import io, json, struct, zlib
import numpy as np

# ----------------------------------------------------------------------------------------------
# Canonical skeleton
# ----------------------------------------------------------------------------------------------
JOINTS = [
    "palm", "wrist",
    "thumb_metacarpal", "thumb_proximal", "thumb_distal", "thumb_tip",
    "index_metacarpal", "index_proximal", "index_intermediate", "index_distal", "index_tip",
    "middle_metacarpal", "middle_proximal", "middle_intermediate", "middle_distal", "middle_tip",
    "ring_metacarpal", "ring_proximal", "ring_intermediate", "ring_distal", "ring_tip",
    "little_metacarpal", "little_proximal", "little_intermediate", "little_distal", "little_tip",
]
J = {n: i for i, n in enumerate(JOINTS)}
PARENT = [1, -1,            # palm -> wrist, wrist = root
          1, 2, 3, 4,       # thumb
          1, 6, 7, 8, 9,    # index
          1, 11, 12, 13, 14,
          1, 16, 17, 18, 19,
          1, 21, 22, 23, 24]
FINGERS = {  # chain of joints from metacarpal to tip
    "thumb":  [2, 3, 4, 5],
    "index":  [6, 7, 8, 9, 10],
    "middle": [11, 12, 13, 14, 15],
    "ring":   [16, 17, 18, 19, 20],
    "little": [21, 22, 23, 24, 25],
}

# Default (generic) bind-pose joint positions, right hand, metres. Adult hand ~19 cm wrist->middle tip.
def _chain(base, knuckle, lens, droop=0.0):
    base = np.asarray(base, float); knuckle = np.asarray(knuckle, float)
    d = knuckle - base; d /= np.linalg.norm(d)
    pts = [base, knuckle]; p = knuckle.copy()
    for L in lens:
        d2 = d + np.array([0, -droop, 0]); d2 /= np.linalg.norm(d2)
        p = p + d2 * L; pts.append(p)
    return pts

def default_rest_positions():
    """The generic hand's bind joints. Finger bones (proximal, intermediate, distal + a 4.5 mm tip pad) follow the mean
    phalanx lengths of Buryanov & Kotiuk, "Proportions of Hand Segments", Int. J. Morphol. 28(3), 2010 (X-rays of 66
    adults: index 39.8/22.4/15.8, middle 44.6/26.3/17.4, ring 41.4/25.7/17.3, little 32.7/18.1/16.0 mm), scaled by
    190/193 (ANSUR II's median male hand length is 193 mm). The knuckles sit 4 mm further out than the S2 hand so that
    wrist -> middle tip stays 0.19 m (wrist -> MCP III 0.098 m = 0.52 of the hand length, as 193 - 93 mm)."""
    P = np.zeros((26, 3))
    P[J["wrist"]] = (0, 0, 0)
    fingers = {
        #           metacarpal base(CMC)        knuckle(MCP)               proximal, intermediate, distal (+ pad)
        "index":  ((-0.017, 0.004, -0.024), (-0.024, 0.003, -0.094), (0.0392, 0.0220, 0.0201)),
        "middle": ((-0.004, 0.005, -0.025), (-0.004, 0.004, -0.0986), (0.0439, 0.0259, 0.0216)),
        "ring":   (( 0.008, 0.004, -0.024), ( 0.015, 0.002, -0.092), (0.0407, 0.0253, 0.0215)),
        "little": (( 0.018, 0.001, -0.022), ( 0.032, -0.002, -0.082), (0.0322, 0.0178, 0.0202)),
    }
    for f, (b, k, L) in fingers.items():
        for jj, p in zip(FINGERS[f], _chain(b, k, L, droop=0.02)):
            P[jj] = p
    # thumb: CMC near the wrist on the radial side, pointing forward-out and slightly palmar
    tb = np.array([-0.022, -0.010, -0.018]); tdir = np.array([-0.60, -0.14, -0.79]); tdir /= np.linalg.norm(tdir)
    P[2] = tb; P[3] = tb + tdir * 0.040; P[4] = P[3] + tdir * 0.032; P[5] = P[4] + tdir * 0.027
    P[J["palm"]] = 0.5 * (P[11] + P[12])  # centre of the middle metacarpal
    return P

THUMB_NAIL_HINT = np.array([-0.75, 0.66, 0.0])   # thumbnail faces out and up at the open pose (right hand)

def rest_frames(P, left=False, up=(0.0, 1.0, 0.0), thumb_nail=None):
    """Joint rotation matrices (columns = local X,Y,Z in hand space) following OpenXR conventions.
    left=True: P is in the LEFT hand's wrist space (thumb on +X)."""
    R = np.zeros((26, 3, 3))
    def frame(z, yhint):
        z = z / np.linalg.norm(z)
        y = yhint - z * np.dot(yhint, z); y /= np.linalg.norm(y)
        x = np.cross(y, z)
        return np.stack([x, y, z], axis=1)
    up = np.asarray(up, float)
    tn = np.asarray(THUMB_NAIL_HINT if thumb_nail is None else thumb_nail, float)
    if left and thumb_nail is None: tn = tn * np.array([-1, 1, 1])
    for f, ch in FINGERS.items():
        yh = tn / np.linalg.norm(tn) if f == "thumb" else up
        for a, b in zip(ch[:-1], ch[1:]):
            R[a] = frame(P[a] - P[b], yh)
        R[ch[-1]] = R[ch[-2]]
    R[J["wrist"]] = frame(-(P[J["middle_proximal"]] - P[J["wrist"]]), up)
    R[J["palm"]] = R[J["middle_metacarpal"]]
    return R

def world_mats(P, R):
    M = np.tile(np.eye(4), (26, 1, 1)); M[:, :3, :3] = R; M[:, :3, 3] = P
    return M

# ----------------------------------------------------------------------------------------------
# Procedural poses (what a controller-driven runtime would do): per-joint flexion about local X
# ----------------------------------------------------------------------------------------------
THUMB_OPPOSE = np.array([-0.20, -0.05, -0.80])   # metacarpal direction at full opposition (right hand)

def arc(a, b):
    """Minimal rotation taking unit-ish vector a onto b."""
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b)
    v = np.cross(a, b); c = float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-9: return np.eye(3)
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K * (1 / (1 + c))

def rotx(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
def roty(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])

def pose_world(P, R, curls, thumb_curl=0.0, thumb_opp=0.0):
    """curls: dict finger -> 0..1 curl. Returns posed 4x4 joint world matrices (bind-space -> posed).
    Flexion is a rotation about the joint's local X (negative angle = toward the palm)."""
    rest = world_mats(P, R)
    local = np.zeros((26, 4, 4))
    for j in range(26):
        p = PARENT[j]
        local[j] = rest[j] if p < 0 else np.linalg.inv(rest[p]) @ rest[j]
    flex = np.zeros((26, 3, 3)); flex[:] = np.eye(3)
    maxang = {"proximal": 1.45, "intermediate": 1.75, "distal": 1.20}
    for f, ch in FINGERS.items():
        if f == "thumb":
            # thumb: opposition swings the metacarpal toward THUMB_OPPOSE (in the wrist frame), then the
            # proximal/distal joints flex about their local X (toward the thumb pad, i.e. across the palm)
            Rm = R[ch[0]]; d0 = -Rm[:, 2]
            d1 = d0 + (THUMB_OPPOSE / np.linalg.norm(THUMB_OPPOSE) - d0) * thumb_opp
            Rw = arc(d0, d1) @ Rm                               # new world frame of the metacarpal
            flex[ch[0]] = Rm.T @ Rw @ rotx(-0.15 * thumb_curl)  # expressed in the joint's own frame
            flex[ch[1]] = rotx(-0.55 * thumb_curl)
            flex[ch[2]] = rotx(-0.90 * thumb_curl)
            continue
        c = curls.get(f, 0.0)
        flex[ch[1]] = rotx(-maxang["proximal"] * c)
        flex[ch[2]] = rotx(-maxang["intermediate"] * c)
        flex[ch[3]] = rotx(-maxang["distal"] * c)
    W = np.zeros((26, 4, 4))
    order = [1, 0] + list(range(2, 26))
    for j in order:
        L = local[j].copy(); F = np.eye(4); F[:3, :3] = flex[j]
        # rotation applied in the joint's own frame, after its offset from the parent
        L = L @ F
        W[j] = L if PARENT[j] < 0 else W[PARENT[j]] @ L
    return W

POSES = {
    "rest":  dict(curls={}, thumb_curl=0.0, thumb_opp=0.0),
    "fist":  dict(curls={"index": 1, "middle": 1, "ring": 1, "little": 1}, thumb_curl=1.0, thumb_opp=1.0),
    "point": dict(curls={"index": 0.0, "middle": 1, "ring": 1, "little": 1}, thumb_curl=0.8, thumb_opp=1.0),
    "grip":  dict(curls={"index": 0.55, "middle": 0.6, "ring": 0.62, "little": 0.65}, thumb_curl=0.5, thumb_opp=0.6),
}

def skin_vertices(pos, joints4, weights4, bindW, poseW):
    """Linear blend skinning. joints4/weights4: (N,4). Returns posed positions."""
    S = np.einsum("jab,jbc->jac", poseW, np.linalg.inv(bindW))        # 26 skinning matrices
    hp = np.c_[pos, np.ones(len(pos))]
    out = np.zeros((len(pos), 3))
    for k in range(4):
        Mk = S[joints4[:, k]]                                            # (N,4,4)
        out += weights4[:, k:k + 1] * np.einsum("nab,nb->na", Mk, hp)[:, :3]
    return out

def mirror_glb(g):
    """Mirror a read_glb() result across X (left <-> right hand space): positions, normals, winding, bind."""
    S4 = np.diag([-1.0, 1.0, 1.0, 1.0]); out = dict(g)
    out["bindW"] = np.einsum("ab,jbc,cd->jad", S4, g["bindW"], S4)
    prims = []
    for pr in g["prims"]:
        q = dict(pr); q["pos"] = pr["pos"] * [-1, 1, 1]; q["nrm"] = pr["nrm"] * [-1, 1, 1]
        q["idx"] = pr["idx"].reshape(-1, 3)[:, ::-1].reshape(-1); prims.append(q)
    out["prims"] = prims
    return out

def mirror_x(P=None, R=None):
    """Left-hand data from right-hand data: reflect X. Rotations: S R S keeps them proper and keeps
    OpenXR's axis meaning (+Y dorsal, +Z away from tip) for the left hand."""
    S = np.diag([-1.0, 1.0, 1.0])
    out = []
    if P is not None: out.append(P * np.array([-1, 1, 1]))
    if R is not None: out.append(np.einsum("ab,jbc,cd->jad", S, R, S))
    return out

# ----------------------------------------------------------------------------------------------
# glTF 2.0 binary writer / reader (just what a hand pack needs)
# ----------------------------------------------------------------------------------------------
def _quat_from_mat(m):
    t = np.trace(m)
    if t > 0:
        s = np.sqrt(t + 1.0) * 2; w = 0.25 * s
        x = (m[2, 1] - m[1, 2]) / s; y = (m[0, 2] - m[2, 0]) / s; z = (m[1, 0] - m[0, 1]) / s
    else:
        i = int(np.argmax(np.diag(m)))
        if i == 0:
            s = np.sqrt(1 + m[0, 0] - m[1, 1] - m[2, 2]) * 2
            w = (m[2, 1] - m[1, 2]) / s; x = 0.25 * s; y = (m[0, 1] + m[1, 0]) / s; z = (m[0, 2] + m[2, 0]) / s
        elif i == 1:
            s = np.sqrt(1 + m[1, 1] - m[0, 0] - m[2, 2]) * 2
            w = (m[0, 2] - m[2, 0]) / s; x = (m[0, 1] + m[1, 0]) / s; y = 0.25 * s; z = (m[1, 2] + m[2, 1]) / s
        else:
            s = np.sqrt(1 + m[2, 2] - m[0, 0] - m[1, 1]) * 2
            w = (m[1, 0] - m[0, 1]) / s; x = (m[0, 2] + m[2, 0]) / s; y = (m[1, 2] + m[2, 1]) / s; z = 0.25 * s
    q = np.array([x, y, z, w]); return q / np.linalg.norm(q)

def write_glb(path, prims, P, R, name="hand", generator="ffxvr handlib", copyright=None):
    """prims: list of dicts {pos (N,3) f32, nrm (N,3), uv (N,2) or None, joints (N,4) u8, weights (N,4) f32,
    idx (M,) u16/u32, color (r,g,b,a), texture_png bytes or None, name}. Bind pose = (P,R)."""
    bin_ = bytearray(); views = []; accs = []
    def add_view(data, target=None):
        while len(bin_) % 4: bin_.append(0)
        off = len(bin_); bin_.extend(data)
        v = {"buffer": 0, "byteOffset": off, "byteLength": len(data)}
        if target: v["target"] = target
        views.append(v); return len(views) - 1
    def add_acc(arr, ctype, typ, target=None, minmax=False, normalized=False):
        arr = np.ascontiguousarray(arr)
        vi = add_view(arr.tobytes(), target)
        a = {"bufferView": vi, "componentType": ctype, "count": int(arr.shape[0]), "type": typ}
        if normalized: a["normalized"] = True
        if minmax:
            a["min"] = [float(x) for x in arr.reshape(arr.shape[0], -1).min(0)]
            a["max"] = [float(x) for x in arr.reshape(arr.shape[0], -1).max(0)]
        accs.append(a); return len(accs) - 1
    images, textures, materials, gprims, samplers = [], [], [], [], []
    seen_img = {}
    for pr in prims:
        attrs = {
            "POSITION": add_acc(pr["pos"].astype(np.float32), 5126, "VEC3", 34962, minmax=True),
            "NORMAL": add_acc(pr["nrm"].astype(np.float32), 5126, "VEC3", 34962),
            "JOINTS_0": add_acc(pr["joints"].astype(np.uint8), 5121, "VEC4", 34962),
            "WEIGHTS_0": add_acc(pr["weights"].astype(np.float32), 5126, "VEC4", 34962),
        }
        mat = {"name": pr.get("name", "mat"), "pbrMetallicRoughness": {
            "baseColorFactor": list(pr.get("color", (1, 1, 1, 1))), "metallicFactor": 0.0, "roughnessFactor": 0.8}}
        if pr.get("uv") is not None:
            attrs["TEXCOORD_0"] = add_acc(pr["uv"].astype(np.float32), 5126, "VEC2", 34962)
        if pr.get("texture_png") is not None:   # PNG or JPEG bytes (the name is historical); one image per distinct file
            if not samplers: samplers.append({"magFilter": 9729, "minFilter": 9987, "wrapS": 10497, "wrapT": 10497})
            data = bytes(pr["texture_png"])
            if data not in seen_img:
                iv = add_view(data)
                mime = "image/jpeg" if data[:2] == b"\xff\xd8" else "image/png"
                images.append({"bufferView": iv, "mimeType": mime, "name": pr.get("name", "tex")})
                textures.append({"sampler": 0, "source": len(images) - 1}); seen_img[data] = len(textures) - 1
            mat["pbrMetallicRoughness"]["baseColorTexture"] = {"index": seen_img[data]}
        materials.append(mat)
        idx = pr["idx"].astype(np.uint16 if pr["pos"].shape[0] < 65536 else np.uint32)
        ia = add_acc(idx, 5123 if idx.dtype == np.uint16 else 5125, "SCALAR", 34963)
        gprims.append({"attributes": attrs, "indices": ia, "material": len(materials) - 1})
    bindW = world_mats(P, R)
    ibm = np.linalg.inv(bindW).transpose(0, 2, 1).astype(np.float32)  # column-major
    ibm_acc = add_acc(ibm.reshape(26, 16), 5126, "MAT4")
    # nodes: 0 = mesh node, 1.. = joints (node 1+j = joint j)
    nodes = [{"name": name, "mesh": 0, "skin": 0}]
    for j in range(26):
        p = PARENT[j]
        L = bindW[j] if p < 0 else np.linalg.inv(bindW[p]) @ bindW[j]
        n = {"name": JOINTS[j], "translation": [float(x) for x in L[:3, 3]],
             "rotation": [float(x) for x in _quat_from_mat(L[:3, :3])]}
        kids = [1 + c for c in range(26) if PARENT[c] == j]
        if kids: n["children"] = kids
        nodes.append(n)
    gltf = {
        "asset": dict({"version": "2.0", "generator": generator}, **({"copyright": copyright} if copyright else {})),
        "scene": 0, "scenes": [{"nodes": [0, 1 + J["wrist"]]}],
        "nodes": nodes, "meshes": [{"name": name, "primitives": gprims}],
        "skins": [{"joints": [1 + j for j in range(26)], "inverseBindMatrices": ibm_acc, "skeleton": 1 + J["wrist"]}],
        "accessors": accs, "bufferViews": views, "buffers": [{"byteLength": 0}], "materials": materials,
    }
    if images: gltf.update(images=images, textures=textures, samplers=samplers)
    while len(bin_) % 4: bin_.append(0)
    gltf["buffers"][0]["byteLength"] = len(bin_)
    js = json.dumps(gltf, separators=(",", ":")).encode()
    js += b" " * ((4 - len(js) % 4) % 4)
    total = 12 + 8 + len(js) + 8 + len(bin_)
    with open(path, "wb") as f:
        f.write(struct.pack("<III", 0x46546C67, 2, total))
        f.write(struct.pack("<II", len(js), 0x4E4F534A)); f.write(js)
        f.write(struct.pack("<II", len(bin_), 0x004E4942)); f.write(bin_)

_CT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
_NC = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}

def read_glb(path):
    """Reads a hand-pack glb and checks the contract. Returns dict(prims, bindW, joint_names, issues)."""
    data = open(path, "rb").read()
    issues = []
    magic, ver, total = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or ver != 2: raise ValueError("not a glTF 2.0 binary")
    jl, jt = struct.unpack_from("<II", data, 12); js = json.loads(data[20:20 + jl])
    bl, bt = struct.unpack_from("<II", data, 20 + jl); bin_ = data[28 + jl:28 + jl + bl]
    def acc(i):
        a = js["accessors"][i]; v = js["bufferViews"][a["bufferView"]]
        n = _NC[a["type"]]; dt = _CT[a["componentType"]]
        off = v.get("byteOffset", 0) + a.get("byteOffset", 0)
        arr = np.frombuffer(bin_, dt, a["count"] * n, off).reshape(a["count"], n) if n > 1 else \
              np.frombuffer(bin_, dt, a["count"], off)
        return arr
    skin = js["skins"][0]
    names = [js["nodes"][n].get("name", "") for n in skin["joints"]]
    if names != JOINTS: issues.append("joint names/order differ from the OpenXR 26-joint set")
    ibm = acc(skin["inverseBindMatrices"]).reshape(-1, 4, 4).transpose(0, 2, 1)
    bindW = np.linalg.inv(ibm)
    prims = []
    for p in js["meshes"][0]["primitives"]:
        a = p["attributes"]
        pr = {"pos": acc(a["POSITION"]).astype(float), "nrm": acc(a["NORMAL"]).astype(float),
              "joints": acc(a["JOINTS_0"]).astype(int), "weights": acc(a["WEIGHTS_0"]).astype(float),
              "idx": acc(p["indices"]).astype(int), "uv": acc(a["TEXCOORD_0"]).astype(float) if "TEXCOORD_0" in a else None}
        m = js["materials"][p["material"]]["pbrMetallicRoughness"]
        pr["color"] = m.get("baseColorFactor", [1, 1, 1, 1]); pr["texture_png"] = None
        if "baseColorTexture" in m:
            img = js["images"][js["textures"][m["baseColorTexture"]["index"]]["source"]]
            v = js["bufferViews"][img["bufferView"]]
            pr["texture_png"] = bin_[v.get("byteOffset", 0):v.get("byteOffset", 0) + v["byteLength"]]
        ws = pr["weights"].sum(1)
        if np.abs(ws - 1).max() > 1e-3: issues.append(f"weights not normalised (max err {np.abs(ws-1).max():.3g})")
        if pr["joints"].max() >= 26: issues.append("joint index out of range")
        if pr["idx"].max() >= len(pr["pos"]): issues.append("index out of range")
        prims.append(pr)
    return dict(prims=prims, bindW=bindW, joint_names=names, issues=issues, json=js)

# ----------------------------------------------------------------------------------------------
# Geometry helpers
# ----------------------------------------------------------------------------------------------
def vertex_normals(pos, tris):
    n = np.zeros_like(pos)
    fn = np.cross(pos[tris[:, 1]] - pos[tris[:, 0]], pos[tris[:, 2]] - pos[tris[:, 0]])
    for k in range(3): np.add.at(n, tris[:, k], fn)
    l = np.linalg.norm(n, axis=1, keepdims=True); l[l == 0] = 1
    return n / l

def top4(W):
    """(N,26) weights -> joints4 u8, weights4 normalised."""
    idx = np.argsort(-W, axis=1)[:, :4]
    w = np.take_along_axis(W, idx, 1); w = np.maximum(w, 0)
    s = w.sum(1, keepdims=True); s[s == 0] = 1
    return idx.astype(np.uint8), (w / s).astype(np.float32)

# ----------------------------------------------------------------------------------------------
# Tiny software rasteriser for previews (z-buffer, per-pixel UV texturing, Lambert)
# ----------------------------------------------------------------------------------------------
def decode_png(b):
    """PNG or JPEG bytes (by content) -> float RGB. Pillow (which matplotlib needs anyway), else matplotlib's PNG reader."""
    try:
        from PIL import Image
        im = np.asarray(Image.open(io.BytesIO(b)).convert("RGB"), dtype=np.float64) / 255.0
        return im
    except ImportError:
        pass
    import matplotlib.image as mpimg
    im = mpimg.imread(io.BytesIO(b), format="png")
    if im.dtype != np.float32 and im.dtype != np.float64: im = im / 255.0
    if im.ndim == 2: im = np.stack([im] * 3, -1)
    return im[..., :3]

def look_rot(yaw, pitch):
    cy, sy, cp, sp = np.cos(yaw), np.sin(yaw), np.cos(pitch), np.sin(pitch)
    Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]); Rx = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]])
    return Rx @ Ry

def render(meshes, view_R, size=(420, 420), bg=(0.93, 0.93, 0.95), bones=None, extent=None, center=None,
           light=(0.3, 0.6, 0.75)):
    """meshes: list of (pos (N,3), tris (M,3), uv or None, tex image or None, color rgb).
    Orthographic. view_R rotates world -> view (x right, y up, z toward viewer)."""
    Wd, Ht = size
    allp = np.concatenate([m[0] for m in meshes]) @ view_R.T
    c = allp.mean(0) if center is None else np.asarray(center) @ view_R.T
    ext = (np.abs(allp[:, :2] - c[:2]).max() * 1.12) if extent is None else extent
    sc = min(Wd, Ht) / (2 * ext)
    img = np.ones((Ht, Wd, 3)) * np.array(bg); zb = np.full((Ht, Wd), -np.inf)
    L = np.asarray(light, float); L /= np.linalg.norm(L)
    for pos, tris, uv, tex, col in meshes:
        v = pos @ view_R.T
        sx = (v[:, 0] - c[0]) * sc + Wd / 2; sy = -(v[:, 1] - c[1]) * sc + Ht / 2; sz = v[:, 2]
        fn = np.cross(v[tris[:, 1]] - v[tris[:, 0]], v[tris[:, 2]] - v[tris[:, 0]])
        fl = np.linalg.norm(fn, axis=1); fl[fl == 0] = 1; fn /= fl[:, None]
        shade = 0.35 + 0.65 * np.abs(fn @ L)
        for t in range(len(tris)):
            i0, i1, i2 = tris[t]
            x0, x1, x2 = sx[i0], sx[i1], sx[i2]; y0, y1, y2 = sy[i0], sy[i1], sy[i2]
            xmin = max(int(np.floor(min(x0, x1, x2))), 0); xmax = min(int(np.ceil(max(x0, x1, x2))), Wd - 1)
            ymin = max(int(np.floor(min(y0, y1, y2))), 0); ymax = min(int(np.ceil(max(y0, y1, y2))), Ht - 1)
            if xmin > xmax or ymin > ymax: continue
            den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
            if abs(den) < 1e-12: continue
            gx, gy = np.meshgrid(np.arange(xmin, xmax + 1) + 0.5, np.arange(ymin, ymax + 1) + 0.5)
            a = ((y1 - y2) * (gx - x2) + (x2 - x1) * (gy - y2)) / den
            b = ((y2 - y0) * (gx - x2) + (x0 - x2) * (gy - y2)) / den
            g = 1 - a - b
            m = (a >= -1e-6) & (b >= -1e-6) & (g >= -1e-6)
            if not m.any(): continue
            z = a * sz[i0] + b * sz[i1] + g * sz[i2]
            sub = zb[ymin:ymax + 1, xmin:xmax + 1]
            m &= z > sub
            if not m.any(): continue
            sub[m] = z[m]
            if tex is not None and uv is not None:
                u = a * uv[i0, 0] + b * uv[i1, 0] + g * uv[i2, 0]
                w = a * uv[i0, 1] + b * uv[i1, 1] + g * uv[i2, 1]
                th, tw = tex.shape[:2]
                tx = np.clip(((u % 1.0) * tw).astype(int), 0, tw - 1)
                ty = np.clip(((w % 1.0) * th).astype(int), 0, th - 1)   # glTF: v down from top
                cc = tex[ty[m], tx[m]] * np.asarray(col)[:3]
            else:
                cc = np.asarray(col)[:3]
            img[ymin:ymax + 1, xmin:xmax + 1][m] = cc * shade[t]
    if bones is not None:   # overlay joints/bones as lines (drawn on top)
        Pv = bones @ view_R.T
        bx = (Pv[:, 0] - c[0]) * sc + Wd / 2; by = -(Pv[:, 1] - c[1]) * sc + Ht / 2
        for j in range(26):
            p = PARENT[j]
            if p < 0 or j == 0: continue
            n = int(max(abs(bx[j] - bx[p]), abs(by[j] - by[p]))) + 1
            for s in np.linspace(0, 1, n):
                xx = int(bx[p] + (bx[j] - bx[p]) * s); yy = int(by[p] + (by[j] - by[p]) * s)
                if 0 <= xx < Wd and 0 <= yy < Ht: img[yy, xx] = (0.9, 0.1, 0.1)
        for j in range(26):
            xx, yy = int(bx[j]), int(by[j])
            if 2 <= xx < Wd - 2 and 2 <= yy < Ht - 2: img[yy - 2:yy + 3, xx - 2:xx + 3] = (0.1, 0.3, 0.9)
    return img

def save_png(path, img):
    import matplotlib.image as mpimg
    mpimg.imsave(path, np.clip(img, 0, 1))

def grid(imgs, cols):
    h, w = imgs[0].shape[:2]; rows = (len(imgs) + cols - 1) // cols
    out = np.ones((rows * h, cols * w, 3))
    for i, im in enumerate(imgs):
        r, c = divmod(i, cols); out[r * h:(r + 1) * h, c * w:(c + 1) * w] = im
    return out
