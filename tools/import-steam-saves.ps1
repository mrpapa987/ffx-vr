# import-steam-saves.ps1
#
# Copies your normal (Steam) FFX saves into Fahrenheit's default save set, so
# they show up when the game runs under the mod loader. One-way: nothing in
# the Steam folder is touched, and saves made under the mod stay in
# Fahrenheit's set.
#
#   Steam:      Documents\SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster\<steam id>\FINAL FANTASY X\ffx_NNN
#   Fahrenheit: <game>\fahrenheit\saves\0\default\FINAL FANTASY X\ffx_NNN
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File C:\projects\FFX-VR\tools\import-steam-saves.ps1

$docs = [Environment]::GetFolderPath('MyDocuments')
$root = Join-Path $docs 'SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster'
$game = 'C:\Program Files (x86)\Steam\steamapps\common\FINAL FANTASY FFX&FFX-2 HD Remaster'
$dest = Join-Path $game 'fahrenheit\saves\0\default\FINAL FANTASY X'

if (-not (Test-Path $root)) { Write-Host "No Steam save folder at $root"; exit 1 }

# Saves live either directly in the game folder or under a numbered Steam-account folder (newest if several).
$from = $null
if (Test-Path (Join-Path $root 'FINAL FANTASY X')) { $from = Join-Path $root 'FINAL FANTASY X' }
else {
    $src = Get-ChildItem $root -Directory |
           Where-Object { Test-Path (Join-Path $_.FullName 'FINAL FANTASY X') } |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($src) { $from = Join-Path $src.FullName 'FINAL FANTASY X' }
}
if (-not $from) { Write-Host "No FINAL FANTASY X save folder under $root"; exit 1 }
$files = Get-ChildItem $from -File -Filter 'ffx_*'
if ($files.Count -eq 0) { Write-Host "No ffx_* saves in $from"; exit 1 }

New-Item -ItemType Directory -Force -Path $dest | Out-Null
$copied = 0; $skipped = 0
foreach ($f in $files) {
    $target = Join-Path $dest $f.Name
    if ((Test-Path $target) -and ((Get-Item $target).LastWriteTime -ge $f.LastWriteTime)) { $skipped++; continue }
    Copy-Item $f.FullName -Destination $target -Force
    $copied++
}
Write-Host "Imported $copied save(s) from $from"
Write-Host "Skipped $skipped already up to date."
Write-Host "Fahrenheit set: $dest"
