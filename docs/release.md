# Making a proper release (notes, started 22 Oct 2026)

What it takes to put this mod on GitHub as a real release - a zip people
can drop into their game - without stepping on anyone's licence or
shipping anything that is not ours to ship. Running notes; tick things off
as they get done.

## 1. What a release is

A GitHub release (tag `vX.Y.Z`) with one zip attached, made by
`tools\package-release.ps1` (22 Oct 2026). The zip is for players, who install without
building anything:

    ffx-vr-<label>/
      README.md                 packaging/README-zip.md, version/commit/date filled in
      LICENSE, THIRD-PARTY-NOTICES
      mod/ffxvr/
        ffxvr.dll, ffxvr.deps.json, ffxvr.runtimeconfig.json, ffxvr.manifest.json
        ffxvr_native.dll
        openxr_loader.dll       Khronos OpenXR loader, 32-bit (Apache-2.0)
      tools/
        install-mod.ps1         from packaging/: checks Fahrenheit, .NET x86, the 32-bit
                                OpenXR runtime and FFX-60fps; copies mod/ffxvr into
                                <game>\fahrenheit\mods\ffxvr (keeps the ini), unblocks the
                                files, adds the loadorder line, warns about diagnostics left on in a
                                kept ini; -Uninstall [-Purge], -ResetLogging, -DeleteLogs
        paths.ps1               the same game finder the repo uses (env var, paths.local.ps1, Steam)
        run-ffx.ps1, game-settings.ps1, import-steam-saves.ps1,
        export-save-to-steam.ps1, install-texture-pack.ps1,
        set-logging.ps1                                      (they only need paths.ps1)

`<label>` is `-Version` if given, else the csproj `<Version>` plus the short commit
(`-dirty` with uncommitted changes). A `.sha256` file is written next to the zip; `dist/` is
git-ignored. The script builds with `build-mod.ps1 -NoDeploy` (works without the game
installed), takes the managed files from the build folder that has the manifest, and
assembles everything in a fresh staging folder, never from the live mod folder (that has the
ini, logs and captures). `-SkipBuild` packs the existing build but refuses if any source
file is newer than the DLLs. It warns if a packed DLL contains a `C:\Users\<name>\` path
(debug-info paths; ours are `C:\projects\FFX-VR\...`, which is fine).

Not in the zip: `ffxvr.ini` (the mod writes it on first run), `.pdb`, logs, captures, bone
labels, `.bak`; Fahrenheit and FFX-60fps (separate projects, own licences and releases; the
README sends players to them). The README puts the HD texture pack last, as an optional
"later" step: most players won't start with it.

Fahrenheit for zip players (resolved, Oct 2026): the official release v1.0.0-alpha12 works with
the 1 Oct 2026 game patch, so players download its zip from the Fahrenheit releases page and
extract it into `<game>\fahrenheit\` (README-zip section 2); nobody has to build Fahrenheit. The
README names the tested release and commit from `tools/paths.ps1` (`{FAHRENHEIT_RELEASE}`,
`{FAHRENHEIT_COMMIT}`), and `install-mod.ps1` shows the version it finds. Bundling a Fahrenheit
build would still be allowed for the LGPL code but not for its assets folder (section 2).

The source release is the repo itself at the tag. The repo must stay free
of: game files of any kind (VBF contents, `.phyre`, shaders, textures,
models - including the PS2 Tidus model used for the skeleton work, which
lives outside the repo), texture packs, RenderDoc captures, eye-image
captures (`seq_*.bmp`, `img_*.bmp`), the texture pack archive, `_ext/`
clones. `.gitignore` covers these today; check `git ls-files` before
tagging. The docs quote addresses and short disassembly from `FFX.exe`,
which is normal for a modding project; keep it to what is needed (no
bulk dumps).

## 2. Licences, component by component

| Component | Licence | In the zip? | Obligation |
|---|---|---|---|
| Fahrenheit (framework we run in; `Fahrenheit.Core` etc.) | LGPL-3.0-or-later; its `assets` folder is NOT free (forks of Fahrenheit only) | **No** - users install Fahrenheit themselves | Our module links against it as a "work that uses the library": we may use any licence; must not bundle their assets; credit + link. Do not redistribute a Fahrenheit build (the assets clause makes a bundled copy questionable). |
| Fahrenheit SDK (`Fahrenheit.Sdk`, the build-time package, from nuget.org; version = `$FfxvrFhVersion` in `tools/paths.ps1`) | part of Fahrenheit | No | none beyond the above |
| OpenXR loader + headers (Khronos, NuGet `OpenXR.Loader` / `OpenXR.Headers`) | Apache-2.0 | **Yes** (`openxr_loader.dll`) | ship the Apache-2.0 text and the package's NOTICE in THIRD-PARTY-NOTICES; state that the DLL is unmodified |
| Dear ImGui / Hexa.NET.ImGui | MIT | No - we call Fahrenheit's copy | credit only |
| Direct3D 11, DXGI, D3DCompiler, XInput, PSAPI | Windows system libraries | No | none |
| FFX-60fps (peternguyenst) | Apache-2.0; its MinHook is BSD-2-Clause | **No** - `tools/build-fps60.ps1` clones and builds it on the user's machine | credit + link. If we ever bundle its binaries: its LICENSE, NOTICE and MinHook's licence go in the zip. Audit: no code of theirs has been copied into `src/` (checked 22 Oct; only addresses learned from reading it, which is fine). |
| Farplane (topher-au) | **none**: no LICENSE / COPYING / NOTICE file anywhere in the repo, no licence in its README; only `AssemblyInfo.cs`'s "Copyright © 2016" (checked 2 Nov 2026, last commit 943b2c7 of Apr 2018). So all rights reserved | No | reference only (memory offsets, data layouts, which we re-verified in the exe ourselves; facts, not code); no code, data or resources copied, and none may be; credit + link (README "Standing on", THIRD-PARTY-NOTICES) |
| Re-Remastered texture pack (HitokageWorshipper, Nexus 244) | Nexus mod, author's permissions | **No** | `tools/install-texture-pack.ps1` works on the user's own download; link to the Nexus page; do not mirror |
| Virtual Desktop / VDXR, SteamVR | proprietary runtimes | No | nothing to ship; name them in the requirements |
| Our own code | to decide (see 3) | Yes | LICENSE file at the root and in the zip |

## 3. Our licence

**Decided (22 Oct 2026): MIT.** `LICENSE` at the root (copyright "mrpapa" -
change to a real name if wanted), SPDX headers in all five source files,
`THIRD-PARTY-NOTICES` at the root (the OpenXR loader with the full
Apache-2.0 text, credits for the rest), README licence and disclaimer
sections. The notes below were written before the decision.

Still "to be decided" in the README. Constraints: nothing we ship is
copyleft-contaminated (Fahrenheit is used, not copied; OpenXR is Apache).
Candidates: **MIT** (simplest, what most mods use) or **Apache-2.0** (same
family as the OpenXR loader, adds a patent grant). Either is fine; pick
one, add `LICENSE`, put the SPDX id in `ffxvr.csproj`
(`<PackageLicenseExpression>`) and the `.vcxproj` has no field, so a
header comment in the two `.cpp`/`.h` files. Note in the README that the
mod contains no game assets and that the game must be bought.

## 4. The Fahrenheit dependency

Built and tested against the official Fahrenheit release **v1.0.0-alpha12** (commit 7e85839;
the version and commit live in `tools/paths.ps1`, `ffxvr.csproj`'s SDK version must equal it,
build-mod checks). The SDK and the `fahrenheit` package come from nuget.org only
(`src/managed/nuget.config`, into `_ext/nuget`), and build-mod logs where each came from. Until
alpha13, our module turns Fahrenheit's ImGui DPI font scaling off itself (FH-1, design-notes
"Fahrenheit port"). Options, then and now:
1. Build against an official release and state its version: done (alpha12).
2. Pin a main commit and have users build Fahrenheit (`tools/build-fahrenheit-full.ps1`): now only
   the route for trying alpha13-dev code before its release.
3. Not an option: bundling our own Fahrenheit build (assets clause above).

Also record the exact FFX-60fps commit the mod was tested with, and the
game version (Steam build id).

## 5. Before the first tag

- [x] LICENSE (ours) + THIRD-PARTY-NOTICES (OpenXR Apache-2.0; credits for the rest) at the repo root
- [x] both copied into the release zip by the packaging script
- [x] `tools/package-release.ps1`: builds, assembles the zip from a clean folder (never from the live mod folder - it has the ini, logs and captures), writes a SHA-256 next to it
- [x] version: one place (`ffxvr.csproj` `<Version>`, now `0.1.0-alpha.1`) flowing into the assembly's informational version,
  the manifest, and (through `src/native/ffxvr_version.h`, written by build-mod) the native DLL's `ffxvr_build_info`; both
  shown at the bottom of the F8 overlay, in Debug, and at the top of `ffxvr_diag.log`; package-release names the zip from it.
  The tag is `v` + it (release procedure below)
- [x] CHANGELOG.md
- [x] README: player-facing (requirements, install in brief, first-run notes, known issues, bug reports, credits,
  disclaimer). **Still to do: screenshots or a short clip** (placeholder comment at the top of README.md)
- [x] `ffxvr.ini` defaults reviewed (Nov 2026, release prep notes): no change to the play defaults; questions for the owner
  on battle_transition (code 2, the owner runs 1), dream_overlay (code 1, the owner runs 0) and hd_cluster_cache (code 0,
  the owner runs 1)
- [x] debug-only defaults: every diagnostic off for a fresh install (diag_load_lines 1 -> 0; the new diag_verbose gates
  the periodic lines, the legacy markers' 1 s "window:" line, the camera trace and Ctrl+F9's 100 MB dump);
  `ffxvr_diag.log` per session and capped at 4 MB (64 MB while any diagnostic is on); `tools/set-logging.ps1 -Profile
  Release|Verbose|Dev` (players are pointed at Verbose: more logging, no hooks; Dev is the developer's set); hands_dev was
  already 0
- [x] `git ls-files` audit (2 Nov 2026): no personal paths, no game files, captures or binaries; three research notes
  had the research container's `/home/...` path (made relative). The handle `mrpapa` (LICENSE, csproj Authors) and
  `mrpapa987` (the repo URL) are intended
- [x] issue templates (`.github/ISSUE_TEMPLATE/bug_report.yml`, `feature_request.yml`, `config.yml`) and a release notes
  template (`.github/release-template.md`)
- [ ] GitHub (the owner, on the site): repo description, topics (ffx, final-fantasy-x, vr, openxr, fahrenheit, quest,
  virtual-desktop), Issues enabled (Discussions optional: `config.yml` links to it), make the repository public
- [ ] Nexus / Reddit posts link to the GitHub release, never to a rehost
- [ ] ImGui text is a printf format (Hexa passes `ImGui.Text*`'s string as the format): a stray `%` prints garbage or kills
  the game. Before every release this must print nothing (Git Bash or WSL, from the repo root); fix a hit with `%%`:
  ```
  grep -noE 'ImGui\.(Text|TextWrapped|TextDisabled|TextColored|BulletText|LabelText|SetTooltip|SetItemTooltip)\(("([^"\\]|\\.)*"|[^";])*' src/managed/*.cs | grep -E '[^%]%[^%]|%$'
  grep -nE '^\s*(\+|"|\$")' src/managed/*.cs | grep -vE 'ImGui\.' | grep -E '[^%]%[^%]|%$'
  ```
  The first checks each call up to its `;` (so a slider's `"%.1f"` later on the line is not a hit), the second the
  continuation lines of a call split over several lines (review any hit by hand: a log line is fine).
  **The rule:** every **non-literal** argument of `ImGui.Text*` / `SetTooltip` goes through `esc()` (diag7.cs): fields,
  helper return values (our own status helpers included: a string built elsewhere is invisible to the two greps above, as
  uidepth's "UI on NN% of the panel" was), native, file and game strings, exception messages, paths. The only exemption is
  an interpolated string whose holes are numbers or our own literal ternaries. Slider/drag formats (`"%.1f"`) are formats
  on purpose; widget labels are not formatted.
  A third, catch-all command lists every string literal with a lone `%` anywhere in the managed code:
  ```
  grep -nE '"[^"]*[^%]%[^%]|"%[^%.]' src/managed/*.cs | grep -vE '^\S+:\s*//' | grep -vE '"%\.[0-9]+f[^"]*"'
  ```
  Known hits, all fine (Oct 2026, branch fhport): `diag7.cs` `esc()` itself; `hd_cache.cs` `closed (vram use ... % of ...)`
  (a log line); `module.cs` `(i % 9) % 4` in "reset calibration" (a modulo); `uidepth.cs` `UI on ...% of the panel`
  (shown through `esc()`). Any other line needs a look.

## 6. Open questions

- ~~Farplane's licence~~: it has none (section 2); kept in the credits as a reference only.
- Whether the Fahrenheit crew is fine with a mod depending on `main`; ask them.
- Code signing: not required; mention in the README that the DLLs are unsigned and SmartScreen may complain.

## 7. Release procedure

**Status (3 Nov 2026): experimental.** The release tools (`package-release.ps1`, `set-logging.ps1`, `install-mod.ps1`'s
`-ResetLogging` / `-DeleteLogs` / `-Purge`, the fresh-install defaults) were reviewed and run in a sandbox, and the new
build runs on the developer's setup without breaking it, but the player-side tests (release-prep test steps 3-7: the
logging profiles, a fresh install from the zip, no headset, uninstall) have not been run on Windows yet. Before the first
public tag, have a second person run them on a separate install.

1. **Version.** Set `<Version>` in `src/managed/ffxvr.csproj` (e.g. `0.1.0-alpha.1`); move the CHANGELOG entry from
   "unreleased" to the date. Commit. Nothing else holds the version.
2. **Checks.** Section 5's printf greps print nothing; `git status` is clean; `git ls-files` has nothing new that should not
   be public.
3. **Tag.** `git tag -a v0.1.0-alpha.1 -m "FFX VR 0.1.0-alpha.1"` on that commit, `git push --tags`.
4. **Package**, from the tagged, clean checkout:
   `powershell -ExecutionPolicy Bypass -File .\tools\package-release.ps1 -Version 0.1.0-alpha.1`
   (it warns if `-Version` is not the csproj's, or if the tree is dirty). Out: `dist\ffx-vr-0.1.0-alpha.1.zip` and its
   `.sha256`.
5. **Smoke test the zip** on the game PC (the fail-safe tests in the release-prep notes): a fresh install into a game folder
   with no `ffxvr.ini` (F8 shows the version and "diagnostics: all off"; `ffxvr_diag.log` stays small), a start without the
   headset, `install-mod.ps1 -Uninstall -Purge`.
6. **GitHub release** from the tag: title `FFX VR 0.1.0-alpha.1`, tick "pre-release", body from
   `.github/release-template.md` + the CHANGELOG entry + the SHA-256; attach the zip and the `.sha256`.
7. **Your own setup afterwards.** The release defaults are for players; for development keep (or restore) the diagnostics
   with `tools\set-logging.ps1 -Profile Dev` (and `-Profile Release` to see what a player gets). A new build keeps an existing
   ini's values: an ini older than `ini_rev` 2 reads diag_verbose as 1 and is saved as rev 2 with it a few seconds after the
   start (and one older than rev 1 still gets the loads build's load_lock_release / heat_haze migration).
8. Nexus / Reddit posts link to the GitHub release, never to a rehost.
