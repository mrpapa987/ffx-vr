# package-release.ps1
#
# Builds the mod and packs a release zip for players, who install it without building anything:
#
#   dist\ffx-vr-<label>.zip
#     ffx-vr-<label>\
#       README.md              packaging\README-zip.md with the version filled in (its image links ../docs/img/ -> docs/img/)
#       TESTING.md             docs\tester-checklist.md (its image links img/ -> docs/img/): what a tester tries and sends back
#       LICENSE, THIRD-PARTY-NOTICES
#       docs\img\              controls.png, controls-chords.png (the controller diagrams the README and TESTING.md show)
#       mod\ffxvr\             ffxvr.dll (+ deps/runtimeconfig/manifest json), ffxvr_native.dll,
#                              openxr_loader.dll, hands\generic-glove\ and hands\generic\ (our generic hand packs, MIT:
#                              an allowlist, tools\paths.ps1 $FfxvrHandPacks; a guard below fails on anything else there)
#       tools\                 install-mod.ps1 (from packaging\), paths.ps1, run-ffx.ps1, import-hands.ps1 and
#                              hands\{import-hands,objio,handlib,preview-pack}.py (VR hands: make a pack from your own model),
#                              game-settings.ps1, import-steam-saves.ps1, export-save-to-steam.ps1,
#                              install-texture-pack.ps1, set-logging.ps1
#   dist\ffx-vr-<label>.zip.sha256
#   dist\ffxvr-target-art-<label>.zip (+ .sha256)   OPTIONAL, a separate download: the owner's AI-generated art, never in
#                              the repository or in the mod's zip: the Bushido target art pack (the medallions), from
#                              <game>\fahrenheit\mods\ffxvr\art\targets\ or -ArtDir, and the overdrive bar art pack (Tidus's
#                              Swordplay bar, Lulu's countdown), from <game>\fahrenheit\mods\ffxvr\art\odbar\ or -BarArtDir; it
#                              mirrors the mod zip's layout, ffx-vr-<label>\mod\ffxvr\art\targets\ (8 PNGs, arrow.png when there,
#                              ART-NOTICE.txt) and ...\art\odbar\ (7 PNGs, ART-NOTICE.txt), so extracting both zips into one folder
#                              and running install-mod installs the art too. Each pack goes in when it is complete; neither:
#                              skipped with a note, never a failure. -NoArt skips it.
#
# Fahrenheit and FFX-60fps are NOT included: they are separate projects with their own licences
# and releases (see docs\release.md). No settings, logs, captures or .pdb files are included, and
# nothing is taken from the game folder: everything comes from this repository's build output.
# Never FMV depth (fmvdepth\, *.fvd) or previews: they are made on a player's PC from the game's videos (a guard below
# fails the package if any turns up). The FMV depth converter (convert-fmv-depth.ps1, tools\fmvdepth\) is not in the zip
# yet: it is a developer tool for now (docs\setup.md).
#
# <label> is -Version if given, otherwise the csproj <Version> plus the git commit, e.g.
# 0.1.0-alpha.1-b993ffc ("-dirty" is added when there are uncommitted changes). For the release itself pass
# -Version equal to the csproj <Version> (e.g. -Version 0.1.0-alpha.1), so the zip is named like the tag v0.1.0-alpha.1.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\package-release.ps1 [-Version 0.1.0-alpha.1] [-SkipBuild] [-OutDir <folder>]
#              [-ArtDir <folder with the 8 PNGs>] [-BarArtDir <folder with the 7 bar PNGs>] [-NoArt]

param(
    [string]$Version = '',
    [switch]$SkipBuild,
    [string]$OutDir = '',
    [string]$ArtDir = '',
    [string]$BarArtDir = '',
    [switch]$NoArt
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$proj  = $FfxvrRepo
$modId = 'ffxvr'
if (-not $OutDir) { $OutDir = Join-Path $proj 'dist' }
$OutDir = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { Join-Path (Get-Location).Path $OutDir }))

function Fail($msg) { Write-Host ''; Write-Host "ERROR: $msg"; Write-Host 'No package made.'; exit 1 }

# Git is optional here. Run it with errors relaxed: in Windows PowerShell 5.1 a redirected stderr
# line (even 2>$null) is an error record, and under ErrorActionPreference Stop the first one
# ("not a git repository", "dubious ownership") would end the script. $null when git fails.
function Invoke-GitQuiet {
    $ErrorActionPreference = 'Continue'
    $out = & git @args 2>$null
    if ($LASTEXITCODE -ne 0) { return $null }
    return $out
}

# ---------------------------------------------------------------- build
if (-not $SkipBuild) {
    Write-Host '[build] tools\build-mod.ps1 -NoDeploy'
    $host_exe = (Get-Process -Id $PID).Path
    & $host_exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build-mod.ps1') -NoDeploy
    if ($LASTEXITCODE -ne 0) { Fail 'the build failed (see the log above).' }
    Write-Host ''
}

# ---------------------------------------------------------------- build outputs
$natDll = Join-Path $proj 'src\native\bin\Win32\Release\ffxvr_native.dll'
if (-not (Test-Path $natDll)) { Fail "ffxvr_native.dll not found at $natDll. Build first." }

# The managed output: the newest ffxvr.dll with a manifest beside it, never a Debug build (one
# from Visual Studio can be newer). The Debug test runs on the path below src\managed.
$srcMgd = Join-Path $proj 'src\managed'
$mgd = @(Get-ChildItem -LiteralPath $srcMgd -Recurse -File -Filter "$modId.dll" -ErrorAction SilentlyContinue |
         Where-Object { $r = $_.FullName.Substring($srcMgd.Length)
                        $r -match '\\(bin|artifacts)\\' -and $r -notmatch '\\(debug|dbg)(_[^\\]*)?\\' -and
                        (Test-Path -LiteralPath (Join-Path $_.DirectoryName "$modId.manifest.json")) } |
         Sort-Object LastWriteTime -Descending)
if ($mgd.Count -eq 0) { Fail "no Release $modId.dll with a manifest under src\managed. Build first." }
$mgdDir = $mgd[0].DirectoryName
Write-Host "Mod files from: $mgdDir"
if ($mgd.Count -gt 1) { Write-Host ('  (also found, older: ' + (($mgd | Select-Object -Skip 1 | ForEach-Object { $_.DirectoryName }) -join '; ') + ')') }
$mgdFiles = @("$modId.dll", "$modId.deps.json", "$modId.runtimeconfig.json", "$modId.manifest.json")
foreach ($f in $mgdFiles) {
    if (-not (Test-Path (Join-Path $mgdDir $f))) { Fail "$f missing from $mgdDir" }
}

$oxrDll = Get-ChildItem (Join-Path $proj '_ext\openxr\loader') -Recurse -Filter 'openxr_loader.dll' -ErrorAction SilentlyContinue |
          Where-Object { $_.FullName -match '\\Win32\\release\\bin\\' } | Select-Object -First 1
if (-not $oxrDll) { Fail 'no 32-bit openxr_loader.dll under _ext\openxr\loader. Run tools\build-mod.ps1 once.' }

# With -SkipBuild, make sure the binaries are not older than the source.
if ($SkipBuild) {
    $newestSrc = Get-ChildItem (Join-Path $proj 'src') -Recurse -File -Include *.cpp, *.h, *.cs, *.csproj, *.vcxproj |
                 Where-Object { $_.FullName -notmatch '\\(bin|obj)\\' } |
                 Sort-Object LastWriteTime -Descending | Select-Object -First 1
    $oldestBin = @((Get-Item $natDll), (Get-Item (Join-Path $mgdDir "$modId.dll"))) | Sort-Object LastWriteTime | Select-Object -First 1
    if ($newestSrc -and $newestSrc.LastWriteTime -gt $oldestBin.LastWriteTime) {
        Fail ("$($newestSrc.Name) is newer than $($oldestBin.Name): the build is stale. Run without -SkipBuild.")
    }
}

# ---------------------------------------------------------------- version
$csproj = Join-Path $proj 'src\managed\ffxvr.csproj'
$csVer  = ([xml](Get-Content $csproj -Raw)).Project.PropertyGroup | ForEach-Object { $_.Version } |
          Where-Object { $_ } | Select-Object -First 1
if (-not $csVer) { $csVer = '0.0.0' }

$commit = 'unknown'; $short = 'nogit'; $dirty = $false
if (Get-Command git -ErrorAction SilentlyContinue) {
    $c = Invoke-GitQuiet -C $proj rev-parse HEAD
    if ($c) {
        $commit = "$c".Trim(); $short = $commit.Substring(0, 7)
        $dirty = [bool](Invoke-GitQuiet -C $proj status --porcelain --untracked-files=no)
    }
}
if ($Version) {
    $label = $Version
    if ($Version -ne $csVer) { Write-Host "WARNING: -Version $Version is not ffxvr.csproj's <Version> ${csVer}: the overlay and the logs will say $csVer." }
} else { $label = "$csVer-$short" }
if ($dirty) {
    $label += '-dirty'
    Write-Host 'WARNING: uncommitted changes. The zip is labelled -dirty; commit first for a real release.'
}
$label = $label -replace '[^A-Za-z0-9._+-]', '-'

# The Fahrenheit release this build is meant for (tools\paths.ps1), and a warning when the packages
# it was compiled against did not come from nuget.org (a local Fahrenheit build shares the version).
$fhCommit  = $FfxvrFhCommit
$fhRelease = "v$FfxvrFhVersion"
$fhSdkVer  = ((([xml](Get-Content -LiteralPath $csproj -Raw)).Project.Sdk) -split '/')[1]
if ($fhSdkVer -ne $FfxvrFhVersion) { Fail "ffxvr.csproj builds against Fahrenheit.Sdk '$fhSdkVer', tools\paths.ps1 says $FfxvrFhVersion." }
foreach ($id in 'fahrenheit.sdk', 'fahrenheit') {
    $from = $null
    try { $from = (Get-Content -LiteralPath (Join-Path $proj "_ext\nuget\$id\$FfxvrFhVersion\.nupkg.metadata") -Raw -ErrorAction Stop | ConvertFrom-Json).source } catch { }
    if ($from -notmatch 'api\.nuget\.org') {
        Write-Host "WARNING: $id $FfxvrFhVersion in _ext\nuget came from '$from', not nuget.org (see build-mod's log)."
        Write-Host "         The README will still name release $fhRelease."
    }
}

# ---------------------------------------------------------------- stage
$name  = "ffx-vr-$label"
$stage = Join-Path $OutDir '_stage'
$root  = Join-Path $stage $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$modDst   = Join-Path $root "mod\$modId"
$toolsDst = Join-Path $root 'tools'
New-Item -ItemType Directory -Force -Path $modDst, $toolsDst | Out-Null

foreach ($f in $mgdFiles) { Copy-Item (Join-Path $mgdDir $f) -Destination $modDst }
Copy-Item $natDll          -Destination $modDst
Copy-Item $oxrDll.FullName -Destination $modDst
# VR hands: only the shipped hand packs, from the repository (never from a game folder, where a user's own packs live).
foreach ($pk in $FfxvrHandPacks) {
    $from = Join-Path $proj "assets\hands\$pk"
    foreach ($f in 'hand.json', 'right.glb') { if (-not (Test-Path -LiteralPath (Join-Path $from $f))) { Fail "assets\hands\$pk\$f is missing (tools\hands\make-generic-hand.py writes it)" } }
    $to = Join-Path $modDst "hands\$pk"
    New-Item -ItemType Directory -Force -Path $to | Out-Null
    Get-ChildItem -LiteralPath $from -File | Copy-Item -Destination $to
}
# Guard: under hands\ only the allowlisted folders, and nothing marked private (a pack made from a user's own model, often
# game-derived, must never ship: tools\hands\ marks those with "private": true and PRIVATE-do-not-share.txt).
$handsDst = Join-Path $modDst 'hands'
foreach ($f in @(Get-ChildItem -LiteralPath $handsDst -Recurse -File -ErrorAction SilentlyContinue)) {
    $rel = $f.FullName.Substring($handsDst.Length + 1); $top = ($rel -split '[\\/]')[0]
    if ($FfxvrHandPacks -notcontains $top) { Fail "hands\$rel is not one of the shipped hand packs ($($FfxvrHandPacks -join ', '))" }
    if ($f.Name -eq 'PRIVATE-do-not-share.txt' -or $f.Name -like '*.private.glb') { Fail "hands\$rel is marked private" }
    if ($f.Name -eq 'hand.json' -and ([IO.File]::ReadAllText($f.FullName) -match '"private"\s*:\s*true')) { Fail "hands\$rel says it is private" }
}

foreach ($f in 'paths.ps1', 'run-ffx.ps1', 'game-settings.ps1', 'import-steam-saves.ps1',
               'export-save-to-steam.ps1', 'install-texture-pack.ps1', 'set-logging.ps1') {
    $p = Join-Path $PSScriptRoot $f
    if (-not (Test-Path $p)) { Fail "tools\$f is missing" }
    Copy-Item $p -Destination $toolsDst
}
Copy-Item (Join-Path $proj 'packaging\install-mod.ps1') -Destination $toolsDst
# VR hands S3: the importer for a player's own model (the scripts only, by name: never a pack, a model or a cache).
Copy-Item (Join-Path $PSScriptRoot 'import-hands.ps1') -Destination $toolsDst
New-Item -ItemType Directory -Force -Path (Join-Path $toolsDst 'hands') | Out-Null
foreach ($f in 'import-hands.py', 'objio.py', 'handlib.py', 'preview-pack.py') {
    $p = Join-Path $PSScriptRoot "hands\$f"
    if (-not (Test-Path -LiteralPath $p)) { Fail "tools\hands\$f is missing" }
    Copy-Item -LiteralPath $p -Destination (Join-Path $toolsDst 'hands')
}
# Guard over the whole staged tree: no private marker anywhere, and no .glb outside the shipped hand packs.
foreach ($f in @(Get-ChildItem -LiteralPath $root -Recurse -File)) {
    $rel = $f.FullName.Substring($root.Length + 1)
    if ($f.Name -eq 'PRIVATE-do-not-share.txt' -or $f.Name -like '*.private.glb') { Fail "$rel is marked private" }
    if ($f.Extension -eq '.glb' -and -not $f.FullName.StartsWith($handsDst + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { Fail "${rel}: a .glb outside mod\$modId\hands\" }
    # FMV depth (tools\paths.ps1 $FfxvrFmvDepthDir) is made on a player's PC from the game's videos and must never ship:
    # no .fvd, no fmvdepth\ folder (the depth or its previews), no video. (The converter, tools\fmvdepth\, is not in the
    # player zip yet either; when it is, copy its .py files by name and allow exactly those here.)
    if (@($rel -split '[\\/]') -contains $FfxvrFmvDepthDir) { Fail "${rel}: under a $FfxvrFmvDepthDir\ folder (FMV depth or previews made from the game's videos)" }
    if (@('.fvd', '.part', '.mp4', '.webm', '.bik', '.npz', '.exr') -contains $f.Extension.ToLowerInvariant()) { Fail "${rel}: FMV depth, a preview or a video does not belong in the zip" }
    # The Bushido target art pack (<mod>\art\targets\, assets\art\) is the owner's own AI-generated art: never in the zip.
    if (@($rel -split '[\\/]') -contains 'art') { Fail "${rel}: under an art\ folder (the local target art pack is never shipped)" }
}

foreach ($f in 'LICENSE', 'THIRD-PARTY-NOTICES') {
    $p = Join-Path $proj $f
    if (-not (Test-Path $p)) { Fail "$f is missing from the repository root" }
    Copy-Item $p -Destination $root
}

$readme = [IO.File]::ReadAllText((Join-Path $proj 'packaging\README-zip.md'))
$readme = $readme.Replace('{VERSION}', $label).Replace('{COMMIT}', $short).
                  Replace('{DATE}', (Get-Date -Format 'yyyy-MM-dd')).Replace('{FAHRENHEIT_COMMIT}', $fhCommit).
                  Replace('{FAHRENHEIT_RELEASE}', $fhRelease)
# In the repository the README sits in packaging\ and shows the diagrams as ../docs/img/...; in the zip it is at the root.
$readme = $readme.Replace('](../docs/img/', '](docs/img/')
[IO.File]::WriteAllText((Join-Path $root 'README.md'), $readme, (New-Object Text.UTF8Encoding $false))

# The controller diagrams (docs\img\, the PNGs the README shows) and the tester checklist (as TESTING.md at the root; in the
# repository it sits in docs\ and shows the diagrams as img/...).
$imgDst = Join-Path $root 'docs\img'
New-Item -ItemType Directory -Force -Path $imgDst | Out-Null
foreach ($f in 'controls.png', 'controls-chords.png') {
    $p = Join-Path $proj "docs\img\$f"
    if (-not (Test-Path -LiteralPath $p)) { Fail "docs\img\$f is missing" }
    Copy-Item -LiteralPath $p -Destination $imgDst
}
$checklist = Join-Path $proj 'docs\tester-checklist.md'
if (-not (Test-Path -LiteralPath $checklist)) { Fail 'docs\tester-checklist.md is missing' }
$testing = [IO.File]::ReadAllText($checklist).Replace('](img/', '](docs/img/')
[IO.File]::WriteAllText((Join-Path $root 'TESTING.md'), $testing, (New-Object Text.UTF8Encoding $false))

# A personal folder baked into a binary (a debug-info path such as C:\Users\<name>\...) would
# ship with it. Repository-relative build paths are fine; user-profile ones are worth a look.
foreach ($b in Get-ChildItem $modDst -File -Filter *.dll) {
    $bytes = [IO.File]::ReadAllBytes($b.FullName)
    $text  = [Text.Encoding]::ASCII.GetString($bytes) + [Text.Encoding]::Unicode.GetString($bytes)
    $hits  = [regex]::Matches($text, '[A-Za-z]:\\Users\\[^\\\x00]{1,40}\\') | ForEach-Object { $_.Value } | Sort-Object -Unique
    if ($hits) { Write-Host ("WARNING: {0} contains {1}" -f $b.Name, ($hits -join ', ')) }
}

# ---------------------------------------------------------------- zip
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($root, $zip, [IO.Compression.CompressionLevel]::Optimal, $true)
Remove-Item $stage -Recurse -Force

$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Set-Content -Path "$zip.sha256" -Value "$hash  $name.zip" -Encoding ascii

# ---------------------------------------------------------------- report
Write-Host ''
Write-Host ('=' * 70)
Write-Host "Package: $zip"
Write-Host ('  {0:N0} KB   sha256 {1}' -f ((Get-Item $zip).Length / 1KB), $hash)
Write-Host "  version $label   commit $short   Fahrenheit $($fhCommit.Substring(0, [Math]::Min(7, $fhCommit.Length)))"
Write-Host ''
$z = [IO.Compression.ZipFile]::OpenRead($zip)
try { $z.Entries | Sort-Object FullName | ForEach-Object { Write-Host ('  {0,10:N0}  {1}' -f $_.Length, $_.FullName) } }
finally { $z.Dispose() }
Write-Host ''
Write-Host 'Players: extract it anywhere, then follow README.md (tools\install-mod.ps1).'

# ---------------------------------------------------------------- the optional target art zip (a separate download)
# The target art packs (the Bushido targets, the overdrive bar) are the owner's own AI-generated art: never in the repository,
# never in the mod's zip (the guard above fails on any art\ folder there). This step only packs them as their own optional zip,
# each pack when it is complete. Anything wrong: a note, never a failure.
$artFiles = @('frame.png', 'glow.png', 'halo.png', 'swirl.png', 'back.png', 'edge.png', 'burst.png', 'sparks.png')
$artNotice = 'Generated with Google Gemini and upscaled with Upscayl by the project owner; AI-generated; no rights claimed, free to use. Optional: without it the mod draws plain targets.'
$barFiles = @('frame.png', 'track_fill.png', 'track_empty.png', 'plate.png', 'zone.png', 'cursor_down.png', 'cursor_up.png')   # (tools\target-art\bar_art.py's output)
$barNotice = 'Generated with AI by the project owner (a sprite sheet, cut with tools\target-art\bar_art.py); AI-generated; no rights claimed, free to use. Optional: without it the mod draws its plain overdrive bar.'
# The sizes the mod accepts (width, smallest and largest height): ffxvr_odtarget.h odt::art::size_bad (1024 x 1024; the edge
# strip 1024 x 32..512) and ffxvr_odbar.h odb::cell. A pack the mod would refuse is not packed (review S3).
$artSizes = @{ 'frame.png' = @(1024, 1024, 1024); 'glow.png' = @(1024, 1024, 1024); 'halo.png' = @(1024, 1024, 1024); 'swirl.png' = @(1024, 1024, 1024);
               'back.png' = @(1024, 1024, 1024); 'edge.png' = @(1024, 32, 512); 'burst.png' = @(1024, 1024, 1024); 'sparks.png' = @(1024, 1024, 1024) }
$barSizes = @{ 'frame.png' = @(1024, 112, 112); 'track_fill.png' = @(1024, 96, 96); 'track_empty.png' = @(1024, 96, 96); 'plate.png' = @(1024, 160, 160);
               'zone.png' = @(512, 224, 224); 'cursor_down.png' = @(256, 208, 208); 'cursor_up.png' = @(256, 208, 208) }
# A PNG's width and height from its IHDR (bytes 16..23, big-endian); $null when it is not a PNG.
function Get-PngSize([string]$Path) {
    $b = New-Object byte[] 24
    $fs = [IO.File]::OpenRead($Path)
    try { $n = $fs.Read($b, 0, 24) } finally { $fs.Dispose() }
    if ($n -ne 24 -or $b[0] -ne 0x89 -or $b[1] -ne 0x50 -or $b[2] -ne 0x4E -or $b[3] -ne 0x47 -or $b[12] -ne 0x49 -or $b[13] -ne 0x48 -or $b[14] -ne 0x44 -or $b[15] -ne 0x52) { return $null }
    $w = ([int]$b[16] -shl 24) -bor ([int]$b[17] -shl 16) -bor ([int]$b[18] -shl 8) -bor [int]$b[19]
    $h = ([int]$b[20] -shl 24) -bor ([int]$b[21] -shl 16) -bor ([int]$b[22] -shl 8) -bor [int]$b[23]
    return @($w, $h)
}
# What keeps a pack from the zip: each file missing, not a PNG, or of a size the mod refuses ("" none).
function Get-ArtProblems([string]$Dir, [string[]]$Files, [hashtable]$Sizes) {
    $p = New-Object System.Collections.Generic.List[string]
    foreach ($f in $Files) {
        $path = Join-Path $Dir $f
        if (-not (Test-Path -LiteralPath $path)) { $p.Add("$f missing"); continue }
        $sz = Get-PngSize $path
        $r = $Sizes[$f]
        if ($null -eq $sz) { $p.Add("$f is not a PNG"); continue }
        if ($sz[0] -ne $r[0] -or $sz[1] -lt $r[1] -or $sz[1] -gt $r[2]) {
            $want = if ($r[1] -eq $r[2]) { "$($r[0])x$($r[1])" } else { "$($r[0])x$($r[1])..$($r[2])" }
            $p.Add(("{0} is {1}x{2}, not {3}" -f $f, $sz[0], $sz[1], $want))
        }
    }
    return $p.ToArray()
}
Write-Host ''
if ($NoArt) {
    Write-Host 'Target art zip: skipped (-NoArt).'
} else {
    try {
        $artSrc = $ArtDir
        $barSrc = $BarArtDir
        if (-not $artSrc -or -not $barSrc) {
            $g = Find-FfxvrGame
            if ($g) {
                if (-not $artSrc) { $artSrc = [IO.Path]::Combine($g, 'fahrenheit', 'mods', $modId, 'art', 'targets') }
                if (-not $barSrc) { $barSrc = [IO.Path]::Combine($g, 'fahrenheit', 'mods', $modId, 'art', 'odbar') }
            }
        }
        $missing = @()
        if ($artSrc) { $missing = @(Get-ArtProblems $artSrc $artFiles $artSizes) }
        $barMissing = @()
        if ($barSrc) { $barMissing = @(Get-ArtProblems $barSrc $barFiles $barSizes) }
        $haveTargets = [bool]$artSrc -and $missing.Count -eq 0
        $haveBar = [bool]$barSrc -and $barMissing.Count -eq 0
        if (-not $artSrc) {
            Write-Host 'Target art zip: the Bushido targets left out (no game folder found and no -ArtDir; the mod zip does not need them).'
        } elseif (-not $haveTargets) {
            Write-Host ("Target art zip: the Bushido targets left out (in {0}: {1}; the mod would refuse them; the mod zip does not need them)." -f $artSrc, ($missing -join ', '))
        }
        if (-not $barSrc) {
            Write-Host 'Target art zip: the overdrive bar left out (no game folder found and no -BarArtDir; the mod zip does not need it).'
        } elseif (-not $haveBar) {
            Write-Host ("Target art zip: the overdrive bar left out (in {0}: {1}; the mod would refuse it; cut it again with tools\target-art\bar_art.py; the mod zip does not need it)." -f $barSrc, ($barMissing -join ', '))
        }
        if (-not $haveTargets -and -not $haveBar) {
            Write-Host 'Target art zip: skipped (no complete pack; the mod zip does not need it).'
        } else {
            $artName  = "ffxvr-target-art-$label"
            $artStage = Join-Path $OutDir '_stage_art'
            if (Test-Path -LiteralPath $artStage) { Remove-Item -LiteralPath $artStage -Recurse -Force }
            if ($haveTargets) {
                $artTo = [IO.Path]::Combine($artStage, $name, 'mod', $modId, 'art', 'targets')   # the mod zip's layout (install-mod copies it)
                New-Item -ItemType Directory -Force -Path $artTo | Out-Null
                foreach ($f in $artFiles) { Copy-Item -LiteralPath (Join-Path $artSrc $f) -Destination $artTo -Force }
                $artArrow = Join-Path $artSrc 'arrow.png'   # optional: the pack's own arrow (without it the mod draws its procedural arrow)
                if (Test-Path -LiteralPath $artArrow) {
                    $as = Get-PngSize $artArrow
                    if ($null -ne $as -and $as[0] -eq 1024 -and $as[1] -eq 1024) { Copy-Item -LiteralPath $artArrow -Destination $artTo -Force }
                    else { Write-Host 'Target art zip: arrow.png left out (not a 1024 x 1024 PNG: the mod would draw its own arrow).' }
                }
                [IO.File]::WriteAllText((Join-Path $artTo 'ART-NOTICE.txt'), $artNotice + "`r`n", (New-Object Text.UTF8Encoding $false))
            }
            if ($haveBar) {
                $barTo = [IO.Path]::Combine($artStage, $name, 'mod', $modId, 'art', 'odbar')   # the same layout (install-mod copies it)
                New-Item -ItemType Directory -Force -Path $barTo | Out-Null
                foreach ($f in $barFiles) { Copy-Item -LiteralPath (Join-Path $barSrc $f) -Destination $barTo -Force }
                [IO.File]::WriteAllText((Join-Path $barTo 'ART-NOTICE.txt'), $barNotice + "`r`n", (New-Object Text.UTF8Encoding $false))
            }
            $artZip = Join-Path $OutDir "$artName.zip"
            if (Test-Path -LiteralPath $artZip) { Remove-Item -LiteralPath $artZip -Force }
            [IO.Compression.ZipFile]::CreateFromDirectory($artStage, $artZip, [IO.Compression.CompressionLevel]::Optimal, $false)
            Remove-Item -LiteralPath $artStage -Recurse -Force
            $artHash = (Get-FileHash $artZip -Algorithm SHA256).Hash.ToLower()
            Set-Content -Path "$artZip.sha256" -Value "$artHash  $artName.zip" -Encoding ascii
            Write-Host "Target art zip (optional, a separate download): $artZip"
            Write-Host ('  {0:N0} KB   sha256 {1}   the Bushido targets: {2}   the overdrive bar: {3}' -f ((Get-Item $artZip).Length / 1KB), $artHash, $(if ($haveTargets) { $artSrc } else { 'left out' }), $(if ($haveBar) { $barSrc } else { 'left out' }))
            $za = [IO.Compression.ZipFile]::OpenRead($artZip)
            try { $za.Entries | Sort-Object FullName | ForEach-Object { Write-Host ('  {0,10:N0}  {1}' -f $_.Length, $_.FullName) } }
            finally { $za.Dispose() }
            Write-Host '  AI-generated art: tick the AI disclosure where it is hosted (docs\release.md).'
        }
    } catch {
        Write-Host "Target art zip: skipped ($($_.Exception.Message)); the mod zip is made."
    }
}
