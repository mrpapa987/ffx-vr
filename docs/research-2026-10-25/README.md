# Overnight round, 24-25 Oct 2026: battle load times, frame budget, VR opportunities

A second team round on the Ghidra decompile of FFX.exe (static analysis; nothing measured in game yet), plus
the code builds prepared and reviewed overnight.

- `battle-load.md` - why the 2nd+ random battle in an area loads slowly, where the stalls come from, and the
  ranked caching options (read its correction box first).
- `battle-data-load.md` - the battle-data side: one battle map per encounter zone, what is freed/reloaded.
- `exe-verify-round2.md` - the verifier's checks (overrides the two above where they differ), plus what
  Fahrenheit's file loader allows and the 32-bit address-space budget.
- `perf-plan.md` - frame-time budget, the 90/90 settings recipe, staged GPU savings.
- `vr-opportunities.md` - ranked, ready-to-build designs (loading black screens, rumble, 3D damage numbers,
  battle diorama framing, per-area view memory, FMV, dialogue near the speaker).
- `BOARD.md` - the team's shared board.

## Key findings

**Battle load times.** Every random battle frees the field map, loads the battle map into the game's only map
slot, and reloads the field map afterwards; each map free also empties the character model cache. All random
battles in a zone use the same battle map, so battles 2, 3, ... reload what battle 1 freed. The black screens
are the game waiting (no timeout) for its single loader thread, which opens/reads/decompresses/creates every
texture one by one. The engine already caches HD files by path (refcounted), and the model cache can be kept
across battles - but its memory is NOT self-bounded (verifier), and Fahrenheit keeps the game pool below 2 GB on
purpose, so any caching needs its own limits. Ranked options: (1) bounded skip of the model-cache flush on
random-battle entry/exit in the same zone; (2) hold one extra reference on HD map/character textures until a
real area change; (3) a RAM copy of the small battle files; (4) a Defender exclusion for the texture folder.
Option 1 is built behind `battle_cache_models` (default off) together with per-battle load timing logs;
one session decides the rest.

**Frame rate.** Blitzball's 30 fps is FFX-60fps falling back by itself when frames average over 18.2 ms. The
game renders at 3840x2160 while the headset needs ~2496 px wide per eye: 2560x1440 should cut field GPU time from
11-13 ms to ~7-8.5 ms (blitz ~12-15 ms), making 90 Hz with fps=90 realistic. 120 Hz with fps=60 is the
no-GPU-cost fallback that removes 72 Hz judder. Staged code savings: blitz-only MSAA 2x, skip eye 2 on
flat-screen frames, a safer design for skipping identical eye-2 passes, cheaper headset layers.

**VR opportunities (ranked).** (1) No more black screens during loads: turn the render thread's endless idle
wait into a timed one that re-sends the last frame, head-tracked, with no extra D3D work (S-M). (2) Rumble to
the Quest controllers, with the PC port's motor-value quirk decoded (built overnight). (3) The game's damage
numbers in 3D above targets, the cursor approach again. (4) Battle diorama auto-framing incl. bosses.
(5) Per-area view memory. (6) FMV handling. (7) Dialogue near the speaker (workable for field conversations).
