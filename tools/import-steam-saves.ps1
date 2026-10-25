# import-steam-saves.ps1
#
# Copies your normal (Steam) FFX saves into Fahrenheit's default save set, so
# they show up when the game runs under the mod loader. One-way: nothing in
# the Steam folder is touched, and saves made under the mod stay in
# Fahrenheit's set.
#
#   Steam:      Documents\SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster\[<steam id>\]FINAL FANTASY X\ffx_NNN
#   Fahrenheit: <game>\fahrenheit\saves\0\default\FINAL FANTASY X\ffx_NNN
#
# Steam and Fahrenheit number their slots separately, so ffx_003 on each side can be two different
# games. A slot that exists on both sides with different contents is KEPT (Fahrenheit's save wins)
# unless you pass -Force; the Fahrenheit saves are then backed up to logs\ first.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\import-steam-saves.ps1
#   ... -Force               replace every clashing Fahrenheit slot with the Steam one (after a backup)
#   ... -Force -Slot 3       replace only ffx_003 (several: -Slot "3,5")
#   ... -SteamId <number>    which Steam account's saves, when there are several

param(
    [switch]$Force,
    [string]$Slot = '',      # a string, not [int[]]: with -File, "3,5" would bind as the number 35
    [string]$SteamId = ''
)

. (Join-Path $PSScriptRoot 'paths.ps1')
$slots = @()
foreach ($s in @($Slot -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ })) {
    if ($s -notmatch '^\d{1,3}\z') { Write-Host "-Slot: '$s' is not a slot number (ffx_003 is -Slot 3)"; exit 1 }
    $slots += ('ffx_{0:D3}' -f [int]$s)
}
$game = Get-FfxvrGame
Assert-FfxvrGameClosed 'the save files'
$dest = Join-Path $game 'fahrenheit\saves\0\default\FINAL FANTASY X'

$from = Get-FfxvrSteamSaveDir $SteamId
if (-not $from) { Write-Host 'No Steam FFX save folder found under Documents\SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster'; exit 1 }
$files = @(Get-ChildItem -LiteralPath $from -File -Filter 'ffx_*')
if ($files.Count -eq 0) { Write-Host "No ffx_* saves in $from"; exit 1 }

Assert-FfxvrWritable $dest
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$new = @(); $same = 0; $clash = @()
foreach ($f in $files) {
    $target = Join-Path $dest $f.Name
    if (-not (Test-Path -LiteralPath $target)) { $new += $f; continue }
    if ((Get-FileHash -LiteralPath $target).Hash -eq (Get-FileHash -LiteralPath $f.FullName).Hash) { $same++; continue }
    $clash += $f
}
$replace = @($clash | Where-Object { $Force -and ($slots.Count -eq 0 -or $slots -contains $_.Name) })

if ($Slot -and -not $Force) { Write-Host 'Note: -Slot has no effect without -Force (nothing is replaced).' }

# With -Force the backup is the only other copy of the VR saves: it must be complete before
# anything is replaced.
if ($replace.Count -gt 0) {
    $logs = Join-Path $FfxvrRepo 'logs'
    Assert-FfxvrWritable $logs
    $backup = Join-Path $logs ('fahrenheit-saves-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    try {
        New-Item -ItemType Directory -Force -Path $backup -ErrorAction Stop | Out-Null
        Get-ChildItem -LiteralPath $dest -File | Copy-Item -Destination $backup -Force -ErrorAction Stop
    } catch {
        Write-Host "ERROR: backing up the Fahrenheit saves to $backup failed: $($_.Exception.Message)"
        Write-Host 'Nothing was replaced.'
        exit 1
    }
    $names = @(Get-ChildItem -LiteralPath $backup -File | ForEach-Object { $_.Name })
    $want  = @(Get-ChildItem -LiteralPath $dest -File).Count
    if ($names.Count -ne $want) {
        Write-Host "ERROR: the backup in $backup has $($names.Count) of $want files. Nothing was replaced."
        exit 1
    }
    Write-Host "Backed up the Fahrenheit saves ($($names -join ', ')) to $backup"
}
foreach ($f in @($new + $replace)) {
    Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $dest $f.Name) -Force
}

Write-Host "Imported $($new.Count) new save(s) and replaced $($replace.Count) from $from"
Write-Host "$same already identical."
$kept = @($clash | Where-Object { $replace -notcontains $_ })
foreach ($f in $kept) {
    $vr = Get-Item -LiteralPath (Join-Path $dest $f.Name)
    $which = if ($f.LastWriteTime -gt $vr.LastWriteTime) { 'Steam newer' } elseif ($f.LastWriteTime -lt $vr.LastWriteTime) { 'VR newer' } else { 'same time' }
    Write-Host ('  kept {0}: Steam {1:yyyy-MM-dd HH:mm}, VR {2:yyyy-MM-dd HH:mm} ({3})' -f $f.Name, $f.LastWriteTime, $vr.LastWriteTime, $which)
}
foreach ($n in $slots) {
    if ($Force -and -not @($clash | Where-Object { $_.Name -eq $n })) { Write-Host "  ${n}: not a clash (new, identical or absent), nothing to replace" }
}
if ($kept.Count -gt 0) {
    Write-Host '  Steam and Fahrenheit number their slots separately, so a kept slot may be a different game.'
    Write-Host '  -Force -Slot 3 replaces just ffx_003 (or -Force alone for all of them), after a backup in logs\.'
}
Write-Host "Fahrenheit set: $dest"
