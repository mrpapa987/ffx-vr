# Changelog

All notable changes for players. Versions follow [Semantic Versioning](https://semver.org/); the
`-alpha.N` releases are tested on one setup only (Quest 3, Virtual Desktop VDXR, RTX 3080 Ti).

## 0.1.0-alpha.1 - unreleased

The first public release.

**Requires** FINAL FANTASY X/X-2 HD Remaster (Steam, after the 1 October 2026 patch), Fahrenheit
v1.0.0-alpha12 (the official release), the .NET 10 x86 runtime, and a Quest 3 with Virtual Desktop's
VDXR OpenXR runtime (120 Hz or 72 Hz, SSW Disabled, the game's V-Sync off). FFX-60fps at 60 fps is
recommended.

### What's in it

- **Stereo 3D in the headset** for the field, battles and in-engine cutscenes, with head tracking and
  the headset's own field of view.
- **Three views**, switched with right grip + right stick click: **first person** (your own body
  hidden, your shadow kept), **third person** behind the player, and a **tabletop diorama** you can
  grab, move and scale with the grips. In battle, first person follows whoever is on the field and the
  summoner during a summon; left grip + right stick click cycles through the party.
- **A UI panel** for menus, dialogue and the battle command window, which comes closer when the scene
  does and has a soft backing behind the text.
- **A virtual screen** for 2D scenes, pre-rendered movies and Fahrenheit's save screen; a **stereo 3D
  screen** with automatic depth for blitzball (optionally for the whole game).
- **The game's own battle cursors in 3D** over their targets.
- **The battle-entry glass break** shown in the headset.
- **Quest controllers as the gamepad**, with the game's rumble on the controllers.
- **Smooth stick turning at the headset's refresh rate** (stick reprojection), and the last picture
  kept, head-tracked, while the game loads instead of a black headset.
- **Faster loads in the headset**: the game's loader no longer waits while the mod waits for the headset's next frame.
- **4x MSAA** handled by the mod; the F8 overlay warns when the game renders below the headset's
  resolution.
- **The F8 overlay** for every setting, saved to `ffxvr.ini` (hand edits are read back while the game
  runs).
- **Installer** (`tools\install-mod.ps1`) that checks every requirement, and `tools\run-ffx.ps1` that
  starts the game and collects the logs for a bug report.
- **Small logs by default**: one `ffxvr_diag.log` per session, at most 4 MB; the diagnostics for bug
  hunting are off and switched with `tools\set-logging.ps1 -Profile Dev | Release`.

### Known issues

- Stick turning can still show some ghosting (head turning is clean).
- The heat haze over fire is off in the headset (it was wrong in one eye).
- The dream-sequence filter is hidden in the headset; barely tested.
- Only Virtual Desktop (VDXR) on a Quest 3 has been tested; SteamVR is not supported.
