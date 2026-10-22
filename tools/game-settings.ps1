# game-settings.ps1
#
# Switches the game's own render settings (GameSetting.ini in Documents) between
# a VR profile and your original flat-screen settings. The headset gets
# 2496x1404 per eye, so 4K with 8x MSAA only costs GPU time - twice, for two
# eyes - and starves the frame rate in heavy battles.
#
#   -Profile vr     Resolution=2560*1440, MSAA=4 (other settings untouched)
#   -Profile flat   restore GameSetting.ini.flat-backup (made the first time vr is applied)
#   -Width/-Height/-Msaa   override the vr numbers, e.g. -Width 3200 -Height 1800 -Msaa 4
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\game-settings.ps1 -Profile vr

param(
    [ValidateSet('vr', 'flat')] [string]$Profile = 'vr',
    [int]$Width = 2560, [int]$Height = 1440, [int]$Msaa = 4
)

$docs = [Environment]::GetFolderPath('MyDocuments')
$dir  = Join-Path $docs 'SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster'
$ini  = Join-Path $dir 'GameSetting.ini'
$bak  = Join-Path $dir 'GameSetting.ini.flat-backup'
if (-not (Test-Path $ini)) { Write-Host "No $ini"; exit 1 }

if ($Profile -eq 'flat') {
    if (-not (Test-Path $bak)) { Write-Host "No backup at $bak - nothing to restore"; exit 1 }
    Copy-Item $bak $ini -Force
    Write-Host "Restored flat-screen settings from $bak"
}
else {
    if (-not (Test-Path $bak)) { Copy-Item $ini $bak; Write-Host "Backed up original to $bak" }
    $text = Get-Content $ini -Raw
    $text = [regex]::Replace($text, '(?m)^Resolution=.*$', "Resolution=$Width*$Height")
    $text = [regex]::Replace($text, '(?m)^MSAA=.*$', "MSAA=$Msaa")
    Set-Content $ini $text -NoNewline
    Write-Host "VR profile: Resolution=$Width*$Height MSAA=$Msaa"
}
Get-Content $ini | Where-Object { $_ -match '^(Resolution|MSAA|HDAO|PostAA|VSync|ScreenMode)=' }
