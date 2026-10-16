# analyse_rdc.py
#
# Finds the camera matrices in a RenderDoc capture of Final Fantasy X.
#
# Run this from RenderDoc's Python Shell (Window -> Python Shell), which already
# has the renderdoc module loaded:
#
#     exec(open(r"C:\projects\FFX-VR\tools\analyse_rdc.py").read())
#
# It walks every draw call, reads the constant buffers bound to the vertex
# shader, and looks for 4x4 blocks whose upper 3x3 is a rotation - the signature
# of a view or world matrix, which no UI or screen-space buffer has.
#
# Output goes to C:\projects\FFX-VR\logs\rdc_analysis.txt

import os, math, struct

CAPTURE = None   # set below; or hardcode a path to a .rdc
OUT     = r"C:\projects\FFX-VR\logs\rdc_analysis.txt"

try:
    import renderdoc as rd
except ImportError:
    raise SystemExit("renderdoc module not available - run this inside RenderDoc's Python Shell")


def is_rotation(m):
    """Upper-left 3x3 orthonormal? True for view and unscaled world matrices."""
    rows = [[m[r * 4 + c] for c in range(3)] for r in range(3)]
    for r in rows:
        if abs(math.sqrt(sum(x * x for x in r)) - 1.0) > 0.03:
            return False
    for i in range(3):
        for j in range(i + 1, 3):
            if abs(sum(rows[i][k] * rows[j][k] for k in range(3))) > 0.03:
                return False
    return True


def collect_draws(actions, out):
    for a in actions:
        try:
            if a.flags & rd.ActionFlags.Drawcall:
                out.append(a)
        except Exception:
            pass
        if getattr(a, "children", None):
            collect_draws(a.children, out)
    return out


def analyse(controller, log):
    draws = collect_draws(controller.GetRootActions(), [])
    log(f"draw calls in capture: {len(draws)}")

    seen = {}          # (resourceId, offset) -> contents
    rotations = []     # candidates

    for n, draw in enumerate(draws):
        controller.SetFrameEvent(draw.eventId, True)
        state = controller.GetPipelineState()

        for slot in range(6):
            try:
                cb = state.GetConstantBuffer(rd.ShaderStage.Vertex, slot, 0)
            except Exception:
                continue
            if cb is None or cb.resourceId == rd.ResourceId.Null():
                continue

            size = cb.byteSize if cb.byteSize else 1024
            try:
                data = controller.GetBufferData(cb.resourceId, cb.byteOffset, size)
            except Exception:
                continue
            if not data or len(data) < 64:
                continue

            key = (str(cb.resourceId), cb.byteOffset, len(data))
            if key in seen:
                continue
            seen[key] = True

            floats = list(struct.unpack_from("<%df" % (len(data) // 4), data, 0))
            for blk in range(len(floats) // 16):
                m = floats[blk * 16: blk * 16 + 16]
                if any(math.isnan(x) or math.isinf(x) for x in m):
                    continue
                if is_rotation(m):
                    rotations.append((draw.eventId, slot, key, blk, m))

    log("")
    log("=" * 68)
    log(f"ROTATION-LIKE 4x4 BLOCKS FOUND: {len(rotations)}")
    log("=" * 68)

    for eid, slot, key, blk, m in rotations[:25]:
        log(f"\neid {eid}  VS slot {slot}  buffer {key[0]} +{key[1]}  block {blk} (byte {blk*64})")
        for r in range(4):
            log("    " + " ".join(f"{m[r*4+c]:12.5f}" for c in range(4)))
        # a view matrix encodes the camera position in its translation
        tx, ty, tz = m[12], m[13], m[14]
        px = -(m[0]*tx + m[1]*ty + m[2]*tz)
        py = -(m[4]*tx + m[5]*ty + m[6]*tz)
        pz = -(m[8]*tx + m[9]*ty + m[10]*tz)
        log(f"    if this is a view matrix, camera sits at {px:.2f} {py:.2f} {pz:.2f}")

    if not rotations:
        log("\nNone found. Dumping the first few VS constant buffers verbatim instead:")
        for i, key in enumerate(list(seen)[:6]):
            log(f"  {key}")


def main():
    path = CAPTURE
    if not path:
        # most recent .rdc under the usual folders
        cands = []
        for d in (r"C:\projects\FFX-VR\_staging",
                  os.path.expanduser(r"~\Documents"),
                  os.path.expanduser(r"~\AppData\Local\Temp\RenderDoc")):
            if os.path.isdir(d):
                for f in os.listdir(d):
                    if f.lower().endswith(".rdc"):
                        cands.append(os.path.join(d, f))
        if not cands:
            raise SystemExit("no .rdc found - set CAPTURE at the top of this script")
        path = max(cands, key=os.path.getmtime)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fh = open(OUT, "w", encoding="utf-8")

    def log(s=""):
        print(s)
        fh.write(str(s) + "\n")

    log(f"capture: {path}")

    cap = rd.OpenCaptureFile()
    st = cap.OpenFile(path, "", None)
    if st != rd.ResultCode.Succeeded:
        raise SystemExit(f"could not open capture: {st}")

    res = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = res[1] if isinstance(res, tuple) else res

    try:
        analyse(controller, log)
    finally:
        try:
            controller.Shutdown()
        except Exception:
            pass
        cap.Shutdown()
        fh.close()

    print(f"\nwritten to {OUT}")


main()
