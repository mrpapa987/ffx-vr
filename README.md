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

See [docs/design.md](docs/design.md) for the reasoning behind each choice,
[docs/design-notes.md](docs/design-notes.md) for the running log of findings,
[docs/camera-found.md](docs/camera-found.md) for the constant-buffer camera,
and [docs/exe-analysis.md](docs/exe-analysis.md) for everything known about
FFX.exe itself (addresses, structures, cull sites, patches, leads).

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
  the first-person battle view now follows.
- **[OpenXR](https://www.khronos.org/openxr/)** (Khronos; loader and headers from the official
  NuGet packages) - the headset interface, with Virtual Desktop's VDXR as the runtime it was
  developed against and SteamVR as the fallback.
- **[Re-Remastered](https://www.nexusmods.com/finalfantasyxx2hdremaster/mods/244)**
  (HitokageWorshipper) - the texture pack installed alongside, written for ffgriever's External
  File Loader and served here through Fahrenheit's file loader instead.
- The PS2-era modding community's documentation of the game's formats (the VBF archive,
  Phyre assets), and the game's own debug leftovers, read with objdump.

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
