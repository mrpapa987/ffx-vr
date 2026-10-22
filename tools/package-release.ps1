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
#                              install-texture-pack.ps1
#   dist\ffx-vr-<label>.zip.sha256
#
# Fahrenheit and FFX-60fps are NOT included: they are separate projects with their own licences
# and releases (see docs\release.md). No settings, logs, captures or .pdb files are included, and
# nothing is taken from the game folder: everything comes from this repository's build output.
#
# <label> is -Version if given, otherwise the csproj <Version> plus the git commit, e.g.
# 0.1.0-dev-b993ffc ("-dirty" is added when there are uncommitted changes).
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

$manifest = Get-ChildItem (Join-Path $proj 'src\managed') -Recurse -File -Filter "$modId.manifest.json" -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match '\\(bin|artifacts)\\' } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (-not $manifest) { Fail "no $modId.manifest.json under src\managed. Build first." }
$mgdDir = $manifest.DirectoryName
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
    $c = & git -C $proj rev-parse HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $c) {
        $commit = $c.Trim(); $short = $commit.Substring(0, 7)
        $dirty = [bool](& git -C $proj status --porcelain --untracked-files=no 2>$null)
    }
}
if ($Version) { $label = $Version } else { $label = "$csVer-$short" }
if ($dirty) {
    $label += '-dirty'
    Write-Host 'WARNING: uncommitted changes. The zip is labelled -dirty; commit first for a real release.'
}
$label = $label -replace '[^A-Za-z0-9._+-]', '-'

# The Fahrenheit commit this build is meant for (the default of build-fahrenheit-full.ps1), and
# a warning when the SDK in _ext\fahrenheit was built from something else.
$fhCommit = 'unknown'
$m = Select-String -Path (Join-Path $PSScriptRoot 'build-fahrenheit-full.ps1') -Pattern "\`$Commit\s*=\s*'([0-9a-f]{40})'" | Select-Object -First 1
if ($m) { $fhCommit = $m.Matches[0].Groups[1].Value }
$fhDir = Join-Path $proj '_ext\fahrenheit'
if ((Test-Path (Join-Path $fhDir '.git')) -and (Get-Command git -ErrorAction SilentlyContinue)) {
    $built = (& git -C $fhDir rev-parse HEAD 2>$null)
    if ($built -and $fhCommit -ne 'unknown' -and $built.Trim() -ne $fhCommit) {
        Write-Host "WARNING: _ext\fahrenheit is at $($built.Trim().Substring(0,7)), not the pinned $($fhCommit.Substring(0,7))."
        Write-Host '         The mod was built against that SDK; the README will still name the pinned commit.'
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
               'export-save-to-steam.ps1', 'install-texture-pack.ps1') {
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
                  Replace('{DATE}', (Get-Date -Format 'yyyy-MM-dd')).Replace('{FAHRENHEIT_COMMIT}', $fhCommit)
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
