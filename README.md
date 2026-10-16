# FFX VR

A VR mod for the PC release of **Final Fantasy X HD Remaster** (Steam), built on the
[Fahrenheit](https://github.com/fahrenheit-crew/fahrenheit) mod framework.

**Status:** pre-alpha. Nothing is implemented yet — this is the project skeleton.

## What it does

Each part of the game gets the viewpoint that suits it, rather than forcing one VR camera
on all of them:

| Game state | Viewpoint |
| --- | --- |
| Overworld | Fixed at the authored camera position; free head look, clamped lean |
| Battles | Free: tabletop diorama or life-size, repositionable |
| In-engine cutscenes | Authored camera position, shot by shot |
| Pre-rendered FMVs | Movie screen in its own environment |
| Blitzball | Game's own camera on a panel first; tabletop sphere later |

See [docs/design.md](docs/design.md) for the reasoning behind each choice.

## Layout

    src/managed/   C# Fahrenheit module: camera hook, mode detection, settings
    src/native/    C++ DLL: D3D11 hooks, OpenXR session, per-eye render, UI panels
    tools/         Build and diagnostic scripts
    docs/          Design notes
    logs/          Build and runtime logs (gitignored)

## Requirements

Run `tools\check-prereqs.ps1` to verify these. In short:

- Windows, with the Steam copy of FFX/X-2 HD Remaster installed **outside** `C:\Program Files`
- SteamVR 2.17 or newer, set as the default OpenXR runtime — **including its 32-bit entry**,
  because `FFX.exe` is a 32-bit program
- .NET 10 SDK, plus the x86 .NET 10 runtime
- Visual Studio with the C++ workload, and vcpkg
- PowerShell 7 and Git

## Non-goals

- No game files are redistributed; the mod ships only its own code.
- No asset modding — the VBF archives are left alone.

## License

Intended to be open source. Specific license still to be decided; note that Fahrenheit
itself is LGPL-3.0, which constrains the choice for the managed half.
