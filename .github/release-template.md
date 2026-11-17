<!-- GitHub release notes for vX.Y.Z (copy the CHANGELOG entry; fill in the brackets). Tick "Set as a pre-release" for
     an -alpha / -beta version. Attach: ffx-vr-X.Y.Z.zip, ffx-vr-X.Y.Z.zip.sha256, and, when made,
     ffxvr-target-art-X.Y.Z.zip and its .sha256 (docs/release.md section 7). -->
**FFX VR X.Y.Z** - VR for FINAL FANTASY X HD Remaster (Steam, PC), alpha.

Download `ffx-vr-X.Y.Z.zip` below, extract it outside the game folder and follow the `README.md` inside.
SHA-256: `[paste from ffx-vr-X.Y.Z.zip.sha256]`

**Requires:** FINAL FANTASY X/X-2 HD Remaster (Steam, current patch) · Fahrenheit
[v1.0.0-alpha12](https://github.com/fahrenheit-crew/fahrenheit/releases/tag/v1.0.0-alpha12) (official release zip) ·
.NET 10 x86 runtime · Quest 3 + Virtual Desktop (VDXR), 120 Hz, SSW Disabled · game V-Sync off ·
recommended: [FFX-60fps](https://github.com/peternguyenst/FFX-60fps) at 60 fps.

**Tested on one setup only:** Quest 3, Virtual Desktop (VDXR), RTX 3080 Ti.

**HD texture pack caution:** the Re-Remastered pack is optional and caused performance problems on the developer's
RTX 3080 Ti: not recommended without stronger hardware. It also needs a lot of disk space (the Medium pack is about
18 GB of loose files once installed).

**Optional download: `ffxvr-target-art-X.Y.Z.zip`** - textured medallions for the Bushido targets. **AI-generated art**
(generated with Google Gemini and upscaled with Upscayl by the project owner; no rights claimed, free to use). Extract
it into the same folder as the mod's zip, then run `tools\install-mod.ps1`. Without it the mod draws plain targets.
SHA-256: `[paste from ffxvr-target-art-X.Y.Z.zip.sha256]`

**Updating:** extract the new zip and run its `tools\install-mod.ps1`; your settings are kept (so a changed default,
such as VR hands now being on, does not reach an existing `ffxvr.ini` by itself: the CHANGELOG says what to tick).

**Controls:** `docs\img\controls.png` and `docs\img\controls-chords.png` in the zip show every button and chord.

**Testers:** `TESTING.md` in the zip says what to try in about an hour and what to send back.

### Changes
[the CHANGELOG entry]

### Known issues
[from the CHANGELOG / README]

**Bugs:** please report them in this repository's Issues with the logs from `tools\run-ffx.ps1`, never to
the Fahrenheit or FFX-60fps projects. Unofficial fan mod; not affiliated with Square Enix. No game files included.
