# game-settings.ps1
#
# Switches the game's own render settings (GameSetting.ini in Documents) between
# a VR profile and your original flat-screen settings. The headset gets
# 2496x1404 per eye, so 4K with 8x MSAA only costs GPU time - twice, for two
# eyes - and starves the frame rate in heavy battles.
#
#   -Profile vr     Resolution=2560*1440, MSAA=4, and a ScreenMode that can show that size (below)
#   -Profile flat   restore GameSetting.ini.flat-backup (made the first time vr is applied)
#   -Width/-Height/-Msaa   override the vr numbers, e.g. -Width 3200 -Height 1800 -Msaa 4
#   -ScreenMode auto|fullscreen|borderless|window   (vr only; default auto)
#
# ScreenMode (crash-1440): the game's SM_FULLSCREEN is a display mode change to exactly Width x Height. A size your
# display does not list cannot be shown that way (the official launcher offers only listed sizes), and the owner's
# 2560x1440 on a 4K monitor in SM_FULLSCREEN crashed at the headset start. SM_BORDERLESS never changes the display mode:
# the game renders a Width x Height window at the top left of the desktop, any size up to the desktop's.
#   auto        the desktop's own size keeps the ScreenMode you have; any other size gets SM_BORDERLESS
#   fullscreen  SM_FULLSCREEN, only for a size the display lists (-Force to write it anyway)
#   borderless  SM_BORDERLESS;  window: SM_WINDOW (a window with a title bar)
#
# Close the game and its launcher first: they write GameSetting.ini themselves.
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\game-settings.ps1 -Profile vr

param(
    [ValidateSet('vr', 'flat')] [string]$Profile = 'vr',
    [int]$Width = 2560, [int]$Height = 1440, [int]$Msaa = 4,
    [ValidateSet('auto', 'fullscreen', 'borderless', 'window')] [string]$ScreenMode = 'auto',
    [switch]$Force
)

. (Join-Path $PSScriptRoot 'paths.ps1')      # helpers only; the game folder isn't needed here
Assert-FfxvrGameClosed 'GameSetting.ini' $FfxvrRootExes

$docs = [Environment]::GetFolderPath('MyDocuments')
$dir  = Join-Path $docs 'SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster'
$ini  = Join-Path $dir 'GameSetting.ini'
$bak  = Join-Path $dir 'GameSetting.ini.flat-backup'
if (-not (Test-Path -LiteralPath $ini)) { Write-Host "No $ini"; exit 1 }

# The primary display's modes, as the game's mode change sees them (EnumDisplaySettings, 32 bits per pixel; the game's
# window opens at 0,0, on the primary display), and the desktop's current size. $null when they cannot be read.
function Get-FfxvrDisplayModes {
    try {
        if (-not ('FfxvrDisplayModes' -as [type])) {
            Add-Type -TypeDefinition @'
using System; using System.Collections.Generic; using System.Runtime.InteropServices;
public static class FfxvrDisplayModes {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct DEVMODE {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmDeviceName;
        public short dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; public int dmFields;
        public int dmPositionX, dmPositionY, dmDisplayOrientation, dmDisplayFixedOutput;
        public short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string dmFormName;
        public short dmLogPixels; public int dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
        public int dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
    }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern bool EnumDisplaySettingsW(string dev, int mode, ref DEVMODE dm);
    static DEVMODE New() { DEVMODE d = new DEVMODE(); d.dmSize = (short)Marshal.SizeOf(typeof(DEVMODE)); return d; }
    public static int[] Current() { DEVMODE d = New(); return EnumDisplaySettingsW(null, -1, ref d) ? new int[] { d.dmPelsWidth, d.dmPelsHeight } : null; }
    public static string[] Modes() {
        List<string> r = new List<string>(); DEVMODE d = New();
        for (int i = 0; i < 4096 && EnumDisplaySettingsW(null, i, ref d); i++) {
            string s = d.dmPelsWidth + "x" + d.dmPelsHeight;
            if (d.dmBitsPerPel == 32 && !r.Contains(s)) r.Add(s);
            d = New();
        }
        return r.ToArray();
    }
}
'@
        }
        $cur = [FfxvrDisplayModes]::Current(); $modes = [FfxvrDisplayModes]::Modes()
        if ($null -eq $cur -or $modes.Count -eq 0) { return $null }
        return @{ Width = $cur[0]; Height = $cur[1]; Modes = $modes }
    }
    catch { return $null }
}

if ($Profile -eq 'flat') {
    if (-not (Test-Path -LiteralPath $bak)) { Write-Host "No backup at $bak - nothing to restore"; exit 1 }
    Copy-Item -LiteralPath $bak -Destination $ini -Force
    Write-Host "Restored flat-screen settings from $bak"
}
else {
    $text = "$(Get-Content -LiteralPath $ini -Raw)"
    # The ScreenMode to write. Decided before anything is written, so a refusal leaves the file (and the backup) alone.
    $disp = Get-FfxvrDisplayModes
    $size = "${Width}x${Height}"
    $m = [regex]::Match($text, '(?mi)^ScreenMode[ \t]*=[ \t]*([^\r\n]*)')
    $have = if ($m.Success) { $m.Groups[1].Value.Trim() } else { '' }
    if ($null -eq $disp) { Write-Warning "Could not read the display's modes: a fullscreen size cannot be checked." }
    else {
        $l = @($disp.Modes | Where-Object { $_ -match '^(\d+)x(\d+)$' -and [int]$matches[1] * 9 -eq [int]$matches[2] * 16 })
        Write-Host "Display: desktop $($disp.Width)x$($disp.Height); 16:9 fullscreen sizes it lists: $($l -join ', ')"
    }
    $atDesktop = $null -ne $disp -and $Width -eq $disp.Width -and $Height -eq $disp.Height
    $listed = $null -ne $disp -and $disp.Modes -contains $size
    $want = switch ($ScreenMode) {
        'fullscreen' { 'SM_FULLSCREEN' }
        'borderless' { 'SM_BORDERLESS' }
        'window'     { 'SM_WINDOW' }
        default      { if ($atDesktop -and $have -ne '') { $have } else { 'SM_BORDERLESS' } }
    }
    if ($want -eq 'SM_FULLSCREEN' -and -not $listed -and -not $atDesktop) {
        if (-not $Force) {
            Write-Warning "$size is not a fullscreen size this display lists$(if ($null -eq $disp) { ' (could not be checked)' }): SM_FULLSCREEN cannot show it (the 2560x1440 crash at the headset start). Use -ScreenMode borderless (or auto), a listed size, or -Force. Nothing written."
            exit 1
        }
        Write-Warning "-Force: writing SM_FULLSCREEN at $size, which this display does not list."
    }
    if ($want -ne 'SM_FULLSCREEN' -and $null -ne $disp -and ($Width -gt $disp.Width -or $Height -gt $disp.Height)) {
        Write-Warning "$size is larger than the desktop ($($disp.Width)x$($disp.Height)): the game's window would run off the screen. Nothing written."
        exit 1
    }
    if ($ScreenMode -eq 'auto' -and $want -ne $have) { Write-Host "ScreenMode: $size is not the desktop's size, so SM_BORDERLESS (no display mode change; the game's window at the top left of the desktop)" }

    if (-not (Test-Path -LiteralPath $bak)) { Copy-Item -LiteralPath $ini -Destination $bak; Write-Host "Backed up original to $bak" }
    # A key that is not in the file is not added (its format is the game's); say so, so a missing key cannot pass silently
    # (fix-render review N6). F8's "game renders WxH" line shows what the game actually used.
    $missing = @()
    foreach ($key in 'Resolution', 'MSAA', 'ScreenMode') {
        if (-not [regex]::IsMatch($text, "(?mi)^$key[ \t]*=")) {
            $missing += $key
            Write-Warning "$key= is not in $ini - not changed. Set it once in the game's launcher (FFX&X-2_LAUNCHER.exe), then run this again."
        }
    }
    # [^\r\n]*, not .*$: '.' also eats the CR of a CRLF line. Keys match case-insensitively.
    $text = [regex]::Replace($text, '(?mi)^Resolution[ \t]*=[^\r\n]*', "Resolution=$Width*$Height")
    $text = [regex]::Replace($text, '(?mi)^MSAA[ \t]*=[^\r\n]*', "MSAA=$Msaa")
    $text = [regex]::Replace($text, '(?mi)^ScreenMode[ \t]*=[^\r\n]*', "ScreenMode=$want")
    # Write it back in the encoding it came in: Get-Content honours a BOM, but 5.1's Set-Content
    # would otherwise write ANSI.
    $b = [IO.File]::ReadAllBytes($ini)
    $enc = @{}
    if     ($b.Length -ge 2 -and $b[0] -eq 0xFF -and $b[1] -eq 0xFE)                     { $enc = @{ Encoding = 'Unicode' } }
    elseif ($b.Length -ge 2 -and $b[0] -eq 0xFE -and $b[1] -eq 0xFF)                     { $enc = @{ Encoding = 'BigEndianUnicode' } }
    elseif ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF) { $enc = @{ Encoding = 'UTF8' } }
    Set-Content -LiteralPath $ini -Value $text -NoNewline @enc
    $set = @()
    if ($missing -notcontains 'Resolution') { $set += "Resolution=$Width*$Height" }
    if ($missing -notcontains 'MSAA') { $set += "MSAA=$Msaa" }
    if ($missing -notcontains 'ScreenMode') { $set += "ScreenMode=$want" }
    if ($set.Count -eq 0) { $msg = "VR profile: nothing changed (none of the keys is in $ini)" }
    else {
        $msg = "VR profile: $($set -join ' ')"
        if ($missing.Count -gt 0) { $msg += " (not set: $($missing -join ', '))" }
    }
    Write-Host $msg
}
Get-Content -LiteralPath $ini | Where-Object { $_ -match '^(Resolution|MSAA|HDAO|PostAA|VSync|ScreenMode)=' }
