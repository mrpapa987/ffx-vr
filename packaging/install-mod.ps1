# install-mod.ps1 - ships in the FFX VR release zip (as tools\install-mod.ps1).
#
# Copies mod\ffxvr\ from the zip into <game>\fahrenheit\mods\ffxvr\ and adds 'ffxvr' to
# Fahrenheit's load order. Your settings (ffxvr.ini) and anything else in that folder are kept,
# so installing a newer zip over an older one is safe. The shipped hand packs (hands\generic-glove\,
# hands\generic\) are replaced; a hand pack folder of your own under hands\ is never touched. Neither is fmvdepth\ (FMV depth
# made on your PC by convert-fmv-depth.ps1, private): not even -Uninstall -Purge deletes it. The optional target art
# (ffxvr-target-art-<version>.zip extracted into the same folder as this zip: mod\ffxvr\art\targets\ and art\odbar\) is copied
# when it is there; an installed art\ folder is never deleted by an install whose zip has none (only -Uninstall -Purge deletes it).
# It also checks what the mod needs at run time and says what is missing.
#
#   -Uninstall     remove the mod's files and its load-order line (ffxvr.ini, logs and your own hand packs are kept, and listed)
#   -Purge         with -Uninstall: also delete the whole mod folder (settings, logs, captures, your own hand packs, named
#                  first), except fmvdepth\ (FMV depth made on this PC: hours to remake), which is kept and named; delete
#                  that folder yourself if you no longer want it
#   -ResetLogging  set every diagnostic in an existing ffxvr.ini to off (tools\set-logging.ps1 -Profile Release;
#                  a timestamped .bak is made first). A fresh install needs nothing: its defaults are all off.
#   -DeleteLogs    delete the mod's own old logs, dumps and captures from the mod folder (ffxvr_*.log,
#                  ffxvr_cbdump_*.txt, img_*.bmp, seq_*.bmp, ui_atlas_*.dds, battle_atlas.dds, ffxvr_bones_*.bin: the
#                  list is $FfxvrLogPatterns in paths.ps1), listing each with its size. Settings and other files are kept.
#   -Force         install even though Fahrenheit was not found
#
# Run with (from the ffx-vr-<version> folder, the one with README.md and tools\ in it):
#   powershell -ExecutionPolicy Bypass -File .\tools\install-mod.ps1

param([switch]$Uninstall, [switch]$Purge, [switch]$ResetLogging, [switch]$DeleteLogs, [switch]$Force)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$game  = Get-FfxvrGame
$modId = 'ffxvr'
$src   = Join-Path $FfxvrRepo "mod\$modId"
$fh    = Join-Path $game 'fahrenheit'
$mods  = Join-Path $fh 'mods'
$dest  = Join-Path $mods $modId
$lo    = Join-Path $mods 'loadorder'
$files = @('ffxvr.dll', 'ffxvr.deps.json', 'ffxvr.runtimeconfig.json', 'ffxvr.manifest.json',
           'ffxvr_native.dll', 'openxr_loader.dll')

Write-Host "Game folder: $game"
Write-Host ''

Assert-FfxvrGameClosed 'the mod''s DLLs'
if ($Purge -and -not $Uninstall) { Write-Host '-Purge goes with -Uninstall. Nothing was changed.'; exit 1 }

# The mod's logs, dumps and captures in its folder (tools\paths.ps1 $FfxvrLogPatterns), largest first.
function Get-ModLogFiles {
    if (-not (Test-Path -LiteralPath $dest)) { return @() }
    @(Get-ChildItem -LiteralPath $dest -File -ErrorAction SilentlyContinue |
      Where-Object { $n = $_.Name; [bool]($FfxvrLogPatterns | Where-Object { $n -like $_ }) } |
      Sort-Object Length -Descending)
}
function Remove-ModLogs {
    $logs = Get-ModLogFiles
    if ($logs.Count -eq 0) { Write-Host '  no old logs or captures in the mod folder'; return }
    $total = 0L
    foreach ($l in $logs) {
        try { Remove-Item -LiteralPath $l.FullName -Force -ErrorAction Stop; $total += $l.Length; Write-Host ('  deleted {0,12:N0} KB  {1}' -f ($l.Length / 1KB), $l.Name) }
        catch { Write-Host "  could not delete $($l.Name): $($_.Exception.Message)" }
    }
    Write-Host ('  freed {0:N1} MB' -f ($total / 1MB))
}

# A junction or symbolic link at or under $Dir (Windows PowerShell 5.1's Remove-Item -Recurse follows junctions into their
# targets). The walk does not descend into one.
function Find-ReparsePoint([string]$Dir) {
    $stack = New-Object System.Collections.Stack; $stack.Push((Get-Item -LiteralPath $Dir -Force))
    while ($stack.Count -gt 0) {
        $d = $stack.Pop()
        if ($d.Attributes -band [IO.FileAttributes]::ReparsePoint) { return $d.FullName }
        foreach ($c in @(Get-ChildItem -LiteralPath $d.FullName -Force -ErrorAction SilentlyContinue)) {
            if ($c.Attributes -band [IO.FileAttributes]::ReparsePoint) { return $c.FullName }
            if ($c.PSIsContainer) { $stack.Push($c) }
        }
    }
    return $null
}

# FMV depth (tools\paths.ps1 $FfxvrFmvDepthDir): made on this PC from the game's videos, private. Never deleted here.
# The path is normalised (Get-Item's FullName, as the FullName of the files Get-ChildItem lists) so Test-InDepth matches
# with any spelling of the game path (FFXVR_GAME_DIR with forward slashes, '..').
$destFull = if (Test-Path -LiteralPath $dest) { (Get-Item -LiteralPath $dest -Force).FullName.TrimEnd('\') } else { $dest }
$depthDir = $destFull + '\' + $FfxvrFmvDepthDir
function Get-DepthNote {
    if (-not (Test-Path -LiteralPath $depthDir)) { return $null }
    $f = @(Get-ChildItem -LiteralPath $depthDir -Recurse -File -Force -ErrorAction SilentlyContinue)
    return ('{0}\ ({1} files, {2:N1} MB: FMV depth made on this PC; delete that folder yourself if you no longer want it)' -f $FfxvrFmvDepthDir, $f.Count, (($f | Measure-Object Length -Sum).Sum / 1MB))
}
function Test-InDepth([string]$Path) { $Path.StartsWith($depthDir + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) }

# ---------------------------------------------------------------- uninstall
if ($Uninstall) {
    Assert-FfxvrWritable $mods
    if ($Purge -and (Test-Path -LiteralPath $dest)) {
        # The mod folder itself first: Windows PowerShell 5.1's Remove-Item -Recurse on a junction deletes its target's
        # contents (review B1: this check had been lost when fmvdepth\ was exempted).
        if ((Get-Item -LiteralPath $dest -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            Write-Host "  $dest is a junction or link: -Purge does not delete through it. Nothing was changed."
            Write-Host '  Remove the link yourself, or uninstall without -Purge.'
            exit 1
        }
        # Then everything in it except fmvdepth\, which is kept, so it is not walked (it may well be a junction to
        # another drive)
        foreach ($c in @(Get-ChildItem -LiteralPath $dest -Force | Where-Object { $_.Name -ne $FfxvrFmvDepthDir })) {
            $rp = Find-ReparsePoint $c.FullName
            if ($rp) {
                Write-Host "  $rp is a junction or link: -Purge does not delete through it. Nothing was changed."
                Write-Host '  Remove the link yourself, or uninstall without -Purge.'
                exit 1
            }
        }
    }
    # The load-order line first: if a delete below fails part-way, Fahrenheit no longer tries to load a half-removed mod.
    if (Test-Path -LiteralPath $lo) {
        $lines = @(Get-Content -LiteralPath $lo | Where-Object { $_.Trim() -ne '' -and $_.Trim() -ne $modId })
        Set-Content -LiteralPath $lo -Value $lines -Encoding ascii
        Write-Host "  removed '$modId' from mods\loadorder"
    }
    $ownPacks = @(Get-ChildItem -LiteralPath (Join-Path $dest 'hands') -Directory -ErrorAction SilentlyContinue | Where-Object { $FfxvrHandPacks -notcontains $_.Name })
    if ($Purge) {
        if (Test-Path -LiteralPath $dest) {
            if ($ownPacks.Count -gt 0) { Write-Host ('  -Purge also deletes your own hand packs: ' + (($ownPacks | ForEach-Object { "hands\$($_.Name)" }) -join ', ')) }
            $depthNote = Get-DepthNote
            $all = @(Get-ChildItem -LiteralPath $dest -Recurse -File -Force -ErrorAction SilentlyContinue | Where-Object { -not (Test-InDepth $_.FullName) })
            try {
                if ($depthNote) {
                    Get-ChildItem -LiteralPath $dest -Force | Where-Object { $_.Name -ne $FfxvrFmvDepthDir } | Remove-Item -Recurse -Force -ErrorAction Stop
                } else {
                    Remove-Item -LiteralPath $dest -Recurse -Force -ErrorAction Stop
                }
                Write-Host ('  deleted {0} ({1} files, {2:N1} MB: the mod, its settings, logs and captures)' -f $dest, $all.Count, (($all | Measure-Object Length -Sum).Sum / 1MB))
                if ($depthNote) { Write-Host "  kept $depthNote" }
            } catch {
                Write-Host "  could not delete everything in ${dest}: $($_.Exception.Message)"
                Write-Host '  Something holds a file (an Explorer window, an antivirus scan?). Close it and run -Uninstall -Purge again.'
            }
        }
    } else {
        foreach ($f in $files) {
            $p = Join-Path $dest $f
            if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force; Write-Host "  removed $f" }
        }
        foreach ($pk in $FfxvrHandPacks) {   # the shipped hand packs; your own pack folders stay
            $p = Join-Path $dest "hands\$pk"
            if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force; Write-Host "  removed hands\$pk" }
        }
        $hd = Join-Path $dest 'hands'
        if ((Test-Path -LiteralPath $hd) -and @(Get-ChildItem -LiteralPath $hd -Force).Count -eq 0) { Remove-Item -LiteralPath $hd -Force }
        if ($ownPacks.Count -gt 0) { Write-Host ('  kept your own hand packs: ' + (($ownPacks | ForEach-Object { "hands\$($_.Name)" }) -join ', ')) }
        if ($DeleteLogs) { Remove-ModLogs }
    }
    Write-Host ''
    $depthNote = Get-DepthNote
    $left = @(if (Test-Path -LiteralPath $dest) { Get-ChildItem -LiteralPath $dest -Recurse -File -ErrorAction SilentlyContinue | Where-Object { -not (Test-InDepth $_.FullName) } })
    if ($left.Count -eq 0 -and -not $depthNote) {
        if (Test-Path -LiteralPath $dest) { Remove-Item -LiteralPath $dest -Force -Recurse -ErrorAction SilentlyContinue }
        Write-Host 'Done. Nothing of FFX VR is left in the game folder.'
    } elseif ($left.Count -eq 0) {
        Write-Host "Done. Left in ${dest}: $depthNote"
    } else {
        Write-Host ('Done. Left in {0} ({1} files, {2:N1} MB): your settings, logs and captures:' -f $dest, $left.Count, (($left | Measure-Object Length -Sum).Sum / 1MB))
        $left | Sort-Object Length -Descending | Select-Object -First 8 | ForEach-Object { Write-Host ('  {0,12:N0} KB  {1}' -f ($_.Length / 1KB), $_.Name) }
        Write-Host '  To remove them too: install-mod.ps1 -Uninstall -Purge'
        if ($depthNote) { Write-Host "  Also kept: $depthNote" }
    }
    exit 0
}

# ---------------------------------------------------------------- checks
$problems = 0
Write-Host '[checks]'

$fhOk = (Test-Path -LiteralPath (Join-Path $fh 'bin\fhstage0.exe')) -and (Test-Path -LiteralPath (Join-Path $fh 'bin\fhstage1.dll'))
if ($fhOk) {
    Write-Host '  Fahrenheit         : found'
    # Its version: '1.0.0-alphaNN (<commit>, official release | built from source)'. alpha11 and older predate
    # the game's 1 October 2026 patch; that floor is a fact about the game and never moves, so it is a literal.
    $i = Get-FfxvrFhInfo (Join-Path $fh 'bin')
    if ($i -and $i.Version -match '^1\.0\.0-alpha(\d+)$' -and [int]$Matches[1] -lt 12) {
        Write-Host "  Fahrenheit version : $($i.Text) - too old: it does not work with the current game."
        Write-Host "                       Install release v$FfxvrFhVersion (README.md, section 2)."
        $problems++
    } elseif ($i) {
        $note = if (-not $i.Tested) { " (FFX VR is tested with release v$FfxvrFhVersion)" } else { '' }
        Write-Host "  Fahrenheit version : $($i.Text)$note"
    }
} else {
    Write-Host "  Fahrenheit         : NOT FOUND (expected $fh\bin\fhstage0.exe)"
    Write-Host '                       FFX VR is a Fahrenheit mod. See "Fahrenheit" in README.md.'
    $problems++
}

$net86 = Join-Path ${env:ProgramFiles(x86)} 'dotnet\shared\Microsoft.NETCore.App'
$rt = @(Get-ChildItem -LiteralPath $net86 -Directory -Filter '10.*' -ErrorAction SilentlyContinue)
if ($rt.Count -gt 0) {
    Write-Host ('  .NET 10 x86 runtime: ' + (($rt | ForEach-Object { $_.Name }) -join ', '))
} else {
    Write-Host '  .NET 10 x86 runtime: NOT FOUND'
    Write-Host '                       winget install Microsoft.DotNet.Runtime.10 --architecture x86'
    $problems++
}

$rt32 = (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\Khronos\OpenXR\1' -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime
if (-not $rt32) {
    Write-Host '  OpenXR (32-bit)    : none registered. In the Virtual Desktop Streamer, choose VDXR as the OpenXR runtime.'
    $problems++
} elseif ($rt32 -match 'virtualdesktop') {
    Write-Host "  OpenXR (32-bit)    : VDXR ($rt32)"
} else {
    Write-Host "  OpenXR (32-bit)    : $rt32"
    Write-Host '                       Not VDXR. Only Virtual Desktop (VDXR) has been tested; choose it in the Streamer.'
}

# fps60_ffx.dll, not dbghelp.dll: another mod (or Fahrenheit's own fix-up) may also use that name.
if (Test-Path -LiteralPath (Join-Path $game 'fps60_ffx.dll')) {
    Write-Host '  FFX-60fps          : looks installed (fps60_ffx.dll next to FFX.exe)'
} else {
    Write-Host '  FFX-60fps          : not installed (recommended; see README.md)'
}
Write-Host ''

if (-not $fhOk -and -not $Force) {
    Write-Host 'Not installed: Fahrenheit has to be in place first. (-Force installs anyway.)'
    exit 1
}

# ---------------------------------------------------------------- copy
foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $src $f))) {
        Write-Host "ERROR: $f is missing from $src. Extract the whole zip again."
        exit 1
    }
}
Assert-FfxvrWritable $mods
Write-Host "[install] -> $dest"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
foreach ($f in $files) {
    Copy-Item -LiteralPath (Join-Path $src $f) -Destination $dest -Force
    $p = Join-Path $dest $f
    Unblock-File -LiteralPath $p -ErrorAction SilentlyContinue   # drop the "downloaded from the internet" mark
    Write-Host ('  {0}  ({1} bytes)' -f $f, (Get-Item -LiteralPath $p).Length)
}
# VR hands: the shipped hand packs (replaced); a pack folder of your own under hands\ is left as it is.
foreach ($pk in $FfxvrHandPacks) {
    $from = Join-Path $src "hands\$pk"
    if (-not (Test-Path -LiteralPath (Join-Path $from 'right.glb'))) { Write-Host "  WARNING: hands\$pk is missing from the zip: VR hands will draw boxes. Extract the whole zip again."; continue }
    $to = Join-Path $dest "hands\$pk"
    New-Item -ItemType Directory -Force -Path $to | Out-Null
    foreach ($f in @(Get-ChildItem -LiteralPath $from -File)) {
        Copy-Item -LiteralPath $f.FullName -Destination $to -Force
        Unblock-File -LiteralPath (Join-Path $to $f.Name) -ErrorAction SilentlyContinue
    }
    Write-Host "  hands\$pk\  (the generic hand)"
}
# The optional target art (its own zip, extracted over this one): the Bushido targets (art\targets) and the overdrive bar
# (art\odbar, Tidus's Swordplay bar and Lulu's countdown); each copied when present, its files replaced; nothing is deleted.
foreach ($art in @(@('targets', 'the Bushido targets', 'the mod draws plain targets'), @('odbar', 'the overdrive bar', 'the mod draws its plain overdrive bar'))) {
    $artFrom = Join-Path $src ('art\' + $art[0])
    $artTo = Join-Path $dest ('art\' + $art[0])
    if (Test-Path -LiteralPath $artFrom) {
        New-Item -ItemType Directory -Force -Path $artTo | Out-Null
        $artN = 0
        foreach ($f in @(Get-ChildItem -LiteralPath $artFrom -File)) {
            Copy-Item -LiteralPath $f.FullName -Destination $artTo -Force
            Unblock-File -LiteralPath (Join-Path $artTo $f.Name) -ErrorAction SilentlyContinue
            $artN++
        }
        Write-Host ('  art\{0}\  ({1} files: the optional target art, {2})' -f $art[0], $artN, $art[1])
    } elseif (Test-Path -LiteralPath $artTo) {
        Write-Host ('  art\{0}\  (kept: the target art installed before, {1}; none in this folder)' -f $art[0], $art[1])
    } else {
        Write-Host ('  art\{0}\  (the optional target art, {1}: not found, looked in {2}; {3})' -f $art[0], $art[1], $artFrom, $art[2])
    }
}

# ---------------------------------------------------------------- loadorder
$lines = @()
if (Test-Path -LiteralPath $lo) { $lines = @(Get-Content -LiteralPath $lo | Where-Object { $_.Trim() -ne '' }) }
if ($lines -notcontains $modId) {
    $lines += $modId
    Set-Content -LiteralPath $lo -Value $lines -Encoding ascii
    Write-Host "  added '$modId' to mods\loadorder"
} else {
    Write-Host "  '$modId' already in mods\loadorder"
}

# ---------------------------------------------------------------- logging
# A fresh install has no ffxvr.ini: the mod's defaults have every diagnostic off. An ini kept from an earlier (tester or
# developer) install may still have them on; -ResetLogging turns them off.
Write-Host ''
Write-Host '[logging]'
$ini = Join-Path $dest 'ffxvr.ini'
if ($ResetLogging -and (Test-Path -LiteralPath $ini)) {
    & (Join-Path $PSScriptRoot 'set-logging.ps1') -Profile Release -IniPath $ini
} elseif (Test-Path -LiteralPath $ini) {
    $on = Get-FfxvrDiagOn ([IO.File]::ReadAllText($ini))
    if ($on.Count -gt 0) {
        Write-Host ('  WARNING: your ffxvr.ini has diagnostics on: ' + ($on -join ', '))
        Write-Host '           They write large logs and cost frame time. Turn them off with:'
        Write-Host '             powershell -ExecutionPolicy Bypass -File .\tools\install-mod.ps1 -ResetLogging'
    } else { Write-Host '  diagnostics: all off' }
} else { Write-Host '  no ffxvr.ini yet: the defaults (diagnostics all off) are used, and the mod writes the file at the first settings change' }
if ($DeleteLogs) { Remove-ModLogs }
else {
    $logs = Get-ModLogFiles; $sz = ($logs | Measure-Object Length -Sum).Sum
    if ($sz -gt 50MB) { Write-Host ('  {0:N0} MB of old logs and captures in the mod folder; -DeleteLogs removes them' -f ($sz / 1MB)) }
}

# ---------------------------------------------------------------- settings kept from an older build
# A kept ffxvr.ini holds every key, so a default that changed does not reach it. 0.1.0-alpha.1: VR hands are on by default
# (vr_hands, 0 before). Nothing is changed here: the note only says what the player has and where the switch is.
if ((Test-Path -LiteralPath $ini) -and ([IO.File]::ReadAllText($ini) -match '(?m)^\s*vr_hands\s*=\s*0\s*$')) {
    Write-Host ''
    Write-Host '[settings]'
    Write-Host '  Your kept ffxvr.ini has VR hands off (vr_hands=0, the default before 0.1.0-alpha.1). A fresh install has them on.'
    Write-Host '  To turn them on: F8 > Hands > "VR hands: your own hands at the controllers".'
    Write-Host '  The cloister sphere grab and tap to press need them.'
}

Write-Host ''
if ($problems -gt 0) {
    Write-Host "Installed, but $problems check(s) above need fixing before it will work."
} else {
    Write-Host 'Installed. Start the game with:  powershell -ExecutionPolicy Bypass -File .\tools\run-ffx.ps1'
}
if (Test-Path -LiteralPath (Join-Path $FfxvrRepo 'TESTING.md')) {
    Write-Host 'Testing this build? TESTING.md (next to README.md) says what to try and what to send back.'
}
