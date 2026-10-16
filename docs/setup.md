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
| SteamVR | 2.17.10 | only needed as an OpenXR fallback |
| Virtual Desktop Streamer | | provides the active 32-bit OpenXR runtime |

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
