# Making a proper release (notes, started 22 Oct 2026)

What it takes to put this mod on GitHub as a real release - a zip people
can drop into their game - without stepping on anyone's licence or
shipping anything that is not ours to ship. Running notes; tick things off
as they get done.

## 1. What a release is

A GitHub release (tag `vX.Y.Z`) with one zip attached, laid out exactly as
the mod folder is deployed:

    ffxvr/
      ffxvr.dll                 our managed module (C#)
      ffxvr.deps.json
      ffxvr.runtimeconfig.json
      ffxvr.manifest.json       Fahrenheit's mod manifest (version, authors, link)
      ffxvr_native.dll          our native DLL (C++)
      openxr_loader.dll         Khronos OpenXR loader, 32-bit (Apache-2.0)
      efl/x/, efl/x2/           empty file-loader roots (Fahrenheit layout)
      LICENSE                   ours
      THIRD-PARTY-NOTICES       everything below that ends up in the zip
      README (install steps)

Users unzip into `fahrenheit\mods\` and add `ffxvr` to `loadorder`. The
`ffxvr.ini` is NOT in the zip: the mod writes it on first run (and
build-mod.ps1 now preserves it across installs). No `.pdb`, no logs, no
captures, no bone labels, no `.bak`.

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
| Fahrenheit SDK (`Fahrenheit.Sdk/1.0.0-alpha12`, the build-time package) | part of Fahrenheit | No | none beyond the above |
| OpenXR loader + headers (Khronos, NuGet `OpenXR.Loader` / `OpenXR.Headers`) | Apache-2.0 | **Yes** (`openxr_loader.dll`) | ship the Apache-2.0 text and the package's NOTICE in THIRD-PARTY-NOTICES; state that the DLL is unmodified |
| Dear ImGui / Hexa.NET.ImGui | MIT | No - we call Fahrenheit's copy | credit only |
| Direct3D 11, DXGI, D3DCompiler, XInput, PSAPI | Windows system libraries | No | none |
| FFX-60fps (peternguyenst) | Apache-2.0; its MinHook is BSD-2-Clause | **No** - `tools/build-fps60.ps1` clones and builds it on the user's machine | credit + link. If we ever bundle its binaries: its LICENSE, NOTICE and MinHook's licence go in the zip. Audit: no code of theirs has been copied into `src/` (checked 22 Oct; only addresses learned from reading it, which is fine). |
| Farplane (topher-au) | check its repo before release (no licence file seen in the clone's root - treat as all-rights-reserved until confirmed) | No | we used it as reference only (offsets, byte patterns); credit + link; no code copied |
| Re-Remastered texture pack (HitokageWorshipper, Nexus 244) | Nexus mod, author's permissions | **No** | `tools/install-texture-pack.ps1` works on the user's own download; link to the Nexus page; do not mirror |
| Virtual Desktop / VDXR, SteamVR | proprietary runtimes | No | nothing to ship; name them in the requirements |
| Our own code | to decide (see 3) | Yes | LICENSE file at the root and in the zip |

## 3. Our licence

Still "to be decided" in the README. Constraints: nothing we ship is
copyleft-contaminated (Fahrenheit is used, not copied; OpenXR is Apache).
Candidates: **MIT** (simplest, what most mods use) or **Apache-2.0** (same
family as the OpenXR loader, adds a patent grant). Either is fine; pick
one, add `LICENSE`, put the SPDX id in `ffxvr.csproj`
(`<PackageLicenseExpression>`) and the `.vcxproj` has no field, so a
header comment in the two `.cpp`/`.h` files. Note in the README that the
mod contains no game assets and that the game must be bought.

## 4. The Fahrenheit dependency problem

The mod is built against Fahrenheit `main` (the alpha11 release does not
work; setup.md says so) with `Fahrenheit.Sdk/1.0.0-alpha12`. A user cannot
download a matching Fahrenheit. Options, in order of preference:
1. Ask the Fahrenheit crew whether a release is coming that includes what
   we need (stage-0 stub debugger, the file loader, the battle structs);
   once it exists, build against it and state the version.
2. Until then: pin the exact Fahrenheit commit in `docs/setup.md` and the
   release notes, and ship `tools/build-fahrenheit-full.ps1` as the
   supported route ("build Fahrenheit from commit X"). Painful but honest.
3. Not an option: bundling our own Fahrenheit build (assets clause above).

Also record the exact FFX-60fps commit the mod was tested with, and the
game version (Steam build id).

## 5. Before the first tag

- [ ] LICENSE (ours) + THIRD-PARTY-NOTICES (OpenXR Apache-2.0 + NOTICE; credits for the rest) at the repo root; both copied into the zip by build-mod.ps1
- [ ] `tools/package-release.ps1`: builds, assembles the zip from a clean folder (never from the live mod folder - it has the ini, logs and captures), writes a SHA-256 next to it
- [ ] version: one place (`ffxvr.csproj` `<Version>`) flowing into the manifest and a `ffxvr_native` version string shown in the overlay; tag matches
- [ ] CHANGELOG.md
- [ ] README: install section for users (requirements, Fahrenheit version/commit, the loadorder line, first-run notes: F8 overlay, headset autostart, recentre), the "Standing on" credits, screenshots or a short clip
- [ ] `ffxvr.ini` defaults reviewed (headset_autostart, overlay_at_start, hide_pointers, markers) - what a new user gets
- [ ] remove the debug-only defaults that only make sense on the dev machine (markers logging every second, hands_dev, census buttons can stay but should be in a "Debug" header - they are)
- [ ] `git ls-files` audit for anything that should not be public (paths with the user's name in docs? the `Tidus - HP` folder name appears in docs - fine, it is just a name)
- [ ] GitHub: repo description, topics (ffx, vr, openxr, fahrenheit, quest), issues enabled, a release template
- [ ] Nexus / Reddit posts link to the GitHub release, never to a rehost

## 6. Open questions

- Farplane's licence (and whether to keep it in the credits as "reference" only).
- Whether the Fahrenheit crew is fine with a mod depending on `main`; ask them.
- Code signing: not required; mention in the README that the DLLs are unsigned and SmartScreen may complain.
