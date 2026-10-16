# run-ffx.ps1
#
# Launches Final Fantasy X through the Fahrenheit loader and captures all output
# into C:\projects\FFX-VR\logs, plus any log files the game or Fahrenheit write
# during the run.
#
# Steam must already be running. Do NOT run this elevated - Fahrenheit's docs
# say running it as administrator breaks startup.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File C:\projects\FFX-VR\tools\run-ffx.ps1

$ErrorActionPreference = 'Continue'

$game   = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY FFX&FFX-2 HD Remaster'
$bin    = Join-Path $game 'fahrenheit\bin'
$logDir = 'C:\projects\FFX-VR\logs'
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$log    = Join-Path $logDir "run-$stamp.txt"

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

if (-not (Test-Path (Join-Path $bin 'fhstage0.exe'))) {
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

Push-Location $bin
try {
    & .\fhstage0.exe ..\..\FFX.exe *>&1 | Tee-Object -FilePath $log
    "fhstage0 exit code: $LASTEXITCODE" | Tee-Object -FilePath $log -Append
}
finally {
    Pop-Location
}

# Gather any log files produced during this run
$collected = Join-Path $logDir "run-$stamp-gamelogs"
$found = Get-ChildItem -Path $game -Recurse -Include *.log, *.txt -ErrorAction SilentlyContinue |
         Where-Object { $_.LastWriteTime -ge $startTime }

foreach ($f in $found) {
    New-Item -ItemType Directory -Force -Path $collected | Out-Null
    Copy-Item $f.FullName -Destination (Join-Path $collected $f.Name) -Force -ErrorAction SilentlyContinue
    Write-Host ('collected: ' + $f.FullName)
}

Write-Host ''
Write-Host "Console output: $log"
if (Test-Path $collected) {
    Write-Host "Game-side logs: $collected"
} else {
    Write-Host 'Game-side logs: none were written during this run.'
    Write-Host '  (If Fahrenheit expected to write one, this may be the Program Files permission problem.)'
}
