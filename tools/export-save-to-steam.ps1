# export-save-to-steam.ps1
#
# Copies a save made under Fahrenheit into your normal (Steam) FFX save set,
# so you can load it in the base game (launched from Steam, no mod loader).
# The Steam folder is backed up first and the save goes into an UNUSED slot,
# so nothing of yours is overwritten. Same file format both sides (ffx_NNN).
#
#   Fahrenheit: <game>\fahrenheit\saves\0\default\FINAL FANTASY X\ffx_NNN
#   Steam:      Documents\SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster\[<steam id>\]FINAL FANTASY X\ffx_NNN  (OneDrive Documents too)
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\export-save-to-steam.ps1            # newest Fahrenheit save
#   powershell -ExecutionPolicy Bypass -File .\tools\export-save-to-steam.ps1 -Slot 2    # Fahrenheit slot ffx_002
#   ... -ToSlot 4     to choose the Steam slot (must be free unless -Force)
#   ... -SteamId <number>   which Steam account, when there are several
#
# Steam Cloud: a new file in the folder is uploaded on the next sync; if Steam
# shows a cloud-conflict dialog, pick "upload local".

param(
    [int]$Slot = -1,      # Fahrenheit slot number; -1 = newest file
    [int]$ToSlot = -1,    # Steam slot number; -1 = the lowest unused slot (from ffx_000)
    [switch]$Force,       # allow overwriting an existing Steam slot
    [string]$SteamId = ''
)

. (Join-Path $PSScriptRoot 'paths.ps1')
$game = Get-FfxvrGame
Assert-FfxvrGameClosed 'the Steam save files'
$fh   = Join-Path $game 'fahrenheit\saves\0\default\FINAL FANTASY X'

if (-not (Test-Path -LiteralPath $fh)) { Write-Host "No Fahrenheit saves at $fh"; exit 1 }

# Source
if ($Slot -ge 0) {
    $src = Get-Item -LiteralPath (Join-Path $fh ('ffx_{0:D3}' -f $Slot)) -ErrorAction SilentlyContinue
    if (-not $src) { Write-Host "No Fahrenheit save ffx_$('{0:D3}' -f $Slot)"; exit 1 }
}
else {
    $src = Get-ChildItem -LiteralPath $fh -File -Filter 'ffx_*' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $src) { Write-Host "No ffx_* saves in $fh"; exit 1 }
}
Write-Host "Source: $($src.FullName)  ($($src.LastWriteTime))"

# Destination: saves live either directly in the game's Documents folder or under a numbered
# Steam-account folder (the one with the newest save if several; -SteamId chooses).
$dest = Get-FfxvrSteamSaveDir $SteamId
if (-not $dest) { Write-Host 'No Steam FFX save folder found (Documents\SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster).'; exit 1 }

# Backup
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
# Verified before anything is written to the Steam folder (-Force can overwrite a slot).
$logs   = Join-Path $FfxvrRepo 'logs'
Assert-FfxvrWritable $logs
$backup = Join-Path $logs "steam-saves-backup-$stamp"
try {
    New-Item -ItemType Directory -Force -Path $backup -ErrorAction Stop | Out-Null
    Get-ChildItem -LiteralPath $dest -File | Copy-Item -Destination $backup -Force -ErrorAction Stop
} catch {
    Write-Host "ERROR: backing up $dest to $backup failed: $($_.Exception.Message)"
    Write-Host 'Nothing was copied to Steam.'
    exit 1
}
$got  = @(Get-ChildItem -LiteralPath $backup -File).Count
$want = @(Get-ChildItem -LiteralPath $dest -File).Count
if ($got -ne $want) { Write-Host "ERROR: the backup in $backup has $got of $want files. Nothing was copied to Steam."; exit 1 }
Write-Host "Backed up $dest -> $backup"

# Target slot: the lowest unused number (a gap left by a deleted save is reused)
if ($ToSlot -lt 0) {
    $ToSlot = 0
    while (Test-Path -LiteralPath (Join-Path $dest ('ffx_{0:D3}' -f $ToSlot))) { $ToSlot++ }
}
$target = Join-Path $dest ('ffx_{0:D3}' -f $ToSlot)
if ((Test-Path -LiteralPath $target) -and -not $Force) { Write-Host "Steam slot ffx_$('{0:D3}' -f $ToSlot) exists; pass -Force to overwrite or choose another -ToSlot"; exit 1 }

Copy-Item -LiteralPath $src.FullName -Destination $target -Force
Write-Host "Copied to $target"
Write-Host "In the base game it is save slot $($ToSlot + 1) (file ffx_$('{0:D3}' -f $ToSlot)). Launch FFX from Steam and load it."
