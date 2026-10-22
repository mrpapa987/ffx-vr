# paths.ps1 - where things are, for every script in tools\. Dot-sourced:
#     . (Join-Path $PSScriptRoot 'paths.ps1')
#
# $FfxvrRepo   this repository (the folder above tools\), wherever it was cloned
# Get-FfxvrGame  the game's install folder, found in this order:
#   1. the environment variable FFXVR_GAME_DIR
#   2. tools\paths.local.ps1 (your own file, not in git), containing one line such as
#          $FfxvrGameOverride = 'D:\SteamLibrary\steamapps\common\FINAL FANTASY FFX&FFX-2 HD Remaster'
#   3. Steam: its install folder from the registry, plus every library in libraryfolders.vdf
# A folder only counts if FFX.exe is in it. Nothing here points at any one person's PC.

$FfxvrRepo         = Split-Path -Parent $PSScriptRoot
$FfxvrGameName     = 'FINAL FANTASY FFX&FFX-2 HD Remaster'
$FfxvrGameOverride = $null
$ffxvrLocal = Join-Path $PSScriptRoot 'paths.local.ps1'
if (Test-Path $ffxvrLocal) { . $ffxvrLocal }

function Test-FfxvrGame([string]$dir) {
    return ($dir -and (Test-Path (Join-Path $dir 'FFX.exe')))
}

function Find-FfxvrGame {
    if (Test-FfxvrGame $env:FFXVR_GAME_DIR) { return $env:FFXVR_GAME_DIR }
    if (Test-FfxvrGame $FfxvrGameOverride)  { return $FfxvrGameOverride }
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { return $null }
    $steam = $steam -replace '/', '\'
    $libs = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in (Select-String -Path $vdf -Pattern '"path"\s+"(.+?)"' -AllMatches)) {
            foreach ($g in $m.Matches) { $libs += ($g.Groups[1].Value -replace '\\\\', '\') }
        }
    }
    foreach ($lib in ($libs | ForEach-Object { $_.TrimEnd('\') } | Sort-Object -Unique)) {
        $dir = Join-Path $lib (Join-Path 'steamapps\common' $FfxvrGameName)
        if (Test-FfxvrGame $dir) { return $dir }
    }
    return $null
}

# The game folder, or a clear message and exit.
function Get-FfxvrGame {
    $dir = Find-FfxvrGame
    if ($dir) { return $dir }
    Write-Host ''
    Write-Host "Could not find the game ($FfxvrGameName, FFX.exe)."
    Write-Host 'Tell the scripts where it is, either way:'
    Write-Host "  - set FFXVR_GAME_DIR, e.g.  `$env:FFXVR_GAME_DIR = 'D:\SteamLibrary\steamapps\common\$FfxvrGameName'"
    Write-Host "  - or create tools\paths.local.ps1 with the line"
    Write-Host "        `$FfxvrGameOverride = 'D:\SteamLibrary\steamapps\common\$FfxvrGameName'"
    exit 1
}
