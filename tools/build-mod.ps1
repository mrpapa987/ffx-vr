# build-mod.ps1
#
# Builds the FFX VR mod, deploys it into the game's fahrenheit\mods folder,
# and makes sure its id is listed in 'loadorder'.
#
# This is the iteration loop: edit source -> run this -> run run-ffx.ps1.
#
# Requires tools\build-fahrenheit-full.ps1 to have been run at least once,
# because the mod builds against the Fahrenheit SDK produced by that build.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File C:\projects\FFX-VR\tools\build-mod.ps1

$ErrorActionPreference = 'Continue'

$proj    = 'C:\projects\FFX-VR'
$src     = Join-Path $proj 'src\managed'
$game    = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY FFX&FFX-2 HD Remaster'
$modsDir = Join-Path $game 'fahrenheit\mods'
$modId   = 'ffxvr'
$logDir  = Join-Path $proj 'logs'
$stamp   = Get-Date -Format 'yyyyMMdd-HHmmss'
$log     = Join-Path $logDir "build-mod-$stamp.txt"

New-Item -ItemType Directory -Force -Path $logDir | Out-Null

function Log($s) {
    Write-Host $s
    $s | Out-File -FilePath $log -Encoding utf8 -Append
}

Log "FFX VR mod build - $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Log ('=' * 70)

# ---------------------------------------------------------------- preflight
$pkgDir = Join-Path $proj '_ext\fahrenheit\artifacts\pkg\rel'
if (-not (Test-Path (Join-Path $pkgDir 'Fahrenheit.Sdk.1.0.0-alpha12.nupkg'))) {
    Log ''
    Log "ERROR: Fahrenheit SDK package not found in $pkgDir"
    Log '  Run tools\build-fahrenheit-full.ps1 first.'
    exit 1
}

# ---------------------------------------------------------------- build
Log ''
Log '[dotnet] building the mod'
Push-Location $src
try {
    & dotnet build -c Release 2>&1 | ForEach-Object { Log "  $_" }
    $code = $LASTEXITCODE
}
finally {
    Pop-Location
}
Log "[dotnet] exit code: $code"

if ($code -ne 0) {
    Log ''
    Log 'Build failed - nothing deployed.'
    Log "Log: $log"
    exit 1
}

# ---------------------------------------------------------------- locate output
$dll = Get-ChildItem $src -Recurse -File -Filter "$modId.dll" -ErrorAction SilentlyContinue |
       Where-Object { $_.FullName -match '\\(bin|artifacts)\\' } |
       Sort-Object LastWriteTime -Descending |
       Select-Object -First 1

if (-not $dll) {
    Log ''
    Log "ERROR: built $modId.dll not found under $src"
    exit 1
}

$outDir = $dll.Directory.FullName
Log ''
Log "[output] $outDir"
Get-ChildItem $outDir -File | ForEach-Object { Log ('  ' + $_.Name + '  (' + $_.Length + ' bytes)') }

if (-not (Test-Path (Join-Path $outDir "$modId.manifest.json"))) {
    Log ''
    Log 'WARNING: no manifest.json beside the DLL. Fahrenheit needs one to load the mod.'
}

# ---------------------------------------------------------------- deploy
$dest = Join-Path $modsDir $modId
Log ''
Log "[deploy] -> $dest"
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item (Join-Path $outDir '*') -Destination $dest -Recurse -Force
Log ("  copied " + (Get-ChildItem $dest -Recurse -File).Count + " files")

# ---------------------------------------------------------------- loadorder
$loadorder = Join-Path $modsDir 'loadorder'
$lines = @()
if (Test-Path $loadorder) {
    $lines = @(Get-Content $loadorder | Where-Object { $_.Trim() -ne '' })
}
if ($lines -notcontains $modId) {
    $lines += $modId
    Set-Content -Path $loadorder -Value $lines -Encoding ascii -NoNewline:$false
    Log ''
    Log "[loadorder] added '$modId'"
} else {
    Log ''
    Log "[loadorder] '$modId' already listed"
}
Log ('  contents: ' + ((Get-Content $loadorder) -join ', '))

Log ''
Log ('=' * 70)
Log 'Done. Now run:  tools\run-ffx.ps1'
Log "Log: $log"
