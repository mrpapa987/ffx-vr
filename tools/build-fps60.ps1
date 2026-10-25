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
#   powershell -ExecutionPolicy Bypass -File .\tools\build-fps60.ps1 [-Fps 60] [-NoUpdate] [-Uninstall]
#
# The project is written for the VS 2026 toolset (v145). When only an older
# toolset is installed the build is retried with that one; the code is C++17.
# -Uninstall removes dbghelp.dll only when it is FFX-60fps's loader, never another mod's.

param(
    [int]$Fps = 0,          # 0 = leave fps60.ini alone; otherwise write fps=<n> (multiple of 30, 60..240)
    [switch]$NoUpdate,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Continue'

. (Join-Path $PSScriptRoot 'paths.ps1')
$proj   = $FfxvrRepo
$ext    = Join-Path $proj '_ext\FFX-60fps'
$game   = Get-FfxvrGame
$logDir = Join-Path $proj 'logs'
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$log    = Join-Path $logDir "build-fps60-$stamp.txt"
$repo   = 'https://github.com/peternguyenst/FFX-60fps.git'

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
function Log($m) { Write-FfxvrLog $log $m }      # UTF-8, and nothing on the pipeline

# Is this dbghelp.dll FFX-60fps's proxy loader? Byte-identical to our build, or it names
# fps60_ffx.dll, which the loader stores as UTF-16 text. Latin-1 maps bytes to chars one to one,
# so the search finds the UTF-16 bytes at any offset.
function Test-Fps60Loader([string]$p) {
    if (-not (Test-Path -LiteralPath $p)) { return $false }
    $built = Join-Path $ext 'artifacts\loader\Release\dbghelp.dll'
    if ((Test-Path -LiteralPath $built) -and ((Get-FileHash -LiteralPath $built).Hash -eq (Get-FileHash -LiteralPath $p).Hash)) { return $true }
    $latin1 = [Text.Encoding]::GetEncoding(28591)
    $text   = $latin1.GetString([IO.File]::ReadAllBytes($p))
    $needle = $latin1.GetString([Text.Encoding]::Unicode.GetBytes('fps60_ffx.dll'))
    return ($text.IndexOf($needle, [StringComparison]::OrdinalIgnoreCase) -ge 0)
}

if ($Uninstall) {
    Assert-FfxvrGameClosed 'fps60_ffx.dll and dbghelp.dll' $FfxvrRootExes
    $dbg = Join-Path $game 'dbghelp.dll'
    if (Test-Path -LiteralPath $dbg) {
        if (Test-Fps60Loader $dbg) { Remove-Item -LiteralPath $dbg -Force; Log "removed $dbg" }
        else { Log "kept ${dbg}: it isn't FFX-60fps's loader (another mod's?)" }
    }
    $fp = Join-Path $game 'fps60_ffx.dll'
    if (Test-Path -LiteralPath $fp) { Remove-Item -LiteralPath $fp -Force; Log "removed $fp" }
    Log 'fps60.ini and fps60.log left in place.'
    exit 0
}

# ---------------------------------------------------------------- source
if (-not (Test-Path -LiteralPath (Join-Path $ext '.git'))) {
    Log "[fps60] cloning $repo"
    New-Item -ItemType Directory -Force -Path (Split-Path $ext) | Out-Null
    & git clone --depth 1 $repo $ext 2>&1 | ForEach-Object { Log "  $_" }
    if (-not (Test-Path -LiteralPath (Join-Path $ext '.git'))) { Log 'ERROR: clone failed'; exit 1 }
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
if (Test-Path -LiteralPath $vswhere) {
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format value -property installationPath 2>$null | Select-Object -First 1
    if ($vsPath) { $msbuild = & $vswhere -path $vsPath -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1 }
}
if (-not $msbuild) { Log 'ERROR: MSBuild with the C++ workload not found.'; exit 1 }
Log "[fps60] MSBuild: $msbuild"

# Installed toolsets, from the MSVC folders: 14.1x = v141, 14.2x = v142, 14.3x and 14.4x = v143
# (VS 2022), 14.5x = v145 (VS 2026). Anything newer is logged, not guessed. @() keeps a single
# toolset an array (a lone string would make $toolsets[0] the letter 'v').
$toolsets = @()
$msvc = Join-Path $vsPath 'VC\Tools\MSVC'
if (Test-Path -LiteralPath $msvc) {
    $toolsets = @(Get-ChildItem -LiteralPath $msvc -Directory | ForEach-Object {
        $mi = 0; $v = $_.Name.Split('.')
        if ($v.Count -ge 2 -and $v[0] -eq '14' -and [int]::TryParse($v[1], [ref]$mi)) {
            if     ($mi -ge 60) { Log "[fps60] unknown MSVC $($_.Name), skipped" }   # newer than this table: don't guess
            elseif ($mi -ge 50) { 'v145' }
            elseif ($mi -ge 30) { 'v143' }
            elseif ($mi -ge 20) { 'v142' }
            elseif ($mi -ge 10) { 'v141' }
            else                { 'v140' }
        }
    } | Where-Object { $_ } | Sort-Object -Unique -Descending)
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
if (-not $ok -or -not (Test-Path -LiteralPath $loaderDll) -or -not (Test-Path -LiteralPath $modDll)) {
    Log ''
    Log 'Build failed - nothing installed.'
    Log "Log: $log"
    exit 1
}

# ---------------------------------------------------------------- install
Log ''
if ((Get-FfxvrGameProcess $FfxvrRootExes).Count -gt 0) {
    Log 'ERROR: a program in the game folder is running; nothing installed.'
    Assert-FfxvrGameClosed 'dbghelp.dll and fps60_ffx.dll' $FfxvrRootExes
}
# Ours when it is FFX-60fps's loader, or fps60_ffx.dll sits beside it (an older loader that a
# git pull has since changed); anything else is another mod's dbghelp.dll.
$existing = Join-Path $game 'dbghelp.dll'
if ((Test-Path -LiteralPath $existing) -and -not ((Test-Fps60Loader $existing) -or (Test-Path -LiteralPath (Join-Path $game 'fps60_ffx.dll')))) {
    Log "WARNING: $existing already exists and is not FFX-60fps's (another dbghelp.dll mod?). Not overwriting."
    exit 1
}
try {
    Copy-Item -LiteralPath $loaderDll -Destination (Join-Path $game 'dbghelp.dll') -Force -ErrorAction Stop;   Log "installed $game\dbghelp.dll"
    Copy-Item -LiteralPath $modDll    -Destination (Join-Path $game 'fps60_ffx.dll') -Force -ErrorAction Stop; Log "installed $game\fps60_ffx.dll"
} catch {
    Log "ERROR: install failed: $($_.Exception.Message)"
    exit 1
}
$ini = Join-Path $game 'fps60.ini'
if (-not (Test-Path -LiteralPath $ini)) {
    try { Copy-Item -LiteralPath (Join-Path $ext 'ship\fps60.ini') -Destination $ini -ErrorAction Stop; Log "installed $ini (release defaults)" }
    catch { Log "ERROR: could not install the default fps60.ini ($($_.Exception.Message)). The DLLs are installed; fps60.ini was not written."; exit 1 }
}
else { Log "kept $ini" }
$text = "$(Get-Content -LiteralPath $ini -Raw)"            # an empty file gives '', not $null
if ($Fps -gt 0) {
    if ($text -match '(?mi)^fps[ \t]*=[ \t]*\d+') { $text = [regex]::Replace($text, '(?mi)^fps[ \t]*=[ \t]*\d+', "fps=$Fps"); Log "fps60.ini: fps=$Fps" }
    else { Log "WARNING: no fps= line in $ini; -Fps $Fps not applied." }
}
# VR compatibility: the Config-page Frame Rate row (CFG-MENU) builds its text at
# install through the game's text-language getter, which faults before the game
# has created its settings object; under Fahrenheit's stage-0 debugger that
# fault is fatal (crash at start, fps60_ffx!build_fps_text). The rate is set
# here, so the row stays off. Keys and the section name match case-insensitively, as ini readers
# do; [^\r\n]* keeps the line's CR; the new line goes right after the FIRST [rows] header (a
# reader uses the first section of that name).
$cfgLine = "; VR setup: Config-page Frame Rate row off (install-time text fault under Fahrenheit)`r`nCFG-MENU=0`r`n"
$rows    = [regex]::Match($text, '(?mi)^\[rows\][ \t]*(\r?\n|\z)')
if ($text -match '(?mi)^CFG-MENU[ \t]*=') {
    $text = [regex]::Replace($text, '(?mi)^CFG-MENU[ \t]*=[^\r\n]*', 'CFG-MENU=0')
    Log 'fps60.ini: CFG-MENU=0 (Config-page row off)'
} elseif ($rows.Success) {
    $pre  = if ($rows.Groups[1].Value) { '' } else { "`r`n" }       # [rows] was the last line, no newline
    $text = $text.Insert($rows.Index + $rows.Length, $pre + $cfgLine)
    Log 'fps60.ini: CFG-MENU=0 added (Config-page row off)'
} else {
    $text = $text.TrimEnd() + "`r`n`r`n[rows]`r`n$cfgLine"
    Log 'WARNING: fps60.ini had no [rows] section; added one with CFG-MENU=0. Check fps60.log that the mod read it.'
}
if ($text -notmatch '(?mi)^CFG-MENU=0[ \t]*\r?$') { Log 'WARNING: fps60.ini still has no CFG-MENU=0 line; set it by hand, or the game may crash at start under Fahrenheit.' }
Set-Content -LiteralPath $ini -Value $text -NoNewline
Log ''
Log 'Done. Start the game as usual (run-ffx.ps1); fps60.log in the game folder says what the mod found.'
Log "Log: $log"
