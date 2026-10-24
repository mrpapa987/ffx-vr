# FFX VR: agreed fix plan (24 Oct 2026)

How this was made: the item list (docs/review-2026-10-24.md, R1-R48 and S1-S9) was split into five
groups. For each group, one agent proposed fixes, a second agent who hadn't seen the proposal critiqued
it against the code, and the proposer then answered every objection.

**Result: every item is AGREED; nothing is left disputed.** The detailed designs are in
G1-final.md (C# module), G2-final.md (native performance), G3-final.md (camera/view), G4-final.md
(resource and OpenXR safety) and G5-final.md (scripts). Each has line references to commit 9a8715e,
an in-game check and an effort estimate. No code has been changed.

## What the discussion changed from the first review

- **R1 (settings only pushed while the overlay is open).** No default needs retuning: turn speed was tuned
  with the overlay open, where the frame time was right. With the overlay closed, turning and follow ran at
  about 83% speed in normal scenes and about 42% in blitzball. After the fix, blitzball turning and auto-depth
  feel about 2.4x faster: that is the intended speed.
- **R4 (smoothing per upload).** Today there is effectively no smoothing: the anchor snaps every frame and the
  follow-pan rate limits are almost unbounded. The fix makes it once per frame with time constants. **The first
  build ships them at 0 ms (snap), which is closest to today**, so R1's change can be judged on its own. A
  separate A/B session then tries the documented intent (24 ms in the field, 325 ms in battle).
- **R9 (stale UI vertex pointers).** No 2-frame age check, because design-notes records that exact check
  breaking damage numbers and pointers. Instead: purge entries when a buffer is created at a reused address, plus
  a VirtualQuery liveness check once per buffer per frame.
- **R11 (raw D3D view pointers).** The first design would have blacked out every 2D screen in the headset. The
  agreed design is to read the real bindings in eye2_begin/end, plus a "destruction sentinel" on the backbuffer
  that clears our pointer when DXGI frees it, without holding a reference (so ResizeBuffers is unaffected).
- **R8/R15 (swapchain timeout, no image).** Submitting zero layers shows a black frame by spec, so both now hold
  the last frame (up to 30 frames) instead.
- **R18 and R23 (eye-2 cost).** The reviewer's quick fix for R23 would have broken post-processing. R18's
  caching is folded into R11. R23 starts as a counter only, to measure whether skipping passes is worth it.
- **R19.** Cap the per-write copy at 256 bytes instead of gating it: no reader looks further, and gating would
  break the pick and census.
- **R17.** Not urgent: about 0.1-0.2 ms. Left for the blitzball performance pass.
- **R37 (Magus Sisters).** Extend both loops to 18 entries; the first proposal's reason for keeping 16 was wrong.
- **S1 is worse than reported.** `-Id ..` deletes the whole `fahrenheit` folder, `-Id *` every mod, and
  `-Id ffxvr.` resolves to the VR mod's folder on Windows. Fixed with a strict id rule plus an ownership marker
  the script already writes.
- **S4.** The script never exits 1; INSTALL-ALPHA's "exit code 1" is msbuild's logged code. A failed build is now
  only accepted when the known dbghelp error is the only error.
- **S9g withdrawn.** Steam's own Program Files folder is writable for normal users, so the "Program Files isn't
  writable" problem mostly doesn't exist.

## Cross-group loose ends, resolved here

| Conflict | Resolution |
|---|---|
| dt clamp: G1 says 1-50 ms, G2's begin_frame order says 1-100 ms | **50 ms** (G1 owns g_dt; slow motion and load stalls shouldn't make auto-depth jump) |
| Optional exports (G2 keepalive, G4 `ffxvr_xr_debug`) vs G1's "every export required + `ffxvr_abi()`" | **Required.** Both halves always ship together. Every new export bumps the ABI number, so a mismatch shows a clear error instead of degrading silently |
| G2's begin_frame step 11 still clears `g_bb_bound_by_game` | **Strike it.** G4 replaced that flag with the sentinel |
| Where `ptr_sprites_release` goes (R43) | **After `xr_tick`** (between steps 9 and 10 of G2's agreed order) |
| Who edits shared functions | `eye2_begin/end`, the RTV0 cache, backbuffer, `on_ps_srvs`: G4. `ffxvr_xr_set`, XR logging, heartbeat: G2 (including G3's `leave_xr_state`). `g_dt`: G1. |

## One decision for you

**After turning the headset off, should the desktop view go back to how it was?** Turning the headset on
forces the camera override and eye 2 on (module.cs 1991), and nothing turns them off again. So after headset
off, the monitor keeps showing the override camera, with eye-2 rendering still running. Options: (a) restore
whatever they were before the headset went on (recommended), (b) keep today's behaviour.

## Proposed build sequence

Each build is small enough to test in one session, so a regression points at one change set.

| Build | Contents | What you test |
|---|---|---|
| 1. Frame time and pushes | R1 (push_all, native-measured dt), R13 (ABI check), R34-R38, R6 + your decision above, R7 (headset-start back-off, quiet logs) | Stick turn and follow with the overlay closed vs open (should now match); blitzball turning; collapse the overlay window; headset off and on; start with VD disconnected |
| 2. Free frame time | R2, R3, R19, R20, R24, R23 counter | GPU/frame timings in the overlay before and after, field and blitzball; captures, picks and the eye preview still work |
| 3. Small crash-proofing | R9, R10, R14, R16, R40, R43, R44, R42 (export checks, bg rects, dump colours, dead hooks) | A normal session with area changes and battles; F9 picks; cursor hiding still works |
| 4. Smoothing per frame | R4 at 0 ms | Third person, diorama, battle diorama, follow pans: compare with build 3 |
| 4b. Retune (settings only) | R4 at 24 / 325 ms | A/B: keep whichever feels better |
| 5. XR robustness | R8a, R15, R41, watchdog wake, EndSession only in STOPPING | One Virtual Desktop session: disconnect/reconnect, take the headset off, use the test toggles |
| 6. Eye-2 bindings | R12, then R11 on its own | Walk every 2D screen (title, menus, save, FMV, loading), alt-tab, change resolution in game settings |
| 7. Cursor hiding | R39 + R48 | Battles with many targets; damage digits and names stay visible |
| 8. Settings file | R21, R22, R27-R33 | Hand-edit the ini while playing; bad values; quit right after an edit |
| 9. Hold last frame | R8b | VD stalls |
| Scripts (any time, before a zip goes out) | paths.ps1 helpers, R5, S1-S9, in G5's order | Each script's check in G5-final.md; needs a few answers from your PC (listed there) |

**Later:** R17, R23 step 2, R25, R26, and the hands items R42c, R45-R47.
