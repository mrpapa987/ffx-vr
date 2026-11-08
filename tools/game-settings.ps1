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
# Close the game and its launcher first: they write GameSetting.ini themselves.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\game-settings.ps1 -Profile vr

param(
    [ValidateSet('vr', 'flat')] [string]$Profile = 'vr',
    [int]$Width = 2560, [int]$Height = 1440, [int]$Msaa = 4
)

. (Join-Path $PSScriptRoot 'paths.ps1')      # helpers only; the game folder isn't needed here
Assert-FfxvrGameClosed 'GameSetting.ini' $FfxvrRootExes

$docs = [Environment]::GetFolderPath('MyDocuments')
$dir  = Join-Path $docs 'SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster'
$ini  = Join-Path $dir 'GameSetting.ini'
$bak  = Join-Path $dir 'GameSetting.ini.flat-backup'
if (-not (Test-Path -LiteralPath $ini)) { Write-Host "No $ini"; exit 1 }

if ($Profile -eq 'flat') {
    if (-not (Test-Path -LiteralPath $bak)) { Write-Host "No backup at $bak - nothing to restore"; exit 1 }
    Copy-Item -LiteralPath $bak -Destination $ini -Force
    Write-Host "Restored flat-screen settings from $bak"
}
else {
    if (-not (Test-Path -LiteralPath $bak)) { Copy-Item -LiteralPath $ini -Destination $bak; Write-Host "Backed up original to $bak" }
    $text = "$(Get-Content -LiteralPath $ini -Raw)"
    # A key that is not in the file is not added (its format is the game's); say so, so a missing key cannot pass silently
    # (fix-render review N6). F8's "game renders WxH" line shows what the game actually used.
    foreach ($key in 'Resolution', 'MSAA') {
        if (-not [regex]::IsMatch($text, "(?mi)^$key[ \t]*=")) {
            Write-Warning "$key= is not in $ini - not changed. Set it once in the game's launcher (FFX&X-2_LAUNCHER.exe), then run this again."
        }
    }
    # [^\r\n]*, not .*$: '.' also eats the CR of a CRLF line. Keys match case-insensitively.
    $text = [regex]::Replace($text, '(?mi)^Resolution[ \t]*=[^\r\n]*', "Resolution=$Width*$Height")
    $text = [regex]::Replace($text, '(?mi)^MSAA[ \t]*=[^\r\n]*', "MSAA=$Msaa")
    # Write it back in the encoding it came in: Get-Content honours a BOM, but 5.1's Set-Content
    # would otherwise write ANSI.
    $b = [IO.File]::ReadAllBytes($ini)
    $enc = @{}
    if     ($b.Length -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE)                     { $enc = @{ Encoding = 'Unicode' } }
    elseif ($b.Length -ge 2 -and $b[0] -eq 0xFE -and $b[1] -eq 0xFF)                     { $enc = @{ Encoding = 'BigEndianUnicode' } }
    elseif ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) { $enc = @{ Encoding = 'UTF8' } }
    Set-Content -LiteralPath $ini -Value $text -NoNewline @enc
    Write-Host "VR profile: Resolution=$Width*$Height MSAA=$Msaa"
}
Get-Content -LiteralPath $ini | Where-Object { $_ -match '^(Resolution|MSAA|HDAO|PostAA|VSync|ScreenMode)=' }
