# install-mod.ps1 - ships in the FFX VR release zip (as tools\install-mod.ps1).
#
# Copies mod\ffxvr\ from the zip into <game>\fahrenheit\mods\ffxvr\ and adds 'ffxvr' to
# Fahrenheit's load order. Your settings (ffxvr.ini) and anything else in that folder are kept,
# so installing a newer zip over an older one is safe.
# It also checks what the mod needs at run time and says what is missing.
#
#   -Uninstall     remove the mod's files and its load-order line (ffxvr.ini and logs are kept, and listed)
#   -Purge         with -Uninstall: also delete the whole mod folder (settings, logs, captures): nothing is left
#   -ResetLogging  set every diagnostic in an existing ffxvr.ini to off (tools\set-logging.ps1 -Profile Release;
#                  a timestamped .bak is made first). A fresh install needs nothing: its defaults are all off.
#   -DeleteLogs    delete the mod's old logs, dumps and captures (ffxvr_*.log, ffxvr_cbdump_*.txt, *.bmp, *.dds,
#                  *.bin captures) from the mod folder, listing each with its size. Settings are never touched.
#   -Force         install even though Fahrenheit was not found
#
# Run with (from the ffx-vr-<version> folder, the one with README.md and tools\ in it):
#   powershell -ExecutionPolicy Bypass -File .\tools\install-mod.ps1

param([switch]$Uninstall, [switch]$Purge, [switch]$ResetLogging, [switch]$DeleteLogs, [switch]$Force)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$game  = Get-FfxvrGame
$modId = 'ffxvr'
$src   = Join-Path $FfxvrRepo "mod\$modId"
$fh    = Join-Path $game 'fahrenheit'
$mods  = Join-Path $fh 'mods'
$dest  = Join-Path $mods $modId
$lo    = Join-Path $mods 'loadorder'
$files = @('ffxvr.dll', 'ffxvr.deps.json', 'ffxvr.runtimeconfig.json', 'ffxvr.manifest.json',
           'ffxvr_native.dll', 'openxr_loader.dll')

Write-Host "Game folder: $game"
Write-Host ''

Assert-FfxvrGameClosed 'the mod''s DLLs'
if ($Purge -and -not $Uninstall) { Write-Host '-Purge goes with -Uninstall. Nothing was changed.'; exit 1 }

# The mod's logs, dumps and captures in its folder (tools\paths.ps1 $FfxvrLogPatterns), largest first.
function Get-ModLogFiles {
    if (-not (Test-Path -LiteralPath $dest)) { return @() }
    @(Get-ChildItem -LiteralPath $dest -File -ErrorAction SilentlyContinue |
      Where-Object { $n = $_.Name; [bool]($FfxvrLogPatterns | Where-Object { $n -like $_ }) } |
      Sort-Object Length -Descending)
}
function Remove-ModLogs {
    $logs = Get-ModLogFiles
    if ($logs.Count -eq 0) { Write-Host '  no old logs or captures in the mod folder'; return }
    $total = 0L
    foreach ($l in $logs) {
        try { Remove-Item -LiteralPath $l.FullName -Force -ErrorAction Stop; $total += $l.Length; Write-Host ('  deleted {0,12:N0} KB  {1}' -f ($l.Length / 1KB), $l.Name) }
        catch { Write-Host "  could not delete $($l.Name): $($_.Exception.Message)" }
    }
    Write-Host ('  freed {0:N1} MB' -f ($total / 1MB))
}

# ---------------------------------------------------------------- uninstall
if ($Uninstall) {
    Assert-FfxvrWritable $mods
    if ($Purge) {
        if (Test-Path -LiteralPath $dest) {
            $all = @(Get-ChildItem -LiteralPath $dest -Recurse -File -ErrorAction SilentlyContinue)
            Remove-Item -LiteralPath $dest -Recurse -Force
            Write-Host ('  deleted {0} ({1} files, {2:N1} MB: the mod, its settings, logs and captures)' -f $dest, $all.Count, (($all | Measure-Object Length -Sum).Sum / 1MB))
        }
    } else {
        foreach ($f in $files) {
            $p = Join-Path $dest $f
            if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; Write-Host "  removed $f" }
        }
        if ($DeleteLogs) { Remove-ModLogs }
    }
    if (Test-Path -LiteralPath $lo) {
        $lines = @(Get-Content -LiteralPath $lo | Where-Object { $_.Trim() -ne '' -and $_.Trim() -ne $modId })
        Set-Content -LiteralPath $lo -Value $lines -Encoding ascii
        Write-Host "  removed '$modId' from mods\loadorder"
    }
    Write-Host ''
    $left = @(if (Test-Path -LiteralPath $dest) { Get-ChildItem -LiteralPath $dest -Recurse -File -ErrorAction SilentlyContinue })
    if ($left.Count -eq 0) {
        if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Force -Recurse -ErrorAction SilentlyContinue }
        Write-Host 'Done. Nothing of FFX VR is left in the game folder.'
    } else {
        Write-Host ('Done. Left in {0} ({1} files, {2:N1} MB): your settings, logs and captures:' -f $dest, $left.Count, (($left | Measure-Object Length -Sum).Sum / 1MB))
        $left | Sort-Object Length -Descending | Select-Object -First 8 | ForEach-Object { Write-Host ('  {0,12:N0} KB  {1}' -f ($_.Length / 1KB), $_.Name) }
        Write-Host '  To remove them too: install-mod.ps1 -Uninstall -Purge'
    }
    exit 0
}

# ---------------------------------------------------------------- checks
$problems = 0
Write-Host '[checks]'

$fhOk = (Test-Path -LiteralPath (Join-Path $fh 'bin\fhstage0.exe')) -and (Test-Path -LiteralPath (Join-Path $fh 'bin\fhstage1.dll'))
if ($fhOk) {
    Write-Host '  Fahrenheit         : found'
    # Its version: '1.0.0-alphaNN (<commit>, official release | built from source)'. alpha11 and older predate
    # the game's 1 October 2026 patch; that floor is a fact about the game and never moves, so it is a literal.
    $i = Get-FfxvrFhInfo (Join-Path $fh 'bin')
    if ($i -and $i.Version -match '^1\.0\.0-alpha(\d+)$' -and [int]$Matches[1] -lt 12) {
        Write-Host "  Fahrenheit version : $($i.Text) - too old: it does not work with the current game."
        Write-Host "                       Install release v$FfxvrFhVersion (README.md, section 2)."
        $problems++
    } elseif ($i) {
        $note = if (-not $i.Tested) { " (FFX VR is tested with release v$FfxvrFhVersion)" } else { '' }
        Write-Host "  Fahrenheit version : $($i.Text)$note"
    }
} else {
    Write-Host "  Fahrenheit         : NOT FOUND (expected $fh\bin\fhstage0.exe)"
    Write-Host '                       FFX VR is a Fahrenheit mod. See "Fahrenheit" in README.md.'
    $problems++
}

$net86 = Join-Path ${env:ProgramFiles(x86)} 'dotnet\shared\Microsoft.NETCore.App'
$rt = @(Get-ChildItem -LiteralPath $net86 -Directory -Filter '10.*' -ErrorAction SilentlyContinue)
if ($rt.Count -gt 0) {
    Write-Host ('  .NET 10 x86 runtime: ' + (($rt | ForEach-Object { $_.Name }) -join ', '))
} else {
    Write-Host '  .NET 10 x86 runtime: NOT FOUND'
    Write-Host '                       winget install Microsoft.DotNet.Runtime.10 --architecture x86'
    $problems++
}

$rt32 = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenXR\1' -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime
if (-not $rt32) {
    Write-Host '  OpenXR (32-bit)    : none registered. In the Virtual Desktop Streamer, choose VDXR as the OpenXR runtime.'
    $problems++
} elseif ($rt32 -match 'virtualdesktop') {
    Write-Host "  OpenXR (32-bit)    : VDXR ($rt32)"
} else {
    Write-Host "  OpenXR (32-bit)    : $rt32"
    Write-Host '                       Not VDXR. Only Virtual Desktop (VDXR) has been tested; choose it in the Streamer.'
}

# fps60_ffx.dll, not dbghelp.dll: another mod (or Fahrenheit's own fix-up) may also use that name.
if (Test-Path -LiteralPath (Join-Path $game 'fps60_ffx.dll')) {
    Write-Host '  FFX-60fps          : looks installed (fps60_ffx.dll next to FFX.exe)'
} else {
    Write-Host '  FFX-60fps          : not installed (recommended; see README.md)'
}
Write-Host ''

if (-not $fhOk -and -not $Force) {
    Write-Host 'Not installed: Fahrenheit has to be in place first. (-Force installs anyway.)'
    exit 1
}

# ---------------------------------------------------------------- copy
foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $src $f))) {
        Write-Host "ERROR: $f is missing from $src. Extract the whole zip again."
        exit 1
    }
}
Assert-FfxvrWritable $mods
Write-Host "[install] -> $dest"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
foreach ($f in $files) {
    Copy-Item -LiteralPath (Join-Path $src $f) -Destination $dest -Force
    $p = Join-Path $dest $f
    Unblock-File -LiteralPath $p -ErrorAction SilentlyContinue   # drop the "downloaded from the internet" mark
    Write-Host ('  {0}  ({1} bytes)' -f $f, (Get-Item -LiteralPath $p).Length)
}

# ---------------------------------------------------------------- loadorder
$lines = @()
if (Test-Path -LiteralPath $lo) { $lines = @(Get-Content -LiteralPath $lo | Where-Object { $_.Trim() -ne '' }) }
if ($lines -notcontains $modId) {
    $lines += $modId
    Set-Content -LiteralPath $lo -Value $lines -Encoding ascii
    Write-Host "  added '$modId' to mods\loadorder"
} else {
    Write-Host "  '$modId' already in mods\loadorder"
}

# ---------------------------------------------------------------- logging
# A fresh install has no ffxvr.ini: the mod's defaults have every diagnostic off. An ini kept from an earlier (tester or
# developer) install may still have them on; -ResetLogging turns them off.
Write-Host ''
Write-Host '[logging]'
$ini = Join-Path $dest 'ffxvr.ini'
if ($ResetLogging -and (Test-Path -LiteralPath $ini)) {
    & (Join-Path $PSScriptRoot 'set-logging.ps1') -Profile Release -IniPath $ini
} elseif (Test-Path -LiteralPath $ini) {
    $on = Get-FfxvrDiagOn ([IO.File]::ReadAllText($ini))
    if ($on.Count -gt 0) {
        Write-Host ('  WARNING: your ffxvr.ini has diagnostics on: ' + ($on -join ', '))
        Write-Host '           They write large logs and cost frame time. Turn them off with:'
        Write-Host '             powershell -ExecutionPolicy Bypass -File .\tools\install-mod.ps1 -ResetLogging'
    } else { Write-Host '  diagnostics: all off' }
} else { Write-Host '  no ffxvr.ini yet: the defaults (diagnostics all off) are used, and the mod writes the file at the first settings change' }
if ($DeleteLogs) { Remove-ModLogs }
else {
    $logs = Get-ModLogFiles; $sz = ($logs | Measure-Object Length -Sum).Sum
    if ($sz -gt 50MB) { Write-Host ('  {0:N0} MB of old logs and captures in the mod folder; -DeleteLogs removes them' -f ($sz / 1MB)) }
}

Write-Host ''
if ($problems -gt 0) {
    Write-Host "Installed, but $problems check(s) above need fixing before it will work."
} else {
    Write-Host 'Installed. Start the game with:  powershell -ExecutionPolicy Bypass -File .\tools\run-ffx.ps1'
}
