# paths.ps1 - where things are, for every script in tools\. Dot-sourced:
#     . (Join-Path $PSScriptRoot 'paths.ps1')
#
# Definitions only: nothing in this file may do work when it is dot-sourced, apart from loading
# tools\paths.local.ps1. It ships in the player zip and must run in Windows PowerShell 5.1.
#
# $FfxvrRepo   this repository (the folder above tools\), wherever it was cloned
# Get-FfxvrGame  the game's install folder, found in this order:
#   1. the environment variable FFXVR_GAME_DIR
#   2. tools\paths.local.ps1 (your own file, not in git), containing one line such as
#          $FfxvrGameOverride = 'D:\SteamLibrary\steamapps\common\FINAL FANTASY FFX&FFX-2 HD Remaster'
#   3. Steam: its install folder from the registry, plus every library in libraryfolders.vdf
# A folder only counts if FFX.exe is in it. Nothing here points at any one person's PC.
#
# Paths are tested with -LiteralPath ('[' and ']' are wildcards to -Path, e.g. 'D:\Games [SSD]'),
# and built with [IO.Path]::Combine where a drive may be missing (Join-Path throws "Cannot find
# drive" for a Steam library on an unplugged disk).

$FfxvrRepo         = Split-Path -Parent $PSScriptRoot
$FfxvrGameName     = 'FINAL FANTASY FFX&FFX-2 HD Remaster'
# The Fahrenheit release FFX VR is tested with. ffxvr.csproj's Sdk="Fahrenheit.Sdk/<version>" must match (build-mod checks).
$FfxvrFhVersion    = '1.0.0-alpha12'
$FfxvrFhCommit     = '7e85839b2efb67124d1760598c9f220cb559a41a'   # tag v1.0.0-alpha12
$FfxvrGameOverride = $null
$ffxvrLocal = Join-Path $PSScriptRoot 'paths.local.ps1'
if (Test-Path -LiteralPath $ffxvrLocal) { . $ffxvrLocal }

# Every program that runs from the game folder, and so loads its dbghelp.dll (FFX-60fps's proxy)
# or rewrites GameSetting.ini. The default game check needs only FFX and Fahrenheit's stage 0.
$FfxvrRootExes = @('FFX', 'FFX-2', 'FFX&X-2_LAUNCHER', 'FFX&X-2_Will', 'fhstage0')
$FfxvrUtf8     = New-Object Text.UTF8Encoding $false

# The mod's diagnostic ffxvr.ini keys (tools\set-logging.ps1, install-mod.ps1), with the value each logging profile writes
# ($null: that profile leaves the key as it is):
#   Release = a fresh install: every diagnostic off (also what the mod uses for an absent key, except diag_verbose in an ini
#             older than ini_rev 2, which the mod reads as 1);
#   Verbose = more logging only, for a player's bug report: the periodic lines and the load: lines; no extra hooks, patches
#             or samplers;
#   Dev     = the developer's set, hooks and profilers included (they cost frame time and change how the game runs).
# Keep in step with diag_on() in src\managed\logging.cs.
$FfxvrDiagKeys = [ordered]@{
    diag_verbose       = @{ Release = '0'; Verbose = '1';   Dev = '1' }   # the periodic log lines, the camera trace, Ctrl+F9 dumps
    diag_build7        = @{ Release = '0'; Verbose = $null; Dev = '1' }   # build 7: game-function hooks, cursor / timing lines, the stall watchdog
    diag_load          = @{ Release = '0'; Verbose = $null; Dev = '1' }   # the load lines' expensive parts (address-space walks, stack walks)
    diag_load_lines    = @{ Release = '0'; Verbose = '1';   Dev = '1' }   # one load: line per load (no hook of its own)
    diag_actors        = @{ Release = '0'; Verbose = $null; Dev = $null } # the actors: line every 10 s (Fahrenheit's log)
    diag_sphere        = @{ Release = '0'; Verbose = $null; Dev = $null } # the sphere research log anywhere (sph: lines; in the cloisters it runs with any diagnostic on)
    diag_weapon        = @{ Release = '0'; Verbose = $null; Dev = $null } # the weapon research log in battles (wpn: lines)
    diag_cluster_bytes = @{ Release = '0'; Verbose = $null; Dev = '1' }   # the game pool's alloc / free hooks (next launch)
    diag_va_log        = @{ Release = '0'; Verbose = $null; Dev = '1' }   # the ntdll reservation patch (next launch)
    diag_transition    = @{ Release = '0'; Verbose = $null; Dev = '1' }   # the per-frame bt: lines and the first glass break's frame census
    diag_load_prof     = @{ Release = '0'; Verbose = $null; Dev = '2' }   # the load profiler: a sampler thread, probe hooks (next launch)
}
# VR hands: the hand packs the mod ships, ours (tools\hands\make-generic-hand.py, MIT): assets\hands\<name>\ in the
# repository, mod\ffxvr\hands\<name>\ in the release zip, <game>\fahrenheit\mods\ffxvr\hands\<name>\ installed. The tools
# write and remove only these folders: any other folder under hands\ is a user's own pack (often made from a game model) and
# is never copied, packaged or deleted, except by install-mod -Uninstall -Purge, which names it first.
$FfxvrHandPacks = @('generic-glove', 'generic')
# FMV depth: <game>\fahrenheit\mods\ffxvr\fmvdepth\ (.fvd files, manifest.json, convert.log, preview\), made on the user's PC
# by tools\convert-fmv-depth.ps1 from the game's own videos (1-2 hours of GPU time to remake). It is private and derived
# from the game: no tool may package, copy or collect it, and no tool deletes it - install-mod -Uninstall -Purge included,
# which keeps it and says so.
$FfxvrFmvDepthDir = 'fmvdepth'
# What the mod writes into its folder besides its DLLs, settings (ffxvr.ini*) and bone_labels.txt: logs, dumps, captures.
$FfxvrLogPatterns = @(   # install-mod -DeleteLogs deletes exactly these
                      'ffxvr_diag.log', 'ffxvr_diag.prev.log', 'ffxvr_native.log', 'ffxvr_trace.log', 'ffxvr_stacks.log',
                      'ffxvr_frame.log', 'ffxvr_frame_auto.log', 'ffxvr_frame_auto.prev.log', 'ffxvr_cbdump_*.txt', 'ffxvr_bones_*.bin',
                      'img_*.bmp', 'seq_*.bmp', 'ui_atlas_*.dds', 'battle_atlas.dds', 'ffxvr.ini.tmp')

# The diagnostics an ffxvr.ini text has on, as the mod reads them: @('diag_load=1', ...). A key listed twice counts as
# its last value; an absent key is off, except diag_verbose in an ini older than ini_rev 2 (the mod reads it as 1).
function Get-FfxvrDiagOn([string]$Text) {
    $kv = @{}
    foreach ($raw in ($Text -split "`n")) {
        $line = $raw.Trim(); $eq = $line.IndexOf('=')
        if ($line.Length -eq 0 -or $line[0] -eq '#' -or $eq -lt 0) { continue }
        $kv[$line.Substring(0, $eq).Trim()] = $line.Substring($eq + 1).Trim()
    }
    $rev = 0; if ($kv.ContainsKey('ini_rev')) { [void][int]::TryParse($kv['ini_rev'], [ref]$rev) }
    $on = @()
    foreach ($k in $FfxvrDiagKeys.Keys) {
        if ($kv.ContainsKey($k)) {
            if (@('0', 'false', 'no', 'off') -notcontains $kv[$k].ToLowerInvariant()) { $on += "$k=$($kv[$k])" }
        } elseif ($k -eq 'diag_verbose' -and $rev -lt 2) { $on += 'diag_verbose (absent in an ini older than rev 2: read as 1)' }
    }
    return ,$on
}

function Test-FfxvrGame([string]$dir) {
    if (-not $dir) { return $false }
    try { return (Test-Path -LiteralPath ([IO.Path]::Combine($dir, 'FFX.exe'))) } catch { return $false }
}

function Find-FfxvrGame {
    if (Test-FfxvrGame $env:FFXVR_GAME_DIR) { return $env:FFXVR_GAME_DIR }
    if (Test-FfxvrGame $FfxvrGameOverride)  { return $FfxvrGameOverride }
    $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction SilentlyContinue).SteamPath
    if (-not $steam) { return $null }
    $steam = $steam -replace '/', '\'
    $libs = @($steam)
    $vdf = $null
    try { $vdf = [IO.Path]::Combine($steam, 'steamapps', 'libraryfolders.vdf') } catch { }
    if ($vdf -and (Test-Path -LiteralPath $vdf)) {
        # The file is UTF-8 (5.1's Select-String reads it as ANSI); backslashes are escaped as \\
        try {
            foreach ($g in [regex]::Matches([IO.File]::ReadAllText($vdf, [Text.Encoding]::UTF8), '"path"\s+"(.+?)"')) {
                $libs += ($g.Groups[1].Value -replace '\\\\', '\')
            }
        } catch { }
    }
    foreach ($lib in ($libs | ForEach-Object { $_.TrimEnd('\') } | Sort-Object -Unique)) {
        try { $dir = [IO.Path]::Combine($lib, 'steamapps', 'common', $FfxvrGameName) } catch { continue }
        if (Test-FfxvrGame $dir) { return $dir }
    }
    return $null
}

# What fh.dll in $bin is: Version, Commit (from its ProductVersion '1.0.0-alphaNN+<commit>'), Tested (that commit is
# $FfxvrFhCommit), Signed (a Valid Authenticode signature: the official release zip is signed by its maintainer; a source
# build is not signed at all) and a one-line Text. Any other signature status (e.g. UnknownError on a PC that could not
# fetch the signer's root) is shown as it is, not as a source build. $null if there is no fh.dll.
function Get-FfxvrFhInfo([string]$bin) {
    $p = $null
    try { $p = [IO.Path]::Combine($bin, 'fh.dll') } catch { return $null }
    if (-not (Test-Path -LiteralPath $p)) { return $null }
    $pv = "$((Get-Item -LiteralPath $p).VersionInfo.ProductVersion)"
    $v, $c = $pv -split '\+', 2
    $c = "$c".Trim()
    $st = 'Unknown'
    try { $st = "$((Get-AuthenticodeSignature -LiteralPath $p).Status)" } catch { $st = 'Unknown' }
    $sig = ($st -eq 'Valid')
    $short = if ($c) { $c.Substring(0, [Math]::Min(7, $c.Length)) } else { 'no commit' }
    $kind  = if ($sig) { 'official release' } elseif ($st -eq 'NotSigned') { 'built from source' } else { "signature: $st" }
    [pscustomobject]@{
        Version = "$v".Trim()
        Commit  = $c
        Tested  = [bool]($c -and ($c -like ($FfxvrFhCommit.Substring(0, 7) + '*')))
        Signed  = [bool]$sig
        Text    = "$("$v".Trim()) ($short, $kind)"
    }
}

# The game folder, or a clear message and exit.
function Get-FfxvrGame {
    $dir = Find-FfxvrGame
    if ($dir) { return $dir }
    Write-Host ''
    Write-Host "Could not find the game ($FfxvrGameName, FFX.exe)."
    Write-Host 'Tell the scripts where it is, either way:'
    Write-Host "  - set FFXVR_GAME_DIR, e.g.  `$env:FFXVR_GAME_DIR = 'D:\SteamLibrary\steamapps\common\$FfxvrGameName'"
    Write-Host "  - or create tools\paths.local.ps1 with the line"
    Write-Host "        `$FfxvrGameOverride = 'D:\SteamLibrary\steamapps\common\$FfxvrGameName'"
    exit 1
}

# One log line, on screen and appended to $Path as UTF-8 without BOM (Tee-Object in 5.1 writes
# UTF-16). $Path must be absolute: AppendAllText ignores the PowerShell location. A failed log
# write never stops the script.
function Write-FfxvrLog([string]$Path, $Text) {
    $s = "$Text"
    Write-Host $s
    if ($Path) { try { [IO.File]::AppendAllText($Path, $s + "`r`n", $FfxvrUtf8) } catch { } }
}

# Running game processes (files in the game folder are locked while they run).
function Get-FfxvrGameProcess([string[]]$Names = @('FFX', 'fhstage0')) {
    @(Get-Process -Name $Names -ErrorAction SilentlyContinue)
}

# Stop with a clear message while the game runs. $What names the files that would be in use.
function Assert-FfxvrGameClosed([string]$What = 'its files', [string[]]$Names = @('FFX', 'fhstage0')) {
    $p = Get-FfxvrGameProcess $Names
    if ($p.Count -eq 0) { return }
    $n = @($p | ForEach-Object { $_.ProcessName } | Sort-Object -Unique)
    Write-Host ''
    Write-Host ('The game is running (' + (($n | ForEach-Object { "$_.exe" }) -join ', ') + "). Close it first: $What would be in use. Nothing was changed.")
    if ($n -contains 'fhstage0' -and $n -notcontains 'FFX') { Write-Host '  If the game already closed, end fhstage0.exe in Task Manager.' }
    exit 1
}

# Stop with a clear message when this user can't write to $Dir (or, if it doesn't exist yet, to
# its nearest existing parent).
function Assert-FfxvrWritable([string]$Dir) {
    while ($Dir -and -not (Test-Path -LiteralPath $Dir)) { $Dir = Split-Path $Dir }
    if (-not $Dir) { return }
    $probe = [IO.Path]::Combine($Dir, ".ffxvr-write-test-$PID")
    try { [IO.File]::WriteAllText($probe, '') }
    catch [UnauthorizedAccessException] {
        Write-Host ''
        Write-Host "Windows denied write access to: $Dir"
        Write-Host 'Do not run as administrator (Fahrenheit refuses to start elevated, and the mod writes its'
        Write-Host 'settings there while you play). Check that folder''s permissions, or move the game to a'
        Write-Host 'Steam library on another drive (Steam: the game > Properties > Installed Files > Move install folder).'
        exit 1
    }
    catch {
        $m = if ($_.Exception.InnerException) { $_.Exception.InnerException.Message } else { $_.Exception.Message }
        Write-Host ''
        Write-Host "Can't write to ${Dir}: $m"
        exit 1
    }
    try { [IO.File]::Delete($probe) } catch { }      # an AV scan holding the new file is not a write problem
}

# Newest ffx_* save in a Steam account folder (MinValue when none). Sort-Object, not
# Measure-Object -Maximum, which only takes numbers in 5.1.
function Get-FfxvrNewestSave([string]$AccountDir) {
    $n = Get-ChildItem -LiteralPath ([IO.Path]::Combine($AccountDir, 'FINAL FANTASY X')) -File -Filter 'ffx_*' -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($n) { return $n.LastWriteTime } else { return [datetime]::MinValue }
}

# The Steam save folder (...\FINAL FANTASY X): directly in the game's Documents folder, or in a
# numbered Steam-account folder. With several accounts, says which one is used and how to choose.
function Get-FfxvrSteamSaveDir([string]$Account = '') {
    $root = [IO.Path]::Combine([Environment]::GetFolderPath('MyDocuments'), 'SQUARE ENIX', 'FINAL FANTASY X&X-2 HD Remaster')
    $direct = [IO.Path]::Combine($root, 'FINAL FANTASY X')
    if (-not $Account -and (Test-Path -LiteralPath $direct)) { return $direct }
    $accts = @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
               Where-Object { Test-Path -LiteralPath ([IO.Path]::Combine($_.FullName, 'FINAL FANTASY X')) } |
               Sort-Object { Get-FfxvrNewestSave $_.FullName } -Descending)
    if ($Account) { $accts = @($accts | Where-Object { $_.Name -eq $Account }) }
    if ($accts.Count -eq 0) { return $null }
    if ($accts.Count -gt 1) {
        Write-Host 'Several Steam accounts have FFX saves (newest save):'
        foreach ($a in $accts) { Write-Host ('  {0}  {1:yyyy-MM-dd HH:mm}' -f $a.Name, (Get-FfxvrNewestSave $a.FullName)) }
        Write-Host "Using $($accts[0].Name). Pass -SteamId <number> to choose."
    }
    return [IO.Path]::Combine($accts[0].FullName, 'FINAL FANTASY X')
}
