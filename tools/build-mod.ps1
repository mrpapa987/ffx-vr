# build-mod.ps1
#
# Builds both halves of the FFX VR mod, deploys them into the game's
# fahrenheit\mods folder, and makes sure the mod id is listed in 'loadorder'.
#
#   src\native   -> ffxvr_native.dll  (C++, Win32, D3D11 constant buffer hooks)
#   src\managed  -> ffxvr.dll         (C#, Fahrenheit module)
#
# This is the iteration loop: edit source -> run this -> run run-ffx.ps1.
#
# Requires tools\build-fahrenheit-full.ps1 to have been run at least once,
# because the managed half builds against the Fahrenheit SDK it produces.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File C:\projects\FFX-VR\tools\build-mod.ps1

$ErrorActionPreference = 'Continue'

$proj    = 'C:\projects\FFX-VR'
$srcMgd  = Join-Path $proj 'src\managed'
$srcNat  = Join-Path $proj 'src\native'
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

# ---------------------------------------------------------------- native half
Log ''
Log '[native] locating MSBuild'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = $null
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -format value -property installationPath 2>$null | Select-Object -First 1
    if ($vsPath) {
        $msbuild = & $vswhere -path $vsPath -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null | Select-Object -First 1
    }
}

if (-not $msbuild) {
    Log 'ERROR: MSBuild with the C++ workload not found.'
    exit 1
}
Log "  $msbuild"

Log ''
Log '[native] building ffxvr_native.dll (Win32 / Release)'
& $msbuild (Join-Path $srcNat 'ffxvr_native.vcxproj') /p:Configuration=Release /p:Platform=Win32 /v:minimal 2>&1 |
    ForEach-Object { Log "  $_" }
$natCode = $LASTEXITCODE
Log "[native] exit code: $natCode"

$natDll = Join-Path $srcNat 'bin\Win32\Release\ffxvr_native.dll'
if (-not (Test-Path $natDll)) {
    Log ''
    Log "ERROR: ffxvr_native.dll was not produced at $natDll"
    exit 1
}
Log ("[native] " + (Get-Item $natDll).Length + " bytes")

# ---------------------------------------------------------------- managed half
Log ''
Log '[managed] building ffxvr.dll'
Push-Location $srcMgd
try {
    & dotnet build -c Release 2>&1 | ForEach-Object { Log "  $_" }
    $mgdCode = $LASTEXITCODE
}
finally {
    Pop-Location
}
Log "[managed] exit code: $mgdCode"

if ($mgdCode -ne 0) {
    Log ''
    Log 'Managed build failed - nothing deployed.'
    Log "Log: $log"
    exit 1
}

$dll = Get-ChildItem $srcMgd -Recurse -File -Filter "$modId.dll" -ErrorAction SilentlyContinue |
       Where-Object { $_.FullName -match '\\(bin|artifacts)\\' } |
       Sort-Object LastWriteTime -Descending |
       Select-Object -First 1

if (-not $dll) {
    Log ''
    Log "ERROR: built $modId.dll not found under $srcMgd"
    exit 1
}

$outDir = $dll.Directory.FullName
Log "[managed] $outDir"

# ---------------------------------------------------------------- deploy
$dest = Join-Path $modsDir $modId
Log ''
Log "[deploy] -> $dest"
if (Test-Path $dest) { Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue }
New-Item -ItemType Directory -Force -Path $dest | Out-Null

Copy-Item (Join-Path $outDir '*') -Destination $dest -Recurse -Force
Copy-Item $natDll                 -Destination $dest -Force

Get-ChildItem $dest -File | ForEach-Object { Log ('  ' + $_.Name + '  (' + $_.Length + ' bytes)') }

if (-not (Test-Path (Join-Path $dest "$modId.manifest.json"))) {
    Log ''
    Log 'WARNING: no manifest.json deployed. Fahrenheit needs one to load the mod.'
}

# ---------------------------------------------------------------- loadorder
$loadorder = Join-Path $modsDir 'loadorder'
$lines = @()
if (Test-Path $loadorder) {
    $lines = @(Get-Content $loadorder | Where-Object { $_.Trim() -ne '' })
}
if ($lines -notcontains $modId) {
    $lines += $modId
    Set-Content -Path $loadorder -Value $lines -Encoding ascii
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
Log '  In game, press F9 to dump constant buffers (once standing still,'
Log '  once after moving). Dumps land beside the mod DLL and are collected'
Log '  automatically when the game exits.'
Log "Log: $log"
