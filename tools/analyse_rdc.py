# analyse_rdc.py  (v3)
#
# Finds the camera matrices in a RenderDoc capture of Final Fantasy X.
# Written against the API the probe actually reported (RenderDoc 1.46):
#   PipeState.GetConstantBlocks(stage) / GetConstantBlock(...)
#   ReplayController.GetBufferData(id, offset, size)
#
# Field names on the returned objects are discovered at runtime rather than
# assumed, because two guesses at them have already been wrong.
#
# Run from RenderDoc's Python Shell:
#     exec(open(r"C:\projects\FFX-VR\tools\analyse_rdc.py").read())
#
# Output: C:\projects\FFX-VR\logs\rdc_analysis.txt

import os, math, struct, inspect
import renderdoc as rd

OUT = r"C:\projects\FFX-VR\logs\rdc_analysis.txt"
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fh = open(OUT, "w", encoding="utf-8")

def log(s=""):
    print(s); fh.write(str(s) + "\n")


def pick(obj, *names):
    """First attribute that exists, else None."""
    for n in names:
        if hasattr(obj, n):
            return getattr(obj, n)
    return None


def is_rotation(m, tol=0.03):
    rows = [[m[r * 4 + c] for c in range(3)] for r in range(3)]
    for r in rows:
        if abs(math.sqrt(sum(x * x for x in r)) - 1.0) > tol:
            return False
    for i in range(3):
        for j in range(i + 1, 3):
            if abs(sum(rows[i][k] * rows[j][k] for k in range(3))) > tol:
                return False
    return True


def transpose(m):
    return [m[c * 4 + r] for r in range(4) for c in range(4)]


def collect_draws(acts, out):
    for a in acts:
        try:
            if a.flags & rd.ActionFlags.Drawcall:
                out.append(a)
        except Exception:
            pass
        if getattr(a, "children", None):
            collect_draws(a.children, out)
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
                    if path is None or os.path.getmtime(p) > os.path.getmtime(path):
                        path = p
    log(f"capture: {path}")

    cap = rd.OpenCaptureFile()
    ok = getattr(rd, "ResultCode", None)
    good = ok.Succeeded if ok else rd.ReplayStatus.Succeeded
    if cap.OpenFile(path, "", None) != good:
        raise SystemExit("could not open capture")
    res = cap.OpenCapture(rd.ReplayOptions(), None)
    controller = res[1] if isinstance(res, tuple) else res

    try:
        draws = collect_draws(controller.GetRootActions(), [])
        log(f"draw calls: {len(draws)}")

        seen, rots, errors, reported = {}, [], {}, [False]

        for draw in draws:
            controller.SetFrameEvent(draw.eventId, True)
            ps = controller.GetPipelineState()

            try:
                blocks = ps.GetConstantBlocks(rd.ShaderStage.Vertex)
            except TypeError:
                try:
                    blocks = ps.GetConstantBlocks(rd.ShaderStage.Vertex, True)
                except Exception as e:
                    errors.setdefault("GetConstantBlocks", str(e)); continue
            except Exception as e:
                errors.setdefault("GetConstantBlocks", str(e)); continue

            if blocks and not reported[0]:
                reported[0] = True
                b0 = blocks[0]
                log(f"\nconstant block object type: {type(b0).__name__}")
                log(f"  members: {', '.join(n for n in dir(b0) if not n.startswith('_'))}")

            for slot, blk in enumerate(blocks or []):
                # the buffer may sit directly on the block or on a .descriptor
                src = blk
                if hasattr(blk, "descriptor"):
                    src = blk.descriptor
                rid = pick(src, "resourceId", "resource", "buffer")
                off = pick(src, "byteOffset", "offset") or 0
                size = pick(src, "byteSize", "size") or 0
                if rid is None:
                    errors.setdefault("no resource id on block",
                                      ', '.join(n for n in dir(src) if not n.startswith('_')))
                    continue
                try:
                    if rid == rd.ResourceId.Null():
                        continue
                except Exception:
                    pass

                size = int(size) if size else 4096
                key = (str(rid), int(off), size)
                if key in seen:
                    continue
                try:
                    data = controller.GetBufferData(rid, int(off), size)
                except Exception as e:
                    errors.setdefault("GetBufferData", str(e)); continue
                if not data or len(data) < 16:
                    continue

                n = len(data) // 4
                floats = list(struct.unpack_from("<%df" % n, bytes(data), 0))
                seen[key] = (floats, draw.eventId, slot)

                for b in range(len(floats) // 16):
                    m = floats[b * 16: b * 16 + 16]
                    if any(math.isnan(x) or math.isinf(x) for x in m):
                        continue
                    if is_rotation(m):
                        rots.append((draw.eventId, slot, key, b, m, "row"))
                    elif is_rotation(transpose(m)):
                        rots.append((draw.eventId, slot, key, b, transpose(m), "col"))

        if errors:
            log("\nAPI problems (first of each):")
            for k, v in errors.items():
                log(f"   {k}: {v}")

        log(f"\ndistinct VS constant buffers read: {len(seen)}")

        log("")
        log("=" * 68)
        log(f"ROTATION-LIKE 4x4 BLOCKS: {len(rots)}")
        log("=" * 68)
        for eid, slot, key, b, m, order in rots[:20]:
            log(f"\neid {eid}  VS block {slot}  buffer {key[0]} +{key[1]}  "
                f"offset {b*64} bytes  ({order}-major)")
            for r in range(4):
                log("    " + " ".join(f"{m[r*4+c]:12.5f}" for c in range(4)))
            tx, ty, tz = m[12], m[13], m[14]
            px = -(m[0]*tx + m[1]*ty + m[2]*tz)
            py = -(m[4]*tx + m[5]*ty + m[6]*tz)
            pz = -(m[8]*tx + m[9]*ty + m[10]*tz)
            log(f"    camera position if this is a view matrix: {px:.2f} {py:.2f} {pz:.2f}")

        log("")
        log("=" * 68)
        log("ALL DISTINCT VS CONSTANT BUFFERS (first 32 floats)")
        log("=" * 68)
        for i, (key, (floats, eid, slot)) in enumerate(seen.items()):
            if i >= 40:
                log(f"\n... and {len(seen)-40} more"); break
            log(f"\n[{i}] {key[0]} +{key[1]}  {len(floats)} floats  eid {eid} block {slot}")
            for r in range(min(8, len(floats)//4)):
                log("    " + " ".join(f"{floats[r*4+c]:12.5f}" for c in range(4)))
    finally:
        try: controller.Shutdown()
        except Exception: pass
        cap.Shutdown()
        fh.close()

    print(f"\nwritten to {OUT}")

main()
