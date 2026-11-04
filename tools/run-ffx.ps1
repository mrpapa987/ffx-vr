# run-ffx.ps1
#
# Launches Final Fantasy X through the Fahrenheit loader and captures all output
# into logs\ in this repository, plus any log files the game or Fahrenheit write
# during the run.
#
# Steam must already be running. Do NOT run this elevated - Fahrenheit's docs
# say running it as administrator breaks startup.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\run-ffx.ps1

$ErrorActionPreference = 'Continue'

. (Join-Path $PSScriptRoot 'paths.ps1')
$game   = Get-FfxvrGame
$bin    = Join-Path $game 'fahrenheit\bin'
$logDir = Join-Path $FfxvrRepo 'logs'
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$log    = Join-Path $logDir "run-$stamp.txt"

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

if (-not (Test-Path -LiteralPath (Join-Path $bin 'fhstage0.exe'))) {
    Write-Host "ERROR: fhstage0.exe not found in $bin"
    exit 1
}

if (-not (Get-Process -Name steam -ErrorAction SilentlyContinue)) {
    Write-Host 'WARNING: Steam does not look like it is running. The game may refuse to start.'
    Write-Host ''
}

# Anything written after this moment is from this run
$startTime = Get-Date

Write-Host 'Launching FFX through Fahrenheit...'
Write-Host 'Close the game normally when you have seen enough.'
Write-Host ''

# The console log is UTF-8 (Tee-Object would write UTF-16 in Windows PowerShell 5.1). One writer
# for the whole run: Fahrenheit echoes every log line to the console (tens of thousands with a
# texture pack), and opening the file per line would slow the pipe the game writes into.
$w = $null
try { $w = New-Object IO.StreamWriter($log, $true, $FfxvrUtf8) -ErrorAction Stop; $w.AutoFlush = $true }
catch { Write-Host "WARNING: can't write $log; the console output is shown but not saved." }
Push-Location -LiteralPath $bin
try {
    & .\fhstage0.exe ..\..\FFX.exe *>&1 | ForEach-Object { $s = "$_"; Write-Host $s; if ($w) { $w.WriteLine($s) } }
    $s = "fhstage0 exit code: $LASTEXITCODE"; Write-Host $s; if ($w) { $w.WriteLine($s) }
}
finally {
    if ($w) { $w.Dispose() }
    Pop-Location
}

# Gather the logs and captures this run wrote (UI atlas dumps are .dds, bone captures .bin). They
# only appear in three places: the game folder itself (fps60.log), fahrenheit\logs\<run>\, and
# the VR mod's folder. The rest of the game folder is not walked: a texture pack under
# fahrenheit\mods holds tens of thousands of files. Paths are kept relative to the game folder,
# so two files with the same name don't overwrite each other.
$collected = Join-Path $logDir "run-$stamp-gamelogs"
$ext   = '.log', '.txt', '.bmp', '.dds', '.bin'
$found = @()
$found += @(Get-ChildItem -LiteralPath $game -File -ErrorAction SilentlyContinue)
foreach ($d in (Join-Path $game 'fahrenheit\logs'), (Join-Path $game 'fahrenheit\mods\ffxvr')) {
    if (Test-Path -LiteralPath $d) { $found += @(Get-ChildItem -LiteralPath $d -File -Recurse -ErrorAction SilentlyContinue) }
}
# Never fmvdepth\ (FMV depth and its previews, made from the game's videos: private; convert.log is not a game log).
# The prefix comes from the normalised game path (Get-Item's FullName, like the FullName of the files found above; a
# FFXVR_GAME_DIR with forward slashes or '..' would not match otherwise), compared case-insensitively and ending in '\'
# so fmvdepth2\ is not caught. A path component named fmvdepth (an 8.3 short game path, say) is excluded as well.
$gameFull = (Get-Item -LiteralPath $game).FullName.TrimEnd('\')
$depthPfx = $gameFull + '\fahrenheit\mods\ffxvr\' + $FfxvrFmvDepthDir + '\'
$found = @($found | Where-Object { $ext -contains $_.Extension.ToLower() -and $_.LastWriteTime -ge $startTime -and
                                   -not $_.FullName.StartsWith($depthPfx, [StringComparison]::OrdinalIgnoreCase) -and
                                   -not (@($_.FullName -split '\\') -contains $FfxvrFmvDepthDir) })

$gameLen = $gameFull.Length     # normalised, like FullName below
foreach ($f in $found) {
    $rel = $f.FullName.Substring($gameLen).TrimStart('\')       # e.g. fahrenheit\mods\ffxvr\img_final_game.bmp
    $to  = Join-Path $collected $rel
    New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
    Copy-Item -LiteralPath $f.FullName -Destination $to -Force -ErrorAction SilentlyContinue
    Write-Host ('collected: ' + $rel)
}

Write-Host ''
Write-Host "Console output: $log"
if (Test-Path -LiteralPath $collected) {
    Write-Host "Game-side logs: $collected"
} else {
    Write-Host 'Game-side logs: none were written during this run.'
    Write-Host '  (If Fahrenheit expected to write one, this may be the Program Files permission problem.)'
}
