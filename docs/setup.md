# Working setup

The configuration verified working on 16 October 2026: game launches through
Fahrenheit, F7 overlay opens, 15 modules initialized, zero hook failures.

## The problem this documents

Square Enix patched FFX on Steam on **1 October 2026**. Fahrenheit hardcodes
addresses into the game executable, and calls the post-patch executables the
**"P4" binaries**. Support for them landed on `main` between 2 and 4 October.

Release **alpha11 (19 September) predates the patch and does not work.** It fails
with `MH_ERROR_UNSUPPORTED_FUNCTION` on `signal_enter_save`, then crashes with
`System.ArgumentOutOfRangeException` on the first rendered frame.

Mixing alpha11's native loaders with freshly built managed code **also fails** —
worse, in fact, dying during module load with no error log. Commits #306 and #342
changed the native loaders, so both halves must come from the same commit.

**Since 30 October 2026: use the official release v1.0.0-alpha12** (tag 7e85839,
20 October), the first release with P4 support. `tools/install-fahrenheit.ps1 -Zip
<zip>` installs it (INSTALL-ALPHA step 4); the mod restores `Fahrenheit.Sdk` and
`Fahrenheit` 1.0.0-alpha12 from nuget.org (`src/managed/nuget.config`). Until then
the mod was built against a source build of commit ecb6702, which carries the same
version string; `tools/paths.ps1` (`$FfxvrFhVersion`, `$FfxvrFhCommit`) is now the
one place that names the tested release. Why not main, and what to check before
alpha13: `docs/design-notes.md`, the Fahrenheit port section.

## Required

| Component | Version used | Notes |
| --- | --- | --- |
| Visual Studio **Build Tools 2026** | 18.11.12306.390 | winget id is `Microsoft.VisualStudio.BuildTools` — no year in the id |
| → Desktop development with C++ | | required for stage0/stage1 |
| → .NET desktop build tools | | required for a full build |
| → vcpkg component | | only for building Fahrenheit from source; then run `vcpkg integrate install` once |
| .NET SDK | 10.0.201 | 64-bit SDK is fine |
| .NET runtime, **x86** | 10.0.13 | separate install; the mod is a 32-bit managed plugin |
| Git | 2.43 | |
| PowerShell 7 | 7.6.7 | |
| Virtual Desktop Streamer | | **the** runtime: its VDXR provides the active 32-bit OpenXR runtime (the only one in use since SteamVR problems; SteamVR untested) |

VS Build Tools 2019 and 2022 may remain installed; the build script picks the
highest version with the C++ workload.

## Known build quirks (Fahrenheit source build only)

**`dbghelp.dll` is not copied by the build.** `Fahrenheit.Stage0.vcxproj` tries to
copy `dbghelp.dll`, `symsrv.dll` and (from alpha12, #369) `dbgcore.dll` from
`Windows Kits\10\Debuggers\x86\`, which only exists if Debugging Tools for Windows
is installed. The build reports `MSB3030` errors and exits 1, but `fhstage0.exe`
itself links fine; `build-fahrenheit-full.ps1` recognises exactly these errors.

`dbghelp.dll` is a **load-time import** of `fhstage0.exe` (commit #306 turned
stage0 into a stub debugger; `MiniDumpWriteDump` lives there), so stage0 will not
start without it. `symsrv.dll` is not imported and can be ignored.

Workaround used:

    copy C:\Windows\SysWOW64\dbghelp.dll <artifacts>\deploy\rel\bin\

The proper fix is installing Debugging Tools for Windows via the Windows SDK.

**Exit code 1 does not mean failure here.** Check whether the four native
artifacts exist and are dated today instead:
`fhstage0.exe`, `fhstage1.dll`, `minhook.x32.dll`, `nethost.dll`.

## Game install

Tested in the default Steam location, `C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY FFX&FFX-2 HD Remaster`.
The scripts find the game through Steam on any machine; `tools/paths.ps1` explains
the overrides (`FFXVR_GAME_DIR`, `tools/paths.local.ps1`).

Living under Program Files turned out **not** to be a problem: Fahrenheit writes
its logs, config and state there without elevation. Do not run the game or
Fahrenheit as administrator — Fahrenheit's docs say that breaks startup.

FFX.exe as tested: 10,687,744 bytes, PE timestamp 2026-09-10,
sha256 `0537b2a1047f3266e73495cd4e35f63f0777f4231d417699f979954686da686d`.
If a future Steam patch changes this, expect everything to break until upstream
re-maps the addresses again.

## Scripts

| Script | Purpose |
| --- | --- |
| `tools/check-prereqs.ps1` | Reports toolchain, OpenXR runtime registration, Steam paths |
| `tools/build-fahrenheit-full.ps1` | Optional source build: clones if needed, checks out the tested release (`-Commit` for another, `-Latest` for main), full msbuild build, reports native artifacts |
| `tools/install-fahrenheit.ps1` | `-Zip <zip>`: installs the official release zip; without it, copies the source build. Into `<game>\fahrenheit\bin` (cleared first; config/mods/saves untouched), adds dbghelp.dll if missing, prints the version, commit and signature it installed |
| `tools/package-release.ps1` | Builds (`build-mod.ps1 -NoDeploy`) and packs the player zip into `dist\` with its README and installer; see `docs/release.md` §1 |
| `tools/paths.ps1` | Shared by all scripts: repository root, game folder (env var / local file / Steam) |
| `tools/run-ffx.ps1` | Launches via Fahrenheit, captures console and game-side logs |
| `tools/convert-fmv-depth.ps1` | (Experimental) Makes FMV depth (`fmvdepth\*.fvd`) from your own game copy on your GPU; see "FMV depth" below |

The old managed-only `tools/build-fahrenheit.ps1` was deleted on 30 October 2026:
it reset `_ext\fahrenheit` to main and rebuilt it, which silently replaced the
source build that the rollback (`install-fahrenheit.ps1` without `-Zip`) installs.

## Deploying a build

`install-fahrenheit.ps1` does this. By hand: copy everything from the release
zip's `bin\` (or `_ext\fahrenheit\artifacts\deploy\rel\bin\`) over
`<game>\fahrenheit\bin\`. Clear the destination first so removed files do not
linger — `fhatelier.*` and `fheedit.*` were deleted upstream on 13 October and
will be stale against a current `fh.dll`.

Do not touch the sibling `config`, `mods`, `saves`, `state` and `logs` folders.
`mods\loadorder` lists mod ids to load, one per line; empty means load none.

## Frame rate: the FFX-60fps mod

The game's logic is a fixed 30 Hz step and cannot be uncapped. The VR setup
uses [peternguyenst/FFX-60fps](https://github.com/peternguyenst/FFX-60fps)
(Apache-2.0) alongside Fahrenheit: it keeps the logic at 30 Hz and draws N
frames per logic tick (`fps=60/90/120...`), interpolating the camera and the
characters, so our hooks see a camera upload - and a head pose - per drawn
frame. There are no prebuilt releases; `tools\build-fps60.ps1` clones, builds
(VS toolset v145, or the newest installed) and installs it:

    powershell -ExecutionPolicy Bypass -File .\tools\build-fps60.ps1 -Fps 60
    powershell -ExecutionPolicy Bypass -File .\tools\build-fps60.ps1 -Uninstall

Files it puts next to FFX.exe: `dbghelp.dll` (proxy loader; the game imports
dbghelp.dll, so it loads at start), `fps60_ffx.dll`, `fps60.ini`. Fahrenheit
launches the game through fhstage0.exe and does not use a dbghelp proxy, so the
two coexist. `fps60.log` (game folder, collected by run-ffx.ps1) says what the
mod found and enabled.

Bring-up order:
1. Flat screen, headset off, `fps=60`: the game must play normally and
   `fps60.log` must report its rows enabled; check `ffxvr_diag.log` perf lines
   read ~16.7 ms.
2. Headset on at 60; then `fps=90` with Virtual Desktop at 90 Hz (V-Sync off in
   the game's Config page is required above 60).
3. Above 60 the GPU budget matters: 90 fps x 2 eyes at 4K 8x MSAA will not
   hold on a 3080 Ti; lower the game's render resolution/MSAA first.
   `PERF-002=0` in fps60.ini stops the mod stepping the rate down on its own
   (it would read our headset waits as "cannot hold the rate").

### 60 fps in the headset (settled 19 Oct 2026)

The runtime (VDXR) paces frames to whole refresh slots, so the reachable
rates are refresh / N. At 90 Hz that is 90 / 45 / 30 and the mod's PERF-002
measures ~22 ms (45 fps), calls 60 "not held" and drops to 30. **Set Virtual
Desktop to 120 Hz**: 60 fps is then exactly two slots, and with the GPU span
under 16.7 ms PERF-002 holds it (`fps60.log`: "60 fps retry holds").

The GPU span (the `gpu` figure on the status line / `perf:` diag lines, measured
with timestamp queries from the game's first clear to Present) is 11-17 ms in
Besaid village at 3840x2160 with the scene target at 8x MSAA, drawn for both
eyes. The margin is thin. `MSAA=` in `GameSetting.ini` is ignored on the
Fahrenheit launch path (0 and 2 both left the target at 8 samples); AA has to
be changed through `FFX&X-2_LAUNCHER.exe` or, later, by the mod itself.

### GPU-bound scenes: the game's render size and MSAA (7 Nov 2026)

Once the GPU span (the `gpu` figure) passes about **13.5 ms**, the 120 Hz extra
frames that keep stick turns and head motion smooth collapse (60/s down to
6-25/s) and fresh frames reach the headset late. The 6-7 Nov logs had that in
a quarter of all windows (worst: Djose djyt0100/0300/0800, Mushroom Rock Road
kino0200/0300). The game still renders at **3840x2160 with 4x MSAA**
(`msaa=4`) for both eyes, while each eye image is about 2496x1404: the extra
pixels only buy supersampling. Nothing is changed by default; these are the
settings to try, in this order, one at a time:

1. **Game resolution 2560x1440** (close the game and its launcher first):

       powershell -ExecutionPolicy Bypass -File .\tools\game-settings.ps1 -Profile vr

   It edits `GameSetting.ini` in Documents (a backup is made the first time;
   `-Profile flat` restores it). If you miss the 4K sharpness, use
   `-Width 2880 -Height 1620` (or 3200x1800). Check in F8 that the line says
   `game renders 2560x1440`: if it still says 3840x2160, this launch path does
   not read `Resolution=` (as it ignores `MSAA=`), so set it in the game's
   launcher (`FFX&X-2_LAUNCHER.exe`) instead. The headset's eye image stays
   2496x1404, so no low-resolution warning should appear.
2. **Then `msaa=2`** (F8: MSAA for the scene targets, "2x"; or `msaa=2` in
   ffxvr.ini). It takes effect at the next start. Edges alias a little more.
3. Optional comparisons, only to measure: the texture pack off (same spot),
   and `cull_scene=0` ("keep all geometry drawn" off: cheaper, but scenery the
   authored camera does not see can disappear when you look around).

**What to read** (F8, the line under the frame time, updated every second
with 10 s windows): `GPU budget: span X ms (max Y) against the ~13.5 ms knee`
(`OVER` when above), `extra frames N/s` (60/s at 120 Hz with fps=60 is
ideal), `dropped (slots repeated) N/s` (should be near 0) and `fresh frames
late N%`. Compare the same spots before and after: the djyt0300 entrance,
djyt0100, kino0300 near the bridge. The `perf:` and `sr:` lines in
`ffxvr_diag.log` carry the same numbers. Expected with 2560x1440: a span of
about 6-9 ms in the field (perf-plan S1).

## Settings (ffxvr.ini)

`fahrenheit\mods\ffxvr\ffxvr.ini` holds every overlay setting; the overlay
writes it whenever something changes, and the mod re-reads it within a
second if it is edited on disk (so hand edits while the game runs are kept,
not overwritten). Launch-only keys: `headset_autostart` (start XR at the
first 3D frame), `start_view` (0 seat, 1 first person, 3 third person, 2
diorama), `overlay_at_start` (0 = no mod window until F8). `hands_dev=1`
shows the experimental VR-hands section.

## Controls (Quest controllers only)

The Quest controllers are the gamepad (`vrpad=1`), merged with an Xbox pad if one is connected (buttons OR, the stick
pushed further wins). Mapping: right A / B = A / B, left X / Y = X / Y, triggers = LT / RT, grips = LB / RB (below), left
menu = Start, left stick click = Back, right stick click = R3, sticks = sticks.
The triggers count as pressed from about 12 % of their travel (30 / 255, XInput's standard threshold) and as let go below
about 8 %, so a finger resting on a trigger presses nothing; a very light half-pull does not register.

| Where | Grips | Left stick |
|---|---|---|
| Field, first / third person | LB / RB at once (holdable: the main menu's character pages) | walks relative to where you look |
| Field, seat / diorama | a held grip grabs the world; a **quick squeeze** (released within 300 ms, the hand still) is LB / RB and the world does not move | seat: as the game's camera; diorama: relative to where you look |
| Battle, every view | a **quick squeeze** is LB / RB (left: open the switch list, next in it; right: one CTB row back); a held grip is the chord modifier (and the grab in the diorama) | raw, as a gamepad (menus, targets; left = Overdrive, right = Weapon / Armor / Escape) |
| Field menus, shops, choice boxes, the sphere grid | LB / RB at once (a quick squeeze if the seat / diorama stays in 3D behind them) | raw |
| Fahrenheit's save / load screen | page the list | moves the highlight; A loads / saves, B backs out, triggers jump to the ends |

Battle: Y = Defend, right trigger held = scroll the turn order forward, left trigger = previous in the switch list. Chords
(never reach the game): right grip + right stick click = the next view, left grip + right stick click = the next party
member's view (battle), left grip + left stick click = reset the view, both stick clicks = Virtual Desktop's overlay (the
game gets neither Back nor R3). Overdrive inputs: a pad and the controllers' buttons always work; F8 > Controllers has the
gesture options: Lulu's circles, Tidus's swing, and "Auron's Bushido:" off / slashes and punches / strike the targets (cut the
big plate in front of you with either hand: an arrow along its sliding chevrons, a letter or L / R any way). "Practice targets" (same place, headset on, no battle needed) lets you try the targets
and set the blade pitch, target distance, target height and target size first.

Keys (F8 > Controllers): `move_remap_walk_only` (1; 0 = the stick is rotated in every first-person view, the old
behaviour), `grip_lbrb` (1 = quick squeeze in battle and the seat / diorama; 0 = LB / RB at once everywhere, the old
behaviour; 2 = quick squeeze everywhere), `grip_tap_ms` (300, 150..500), `grip_tap_press_ms` (0: the LB / RB press lasts 2
game polls, one turn-order row; raise it, e.g. 100, if a squeeze is ever missed; at most 150, as a longer press trips the
game's key repeat), `vd_overlay_mask` (1: also delays Back
and R3 by 80 ms, so the chord is seen first), `imgui_vrpad` (1 = the Quest controllers in Fahrenheit's save screen; 0 =
off, also not installed at the next launch; 2 = in every ImGui window, Start and Back too, so both grips + menu is
Fahrenheit's soft reset, a game over in battle), `imgui_vrpad_strict` (0; 1 = do not install when the game's XInput
import resolves into ImGui's XInput DLL). The save screen's line in `ffxvr_diag.log` starts
with `imgui pad:`; quick squeezes log `grips:` lines.

## Texture packs (External File Loader style) under Fahrenheit

Loose-file texture packs for this game (e.g. HitokageWorshipper's
"Re-Remastered", Nexus mod 244) are written for ffgriever's External File
Loader, which serves files from `data\mods\ffx_data\...` mirroring the VBF.
**Do not install that loader next to Fahrenheit**: Fahrenheit's own
`FhFileLoaderModule` hooks the same game functions and would fight it.
Fahrenheit supports file-only mods: a folder under `fahrenheit\mods\<id>\`
with `<id>.manifest.json` and the files under `efl\x\...`, listed in
`fahrenheit\mods\loadorder`. `efl\x` is the VBF root: `ffx_ps2\...` and
`ffx_data\gamedata\...` side by side, the same tree EFL serves from
`data\mods\`. The file loader indexes each file by its path relative to
`efl\x` and matches the paths the game asks for (`/ffx_ps2/...`,
`/FFX_Data/GameData/...`; `FFX_Data` is `ffx_data`, the lookup ignores case).
The example in Fahrenheit's `fileloader.cs` (`efl\x\FFX_Data\ffx_ps2\...`)
is misleading: that extra level never matches, and a pack installed that way
is silently ignored. The first install here did exactly that.

`tools\install-texture-pack.ps1 -Archive <downloaded .7z> [-Compress]` does
exactly that: extracts with 7-Zip, finds the VBF root in the archive, moves
it into the mod folder, adds the mod to the load order before ffxvr, and can
turn on NTFS compression (about half the size). `-Uninstall` removes it. The
Fahrenheit file-loader log lists every replaced file at launch, which is the
check that the files are being served.

## FMV depth (the converter, stage S1; 3 Nov 2026) - experimental

**Experimental:** the converter has not yet had its first run on a real PC (the GPU setup, the model download and
Windows' compression are untested), and the mod's relief screen (below) has not been tried in a headset yet. Treat it as a
developer tool for now.

`tools\convert-fmv-depth.ps1` makes 8-bit relative depth for every frame of the game's FMVs, for a later
"relief screen" (FMVs shown with depth in VR). It runs **on your own PC, against your own copy**: each video is
streamed out of `data\FFX_Data.vbf` (read only, nothing extracted), decoded by ffmpeg, and run through
Video-Depth-Anything-**Small** (Apache-2.0) on the GPU. Output: one `<name>.fvd` per video (320x180 a frame, 240x180
for the 4:3 clips; format in `tools/fmvdepth/FORMAT.md`), `manifest.json` and `convert.log` in
`<game>\fahrenheit\mods\ffxvr\fmvdepth\`. With F8 > View > `FMVs in 3D (needs converted depth)` (ini `fmv_depth=1`,
off by default; `fmv_depth_amount`, `fmv_depth_range`, `fmv_depth_pop`, `fmv_depth_grid`, `fmv_depth_soften`, `fmv_depth_smooth`, `fmv_depth_flat_subs`) a converted FMV plays on the virtual screen with real depth in
the headset (stage S3, design-notes "FMV relief screen"); any other FMV stays on the flat screen, and F8 says why.

Recommended relief settings (F8 > View, under `FMVs in 3D`; these are the defaults since 6 Nov 2026, from the owner's own
tuning on converter v2 files): `amount` 1.00, `depth range` 10 m (ini `fmv_depth_range=10`), `pop-out` 0.5 m
(`fmv_depth_pop=0.5`), grid `640 grid, smooth depth (recommended)` (`fmv_depth_grid=1`), `edge softening` 1.0 depth texels
(`fmv_depth_soften=1.0`); `smoothing` 0.36 worked well for him (the default stays 0.6). A larger screen distance (he used
5.2 m) gives the depth more room: the comfort limits allow more pop-out the further the screen is. The sliders go to 20 m
and 2 m, but at a near screen the limits cap them (+1 deg in front, -1.5 deg behind) and the line next to the sliders says
"limited". An ini saved by an older build keeps its own range and pop-out; set them by hand to try these. The
`320 grid (round 7)` choice shows sharp v2 edges as a staircase; F8 > Debug's `640 grid, guided (round 10)` is
experimental and streaks at silhouettes.

**The depth is derived from Square Enix's videos: it is made locally, it stays private, and it is never shipped.**
The folder holds `PRIVATE-do-not-share.txt`; `package-release.ps1` fails on any `.fvd` or `fmvdepth\` path;
`install-mod.ps1` never deletes it (not even `-Uninstall -Purge`, which keeps it and says so); `build-mod.ps1` and
`run-ffx.ps1` leave it alone; `.gitignore` covers `*.fvd`, the previews and every folder named `fmvdepth` (except
`tools/fmvdepth/`, the converter's source); the converter refuses to write inside a git checkout or the game's `data\`
folder (junctions resolved). The converter is not in the player zip yet.

Requirements:

| | |
| --- | --- |
| NVIDIA GPU | 8 GB VRAM or more, a current driver (528+ for CUDA 12.4). Designed on an RTX 3080 Ti (12 GB). |
| Python | 3.10 - 3.13 (`winget install Python.Python.3.12`); the script makes its own venv |
| ffmpeg + ffprobe | any recent build on PATH (`winget install Gyan.FFmpeg`), or `-Ffmpeg <path>` |
| Disk | ~1.2-1.5 GB of `.fvd` for the US list (estimate from a proxy; measure on the first run), plus ~3-5 GB for `%LOCALAPPDATA%\ffxvr\fmvdepth-venv` (PyTorch with CUDA) and ~110 MB of weights |
| Time | US list (61 videos, 86,353 frames, 48 min of video) on a 3080 Ti + i9-12900KF: converter 0.1 took 22 min. 0.2 (per-shot windows, sharper edges on the CPU while the GPU runs the next window) is estimated at ~35-45 min with the defaults (up to ~1 h if FFX's depth turns out very edge-dense), ~1.1-1.2 h with `-Flip`, ~1.3-1.6 h with `-InputSize 714`, ~2.3-2.9 h with both (an overnight job; estimates until measured, assuming ~1,000 shots in the list). Ctrl+C stops within a second or so; finished videos are kept. `-InputSize 392` is faster. |
| Disk | while converting, a few GB free in the output folder for one very long shot (its depth is held in a temporary `fvdshot-*` file past 2,400 frames, deleted afterwards; a leftover from a power cut is deleted on the next run) |

First run sets up, under `%LOCALAPPDATA%\ffxvr\`: `fmvdepth-venv\` (pinned torch 2.6.0 + torchvision 0.21.0 from the
PyTorch cu124 index, numpy, OpenCV headless, einops, easydict, tqdm), `fmvdepth-vda\` (the model code at commit
4f5ae23, from GitHub; its files are checked against their SHA-256) and `fmvdepth-models\` (the Small weights; their
SHA-256 is recorded on the first download and checked after - not pinned in the script yet: until it is, every run
prints a block with the hash and what Hugging Face said about the file; please send it to the developers). Delete those folders to
remove them.

    # S0, the quality check: SIN_us and BTS_us, then video | depth previews in fmvdepth\preview\ (private)
    powershell -ExecutionPolicy Bypass -File .\tools\convert-fmv-depth.ps1 -Preview
    # everything the US game plays (resumes: finished videos are skipped)
    powershell -ExecutionPolicy Bypass -File .\tools\convert-fmv-depth.ps1

Options: `-Only A,B`, `-All` (all 95 files), `-Region us|jp|asia`, `-InputSize 518|714|392` (default 518, what the
model was trained at; 714 for finer detail at about twice the model time and VRAM), `-Flip` (each window also run
mirrored and averaged: less noise, no left/right bias, twice the model time), `-Force`, `-Frame N` (a PNG of one frame
with `-Preview`), `-Gray`, `-List`, `-SetupOnly`, `-OutDir`. Out of GPU memory: it says so; run again with
`-InputSize 518` (from 714) or `392`.

**Converter 0.2 (5 Nov 2026)** makes better depth from the same model: the model's windows restart at every cut, a
flash is no longer a cut, each shot gets one fixed depth range (nothing sinks back when something enters, no flat
plateau on the nearest 2 %; dissolves and fades through black are handled), a centred temporal filter removes most of
the flicker without lag, and silhouettes are sharpened at the model's resolution before the shrink (design-notes,
"Converter 0.2"). The file format stays version 1; 0.2 files carry a header flag (HF_SEGMENT_RANGE) that tells the
mod not to re-normalise them per shot: play them with a mod build from fmv-relief 9812b55 or later (an older build
shows short shots and dissolves with the wrong depth range). Each video records the settings it was made with: the
first run of 0.2 converts every video again by itself (the log says how many and why), and any later change of
`-InputSize` or `-Flip` does the same, while videos already made with the same settings are skipped. Overnight, the
most thorough settings (714 has not been compared with 518 on the real videos yet: the A/B below is that check):

    powershell -ExecutionPolicy Bypass -File .\tools\convert-fmv-depth.ps1 -InputSize 714 -Flip

To compare first without touching the main folder: `-Only SIN_us,BTS_us -Preview -OutDir <another folder>` with and
without those options. Tests (no torch, no GPU, no game data): `python -m unittest discover -s tools/fmvdepth/tests`
(the end-to-end test needs ffmpeg on PATH).

Depth quality figures (read-only, prints numbers; noise, pumping, the model's window stride, clipping, per-shot raw range):
`& "$env:LOCALAPPDATA\ffxvr\fmvdepth-venv\Scripts\python.exe" tools\fmvdepth\fvd_stats.py <game>\fahrenheit\mods\ffxvr\fmvdepth\opn_us.fvd
<...>\sin_us.fvd <...>\end_us.fvd --shots` (the converter's venv: XPRESS needs Windows' cabinet.dll). Send the output.
