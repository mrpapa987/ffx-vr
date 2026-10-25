# install-fahrenheit.ps1
#
# Copies the Fahrenheit build (from tools\build-fahrenheit-full.ps1) into the game folder:
#   _ext\fahrenheit\artifacts\deploy\rel\bin\*  ->  <game>\fahrenheit\bin\
# The destination bin\ is emptied first, so files removed upstream do not linger.
# config\, mods\, saves\, state\ and logs\ next to it are never touched.
# Also makes sure mods\loadorder exists, and that dbghelp.dll is present (a load-time
# import of fhstage0.exe that the Fahrenheit build skips without Debugging Tools for
# Windows installed; the system's own 32-bit copy works).
# Refuses while the game (or Fahrenheit's stage 0) runs: its files in bin\ are locked then, and
# the old bin\ would be left half deleted.
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
    if (-not (Test-Path -LiteralPath (Join-Path $src $f))) {
        Write-Host "ERROR: $f is missing from $src"
        Write-Host '  Build Fahrenheit first: .\tools\build-fahrenheit-full.ps1'
        exit 1
    }
}

Assert-FfxvrGameClosed 'Fahrenheit''s files'
Assert-FfxvrWritable $dst                 # probes the nearest existing parent while bin\ doesn't exist yet

Write-Host "Installing Fahrenheit into $fh"
New-Item -ItemType Directory -Force -Path $dst | Out-Null
try {
    Get-ChildItem -LiteralPath $dst -Force | Remove-Item -Recurse -Force
    Get-ChildItem -LiteralPath $src | Copy-Item -Destination $dst -Recurse -Force
} catch {
    Write-Host "ERROR: $($_.Exception.Message)"
    Write-Host "  $dst is incomplete now. Close anything using it and run install-fahrenheit.ps1 again."
    exit 1
}
if (-not (Test-Path -LiteralPath (Join-Path $dst 'dbghelp.dll'))) {
    $sys = Join-Path $env:WINDIR 'SysWOW64\dbghelp.dll'
    if (Test-Path -LiteralPath $sys) { Copy-Item -LiteralPath $sys -Destination $dst; Write-Host '  dbghelp.dll: copied the system''s 32-bit copy' }
    else { Write-Host '  WARNING: dbghelp.dll missing and no SysWOW64 copy found; fhstage0 will not start' }
}

$mods = Join-Path $fh 'mods'
New-Item -ItemType Directory -Force -Path $mods | Out-Null
$lo = Join-Path $mods 'loadorder'
if (-not (Test-Path -LiteralPath $lo)) { Set-Content -LiteralPath $lo -Value @() -Encoding ascii; Write-Host '  created mods\loadorder (empty; build-mod.ps1 adds ffxvr)' }

Write-Host ('  {0} files in {1}' -f @(Get-ChildItem -LiteralPath $dst -Recurse -File).Count, $dst)
Write-Host 'Done. Next: .\tools\build-mod.ps1'
