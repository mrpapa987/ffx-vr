# analyse_rdc.py  (v2)
#
# Finds the camera matrices in a RenderDoc capture of Final Fantasy X.
#
# Run from RenderDoc's Python Shell (Window -> Python Shell):
#     exec(open(r"C:\projects\FFX-VR\tools\analyse_rdc.py").read())
#
# v1 found 421 draws but read no constant buffers, because the generic
# GetConstantBuffer() call failed and the error was swallowed. This version
# tries the D3D11-specific pipeline state first, reports every failure, and
# dumps what it finds whether or not the rotation heuristic matches.
#
# Output: C:\projects\FFX-VR\logs\rdc_analysis.txt

import os, math, struct

CAPTURE = None
OUT     = r"C:\projects\FFX-VR\logs\rdc_analysis.txt"

import renderdoc as rd


def is_rotation(m):
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


def vs_constant_buffers(controller, log, errors):
    """Yield (slot, resourceId, offset, size) for VS constant buffers.
    Tries the D3D11 path first, then the generic one."""
    try:
        st = controller.GetD3D11PipelineState()
        vs = st.vertexShader
        for i, cb in enumerate(vs.constantBuffers):
            rid = cb.resourceId
            if rid == rd.ResourceId.Null():
                continue
            off = getattr(cb, "vecOffset", 0) * 16
            cnt = getattr(cb, "vecCount", 0) * 16
            yield (i, rid, off, cnt if cnt else 4096)
        return
    except Exception as e:
        errors.setdefault("d3d11", str(e))

    try:
        st = controller.GetPipelineState()
        for slot in range(14):
            cb = st.GetConstantBuffer(rd.ShaderStage.Vertex, slot, 0)
            if cb is None or cb.resourceId == rd.ResourceId.Null():
                continue
            yield (slot, cb.resourceId, cb.byteOffset, cb.byteSize or 4096)
    except Exception as e:
        errors.setdefault("generic", str(e))


def analyse(controller, log):
    draws = collect_draws(controller.GetRootActions(), [])
    log(f"draw calls: {len(draws)}")

    errors = {}
    seen   = {}   # (rid, off, size) -> (floats, first eid, slot)
    rots   = []

    for draw in draws:
        controller.SetFrameEvent(draw.eventId, True)
        for slot, rid, off, size in vs_constant_buffers(controller, log, errors):
            key = (str(rid), off, size)
            if key in seen:
                continue
            try:
                data = controller.GetBufferData(rid, off, size)
            except Exception as e:
                errors.setdefault("getbufferdata", str(e))
                continue
            if not data or len(data) < 16:
                continue
            n = len(data) // 4
            floats = list(struct.unpack_from("<%df" % n, bytes(data), 0))
            seen[key] = (floats, draw.eventId, slot)
            for blk in range(len(floats) // 16):
                m = floats[blk * 16: blk * 16 + 16]
                if any(math.isnan(x) or math.isinf(x) for x in m):
                    continue
                if is_rotation(m):
                    rots.append((draw.eventId, slot, key, blk, m))

    if errors:
        log("")
        log("API problems encountered (first of each kind):")
        for k, v in errors.items():
            log(f"   {k}: {v}")

    log("")
    log(f"distinct VS constant buffers read: {len(seen)}")

    log("")
    log("=" * 68)
    log(f"ROTATION-LIKE 4x4 BLOCKS: {len(rots)}")
    log("=" * 68)
    for eid, slot, key, blk, m in rots[:20]:
        log(f"\neid {eid}  VS slot {slot}  buffer {key[0]} +{key[1]}  block {blk} (byte {blk*64})")
        for r in range(4):
            log("    " + " ".join(f"{m[r*4+c]:12.5f}" for c in range(4)))
        tx, ty, tz = m[12], m[13], m[14]
        px = -(m[0]*tx + m[1]*ty + m[2]*tz)
        py = -(m[4]*tx + m[5]*ty + m[6]*tz)
        pz = -(m[8]*tx + m[9]*ty + m[10]*tz)
        log(f"    camera position if this is a view matrix: {px:.2f} {py:.2f} {pz:.2f}")

    # Dump everything regardless, so a failed heuristic still yields data.
    log("")
    log("=" * 68)
    log("ALL DISTINCT VS CONSTANT BUFFERS (first 32 floats each)")
    log("=" * 68)
    for i, (key, (floats, eid, slot)) in enumerate(seen.items()):
        if i >= 40:
            log(f"\n... and {len(seen) - 40} more")
            break
        log(f"\n[{i}] buffer {key[0]} +{key[1]}  {len(floats)} floats  first seen eid {eid} slot {slot}")
        for r in range(min(8, len(floats) // 4)):
            log("    " + " ".join(f"{floats[r*4+c]:12.5f}" for c in range(4)))


def main():
    path = CAPTURE
    if not path:
        cands = []
        for d in (r"C:\projects\FFX-VR\_staging",
                  os.path.expanduser(r"~\Documents"),
                  os.path.expanduser(r"~\AppData\Local\Temp\RenderDoc")):
            if os.path.isdir(d):
                for f in os.listdir(d):
                    if f.lower().endswith(".rdc"):
                        cands.append(os.path.join(d, f))
        if not cands:
            raise SystemExit("no .rdc found")
        path = max(cands, key=os.path.getmtime)

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fh = open(OUT, "w", encoding="utf-8")

    def log(s=""):
        print(s)
        fh.write(str(s) + "\n")

    log(f"capture: {path}")

    cap = rd.OpenCaptureFile()
    st = cap.OpenFile(path, "", None)
    ok = getattr(rd, "ResultCode", None)
    good = ok.Succeeded if ok else rd.ReplayStatus.Succeeded
    if st != good:
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
