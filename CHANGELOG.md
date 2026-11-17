# Changelog

All notable changes for players. Versions follow [Semantic Versioning](https://semver.org/); the
`-alpha.N` releases are tested on one setup only (Quest 3, Virtual Desktop VDXR, RTX 3080 Ti).

## 0.1.0-alpha.1 - 17 November 2026

The first public release.

**Requires** FINAL FANTASY X/X-2 HD Remaster (Steam, after the 1 October 2026 patch), Fahrenheit
v1.0.0-alpha12 (the official release), the .NET 10 x86 runtime, and a Quest 3 with Virtual Desktop's
VDXR OpenXR runtime (120 Hz or 72 Hz, SSW Disabled, the game's V-Sync off). FFX-60fps at 60 fps is
recommended. The Re-Remastered HD texture pack is optional and **not recommended without hardware stronger than an
RTX 3080 Ti**: it caused performance problems on the developer's PC, and it needs a lot of disk space (the Medium pack
is about 18 GB of loose files once installed).

**Updating from a test build:** your `ffxvr.ini` is kept, so the new defaults below do not reach you by themselves. VR
hands were off by default before this release: tick F8 > Hands > "VR hands: your own hands at the controllers" to get
them (the cloister sphere grab and tap to press need them). The other defaults that changed: Auron's Bushido on
"strike the targets" with the targets at 0.76 m, size 1.6 and "More time for Bushido (x)" 2 (F8 > Overdrives), the
battle start effect "head-locked" (F8 > Battle), and `hd_cluster_cache` on. Or delete `ffxvr.ini` with the game closed
to start from the defaults.

### What's in it

- **Stereo 3D in the headset** for the field, battles and in-engine cutscenes, with head tracking and
  the headset's own field of view.
- **Three views**, switched with right grip + right stick click: **first person** (your own body
  hidden, your shadow kept), **third person** behind the player, and a **tabletop diorama** you can
  grab, move and scale with the grips. In battle, first person follows whoever is on the field and the
  summoner during a summon; left grip + right stick click cycles through the party.
- **A steady view during scripted moves** (F8 > View > First person > "Steady view during scripted moves", on by
  default): in first person, when the game moves your character by itself at an interaction (taking a cloister sphere,
  for one), your eyes stay where you stood instead of being pulled along.
- **Battles show everyone who should be there.** In first person only your own character is hidden (F8 > View > First
  person > "Your body in battle": "hide by model (only your own character)"), so an aeon or a party member who comes
  close stays visible. Characters the game's battle camera would hide for a moment during some actions stay shown in
  first person and the diorama (F8 > Battle > "Characters the game's battle camera hides": "keep them shown"). Spells
  that draw a character by themselves (Holy, and probably others) now show the target and their effects in the 3D view
  with the battle menus on their panel (F8 > Battle > "Characters a spell draws by itself": "in 3D"). "Aim spell
  effects at your eyes" is there to try (experimental, off by default).
- **The battle diorama is a table centred on the fight**: the party members on the field and the enemies, not the
  members on the bench (F8 > Battle > "Battle diorama: centre on the fight"). It starts each battle from the default
  view, looking from the party toward the enemies ("Battle diorama: start each battle from the default view"), holds
  still while characters run in to attack, and puts the field's own diorama view back when the battle ends.
- **A UI panel** for menus, dialogue and the battle command window, which comes closer when the scene
  does and has a soft backing behind the text.
- **A virtual screen** for 2D scenes, pre-rendered movies and Fahrenheit's save screen; a **stereo 3D
  screen** with automatic depth for blitzball (optionally for the whole game).
- **The game's own battle cursors in 3D** over their targets.
- **The battle-entry glass break** shown in the headset, locked to your head by default (F8 > Battle > "Battle
  start effect:": "head-locked"; "frozen world" and "original" are the other choices).
- **Quest controllers as the gamepad**, with the game's rumble on the controllers. They also work in Fahrenheit's
  save / load screen; the left stick acts as a gamepad's in battle and menus (it follows your view only while you walk);
  in battle and the diorama a quick squeeze of a grip is LB / RB (switch characters, scroll the turn order) while a held
  grip still grabs. The triggers need about 12 % of their travel to press (XInput's standard threshold), so a finger resting
  on one presses nothing.
- **Smooth stick turning at the headset's refresh rate** (stick reprojection), and the last picture
  kept, head-tracked, while the game loads instead of a black headset.
- **Faster loads in the headset**: the game's loader no longer waits while the mod waits for the headset's next frame,
  and the textures of the field and battle maps stay loaded across random battles in one zone (`hd_cluster_cache`, on
  by default; its switch is in the Developer tab).
- **4x MSAA** handled by the mod; the F8 overlay warns when the game renders below the headset's
  resolution.
- **The F8 overlay** for every setting, saved to `ffxvr.ini` (hand edits are read back while the game
  runs): a header with the headset switch, Recentre, the status and the version, then tabs (View, Controls, Battle,
  Overdrives, Movies, Hands). Every tooltip names its ini key; experimental settings are marked, and a setting that needs
  the next launch says so. The **Developer** checkbox (`diag_verbose`, off by default) adds the Developer tab and the
  verbose log; the engine-lock option for loads (`load_lock_release`) is there now, not on a player tab.
  In the headset the F8 menu is **a panel fixed in the room**, about 1 m ahead and a little below your eyes, sharp
  enough to read: squeeze a grip on it (pointing at it or touching it) to move it, both grips to resize it; it opens where
  you left it, and Home or **Bring here** puts it in front of you again. Point at it with either controller: a beam and a
  dot show where, the trigger clicks, the stick scrolls, and neither reaches the game meanwhile.
  Hold the left grip and press the left menu button to open or close the menu, as F8 does (the button alone is still
  Start; Virtual Desktop keeps that button's long press and double tap for its own menu).
- **Installer** (`tools\install-mod.ps1`) that checks every requirement, and `tools\run-ffx.ps1` that
  starts the game and collects the logs for a bug report.
- **VR hands, on by default**: your hands drawn at the controllers in first
  person (optionally in the diorama, third person and the seat view; never in battle), depth-tested in both eyes,
  fingers following the trigger, grip and touch sensors. A fresh install shows our generic dark glove; F8 > Hands >
  **hand model** picks the look: that glove or our generic bare hand (MIT), or a pack
  you make from a model you own with `tools\import-hands.ps1` (Python 3 + numpy). Such a pack is marked private and
  stays on your PC; the mod's tools never package, copy or delete it. With VR hands on, **tap to press action**: squeeze
  the grip and push your hand forward about a hand's width toward what you face to press confirm (talk, examine, open) in
  the field in first person; never in battle, menus, the sphere grid or the save screen, nor (unless you tick it) while a
  text box is up.
- **Cloister of Trials spheres with your hands (Djose only so far)**, in first person with VR hands (F8 > Hands >
  "Grab spheres with your hands (Cloisters of Trials)"): a label names the sphere or socket your hand comes near
  ("Take ...", "Insert ..."; "Step closer" when you are not near enough), and a squeeze of that hand's grip takes or
  places the sphere, the game's prompt answered for you (in English; in another language the grip presses A once and
  the prompt shows as usual). The sphere you carry is drawn in your palm ("Carried sphere in your hand", "Sphere
  straight to and from your hand"), and the game's take / place animation passes quickly ("Fast sphere grab", with a
  speed slider). The Destruction Sphere and the other cloisters still use the A button.
- **Overdrives with gestures** (F8 > Overdrives; the buttons always keep working): Lulu's Fury by drawing circles, and
  for Auron's Bushido cut each input's target along its arrow with a blade in either hand ("strike the targets", on
  by default; the targets sit 0.76 m ahead at 1.6 times size, with twice the game's time). "Practice targets" lets you try them without a battle, "More time for Bushido"
  slows the game's timer, and "Overdrives always ready" lets you practise any overdrive at once. An optional download,
  `ffxvr-target-art-<version>.zip` (AI-generated art), turns the targets into textured medallions. "Tidus: swing for
  Swordplay" and Bushido's "slashes and punches" are there to try (off by default).
- **3D damage numbers** (F8 > Battle, experimental, off by default): the damage and heal numbers at each target in the
  world.
- **Controller diagrams**: every button and every chord on two pictures, `docs/img/controls.png` and
  `docs/img/controls-chords.png` (also in the zip).
- **Chocobo rides in first person**: your eyes on Tidus's head on the bird, the bird's running bob smoothed
  (F8 > View > First person > Riding), the bird drawn under you (or hidden, if its neck is in the way);
  third person and the diorama follow the bird too.
- **Small logs by default**: one `ffxvr_diag.log` per session, at most 4 MB; the diagnostics for bug
  hunting are off; `tools\set-logging.ps1 -Profile Verbose` turns on a fuller log for a bug report, and
  `-Profile Release` turns it off again.

Developer-only: the research logs for work in progress (spheres, weapons, battle commands) are under F8 > Developer >
Inspect, off by default; a player never needs them unless asked.

### Known issues

- Stick turning can still show some ghosting (head turning is clean).
- The heat haze over fire is off in the headset (it was wrong in one eye).
- The dream-sequence filter is hidden in the headset; barely tested.
- The sphere grab works in Djose's cloister only; the Destruction Sphere there, and every other cloister, use the A
  button as in the flat game. The prompts are answered automatically only when the game runs in English.
- Weapons are not in your hands yet: in a first-person battle your character's weapon is where the game draws it, and
  the VR hands are not shown in battle.
- Spells and overdrives with their own staging are seen from where you stand, not from the game's camera, so some
  effects are seen from the side or from behind. Holy is the only spell looked at so far with "Characters a spell draws
  by itself:" on "in 3D"; if another spell, overdrive or aeon attack looks wrong, pick "on the menu panel, as before"
  and send the log.
- Blitzball uses a stereo screen, not a full VR view. Pre-rendered movies are flat by default.
- The Re-Remastered HD texture pack caused performance problems on the developer's RTX 3080 Ti and needs a lot of
  disk space: not recommended without stronger hardware.
- Only Virtual Desktop (VDXR) on a Quest 3 has been tested; SteamVR is not supported.
