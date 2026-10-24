# G5 final: PowerShell tools and packaging (R5, S1-S9)

Round 3. This is the proposer's answer to G5-critique.md. I re-checked each objection with pwsh 7 in
fixes/g5-test/ (t4-t8). Where 5.1 can't be run here, the reasoning is given.

**Result: every item is AGREED.** I accepted all of the critic's requests. Two of them are adjusted
in how they're implemented, not in what they're for:

- The account sort the critic proposed uses `Measure-Object -Maximum` on DateTime. That fails in
  5.1, so it's done with Sort-Object instead.
- The critic's `-Slot <int[]>` has a binding trap. Under `-File`, `-Slot 3,5` binds as the single
  number **35**: the comma is read as a thousands separator. Reproduced in t8. So the parameter is a
  string that gets split.

Line numbers refer to the 9a8715e snapshot.

---

## Section 0: paths.ps1 helpers. AGREED

All four requested changes are accepted.

**Writable check.** Only UnauthorizedAccessException gets the friendly message. The typed catch
works through the MethodInvocationException wrapper (t5 shows "caught UAE typed"). In 5.1, catch
matching also checks the inner exception, the same as `catch [Net.WebException]` around .NET calls.
- Any other failure prints the real message.
- The check probes the nearest existing parent folder.
- It runs before any `New-Item`.

**Account sort.** Accounts are sorted by their newest `ffx_*` file. In 5.1, `Measure-Object -Maximum`
only takes numbers ("is not numeric" on DateTime; DateTime support arrived in 6.0). So the newest
file is found with Sort-Object, which t7 confirms picks the right account.

**Library loop.** The body is wrapped in try/catch. A bad vdf entry, or a missing drive, is skipped.

**Header.** It now says "definitions only, no top-level work besides paths.local.ps1".

**Process names.** These are now a parameter. The default is `FFX`, `fhstage0`. build-fps60 and
game-settings pass extra names (S3, S5, S7).

Final helper set (5.1-safe: no `::new`, no `utf8NoBOM`, no `??`):
```powershell
# paths.ps1 - definitions only. Nothing here may do work at dot-source time except loading paths.local.ps1.
function Test-FfxvrGame([string]$dir) {
    if (-not $dir) { return $false }
    try { return (Test-Path -LiteralPath ([IO.Path]::Combine($dir, 'FFX.exe'))) } catch { return $false }
}
# Find-FfxvrGame 29-38:
    $vdf = [IO.Path]::Combine($steam, 'steamapps', 'libraryfolders.vdf')
    if (Test-Path -LiteralPath $vdf) {
        try { foreach ($g in [regex]::Matches([IO.File]::ReadAllText($vdf, [Text.Encoding]::UTF8), '"path"\s+"(.+?)"')) {
                  $libs += ($g.Groups[1].Value -replace '\\\\', '\') } } catch { }
    }
    foreach ($lib in ($libs | ForEach-Object { $_.TrimEnd('\') } | Sort-Object -Unique)) {
        try { $dir = [IO.Path]::Combine($lib, 'steamapps', 'common', $FfxvrGameName) } catch { continue }
        if (Test-FfxvrGame $dir) { return $dir }
    }
# line 16: if (Test-Path -LiteralPath $ffxvrLocal) { . $ffxvrLocal }

$FfxvrUtf8 = New-Object Text.UTF8Encoding $false
function Write-FfxvrLog([string]$Path, $Text) {        # $Path must be absolute (AppendAllText ignores $PWD)
    $s = "$Text"; Write-Host $s
    if ($Path) { try { [IO.File]::AppendAllText($Path, $s + "`r`n", $FfxvrUtf8) } catch { } }
}
function Get-FfxvrGameProcess([string[]]$Names = @('FFX', 'fhstage0')) {
    @(Get-Process -Name $Names -ErrorAction SilentlyContinue)
}
function Assert-FfxvrGameClosed([string]$What = 'its files', [string[]]$Names = @('FFX', 'fhstage0')) {
    $p = Get-FfxvrGameProcess $Names
    if ($p.Count -eq 0) { return }
    $n = @($p | ForEach-Object { $_.ProcessName } | Sort-Object -Unique)
    Write-Host ''
    Write-Host ("The game is running (" + (($n | ForEach-Object { "$_.exe" }) -join ', ') + "). Close it first: $What would be in use. Nothing was changed.")
    if ($n -contains 'fhstage0' -and $n -notcontains 'FFX') { Write-Host '  If the game already closed, end fhstage0.exe in Task Manager.' }
    exit 1
}
function Assert-FfxvrWritable([string]$Dir) {
    while ($Dir -and -not (Test-Path -LiteralPath $Dir)) { $Dir = Split-Path $Dir }   # nearest existing parent
    if (-not $Dir) { return }
    $probe = [IO.Path]::Combine($Dir, ".ffxvr-write-test-$PID")
    try { [IO.File]::WriteAllText($probe, ''); [IO.File]::Delete($probe) }
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
        Write-Host ''; Write-Host "Can't write to ${Dir}: $m"; exit 1
    }
}
function Get-FfxvrSteamSaveDir([string]$Account = '') {
    $root = [IO.Path]::Combine([Environment]::GetFolderPath('MyDocuments'), 'SQUARE ENIX', 'FINAL FANTASY X&X-2 HD Remaster')
    $direct = [IO.Path]::Combine($root, 'FINAL FANTASY X')
    if (-not $Account -and (Test-Path -LiteralPath $direct)) { return $direct }
    $newest = { $n = Get-ChildItem -LiteralPath ([IO.Path]::Combine($_.FullName, 'FINAL FANTASY X')) -File -Filter 'ffx_*' -ErrorAction SilentlyContinue |
                     Sort-Object LastWriteTime -Descending | Select-Object -First 1
                if ($n) { $n.LastWriteTime } else { [datetime]::MinValue } }
    $accts = @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
               Where-Object { Test-Path -LiteralPath ([IO.Path]::Combine($_.FullName, 'FINAL FANTASY X')) } |
               Sort-Object $newest -Descending)
    if ($Account) { $accts = @($accts | Where-Object { $_.Name -eq $Account }) }
    if ($accts.Count -eq 0) { return $null }
    if ($accts.Count -gt 1) {
        Write-Host 'Several Steam accounts have FFX saves (newest save):'
        foreach ($a in $accts) { $_ = $a; Write-Host ('  {0}  {1:yyyy-MM-dd HH:mm}' -f $a.Name, (& $newest)) }
        Write-Host "Using $($accts[0].Name). Pass -SteamId <number> to choose."
    }
    return [IO.Path]::Combine($accts[0].FullName, 'FINAL FANTASY X')
}
```
(The `$_ = $a; & $newest` line reuses the scriptblock for printing. Writing it with a small named
function `Get-NewestSave($dir)` instead is equally fine.)

**Verify.** Every script still starts. Run `check-prereqs` and `install-mod` once in **powershell.exe
5.1** to catch any parse error in the shared file.

**Effort:** S

---

## R5: build-mod deletes captures; run-ffx misses .dds. AGREED

- **Verdict:** fix now.
- **Fix, build-mod:** delete only build output, `@('*.dll','*.pdb','*.deps.json','*.runtimeconfig.json')`.
  This is inside S3's try block, shown there. ffxvr.manifest.json is overwritten by the copy.
- **Fix, run-ffx:** collect `.log .txt .bmp .dds .bin`, as in S9c.
- **Accepted from the critique:**
  - The mod folder doesn't grow without bound. Capture names are reused, so it levels off.
  - Add a log line that shows what was kept:
    ```powershell
    $kept = @(Get-ChildItem -LiteralPath $dest -Recurse -File | Where-Object { $n = $_.Name; -not ($buildOut | Where-Object { $n -like $_ }) -and $n -notlike '*.manifest.json' })
    Log ("[deploy] kept {0} settings/capture/log files ({1:N1} MB)" -f $kept.Count, (($kept | Measure-Object Length -Sum).Sum / 1MB))
    ```
    (`Measure-Object -Sum` on Length is numeric, so it's fine in 5.1.)
  - `-Recurse` on the delete: the owner checks the managed output folder once. If it has no
    subfolders, drop `-Recurse`, so a hand-made `old\` A/B folder survives. Otherwise keep it.
- **Verify:**
  - Take an img/seq capture and a UI atlas dump, close the game, and run build-mod. The files are
    still there, and the "kept N files" line counts them.
  - The next run-ffx copies the .dds into `gamelogs\fahrenheit\mods\ffxvr\`.
- **Effort:** S

---

## S1: install-texture-pack -Id validation and ownership. AGREED

- **Verdict:** fix now.
- **Accepted:**
  - **No dots, `\z`, reserved names.** Confirmed: `ffxvr.` passed the old regex and is the VR mod's
    folder on Windows. `"abc`n"` passed `$`. t4 shows all of these refused under the new rule;
    `reremaster` and `re-remaster_v4` are accepted.
  - **Marker-based ownership test**, applied to install-into-existing-folder as well as uninstall.
  - **Uninstall with the folder already gone** still removes the loadorder line.
  - **Top-level DLL check only.**

Final design:
```powershell
$ErrorActionPreference = 'Stop'
if ($Id -notmatch '^[A-Za-z0-9][A-Za-z0-9_-]{0,63}\z' -or $Id -eq 'ffxvr' -or $Id -match '^(con|nul|aux|prn|com\d|lpt\d)\z') {
    throw "-Id '$Id' is not allowed: letters, digits, '-' and '_' only, and not 'ffxvr' (the VR mod)."
}
# (S9a Archive split here)
. (Join-Path $PSScriptRoot 'paths.ps1')
...
# Ours = our manifest marker (written first, before any extraction) or an efl\ folder, and no DLL at the root.
function Test-OurPack([string]$d, [string]$id) {
    $m = Join-Path $d "$id.manifest.json"
    $marker = (Test-Path -LiteralPath $m) -and ([IO.File]::ReadAllText($m) -match 'install-texture-pack\.ps1')
    $dlls   = @(Get-ChildItem -LiteralPath $d -File -Filter *.dll -ErrorAction SilentlyContinue).Count
    return ($dlls -eq 0) -and ($marker -or (Test-Path -LiteralPath (Join-Path $d 'efl')))
}
if (!(Test-Path -LiteralPath $mods)) { throw "Fahrenheit mods folder not found: $mods" }
Assert-FfxvrGameClosed 'the texture files'
Assert-FfxvrWritable $mods
if ((Test-Path -LiteralPath $modDir) -and -not (Test-OurPack $modDir $Id)) {
    throw "$modDir belongs to another mod (it has DLLs, or no manifest from this script). Pick another -Id; nothing changed."
}
if ($Uninstall) {
    if (Test-Path -LiteralPath $order) {
        Set-Content -LiteralPath $order -Value @(Get-Content -LiteralPath $order | Where-Object { $_.Trim() -ne $Id }) -Encoding ascii
        Log "removed $Id from loadorder"
    }
    if (Test-Path -LiteralPath $modDir) { Remove-Item -LiteralPath $modDir -Recurse -Force; Log "removed $modDir" }
    exit 0
}
```
- **Ownership check placement.** It sits before both paths. When the folder isn't ours, the script
  refuses the whole call and leaves the loadorder alone, because removing the line would disable
  the other mod.
- **Line 118** becomes `Set-Content -LiteralPath $order ... -Encoding ascii`.

**Verify:**
- Each of these is refused and nothing changes: `-Id . -Uninstall`, `-Id .. -Uninstall`,
  `-Id ffxvr. -Uninstall`, `-Id ffxvr -Uninstall`.
- `-Id ffxvr -Archive x.7z` (install) is refused too.
- `-Id reremaster -Uninstall` works.
- Delete `efl\` by hand and run uninstall again: it still works, through the marker.
- With the game running, the script says "close it first".

**Effort:** S

---

## S2: import-steam-saves overwrites Fahrenheit slots. AGREED

- **Verdict:** fix now.
- **Accepted:**
  - Print both dates for each clash and say which is newer.
  - Back up once, before the first overwrite, and name the files backed up.
  - `-Force` can be targeted at chosen slots.
  - FFX-2 is not in this script's process list (the section 0 default).
- **Changed from the critique:** `-Slot` is a **string**, not `int[]`. Under `powershell -File`,
  `-Slot 3,5` arrives as the text "3,5", and `[int[]]` turns it into **35** with no error (t8).
  That would silently target the wrong slot.

```powershell
param([switch]$Force, [string]$Slot = '', [string]$SteamId = '')    # -Slot "3,5": slot numbers as the game shows them minus 1 (ffx_003 = 3)
. (Join-Path $PSScriptRoot 'paths.ps1')
$game = Get-FfxvrGame
Assert-FfxvrGameClosed 'the save files'
$slots = @($Slot -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ } | ForEach-Object { 'ffx_{0:D3}' -f [int]$_ })
$dest = Join-Path $game 'fahrenheit\saves\0\default\FINAL FANTASY X'
$from = Get-FfxvrSteamSaveDir $SteamId
if (-not $from) { Write-Host 'No Steam FFX save folder found under Documents\SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster'; exit 1 }
$files = @(Get-ChildItem -LiteralPath $from -File -Filter 'ffx_*')
if ($files.Count -eq 0) { Write-Host "No ffx_* saves in $from"; exit 1 }
Assert-FfxvrWritable $dest
New-Item -ItemType Directory -Force -Path $dest | Out-Null
$new = @(); $same = 0; $clash = @()
foreach ($f in $files) {
    $t = Join-Path $dest $f.Name
    if (-not (Test-Path -LiteralPath $t)) { $new += $f; continue }
    if ((Get-FileHash -LiteralPath $t).Hash -eq (Get-FileHash -LiteralPath $f.FullName).Hash) { $same++; continue }
    $clash += $f
}
$replace = @($clash | Where-Object { $Force -and ($slots.Count -eq 0 -or $slots -contains $_.Name) })
if ($replace.Count -gt 0) {
    $backup = Join-Path (Join-Path $FfxvrRepo 'logs') ('fahrenheit-saves-backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    New-Item -ItemType Directory -Force -Path $backup | Out-Null
    Get-ChildItem -LiteralPath $dest -File | Copy-Item -Destination $backup -Force
    Write-Host "Backed up the Fahrenheit saves ($((Get-ChildItem -LiteralPath $backup -File | ForEach-Object Name) -join ', ')) to $backup"
}
foreach ($f in $new + $replace) { Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $dest $f.Name) -Force }
Write-Host "Imported $($new.Count) new save(s), replaced $($replace.Count), $same already identical. From $from"
foreach ($f in @($clash | Where-Object { $replace -notcontains $_ })) {
    $vr = Get-Item -LiteralPath (Join-Path $dest $f.Name)
    $which = if ($f.LastWriteTime -gt $vr.LastWriteTime) { 'Steam newer' } else { 'VR newer' }
    Write-Host ('  kept {0}: Steam {1:yyyy-MM-dd HH:mm}, VR {2:yyyy-MM-dd HH:mm} ({3})' -f $f.Name, $f.LastWriteTime, $vr.LastWriteTime, $which)
}
if ($clash.Count -gt $replace.Count) {
    Write-Host '  Steam and Fahrenheit number slots separately, so a kept slot may be a different game.'
    Write-Host '  -Force -Slot 3 replaces just ffx_003 (or -Force alone for all), after a backup in logs\.'
}
```
(`ForEach-Object Name` is the 3.0+ simplified syntax and fine in 5.1. The `-Slot` numbering matches
`export-save-to-steam -Slot`.)

**Docs:** in README-zip section 6 and INSTALL-ALPHA section 7, add "(it never replaces a VR save
unless you pass `-Force`)".

**Verify:**
- Put different saves in slot 1 on each side and run the import: a "kept ffx_000: Steam ..., VR ...
  (VR newer)" line appears, and the VR save still loads.
- `-Force -Slot 0` replaces only that slot and creates the backup folder.
- A rerun reports the save as identical.

**Effort:** S

---

## S3: build-mod deploys half a build. AGREED

- **Verdict:** fix now.
- **Accepted:**
  - Default process names are `FFX` and `fhstage0`.
  - The early check becomes a **warning**, so the owner can still compile with the game open. The
    hard stop is at deploy.
  - The Debug filter runs on the path **relative to `src\managed`**.
  - The deploy-time refusal is written to the build log.
  - The fhstage0 hint comes from the helper.

```powershell
# after line 28
if (-not $NoDeploy -and (Get-FfxvrGameProcess).Count -gt 0) { Log 'NOTE: the game is running; the build will run, but deploying will stop until it is closed.' }
# preflight
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) { Log 'ERROR: dotnet (the .NET 10 SDK) is not on PATH. winget install Microsoft.DotNet.SDK.10'; exit 1 }
# 161-164 (same filter in package-release, S6)
       Where-Object { $r = $_.FullName.Substring($srcMgd.Length); $r -match '\\(bin|artifacts)\\' -and $r -notmatch '\\(debug|dbg)(_[^\\]*)?\\' } |
# deploy (replaces 184-201)
if ((Get-FfxvrGameProcess).Count -gt 0) { Log 'ERROR: the game is running; nothing deployed. Close it and run build-mod again.'; Assert-FfxvrGameClosed 'the mod''s DLLs' }
$dest = Join-Path $modsDir $modId
Log ''; Log "[deploy] -> $dest"
$buildOut = @('*.dll', '*.pdb', '*.deps.json', '*.runtimeconfig.json')
try {
    if (Test-Path -LiteralPath $dest) {
        Get-ChildItem -LiteralPath $dest -Recurse -File |
            Where-Object { $n = $_.Name; [bool]($buildOut | Where-Object { $n -like $_ }) } | Remove-Item -Force -ErrorAction Stop
    }
    New-Item -ItemType Directory -Force -Path $dest -ErrorAction Stop | Out-Null
    Get-ChildItem -LiteralPath $outDir | Copy-Item -Destination $dest -Recurse -Force -ErrorAction Stop
    Copy-Item -LiteralPath $natDll          -Destination $dest -Force -ErrorAction Stop
    Copy-Item -LiteralPath $oxrDll.FullName -Destination $dest -Force -ErrorAction Stop
} catch {
    Log ''; Log "ERROR: deploy failed: $($_.Exception.Message)"
    Log '  The mod folder may be incomplete. Close whatever holds it (game, Explorer preview, antivirus) and run build-mod again.'
    Log "Log: $log"; exit 1
}
# + R5's "kept N files" line; loadorder 213-225 with -LiteralPath
```
**Verify:**
- With the game open, the NOTE appears, the build runs, then "ERROR: the game is running; nothing
  deployed" is printed with exit 1, and the log file shows it.
- With dotnet removed from PATH, the script stops at preflight with exit 1.
- A normal run deploys and returns `$LASTEXITCODE` 0.

**Effort:** S

---

## S4: build-fahrenheit-full exit code and INSTALL-ALPHA. AGREED

- **Verdict:** fix now (script and doc).
- **Accepted:** the critic found a real hole.
  - With `buildCode = 1` and no recognised error line, my rule passed (stale loaders looked fine).
  - The new rule accepts a non-zero exit **only when the quirk line is positively identified**.
    t4 shows all four cases: (0, none) OK; (1, none) FAILED; (1, dbghelp only) OK;
    (1, dbghelp + C2065) FAILED.
  - The commit is resolved with `rev-parse --verify "$Commit^{commit}"`. Tested: an uppercase
    abbreviation resolves, and a bad hash gives rc 1.
- **Widened error pattern:** it now also matches `EXEC : error : ...` and `error : ...` with no
  code (t4). Over-matching only pushes toward FAILED, which is the safe direction.

```powershell
# after the submodule/log lines (~111)
$head = "$(& git -C $repo rev-parse HEAD 2>$null)".Trim()
if (-not $Latest) {
    $want = "$(& git -C $repo rev-parse --verify --quiet "$Commit^{commit}" 2>$null)".Trim()
    if (-not $want -or $head -ne $want) { Log "ERROR: _ext\fahrenheit is at '$head', not $Commit (see the git output above). Nothing built."; exit 1 }
}
# build pipeline
$errLines = New-Object System.Collections.Generic.List[string]
$errRe    = '(^|[\s:])error(\s+[A-Z]+\d+)?\s*:'
& $msbuild .\Fahrenheit.slnx /p:Configuration=Release /v:minimal 2>&1 |
    ForEach-Object { Log "  $_"; if ("$_" -match $errRe) { $errLines.Add("$_") } }
# results: count MISSING into $missing, then
$knownQuirk = 'dbghelp'      # MSBuild's dbghelp copy step fails without Debugging Tools; install-fahrenheit supplies the DLL
$realErrors = @($errLines | Sort-Object -Unique | Where-Object { $_ -notmatch $knownQuirk })
$ok = ($restoreCode -eq 0) -and ($missing -eq 0) -and
      ($buildCode -eq 0 -or ($errLines.Count -gt 0 -and $realErrors.Count -eq 0))
if ($buildCode -ne 0 -and $ok) { Log "NOTE: msbuild exit code $buildCode came only from the known dbghelp step; the loaders were built." }
if (-not $ok -and $realErrors.Count -gt 0) { Log 'Errors:'; $realErrors | Select-Object -First 20 | ForEach-Object { Log "  $_" } }
if ($ok) { Log 'RESULT: OK. Next: tools\install-fahrenheit.ps1' } else { Log 'RESULT: FAILED. Don''t run install-fahrenheit.ps1 until this builds.' }
Log "Log: $log"
exit $(if ($ok) { 0 } else { 1 })
```
- **Leave line 21 (`$Commit = '...'`) unchanged:** package-release reads it with a regex.
- **Rejected, as the critic also concluded:** deleting the deploy files before the build.

INSTALL-ALPHA 94-97 becomes:
> The first script clones Fahrenheit into `_ext\fahrenheit` and builds it. It ends with **RESULT: OK** or
> **RESULT: FAILED** (exit code 1), above a list of the four loader files. MSBuild itself may report
> exit code 1 because of a known step about debugging DLLs (dbghelp); the script recognises that and
> says so. `-Latest` builds Fahrenheit's newest code instead, which is untested with this mod.

**Owner, once:** grep an old `logs\build-full-*.txt` for `error` and confirm that the quirk line
both matches `$errRe` and contains "dbghelp". If not, adjust `$knownQuirk`. A mismatch shows up as a
visible FAILED that lists the line.

**Verify:**
- A normal run ends RESULT: OK, possibly with the NOTE, and exit code 0.
- `-Commit 0000000` errors with exit 1.
- `-Commit ECB6702` (uppercase) is accepted.
- A deliberately broken build (for example a syntax error in a Fahrenheit .cs file under `_ext`)
  gives RESULT: FAILED with exit 1. Revert afterwards with `git -C _ext\fahrenheit checkout .`.

**Effort:** S

---

## S5: build-fps60 toolset, Replace count, CFG-MENU. AGREED

- **Verdict:** fix now.
- **Accepted:**
  - The table is bounded: unknown MSVC versions are logged, not guessed.
  - Reading the file is null-safe.
  - game-settings gets the CRLF fix and now dot-sources paths.ps1 for the game check.
  - The PlatformToolsets folder scan stays optional. It isn't verifiable here, so the table is kept.

```powershell
$toolsets = @(Get-ChildItem -LiteralPath $msvc -Directory | ForEach-Object {
    $mi = 0; $v = $_.Name.Split('.')
    if ($v.Count -ge 2 -and $v[0] -eq '14' -and [int]::TryParse($v[1], [ref]$mi)) {
        if     ($mi -ge 60) { Log "[fps60] unknown MSVC $($_.Name), skipped" }   # newer than this table: don't guess
        elseif ($mi -ge 50) { 'v145' }      # VS 2026
        elseif ($mi -ge 30) { 'v143' }      # VS 2022 (14.3x and 14.4x)
        elseif ($mi -ge 20) { 'v142' } elseif ($mi -ge 10) { 'v141' } else { 'v140' }
    }
} | Where-Object { $_ } | Sort-Object -Unique -Descending)
...
$text = "$(Get-Content -LiteralPath $ini -Raw)"          # empty file -> '' (t4), not $null
if ($Fps -gt 0) { if ($text -match '(?m)^fps=\d+') { $text = [regex]::Replace($text, '(?m)^fps=\d+', "fps=$Fps"); Log "fps60.ini: fps=$Fps" }
                  else { Log "WARNING: no fps= line in $ini; -Fps $Fps not applied." } }
$cfgLine = "; VR setup: Config-page Frame Rate row off (install-time text fault under Fahrenheit)`r`nCFG-MENU=0`r`n"
$rowsRe  = New-Object regex '(?m)^\[rows\]\r?\n'
if     ($text -match '(?m)^CFG-MENU=') { $text = [regex]::Replace($text, '(?m)^CFG-MENU=[^\r\n]*', 'CFG-MENU=0'); Log 'fps60.ini: CFG-MENU=0' }
elseif ($rowsRe.IsMatch($text))         { $text = $rowsRe.Replace($text, "`$0$cfgLine", 1); Log 'fps60.ini: CFG-MENU=0 added' }
else   { $text = $text.TrimEnd() + "`r`n`r`n[rows]`r`n$cfgLine"; Log 'WARNING: fps60.ini had no [rows]; added one with CFG-MENU=0. Check fps60.log.' }
Set-Content -LiteralPath $ini $text -NoNewline
```
(The `Log` inside the ForEach-Object depends on S9b. With today's Tee-Object `Log`, its text would
land in `$toolsets`. With `Write-FfxvrLog` (Write-Host) it doesn't, so both changes go into
build-fps60 in the same edit.)

**game-settings.ps1:**
- Add `. (Join-Path $PSScriptRoot 'paths.ps1')` and
  `Assert-FfxvrGameClosed 'GameSetting.ini' @('FFX', 'FFX-2', 'FFX&X-2_LAUNCHER', 'fhstage0')`.
  The launcher's process name is an assumption; the owner confirms it in Task Manager once.
- Change lines 34-35 from `.*$` to `[^\r\n]*`.
- Use `-LiteralPath` throughout.
- Dot-sourcing paths.ps1 here has no side effects, because Get-FfxvrGame isn't called.

**Verify:**
- On the owner's PC the log shows v145 and "CFG-MENU=0".
- Temporarily set `$wanted = 'v146'`: the log shows "building with v145" (or v143 on VS 2022), not "v".
- `game-settings -Profile vr` while FFX runs is refused. With the game closed it works, and
  Notepad++ shows CRLF on the edited lines.

**Effort:** S

---

## S6: package-release under 5.1; manifest pick. AGREED

- **Verdict:** fix now.
- **Fix:**
  - The `Invoke-GitQuiet` function (function-local `$ErrorActionPreference = 'Continue'`,
    `& git @args 2>$null`, `$null` on non-zero exit) replaces lines 87, 90 and 107.
  - The managed pick takes the newest `ffxvr.dll` that has a manifest beside it and is not Debug.
    The Debug test uses the path **relative to `src\managed`**, the same as S3.
  - It prints "Mod files from: <dir>", plus any older candidates.
- **Verify:**
  - In powershell.exe 5.1, run on a copy of the repo without `.git`. It finishes, labelled
    `...-nogit`, with no red NativeCommandError.
  - Do a Debug build in VS after a Release build, then run with `-SkipBuild`. "Mod files from"
    names the Release folder.
- **Effort:** S

---

## S7: install-fahrenheit with the game running; build-fps60 dbghelp ownership. AGREED

- **Verdict:** fix now.
- **Accepted:**
  - **Install:** overwrite is allowed if `Test-Fps60Loader` **or** `fps60_ffx.dll` exists, which is
    today's rule. That way a `git pull` that changes the loader's hash doesn't block updates.
  - **Uninstall:** `Test-Fps60Loader` only.
  - FFX-2 and the launcher are in build-fps60's process check only.
  - The writable check runs before `New-Item`.

```powershell
# install-fahrenheit
$game = Get-FfxvrGame
Assert-FfxvrGameClosed 'Fahrenheit''s files'
Assert-FfxvrWritable $dst                     # probes the nearest existing parent if bin\ is missing
New-Item -ItemType Directory -Force -Path $dst | Out-Null
try { Get-ChildItem -LiteralPath $dst -Force | Remove-Item -Recurse -Force
      Get-ChildItem -LiteralPath $src | Copy-Item -Destination $dst -Recurse -Force }
catch { Write-Host "ERROR: $($_.Exception.Message)"; Write-Host "  $dst is incomplete. Close anything using it and run install-fahrenheit.ps1 again."; exit 1 }

# build-fps60
$rootExes = @('FFX', 'FFX-2', 'FFX&X-2_LAUNCHER', 'fhstage0')     # every exe in the game root loads its dbghelp.dll
function Test-Fps60Loader([string]$p) {
    if (-not (Test-Path -LiteralPath $p)) { return $false }
    $built = Join-Path $ext 'artifacts\loader\Release\dbghelp.dll'
    if ((Test-Path -LiteralPath $built) -and (Get-FileHash -LiteralPath $built).Hash -eq (Get-FileHash -LiteralPath $p).Hash) { return $true }
    $b = [IO.File]::ReadAllBytes($p)
    return ([Text.Encoding]::ASCII.GetString($b) -match 'fps60_ffx') -or ([Text.Encoding]::Unicode.GetString($b) -match 'fps60_ffx')
}
# uninstall: Assert-FfxvrGameClosed 'fps60_ffx.dll and dbghelp.dll' $rootExes; remove dbghelp.dll only if Test-Fps60Loader,
#            else log "kept ...: it isn't FFX-60fps's loader"; remove fps60_ffx.dll
# install:   Assert-FfxvrGameClosed 'dbghelp.dll and fps60_ffx.dll' $rootExes
$existing = Join-Path $game 'dbghelp.dll'
if ((Test-Path -LiteralPath $existing) -and -not ((Test-Fps60Loader $existing) -or (Test-Path -LiteralPath (Join-Path $game 'fps60_ffx.dll')))) {
    Log "WARNING: $existing already exists and is not FFX-60fps's (another dbghelp.dll mod?). Not overwriting."; exit 1 }
# 111-112: Copy-Item -LiteralPath ... -ErrorAction Stop
```
- **install-mod:86** checks for `fps60_ffx.dll` instead of `dbghelp.dll`.
- **Owner, once:** run `Select-String -LiteralPath "<game>\dbghelp.dll" -Pattern fps60_ffx`, then
  the same with `-Encoding unicode`. That shows whether the string test holds. The hash test covers
  our own build regardless.

**Verify:**
- install-fahrenheit with the game open is refused, and bin is intact.
- Put SysWOW64's dbghelp.dll next to FFX.exe and run `build-fps60 -Uninstall`: it keeps the file.
- A normal install, update and uninstall still work.

**Effort:** S

---

## S8: -LiteralPath sweep and vdf encoding. AGREED

- **Verdict:** fix now for player-facing scripts and anything that writes to the game folder. Fix
  repo-only paths in dev scripts when those scripts are next touched.
- **Fix:** the call-site table in G5-proposal.md S8 is unchanged. paths.ps1 is as in section 0.
  Wildcard-dependent copies become `GCI -LiteralPath | Copy-Item`.
- **Accepted:** the player's **extract folder** can contain brackets too. Windows PowerShell before 6
  is known to have trouble invoking scripts from bracketed paths, and that can't be fixed in code,
  because `-File` fails before our code runs. So:
  - add to the verify list: extract to `D:\Mods [VR]\` and run `install-mod.ps1` under
    **powershell.exe 5.1**;
  - if that fails, add one sentence to README-zip's extract paragraph (S9f): "Use a folder whose name
    has no `[` or `]`."
- **Verify:**
  1. Junction `C:\FFX [test]` pointing to the game, with `$env:FFXVR_GAME_DIR` set to it. Run
     check-prereqs, install-mod, `install-mod -Uninstall` and run-ffx.
  2. Install a pack named `pack [v4].7z`.
  3. Extract the zip to a bracketed folder and run under 5.1 (above). Remove the junction afterwards.
- **Effort:** M (mechanical)

---

## S9: various. AGREED

- **S9a, Archive split.** As proposed: split on commas unless the whole string is an existing file
  (t-args: 2 elements under both -File and -Command).
- **S9b, UTF-8 logs.** `Write-FfxvrLog` replaces the Tee-Object logging in:
  - run-ffx 43-44
  - install-texture-pack 47, 82, 124
  - build-fps60 37

  Leave build-fahrenheit.ps1, which looks superseded.
- **S9c, run-ffx collector.** As proposed:
  - Scan the game root and the `fahrenheit\` root files.
  - Scan `fahrenheit\*` subfolders recursively, except `mods` and `saves`.
  - Scan `mods\ffxvr` recursively.
  - Only `.log .txt .bmp .dds .bin` files written since the start of the run.
  - Keep paths relative to the game folder (tested on a fake bracketed tree).
  - **Owner, once:** check an old gamelogs folder for any paths outside the scanned folders.
- **S9d, export comment.** Line 21 becomes "-1 = the lowest unused slot (from ffx_000)". Line 64
  becomes "the lowest unused number (a gap left by a deleted save is reused)".
- **S9e, Steam account.** Both save scripts call `Get-FfxvrSteamSaveDir $SteamId`, which uses the
  newest-save sort from section 0. Export also gets `-SteamId` and
  `Assert-FfxvrGameClosed 'the Steam save files'`.
- **S9f, extract folder.** README-zip 24-26: "Extract it anywhere outside the game folder, e.g.
  `Documents`. Inside you'll find a folder `ffx-vr-{VERSION}` with this README and `tools\` in it.
  Run every command below from that folder (open it in Explorer, right-click an empty spot > Open
  in Terminal), in PowerShell, not as administrator." Add S8's "no `[` `]`" sentence if the 5.1
  test fails. install-mod.ps1:11 gets the matching comment.
- **S9g, not writable.**
  - **Accepted:** Steam grants normal users write access to its own install folder, so the default
    library is normally writable. The "common case" premise is withdrawn.
  - The assert stays, but its message (section 0) states what failed rather than guessing "Program
    Files". The `icacls` line is dropped.
  - install-mod calls `Assert-FfxvrWritable $mods` before `New-Item` at 106, and before the
    uninstall loop.
  - install-mod 31-34 becomes `Assert-FfxvrGameClosed 'the mod''s DLLs'`.
- **Extras:**
  - game-settings: see S5.
  - install-mod:86: see S7.
  - INSTALL-ALPHA: mark "PowerShell 7" optional, because every command uses powershell.exe (5.1).
- **Verify:**
  - S9a: `-Archive "a.7z,b.7z"` extracts both archives.
  - S9b: Notepad shows UTF-8 for `logs\run-*.txt`.
  - S9c: with the texture pack installed, collection takes about a second and keeps subfolders.
  - S9e: a fake second account folder produces the listing with dates.
  - S9g: `icacls <mods> /deny "${env:USERNAME}:(W)"` makes install-mod print the message with exit 1
    and no red stack trace. Undo with `/remove:d`.
- **Effort:** S each.

---

## Order (one edit pass per file)
1. paths.ps1 (section 0). Parse-check it in powershell.exe 5.1.
2. build-mod (R5, S3) and run-ffx (R5, S9b, S9c).
3. Before any zip goes out:
   - install-texture-pack (S1, S9a, S9b, S8)
   - import and export (S2, S9d, S9e, S8)
   - install-mod (S9f, S9g, S7 note, S8)
   - README-zip
4. build-fahrenheit-full and INSTALL-ALPHA (S4); build-fps60 and install-fahrenheit (S5, S7);
   game-settings; package-release (S6).
