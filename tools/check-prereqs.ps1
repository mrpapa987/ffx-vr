# check-prereqs.ps1
#
# READ-ONLY. This script does not install, change or delete anything.
# It reports which build tools and VR settings are present, then writes the
# results to ..\logs\prereqs.txt so they can be reviewed.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File C:\projects\FFX-VR\tools\check-prereqs.ps1

$ErrorActionPreference = 'SilentlyContinue'

$root   = Split-Path -Parent $PSScriptRoot
$logDir = Join-Path $root 'logs'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$outFile = Join-Path $logDir 'prereqs.txt'

$lines = @()
function Add-Line($s) { $script:lines += $s; Write-Host $s }

Add-Line "FFX VR prerequisite check - $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
Add-Line ('=' * 64)

# ---------------------------------------------------------------- OS
Add-Line ''
Add-Line '[Operating system]'
Add-Line ('  ' + (Get-CimInstance Win32_OperatingSystem).Caption)
Add-Line ('  Build: ' + [System.Environment]::OSVersion.Version)

# ---------------------------------------------------------------- .NET
Add-Line ''
Add-Line '[.NET SDKs]  (need a 10.x entry)'
if (Get-Command dotnet -ErrorAction SilentlyContinue) {
    $sdks = & dotnet --list-sdks 2>&1
    if ($sdks) { $sdks | ForEach-Object { Add-Line "  $_" } } else { Add-Line '  none reported' }
} else {
    Add-Line '  dotnet not found on PATH'
}

Add-Line ''
Add-Line '[.NET runtimes, x86]  (the Fahrenheit mod is a 32-bit managed plugin)'
$dotnet86 = Join-Path ${env:ProgramFiles(x86)} 'dotnet\dotnet.exe'
if (Test-Path $dotnet86) {
    $rts = & $dotnet86 --list-runtimes 2>&1 | Where-Object { $_ -match 'Microsoft\.NETCore\.App' }
    if ($rts) { $rts | ForEach-Object { Add-Line "  $_" } } else { Add-Line '  none reported' }
} else {
    Add-Line "  no x86 dotnet found at $dotnet86"
}

# ---------------------------------------------------------------- Visual Studio
Add-Line ''
Add-Line '[Visual Studio C++ tools]  (needed for the native DLL)'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path $vswhere) {
    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -format value -property installationPath 2>&1
    if ($vsPath) {
        $vsVer = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -format value -property installationVersion 2>&1
        Add-Line "  found: $vsPath"
        Add-Line "  version: $vsVer"
    } else {
        Add-Line '  Visual Studio is installed, but the C++ (MSVC) workload is NOT'
    }
} else {
    Add-Line '  vswhere.exe not found - Visual Studio is probably not installed'
}

# ---------------------------------------------------------------- Other tools
Add-Line ''
Add-Line '[Other tools]'
Add-Line ('  PowerShell running this script: ' + $PSVersionTable.PSVersion)
if (Get-Command pwsh -ErrorAction SilentlyContinue) {
    Add-Line ('  pwsh (PowerShell 7): ' + (& pwsh -NoProfile -Command '$PSVersionTable.PSVersion.ToString()' 2>&1))
} else {
    Add-Line '  pwsh (PowerShell 7): not found'
}
if (Get-Command git -ErrorAction SilentlyContinue) {
    Add-Line ('  git: ' + (& git --version 2>&1))
} else {
    Add-Line '  git: not found'
}

# ---------------------------------------------------------------- OpenXR
Add-Line ''
Add-Line '[OpenXR runtime]  *** the critical check ***'
Add-Line '  FFX.exe is 32-bit, so the 32-bit entry is the one that decides whether this can work.'
$rt64 = (Get-ItemProperty 'HKLM:\SOFTWARE\Khronos\OpenXR\1' -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime
$rt32 = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenXR\1' -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime
if ($rt64) { Add-Line "  64-bit: $rt64" } else { Add-Line '  64-bit: (none registered)' }
if ($rt32) { Add-Line "  32-bit: $rt32" } else { Add-Line '  32-bit: (none registered)  <-- must be set before the mod can work' }

# ---------------------------------------------------------------- Steam
Add-Line ''
Add-Line '[Steam, game and SteamVR]'
$steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
if ($steam) {
    $steam = $steam -replace '/', '\'
    Add-Line "  Steam: $steam"

    $libs = @($steam)
    $vdf  = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        $matches = Select-String -Path $vdf -Pattern '"path"\s+"(.+?)"' -AllMatches
        foreach ($m in $matches) {
            foreach ($g in $m.Matches) { $libs += ($g.Groups[1].Value -replace '\\\\', '\') }
        }
    }
    $libs = $libs | ForEach-Object { $_.TrimEnd('\') } | Sort-Object -Unique

    foreach ($lib in $libs) {
        $common = Join-Path $lib 'steamapps\common'
        if (Test-Path $common) {
            Get-ChildItem $common -Directory -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -like '*FINAL FANTASY*' -or $_.Name -like 'SteamVR*' } |
                ForEach-Object { Add-Line ('  found: ' + $_.FullName) }
        }
    }

    # Warn if the game sits somewhere that needs elevation to write to
    foreach ($lib in $libs) {
        $g = Join-Path $lib 'steamapps\common\FINAL FANTASY FFX&FFX-2 HD Remaster'
        if ((Test-Path $g) -and ($g -like '*Program Files*')) {
            Add-Line '  NOTE: the game is under Program Files - writing mod files there needs elevation.'
            Add-Line '        Moving it to a Steam library on another drive avoids that.'
        }
    }

    # SteamVR version, if a version file is present
    foreach ($lib in $libs) {
        $svr = Join-Path $lib 'steamapps\common\SteamVR'
        if (Test-Path $svr) {
            $vfile = Join-Path $svr 'bin\version.txt'
            if (Test-Path $vfile) {
                Add-Line ('  SteamVR build id (read the real version from SteamVR > Settings > About): ' + ((Get-Content $vfile -Raw) -replace '\s+', ' ').Trim())
            } else {
                Add-Line '  SteamVR found, but no bin\version.txt - read the version from'
                Add-Line '        SteamVR > Settings > About instead (need 2.17 or newer)'
            }
        }
    }
} else {
    Add-Line '  Steam install path not found in the registry'
}

Add-Line ''
Add-Line ('=' * 64)
Add-Line 'End of check.'

$lines | Set-Content -Path $outFile -Encoding UTF8
Write-Host ''
Write-Host "Saved to $outFile"
