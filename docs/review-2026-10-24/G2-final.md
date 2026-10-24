# G2 final - native performance and diagnostics cost (round 3)

Line numbers are from the 9a8715e snapshot (`ffxvr_native.cpp` unless stated; "xr" = `ffxvr_xr.cpp`, "managed" =
`module.cs`). I re-checked every objection in G2-critique.md against the code. All of them hold, so every item is
**AGREED**. My answer to each objection is under its item.

## Ownership settled with G4 (R11/R12) and G1 (R1)

| Thing | Owner | Notes |
|---|---|---|
| `ffxvr_eye2_begin` / `ffxvr_eye2_end` (4208-4331) | **G4** (R11a `e2_save`/`e2_restore`, R12, R42 8-RTV) | G2 adds nothing there now. Optional later tweak: see R18. |
| RTV0 bind-time cache, one name: `g_cur_rtv0_tex` | **G4** (R11c) | Covers the on_draw lines G2's R18-A wanted. G2 drops its own `g_cur_rtv_tex[]` / `g_cur_dsv_tex` / `g_cur_ps_tex[]` (M3 withdrawn). |
| Backbuffer size and format: `g_bb_w/g_bb_h/g_bb_fmt` (use at 3875) | **G4** (R11b) | G2 drops its copy of this. |
| UI-path `texture_of(g_cur_ps_srv[0])` (2012, 3751, 3763, 3828, 3922, 3931) | **G4** (`ps_tex()`) | |
| SRV restore only when swapped | **G4** (`srv_swapped`) | Supersedes R18-B. |
| `ffxvr_on_ps_srvs` (4334-4344) | **G4** owns the function (adds `g_ps_srv_hi`) | G2 adds one condition, the R3 snapshot gate. Land them together. |
| `ffxvr_xr_set` (5181-5192) | **G2** writes the combined edit | Includes G3's `leave_xr_state()` (R6). G4's watchdog wake lives in `xr_stop` (xr.cpp), not here. |
| `fail()` (xr 254-258), `xr_diag` (5090), `ffxvr_xr_status` (5322) | **G2** | R7b is a prerequisite for G4's R15. |
| `heartbeat` structure and cadence (2896-3000), `tick()` | **G2** | G4's new counters (`e2_track_mismatch`, `no_depth_twin`) go on the eye-2 fprintf line, outside the lock, unaffected. |
| `ffxvr_begin_frame` order | shared, agreed below | |
| Managed `draw_debug` keepalive line | **G1** places it (`push_all` restructure); **G2** supplies the export | It must sit in the *expanded* Debug branch, not in `push_all`. |
| `g_dt` | **G1** (QPC at the top of `begin_frame`) | G2 uses no dt. |

## Agreed `ffxvr_begin_frame` order

```
 0. g_render_tid = GetCurrentThreadId();                                   (existing)
 1. G1: QPC dt -> g_dt (clamped 1-100 ms)
 2. existing: pick summary, slowmo Sleep, atlas disarm, ptr_sprites_release*, flat-UI resets
 3. locked block A: g_frames++; capture start/stop; G2: if (g_capture_done) { g_capture_done = false; dump_now = true; }
 4. G2: tick()                    -> Ctrl+F9 poll (sets g_arm_next: consumed at the next boundary) + heartbeat(false)
 5. G2: read_camera()             -> gated by diag_armed(), non-blocking
 6. sweep_caches()                -> + G2 R3 idle snapshot release
 7. G4: twin_srv_trim()
 8. gpu_timing_end_span()
 9. xr_tick()                     -> G4 R15: no early return; G3 leave_xr_state() at its exit
10. perf_tick()
11. existing overlay/first-draw/backbuffer flags (3530-3532) + G4: g_bb_bound_by_game = false
12. locked block B: frame_dump / census_dump (existing)
13. existing per-frame resets, bones_frame_end, UI health, scene roll-over, world report (G3 R40 edits here)
14. if (dump_now) dump_capture();
15. G2: if (g_measure_pending && (g_snap_frame + 1 == g_frames || g_frames >= g_measure_deadline)) { g_measure_pending = false; measure_now(); }
```
*`ptr_sprites_release` stays where it is unless R43's owner moves it after `xr_tick`. If moved, it goes between 9 and 10.
Steps 4 and 15 call functions that take `g_mutex`, so they must stay outside blocks A and B.

## Shared mechanisms (final)

**M1 `g_diag_until`**: AGREED, revised. In the proposal M1 had six arming points; the critique pointed out
that none of the captures reads what M1 gates.
- What M1 gates: the slot-0 readback and its latch (R2), the heartbeat cadence (R7c), and the bone records (R26b).
- What the captures read instead: the pick reads `rec.data` (always kept, R19); the frame census reads
  `g_frame_live`; `dump_capture` reads `g_capture`; `measure` reads M2; the sequence dump uses the `e1`/`e2` images.
- So arming from them only added a 1 Hz heartbeat hitch at capture time. **Arming points are now only the
  keepalive**, plus the 1800-frame start window.

```cpp
uint32_t g_diag_until = 1800;
static inline bool diag_armed() { return g_frames < g_diag_until; }
extern "C" __declspec(dllexport) void ffxvr_diag_keepalive(int frames) { const uint32_t u = g_frames + (uint32_t)(frames < 1 ? 1 : frames); if (u > g_diag_until) g_diag_until = u; }
```
Managed (G1 places it): in `draw_debug`, after the `CollapsingHeader("Debug")` test succeeds:
`if (_fn_diag_keepalive != null) _fn_diag_keepalive(2);`.
- Load it as optional (R13 TryGetExport). Put a one-line comment in both halves.
- Mismatched halves degrade gracefully: new managed with old native skips the call; old managed with new native
  gets a 30 s heartbeat and loses the slot-0 diff after 30 s.

**M2 `g_snap_until`**: AGREED, unchanged. Separate from M1, so opening Debug costs no GPU copies.

**M3 bind-time cache**: **withdrawn**. The RTV0 part is G4's `g_cur_rtv0_tex`. The SRV part conflicts with
R11's real-binding lookups and would re-add per-bind `texture_of` in every mode.

---

## R2 - Blocking GPU readback every frame
Final verdict: **fix now**. Status: **AGREED** (with the critique's changes).

Answers to the critique:
- Ring ordering bug: **accepted.** I verified it. With `old = (i+1)%3`, a slot that returned WAS_STILL_DRAWING
  is skipped as `cur` and collected later, *after* a newer slot. The diff then compares non-consecutive frames,
  in reverse order.
- Release `g_slot0` when disarmed, delete the empty lock at 3298-3300, delete the stale comment at 3979-3980:
  accepted. The comment documents an export that no longer exists.

Final fix:
- Latch, 3960: `if (g_latched || g_ctx == nullptr || !diag_armed()) return passes | ret_flags;`.
- In `read_camera`, when `!diag_armed()`:
  `if (g_slot0) { g_slot0->Release(); g_slot0 = nullptr; } return;`.
- When armed, use a 3-entry ring tagged by frame:
```cpp
struct CamRb { ID3D11Buffer* st = nullptr; UINT size = 0; uint32_t frame = 0; bool pending = false; };
static CamRb g_cam_rb[3]; static uint32_t g_cam_last_collected = 0;
// collect: pending entries in ascending .frame; stop at the first WAS_STILL_DRAWING (the GPU completes in order)
//   Map(st, READ, D3D11_MAP_FLAG_DO_NOT_WAIT); on success copy out;
//   if (e.frame == g_cam_last_collected + 1) diff against g_full (as today) else g_diff_count = -1 ("n/a, frames not consecutive");
//   g_cam_last_collected = e.frame; Unmap; pending = false
// issue: take a non-pending entry (if none, skip this frame); recreate if st == null || size != slot0 ByteWidth (exact size);
//   CopyResource(st, g_slot0); frame = g_frames; pending = true
```
  The heartbeat prints "n/a" when `g_diff_count < 0`.
- Remove `g_staging`/`g_staging_size` (194-195) and the code at 3253-3301.

Expected gain: in normal play the per-frame CPU-GPU serialisation is gone. When GPU-bound, frame time moves from
about CPU+GPU towards max(CPU, GPU), possibly several ms, and possibly part of blitz's 30 fps lock (unproven).

Verify in game:
- Compare the perf line `frame` against `gpu` in a busy field scene and in blitzball, before and after.
- `xr wait` may rise: that is idle time moving into the runtime.
- Watch head-motion latency: the blocking Map was acting as an accidental limiter. If motion feels laggier, add
  a one-frame-old `D3D11_QUERY_EVENT` wait.
- With Debug open, `ffxvr_native.log` shows "changed since previous frame: N floats" (or "n/a" right after
  arming).

Effort: S.

---

## R3 - Two full-resolution scene copies every stereo frame
Final verdict: **fix now**. Status: **AGREED**.

Answers to the critique:
- Run `measure_now()` at the end of `begin_frame`: accepted (step 15). Today the button runs during ImGui
  building, after `xr_tick`, with the game frame in the backbuffer. Step 15 has the same state.
- Keep `texture_of` in `on_ps_srvs` under the gate rather than depending on M3: accepted.
- Note the side effect in `eye2_size`/`eye2_srv`: accepted.

Final fix:
- `snap_wanted() = g_frames < g_snap_until || g_measure_pending`. Use it in:
  - `snapshot_if_scene` (4167), as the first test;
  - the resolve trigger at 4378;
  - `ffxvr_on_ps_srvs`: `if (views && g_stereo && snap_wanted()) { loop with texture_of as today }`. Today
    `texture_of` runs even when `!g_stereo`, because the argument is evaluated before the call.
- `ffxvr_eye2_size` and `ffxvr_eye2_srv`, for sources 1 and 2: `g_snap_until = g_frames + 3`, with a comment
  saying "side effect: requests snapshots".
- `ffxvr_measure`: run `measure_now()` at once if `g_snap_tex && g_snap_frame + 1 >= g_frames`. Otherwise set
  `g_measure_pending`, `g_measure_deadline = g_frames + 5` and `g_snap_until = g_frames + 5`; it then runs at
  step 15.
- `sweep_caches`: `if (!snap_wanted() && g_snap_tex && g_frames - g_snap_frame > 600)` release the 4 `g_snap_*`
  objects (with `g_plain_srvs_forget`) and set `g_snap_from = nullptr`.

Expected gain: about 0.3-1 ms GPU per stereo frame, 2x scene-target VRAM after 10 s, and the per-bind
`texture_of` loop.

Verify in game:
- Headset on: compare `gpu` and `vram` in the perf line.
- Flat with stereo on: the preview on "scene" and "scene eye 1" still shows.
- Measure + images: the "scene: game vs eye-2 twin" line and `img_scene_eye*.bmp` are still written.

Effort: S.

---

## R7 - Render-thread file I/O and retries
Final verdict: **fix now**. Status: **AGREED** (with the critique's changes).

Answers to the critique:
- `fail()` dedup state leaking across muted attempts: **accepted.** `xr_fail_reset()` is called at the top of
  `xr_start`. This also makes attempt 2 log in full, as intended.
- The trace header text: accepted.
- One combined `ffxvr_xr_set` edit with G3: accepted.
- The back-off is reset only by `on == 0` and by success, never by `leave_xr_state`: agreed.

Final fix:
- (a) Combined `ffxvr_xr_set`, which G2 lands:
```cpp
static ULONGLONG g_xr_retry_at = 0; static uint32_t g_xr_fails = 0; bool g_xr_quiet = false;
extern "C" __declspec(dllexport) int ffxvr_xr_set(int on, float scale, int rotate, int translate, int use_fov, int seat) {
    g_xr_scale = scale; g_xr_rotate = rotate != 0; g_xr_translate = translate != 0; g_xr_use_fov = use_fov != 0; g_xr_seat = seat != 0;
    if (!on) { g_xr_fails = 0; g_xr_retry_at = 0; if (g_xr_want) { xr_stop(); leave_xr_state(); } return 0; }   // G3 R6
    if (g_xr_want) return 1;
    if (!g_cam_valid || g_backbuffer == nullptr) return 0;          // the first-3D-frame wait is not a failure
    const ULONGLONG now = GetTickCount64(); if (now < g_xr_retry_at) return 0;
    XrHost h = { g_device, g_ctx, g_dir, xr_diag, ffxvr_copy_pass };
    g_xr_quiet = g_xr_fails >= 2; g_xr_want = xr_start(h); g_xr_quiet = false;
    if (g_xr_want) { if (g_xr_fails) diag("xr: started after %u failed attempts", g_xr_fails); g_xr_fails = 0; g_xr_retry_at = 0; g_xr_recenter_pending = true; return 1; }
    g_xr_fails++; g_xr_retry_at = now + (g_xr_fails < 3 ? 2000 : 5000);
    if (g_xr_fails == 2 || g_xr_fails % 60 == 0) diag("xr: start failed %u times (last: %s); retrying every %d s without logging", g_xr_fails, xr_status(), g_xr_fails < 3 ? 2 : 5);
    return 0;
}
```
  - `xr_diag` (5090) starts with `if (g_xr_quiet) return;`. The watchdog only exists after a successful start,
    so it is never muted.
  - `ffxvr_xr_status`: when `!g_xr_want && g_xr_retry_at > now`, append " - retrying in N s".
  - Managed needs no change: G1's `push_all` calls `xr_set` every frame.
- (b) xr `fail()`:
  - de-duplicate (one line per distinct failure, then "(xN in the last S s)" at most every 10 s, then
    "(previous failure repeated N more times)" when it changes);
  - `set_status` on every call;
  - new `void xr_fail_reset()` clears `last`/`reps`, called first thing in `xr_start`.
- (c) Heartbeat:
  - period `diag_armed() ? 1000 : 30000`;
  - copy `{buf, size, updates, via}` (via is a literal) and `g_ovr_stats` into locals under the lock, then print
    outside it, capped at 300 lines plus "... N more";
  - trace header (3367): "camera trace: one line per heartbeat (1 s while debugging, 30 s otherwise)".
- (d) Remove `tick()` from `hk_Unmap` (3221). It now runs at begin_frame step 4. `tick()` keeps the Ctrl+F9 poll
  and `heartbeat(false)`; the capture-done dump moves into block A (step 3). The dead `tick()` calls in the
  uninstalled hooks are left to R42.

Expected gain:
- (a) no per-frame instance create/destroy (5-50 ms each) and no log flood while VD is off with autostart on;
- (b) and (c) remove a once-a-second 1-5 ms hitch;
- (d) minor (0.1-0.5 ms per frame).

Verify in game:
- Autostart on, VD disconnected: normal frame rate; the diag log shows two full attempts, then one summary line
  a minute; the status says "retrying in N s".
- Connect VD: the headset starts within 5 s.
- `ffxvr_native.log` updates every 30 s with the overlay closed, every second with Debug open.
- Ctrl+F9 still writes `ffxvr_cbdump_NN.txt`.

Effort: S.

---

## R18 - Per-draw texture_of / twin lookups, SRV restore
Final verdict: A and B **done by G4's R11** (no G2 code). C is **won't fix**. One optional tweak is **later**.
Status: **AGREED**.

Answers to the critique, all accepted:
- G4's R11 rewrites `eye2_begin`/`eye2_end` around the real bindings (`OMGetRenderTargets`/`PSGetShaderResources`).
  A texture taken from the *tracked* SRV can't be paired with a *real* view.
- `srv_swapped` supersedes B.
- R11(c) adds the same RTV0 cache and `g_bb_w/h`.
- The SRV cache would put back the per-bind `texture_of` that R3 removes, in every mode.
- My "safer" claim was overstated. A cached texture still meets `GetDesc` on a cache miss, so it is no worse,
  but not safer.
- C's invalidation list is wrong once R12 moves the trim to `begin_frame` and makes failed twins retryable.

What G2 asks G4 to fold into R11 (cheap, no new risk):
1. In `eye2_begin`, compute the RTV0 texture **once**:
   `t0 = (s.rtv[0] == g_cur_rtv[0]) ? g_cur_rtv0_tex : texture_of(s.rtv[0])`.
   Use it for `twin_of`, `to_bb` and `gt` instead of three `texture_of(s.rtv[0])` calls. The real view is
   AddRef'd, and a match means the tracked bind is current.
2. Optional: in `on_rendertargets`, keep `g_cur_rtv0_samples`/`w`/`h` next to `g_cur_rtv0_tex`, so on_draw's
   `GetDesc` at 3873-3874 and 3905 becomes a read. Minor; G4's call.
- No on_draw path needs a DSV texture cache. The critique's point 2 is not needed.

Expected gain: from R11(c) plus tweak 1, about 5-8 fewer `texture_of` calls per stereo draw, roughly 0.5-2 ms CPU
per frame at 1-2k draws. R11 adds one `OMGet` + `PSGet` per eye-2 draw, so measure the combined change, not per
item.

Verify in game: the perf line's `eye2` ms and `frame` before and after the combined R11 build, in Luca or
Macalania and in a battle. Both eyes stay identical except for parallax.

Effort: 0 for G2 (part of G4's M).

---

## R19 - Constant-buffer data copies
Final verdict: **fix now**, 256-byte cap. Status: **AGREED**.

Final fix:
```cpp
// rec.data is diagnostic-only (pick, frame census; nothing reads past 64 floats). camera_from_write and the
// world extraction above read `src` and must keep running on EVERY write: never gate record_buffer as a whole.
constexpr UINT kCbKeep = 256;
const UINT keep = size < kCbKeep ? size : kCbKeep;
if (rec.data.size() != keep) rec.data.resize(keep);
memcpy(rec.data.data(), src, keep);
```
The printers at 2035, 2050 and 3838 show `it->second.size` instead of `data.size()`.

Expected gain: about 0.1-0.5 ms CPU per frame in busy scenes. Write-combined reads per CB Unmap are bounded to
about 0.5 KB.

Verify in game: frame ms in a crowded area. F9 pick in battle still shows `cb0 NNNB:` and `ps_cb0` values. A frame
census still prints `cb0+48:` lines.

Effort: S.

---

## R20 - Camera scan for unrecognised buffers
Final verdict: **fix now**. Status: **AGREED**.

Answer to the critique: the `rd == wr` self-overlapping `memcpy` is **accepted**. It is UB even with identical
pointers.

Final fix:
1. `if (base == 0 && !g_have_PV) return src;` before 2731.
2. `replace_known_products(const uint8_t* rd, uint8_t* wr, UINT size, const float* PVn)` with a lazy first-hit
   copy: `if (!copied) { if (wr != rd) memcpy(wr, rd, size); copied = true; }`.
3. Tolerances are computed locally: once for `g_last_PV`, and once per X for `PVX`, via `same4_tol`.
- No per-size "never matched" memo.
- Apply G4's R14 `ring_take` first: both edit 2780-2787.

Expected gain: about 0.05-0.3 ms CPU per frame.

Verify in game:
- Frame ms.
- The override mask still includes "product-only".
- The heartbeat's "camera override, by buffer size" table is unchanged.

Effort: S.

---

## R23 - Eye 2 re-renders identical passes
Final verdict: **step 1 now**; **step 2 later**, behind a toggle, after R11/R12 land and only if step 1 shows a
big target (the blitz reflection). Status: **AGREED**.

Answers to the critique: the name collision is **accepted**. I verified that `g_e2_identical` (388, 4305) already
counts eye-2 CB uploads whose bytes equal eye 1's, and `measure` prints it. "Upper bound" labelling and keying the
tally by the real RTV0/DSV texture: accepted.

Final step 1:
- In `eye2_begin`, after R11's SRV and slot loops: `if (!any_srv_swapped && no_slot_image) g_e2_same_inputs++`.
- A 16-entry per-target tally keyed by `t0` (tweak 1 above) or by `texture_of(s.dsv)`, reset in `begin_frame`.
- The perf diag line gets `same-input N (upper bound)`. The frame census lists the top targets with size and
  sample count.

Step 2: as designed in the proposal (`shared`/`skipped`/`no_share` per Twin, the decision in `on_draw` before
the eye-1 draw, the catch-up copy, and `eye2_tex(game)` for every reader).

Expected gain: unknown until step 1. Possibly several ms of GPU in blitz.

Verify in game: step 1, the perf diag line in field, battle and blitz. Step 2 (later): shadows and reflections
match in both eyes, and `gpu`/`eye2` drop.

Effort: step 1 S; step 2 M.

---

## R24 - XInput probing
Final verdict: **fix now**. Status: **AGREED** (the critique's counter-proposal adopted).

Answer to the critique: **accepted.** When the game is polling, a pad on a slot it doesn't poll can't drive the
game either, so probing other slots has no value. Using only hooked data then removes the spike entirely.

Final fix:
- In `xi_get_state_hook`, right after `g_xi_orig` (before `vrpad_apply`), for `idx < 4`, record
  `g_xi_seen[idx] = {res, rx, GetTickCount()}`. Use aligned plain or volatile stores; torn reads are harmless.
- In `right_stick_x`, after the VR branch:
  - if `g_xi_hooked` and any slot was seen within 500 ms: return `norm(rx)` of the first seen slot with
    `ERROR_SUCCESS`, else 0. **No own XInput calls.**
  - else (no hook, or the game isn't polling): today's `g_xi_pad` direct read, plus probing one slot every 2 s,
    round-robin.

Expected gain: no periodic 0.5-4 ms spike for headset-only players, and zero XInput calls of our own in normal
play.

Verify in game:
- Headset only, stick turning on: the perf line's `max` shows no periodic spike.
- With a pad, smooth turn still works.

Effort: S.

---

## R26 - Smaller ones
Final verdict: **fix later** (S each). Status: **AGREED**.
- **26a**: precompute the affine block list and its PV·X and PmV·X products once per call in
  `write_backdrop_camera` (2252-2268), keeping the same first-match order. Only active in the non-default
  backdrop mode 0.
- **26b**: `bones_record` early-out:
  `if (!g_hands_on && g_wig_amt == 0.f && g_bseq_left <= 0 && !diag_armed()) return 0;`, after the distance
  test. The Debug keepalive keeps the bone map working.
- **26c**: one `classify_buffer` (one GetType + QI + GetDesc) instead of `as_constant_buffer` +
  `as_geometry_buffer`. In `hk_Map`, test `g_ui_vbs.count(buf)` before locking (same thread, immediate context).
  Keep the QI.

Expected gain: 26a moderate in mode 0, nil otherwise; 26b and 26c minor.

Verify in game:
- 26a: backdrop "props" mode in a pre-rendered room; props sit right, frame ms drops.
- 26b: the bone map with Debug open.
- 26c: the perf line in battle with the hide rules on.

---

## Summary

| Item | Final verdict | Status | Effort |
|---|---|---|---|
| M1/M2 | keepalive-only arming; M2 unchanged; M3 withdrawn | AGREED | S |
| R2 | fix now (gate + frame-tagged non-blocking ring) | AGREED | S |
| R3 | fix now (demand-driven snapshots, deferred measure at step 15) | AGREED | S |
| R7 | fix now (combined xr_set with G3, quiet back-off, fail dedup + reset, heartbeat cadence, tick per frame) | AGREED | S |
| R18 | by G4 R11 (plus tweak 1); C won't fix | AGREED | 0 (G4) |
| R19 | fix now (256 B cap) | AGREED | S |
| R20 | fix now (early reject, lazy copy with `wr != rd`, local tolerances) | AGREED | S |
| R23 | step 1 now (`g_e2_same_inputs`); step 2 later behind a toggle | AGREED | S / M |
| R24 | fix now (hooked data only when the game polls) | AGREED | S |
| R26 | fix later | AGREED | S each |
