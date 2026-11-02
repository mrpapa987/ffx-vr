# build-fahrenheit-full.ps1
#
# Builds Fahrenheit from source (the managed components and the native stage0/stage1 loaders), by
# default at the release FFX VR is tested with ($FfxvrFhCommit in tools\paths.ps1, tag
# v1.0.0-alpha12). Players and most developers don't need this: install the official release zip
# with  tools\install-fahrenheit.ps1 -Zip <zip>  (INSTALL-ALPHA step 4). Use this for another
# commit (-Commit, -Latest), or when the release zip is not an option.
#
# Requires: Visual Studio Build Tools 2026 with the C++ workload, and vcpkg
# integration enabled once via:   vcpkg integrate install
#
# This script does NOT touch the game folder. It builds and reports only.
# Log is written as UTF-8 to logs\ in this repository.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\build-fahrenheit-full.ps1     (from the repository folder)
#   ... -Latest            build the newest main instead of the tested release (untested with FFX VR)
#   ... -Commit <hash|tag> build a specific commit or tag, e.g. -Commit v1.0.0-alpha12
# Then tools\install-fahrenheit.ps1 copies the result into the game folder.

param(
    [string]$Commit = '',      # empty: the tested release, $FfxvrFhCommit in tools\paths.ps1
    [switch]$Latest
)
$ErrorActionPreference = 'Continue'

. (Join-Path $PSScriptRoot 'paths.ps1')
if (-not $Commit) { $Commit = $FfxvrFhCommit }   # the tested release, tools\paths.ps1
$proj   = $FfxvrRepo
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
    Log "[git] cloning Fahrenheit into $repo"
    New-Item -ItemType Directory -Force -Path (Split-Path $repo) | Out-Null
    & git clone https://github.com/fahrenheit-crew/fahrenheit $repo 2>&1 | ForEach-Object { Log "  $_" }
    if (-not (Test-Path (Join-Path $repo 'Fahrenheit.slnx'))) { Log 'ERROR: clone failed'; exit 1 }
}

Log ''
& git -C $repo fetch --tags origin main 2>&1 | ForEach-Object { Log "  $_" }
if ($Latest) {
    Log '[git] building the latest main (untested with FFX VR)'
    & git -C $repo reset --hard origin/main 2>&1 | ForEach-Object { Log "  $_" }
} else {
    Log "[git] building commit $Commit"
    & git -C $repo reset --hard $Commit 2>&1 | ForEach-Object { Log "  $_" }
}
& git -C $repo submodule update --init --recursive 2>&1 | ForEach-Object { Log "  $_" }
& git -C $repo log -1 --pretty='  building commit %h  (%ad)  %s' --date=short 2>&1 | ForEach-Object { Log $_ }
# A failed reset (commit not fetched, a lock file) would otherwise build whatever is checked out.
# rev-parse resolves an abbreviated or upper-case hash, or a tag, to the full commit.
$head = "$(& git -C $repo rev-parse HEAD 2>$null)".Trim()
if (-not $Latest) {
    $want = "$(& git -C $repo rev-parse --verify --quiet "$Commit^{commit}" 2>$null)".Trim()
    if (-not $want -or $head -ne $want) {
        Log ''
        Log "ERROR: _ext\fahrenheit is at '$head', not $Commit (see the git output above). Nothing built."
        Log 'RESULT: FAILED'
        Log "Log: $log"
        exit 1
    }
}

# ---------------------------------------------------------------- build
# MSBuild error lines are collected so that a non-zero exit code can be told apart from the one
# known harmless failure (see $knownQuirk below).
$errLines = New-Object System.Collections.Generic.List[string]
$errRe    = '(^|[\s:])error(\s+[A-Z]+\d+)?\s*:'
Push-Location -LiteralPath $repo
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
        ForEach-Object { Log "  $_"; if ("$_" -match $errRe) { $errLines.Add("$_") } }
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
$missing = 0
foreach ($f in 'bin\fhstage0.exe', 'bin\fhstage1.dll', 'bin\minhook.x32.dll', 'bin\nethost.dll') {
    $p = Join-Path $deploy $f
    if (Test-Path -LiteralPath $p) {
        $i = Get-Item -LiteralPath $p
        Log ("  OK       $f  ($($i.Length) bytes, built $($i.LastWriteTime))")
    } else {
        Log ("  MISSING  $f")
        $missing++
    }
}

Log ''
Log '[artifacts] total files produced:'
if (Test-Path -LiteralPath $deploy) {
    $all = Get-ChildItem -LiteralPath $deploy -Recurse -File
    Log ("  $($all.Count) files, $([math]::Round(($all | Measure-Object Length -Sum).Sum / 1MB, 1)) MB")
    Log "  at $deploy"
} else {
    Log "  nothing at $deploy"
}

# ---------------------------------------------------------------- verdict
# The one known harmless failure: without the Debugging Tools for Windows, MSBuild can't copy
# dbghelp.dll, dbgcore.dll and symsrv.dll from the Windows Kits debugger folder (MSB3030 errors,
# exit code 1; Fahrenheit.Stage0.vcxproj copies all three at v1.0.0-alpha12, upstream #369 added
# dbgcore; install-fahrenheit.ps1 supplies dbghelp.dll). A non-zero exit passes only when every
# error line is one of those, so an unrecognised failure, with older loaders still in place, reads FAILED.
$knownQuirk = 'error MSB3030:.*Windows Kits\\+10\\+Debuggers\\+x86\\+(dbghelp|dbgcore|symsrv)\.dll.*not found'   # \\+: tolerates a doubled separator
$realErrors = @($errLines | Sort-Object -Unique | Where-Object { $_ -notmatch $knownQuirk })
$ok = ($restoreCode -eq 0) -and ($missing -eq 0) -and
      ($buildCode -eq 0 -or ($errLines.Count -gt 0 -and $realErrors.Count -eq 0))
Log ''
if ($buildCode -ne 0 -and $ok) {
    Log "NOTE: msbuild exit code $buildCode came only from the known dbghelp.dll/dbgcore.dll/symsrv.dll copy errors; the loaders were built."
}
if (-not $ok -and $realErrors.Count -gt 0) {
    Log 'Errors:'
    $realErrors | Select-Object -First 20 | ForEach-Object { Log "  $_" }
}
if ($restoreCode -ne 0) { Log "restore failed (exit code $restoreCode)." }
Log ('=' * 70)
if ($ok) { Log 'RESULT: OK. Next: tools\install-fahrenheit.ps1' }
else     { Log 'RESULT: FAILED. Don''t run install-fahrenheit.ps1 until this builds.' }
Log "Log: $log"
exit $(if ($ok) { 0 } else { 1 })
