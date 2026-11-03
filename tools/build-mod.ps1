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
# The managed half restores the Fahrenheit SDK from nuget.org (src\managed\nuget.config), into
# _ext\nuget; the first build needs the network. Fahrenheit itself comes from its official release
# (INSTALL-ALPHA step 4). The SDK version in ffxvr.csproj must equal $FfxvrFhVersion (tools\paths.ps1).
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\build-mod.ps1     (from the repository folder)
#
#   -NoDeploy   build only; leave the game folder alone (tools\package-release.ps1 uses this,
#               and it works on a PC without the game installed)

param([switch]$NoDeploy)

$ErrorActionPreference = 'Continue'

. (Join-Path $PSScriptRoot 'paths.ps1')
$proj    = $FfxvrRepo
$srcMgd  = Join-Path $proj 'src\managed'
$srcNat  = Join-Path $proj 'src\native'
$game    = if ($NoDeploy) { $null } else { Get-FfxvrGame }
$modsDir = if ($game) { Join-Path $game 'fahrenheit\mods' } else { $null }
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
if (-not $NoDeploy -and (Get-FfxvrGameProcess).Count -gt 0) {
    Log 'NOTE: the game is running. The build will run, but deploying will stop until it is closed.'
}

# ---------------------------------------------------------------- preflight
# Without dotnet, '& dotnet' below only fails its own statement and $LASTEXITCODE keeps msbuild's 0,
# so an old ffxvr.dll would be deployed as if it were new.
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    Log ''
    Log 'ERROR: dotnet (the .NET 10 SDK) is not on PATH. Install it: winget install Microsoft.DotNet.SDK.10'
    exit 1
}
# The SDK version the project builds against, and the release tools\paths.ps1 says FFX VR is tested with.
$fhVer = $null
try { $fhVer = ((([xml](Get-Content -LiteralPath (Join-Path $srcMgd 'ffxvr.csproj') -Raw)).Project.Sdk) -split '/')[1] } catch { }
if ($fhVer -ne $FfxvrFhVersion) {
    Log ''
    Log "ERROR: ffxvr.csproj builds against Fahrenheit.Sdk '$fhVer', tools\paths.ps1 says $FfxvrFhVersion. Make them agree."
    exit 1
}
if ($env:NUGET_PACKAGES) {
    Log "WARNING: NUGET_PACKAGES is set ($env:NUGET_PACKAGES); it overrides src\managed\nuget.config's _ext\nuget, so the"
    Log '         Fahrenheit package source below can not be checked. Clear it for this build.'
}

# ---------------------------------------------------------------- OpenXR loader
# The headset half needs the Khronos OpenXR loader (32-bit) and the headers.
# They are two NuGet packages (OpenXR.Loader depends on OpenXR.Headers),
# fetched once into _ext\openxr\loader and _ext\openxr\headers.
$oxrDir = Join-Path $proj '_ext\openxr'
$oxrHdr = Join-Path $oxrDir 'headers\include\openxr\openxr.h'
function Fetch-NuGet($id, $dest) {
    $nupkg = Join-Path $proj "_ext\$id.nupkg"
    New-Item -ItemType Directory -Force -Path (Join-Path $proj '_ext') | Out-Null
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri "https://www.nuget.org/api/v2/package/$id" -OutFile $nupkg -UseBasicParsing
    $zip = "$nupkg.zip"
    Copy-Item $nupkg $zip -Force
    if (Test-Path $dest) { Remove-Item $dest -Recurse -Force }
    Expand-Archive -Path $zip -DestinationPath $dest -Force
    Remove-Item $zip -Force
}
if (-not (Test-Path $oxrHdr)) {
    Log ''
    Log '[openxr] fetching the OpenXR loader and header packages (once)'
    try {
        if (Test-Path $oxrDir) { Remove-Item $oxrDir -Recurse -Force }
        Fetch-NuGet 'OpenXR.Loader'  (Join-Path $oxrDir 'loader')
        Fetch-NuGet 'OpenXR.Headers' (Join-Path $oxrDir 'headers')
    } catch {
        Log "ERROR: could not fetch the OpenXR packages: $_"
        exit 1
    }
}
$oxrDll = Get-ChildItem (Join-Path $oxrDir 'loader') -Recurse -Filter 'openxr_loader.dll' -ErrorAction SilentlyContinue |
          Where-Object { $_.FullName -match '\\Win32\\release\\bin\\' } |
          Select-Object -First 1
if (-not (Test-Path $oxrHdr)) {
    Log "ERROR: openxr.h not found under $oxrDir"
    Get-ChildItem $oxrDir -Recurse -Filter 'openxr.h' | ForEach-Object { Log ('  found: ' + $_.FullName) }
    exit 1
}
if (-not $oxrDll) {
    Log "ERROR: no 32-bit openxr_loader.dll in the package under $oxrDir"
    Get-ChildItem $oxrDir -Recurse -Filter 'openxr_loader.dll' | ForEach-Object { Log ('  found: ' + $_.FullName) }
    exit 1
}
Log "[openxr] loader: $($oxrDll.FullName)"

# ---------------------------------------------------------------- version
# One place: ffxvr.csproj's <Version>. The managed half gets it from the SDK (the assembly and ffxvr.manifest.json);
# the native half from src\native\ffxvr_version.h, written here before every build (not in git), with the commit
# ("-dirty" with uncommitted changes). Both are shown in the F8 overlay and at the top of ffxvr_diag.log.
$modVer = '0.0.0'
try { $v = ([xml](Get-Content -LiteralPath (Join-Path $srcMgd 'ffxvr.csproj') -Raw)).Project.PropertyGroup | ForEach-Object { $_.Version } | Where-Object { $_ } | Select-Object -First 1; if ($v) { $modVer = "$v".Trim() } } catch { }
$modCommit = 'nogit'
if (Get-Command git -ErrorAction SilentlyContinue) {
    $c = & git -C $proj rev-parse --short=7 HEAD 2>$null
    if ($LASTEXITCODE -eq 0 -and $c) {
        $modCommit = "$c".Trim()
        $d = & git -C $proj status --porcelain --untracked-files=no 2>$null
        if ($LASTEXITCODE -eq 0 -and $d) { $modCommit += '-dirty' }
    }
}
$verH = "// Written by tools\build-mod.ps1 before every build; not in git.`r`n#define FFXVR_VERSION `"$($modVer -replace '[^A-Za-z0-9._+-]', '-')`"`r`n#define FFXVR_COMMIT `"$($modCommit -replace '[^A-Za-z0-9._+-]', '-')`"`r`n"
$verHPath = Join-Path $srcNat 'ffxvr_version.h'
$old = if (Test-Path -LiteralPath $verHPath) { [IO.File]::ReadAllText($verHPath) } else { '' }
if ($old -ne $verH) { [IO.File]::WriteAllText($verHPath, $verH, $FfxvrUtf8) }   # unchanged: left alone, so MSBuild does not rebuild for nothing
Log ''
Log "[version] $modVer ($modCommit)"

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
if ($natCode -ne 0) {
    Log ''
    Log 'Native build failed - nothing deployed. A stale ffxvr_native.dll next to a'
    Log 'fresh ffxvr.dll would only fail at load time with a missing export.'
    Log "Log: $log"
    exit 1
}
if (-not (Test-Path $natDll)) {
    Log ''
    Log "ERROR: ffxvr_native.dll was not produced at $natDll"
    exit 1
}
Log ("[native] " + (Get-Item $natDll).Length + " bytes")

# ---------------------------------------------------------------- managed half
Log ''
Log '[managed] building ffxvr.dll'
Push-Location -LiteralPath $srcMgd
try {
    & dotnet build -c Release 2>&1 | ForEach-Object { Log "  $_" }
    $mgdCode = $LASTEXITCODE
}
finally {
    Pop-Location
}
Log "[managed] exit code: $mgdCode"

if ($mgdCode -ne 0 -and (Select-String -LiteralPath $log -Pattern 'MSB4236' -Quiet)) {
    Log ''
    Log "HINT: Fahrenheit.Sdk $fhVer could not be restored: no network, or it is not on nuget.org. See the fallback"
    Log '      block in docs\design-notes.md (Fahrenheit port) for src\managed\nuget.config.'
} elseif ($mgdCode -ne 0 -and (Select-String -LiteralPath $log -Pattern 'NU110[12].*[Ff]ahrenheit' -Quiet)) {
    Log ''
    Log "HINT: the fahrenheit $fhVer package could not be restored from nuget.org: no network, or that version is not"
    Log '      published there. Check https://www.nuget.org/packages/Fahrenheit (only nuget.org is used: src\managed\nuget.config).'
}
if ($mgdCode -ne 0) {
    Log ''
    Log 'Managed build failed - nothing deployed.'
    Log "Log: $log"
    exit 1
}

# Where NuGet got the Fahrenheit packages. A local Fahrenheit build has the same version string, so the
# source recorded in .nupkg.metadata is the only difference. fahrenheit (the API we compile against)
# must come from nuget.org; fahrenheit.sdk (props/targets, identical in a local build) only warns.
foreach ($id in 'fahrenheit.sdk', 'fahrenheit') {
    $meta = Join-Path $proj "_ext\nuget\$id\$fhVer\.nupkg.metadata"
    $from = $null
    try { $from = (Get-Content -LiteralPath $meta -Raw -ErrorAction Stop | ConvertFrom-Json).source } catch { }
    if ($from -match 'api\.nuget\.org') { Log "[managed] $id $fhVer from $from" }
    elseif ($from -and $id -eq 'fahrenheit') {
        Log ''
        Log "ERROR: fahrenheit $fhVer came from $from, not nuget.org. Nothing deployed."
        Log '  Delete the _ext\nuget folder, run "dotnet build-server shutdown", and build again.'
        Log "Log: $log"
        exit 1
    }
    elseif ($from) { Log "WARNING: fahrenheit.sdk $fhVer came from $from (the local fallback?); its content equals the release's" }
    else { Log "WARNING: no record of $id $fhVer in _ext\nuget (NUGET_PACKAGES set? nuget.config missing?): source not checked" }
}

# Never a Debug build (one from Visual Studio can be newer than this Release build). The test runs
# on the path below src\managed, so a clone under e.g. C:\debug\ still works.
$dll = Get-ChildItem -LiteralPath $srcMgd -Recurse -File -Filter "$modId.dll" -ErrorAction SilentlyContinue |
       Where-Object { $r = $_.FullName.Substring($srcMgd.Length); $r -match '\\(bin|artifacts)\\' -and $r -notmatch '\\(debug|dbg)(_[^\\]*)?\\' } |
       Sort-Object @{ e = { Test-Path -LiteralPath (Join-Path $_.DirectoryName "$modId.manifest.json") } }, LastWriteTime -Descending |
       Select-Object -First 1

if (-not $dll) {
    Log ''
    Log "ERROR: built $modId.dll not found under $srcMgd"
    exit 1
}

$outDir = $dll.Directory.FullName
Log "[managed] $outDir"

if ($NoDeploy) {
    Log ''
    Log 'Built (-NoDeploy: nothing copied to the game).'
    Log "  native : $natDll"
    Log "  managed: $outDir"
    Log "Log: $log"
    exit 0
}

# ---------------------------------------------------------------- deploy
if ((Get-FfxvrGameProcess).Count -gt 0) {
    Log ''
    Log 'ERROR: the game is running; nothing deployed. Close it and run build-mod again.'
    Log "Log: $log"
    Assert-FfxvrGameClosed 'the mod''s DLLs'
}
$dest = Join-Path $modsDir $modId
Log ''
Log "[deploy] -> $dest"
# Keep the user's files: settings, hand calibration, bone labels, captures (img_*.bmp, seq_*.bmp,
# ui_atlas_*.dds, ffxvr_bones_*.bin) and logs live next to the DLLs. Only the build output is deleted
# and replaced. The managed output has no subfolders (owner checked, 24 Oct 2026), so the delete is not recursive: a hand-made
# subfolder (an old\ copy for A/B tests) survives. ffxvr.manifest.json is overwritten by the copy.
# Any delete or copy error stops the deploy and says so (a locked DLL used to leave a half-new
# folder behind with exit code 0).
$buildOut = @('*.dll', '*.pdb', '*.deps.json', '*.runtimeconfig.json')
try {
    if (Test-Path -LiteralPath $dest) {
        Get-ChildItem -LiteralPath $dest -File | Where-Object {
            $n = $_.Name; [bool]($buildOut | Where-Object { $n -like $_ })
        } | Remove-Item -Force -ErrorAction Stop
    }
    New-Item -ItemType Directory -Force -Path $dest -ErrorAction Stop | Out-Null
    Get-ChildItem -LiteralPath $outDir | Copy-Item -Destination $dest -Recurse -Force -ErrorAction Stop
    Copy-Item -LiteralPath $natDll          -Destination $dest -Force -ErrorAction Stop
    Copy-Item -LiteralPath $oxrDll.FullName -Destination $dest -Force -ErrorAction Stop
    # VR hands: the shipped hand packs -> <mod>\hands\<name>\ (their files replaced). A pack folder of the user's own is
    # never touched (tools\paths.ps1 $FfxvrHandPacks is the whole list).
    foreach ($pk in $FfxvrHandPacks) {
        $from = Join-Path $proj "assets\hands\$pk"
        if (-not (Test-Path -LiteralPath (Join-Path $from 'right.glb'))) { throw "assets\hands\$pk\right.glb is missing from the repository (tools\hands\make-generic-hand.py writes it)" }
        $to = Join-Path $dest "hands\$pk"
        New-Item -ItemType Directory -Force -Path $to -ErrorAction Stop | Out-Null
        Get-ChildItem -LiteralPath $from -File | Copy-Item -Destination $to -Force -ErrorAction Stop
    }
} catch {
    Log ''
    Log "ERROR: deploy failed: $($_.Exception.Message)"
    Log '  The mod folder may be incomplete. Close whatever holds it (the game, an Explorer preview,'
    Log '  an antivirus scan) and run build-mod again.'
    Log "Log: $log"
    exit 1
}

Get-ChildItem -LiteralPath $dest -File | ForEach-Object { Log ('  ' + $_.Name + '  (' + $_.Length + ' bytes)') }
foreach ($pk in $FfxvrHandPacks) { Get-ChildItem -LiteralPath (Join-Path $dest "hands\$pk") -File | ForEach-Object { Log ("  hands\$pk\" + $_.Name + '  (' + $_.Length + ' bytes)') } }
$shipped = @($FfxvrHandPacks | ForEach-Object { [IO.Path]::Combine($dest, 'hands', $_) + [IO.Path]::DirectorySeparatorChar })
$kept = @(Get-ChildItem -LiteralPath $dest -Recurse -File | Where-Object { $n = $_.Name; $fn = $_.FullName
          -not ($buildOut | Where-Object { $n -like $_ }) -and $n -notlike '*.manifest.json' -and -not ($shipped | Where-Object { $fn.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) }) })
Log ("[deploy] kept {0} settings/capture/log files ({1:N1} MB)" -f $kept.Count, (($kept | Measure-Object Length -Sum).Sum / 1MB))

if (-not (Test-Path -LiteralPath (Join-Path $dest "$modId.manifest.json"))) {
    Log ''
    Log 'WARNING: no manifest.json deployed. Fahrenheit needs one to load the mod.'
}

# ---------------------------------------------------------------- loadorder
$loadorder = Join-Path $modsDir 'loadorder'
$lines = @()
if (Test-Path -LiteralPath $loadorder) {
    $lines = @(Get-Content -LiteralPath $loadorder | Where-Object { $_.Trim() -ne '' })
}
if ($lines -notcontains $modId) {
    $lines += $modId
    Set-Content -LiteralPath $loadorder -Value $lines -Encoding ascii
    Log ''
    Log "[loadorder] added '$modId'"
} else {
    Log ''
    Log "[loadorder] '$modId' already listed"
}
Log ('  contents: ' + ((Get-Content -LiteralPath $loadorder) -join ', '))

Log ''
Log ('=' * 70)
Log 'Done. Now run:  tools\run-ffx.ps1'
Log '  Logging: tools\set-logging.ps1 -Profile Dev turns the diagnostics on (Release: off, as a fresh install).'
Log '  Logs and captures land beside the mod DLL and are collected by run-ffx when the game exits.'
Log "Log: $log"
