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
                                files, adds the loadorder line; -Uninstall
        paths.ps1               the same game finder the repo uses (env var, paths.local.ps1, Steam)
        run-ffx.ps1, game-settings.ps1, import-steam-saves.ps1,
        export-save-to-steam.ps1, install-texture-pack.ps1   (they only need paths.ps1)

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
| Farplane (topher-au) | check its repo before release (no licence file seen in the clone's root - treat as all-rights-reserved until confirmed) | No | we used it as reference only (offsets, byte patterns); credit + link; no code copied |
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
- [ ] version: one place (`ffxvr.csproj` `<Version>`) flowing into the manifest and a `ffxvr_native` version string shown in the overlay; tag matches
- [ ] CHANGELOG.md
- [ ] README: install section for users (requirements, Fahrenheit version/commit, the loadorder line, first-run notes: F8 overlay, headset autostart, recentre), the "Standing on" credits, screenshots or a short clip
- [ ] `ffxvr.ini` defaults reviewed (headset_autostart, overlay_at_start, hide_pointers, markers) - what a new user gets
- [ ] remove the debug-only defaults that only make sense on the dev machine (markers logging every second, hands_dev, census buttons can stay but should be in a "Debug" header - they are)
- [ ] `git ls-files` audit for anything that should not be public (paths with the user's name in docs? the `Tidus - HP` folder name appears in docs - fine, it is just a name)
- [ ] GitHub: repo description, topics (ffx, vr, openxr, fahrenheit, quest), issues enabled, a release template
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

- Farplane's licence (and whether to keep it in the credits as "reference" only).
- Whether the Fahrenheit crew is fine with a mod depending on `main`; ask them.
- Code signing: not required; mention in the README that the DLLs are unsigned and SmartScreen may complain.
