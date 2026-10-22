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
#
# Steam Cloud: a new file in the folder is uploaded on the next sync; if Steam
# shows a cloud-conflict dialog, pick "upload local".

param(
    [int]$Slot = -1,      # Fahrenheit slot number; -1 = newest file
    [int]$ToSlot = -1,    # Steam slot number; -1 = first free slot at or above 10
    [switch]$Force        # allow overwriting an existing Steam slot
)

. (Join-Path $PSScriptRoot 'paths.ps1')
$game = Get-FfxvrGame
$fh   = Join-Path $game 'fahrenheit\saves\0\default\FINAL FANTASY X'
$docs = [Environment]::GetFolderPath('MyDocuments')
$root = Join-Path $docs 'SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster'

if (-not (Test-Path $fh)) { Write-Host "No Fahrenheit saves at $fh"; exit 1 }
if (-not (Test-Path $root)) { Write-Host "No Steam save folder at $root"; exit 1 }

# Source
if ($Slot -ge 0) {
    $src = Get-Item (Join-Path $fh ('ffx_{0:D3}' -f $Slot)) -ErrorAction SilentlyContinue
    if (-not $src) { Write-Host "No Fahrenheit save ffx_$('{0:D3}' -f $Slot)"; exit 1 }
}
else {
    $src = Get-ChildItem $fh -File -Filter 'ffx_*' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $src) { Write-Host "No ffx_* saves in $fh"; exit 1 }
}
Write-Host "Source: $($src.FullName)  ($($src.LastWriteTime))"

# Destination: saves live either directly in the game folder or under a
# numbered Steam-account folder (newest if several).
$dest = $null
if (Test-Path (Join-Path $root 'FINAL FANTASY X')) { $dest = Join-Path $root 'FINAL FANTASY X' }
else {
    $acct = Get-ChildItem $root -Directory |
            Where-Object { Test-Path (Join-Path $_.FullName 'FINAL FANTASY X') } |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($acct) { $dest = Join-Path $acct.FullName 'FINAL FANTASY X' }
}
if (-not $dest) { Write-Host "No FINAL FANTASY X save folder under $root"; exit 1 }

# Backup
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$backup = Join-Path (Join-Path $FfxvrRepo 'logs') "steam-saves-backup-$stamp"
New-Item -ItemType Directory -Force -Path $backup | Out-Null
Get-ChildItem $dest -File | Copy-Item -Destination $backup -Force
Write-Host "Backed up $dest -> $backup"

# Target slot: the next one after the existing saves (the game lists slots in order)
if ($ToSlot -lt 0) {
    $ToSlot = 0
    while (Test-Path (Join-Path $dest ('ffx_{0:D3}' -f $ToSlot))) { $ToSlot++ }
}
$target = Join-Path $dest ('ffx_{0:D3}' -f $ToSlot)
if ((Test-Path $target) -and -not $Force) { Write-Host "Steam slot ffx_$('{0:D3}' -f $ToSlot) exists; pass -Force to overwrite or choose another -ToSlot"; exit 1 }

Copy-Item $src.FullName -Destination $target -Force
Write-Host "Copied to $target"
Write-Host "In the base game it is save slot $($ToSlot + 1) (file ffx_$('{0:D3}' -f $ToSlot)). Launch FFX from Steam and load it."
