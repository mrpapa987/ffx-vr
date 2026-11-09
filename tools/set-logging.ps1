# set-logging.ps1 - switches FFX VR's diagnostics (logs, dumps, diagnostic hooks) on or off in the installed ffxvr.ini.
#
#   -Profile Release   every diagnostic key off: what a fresh install has (a small, per-session ffxvr_diag.log of
#                      events and errors only, capped at 4 MB; no periodic lines, no dumps, no diagnostic hooks)
#   -Profile Verbose   more logging for a bug report, nothing else: diag_verbose (the periodic summaries, log cap
#                      64 MB) and diag_load_lines; no extra hooks, patches or samplers (other keys left as they are)
#   -Profile Dev       the developer's set on: diag_verbose, diag_build7, diag_load, diag_load_lines,
#                      diag_cluster_bytes, diag_va_log, diag_transition, diag_load_prof=2 (diag_actors is left as it is).
#                      These add game hooks, an ntdll patch and a loader sampler: for the developer, not for players
#   -GameDir <folder>  the game folder (the one with FFX.exe); otherwise found as every script does (tools\paths.ps1)
#   -IniPath <file>    edit this ffxvr.ini instead (a copy, for a test)
#   -DryRun            only show what would change
#
# The file is edited in place: every other line, comment, the order, the line endings and the encoding stay as they
# are; a key that is missing is added at the end. A timestamped copy (ffxvr.ini.<yyyyMMdd-HHmmss-fff>.bak) is made first.
# The game must be closed: the mod reads the file back and rewrites it while it runs. The keys the F8 overlay's
# Developer tab tags "next launch" (diag_cluster_bytes, diag_va_log, diag_load_prof's probes) take effect at the next start
# anyway; the rest would apply at once, but the game is closed here, so: the next start.
# Without an ffxvr.ini: Release has nothing to do (no file = the defaults = all off); Verbose and Dev write a short one
# (every diagnostic key, so the mod does not take it for a truncated file; it fills in the rest at its next save).
#
# Run with:
#   powershell -ExecutionPolicy Bypass -File .\tools\set-logging.ps1 -Profile Release
#   powershell -ExecutionPolicy Bypass -File .\tools\set-logging.ps1 -Profile Verbose
#   powershell -ExecutionPolicy Bypass -File .\tools\set-logging.ps1 -Profile Dev

param(
    [Parameter(Mandatory = $true)] [ValidateSet('Release', 'Verbose', 'Dev')] [string]$Profile,
    [string]$GameDir = '',
    [string]$IniPath = '',
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')

if ($IniPath) {
    $ini = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($IniPath)   # relative to the PowerShell location
} else {
    if ($GameDir) {
        if (-not (Test-FfxvrGame $GameDir)) { Write-Host "No FFX.exe in $GameDir. Pass the game folder (the one with FFX.exe)."; exit 1 }
        $game = $GameDir
    } else { $game = Get-FfxvrGame }
    $ini = [IO.Path]::Combine($game, 'fahrenheit', 'mods', 'ffxvr', 'ffxvr.ini')
}
$want = [ordered]@{}
foreach ($k in $FfxvrDiagKeys.Keys) { $v = $FfxvrDiagKeys[$k][$Profile]; if ($null -ne $v) { $want[$k] = $v } }

Write-Host "Logging profile $Profile -> $ini"
if (-not $DryRun) { Assert-FfxvrGameClosed 'ffxvr.ini (the mod reads it back and rewrites it while the game runs)' }

# ---------------------------------------------------------------- no ffxvr.ini yet
if (-not (Test-Path -LiteralPath $ini)) {
    if ($Profile -eq 'Release') {
        Write-Host '  No ffxvr.ini: the mod runs on its defaults, which have every diagnostic off. Nothing to change.'
        exit 0
    }
    $dir = Split-Path -Parent $ini
    if (-not (Test-Path -LiteralPath $dir)) { Write-Host "  The mod folder $dir does not exist: install the mod first."; exit 1 }
    # Every diagnostic key (the profile's value, else Release's): with ini_rev that is 10 keys, so the mod's truncation check
    # (fewer than 10 keys next to an ffxvr.ini.bak) does not flag the file.
    $lines = @('# FFX VR settings. Edited by the overlay; hand edits are fine.', 'ini_rev=2')
    foreach ($k in $FfxvrDiagKeys.Keys) {
        $v = if ($want.Contains($k)) { $want[$k] } else { $FfxvrDiagKeys[$k]['Release'] }
        $lines += "$k=$v"; Write-Host "  $k = $v (new file)"
    }
    if ($DryRun) { Write-Host 'Dry run: nothing written.'; exit 0 }
    [IO.File]::WriteAllText($ini, (($lines -join "`n") + "`n"), $FfxvrUtf8)   # as the mod writes it: UTF-8 without BOM, LF
    Write-Host "Written: $ini (the mod fills in every other setting at its next save)."
    exit 0
}

# ---------------------------------------------------------------- read, keeping the encoding
$bytes = [IO.File]::ReadAllBytes($ini)
$skip = 0
if     ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { $enc = New-Object Text.UTF8Encoding $true; $skip = 3 }
elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFF -and $bytes[1] -eq 0xFE) { $enc = New-Object Text.UnicodeEncoding $false, $true; $skip = 2 }
elseif ($bytes.Length -ge 2 -and $bytes[0] -eq 0xFE -and $bytes[1] -eq 0xFF) { $enc = New-Object Text.UnicodeEncoding $true, $true; $skip = 2 }
else {
    $enc = New-Object Text.UTF8Encoding $false, $true    # UTF-8 without BOM (what the mod writes); strict, so ANSI shows up
    try { [void]$enc.GetString($bytes) } catch { $enc = [Text.Encoding]::Default }   # a hand edit saved as ANSI stays ANSI
}
$text = $enc.GetString($bytes, $skip, $bytes.Length - $skip)
$nl = if ($text.Contains("`r`n")) { "`r`n" } else { "`n" }

# ---------------------------------------------------------------- edit
# Split on LF only: a CR stays at the end of its line, so joining with LF gives the same bytes back for every line not changed.
$lines = $text -split "`n", 0
$seen = @{}
$changes = @()
for ($i = 0; $i -lt $lines.Count; $i++) {
    $line = $lines[$i]
    $cr = ''; if ($line.EndsWith("`r")) { $cr = "`r"; $line = $line.Substring(0, $line.Length - 1) }
    $m = [regex]::Match($line, '^(\s*)([^#=\s][^=]*?)(\s*=\s*)(.*?)(\s*)$')
    if (-not $m.Success) { continue }
    $k = $m.Groups[2].Value
    if (-not $want.Contains($k)) { continue }
    $seen[$k] = $true
    $old = $m.Groups[4].Value
    if ($old -eq $want[$k]) { continue }
    $lines[$i] = $m.Groups[1].Value + $k + $m.Groups[3].Value + $want[$k] + $m.Groups[5].Value + $cr
    $changes += "  $k : $old -> $($want[$k])"
}
$add = @()
foreach ($k in $want.Keys) {
    if (-not $seen.ContainsKey($k)) { $add += "$k=$($want[$k])"; $changes += "  $k : (absent) -> $($want[$k]) (added at the end)" }
}
if ($add.Count -gt 0) {
    $out = $lines -join "`n"
    if ($out.Length -gt 0 -and -not $out.EndsWith("`n")) { $out += $nl }
    $out += ($add -join $nl) + $nl
} else { $out = $lines -join "`n" }

if ($changes.Count -eq 0) {
    Write-Host "  Nothing to change: the $Profile values are already set."
} else {
    $changes | ForEach-Object { Write-Host $_ }
}
$on = Get-FfxvrDiagOn $out
Write-Host ('  Diagnostics on afterwards: ' + $(if ($on.Count -gt 0) { $on -join ', ' } else { 'none' }))
if ($changes.Count -eq 0) { exit 0 }
if ($DryRun) { Write-Host 'Dry run: nothing written.'; exit 0 }

# ---------------------------------------------------------------- write
if ((Get-Item -LiteralPath $ini).IsReadOnly) { Write-Host "  $ini is read-only: nothing changed. Clear the read-only flag first."; exit 1 }
$bak = "$ini.$(Get-Date -Format 'yyyyMMdd-HHmmss-fff').bak"
if (Test-Path -LiteralPath $bak) { Write-Host "  $bak already exists: nothing changed. Run again."; exit 1 }   # never overwrite a backup
Copy-Item -LiteralPath $ini -Destination $bak
$body = $enc.GetBytes($out)
$pre  = if ($skip -gt 0) { $enc.GetPreamble() } else { @() }
$all  = New-Object byte[] ($pre.Length + $body.Length)
if ($pre.Length -gt 0) { [Array]::Copy($pre, 0, $all, 0, $pre.Length) }
[Array]::Copy($body, 0, $all, $pre.Length, $body.Length)
[IO.File]::WriteAllBytes($ini, $all)
Write-Host "Backup: $bak"
Write-Host "Done: $($changes.Count) change(s). They take effect at the next game start."
