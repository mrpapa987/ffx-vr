# build-fahrenheit.ps1
#
# Clones Fahrenheit's main branch and builds its MANAGED components.
#
# Why: Square Enix patched FFX on the Steam release on 1 October 2026. Fahrenheit
# calls the new executables the "P4" binaries, and support for them landed on main
# between 2 and 4 October. Release alpha11 (19 September) predates the patch and
# targets the old executable, which is why it crashes on the current game.
#
# This script does NOT touch the game folder. It only clones and builds, and
# writes a log to C:\projects\FFX-VR\logs so the result can be reviewed.
#
# Native loaders (stage0/stage1) are NOT built here - that needs Visual Studio 2026.
# Errors about those two projects are expected and can be ignored; we reuse the
# prebuilt loaders from the alpha11 release.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File C:\projects\FFX-VR\tools\build-fahrenheit.ps1

$ErrorActionPreference = 'Continue'

$proj   = 'C:\projects\FFX-VR'
$ext    = Join-Path $proj '_ext'
$repo   = Join-Path $ext 'fahrenheit'
$logDir = Join-Path $proj 'logs'
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$log    = Join-Path $logDir "build-fahrenheit-$stamp.txt"

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
New-Item -ItemType Directory -Force -Path $ext    | Out-Null

function Log($s) { $s | Tee-Object -FilePath $log -Append }

Log "Fahrenheit build - $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Log ('=' * 64)

# ---------------------------------------------------------------- clone or update
if (Test-Path (Join-Path $repo '.git')) {
    Log ''
    Log '[git] repository already present; fetching latest main'
    & git -C $repo fetch origin main 2>&1 | Tee-Object -FilePath $log -Append
    & git -C $repo reset --hard origin/main 2>&1 | Tee-Object -FilePath $log -Append
} else {
    Log ''
    Log '[git] cloning Fahrenheit (this may take a minute)'
    & git clone https://github.com/fahrenheit-crew/fahrenheit $repo 2>&1 | Tee-Object -FilePath $log -Append
}

if (-not (Test-Path (Join-Path $repo 'Fahrenheit.slnx'))) {
    Log ''
    Log 'ERROR: clone failed - Fahrenheit.slnx not found. Stopping.'
    Log "Log: $log"
    exit 1
}

Log ''
Log '[git] building from commit:'
& git -C $repo log -1 --pretty='  %H%n  %ad  %s' --date=short 2>&1 | Tee-Object -FilePath $log -Append

# ---------------------------------------------------------------- build
Log ''
Log '[dotnet] restoring and building managed components (Release)'
Log '  Errors mentioning stage0 / stage1 are expected on this machine.'
Log ''

Push-Location $repo
try {
    & dotnet build -c Release .\Fahrenheit.slnx 2>&1 | Tee-Object -FilePath $log -Append
    $code = $LASTEXITCODE
}
finally {
    Pop-Location
}

Log ''
Log "[dotnet] exit code: $code"

# ---------------------------------------------------------------- what came out
$deploy = Join-Path $repo 'artifacts\deploy'
Log ''
Log '[artifacts] what the build produced:'
if (Test-Path $deploy) {
    Get-ChildItem $deploy -Recurse -File |
        ForEach-Object { Log ('  ' + $_.FullName.Substring($repo.Length + 1) + '  (' + $_.Length + ' bytes)') }
} else {
    Log "  nothing at $deploy"
    Log '  (searching for any built fh*.dll instead)'
    Get-ChildItem $repo -Recurse -File -Filter 'fh*.dll' -ErrorAction SilentlyContinue |
        Select-Object -First 40 |
        ForEach-Object { Log ('  ' + $_.FullName.Substring($repo.Length + 1) + '  (' + $_.Length + ' bytes)') }
}

Log ''
Log ('=' * 64)
Log "Log saved to: $log"
Write-Host ''
Write-Host "Done. Log: $log"
