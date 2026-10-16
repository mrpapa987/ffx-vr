# analyse_rdc.py  (v4) - pinpoint the camera
#
# v3 found 989 rotation-like matrices across 82 vertex shader constant buffers,
# with translation in the LAST COLUMN (so earlier row-translation tests missed
# them entirely).
#
# Most of those are per-object world matrices. The view matrix is the one that
# sits at the SAME byte offset with the SAME value in every buffer of a frame,
# because every object is viewed from one camera. That is what this looks for.
#
# Run from RenderDoc's Python Shell:
#     exec(open(r"C:\projects\FFX-VR\tools\analyse_rdc.py").read())
#
# Output: C:\projects\FFX-VR\logs\rdc_camera.txt

import os, math, struct
from collections import defaultdict
import renderdoc as rd

OUT = r"C:\projects\FFX-VR\logs\rdc_camera.txt"
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fh = open(OUT, "w", encoding="utf-8")
def log(s=""):
    print(s); fh.write(str(s) + "\n")

def pick(o, *names):
    for n in names:
        if hasattr(o, n): return getattr(o, n)
    return None

def is_rot(m, tol=0.02):
    rows = [[m[r*4+c] for c in range(3)] for r in range(3)]
    for r in rows:
        if abs(math.sqrt(sum(x*x for x in r)) - 1.0) > tol: return False
    for i in range(3):
        for j in range(i+1, 3):
            if abs(sum(rows[i][k]*rows[j][k] for k in range(3))) > tol: return False
    return True

def cam_from_view(m):
    """Translation in the last column: world position = -R^T t."""
    t0, t1, t2 = m[3], m[7], m[11]
    return (-(m[0]*t0 + m[4]*t1 + m[8]*t2),
            -(m[1]*t0 + m[5]*t1 + m[9]*t2),
            -(m[2]*t0 + m[6]*t1 + m[10]*t2))

def looks_projective(m):
    """Perspective projection: a +/-1 in the w row/column and a 0 in m[15]."""
    return abs(m[15]) < 1e-6 and (abs(abs(m[11]) - 1.0) < 1e-3 or abs(abs(m[14]) - 1.0) < 1e-3)

def draws_of(acts, out):
    for a in acts:
        try:
            if a.flags & rd.ActionFlags.Drawcall: out.append(a)
        except Exception: pass
        if getattr(a, "children", None): draws_of(a.children, out)
    return out

def main():
    path = None
    for d in (r"C:\projects\FFX-VR\_staging",
              os.path.expanduser(r"~\Documents"),
              os.path.expanduser(r"~\AppData\Local\Temp\RenderDoc")):
        if os.path.isdir(d):
            for f in os.listdir(d):
                if f.lower().endswith(".rdc"):
                    p = os.path.join(d, f)
                    if path is None or os.path.getmtime(p) > os.path.getmtime(path): path = p
    log(f"capture: {path}")

    cap = rd.OpenCaptureFile()
    ok = getattr(rd, "ResultCode", None)
    good = ok.Succeeded if ok else rd.ReplayStatus.Succeeded
    if cap.OpenFile(path, "", None) != good: raise SystemExit("open failed")
    res = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = res[1] if isinstance(res, tuple) else res

    try:
        draws = draws_of(controller.GetRootActions(), [])
        log(f"draw calls: {len(draws)}")

        bufs = {}
        for draw in draws:
            controller.SetFrameEvent(draw.eventId, True)
            ps = controller.GetPipelineState()
            try: blocks = ps.GetConstantBlocks(rd.ShaderStage.Vertex)
            except Exception: continue
            for blk in (blocks or []):
                src = blk.descriptor if hasattr(blk, "descriptor") else blk
                rid = pick(src, "resourceId", "resource", "buffer")
                off = int(pick(src, "byteOffset", "offset") or 0)
                size = int(pick(src, "byteSize", "size") or 0) or 8192
                if rid is None: continue
                key = (str(rid), off, size)
                if key in bufs: continue
                try: data = controller.GetBufferData(rid, off, size)
                except Exception: continue
                if not data or len(data) < 64: continue
                n = len(data)//4
                bufs[key] = list(struct.unpack_from("<%df" % n, bytes(data), 0))

        log(f"distinct VS constant buffers: {len(bufs)}")

        # group buffers by size; the dominant group is the per-draw object buffer
        bysize = defaultdict(list)
        for k, f in bufs.items(): bysize[len(f)].append((k, f))
        log("\nbuffers by float count: " +
            ", ".join(f"{s}:{len(v)}" for s, v in sorted(bysize.items(), key=lambda kv: -len(kv[1]))[:8]))

        # consider all buffers >= 256 floats together
        big = [(k, f) for k, f in bufs.items() if len(f) >= 256]
        log(f"\nlarge buffers considered: {len(big)}")

        # at each 16-float-aligned offset, how many buffers agree on the value?
        agree = defaultdict(lambda: defaultdict(list))
        for k, f in big:
            for blk in range(len(f)//16):
                m = f[blk*16:(blk+1)*16]
                if any(math.isnan(x) or math.isinf(x) for x in m): continue
                if all(abs(x) < 1e-9 for x in m): continue
                agree[blk*64][tuple(round(x,3) for x in m)].append(k)

        rows = []
        for byte_off, variants in agree.items():
            best = max(variants.items(), key=lambda kv: len(kv[1]))
            rows.append((len(best[1]), len(variants), byte_off, best[0]))
        rows.sort(reverse=True)

        log("")
        log("=" * 70)
        log("OFFSETS WHERE MANY BUFFERS SHARE ONE VALUE  (the camera should be here)")
        log("=" * 70)
        log(f"{'agree':>6} {'variants':>9} {'byte':>7}   kind")
        shown = 0
        for cnt, nvar, byte_off, m in rows:
            if cnt < 3: break
            m = list(m)
            kind = "ROTATION" if is_rot(m) else ("PROJECTION" if looks_projective(m) else "")
            log(f"{cnt:6} {nvar:9} {byte_off:7}   {kind}")
            if kind and shown < 12:
                shown += 1
                for r in range(4):
                    log("        " + " ".join(f"{m[r*4+c]:12.4f}" for c in range(4)))
                if kind == "ROTATION":
                    p = cam_from_view(m)
                    log(f"        -> camera at {p[0]:.2f} {p[1]:.2f} {p[2]:.2f}")
                log("")
    finally:
        try: controller.Shutdown()
        except Exception: pass
        cap.Shutdown(); fh.close()
    print(f"\nwritten to {OUT}")

main()
