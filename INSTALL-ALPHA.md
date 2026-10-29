# FFX VR — alpha install guide

This is a guide for testers. FFX VR is pre-alpha: there's no download yet. You build it from
source with the scripts in `tools\`, which takes about 30–60 minutes the first time, mostly
installing tools and waiting for builds.

It has been developed and tested on one setup: Windows 11, an RTX 3080 Ti, and a **Quest 3
over Virtual Desktop (VDXR)**. Anything else is untested.

> An unofficial fan mod, not affiliated with Square Enix. You need your own copy of
> FINAL FANTASY X/X-2 HD Remaster on Steam. No game files are included.

---

## 1. What you need

### The game and the headset

| | Notes |
|---|---|
| **FINAL FANTASY X/X-2 HD Remaster** (Steam) | The current version, after the 1 October 2026 patch. Run it once normally before starting. |
| **A Quest headset + [Virtual Desktop](https://www.vrdesktop.net/)** | Virtual Desktop on the headset, and the **Virtual Desktop Streamer** on the PC. |
| **VDXR as the active OpenXR runtime** | In the Streamer app on the PC, choose **VDXR** as the OpenXR runtime. FFX is a **32-bit** program, so the 32-bit registration is the one that matters; the prerequisite check (step 3) shows it. SteamVR is not supported in this alpha. |

### Build tools (all free)

| Tool | Why | Install (winget IDs as of Oct 2026; or use the official installers) |
|---|---|---|
| Git | Downloading the source | `winget install Git.Git` |
| PowerShell 7 (optional) | Not needed: every command below uses Windows PowerShell (`powershell`), which comes with Windows | `winget install Microsoft.PowerShell` |
| Visual Studio **Build Tools 2026** | Building the C++ part and Fahrenheit | `winget install Microsoft.VisualStudio.BuildTools`, then in the Visual Studio Installer tick **Desktop development with C++**, **.NET desktop build tools** and the **vcpkg** component |
| vcpkg integration | Fahrenheit's native build | After the Build Tools install, run once in a *Developer* prompt: `vcpkg integrate install` |
| .NET 10 SDK | Building the mod | `winget install Microsoft.DotNet.SDK.10` |
| .NET 10 **x86** runtime | Running the mod (it's 32-bit, like the game) | `winget install Microsoft.DotNet.Runtime.10 --architecture x86` |
| 7-Zip | Only if you install a texture pack | `winget install 7zip.7zip` |

---

## 2. Get the source

Clone the repository anywhere you like. Nothing in the scripts assumes a particular folder:

```
git clone https://github.com/mrpapa987/ffx-vr.git
cd ffx-vr
```

(While the repository is private, you need to have been invited as a collaborator.)

**Run every command below from this folder**, in PowerShell, **not as administrator**.
Fahrenheit refuses to start when elevated.

---

## 3. Check your setup

```
powershell -ExecutionPolicy Bypass -File .\tools\check-prereqs.ps1
```

This writes `logs\prereqs.txt` and prints the same report. Look for:

- **.NET**: a 10.x SDK and a 10.x **x86** runtime.
- **Visual Studio C++ tools**: found.
- **OpenXR runtime → 32-bit**: a path ending in `virtualdesktop-openxr-32.dll`. If it says
  *none registered*, set VDXR as the runtime in the Virtual Desktop Streamer.
- **Paths the tools will use → game**: your FFX install folder.

### If the game folder isn't found

The scripts find the game through Steam, including extra Steam library folders. If yours is
somewhere unusual, tell them where it is in either of two ways:

- for this PowerShell window only:
  `$env:FFXVR_GAME_DIR = 'D:\Games\FINAL FANTASY FFX&FFX-2 HD Remaster'`
- permanently: create `tools\paths.local.ps1` (it's not in git) containing one line:
  `$FfxvrGameOverride = 'D:\Games\FINAL FANTASY FFX&FFX-2 HD Remaster'`

The folder you give must contain `FFX.exe`.

---

## 4. Build and install Fahrenheit (the mod loader)

FFX VR runs inside [Fahrenheit](https://github.com/fahrenheit-crew/fahrenheit). Fahrenheit's
latest *release* (alpha11) predates the October game patch and **does not work** with the
current game, so it's built from source at the commit FFX VR was tested with:

```
powershell -ExecutionPolicy Bypass -File .\tools\build-fahrenheit-full.ps1
powershell -ExecutionPolicy Bypass -File .\tools\install-fahrenheit.ps1
```

- The first script clones Fahrenheit into `_ext\fahrenheit` and builds it. It ends with
  **RESULT: OK** or **RESULT: FAILED** (exit code 1), below a list of the four loader files
  (`fhstage0.exe`, `fhstage1.dll`, `minhook.x32.dll`, `nethost.dll`). MSBuild itself may report
  exit code 1 because it can't copy two debugging DLLs (`dbghelp.dll`, `symsrv.dll`) when the
  Debugging Tools for Windows aren't installed; the script recognises exactly that and says so.
  `-Latest` builds Fahrenheit's newest code instead, which is untested with this mod.
- The second script copies the result into `<game>\fahrenheit\`. It never touches your
  Fahrenheit config, saves or mods.

---

## 5. Install the frame-rate mod (recommended)

FFX runs at 30 fps internally. [FFX-60fps](https://github.com/peternguyenst/FFX-60fps) draws
60 by smoothing between frames, which matters a lot in VR:

```
powershell -ExecutionPolicy Bypass -File .\tools\build-fps60.ps1 -Fps 60
```

`-Uninstall` removes it.

---

## 6. Build and install FFX VR

```
powershell -ExecutionPolicy Bypass -File .\tools\build-mod.ps1
```

This downloads the OpenXR loader from NuGet, builds both halves of the mod, copies them to
`<game>\fahrenheit\mods\ffxvr\` and adds `ffxvr` to Fahrenheit's load order. Run it again
after every `git pull`. Your settings (`ffxvr.ini`) are kept. To reset them, delete `ffxvr.ini` with the game
closed (deleted while the game runs, it is written again at your next settings change). You can edit
`ffxvr.ini` while the game runs: it is read back within a second, and a bad value is ignored and listed in core.log.

---

## 7. Optional extras

| What | Command |
|---|---|
| **VR render resolution**: 2560×1440 instead of 4K; much lighter on the GPU. (Anti-aliasing is handled by the mod: 4× by default, in the overlay's View section) | `.\tools\game-settings.ps1 -Profile vr` (`-Profile flat` restores yours) |
| **Re-Remastered texture pack**: download it yourself from [Nexus](https://www.nexusmods.com/finalfantasyxx2hdremaster/mods/244). Do **not** install its External File Loader; Fahrenheit serves the files instead | `.\tools\install-texture-pack.ps1 -Archive "<path to the .7z>"` (`-Compress` halves the disk use, `-Uninstall` removes it) |
| **Your existing saves**: Fahrenheit keeps its own save slots | `.\tools\import-steam-saves.ps1` copies your normal saves in (it never replaces a VR save unless you pass `-Force`); `.\tools\export-save-to-steam.ps1` copies one back |

Prefix each with `powershell -ExecutionPolicy Bypass -File`.

---

## 8. Play

1. On the PC, start the **Virtual Desktop Streamer**. Put on the headset and connect to the
   PC in Virtual Desktop.
2. Launch the game **through Fahrenheit**, not from Steam:
   ```
   powershell -ExecutionPolicy Bypass -File .\tools\run-ffx.ps1
   ```
   This also collects the logs into `logs\` when the game closes, which is what to send with a
   bug report.
3. In the game, press **F8** for the FFX VR overlay. Tick **Headset**, and **start
   automatically** if you want it on from the next launch.

### Virtual Desktop settings

- **Refresh rate: 120 Hz** (72 Hz also works). Avoid 90 Hz: 60 fps doesn't divide into it, and
  the game falls back to 30.
- **SSW (Synchronous Spacewarp): Disabled**, not "Always" (Auto is not tested with this build).
  The mod fills the refreshes between game frames itself, turned with the stick, so stick turns
  should be as smooth as head turns ("smooth stick turns at the headset rate" in the overlay, on by
  default; the `sr:` lines in `ffxvr_diag.log` show what it does).
- Set the game's **V-Sync off** (the headset paces the frames).
- **Quit through the game's menu**, not by closing the window.
- Play **seated**.

### Controls

The Quest controllers act as the gamepad ("Quest controllers as the gamepad" in the overlay,
on by default). A normal gamepad works too.

| Action | Controllers | Keyboard |
|---|---|---|
| Recentre the view | Hold the **Meta** button | **Home** |
| Turn | Right stick: smooth turning, at the turn speed set in the overlay (there is no snap turning) | Numpad 1 / 3 |
| Change view: first person → third person → diorama | **Right grip + right stick click** | **End** |
| Reset the view (third person / diorama) | **Left grip + left stick click** | |
| Battle: look through the next party member | **Left grip + right stick click** | **Insert** |
| The overlay | | **F8** |

### What to expect

- **Field and battles**: first person (with your body hidden), third person behind the
  player, or a tabletop **diorama** you can grab and scale with the grips. In battle, first
  person follows whoever is on the field, and the summoner during a summon.
- **Menus, dialogue, the battle command window**: on a panel in front of you.
- **2D screens and movies**: a large flat virtual screen.
- **Blitzball**: a large **stereo 3D screen** showing the game's own camera, with automatic
  depth. "Everywhere" in the overlay's View section uses this screen for the whole game
  instead.

---

## 9. When something goes wrong

| Symptom | Try |
|---|---|
| The game starts flat, nothing in the headset | Is Virtual Desktop connected *before* launch? Is **Headset** ticked in the F8 overlay? Does `logs\prereqs.txt` show VDXR as the 32-bit runtime? |
| Fahrenheit doesn't start at all | Not as administrator. Re-run `install-fahrenheit.ps1`. Check `logs\run-*.txt`. |
| Black for a few seconds at some scene changes | The headset should now keep showing the last picture (head-tracked) while the game loads. If it still goes black, send the logs: the `sr:` lines in `ffxvr_diag.log` say why. |
| Ghosting when turning with the stick | Is "smooth stick turns at the headset rate" ticked in the F8 overlay (under the turn speed)? Then the Virtual Desktop refresh rate and SSW, above. If the game drops below 60 fps, use `game-settings.ps1 -Profile vr`. |
| Blurry, or a lower resolution than usual | The F8 overlay says "Game renders at ..., below the headset's per-eye ..." when this is the cause. Two ways to get there: (1) the monitor was off or asleep when the game started: in fullscreen the game then only got Virtual Desktop's virtual display, often 1920x1080. Keep the monitor on when you start the game, raise the virtual display's resolution in the Virtual Desktop Streamer if your version offers it, or use a 4K HDMI/DisplayPort dummy plug. (2) The game's own resolution is set lower than the headset's per-eye size (for example 2560x1440 from `game-settings.ps1 -Profile vr` with a high Virtual Desktop quality preset): raise it with `game-settings.ps1 -Width 3200 -Height 1800`, or lower the quality preset. |
| Everything broke after a Steam update | A game patch moves the addresses Fahrenheit hooks. Wait for Fahrenheit to support the new version, then rebuild with `-Latest`. |

For a bug report, send the newest `logs\run-*.txt` and the `logs\run-*-gamelogs` folder next
to it, plus a description of what you saw.
