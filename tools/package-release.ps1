# package-release.ps1
#
# Builds the mod and packs a release zip for players, who install it without building anything:
#
#   dist\ffx-vr-<label>.zip
#     ffx-vr-<label>\
#       README.md              packaging\README-zip.md with the version filled in
#       LICENSE, THIRD-PARTY-NOTICES
#       mod\ffxvr\             ffxvr.dll (+ deps/runtimeconfig/manifest json), ffxvr_native.dll,
#                              openxr_loader.dll, hands\generic-glove\ and hands\generic\ (our generic hand packs, MIT:
#                              an allowlist, tools\paths.ps1 $FfxvrHandPacks; a guard below fails on anything else there)
#       tools\                 install-mod.ps1 (from packaging\), paths.ps1, run-ffx.ps1, import-hands.ps1 and
#                              hands\{import-hands,objio,handlib,preview-pack}.py (VR hands: make a pack from your own model),
#                              game-settings.ps1, import-steam-saves.ps1, export-save-to-steam.ps1,
#                              install-texture-pack.ps1, set-logging.ps1
#   dist\ffx-vr-<label>.zip.sha256
#   dist\ffxvr-target-art-<label>.zip (+ .sha256)   OPTIONAL, a separate download: the Bushido target art pack (the owner's
#                              AI-generated medallion art, never in the repository or in the mod's zip), from
#                              <game>\fahrenheit\mods\ffxvr\art\targets\ or -ArtDir; it unpacks to
#                              fahrenheit\mods\ffxvr\art\targets\ (8 PNGs + ART-NOTICE.txt). No pack: skipped with a note,
#                              never a failure. -NoArt skips it.
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
#   powershell -ExecutionPolicy Bypass -File .\tools\package-release.ps1 [-Version 0.1.0-alpha1] [-SkipBuild] [-OutDir <folder>]
#              [-ArtDir <folder with the 8 PNGs>] [-NoArt]

param(
    [string]$Version = '',
    [switch]$SkipBuild,
    [string]$OutDir = '',
    [string]$ArtDir = '',
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
[IO.File]::WriteAllText((Join-Path $root 'README.md'), $readme, (New-Object Text.UTF8Encoding $false))

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
# The Bushido target art pack is the owner's own AI-generated art: never in the repository, never in the mod's zip (the guard
# above fails on any art\ folder there). This step only packs it as its own optional zip. Anything wrong: a note, never a failure.
$artFiles = @('frame.png', 'glow.png', 'halo.png', 'swirl.png', 'back.png', 'edge.png', 'burst.png', 'sparks.png')
$artNotice = 'Generated with Google Gemini and upscaled with Upscayl by the project owner; AI-generated; no rights claimed, free to use. Optional: without it the mod draws plain targets.'
Write-Host ''
if ($NoArt) {
    Write-Host 'Target art zip: skipped (-NoArt).'
} else {
    try {
        $artSrc = $ArtDir
        if (-not $artSrc) {
            $g = Find-FfxvrGame
            if ($g) { $artSrc = [IO.Path]::Combine($g, 'fahrenheit', 'mods', $modId, 'art', 'targets') }
        }
        $missing = @()
        if ($artSrc) { $missing = @($artFiles | Where-Object { -not (Test-Path -LiteralPath (Join-Path $artSrc $_)) }) }
        if (-not $artSrc) {
            Write-Host 'Target art zip: skipped (no game folder found and no -ArtDir; the mod zip does not need it).'
        } elseif ($missing.Count -gt 0) {
            Write-Host ("Target art zip: skipped (not in {0}: {1}; the mod zip does not need it)." -f $artSrc, ($missing -join ', '))
        } else {
            $artName  = "ffxvr-target-art-$label"
            $artStage = Join-Path $OutDir '_stage_art'
            $artTo    = [IO.Path]::Combine($artStage, 'fahrenheit', 'mods', $modId, 'art', 'targets')
            if (Test-Path -LiteralPath $artStage) { Remove-Item -LiteralPath $artStage -Recurse -Force }
            New-Item -ItemType Directory -Force -Path $artTo | Out-Null
            foreach ($f in $artFiles) { Copy-Item -LiteralPath (Join-Path $artSrc $f) -Destination $artTo -Force }
            [IO.File]::WriteAllText((Join-Path $artTo 'ART-NOTICE.txt'), $artNotice + "`r`n", (New-Object Text.UTF8Encoding $false))
            $artZip = Join-Path $OutDir "$artName.zip"
            if (Test-Path -LiteralPath $artZip) { Remove-Item -LiteralPath $artZip -Force }
            [IO.Compression.ZipFile]::CreateFromDirectory($artStage, $artZip, [IO.Compression.CompressionLevel]::Optimal, $false)
            Remove-Item -LiteralPath $artStage -Recurse -Force
            $artHash = (Get-FileHash $artZip -Algorithm SHA256).Hash.ToLower()
            Set-Content -Path "$artZip.sha256" -Value "$artHash  $artName.zip" -Encoding ascii
            Write-Host "Target art zip (optional, a separate download): $artZip"
            Write-Host ('  {0:N0} KB   sha256 {1}   from {2}' -f ((Get-Item $artZip).Length / 1KB), $artHash, $artSrc)
            $za = [IO.Compression.ZipFile]::OpenRead($artZip)
            try { $za.Entries | Sort-Object FullName | ForEach-Object { Write-Host ('  {0,10:N0}  {1}' -f $_.Length, $_.FullName) } }
            finally { $za.Dispose() }
            Write-Host '  AI-generated art: tick the AI disclosure where it is hosted (docs\release.md).'
        }
    } catch {
        Write-Host "Target art zip: skipped ($($_.Exception.Message)); the mod zip is made."
    }
}
