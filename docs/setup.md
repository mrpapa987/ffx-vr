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

## Settings (ffxvr.ini)

`fahrenheit\mods\ffxvr\ffxvr.ini` holds every overlay setting; the overlay
writes it whenever something changes, and the mod re-reads it within a
second if it is edited on disk (so hand edits while the game runs are kept,
not overwritten). Launch-only keys: `headset_autostart` (start XR at the
first 3D frame), `start_view` (0 seat, 1 first person, 3 third person, 2
diorama), `overlay_at_start` (0 = no mod window until F8). `hands_dev=1`
shows the experimental VR-hands section.

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
Windows' compression are untested), and the mod does not use the depth yet. Treat it as a developer tool for now.

`tools\convert-fmv-depth.ps1` makes 8-bit relative depth for every frame of the game's FMVs, for a later
"relief screen" (FMVs shown with depth in VR). It runs **on your own PC, against your own copy**: each video is
streamed out of `data\FFX_Data.vbf` (read only, nothing extracted), decoded by ffmpeg, and run through
Video-Depth-Anything-**Small** (Apache-2.0) on the GPU. Output: one `<name>.fvd` per video (320x180 a frame, 240x180
for the 4:3 clips; format in `tools/fmvdepth/FORMAT.md`), `manifest.json` and `convert.log` in
`<game>\fahrenheit\mods\ffxvr\fmvdepth\`. The mod does not read it yet (no runtime code in this round).

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
| Time | estimated 1-2 hours for the US list (61 videos, 86,353 frames, 48 min of video) on a 3080 Ti class GPU; `-InputSize 392` is faster. Not yet measured. |

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

Options: `-Only A,B`, `-All` (all 95 files), `-Region us|jp|asia`, `-InputSize 518|392`, `-Force`, `-Frame N` (a PNG
of one frame with `-Preview`), `-Gray`, `-List`, `-SetupOnly`, `-OutDir`. Out of GPU memory at 518: it says so; run
again with `-InputSize 392`. Tests (no torch, no GPU, no game data): `python -m unittest discover -s tools/fmvdepth/tests`
(the end-to-end test needs ffmpeg on PATH).
