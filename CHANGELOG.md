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
- **Quest controllers as the gamepad**, with the game's rumble on the controllers. They also work in Fahrenheit's
  save / load screen; the left stick acts as a gamepad's in battle and menus (it follows your view only while you walk);
  in battle and the diorama a quick squeeze of a grip is LB / RB (switch characters, scroll the turn order) while a held
  grip still grabs. The triggers need about 12 % of their travel to press (XInput's standard threshold), so a finger resting
  on one presses nothing.
- **Smooth stick turning at the headset's refresh rate** (stick reprojection), and the last picture
  kept, head-tracked, while the game loads instead of a black headset.
- **Faster loads in the headset**: the game's loader no longer waits while the mod waits for the headset's next frame.
- **4x MSAA** handled by the mod; the F8 overlay warns when the game renders below the headset's
  resolution.
- **The F8 overlay** for every setting, saved to `ffxvr.ini` (hand edits are read back while the game
  runs): a header with the headset switch, Recentre, the status and the version, then tabs (View, Controls, Battle,
  Overdrives, Movies, Hands). Every tooltip names its ini key; experimental settings are marked, and a setting that needs
  the next launch says so. The **Developer** checkbox (`diag_verbose`, off by default) adds the Developer tab and the
  verbose log; the engine-lock option for loads (`load_lock_release`) is there now, not on a player tab.
- **Installer** (`tools\install-mod.ps1`) that checks every requirement, and `tools\run-ffx.ps1` that
  starts the game and collects the logs for a bug report.
- **VR hands (experimental, off by default)**: your hands drawn at the controllers in first person (optionally in
  the diorama, third person and the seat view), depth-tested in both eyes, fingers following the trigger, grip and
  touch sensors. F8 > Hands > **hand model** picks the look: our generic dark glove or bare hand (MIT), or a pack
  you make from a model you own with `tools\import-hands.ps1` (Python 3 + numpy). Such a pack is marked private and
  stays on your PC; the mod's tools never package, copy or delete it. With VR hands on, **tap to press action**: squeeze
  the grip and push your hand forward about a hand's width toward what you face to press confirm (talk, examine, open) in
  the field in first person; never in battle, menus, the sphere grid or the save screen, nor (unless you tick it) while a
  text box is up.
- **Chocobo rides in first person**: your eyes on Tidus's head on the bird, the bird's running bob smoothed
  (F8 > View > First person > Riding), the bird drawn under you (or hidden, if its neck is in the way);
  third person and the diorama follow the bird too.
- **Small logs by default**: one `ffxvr_diag.log` per session, at most 4 MB; the diagnostics for bug
  hunting are off; `tools\set-logging.ps1 -Profile Verbose` turns on a fuller log for a bug report, and
  `-Profile Release` turns it off again.

### Known issues

- Stick turning can still show some ghosting (head turning is clean).
- The heat haze over fire is off in the headset (it was wrong in one eye).
- The dream-sequence filter is hidden in the headset; barely tested.
- Only Virtual Desktop (VDXR) on a Quest 3 has been tested; SteamVR is not supported.
