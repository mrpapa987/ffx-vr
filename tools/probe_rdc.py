# probe_rdc.py
#
# Asks RenderDoc what its API actually looks like, rather than guessing.
# Run from RenderDoc's Python Shell:
#     exec(open(r"C:\projects\FFX-VR\tools\probe_rdc.py").read())
#
# Writes C:\projects\FFX-VR\logs\rdc_probe.txt

import os, inspect
import renderdoc as rd

OUT = r"C:\projects\FFX-VR\logs\rdc_probe.txt"
os.makedirs(os.path.dirname(OUT), exist_ok=True)
fh = open(OUT, "w", encoding="utf-8")

def log(s=""):
    print(s); fh.write(str(s) + "\n")

def interesting(names):
    want = ("constant", "cbuffer", "cb", "buffer", "block", "uniform")
    return [n for n in names if any(w in n.lower() for w in want)]

def describe(obj, label, deep=True):
    log("")
    log("=" * 68)
    log(f"{label}   type={type(obj).__name__}")
    log("=" * 68)
    names = [n for n in dir(obj) if not n.startswith("_")]
    hits = interesting(names)
    log(f"  buffer/constant-related members ({len(hits)}):")
    for n in hits:
        try:
            v = getattr(obj, n)
            kind = "method" if callable(v) else type(v).__name__
            extra = ""
            if callable(v):
                try: extra = str(inspect.signature(v))
                except Exception: extra = "(...)"
            log(f"     {n:32} {kind}{extra}")
        except Exception as e:
            log(f"     {n:32} <error: {e}>")
    log(f"  all members ({len(names)}): {', '.join(names)}")

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

log(f"renderdoc version: {getattr(rd, 'GetVersionString', lambda: '?')()}")
log(f"capture: {path}")

cap = rd.OpenCaptureFile()
ok = getattr(rd, "ResultCode", None)
good = ok.Succeeded if ok else rd.ReplayStatus.Succeeded
if cap.OpenFile(path, "", None) != good:
    raise SystemExit("could not open capture")
res = cap.OpenCapture(rd.ReplayOptions(), None)
controller = res[1] if isinstance(res, tuple) else res

try:
    # move to a real draw so the state is populated
    draws = []
    def walk(acts):
        for a in acts:
            try:
                if a.flags & rd.ActionFlags.Drawcall: draws.append(a)
            except Exception: pass
            if getattr(a, "children", None): walk(a.children)
    walk(controller.GetRootActions())
    log(f"draws: {len(draws)}")
    mid = draws[len(draws)//2]
    controller.SetFrameEvent(mid.eventId, True)
    log(f"positioned at eid {mid.eventId}")

    describe(controller, "ReplayController")

    try:
        st = controller.GetD3D11PipelineState()
        describe(st, "D3D11 pipeline state")
        describe(st.vertexShader, "D3D11 vertexShader")
    except Exception as e:
        log(f"\nGetD3D11PipelineState failed: {e}")

    try:
        ps = controller.GetPipelineState()
        describe(ps, "generic PipeState")
    except Exception as e:
        log(f"\nGetPipelineState failed: {e}")

finally:
    try: controller.Shutdown()
    except Exception: pass
    cap.Shutdown()
    fh.close()

print(f"\nwritten to {OUT}")
