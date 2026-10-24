# G3 final (round 3): camera/view logic, mode state, cursor hiding, VR hands

The proposal is G3-proposal.md and the critique is G3-critique.md. I re-checked every objection against the
9a8715e snapshot. Line numbers refer to it. "native" = ffxvr_native.cpp, "managed" = module.cs.

| Item | Final verdict | Status | Effort |
|---|---|---|---|
| R4 | fix now. First build ships 0/0 ms (consistency only); A/B retune to 24/325 ms afterwards | AGREED | M |
| R6 | fix now | AGREED | S |
| R39 + R48 | fix now | AGREED | S-M |
| R40 | fix now | AGREED | S |
| R43 | fix now | AGREED | S |
| R44 | fix now | AGREED | S |
| R42a bg rects | fix now | AGREED | S |
| R42b RGBA dump | fix now | AGREED | S |
| R42d dead hooks | fix now | AGREED | S |
| R42c hands calibration | fix later (with hands) | AGREED | S |
| R45 / R46 | fix later (with hands), one reorder | AGREED | S |
| R47 | fix later (with hands) | AGREED | S |

Nothing is disputed. Every critique point was checked and accepted. Two were accepted only partly or as
optional (R4 point 4 and the size of the R39 cap); the reasons are noted under those items.

---

## R4 - Camera smoothing per upload, made per frame

Final verdict: **fix now**. Restore the documented intent as frame-rate-independent time constants, ticked
once per frame lazily at the first real eye-1 camera upload. **The first build ships with both time
constants at 0 (snap).** The A/B retune session then picks the defaults.

### Response to the critique

1. **Gap snap never fires: accepted, it was a real bug.** I confirmed that the stereo-screen early return
   (1145-1157) and the `fp` / `g_fp_mode >= 2` gates (1161, 1203) let `write_camera` stamp the tick without
   the anchor integrating. Fix: a separate `g_anchor_frame`, stamped only inside the anchor block.
2. **No teleport snap: accepted.** I use the existing rule in `ffxvr_player_set` (5273: 50 units or more is
   a warp), for the field target only. The battle centroid keeps its glide, which is the point of it.
3. **The first camera drives seat follow-pans: accepted.** Ticks happen only on an upload whose main
   projection is 3D (`|P0|,|P5| > 0.2`, the 2726 test; I confirmed the intro's near-2D camera passes the 2690
   test and reaches `write_camera`) and only on the render thread (`g_render_tid`, 175 / 3494). A counter of
   eye-1 uploads in the frame whose `v` differs from the tick's `v` is added. If it is ever non-zero in a
   scene, follow-pans should take its yaw from the frame's main camera instead (follow-up).
4. **1-2 missed frames: accepted as a cheap improvement.** Integrate `g_dt * gap` for a gap of 1-2 frames,
   snap when the gap is more than 2.
5. **"0 ms = today": accepted, reworded** to "0 = snap, the closest to the pre-fix behaviour". Today the first
   uploads of a frame see a partial step and eye 2 is one step further, so 0 ms is not exact anywhere.
6. **What 24 ms buys: accepted.** I re-derived it. A 30 Hz step S under 60 Hz rendering moves the anchor in
   steady state by k(x+S) and k(1-k)(x+S) per frame pair, with x = (1-k)²S / (1-(1-k)²). That gives
   2/3 S / 1/3 S at k = 0.5 (24 ms) and 0.58 / 0.42 at 50 ms. The fallback, if the judder is still seen: a
   step-aware lerp (on a target change, move linearly to the new target over 1/30 s). It is in the plan, not
   built.
7. **The builder is not fully read-only: accepted.** The latches at 1165-1168 and 1247 still fire from
   non-advancing callers. That is harmless, because every latch reset happens at Present and `g_cam` is the
   post-cut camera. A comment will say so; the latches are not gated.
8. **Defaults 0/0 in the first build: accepted.** R1 (real dt with the overlay closed) lands in the same
   build. With 0/0, R4's first build changes only consistency (the same anchor for all objects and both
   eyes), so any change in feel can be put down to R1. The A/B session then moves to 24/325.
9. Minor points accepted: warn about `follow_pans=1` / `tp_follow_rate>0` in the owner's ini; label the
   diagnostic as camera uploads, not builds; removing `ffxvr_anchor_set` is safe (managed never binds it).

**Fit with G1's dt (QPC at the top of `ffxvr_begin_frame`, clamped to 1-100 ms, replacing the dt from
`cuts_set`): it fits as is.** The tick runs at the first upload after that Present, on the same thread, and
reads `g_dt`, which is then the last Present-to-Present interval. My own `fminf(g_dt, 0.1f)` clamp is
dropped, because G1's clamp covers it. Nothing in R4 reads the dt from `cuts_set`. The tick must not cache dt
anywhere else.

### Final fix design

Globals (replacing `g_fp_anchor_k` at 857 and `g_anchor_ovr_k` at 860):
```cpp
float g_fp_anchor_tau  = 0.f;   // s, diorama / third-person anchor; 0 = snap. A/B target: 0.024 (design-notes "0.5 per frame" at 60 fps)
float g_anchor_ovr_tau = 0.f;   // s, battle centre; 0 = snap. A/B target: 0.325 ("0.05 per frame")
uint32_t g_view_tick_frame = 0xFFFFFFFFu, g_anchor_frame = 0;
uint32_t g_view_ticks = 0, g_view_uploads = 0, g_view_othercam = 0; float g_tick_v[12];   // diag
```

`write_camera` (1401-1402):
```cpp
bool advance = false;
if (g_eye_shift == 0.f) {
    g_view_uploads++;
    const float* mp = reinterpret_cast<const float*>(orig + base + 192);                        // main projection (as 2689 / 2726)
    const bool real3d = fabsf(mp[0]) > 0.2f && fabsf(mp[5]) > 0.2f;
    const bool rt = g_render_tid == 0 || GetCurrentThreadId() == g_render_tid;
    if (g_view_tick_frame != g_frames && real3d && rt) { advance = true; g_view_tick_frame = g_frames; g_view_ticks++; memcpy(g_tick_v, v, 48); }
    else if (g_view_tick_frame == g_frames && real3d && memcmp(v, g_tick_v, 48) != 0) g_view_othercam++;
}
build_override_view(v, Vn, In, Pn, advance);
```

`build_override_view(..., bool advance)`. Callers 1680 (`ffxvr_project`) and 5051 / 5052
(`draw_markers_both`) pass `false`. A comment at the top says: "advance = once per frame; the latches still
fire from any caller (idempotent within a frame)."
- 1170: `primary` becomes `advance` (third-person slew; the formula is unchanged and uses `g_dt`).
- 1208-1214 (anchor):
```cpp
const bool ovr_on = g_anchor_ovr_valid && g_fp_mode == 2;
const float* target = ovr_on ? g_anchor_ovr : g_fp_pos;
if (advance) {
    if (ovr_on != g_anchor_ovr_was) { g_fp_anchor_valid = false; g_anchor_ovr_was = ovr_on; }   // battle start / end
    const uint32_t gap = g_frames - g_anchor_frame; g_anchor_frame = g_frames;
    if (gap > 2) g_fp_anchor_valid = false;                                                     // idle: screen, stereo screen, backdrop, seat, fp, loading
    if (g_fp_anchor_valid && !ovr_on) {                                                         // a warp (ffxvr_player_set's 50-unit rule)
        const float dx = target[0]-g_fp_anchor[0], dy = target[1]-g_fp_anchor[1], dz = target[2]-g_fp_anchor[2];
        if (dx*dx + dy*dy + dz*dz > 50.f * 50.f) g_fp_anchor_valid = false;
    }
    const float tau = ovr_on ? g_anchor_ovr_tau : g_fp_anchor_tau;
    const float k = tau > 1e-4f ? 1.f - expf(-g_dt * (float)gap / tau) : 1.f;                 // g_dt: G1's QPC frame time
    if (!g_fp_anchor_valid) { memcpy(g_fp_anchor, target, sizeof(g_fp_anchor)); g_fp_anchor_valid = true; }
    else for (int i = 0; i < 3; ++i) g_fp_anchor[i] += (target[i] - g_fp_anchor[i]) * k;
}
const float* A = g_fp_anchor_valid ? g_fp_anchor : target;
P[0] = A[0]; P[1] = A[1]; P[2] = A[2];
```
- 1248: `else if (g_follow_pans && advance) {` (the body is unchanged; it uses `g_dt`).

Export: `ffxvr_view_smooth_set(float field_ms, float battle_ms)` replaces `ffxvr_anchor_set` (5218). It
clamps to 0-500 and 0-3000 ms. Managed adds the ini keys `anchor_smooth_ms=0` and
`battle_anchor_smooth_ms=0` (clamped per R27), sliders labelled "anchor smoothing (ms, 0 = snap)" (0-150)
and "battle anchor smoothing (ms, 0 = snap)" (0-1000), and a push every frame next to `_fn_tp_set` (or
in G1's `push_all`). Widen the `follow_rate` slider max from 90 to 360.

Diagnostic (on the existing 10 s `ui:` line, or its own line every 10 s):
`view: frames %u, ticks %u, eye-1 camera uploads/frame %.1f, uploads with another camera %u`.
Reset the counters after printing.

design-notes: "0.5 / 0.05 per frame" becomes "24 / 325 ms time constants (A/B pending); before 2026-10 they
ran per camera upload, which was effectively snap".

### Retune plan (after the first build has been played and R1 has settled)

0. Before the build: check the owner's ini for `follow_pans=1` or `tp_follow_rate>0`. Both become about N
   times slower (the honest deg/s), so warn him.
1. Field, third person, then the diorama: `anchor_smooth_ms` at 0, 24 and 50. Run straight, in a circle,
   stop dead. Expect 24 ms to halve the 30 Hz step (2/3-1/3 instead of 1-0) with about 1 frame of lag; 50 ms
   gives little more. If the step judder is still visible at 50 ms, stop raising it and ask for the
   step-aware lerp.
2. Battle diorama: `battle_anchor_smooth_ms` at 0, 325 and 600. Watch a monster dying or appearing, a party
   swap and a summon. Battle start and end must snap.
3. If used: `follow_rate` 60 / 120 / 240, and `tp_follow_rate` from 90.
4. Write the winners in as defaults (native globals and managed fields) and in design-notes.

### Verify in game (first build, 0/0)

- The diag line shows ticks equal to frames in 3D, and "uploads with another camera" at 0, or note the
  scene where it isn't.
- Third person running past near walls: no shimmer between near and far objects. Closing each eye in turn on
  slopes and stairs shows no vertical misalignment.
- First person: unchanged.
- Return from a menu, a stereo-screen shot, a backdrop room or a map change into the diorama or third
  person: the view lands in place, with no fly-in.
- Then the A/B session.

Effort: M.

---

## R6 - VR-only flags survive headset off

Final verdict: **fix now**.

Response:
1. **The UI_DONE symptom: accepted, the claim was wrong.** I confirmed `xr_tick` sets `g_ui_state = UI_IDLE`
   at 5399 in the same Present, before every exit, and that flat mode can't reach DONE. The reset stays for
   robustness; the symptom and its verify step are dropped.
2. **Gesture and pad state: accepted.** I confirmed `gesture_tick` (953-954) is the only place that clears
   `g_gest_active`, `g_vrpad_suppress` and `g_gest_grab[]`. A stale `g_gest_active` also disables the floor
   clamp at 1301, which isn't gated on xr.
3. **Managed `_ovr_enabled` / `_stereo_on` forced on (managed 1991): accepted as an owner decision for G1.**
   The recommendation is to remember both on the `_xr_want` off-to-on edge and restore them on on-to-off,
   about 4 lines in managed.
4. / 5.: agreed. The anchor reset is now redundant with R4's gap snap but harmless. R15 must keep the
   end-of-`xr_tick` exit reachable.

Final fix (native, defined above `ffxvr_xr_set`):
```cpp
static void leave_xr_state() {
    g_xr_want = false; g_xr_active = false;
    g_ovr_suspend = false; g_xr_screen_now = false; g_screen3d_now = false; g_backdrop_room = false;
    g_frames_without_cam = 0; g_blink_frames = 0; g_move_delta = 0.f; g_move_held = false;
    g_ui_state = UI_IDLE; g_ui_early = g_ui_early_keep = false; g_ui_last_frame_had_pass = false;   // robustness
    g_gest_active = false; g_vrpad_suppress = false; g_gest_grab[0] = g_gest_grab[1] = false;
    g_gest_chord_down = g_gest_reset_down = g_gest_nchord_down = false;
    g_fp_anchor_valid = false; g_seat_latched = false; g_fp_yaw_latched = false;
    diag("xr: headset off, VR-only state reset");
}
```
It is called at 5190 (after `xr_stop()`), at 5321 (after `xr_stop()`) and at 5489
(`if (!xr_active()) leave_xr_state();`).

Verify in game:
1. In a menu (screen mode) turn the headset off: with the override enabled, the desktop numpad moves the
   camera.
2. In the blitzball stereo screen, headset off: desktop eye 2 is a normal eye 2.
3. Hold a grip while turning the headset off, turn it back on, grab: the view doesn't jump.
4. Disconnect Virtual Desktop: the diag line appears once.
5. Regression: F9 pick still works on the desktop.

Effort: S.

---

## R39 + R48 - Cursor hiding: several skip ranges, more quads, correct base

Final verdict: **fix now**.

Response:
1. **Write-combined read cost: accepted.** One shared vertex read per draw for hover, hide and pointer (a
   per-draw cache), and a counter on the `perf:` line. **The cap is accepted partly:** I set it to **32 quads**
   (128 vertices) rather than 64, plus a "batches truncated" counter. If that counter shows hidden-kind
   batches over 32 quads, raising to 64 is a one-constant change after a perf check. The pick keeps its own
   read (armed frames only).
2. **The R48 `start` mapping: accepted, the conservative version.** Base vertex is applied exactly. A
   non-zero `start` is only used when `start % 6 == 0`, with the quad mapping, and it is counted
   (`g_ui_start_nonzero`). Otherwise the reader returns 0 vertices (no hiding for that draw, as safe as
   today's "unknown"). One census then tells whether it is live.
3. **Export swap vs R13: accepted.** Keep `ffxvr_draw_skip` as a shim that returns the envelope (first
   range start to last range end) for compatibility. Remove it after R13 lands.
4. Details accepted: `uint64_t qmask` with `1ull << q`, `q < 32`; full hide unchanged (matched == nq_draw
   && nq >= nq_draw); the native ranges call only when bit 0x20000 is set.

Final fix:
- Native: `static const unsigned k_ui_max_q = 32;`. Add `ui_quads_cached(const float** xy)`: once per draw,
  keyed on `g_draw_in_frame`, it calls `ui_sprite_xy(g_ui_xy, k_ui_max_q * 4)` into a static
  `float g_ui_xy[2 * 4 * k_ui_max_q]`, adds to `g_ui_verts_read`, and bumps `g_ui_trunc` when
  `g_draw_n / 6 > k_ui_max_q`. `ui_hover_collect` (1966) and the hide / pointer block (3773, 3797) use it.
- `ui_sprite_xy` (1988-2005) and `ui_sprite_rect` (2103):
  ```cpp
  if (g_draw_base < 0) return 0;
  unsigned first_v = (unsigned)g_draw_base;
  if (g_draw_start != 0) { g_ui_start_nonzero++; if (g_draw_kind != 1 || g_draw_start % 6 != 0) return 0; first_v += (g_draw_start / 6) * 4; }
  // bounds: voff + (first_v + nv) * stride <= mapped size, else nv = 0
  ```
  `ptr_sprite_capture` (596): `off[k] += (first_v + q * 4) * stride[k]`, with the same `first_v` helper.
- Matching (3762-3812): one read, a `uint64_t qmask` from the hide list OR the pointer rule (the pointer
  rule no longer skipped when the hide list matched), then runs become `SkipRange g_skip[8]` with
  `g_skip_overflow` (extra runs stay visible), `return (g_ui_flat_draw_now ? 0 : 1) | 0x20000`.
- Exports: `int ffxvr_draw_skip_ranges(unsigned* out, int max)` (pairs relative to the draw's start), and
  `ffxvr_draw_skip` kept as the envelope shim.
- Managed `h_drawindexed` / `draw_pieces` (556-560, 590-594): `stackalloc uint[16]`; ranges are fetched only
  on 0x20000 and validated (sorted, non-overlapping, f+c <= n, otherwise a full draw). Pieces are drawn
  between the ranges, and the same ranges are used for the eye-2 re-issues.
- The `perf:` line gains `ui verts read %u, truncated %u, start!=0 %u, skip overflow %u`.

Verify in game:
- Battle, headset, hide cursor and hide pointers on, target a group: only the pointers vanish, and damage
  digits and names between them stay.
- `perf:` frame time with hiding on vs off in the same battle: the difference is within noise.
- The counters: `truncated` and `skip overflow` are about 0. Note `start!=0`; if non-zero, arm one frame
  census and check those draws' `start=` / `base=` against the rects in hover mode 2.

Effort: S-M.

---

## R40 - Stale g_world_report
Final: **fix now**, as proposed: `g_world_report_frame`; when `g_world_live_n == 0` for more than 2 frames,
set `g_world_report_n = 0` (begin_frame 3570-3574).
Status: **AGREED**. Verify: stereo screen with auto depth, a cutscene cut to an empty landscape shot: the
status shows visible 0 within a few frames and the plane eases to the set distance. Effort: S.

## R43 - Pointer sprites released before drawing
Final: **fix now**. Move `ptr_sprites_release()` from 3502 to right after `xr_tick()` (3528). Native
`g_ptr_reuse = false` (cosmetic, managed pushes it). Update the overlay checkbox text. Watch for
WRITE_DISCARD renaming (wrong art); the follow-up is to copy the quad's vertices at capture time.
Status: **AGREED**. Verify: battle, "game's own cursor art" and hide pointers on: "sprites drawn last frame"
is above 0, and the `cursor art:` diag line shows captures. Effort: S.

## R44 - Projected-list index used as a live-list index
Final: **fix now**. `g_actor_ui_src[]` is filled in `actors_project_ui`; `quad_is_pointer` returns
`g_actor_ui_src[best]`. Status: **AGREED**. Verify with R43: the art sits over the right actor with one
actor off screen. Effort: S.

## R42a - g_bg_rects filled twice per draw
Final: **fix now**. A dedupe on `g_draw_in_frame` inside `ui_backgrounds_note`, after the per-frame reset
(1912). With R39's shared read it is also called once per draw by construction; keep the guard anyway.
Status: **AGREED**. Verify: F11 hover mode 2 vs off gives the same pointer-hit count in the overlay.
Effort: S.

## R42b - RGBA atlas dump R/B swapped
Final: **fix now**. Masks chosen by format (1866): RGBA uses R = 0xFF and B = 0xFF0000. Add
`R8G8B8A8_UNORM_SRGB` and `B8G8R8A8_UNORM_SRGB` to the filter (1851). Needs R5 so the `.dds` survive a build.
Status: **AGREED**. Verify: an RGBA dump opens with the right colours. Effort: S.

## R42d - Dead hook functions
Final: **fix now**. Delete `hk_DrawIndexed`, `hk_Draw`, `hk_DrawIndexedInstanced`, `hk_DrawInstanced`
(3143-3168), `hk_UpdateSubresource` (3224-3246), their `g_orig_*` (99-102, 105), unused typedefs and
`twin_plain_srv` (4659). Note: the lost `g_vb_capture` UpdateSubresource branch means `g_vb_updates` is
always 0. Port it into `ffxvr_on_updatesub` only if a census needs it. Status: **AGREED**. Verify: it
builds, and a frame census still writes. Effort: S.

## R42c - Hands calibration request stays pending
Final: **fix later (with hands)**. A request frame stamp: it is honoured only within 3 frames, always
cleared at the top of `hands_compute`, and "calibration ignored (why)" goes in `g_hands_why`.
Status: **AGREED**. Verify (hands): calibrate in the seat, then enter first person: no pulse, no change.
Effort: S.

## R45 / R46 - Eye-2 split variant before eye-2 camera; e.eye1 before hand deltas
Final: **fix later (with hands)**. Can ride with R14 because it is neutral with hands off. Reorder
2760-2775: eye2 assign, eye-2 `write_camera` with the PV save/restore, `bones_record`, then
`e.eye1.assign`. (The critic confirmed `bones_record` doesn't touch `g_last_PV*`.) Status: **AGREED**.
Verify (hands): in split mode both hands land in both eyes. Effort: S.

## R47 - World matrix +624 vs +640
Final: **fix later (with hands)**. Pass `maybe_override`'s base-aware `W` into `bones_record` (2572);
comment `record_buffer` (2828). First check `g_ovr_stats` for mask bit 128 on sizes ≥ 1872.
Status: **AGREED**. Effort: S.

---

## Cross-group

- G1: R4 reads `g_dt` once per frame at the first upload; G1's QPC dt at the top of `begin_frame`
  (1-100 ms) is exactly what it needs. Don't keep a second dt from `cuts_set`. G1 also owns the
  `_ovr_enabled` / `_stereo_on` restore (R6, point 3) and the new ini keys and sliders (R4) if managed edits
  are theirs.
- R13 group: exports added (`ffxvr_view_smooth_set`, `ffxvr_draw_skip_ranges`), one removed
  (`ffxvr_anchor_set`), one kept as a shim (`ffxvr_draw_skip`).
- R4, R6, R14, R15, R20 and R45/46 touch `maybe_override` / `write_camera` / the `xr_tick` exits: merge them
  in one "view state" build.
