# build-fahrenheit-full.ps1
#
# Full source build of Fahrenheit: both the managed (.NET) components AND the
# native stage0/stage1 loaders. This is what we need, because the loaders changed
# after release alpha11 (commits #306 and #342) and the alpha11 loaders are not
# compatible with current managed code.
#
# Requires: Visual Studio Build Tools 2026 with the C++ workload, and vcpkg
# integration enabled once via:   vcpkg integrate install
#
# This script does NOT touch the game folder. It builds and reports only.
# Log is written as UTF-8 to C:\projects\FFX-VR\logs.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File C:\projects\FFX-VR\tools\build-fahrenheit-full.ps1

$ErrorActionPreference = 'Continue'

$proj   = 'C:\projects\FFX-VR'
$repo   = Join-Path $proj '_ext\fahrenheit'
$logDir = Join-Path $proj 'logs'
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$log    = Join-Path $logDir "build-full-$stamp.txt"

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

# UTF-8 logging (PowerShell 5.1's Tee-Object writes UTF-16, which is awkward to read)
function Log($s) {
    Write-Host $s
    $s | Out-File -FilePath $log -Encoding utf8 -Append
}

Log "Fahrenheit FULL build - $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Log ('=' * 70)

# ---------------------------------------------------------------- locate toolchain
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    Log 'ERROR: vswhere.exe not found. Visual Studio Build Tools are not installed.'
    exit 1
}

Log ''
Log '[toolchain] Visual Studio installations with the C++ workload:'

# Enumerate all installs rather than guessing a version number for 2026
$installs = & $vswhere -all -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -format value -property installationPath 2>$null

$best = $null
$bestVer = [version]'0.0'
foreach ($p in $installs) {
    if (-not $p) { continue }
    $v = & $vswhere -path $p -format value -property installationVersion 2>$null
    $name = & $vswhere -path $p -format value -property displayName 2>$null
    Log "  $name  ($v)"
    Log "      $p"
    try { $ver = [version]($v -split '\+')[0] } catch { $ver = [version]'0.0' }
    if ($ver -gt $bestVer) { $bestVer = $ver; $best = $p }
}

if (-not $best) {
    Log ''
    Log 'ERROR: no Visual Studio installation has the C++ workload.'
    Log '  Install "Desktop development with C++" in the Visual Studio Installer.'
    exit 1
}

Log ''
Log "[toolchain] using: $best  (version $bestVer)"
if ($bestVer.Major -lt 18) {
    Log '  WARNING: Fahrenheit BUILD.md asks for Visual Studio 2026.'
    Log '           This looks older, so the native loaders may fail to build.'
}

$msbuild = & $vswhere -path $best -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1
if (-not $msbuild) {
    Log 'ERROR: MSBuild.exe not found in that installation.'
    exit 1
}
Log "[toolchain] msbuild: $msbuild"

# ---------------------------------------------------------------- update source
if (-not (Test-Path (Join-Path $repo 'Fahrenheit.slnx'))) {
    Log ''
    Log "ERROR: Fahrenheit source not found at $repo"
    Log '  Run build-fahrenheit.ps1 first to clone it.'
    exit 1
}

Log ''
Log '[git] updating to latest main'
& git -C $repo fetch origin main 2>&1 | ForEach-Object { Log "  $_" }
& git -C $repo reset --hard origin/main 2>&1 | ForEach-Object { Log "  $_" }
& git -C $repo log -1 --pretty='  building commit %h  (%ad)  %s' --date=short 2>&1 | ForEach-Object { Log $_ }

# ---------------------------------------------------------------- build
Push-Location $repo
try {
    Log ''
    Log '[msbuild] restore'
    & $msbuild .\Fahrenheit.slnx /t:Restore /p:Configuration=Release /v:minimal 2>&1 |
        ForEach-Object { Log "  $_" }
    $restoreCode = $LASTEXITCODE
    Log "[msbuild] restore exit code: $restoreCode"

    Log ''
    Log '[msbuild] build'
    & $msbuild .\Fahrenheit.slnx /p:Configuration=Release /v:minimal 2>&1 |
        ForEach-Object { Log "  $_" }
    $buildCode = $LASTEXITCODE
    Log "[msbuild] build exit code: $buildCode"
}
finally {
    Pop-Location
}

# ---------------------------------------------------------------- results
$deploy = Join-Path $repo 'artifacts\deploy\rel'
Log ''
Log '[artifacts] native loaders (the whole point of this build):'
foreach ($f in 'bin\fhstage0.exe', 'bin\fhstage1.dll', 'bin\minhook.x32.dll', 'bin\nethost.dll') {
    $p = Join-Path $deploy $f
    if (Test-Path $p) {
        $i = Get-Item $p
        Log ("  OK       $f  ($($i.Length) bytes, built $($i.LastWriteTime))")
    } else {
        Log ("  MISSING  $f")
    }
}

Log ''
Log '[artifacts] total files produced:'
if (Test-Path $deploy) {
    $all = Get-ChildItem $deploy -Recurse -File
    Log ("  $($all.Count) files, $([math]::Round(($all | Measure-Object Length -Sum).Sum / 1MB, 1)) MB")
    Log "  at $deploy"
} else {
    Log "  nothing at $deploy"
}

Log ''
Log ('=' * 70)
Log "Log: $log"
