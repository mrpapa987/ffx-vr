# install-mod.ps1 - ships in the FFX VR release zip (as tools\install-mod.ps1).
#
# Copies mod\ffxvr\ from the zip into <game>\fahrenheit\mods\ffxvr\ and adds 'ffxvr' to
# Fahrenheit's load order. Your settings (ffxvr.ini) and anything else in that folder are kept,
# so installing a newer zip over an older one is safe.
# It also checks what the mod needs at run time and says what is missing.
#
#   -Uninstall   remove the mod's files and its load-order line (ffxvr.ini is kept)
#   -Force       install even though Fahrenheit was not found
#
# Run with (from the ffx-vr-<version> folder, the one with README.md and tools\ in it):
#   powershell -ExecutionPolicy Bypass -File .\tools\install-mod.ps1

param([switch]$Uninstall, [switch]$Force)

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

# ---------------------------------------------------------------- uninstall
if ($Uninstall) {
    Assert-FfxvrWritable $mods
    foreach ($f in $files) {
        $p = Join-Path $dest $f
        if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; Write-Host "  removed $f" }
    }
    if (Test-Path -LiteralPath $lo) {
        $lines = @(Get-Content -LiteralPath $lo | Where-Object { $_.Trim() -ne '' -and $_.Trim() -ne $modId })
        Set-Content -LiteralPath $lo -Value $lines -Encoding ascii
        Write-Host "  removed '$modId' from mods\loadorder"
    }
    Write-Host ''
    Write-Host "Done. Your settings are still in $dest; delete that folder to remove them too."
    exit 0
}

# ---------------------------------------------------------------- checks
$problems = 0
Write-Host '[checks]'

$fhOk = (Test-Path -LiteralPath (Join-Path $fh 'bin\fhstage0.exe')) -and (Test-Path -LiteralPath (Join-Path $fh 'bin\fhstage1.dll'))
if ($fhOk) {
    Write-Host '  Fahrenheit         : found'
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

Write-Host ''
if ($problems -gt 0) {
    Write-Host "Installed, but $problems check(s) above need fixing before it will work."
} else {
    Write-Host 'Installed. Start the game with:  powershell -ExecutionPolicy Bypass -File .\tools\run-ffx.ps1'
}
