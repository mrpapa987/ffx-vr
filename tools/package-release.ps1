# package-release.ps1
#
# Builds the mod and packs a release zip for players, who install it without building anything:
#
#   dist\ffx-vr-<label>.zip
#     ffx-vr-<label>\
#       README.md              packaging\README-zip.md with the version filled in
#       LICENSE, THIRD-PARTY-NOTICES
#       mod\ffxvr\             ffxvr.dll (+ deps/runtimeconfig/manifest json), ffxvr_native.dll,
#                              openxr_loader.dll
#       tools\                 install-mod.ps1 (from packaging\), paths.ps1, run-ffx.ps1,
#                              game-settings.ps1, import-steam-saves.ps1, export-save-to-steam.ps1,
#                              install-texture-pack.ps1, set-logging.ps1
#   dist\ffx-vr-<label>.zip.sha256
#
# Fahrenheit and FFX-60fps are NOT included: they are separate projects with their own licences
# and releases (see docs\release.md). No settings, logs, captures or .pdb files are included, and
# nothing is taken from the game folder: everything comes from this repository's build output.
#
# <label> is -Version if given, otherwise the csproj <Version> plus the git commit, e.g.
# 0.1.0-alpha.1-b993ffc ("-dirty" is added when there are uncommitted changes). For the release itself pass
# -Version equal to the csproj <Version> (e.g. -Version 0.1.0-alpha.1), so the zip is named like the tag v0.1.0-alpha.1.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\package-release.ps1 [-Version 0.1.0-alpha1] [-SkipBuild] [-OutDir <folder>]

param(
    [string]$Version = '',
    [switch]$SkipBuild,
    [string]$OutDir = ''
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

foreach ($f in 'paths.ps1', 'run-ffx.ps1', 'game-settings.ps1', 'import-steam-saves.ps1',
               'export-save-to-steam.ps1', 'install-texture-pack.ps1', 'set-logging.ps1') {
    $p = Join-Path $PSScriptRoot $f
    if (-not (Test-Path $p)) { Fail "tools\$f is missing" }
    Copy-Item $p -Destination $toolsDst
}
Copy-Item (Join-Path $proj 'packaging\install-mod.ps1') -Destination $toolsDst

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
