# G1 final: module.cs (R1, R13, R17, R21, R22, R27-R38)

This is the round-3 result after G1-critique.md. I re-checked every objection against 9a8715e:
- `player_set` and `player_head_set` are per-call integrators (N5269-5291).
- There is no `#if` anywhere in ffxvr_native.cpp.
- The selection-mask comment is at M1888-1891.
- Bools are parsed both as `v == "1"` and as `v != "0"` (M743-823).

I accept all of the critic's points. On one (the .bak recovery in R30) I took the critic's own fallback option, so no item is disputed.
"M" means a module.cs line and "N" means an ffxvr_native.cpp line.

## Shared design (revised)

### Frame pipeline (R1, R34, R36, R38)

```
render_imgui_inner():
  _frames++; _now = ImGui.GetTime();
  try_install_hooks();
  if (_native_ready) { draw_markers(); actors_auto_log(); blitz_auto_log (try); draw_eye2_preview(); }
  if (_native_ok && _fn_begin_frame != null) _fn_begin_frame();
  handle_hotkeys();          // every key, once; WantTextInput gate (R36)
  if (_native_ready) poll_gestures();   // the ONLY call of gesture_poll (read-and-clear)
  _debug_open = false;       // set true by draw_debug when its header is expanded (G2 hook, below)
  if (_window_open) draw_window();      // widgets, status reads, one-shot buttons only
  if (_native_ready) push_all();
  settings_tick();
```

### Rules for `push_all`

- **Exactly once per frame.** It starts with `if (_push_frame == _frames) return; _push_frame = _frames;`.
  The setters are plain stores, except four kinds that need exactly one call per frame:
  - the edge-triggered `xr_set` and `fp_set`
  - the per-call integrators in `push_player` (`player_set` step and still counter, `player_head_set` EMA)
  - the read-and-clear calls: `gesture_poll`, `markers_game_art`, `ui_pointer_stats`, `ui_hidden_draws`, `ui_rect_stats`, `ui_hide_hits`
  - the guarded cull patches

  Nothing is idempotent under two calls, so **no setter may stay in two places**. The deletion list below is part of the fix.
- **Order:**
  ```
  override_set, stereo_set, turn_set, move_set, cull_dist (on change), msaa_set, vrpad_set,
  imgui_layer_set, gesture_set, fp_body_set (cached mask), cull_scene (on change),
  xr_pitch_follow, cuts_set(..., ImGui.GetIO().DeltaTime), backdrop_set, hist_set, ui_set, noimg_set, bisect_set,
  [G2] diag_arm(_debug_open ? 1 : 0),
  push_player(),  push_hands(),  tp_set, fp_set,
  _xr_on = xr_set(...) != 0
  ```
  - `cuts_set` keeps passing the real ImGui dt. Native ignores it after the fix, but an old native DLL still gets the right value.
  - `push_player` comes before `fp_set` (as today), because it can switch the view for a battle.
  - `push_player` keeps `xr_screen_set` at M1813. It was missing from my round-1 list.
- **Flatten the hidden gate.** M1803-1823 are all inside `if (_fn_ui_hide != null)`. Give each call its own null check.
- **Deleted from the UI:** M1020, M1026 (keep the `cut_count` text), M1057-1060, M1069, M1088, M1110-1111, M1125, M1208, M1237, M1438,
  M1042 together with `_chord_toggle_pending`, and the whole else branch M1982-1994.
  The checkbox at M975-986 becomes `Checkbox("Headset", ref _xr_want)`.
- **Dev-only calls stay in the UI:** `bone_wiggle` (M1347) and `hands_method` (M1330), as today. Closing the tree leaves a lifted bone lifted, as today.

### Compatibility with the other groups

- **G2.**
  - The `xr_start` back-off lives inside `ffxvr_xr_set`. `push_all` calls `xr_set(want)` every frame, as today, and adds no managed retry.
  - G2's "Debug open" export gets a level-triggered value, pushed every frame: 1 while the header is expanded, 0 otherwise.
    So closing the header or the window disarms it.
- **G3.**
  - There is one dt source: QPC at the top of `ffxvr_begin_frame`, clamped to **[1 ms, 50 ms]** by a shared `constexpr float k_dt_min = 0.001f, k_dt_max = 0.05f;`.
  - G3's `k = 1 - exp(-dt/tau)` tick uses `g_dt` as is and drops its own `fminf(g_dt, 0.1f)`, or uses `k_dt_max`.
  - The period is the frame just finished, consumed once per frame. That is consistent for stick turn (in `xr_tick`), auto depth and G3's tick.
  - G3's new ini keys get ranges in R27, and `follow_rate` is widened to 360.
- **G4.** Argument checks go inside the exports, so nothing changes on the managed side. Any signature change bumps `FFXVR_ABI` (R13).
- **All groups:** every new export (G2 diag arm, G3 `view_smooth_set`, R17 `on_draw_ex`) is added as `req` in the same build as its native side.

### Settings lifecycle (R21, R27-R33)

| function | when | does |
|---|---|---|
| `settings_parse` | init and reload | validated, invariant culture, bools via `pb`; `world_scale` routed by view mode |
| `settings_apply_launch` | init only | the autostart block M830-834 |
| `settings_reload_check` | every 1 s of `_now` | reloads a hand-edited file (details in R31) |
| `settings_save_tick` | every 0.5 s | snapshot; writes once the text has been stable for 1 s |
| `settings_write` | from the tick and the exit flush, under `lock (_ini_lock)` | checks for a hand edit first, then flushed temp file + `File.Move(overwrite)`; backup in its own try; log once on failure |

### Build split (kept)

- **Build 1:** R1 (with the guard and the 50 ms clamp), R13 (all `req` + `ffxvr_abi`), R38, R36, R34, R35, R37.
- **Build 2:** R21, R27, R28, R29, R30, R31, R32, R33 (ini check), R22.
- **Perf pass:** R17.

---

## R1 - Settings only reach native while the F8 window is open

Final verdict: **fix now (build 1).**

Fix: the pipeline and `push_all` above, plus:
- **Native**, at the top of `ffxvr_begin_frame` (N3493), before the slow-motion `Sleep`:
  ```cpp
  static LARGE_INTEGER s_dt_prev;
  if (g_qpf.QuadPart == 0) QueryPerformanceFrequency(&g_qpf);
  LARGE_INTEGER now; QueryPerformanceCounter(&now);
  if (s_dt_prev.QuadPart) { float dt = (float)((now.QuadPart - s_dt_prev.QuadPart) / (double)g_qpf.QuadPart);
                            g_dt = dt < k_dt_min ? k_dt_min : dt > k_dt_max ? k_dt_max : dt; }
  s_dt_prev = now;
  ```
- **Native**, `ffxvr_cuts_set` (N5195): delete the `g_dt = dt` store. Keep the parameter.
- `handle_hotkeys()` takes End, Home, Insert, the numpad and Keypad5 (see R36). `poll_gestures()` takes M1481-1488.
  Its view toggles call `toggle_first_person()` directly. Its battle-cycle presses increment `_btl_cycle_req`.
- **Retune plan: change no defaults.**
  - The 90 deg/s turn rate was chosen with the overlay open, i.e. with the right dt.
  - Sessions that never opened the overlay ran at 83%.
  - In blitzball, closed-overlay stick turn and auto-depth easing ran at about 42% (1/72 vs 1/30). They become about 2.4× quicker there. That is the intended speed.
  - The other deg/s filters are dominated by R4 (G3) until that lands.
- **Pre-build check of the owner's ini.** These keys now apply at launch with the overlay closed, so look for non-default values:
  `pitch_follow`, `cut_recentre`, `cut_blink`, `follow_pans`, `follow_rate`, `backdrop_mode`, `ui_panel`, `ui_distance`, `ui_width`, `noimg_mode`, `hist_mode`.
  Their native defaults equal the managed defaults, so only a non-default ini value changes anything.

Status: **AGREED.** The shared `k_dt_max = 50 ms` is pending G3's ack.

Verify in game:
1. Autostart with the overlay closed. One full stick turn at 90 takes 4 s. Open the overlay: still 4 s (the two must now match).
2. Collapse the window: first person keeps following, and the grip chord switches views.
3. With the Debug header open, then closed, G2's diagnostics arm and disarm.
4. F10 slow motion: the camera filters don't run visibly ahead of the slowed scene.

Effort: **M.**

## R13 - Missing native export means a null call

Final verdict: **fix now (build 1).**

Fix:
- Every export is loaded with `req(name)`, which uses `TryGetExport` and collects missing names. Remove `opt` and all four try/catch groups.
  Both DLLs ship together and nothing is conditionally compiled, so a missing name is always a mismatch.
- New native export `extern "C" __declspec(dllexport) int ffxvr_abi() { return FFXVR_ABI; }`.
  It is bumped whenever any export is added, removed or re-signatured. Managed has `const int k_abi = …` and requires them to be equal.
- Mismatch, or a missing name:
  - `_native_ok = false`
  - status reads "ffxvr_native.dll does not match ffxvr.dll (missing …/abi N vs M); reinstall the mod"
  - error log, force-open the window (R38), and no hooks
- `try_install_hooks` requires `_native_ok`.
- `_native_ready = (rc == 0)`. Hooks can't be rolled back, so a partial detour failure keeps running. But `_detour_status != "installed"` is
  shown at the top of the window and force-opens it (R38).
- Null checks at M560, M605 (inside the eye2 loop condition), M977 and M1215, as belt and braces.

Status: **AGREED.**

Verify in game:
- Put an old `ffxvr_native.dll` in place: no crash, the window opens with the mismatch line.
- Put the matching DLL back: normal.

Effort: **S.**

## R17 - Three native calls per draw plus a marshalled chain delegate

Final verdict: **fix later (perf pass).** Fold the per-draw calls. SuppressGCTransition: **won't fix**, because there is nothing left to gain.

Reason, reworded:
- After the fold, the only per-draw native call is `on_draw_ex`. That call re-enters managed code (`ui_pass_begin` → `CopyResource`/`ClearRenderTargetView`
  → `h_copy`/`h_clr`) and takes `g_mutex`, so it can't use SuppressGCTransition.
- The trivial `draw_ctx`/`draw_args` calls would have been safe, but the fold removes them.
- The per-frame setters gain nothing.

Fix:
- **Native:** `ffxvr_on_draw_ex(which, n, start, base)`: return 0 if `g_in_eye2`, else `g_draw_ctx_same++`, set `g_draw_*` and return `ffxvr_on_draw(which)`.
- **Managed, in `h_drawindexed` and `h_draw`:** call `on_draw_ex` for our context only. For other contexts, call only `draw_ctx` (diagnostic).
- **Chain:** optionally switch to `delegate* unmanaged[Stdcall]` via `Marshal.GetFunctionPointerForDelegate`, but only after a log line confirms
  it equals the trampoline address.
- Behaviour change, intended: other contexts and eye-2 draws no longer overwrite `g_draw_*`.

Status: **AGREED.**

Verify in game:
- Blitzball: compare the `perf:` diag line before and after.
- Battle cursors are still hidden.
- F9 pick still lists quads.

Effort: **S.**

## R21 - Save rebuilds text every frame and writes on every drag frame

Final verdict: **fix now (build 2).**

Fix:
- `settings_save_tick` (0.5 s snapshot; write once the text has been stable for 1 s). Optionally, force the write if a change has been pending for more than 10 s.
- `settings_flush()` runs first in `h_exitprocess`, wrapped in try/catch. It only covers ExitProcess: a TerminateProcess or a crash loses less than about 1.5 s of changes.
- `settings_write(text)`:
  - runs under `lock (_ini_lock)`, because the exit hook can run on another thread
  - **first checks for a hand edit:** if the file exists and its mtime or length differs from `_ini_mtime_seen`/`_ini_len_seen`, it doesn't write.
    It sets `_ini_next_check = 0` instead, so the reload runs next frame. The hand edit wins, including in the exit flush.
    A file that doesn't exist may be written.
- **At init**, after the startup read: `_ini_mtime_seen` and `_ini_len_seen` take the file's values, and `_ini_last = raw text`.
  So there is no spurious first reload, and the first write of a session still canonicalises the file and makes `.bak`, as today.

Status: **AGREED.**

Verify in game:
- Drag a slider: no hitch. 2 s after release, the ini has the value.
- Change a slider, then hand-edit another key within 1 s and save: both survive (the edit is reloaded, then the slider is written).
- Hand-edit the ini, then quit within 1 s: the edit is still in the file after exit.

Effort: **S.**

## R22 - Per-frame allocations and syscalls

Final verdict: **fix now (build 2).** Unchanged from round 1:
- `name_is()`: one 16-byte safe read with a per-byte fallback, no allocation. Used at M1779 and M1783.
- `_in_blitz` cached where `_map_last` changes, using ordinal StartsWith.
- Pick result polled for 3 s after F9.
- Head-parts mask cached.

Status: **AGREED.**

Verify in game:
- First person still finds Tidus after a cutscene, a battle and an area change.
- Blitzball overlay routing and the stereo screen still work.
- F9 text still appears.

Effort: **S.**

## R27 - Bad values become 0; nothing is clamped

Final verdict: **fix now (build 2).**

Fix:
- Parse helpers, all keeping the current value on bad input and adding it to `_ini_bad`:
  - `pf` (float: invariant culture, finite, clamped)
  - `pi` (int, clamped)
  - `pe` (int from an allowed set)
  - **`pb` (bool)**: accepts `1/0/true/false/yes/no/on/off`, case-insensitive.
- All bool keys use `pb`. That replaces both the `== "1"` and the `!= "0"` styles.
- One log line per load lists the ignored values.
- Ranges as in round 1, except:
  - `follow_rate` is 1..360
  - G3's `anchor_smooth_ms` is 0..500 and `battle_anchor_smooth_ms` is 0..3000 (the native clamps)
  - G3 confirms the final key names in the same build

Status: **AGREED.**

Verify in game:
- With `world_scale=`, `battle_view=x`, `cull_scene=false` and `gestures=off` in the ini:
  - one "ignored" line for the first two
  - culling stays off and gestures are off
  - the seat scale is still 94
- With `world_scale=5000`: the scale is 300.

Effort: **S.**

## R28 - Live reload in first/third person sets the current scale; the autostart block re-runs

Final verdict: **fix now (build 2).**

Fix:
- `world_scale` is parsed into a local. Afterwards, `if (life_size(_fp_mode)) _scale_before_fp = ws; else _xr_scale = ws;`.
- M830-834 move to `settings_apply_launch()`, which only `init` calls.
- Init order: `_ini_path` is set (M725), then `bone_labels_load()`, then the read and parse, then `settings_apply_launch()`.
  `bone_labels_load` no longer runs on reload.

Status: **AGREED.**

Verify in game:
- In first person, change `turn_rate` in the ini: the world stays life-size, and the turn rate changes.
- Switch to the diorama: the scale is the ini's `world_scale`.

Effort: **S.**

## R29 - Unwritable ini logs every frame

Final verdict: **fix now (build 2).**

Fix:
- Write: log once on the first failure, retry every 5 s, log once on recovery, and delete `ffxvr.ini.tmp` on failure.
- **The backup is its own try/catch.** It is logged once, and `_ini_backed_up = true` either way, so a bad `.bak` never blocks saving.
- Reload: catch `IOException` and `UnauthorizedAccessException`. Use the same log-once and log-on-recovery pattern (`_ini_read_fail_n`).
  The mtime stays unmarked, so the read is retried quietly every second.

Status: **AGREED.**

Verify in game:
- Read-only ini, then change a slider: exactly one warning. Clear the flag: "saving works again" within 5 s.
- Read-only `.bak`: saving still works.

Effort: **S.**

## R30 - Non-atomic write; a short ini overwrites .bak

Final verdict: **fix now (build 2).**

Fix:
- Write the temp file through `new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None)` + `StreamWriter` (UTF-8, no BOM).
- Call `Flush(true)` before `File.Move(tmp, ini, overwrite: true)`.
- Back up once per session, and only if the startup ini parsed at least 10 known keys.
- At init, if the ini parsed fewer than 10 known keys and `.bak` exists: log a warning ("ffxvr.ini looks truncated (N keys); your previous settings are in ffxvr.ini.bak").
  Show it in the status line of the overlay for the session.
- **No automatic load of `.bak`.** A tester's deliberately minimal ini (e.g. just `headset_autostart=1`) must not be replaced by an old backup.
  This is the critic's own "otherwise at least log" option.

Status: **AGREED.**

Verify in game:
- Normal play leaves `ffxvr.ini` and `.bak` and no `.tmp`.
- Cut the ini to 2 lines and start: `.bak` is untouched, and the log and overlay show the "looks truncated" note.

Effort: **S.**

## R31 - Autostart edit reverted, reload Sleep, failed read loses the edit

Final verdict: **fix now (build 2).**

Fix: `settings_reload_check` every 1 s of `_now`:
- The mtime and length are only marked seen after a successful read. A failed read (`IOException` or `UnauthorizedAccessException`, logged once) is retried next second.
- **No Sleep on reload.** The retry loop with sleeps stays for the init read only.
- Parse with no launch effects. File values win: drop the `autostart ||` at M856.
- Keep `_window_open` (it is per session).
- Clear `_ini_pending`.
- **`_ini_last = ini_text()`** (canonical, after clamping). A hand-edited file then stays as typed until a real setting changes.
- **A deleted ini is NOT recreated by the reload.** As today, it reappears on the next setting change, which `settings_write` allows.
  To reset the mod, delete the ini with the game closed. This goes in INSTALL-ALPHA.

Status: **AGREED.**

Verify in game:
- With autostart on, set `headset_autostart=0` on disk: after 2 s it is still 0, and the checkbox is unticked.
- Save an edit while the file is locked for 3 s: no hitch, and the edit is picked up after the lock is released.
- Edit one key with comments in the file: the comments stay until you next move a slider.

Effort: **S.**

## R32 - Ints written and read with the current culture

Final verdict: **fix now (build 2).**

Fix:
- Every int in `ini_text()` goes through `.ToString(CI)`.
- `num()` maps U+2212 to '-'.
- `bone_labels_load` and `_save` use `CI`.

The read side was overstated in the review: .NET 5+ accepts ASCII '-' under a U+2212 culture.

Status: **AGREED.**

Verify in game (optional): with the Swedish regional format, `battle_view=-1` is written with an ASCII hyphen.

Effort: **S.**

## R33 - Timers count frames

Final verdict: **ini check: fix now (build 2). Battle-follow waits: won't fix.**

Reason: FFX logic is frame-locked, so counting render frames tracks the run-in animation under slowdown and F10. A wall-clock timeout would fire early.

Fix:
- The ini check uses `_now` (R31).
- Rewrite the comment at M1760: "15 render frames ≈ 0.25 s at 60 fps. At 60 fps render / 30 Hz logic, every other frame shows no
  movement, so the still count only grows once the actor stops on logic steps too. That is why 0.3 per sample works at 30 and at 60 fps."

Status: **AGREED.**

Verify in game: with F10 on, an ini edit is picked up within about 2 s.

Effort: **S.**

## R34 - Battle-cycle chord fires later

Final verdict: **fix now (build 1).**

Fix:
- One unconditional `_btl_cycle_req = 0;` at the end of `push_player` (after M1777). The round-1 clear sites are dropped.
- `handle_hotkeys` (Insert) and `poll_gestures` (the chord) run before `push_player` in the same frame. A request is therefore consumed by the manual
  branch at M1733 in that frame, or dropped. That covers the field, summons, frames with `roster_ok` false and `plys == null`.
- The Insert check moves out of M1732 into `handle_hotkeys`.

Status: **AGREED.**

Verify in game:
- Do the chord twice in the field, then start a battle: the eyes go to Tidus.
- Press Insert during a summon, then dismiss: the eyes go back to whoever had them before the summon.
- Press Insert in battle: the next party member gets the eyes.

Effort: **S.**

## R35 - hands=1 with hands_dev=0 runs VR hands

Final verdict: **fix now (build 1).** `push_hands` passes `(_hands_dev && _hands_on) ? 1 : 0`. The setting isn't mutated there; M1289 stays.

Status: **AGREED.**

Verify in game: with `hands=1` and `hands_dev=0`, first person with the overlay closed keeps the whole body hidden.

Effort: **S.**

## R36 - Hotkeys fire while typing

Final verdict: **fix now (build 1).**

Fix: `handle_hotkeys()`:
- F8, F10, F11 and F9 (F9 only when `_native_ready`) are always live.
- Then `if (ImGui.GetIO().WantTextInput || !_native_ready) return;`.
- After that: Home, End (`toggle_first_person`), Insert, Keypad5 (reset + motion log), the numpad nudges and Keypad1/3 turn.
- `WantTextInput` is used, not `WantCaptureKeyboard`.
- PageUp/PageDown in the bone finder stay where they are.

Status: **AGREED.**

Verify in game:
- Type "2,3" with the numpad into "head parts": the camera doesn't move and the view doesn't reset.
- Click outside the box: the numpad works again.

Effort: **S.**

## R37 - Magus Sisters: 16 vs 18 player entries

Final verdict: **fix now (build 1).** I accept the critic's reading.

The code's own model (M1888-1891) is one mask over combatant ids: monsters are 20+slot, so bits 16-19 are combatant ids 16-19, i.e. Sandy and Mindy.
Working monster markers depend on that model. My round-1 claim that "bits 16-17 are monster bits" contradicted it.

Fix:
- Add `const int k_ply_n = 18, k_mon_n = 16;`.
- Extend the player loops to 18 at M1658 (split from the monster loop), M1875, M1892-1893 (players only; monsters stay 16) and M1897.
- Keep the `pc.id < 32` checks.

Status: **AGREED.**

Verify in game (when the sisters are available):
- The diorama centres on all three sisters.
- Each sister's turn gets the turn marker.
- Targeting a sister with a party command marks her. The 1 s `window:` log line shows `sel_chrs`/`sel_mons` to confirm the bit model.

Effort: **S.**

## R38 - Native load failure shows nothing on F8

Final verdict: **fix now (build 1).**

Fix:
- `draw_window()` always draws when `_window_open`.
- With `_native_ready` it shows the full UI. Otherwise it shows the status panel: native status, the reinstall hint and "F8: hide".
- When `_detour_status != "installed"`, a red status line goes at the top of the full UI.
- **Hard failure** means: missing DLL, load threw, export or ABI mismatch, init rc != 0, or detours not installed.
  On a hard failure, `_window_open = true` once, and `_window_open_forced = true`.
- **The forced open never reaches the ini.** While `_window_open_forced` is set, `ini_text()` writes `overlay_open` from the value before the
  failure (`_window_open_pre_force`). The flag clears when the player presses F8.

Status: **AGREED.**

Verify in game:
- Rename `ffxvr_native.dll` and start: the window opens with "missing: ffxvr_native.dll".
- Quit: the ini still has the previous `overlay_open`.
- Restore the DLL: normal start with the overlay as configured.

Effort: **S.**

---

## Status list

AGREED: R1 (the 50 ms dt clamp is pending G3's ack), R13, R17, R21, R22, R27, R28, R29, R30, R31, R32, R33, R34, R35, R36, R37, R38.
DISPUTED: none.
