# build-fps60.ps1
#
# Builds peternguyenst/FFX-60fps from source and installs it next to FFX.exe.
# It is the frame-rate half of the VR setup: the game's logic stays at 30 Hz
# and the mod draws N frames per logic tick (fps=60/90/120...), so our hooks
# see a camera upload, and a head pose, per drawn frame.
#
#   _ext\FFX-60fps            git clone (updated on each run unless -NoUpdate)
#   artifacts\loader\Release\dbghelp.dll   -> <game>\dbghelp.dll      (proxy loader)
#   artifacts\Release\fps60_ffx.dll        -> <game>\fps60_ffx.dll
#   ship\fps60.ini                         -> <game>\fps60.ini         (only if absent: your edits are kept)
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File C:\projects\FFX-VR\tools\build-fps60.ps1 [-Fps 60] [-NoUpdate] [-Uninstall]
#
# The project is written for the VS 2026 toolset (v145). When only an older
# toolset is installed the build is retried with that one; the code is C++17.

param(
    [int]$Fps = 0,          # 0 = leave fps60.ini alone; otherwise write fps=<n> (multiple of 30, 60..240)
    [switch]$NoUpdate,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Continue'

$proj   = 'C:\projects\FFX-VR'
$ext    = Join-Path $proj '_ext\FFX-60fps'
$game   = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY FFX&FFX-2 HD Remaster'
$logDir = Join-Path $proj 'logs'
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$log    = Join-Path $logDir "build-fps60-$stamp.txt"
$repo   = 'https://github.com/peternguyenst/FFX-60fps.git'

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
function Log($m) { $m | Tee-Object -FilePath $log -Append }

if ($Uninstall) {
    foreach ($f in 'fps60_ffx.dll', 'dbghelp.dll') {
        $p = Join-Path $game $f
        if (Test-Path $p) { Remove-Item $p -Force; Log "removed $p" }
    }
    Log 'fps60.ini and fps60.log left in place.'
    exit 0
}

# ---------------------------------------------------------------- source
if (-not (Test-Path (Join-Path $ext '.git'))) {
    Log "[fps60] cloning $repo"
    New-Item -ItemType Directory -Force -Path (Split-Path $ext) | Out-Null
    & git clone --depth 1 $repo $ext 2>&1 | ForEach-Object { Log "  $_" }
    if (-not (Test-Path (Join-Path $ext '.git'))) { Log 'ERROR: clone failed'; exit 1 }
}
elseif (-not $NoUpdate) {
    Log '[fps60] updating'
    & git -C $ext pull --ff-only 2>&1 | ForEach-Object { Log "  $_" }
}
$rev = & git -C $ext log --oneline -1 2>$null
Log "[fps60] source: $rev"

# ---------------------------------------------------------------- MSBuild + toolset
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = $null; $vsPath = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format value -property installationPath 2>$null | Select-Object -First 1
    if ($vsPath) { $msbuild = & $vswhere -path $vsPath -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1 }
}
if (-not $msbuild) { Log 'ERROR: MSBuild with the C++ workload not found.'; exit 1 }
Log "[fps60] MSBuild: $msbuild"

# Installed toolsets: VC\Tools\MSVC\14.xx -> v14x
$toolsets = @()
$msvc = Join-Path $vsPath 'VC\Tools\MSVC'
if (Test-Path $msvc) {
    $toolsets = Get-ChildItem $msvc -Directory | ForEach-Object {
        $v = $_.Name.Split('.'); if ($v.Count -ge 2) { 'v' + $v[0] + $v[1].Substring(0, 1) }
    } | Sort-Object -Unique -Descending
}
Log "[fps60] toolsets installed: $($toolsets -join ', ')"
$wanted = 'v145'
$toolset = if ($toolsets -contains $wanted) { $wanted } elseif ($toolsets.Count -gt 0) { $toolsets[0] } else { $wanted }
if ($toolset -ne $wanted) { Log "[fps60] project asks for $wanted; building with $toolset" }

# ---------------------------------------------------------------- build
$ok = $true
foreach ($p in 'loader.vcxproj', 'fps60.vcxproj') {
    Log ''
    Log "[fps60] building $p (Win32 / Release, $toolset)"
    & $msbuild (Join-Path $ext $p) /p:Configuration=Release /p:Platform=Win32 /p:PlatformToolset=$toolset /v:minimal 2>&1 |
        ForEach-Object { Log "  $_" }
    Log "[fps60] exit code: $LASTEXITCODE"
    if ($LASTEXITCODE -ne 0) { $ok = $false }
}
$loaderDll = Join-Path $ext 'artifacts\loader\Release\dbghelp.dll'
$modDll    = Join-Path $ext 'artifacts\Release\fps60_ffx.dll'
if (-not $ok -or -not (Test-Path $loaderDll) -or -not (Test-Path $modDll)) {
    Log ''
    Log 'Build failed - nothing installed.'
    Log "Log: $log"
    exit 1
}

# ---------------------------------------------------------------- install
Log ''
$existing = Join-Path $game 'dbghelp.dll'
if ((Test-Path $existing) -and -not (Test-Path (Join-Path $game 'fps60_ffx.dll'))) {
    Log "WARNING: $existing already exists and is not ours (another dbghelp.dll mod?). Not overwriting."
    exit 1
}
Copy-Item $loaderDll (Join-Path $game 'dbghelp.dll') -Force;   Log "installed $game\dbghelp.dll"
Copy-Item $modDll    (Join-Path $game 'fps60_ffx.dll') -Force; Log "installed $game\fps60_ffx.dll"
$ini = Join-Path $game 'fps60.ini'
if (-not (Test-Path $ini)) { Copy-Item (Join-Path $ext 'ship\fps60.ini') $ini; Log "installed $ini (release defaults)" }
else { Log "kept $ini" }
$text = Get-Content $ini -Raw
if ($Fps -gt 0) { $text = [regex]::Replace($text, '(?m)^fps=\d+', "fps=$Fps"); Log "fps60.ini: fps=$Fps" }
# VR compatibility: the Config-page Frame Rate row (CFG-MENU) builds its text at
# install through the game's text-language getter, which faults before the game
# has created its settings object; under Fahrenheit's stage-0 debugger that
# fault is fatal (crash at start, fps60_ffx!build_fps_text). The rate is set
# here, so the row stays off.
if ($text -notmatch '(?m)^CFG-MENU=') {
    $text = [regex]::Replace($text, '(?m)^\[rows\]\r?\n', "`$0; VR setup: Config-page Frame Rate row off (install-time text fault under Fahrenheit)`r`nCFG-MENU=0`r`n", 1)
    Log 'fps60.ini: CFG-MENU=0 (Config-page row off)'
}
else { $text = [regex]::Replace($text, '(?m)^CFG-MENU=.*$', 'CFG-MENU=0') }
Set-Content $ini $text -NoNewline
Log ''
Log 'Done. Start the game as usual (run-ffx.ps1); fps60.log in the game folder says what the mod found.'
Log "Log: $log"
