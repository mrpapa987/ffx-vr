# install-fahrenheit.ps1
#
# Copies the Fahrenheit build (from tools\build-fahrenheit-full.ps1) into the game folder:
#   _ext\fahrenheit\artifacts\deploy\rel\bin\*  ->  <game>\fahrenheit\bin\
# The destination bin\ is emptied first, so files removed upstream do not linger.
# config\, mods\, saves\, state\ and logs\ next to it are never touched.
# Also makes sure mods\loadorder exists, and that dbghelp.dll is present (a load-time
# import of fhstage0.exe that the Fahrenheit build skips without Debugging Tools for
# Windows installed; the system's own 32-bit copy works).
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\install-fahrenheit.ps1     (from the repository folder)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$game = Get-FfxvrGame
$src  = Join-Path $FfxvrRepo '_ext\fahrenheit\artifacts\deploy\rel\bin'
$fh   = Join-Path $game 'fahrenheit'
$dst  = Join-Path $fh 'bin'

foreach ($f in 'fhstage0.exe', 'fhstage1.dll', 'minhook.x32.dll', 'nethost.dll') {
    if (-not (Test-Path (Join-Path $src $f))) {
        Write-Host "ERROR: $f is missing from $src"
        Write-Host '  Build Fahrenheit first: .\tools\build-fahrenheit-full.ps1'
        exit 1
    }
}

Write-Host "Installing Fahrenheit into $fh"
New-Item -ItemType Directory -Force -Path $dst | Out-Null
Get-ChildItem $dst -Force | Remove-Item -Recurse -Force
Copy-Item (Join-Path $src '*') -Destination $dst -Recurse -Force
if (-not (Test-Path (Join-Path $dst 'dbghelp.dll'))) {
    $sys = Join-Path $env:WINDIR 'SysWOW64\dbghelp.dll'
    if (Test-Path $sys) { Copy-Item $sys -Destination $dst; Write-Host '  dbghelp.dll: copied the system''s 32-bit copy' }
    else { Write-Host '  WARNING: dbghelp.dll missing and no SysWOW64 copy found; fhstage0 will not start' }
}

$mods = Join-Path $fh 'mods'
New-Item -ItemType Directory -Force -Path $mods | Out-Null
$lo = Join-Path $mods 'loadorder'
if (-not (Test-Path $lo)) { Set-Content -Path $lo -Value @() -Encoding ascii; Write-Host '  created mods\loadorder (empty; build-mod.ps1 adds ffxvr)' }

Write-Host ('  {0} files in {1}' -f (Get-ChildItem $dst -Recurse -File).Count, $dst)
Write-Host 'Done. Next: .\tools\build-mod.ps1'
