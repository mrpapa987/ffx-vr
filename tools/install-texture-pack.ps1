# install-texture-pack.ps1
#
# Installs an External-File-Loader style texture pack (loose files that
# mirror the VBF archive, e.g. HitokageWorshipper's "Re-Remastered" pack,
# Nexus mod 244) as a FILE-ONLY FAHRENHEIT MOD. Do NOT install ffgriever's
# External File Loader DLL next to Fahrenheit: Fahrenheit's own file loader
# hooks the same game functions (BigFileStream::openFile, PStreamFile) and
# serves loose files from <game>\fahrenheit\mods\<id>\efl\x\..., where efl\x
# is the VBF root: ffx_ps2\... and ffx_data\gamedata\... side by side, exactly
# the tree EFL reads from data\mods\. (The game asks for "/FFX_Data/GameData/..."
# and "/ffx_ps2/...": FFX_Data is the VBF's ffx_data folder, case-insensitive.)
#
# What it does:
#   1. creates <game>\fahrenheit\mods\<Id>\ with a manifest (no DLL = file-only mod)
#   2. extracts the archive(s) with 7-Zip into that mod's efl\x\ (finds the VBF
#      root in the archive: the folder holding ffx_ps2\ and/or ffx_data\; anything
#      else at that level, such as a bundled EFL loader, is left out)
#   3. adds <Id> to fahrenheit\mods\loadorder BEFORE ffxvr (ffxvr's own efl files, if
#      any, win conflicts)
#   4. optionally turns on NTFS compression for the mod folder (the pack author's
#      tip: roughly halves the size)
#
#   powershell -ExecutionPolicy Bypass -File .\tools\install-texture-pack.ps1 `
#       -Archive "$env:USERPROFILE\Downloads\<the pack>.7z" [-Id reremaster] [-Compress] [-Uninstall]
#
# Multi-part archives: pass the .001 file. Several archives: -Archive a.7z,b.7z.

param(
    [string[]]$Archive = @(),
    [string]$Id = 'reremaster',
    [string]$Name = 'FFX Re-Remastered textures (Nexus 244)',
    [switch]$Compress,
    [switch]$Uninstall,
    [switch]$KeepTemp
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$game   = Get-FfxvrGame
$mods   = Join-Path $game 'fahrenheit\mods'
$modDir = Join-Path $mods $Id
$eflX   = Join-Path $modDir 'efl\x'
$order  = Join-Path $mods 'loadorder'
$proj   = $FfxvrRepo
$log    = Join-Path $proj ("logs\install-texture-pack-{0}.txt" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
function Log($m) { $m | Tee-Object -FilePath $log -Append }

if (!(Test-Path $mods)) { throw "Fahrenheit mods folder not found: $mods" }

if ($Uninstall) {
    if (Test-Path $order) {
        $lines = Get-Content $order | Where-Object { $_.Trim() -ne $Id }
        Set-Content -Path $order -Value $lines
        Log "removed $Id from loadorder"
    }
    if (Test-Path $modDir) { Remove-Item -Recurse -Force $modDir; Log "removed $modDir" }
    exit 0
}

# 7-Zip
$sevenZip = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe", (Get-Command 7z -ErrorAction SilentlyContinue).Source) | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (!$sevenZip) { throw "7-Zip not found (install from 7-zip.org; the pack is a .7z)" }
if ($Archive.Count -eq 0) { throw "pass -Archive <path to the downloaded .7z (or .001)>" }
foreach ($a in $Archive) { if (!(Test-Path $a)) { throw "archive not found: $a" } }

# Mod skeleton
New-Item -ItemType Directory -Force -Path $eflX | Out-Null
$manifest = Join-Path $modDir "$Id.manifest.json"
if (!(Test-Path $manifest)) {
    @{ Id = $Id; Name = $Name; Desc = 'AI-upscaled textures and UI (loose files served by the Fahrenheit file loader)'; Authors = 'HitokageWorshipper (pack); installed by install-texture-pack.ps1'; Version = '4.0'; Link = 'https://www.nexusmods.com/finalfantasyxx2hdremaster/mods/244'; Dependencies = @(); Flags = 'NONE' } |
        ConvertTo-Json | Set-Content -Path $manifest -Encoding UTF8
    Log "wrote $manifest"
}

# Extract to a temp folder on the same drive (a move is then a rename, not a copy)
$tmp = Join-Path $mods ("_extract_{0}" -f $Id)
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
foreach ($a in $Archive) {
    Log "extracting $a ..."
    & $sevenZip x "-o$tmp" -y -bsp1 "$a" | Tee-Object -FilePath $log -Append
    if ($LASTEXITCODE -ne 0) { throw "7-Zip failed on $a (exit $LASTEXITCODE)" }
}

# Find the VBF root inside what was extracted: the folder that contains ffx_ps2\ (and/or ffx_data\)
$root = $null
$candidates = @($tmp) + (Get-ChildItem -Path $tmp -Directory -Recurse -Depth 4 | ForEach-Object { $_.FullName })
foreach ($c in $candidates) {
    if ((Test-Path (Join-Path $c 'ffx_ps2')) -or (Test-Path (Join-Path $c 'ffx_data'))) { $root = $c; break }
}
if (!$root) { throw "could not find ffx_ps2\ or ffx_data\ in the extracted archive (looked under $tmp)" }
Log "VBF root in the archive: $root"

# Merge into efl\x (existing files are replaced); only the VBF's own folders
$moved = 0
Get-ChildItem -Path $root -Directory | Where-Object { $_.Name -in @('ffx_ps2', 'ffx_data', 'ffx2_ps2', 'ffx2_data') } | ForEach-Object {
    $dst = Join-Path $eflX $_.Name
    if (Test-Path $dst) {
        # merge: robocopy moves files, keeps the tree
        & robocopy $_.FullName $dst /E /MOVE /NFL /NDL /NJH /NJS /R:1 /W:1 | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed merging $($_.Name) (exit $LASTEXITCODE)" }
    }
    else { Move-Item -Path $_.FullName -Destination $dst }
    $moved++
}
Log "merged $moved top-level folders into $eflX"
if (!$KeepTemp) { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }

# Load order: before ffxvr
$lines = @()
if (Test-Path $order) { $lines = @(Get-Content $order | Where-Object { $_.Trim() -ne '' }) }
if ($lines -notcontains $Id) {
    $i = [array]::IndexOf($lines, 'ffxvr')
    if ($i -gt 0)      { $lines = @($lines[0..($i-1)]) + @($Id) + @($lines[$i..($lines.Count-1)]) }
    elseif ($i -eq 0)  { $lines = @($Id) + $lines }     # ($lines[0..-1] would wrap round: no slice here)
    else               { $lines = @($Id) + $lines }
    Set-Content -Path $order -Value $lines
    Log "loadorder is now: $($lines -join ', ')"
}

if ($Compress) {
    Log "enabling NTFS compression on $modDir (this takes a while) ..."
    & compact /c /s:"$modDir" /i /q | Select-Object -Last 3 | Tee-Object -FilePath $log -Append
}

$n = (Get-ChildItem -Path $eflX -File -Recurse | Measure-Object).Count
$gb = [math]::Round(((Get-ChildItem -Path $eflX -File -Recurse | Measure-Object -Property Length -Sum).Sum / 1GB), 1)
Log "installed: $n files, $gb GB on disk (before compression) in $eflX"
Log "Fahrenheit indexes the files at launch; the module log Fahrenheit.Runtime.FhFileLoaderModule.log lists every replaced file."
