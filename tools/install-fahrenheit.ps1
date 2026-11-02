# install-fahrenheit.ps1
#
# Installs Fahrenheit (the mod loader) into the game folder, from one of two sources:
#   -Zip <path>   the official release zip (fahrenheit_release_v<version>.zip from the Fahrenheit
#                 releases page; FFX VR is tested with v$FfxvrFhVersion, tools\paths.ps1). The usual way.
#   (no -Zip)     a source build from tools\build-fahrenheit-full.ps1:
#                 _ext\fahrenheit\artifacts\deploy\rel\bin\*
# Either way it goes to <game>\fahrenheit\bin\. The destination bin\ is emptied first, so files
# removed upstream (or from the other source) do not linger.
# config\, mods\, saves\, state\ and logs\ next to it are never touched.
# Also makes sure mods\loadorder exists, and that dbghelp.dll is present (a load-time
# import of fhstage0.exe that neither the release zip nor a build without Debugging Tools for
# Windows carries; the system's own 32-bit copy works). Prints the installed Fahrenheit version,
# its commit, and whether it is the signed official release.
# Refuses while the game (or Fahrenheit's stage 0) runs: its files in bin\ are locked then, and
# the old bin\ would be left half deleted.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\install-fahrenheit.ps1 -Zip <path to the zip>
#   powershell -ExecutionPolicy Bypass -File .\tools\install-fahrenheit.ps1      (the source build in _ext)
# from the repository folder.

param([string]$Zip)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$game = Get-FfxvrGame
$fh   = Join-Path $game 'fahrenheit'
$dst  = Join-Path $fh 'bin'
$tmp  = $null           # the zip's extraction folder; always removed (finally runs on exit too)

try {
    if ($Zip) {
        Assert-FfxvrGameClosed 'Fahrenheit''s files'       # before extracting: nothing to undo then
        if (-not (Test-Path -LiteralPath $Zip -PathType Leaf)) { Write-Host "ERROR: no such file: $Zip"; exit 1 }
        $Zip = (Resolve-Path -LiteralPath $Zip).ProviderPath   # a plain path, also for a UNC share
        Unblock-File -LiteralPath $Zip -ErrorAction SilentlyContinue   # a downloaded zip carries the internet mark onto every file
        $tmp = Join-Path $env:TEMP ('ffxvr-fh-' + [guid]::NewGuid().ToString('N'))
        Write-Host "Extracting $Zip"
        Expand-Archive -LiteralPath $Zip -DestinationPath $tmp
        $s0 = @(Get-ChildItem -LiteralPath $tmp -Recurse -File -Filter 'fhstage0.exe')
        if ($s0.Count -ne 1) {
            Write-Host "ERROR: $Zip has $($s0.Count) fhstage0.exe (expected one, in bin\). Is it the Fahrenheit release zip?"
            exit 1
        }
        $src = $s0[0].DirectoryName          # bin\ at the zip's root for the releases so far; not assumed
    } else {
        $src = Join-Path $FfxvrRepo '_ext\fahrenheit\artifacts\deploy\rel\bin'
    }

    foreach ($f in 'fhstage0.exe', 'fhstage1.dll', 'minhook.x32.dll', 'nethost.dll', 'fh.dll', 'fhr.dll') {
        if (-not (Test-Path -LiteralPath (Join-Path $src $f))) {
            Write-Host "ERROR: $f is missing from $src"
            if ($Zip) { Write-Host '  That zip is not a complete Fahrenheit release.' }
            else      { Write-Host "  Pass -Zip <official release zip> (fahrenheit_release_v$FfxvrFhVersion.zip), or build Fahrenheit: .\tools\build-fahrenheit-full.ps1" }
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
    $i = Get-FfxvrFhInfo $dst
    if ($i) {
        $note = if (-not $i.Tested) { ' - NOT the commit FFX VR is tested with (' + $FfxvrFhCommit.Substring(0, 7) + ', release v' + $FfxvrFhVersion + ')' } else { '' }
        Write-Host "  Fahrenheit $($i.Text)$note"
    }
    Write-Host 'Done. Next: .\tools\build-mod.ps1'
} finally {
    if ($tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}
