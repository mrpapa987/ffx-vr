# FFX VR {VERSION}

VR for **FINAL FANTASY X HD Remaster** (Steam, PC). Alpha: expect rough edges.
Built from commit `{COMMIT}` on {DATE}.

Tested on one setup only: Windows 11, an RTX 3080 Ti, and a **Quest 3 over Virtual Desktop
(VDXR)**. Anything else is untested.

> An unofficial fan mod, not affiliated with Square Enix. You need your own copy of
> FINAL FANTASY X/X-2 HD Remaster on Steam. No game files are included.

---

## What's in this zip

| | |
|---|---|
| `mod\ffxvr\` | The mod itself (copied into the game folder by the installer) |
| `tools\install-mod.ps1` | Installs, updates or uninstalls the mod, and checks what's missing |
| `tools\run-ffx.ps1` | Starts the game through Fahrenheit and collects logs for bug reports |
| `tools\game-settings.ps1`, `import-steam-saves.ps1`, `export-save-to-steam.ps1`, `install-texture-pack.ps1` | Optional extras (section 6) |
| `LICENSE`, `THIRD-PARTY-NOTICES` | Licences (MIT for FFX VR; Apache-2.0 for the bundled OpenXR loader) |

Extract it **anywhere outside the game folder**, e.g. `Documents`. Inside you'll find a folder
`ffx-vr-{VERSION}` with this README and `tools\` in it. **Run every command below from that
folder** (open it in Explorer, then right-click an empty spot > *Open in Terminal*; on Windows 10,
Shift+right-click > *Open PowerShell window here*, or type `powershell` in Explorer's address bar
and press Enter), in PowerShell,
**not as administrator**: Fahrenheit refuses to start when elevated. Pick a folder whose name has
no `[` or `]` in it: Windows PowerShell may not run scripts from there.

---

## 1. What you need

| | |
|---|---|
| **FINAL FANTASY X/X-2 HD Remaster** (Steam) | The current version, after the 1 October 2026 patch. Run it normally once first. |
| **A Quest headset and [Virtual Desktop](https://www.vrdesktop.net/)** | Virtual Desktop on the headset, and the **Virtual Desktop Streamer** on the PC. |
| **VDXR as the OpenXR runtime** | In the Streamer app on the PC, choose **VDXR** as the OpenXR runtime. SteamVR isn't supported in this alpha. |
| **.NET 10 runtime, x86 (32-bit)** | `winget install Microsoft.DotNet.Runtime.10 --architecture x86`. The game is 32-bit, so the x86 one is the one that matters, even on 64-bit Windows. |
| **Fahrenheit** {FAHRENHEIT_RELEASE}, the mod loader | See section 2. |
| **FFX-60fps** (recommended) | See section 3. |

---

## 2. Fahrenheit (read this first)

FFX VR runs inside [Fahrenheit](https://github.com/fahrenheit-crew/fahrenheit), a separate
project that isn't included in this zip. It installs into `<game folder>\fahrenheit\`. This
build was tested with Fahrenheit release **{FAHRENHEIT_RELEASE}** (commit `{FAHRENHEIT_COMMIT}`).
Older releases (alpha11 and earlier) do not work with the current game.

1. Download `fahrenheit_release_{FAHRENHEIT_RELEASE}.zip` from the
   [Fahrenheit releases page](https://github.com/fahrenheit-crew/fahrenheit/releases/tag/{FAHRENHEIT_RELEASE})
   (under *Assets*).
2. If you had Fahrenheit installed before, delete `<game folder>\fahrenheit\bin` (keep `config`,
   `mods`, `saves` and `state`).
3. Extract the zip so that its `bin` folder becomes `<game folder>\fahrenheit\bin`. Watch out:
   Explorer's *Extract All* suggests a new folder named after the zip
   (`...\fahrenheit\fahrenheit_release_{FAHRENHEIT_RELEASE}\bin`), so remove that last part of the
   path and extract to `<game folder>\fahrenheit\`.

`tools\install-mod.ps1` checks that `fahrenheit\bin\fhstage0.exe` is there and shows the
Fahrenheit version it found.

Developers can build Fahrenheit from source: see `INSTALL-ALPHA.md` in the
[FFX VR repository](https://github.com/mrpapa987/ffx-vr).

---

## 3. FFX-60fps (recommended)

The game runs at 30 fps. [FFX-60fps](https://github.com/peternguyenst/FFX-60fps) draws 60 by
smoothing between frames, which makes a big difference in VR. Install it per its own README
(its files go next to `FFX.exe`), with `fps=60` in its `fps60.ini`.

---

## 4. Install FFX VR

```
powershell -ExecutionPolicy Bypass -File .\tools\install-mod.ps1
```

It finds the game through Steam, checks the list in section 1, copies the mod into
`<game folder>\fahrenheit\mods\ffxvr\` and adds `ffxvr` to Fahrenheit's load order.

- **Game not found?** Tell it where the game is, either for this window only:
  `$env:FFXVR_GAME_DIR = 'D:\SteamLibrary\steamapps\common\FINAL FANTASY FFX&FFX-2 HD Remaster'`
  or permanently, by creating `tools\paths.local.ps1` containing the line
  `$FfxvrGameOverride = 'D:\SteamLibrary\steamapps\common\FINAL FANTASY FFX&FFX-2 HD Remaster'`.
  The folder must contain `FFX.exe`.
- **Updating:** extract the new zip and run its `install-mod.ps1`. Your settings
  (`ffxvr.ini`) are kept. To reset them, delete `ffxvr.ini` with the game closed.
- **Uninstalling:** `install-mod.ps1 -Uninstall`.

---

## 5. Play

1. On the PC, start the **Virtual Desktop Streamer**. Put the headset on and connect to the PC
   in Virtual Desktop.
2. Start the game **through Fahrenheit**, not from Steam:
   ```
   powershell -ExecutionPolicy Bypass -File .\tools\run-ffx.ps1
   ```
   Steam must be running. When the game closes, the script collects the logs into `logs\`.
3. In the game, press **F8** for the FFX VR overlay. Tick **Headset**, and **start
   automatically** if you want it on from the next launch.

### Virtual Desktop settings

- **Refresh rate: 120 Hz** (72 Hz also works). Avoid 90 Hz: 60 fps doesn't divide into it.
- **SSW (Synchronous Spacewarp): Disabled**, not "Always" (Auto is not tested with this build).
  The mod fills the refreshes between game frames itself, turned with the stick, so stick turns
  should be as smooth as head turns ("smooth stick turns at the headset rate" in the overlay, on by
  default; the `sr:` lines in `ffxvr_diag.log` show what it does).
- Set the game's **V-Sync off** (the headset paces the frames).
- **Quit through the game's menu**, not by closing the window.
- Play **seated**.

### Controls

The Quest controllers act as the gamepad (on by default; "Quest controllers as the gamepad" in
the overlay). A normal gamepad works too.

| Action | Controllers | Keyboard |
|---|---|---|
| Recentre the view | Hold the **Meta** button | **Home** |
| Turn | Right stick: smooth turning, at the turn speed set in the overlay (there is no snap turning) | Numpad 1 / 3 |
| Change view: first person, third person, diorama | **Right grip + right stick click** | **End** |
| Reset the view (third person / diorama) | **Left grip + left stick click** | |
| Battle: look through the next party member | **Left grip + right stick click** | **Insert** |
| The overlay | | **F8** |

### What to expect

- **Field and battles**: first person (your body hidden), third person behind the player, or
  a tabletop **diorama** you can grab and scale with the grips. In battle, first person
  follows whoever is on the field, and the summoner during a summon.
- **Menus, dialogue, the battle command window**: on a panel in front of you.
- **2D screens and movies**: a large flat virtual screen.
- **Blitzball**: a large **stereo 3D screen** showing the game's own camera, with automatic
  depth. "Everywhere" in the overlay's View section uses this screen for the whole game.

---

## 6. Optional extras

Prefix each with `powershell -ExecutionPolicy Bypass -File`.

| What | Command |
|---|---|
| **Lighter render resolution**: 2560x1440 instead of 4K, much easier on the GPU (the mod does its own anti-aliasing) | `.\tools\game-settings.ps1 -Profile vr` (`-Profile flat` puts yours back) |
| **Your existing saves**: Fahrenheit keeps its own save slots | `.\tools\import-steam-saves.ps1` copies your normal saves in (it never replaces a VR save unless you pass `-Force`); `.\tools\export-save-to-steam.ps1` copies one back |

### Later, if you like: HD textures

Not needed, and best left until everything else works: it's a big download, a lot of disk
space, and one more thing to rule out when something goes wrong. The
[Re-Remastered texture pack](https://www.nexusmods.com/finalfantasyxx2hdremaster/mods/244)
works with FFX VR. Download it yourself (needs [7-Zip](https://www.7-zip.org/) installed), and
**don't** install the External File Loader that comes with it; Fahrenheit serves the files
instead:

```
powershell -ExecutionPolicy Bypass -File .\tools\install-texture-pack.ps1 -Archive "<path to the .7z>"
```

`-Compress` halves the disk use; `-Uninstall` removes it.

---

## 7. When something goes wrong

| Symptom | Try |
|---|---|
| The game starts flat, nothing in the headset | Was Virtual Desktop connected *before* launch? Is **Headset** ticked in the F8 overlay? Does `install-mod.ps1` show VDXR as the 32-bit OpenXR runtime? |
| Fahrenheit doesn't start at all | Not as administrator. Is Fahrenheit release {FAHRENHEIT_RELEASE} installed (section 2; `install-mod.ps1` shows the version it found)? Check `logs\run-*.txt`. |
| No F8 overlay | The mod didn't load: re-run `install-mod.ps1` and check the .NET x86 runtime. |
| Black for a few seconds at some scene changes | The headset should now keep showing the last picture (head-tracked) while the game loads. If it still goes black, send the logs: the `sr:` lines in `ffxvr_diag.log` say why. |
| Ghosting when turning with the stick | Is "smooth stick turns at the headset rate" ticked in the F8 overlay (under the turn speed)? Then the Virtual Desktop refresh rate and SSW, above. If the game drops below 60 fps, try `game-settings.ps1 -Profile vr`. |
| Blurry, or a lower resolution than usual | The F8 overlay says "Game renders at ..., below the headset's per-eye ..." when this is the cause. Two ways to get there: (1) the monitor was off or asleep when the game started: in fullscreen the game then only got Virtual Desktop's virtual display, often 1920x1080. Keep the monitor on when you start the game, raise the virtual display's resolution in the Virtual Desktop Streamer if your version offers it, or use a 4K HDMI/DisplayPort dummy plug. (2) The game's own resolution is set lower than the headset's per-eye size (for example 2560x1440 from `game-settings.ps1 -Profile vr` with a high Virtual Desktop quality preset): raise it with `game-settings.ps1 -Width 3200 -Height 1800`, or lower the quality preset. |
| Everything broke after a Steam update | A game patch moves the addresses Fahrenheit hooks. Wait for a Fahrenheit (and maybe FFX VR) update. |

**Bug reports:** send the newest `logs\run-*.txt` and the `logs\run-*-gamelogs` folder next to
it, with a description of what you saw and the FFX VR version ({VERSION}).

---

FFX VR is MIT-licensed; see `LICENSE`. The bundled OpenXR loader is Apache-2.0; see
`THIRD-PARTY-NOTICES`, which also credits the projects FFX VR builds on.
