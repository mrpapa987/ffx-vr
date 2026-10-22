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

**Build everything from `main`. Do not use the alpha11 release.**

## Required

| Component | Version used | Notes |
| --- | --- | --- |
| Visual Studio **Build Tools 2026** | 18.11.12306.390 | winget id is `Microsoft.VisualStudio.BuildTools` — no year in the id |
| → Desktop development with C++ | | required for stage0/stage1 |
| → .NET desktop build tools | | required for a full build |
| → vcpkg component | | then run `vcpkg integrate install` once |
| .NET SDK | 10.0.201 | 64-bit SDK is fine |
| .NET runtime, **x86** | 10.0.13 | separate install; the mod is a 32-bit managed plugin |
| Git | 2.43 | |
| PowerShell 7 | 7.6.7 | |
| Virtual Desktop Streamer | | **the** runtime: its VDXR provides the active 32-bit OpenXR runtime (the only one in use since SteamVR problems; SteamVR untested) |

VS Build Tools 2019 and 2022 may remain installed; the build script picks the
highest version with the C++ workload.

## Known build quirks

**`dbghelp.dll` is not copied by the build.** `Fahrenheit.Stage0.vcxproj` tries to
copy `dbghelp.dll` and `symsrv.dll` from `Windows Kits\10\Debuggers\x86\`, which
only exists if Debugging Tools for Windows is installed. The build reports two
`MSB3030` errors and exits 1, but `fhstage0.exe` itself links fine.

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

`C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY FFX&FFX-2 HD Remaster`

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
| `tools/build-fahrenheit-full.ps1` | Updates `main`, full msbuild build, reports native artifacts |
| `tools/run-ffx.ps1` | Launches via Fahrenheit, captures console and game-side logs |

`tools/build-fahrenheit.ps1` is the managed-only build. Kept for reference, but it
is not sufficient on its own.

## Deploying a build

Copy everything from `_ext\fahrenheit\artifacts\deploy\rel\bin\` over
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

    powershell -ExecutionPolicy Bypass -File C:\projects\FFX-VR\tools\build-fps60.ps1 -Fps 60
    powershell -ExecutionPolicy Bypass -File C:\projects\FFX-VR\tools\build-fps60.ps1 -Uninstall

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
