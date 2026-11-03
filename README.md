# FFX VR

**Play FINAL FANTASY X HD Remaster (PC, Steam) in VR** on a Quest 3 through Virtual Desktop. An
unofficial fan mod, built on the [Fahrenheit](https://github.com/fahrenheit-crew/fahrenheit) mod
framework.

> **Status: alpha (0.1.0-alpha.1).** Playable on the developer's setup (Quest 3, Virtual Desktop,
> RTX 3080 Ti). Tested on that one setup only: expect rough edges, and please report what you find.

<!-- SCREENSHOTS / CLIP (to do before the release): a short clip, or 3-4 screenshots (first person in
     Besaid, a tabletop battle, the UI panel, the blitzball screen), e.g. in docs/media/. -->

## What it does

Each part of the game gets the viewpoint that suits it, instead of one VR camera forced onto all of them:

| In the game | In the headset |
| --- | --- |
| Field | First person (your body hidden), third person behind the player, or a tabletop **diorama** you can grab and scale; free head look |
| Battles | The same three views; first person follows whoever is on the field (and the summoner during a summon) |
| Menus, dialogue, the battle command window | A UI panel in front of you that comes closer when the scene does |
| 2D screens, pre-rendered movies, the save screen | A large flat virtual screen |
| Blitzball | A large stereo 3D screen showing the game's own camera, with automatic depth |

The Quest controllers act as the gamepad (with rumble), the stick turns smoothly at the headset's
refresh rate, and the game's own battle cursors are drawn in 3D over their targets.

## Requirements

- **FINAL FANTASY X/X-2 HD Remaster** on Steam, the current version (after the 1 October 2026 patch).
- **Meta Quest 3** with **[Virtual Desktop](https://www.vrdesktop.net/)** and its **VDXR** OpenXR runtime
  (chosen in the Virtual Desktop Streamer on the PC). That is the only tested headset and runtime;
  SteamVR is not supported in this alpha. Other Quest headsets may work but are untested.
- **Virtual Desktop settings:** refresh rate **120 Hz** (72 Hz also works; avoid 90 Hz),
  **SSW (Synchronous Spacewarp): Disabled**.
- **In the game's settings: V-Sync off** (the headset paces the frames).
- **[Fahrenheit v1.0.0-alpha12](https://github.com/fahrenheit-crew/fahrenheit/releases/tag/v1.0.0-alpha12)**,
  the official release zip (the mod loader; older releases do not work with the current game).
- **.NET 10 runtime, x86 (32-bit)**: `winget install Microsoft.DotNet.Runtime.10 --architecture x86`.
- A GPU that renders the game twice per frame comfortably (tested on an RTX 3080 Ti).
- Optional, recommended: **[FFX-60fps](https://github.com/peternguyenst/FFX-60fps)** at 60 fps
  (smoother motion; without it the game runs at 30).
- Optional, later: the **[Re-Remastered](https://www.nexusmods.com/finalfantasyxx2hdremaster/mods/244)**
  HD texture pack.

## Install

Download the latest `ffx-vr-<version>.zip` from the
[Releases page](https://github.com/mrpapa987/ffx-vr/releases), extract it **outside** the game folder,
and follow the `README.md` inside. In short:

1. Install Fahrenheit v1.0.0-alpha12 into `<game folder>\fahrenheit\` (its `bin` folder becomes
   `fahrenheit\bin`).
2. Optionally install FFX-60fps per its own README (`fps=60`).
3. From the extracted folder, in PowerShell (not as administrator):
   `powershell -ExecutionPolicy Bypass -File .\tools\install-mod.ps1`
   It checks Fahrenheit, .NET x86, the 32-bit OpenXR runtime and FFX-60fps, copies the mod into
   `<game folder>\fahrenheit\mods\ffxvr\` and adds `ffxvr` to Fahrenheit's load order.
4. Start the game through Fahrenheit: `powershell -ExecutionPolicy Bypass -File .\tools\run-ffx.ps1`.

Uninstall with `install-mod.ps1 -Uninstall` (add `-Purge` to delete the settings and logs too).
Building from source (testers and developers): see [INSTALL-ALPHA.md](INSTALL-ALPHA.md).

The DLLs are not code-signed; Windows SmartScreen or an antivirus may ask about them.

## First run

- **Connect Virtual Desktop to the PC before starting the game.** Without a headset (or with Virtual
  Desktop not running) the game plays normally on the monitor.
- Press **F8** in the game for the FFX VR overlay. Tick **Headset**, and **start automatically** to go
  straight into the headset from the next launch.
- **Recentre**: hold the Meta button, or press **Home**.
- **Change view** (first person, third person, diorama): **right grip + right stick click**, or **End**.
- **Reset the view** (third person, diorama): **left grip + left stick click**.
- **In battle, look through the next party member**: **left grip + right stick click**, or **Insert**.
- **Turn**: right stick, smooth (the speed is in the overlay); there is no snap turn.
- Play seated, and quit through the game's menu rather than by closing the window.

## Known issues

- **Stick turning can still ghost** (a faint double image while turning with the right stick). Turning
  your head is clean. "Smooth stick turns at the headset rate" in the overlay (on by default) helps a
  lot; a full fix is planned.
- **The heat haze over fires and torches is off in the headset**: it showed a wrong patch of the scene
  in one eye. The fire itself is drawn.
- **The dream-sequence filter** (the sepia, grainy look of some story scenes) is hidden in the headset
  by default; this is barely tested.
- Some scene changes can show a frozen picture for a moment while the game loads (the headset keeps the
  last frame, head-tracked, rather than going black).
- If the monitor is off when the game starts, the game may render at a low resolution; the overlay says
  so. Keep the monitor on, or see "When something goes wrong" in the zip's README.
- Blitzball uses a stereo screen rather than a full VR view; chocobo riding has had no VR work yet.
- Only Virtual Desktop (VDXR) on a Quest 3 has been tested.

## VR hands (experimental) and your own hands

F8 > **VR hands (experimental)** > tick **VR hands** draws a pair of hands at your controllers in first person (and,
if you tick them, in the diorama, third person and the seat view). The fingers follow the trigger, the grip and the
touch sensors. It is off by default while it is being tested.

**hand model** lists every pack in the mod folder's `hands\` folder: the two that ship with the mod (our own generic
hand, MIT: a dark glove and a bare hand) and any you make yourself. **rescan** reads the folder again after you add one.

**Your own hands, from a model you own.** `tools\import-hands.ps1` turns a character model you have (a Wavefront `.obj`
with its `.mtl` and textures beside it: a full body in a T or A pose, Y up) into a hand pack: it finds both hands, cuts
them at the wrist or cuff, rigs the fingers and carries the textures over. It needs Python 3 and numpy (it tells you how
to install numpy if it is missing).

```
powershell -ExecutionPolicy Bypass -File .\tools\import-hands.ps1 "D:\models\My Character.obj" -Name mine
```

It writes `<game>\fahrenheit\mods\ffxvr\hands\mine\`. In game: F8 > VR hands > hand model > **rescan**, pick it.
If a hand looks wrong: `-CutRight 0.095` / `-CutLeft` move the wrist cut, `-FlipRight` / `-FlipLeft` swap a palm and
back, `-Only right` mirrors one hand, `-Tip "x,y,z"` picks a hand by hand, `-DebugImages` shows what it found,
`-Preview preview.png` renders the poses into the pack folder (the script's header lists everything). Both hands are
made 0.19 m from wrist to middle fingertip (`-HandLength`; `-KeepProportions` keeps the model's own wrists). `hand.json` in the pack folder holds the per-pack fit (`wrist_offset_m`,
`wrist_rotation_deg`, `scale`, `tint`, `rest_curl`; see `src/managed/hand_pack.cs`).

**A pack made from a model stays on your PC: do not share it.** Most character models, a game's above all, are someone
else's copyright. The importer marks every pack it makes as private (`"private": true` and a
`PRIVATE-do-not-share.txt`), refuses to write into a git checkout, and FFX VR's tools never copy, package or upload such
a pack (`install-mod.ps1 -Uninstall` keeps it; `-Purge` names it before deleting it). FFX VR itself ships no game data.

## Reporting bugs

Please report problems **here, in this repository's [Issues](https://github.com/mrpapa987/ffx-vr/issues)**,
never to the Fahrenheit or FFX-60fps projects: they are not responsible for this mod.

1. Start the game with `tools\run-ffx.ps1`; when it closes, it collects the logs into the `logs\`
   folder next to `tools\`.
2. Attach the newest `logs\run-*.txt` and the `logs\run-*-gamelogs` folder (zipped), and say what you
   did and what you saw. The FFX VR version is shown at the bottom of the F8 overlay.

By default the mod keeps only a small log: `ffxvr_diag.log` in the mod folder (startup, versions,
headset and view events, errors; started fresh each session, at most 4 MB; the session before is
`ffxvr_diag.prev.log`) and `ffxvr_native.log` (a small status snapshot). If we ask for more detail,
turn on the verbose log with the game closed (it only logs more; nothing else changes), play until the
problem shows, and turn it off again afterwards (its log grows up to 64 MB a session):

    powershell -ExecutionPolicy Bypass -File .\tools\set-logging.ps1 -Profile Verbose
    powershell -ExecutionPolicy Bypass -File .\tools\set-logging.ps1 -Profile Release

(The same switch is "verbose log" at the top of the F8 overlay's Debug section.)

## Standing on

This mod only works because of other people's projects:

- **[Fahrenheit](https://github.com/fahrenheit-crew/fahrenheit)** (fahrenheit-crew) - the mod
  framework the whole thing runs in: it loads the managed module into the game, provides the
  structures for the battle state, party, windows and actors, the call hooks, the file loader
  that serves the texture pack, and the Dear ImGui overlay (through Hexa.NET.ImGui) that our
  settings panel is drawn with.
- **[FFX-60fps](https://github.com/peternguyenst/FFX-60fps)** (peternguyenst) - runs the game at
  a VR-friendly frame rate by interpolating camera and character motion between the game's
  30 Hz logic ticks; its source was also the first map of the exe's camera code.
- **[Farplane](https://github.com/topher-au/Farplane)** (topher-au) - a memory editor whose
  pattern-scanned offsets confirmed ours and led to the game's own battle field roster, which
  the first-person battle view now follows (a reference only; no code taken).
- **[OpenXR](https://www.khronos.org/openxr/)** (Khronos; loader and headers from the official
  NuGet packages) - the headset interface, through Virtual Desktop's VDXR runtime.
- **[Re-Remastered](https://www.nexusmods.com/finalfantasyxx2hdremaster/mods/244)**
  (HitokageWorshipper) - the texture pack installed alongside, written for ffgriever's External
  File Loader and served here through Fahrenheit's file loader instead.
- The PS2-era modding community's documentation of the game's formats (the VBF archive,
  Phyre assets), and the game's own debug leftovers, read with objdump.

## For developers

    src/managed/   C# Fahrenheit module: camera hook, mode detection, settings, the F8 overlay
    src/native/    C++ DLL: D3D11 hooks, OpenXR session, per-eye render, UI panels
    tools/         Build, install and diagnostic scripts
    assets/hands/  The generic VR hand packs (ours, MIT; regenerate with python tools/hands/make-generic-hand.py).
                   Only generic/ and generic-glove/ belong here: .gitignore keeps any other pack out of git
    tools/hands/   The hand-pack tools: the generator, the importer (import-hands.py, objio.py; tools/import-hands.ps1),
                   handlib.py, preview-pack.py
    packaging/     The release zip's installer and README
    docs/          Design notes

[docs/design.md](docs/design.md) has the reasoning behind each view,
[docs/design-notes.md](docs/design-notes.md) the running log of findings,
[docs/camera-found.md](docs/camera-found.md) the constant-buffer camera,
[docs/exe-analysis.md](docs/exe-analysis.md) everything known about FFX.exe itself, and
[docs/release.md](docs/release.md) how a release is made. Changes per version: [CHANGELOG.md](CHANGELOG.md).

## Licence

MIT - see [LICENSE](LICENSE). Third-party software redistributed with release builds (the
Khronos OpenXR loader, Apache-2.0) and the projects this mod depends on are listed in
[THIRD-PARTY-NOTICES](THIRD-PARTY-NOTICES). Fahrenheit (LGPL-3.0-or-later) is used as a
library and is not part of this repository.

## Disclaimer

An unofficial fan mod. Not affiliated with or endorsed by Square Enix, Meta, Virtual Desktop, or the
Fahrenheit and FFX-60fps projects. Requires a legitimate copy of FINAL FANTASY X/X-2 HD Remaster; no
game files or assets are included. FINAL FANTASY is a registered trademark of Square Enix Holdings
Co., Ltd.
