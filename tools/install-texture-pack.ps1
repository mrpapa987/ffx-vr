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
# CAUTION (Nov 2026): the Re-Remastered pack caused performance problems in VR on the developer's PC (an RTX 3080 Ti):
# it is not recommended without stronger hardware. It also needs a lot of disk space: the Medium (2.5x) pack is about
# 18 GB of loose files once installed (-Compress roughly halves that with NTFS compression), on top of the downloaded
# archive. -Uninstall removes it again. The script prints this before it extracts anything.
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
# -Id: letters, digits, '-' and '_' only. -Uninstall only removes a folder this script created
# (its manifest says so); it never touches the VR mod or another code mod.

param(
    [string[]]$Archive = @(),
    [string]$Id = 'reremaster',
    [string]$Name = 'FFX Re-Remastered textures (Nexus 244)',
    [switch]$Compress,
    [switch]$Uninstall,
    [switch]$KeepTemp
)

$ErrorActionPreference = 'Stop'

# The id becomes a folder under fahrenheit\mods that -Uninstall deletes. No dots: Windows drops
# trailing dots, so 'ffxvr.' would be the VR mod's own folder. \z, not $ ($ also matches before a
# final newline).
if ($Id -notmatch '^[A-Za-z0-9][A-Za-z0-9_-]{0,63}\z' -or $Id -eq 'ffxvr' -or $Id -match '^(con|nul|aux|prn|com\d|lpt\d)\z') {
    throw "-Id '$Id' is not allowed: letters, digits, '-' and '_' only, and not 'ffxvr' (the VR mod)."
}

# powershell -File passes "-Archive a.7z,b.7z" as ONE string: split it, unless it is a real file
# whose name has a comma in it.
$Archive = @($Archive | Where-Object { $_ } | ForEach-Object { if (Test-Path -LiteralPath $_) { $_ } else { $_ -split ',' } } |
             ForEach-Object { $_.Trim() } | Where-Object { $_ })

. (Join-Path $PSScriptRoot 'paths.ps1')
$game   = Get-FfxvrGame
$mods   = Join-Path $game 'fahrenheit\mods'
$modDir = Join-Path $mods $Id
$eflX   = Join-Path $modDir 'efl\x'
$order  = Join-Path $mods 'loadorder'
$proj   = $FfxvrRepo
$log    = Join-Path $proj ("logs\install-texture-pack-{0}.txt" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force -Path (Split-Path $log) | Out-Null
function Log($m) { Write-FfxvrLog $log $m }

# Ours = this script's manifest marker (written first, before any extraction) or an efl\ folder,
# and no DLL at the root (a Fahrenheit code mod has its DLL there).
function Test-OurPack([string]$d, [string]$id) {
    $m = Join-Path $d "$id.manifest.json"
    $marker = (Test-Path -LiteralPath $m) -and ([IO.File]::ReadAllText($m) -match 'install-texture-pack\.ps1')
    $dlls   = @(Get-ChildItem -LiteralPath $d -File -Filter *.dll -ErrorAction SilentlyContinue).Count
    return ($dlls -eq 0) -and ($marker -or (Test-Path -LiteralPath (Join-Path $d 'efl')))
}

if (!(Test-Path -LiteralPath $mods)) { throw "Fahrenheit mods folder not found: $mods" }
Assert-FfxvrGameClosed 'the texture files'
Assert-FfxvrWritable $mods
if ((Test-Path -LiteralPath $modDir) -and -not (Test-OurPack $modDir $Id)) {
    throw "$modDir belongs to another mod (it has DLLs, or no manifest from this script). Pick another -Id; nothing was changed."
}

if ($Uninstall) {
    if (Test-Path -LiteralPath $order) {
        $lines = @(Get-Content -LiteralPath $order | Where-Object { $_.Trim() -ne $Id })
        Set-Content -LiteralPath $order -Value $lines -Encoding ascii
        Log "removed $Id from loadorder"
    }
    if (Test-Path -LiteralPath $modDir) { Remove-Item -LiteralPath $modDir -Recurse -Force; Log "removed $modDir" }
    exit 0
}

# 7-Zip
$sevenZip = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe", (Get-Command 7z -ErrorAction SilentlyContinue).Source) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
if (!$sevenZip) { throw "7-Zip not found (install from 7-zip.org; the pack is a .7z)" }
if ($Archive.Count -eq 0) { throw "pass -Archive <path to the downloaded .7z (or .001)>" }
foreach ($a in $Archive) { if (!(Test-Path -LiteralPath $a)) { throw "archive not found: $a" } }

Log 'CAUTION: the Re-Remastered pack caused performance problems in VR on the developer''s PC (an RTX 3080 Ti):'
Log '         not recommended without stronger hardware. It also needs a lot of disk space: the Medium pack is about'
Log '         18 GB of loose files once installed (-Compress roughly halves that). -Uninstall removes it again.'

# Mod skeleton
New-Item -ItemType Directory -Force -Path $eflX | Out-Null
$manifest = Join-Path $modDir "$Id.manifest.json"
if (!(Test-Path -LiteralPath $manifest)) {
    @{ Id = $Id; Name = $Name; Desc = 'AI-upscaled textures and UI (loose files served by the Fahrenheit file loader)'; Authors = 'HitokageWorshipper (pack); installed by install-texture-pack.ps1'; Version = '4.0'; Link = 'https://www.nexusmods.com/finalfantasyxx2hdremaster/mods/244'; Dependencies = @(); Flags = 'NONE' } |
        ConvertTo-Json | Set-Content -LiteralPath $manifest -Encoding UTF8
    Log "wrote $manifest"
}

# Extract to a temp folder on the same drive (a move is then a rename, not a copy)
$tmp = Join-Path $mods ("_extract_{0}" -f $Id)
if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
New-Item -ItemType Directory -Force -Path $tmp | Out-Null
foreach ($a in $Archive) {
    Log "extracting $a ..."
    & $sevenZip x "-o$tmp" -y -bsp1 "$a" | ForEach-Object { Log $_ }
    if ($LASTEXITCODE -ne 0) { throw "7-Zip failed on $a (exit $LASTEXITCODE)" }
}

# Find the VBF root inside what was extracted: the folder that contains ffx_ps2\ (and/or ffx_data\)
$root = $null
$candidates = @($tmp) + @(Get-ChildItem -LiteralPath $tmp -Directory -Recurse -Depth 4 | ForEach-Object { $_.FullName })
foreach ($c in $candidates) {
    if ((Test-Path -LiteralPath (Join-Path $c 'ffx_ps2')) -or (Test-Path -LiteralPath (Join-Path $c 'ffx_data'))) { $root = $c; break }
}
if (!$root) { throw "could not find ffx_ps2\ or ffx_data\ in the extracted archive (looked under $tmp)" }
Log "VBF root in the archive: $root"

# Merge into efl\x (existing files are replaced); only the VBF's own folders
$moved = 0
Get-ChildItem -LiteralPath $root -Directory | Where-Object { $_.Name -in @('ffx_ps2', 'ffx_data', 'ffx2_ps2', 'ffx2_data') } | ForEach-Object {
    $dst = Join-Path $eflX $_.Name
    if (Test-Path -LiteralPath $dst) {
        # merge: robocopy moves files, keeps the tree
        & robocopy $_.FullName $dst /E /MOVE /NFL /NDL /NJH /NJS /R:1 /W:1 | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "robocopy failed merging $($_.Name) (exit $LASTEXITCODE)" }
    }
    else { Move-Item -LiteralPath $_.FullName -Destination $dst }
    $moved++
}
Log "merged $moved top-level folders into $eflX"
if (!$KeepTemp) { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }

# Load order: before ffxvr
$lines = @()
if (Test-Path -LiteralPath $order) { $lines = @(Get-Content -LiteralPath $order | Where-Object { $_.Trim() -ne '' }) }
if ($lines -notcontains $Id) {
    $i = [array]::IndexOf($lines, 'ffxvr')
    if ($i -gt 0)      { $lines = @($lines[0..($i-1)]) + @($Id) + @($lines[$i..($lines.Count-1)]) }
    elseif ($i -eq 0)  { $lines = @($Id) + $lines }     # ($lines[0..-1] would wrap round: no slice here)
    else               { $lines = @($Id) + $lines }
    Set-Content -LiteralPath $order -Value $lines -Encoding ascii
    Log "loadorder is now: $($lines -join ', ')"
}

if ($Compress) {
    Log "enabling NTFS compression on $modDir (this takes a while) ..."
    & compact /c /s:"$modDir" /i /q | Select-Object -Last 3 | ForEach-Object { Log $_ }
}

$files = @(Get-ChildItem -LiteralPath $eflX -File -Recurse)
$n  = $files.Count
$gb = [math]::Round((($files | Measure-Object -Property Length -Sum).Sum / 1GB), 1)
Log "installed: $n files, $gb GB on disk (before compression) in $eflX"
Log "Fahrenheit indexes the files at launch; the module log Fahrenheit.Runtime.FhFileLoaderModule.log lists every replaced file."
