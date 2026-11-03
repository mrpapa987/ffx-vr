# import-hands.ps1 - make a VR hand pack from a character model YOU OWN, into the installed mod's hands\ folder.
#
# Runs tools\hands\import-hands.py (Python 3 + numpy) on a Wavefront OBJ (a full body in a T or A pose, Y up, its .mtl
# and textures beside it) and writes <game>\fahrenheit\mods\ffxvr\hands\<Name>\: right.glb, left.glb, hand.json and
# PRIVATE-do-not-share.txt. In game: F8 > VR hands > hand model > rescan, then pick it.
#
# The pack is made from your model: keep it on your PC and never share it (most character models, a game's above all, are
# someone else's copyright). The mod's tools never copy, package or delete it (install-mod -Uninstall -Purge names it first).
#
# Run with (from the repository or the release folder):
#   powershell -ExecutionPolicy Bypass -File .\tools\import-hands.ps1 "D:\models\Tidus - HP.obj" -Name tidus
# Options (all optional; see tools\hands\import-hands.py for more):
#   -Name tidus           the pack folder under hands\ (default: from the model's file name)
#   -Title "My hands"     the name shown in F8 (default: "<model name> hands")
#   -CutRight 0.095       move a hand's wrist cut (fraction of the model height back from the fingertips; -DebugImages
#   -CutLeft 0.11         prints the profile). -Cut sets both
#   -FlipRight / -FlipLeft   that hand's palm and back were detected the wrong way round
#   -Tip "x,y,z"          pick a hand by a point near its fingertips (model units); twice: -Tip "x,y,z","x,y,z"
#   -Height 1800          the model height in model units (a hands-only model)
#   -ZUp                  the model is Z up
#   -Only right|left      use one hand and mirror it
#   -HandLength 0.19      wrist to middle fingertip in metres, both hands (their wrist joints are moved to match)
#   -KeepProportions      keep each hand's own wrist estimate instead (the hands may then differ in length)
#   -DebugImages          print the search, write debug-preview.png into the pack folder (needs matplotlib)
#   -Preview preview.png  previews of the open / fist / point / grip poses (needs matplotlib). A bare file name goes into
#                         the pack folder; like the pack, it is refused inside a git checkout (it is a picture of your model)
#   -OutDir <folder>      write the pack there instead of the mod folder (never inside a git checkout)
#   -Python <exe>         the Python 3 to use (default: py -3, python, python3, whichever works first)
# Windows PowerShell 5.1 and PowerShell 7.

param(
    [Parameter(Mandatory = $true, Position = 0)][string]$Model,
    [string]$Name = '',
    [string]$Title = '',
    [double]$Cut = 0, [double]$CutRight = 0, [double]$CutLeft = 0,
    [switch]$FlipRight, [switch]$FlipLeft,
    [string[]]$Tip = @(),
    [double]$Height = 0,
    [switch]$ZUp,
    [ValidateSet('', 'right', 'left')][string]$Only = '',
    [double]$HandLength = 0,
    [switch]$KeepProportions,
    [switch]$DebugImages,
    [string]$Preview = '',
    [string]$OutDir = '',
    [string]$Python = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$inv = [Globalization.CultureInfo]::InvariantCulture

$script = Join-Path $PSScriptRoot 'hands\import-hands.py'
foreach ($f in 'import-hands.py', 'objio.py', 'handlib.py') {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot "hands\$f"))) { Write-Host "tools\hands\$f is missing: extract the whole release zip again (or check out the repository)."; exit 1 }
}
if (-not (Test-Path -LiteralPath $Model)) { Write-Host "No such file: $Model"; exit 1 }
$Model = (Resolve-Path -LiteralPath $Model).ProviderPath

# ------------------------------------------------------------------ Python 3 and numpy
# 'Continue' from here: in Windows PowerShell 5.1 a native program's stderr under 'Stop' (even redirected) is a terminating
# error, and Python writes its tracebacks and warnings there.
$ErrorActionPreference = 'Continue'
# A candidate counts if it runs and says 3.8 or newer. (Windows' "python" can be the Microsoft Store stub, which prints
# nothing and fails: that is skipped.)
# (No double quotes in -c code: Windows PowerShell 5.1 does not escape them for a native program.)
function Test-Python([string]$exe, [string[]]$pre) {
    try {
        $out = & $exe @($pre + @('-c', 'import sys; print(sys.version_info[0], sys.version_info[1])')) 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $out) { return $null }
        $v = ("$out".Trim() -split '\s+')
        if ($v.Count -ge 2 -and [int]$v[0] -eq 3 -and [int]$v[1] -ge 8) { return ($v[0] + '.' + $v[1]) }
    } catch { }
    return $null
}
$cands = @()
if ($Python) { $cands += [pscustomobject]@{ Exe = $Python; Pre = @() } }
else {
    $cands += [pscustomobject]@{ Exe = 'py'; Pre = @('-3') }
    $cands += [pscustomobject]@{ Exe = 'python'; Pre = @() }
    $cands += [pscustomobject]@{ Exe = 'python3'; Pre = @() }
}
$py = $null; $pyPre = @(); $pyVer = $null
foreach ($c in $cands) {
    $ver = Test-Python $c.Exe ([string[]]$c.Pre)
    if ($ver) { $py = $c.Exe; $pyPre = [string[]]$c.Pre; $pyVer = $ver; break }
}
if (-not $py) {
    Write-Host 'Python 3 (3.8 or newer) was not found.'
    Write-Host '  Install it from https://www.python.org/downloads/ (tick "Add python.exe to PATH"), or: winget install Python.Python.3.12'
    Write-Host '  Then open a new PowerShell window and run this again (or pass -Python <path to python.exe>).'
    exit 1
}
$pyShown = (@($py) + $pyPre) -join ' '
$np = 1
try { & $py @($pyPre + @('-c', 'import numpy')) 2>$null | Out-Null; $np = $LASTEXITCODE } catch { $np = 1 }
if ($np -ne 0) {
    Write-Host "Python $pyVer ($pyShown) has no numpy, which the importer needs. Install it with:"
    Write-Host "    $pyShown -m pip install numpy"
    Write-Host "  (and matplotlib too for -Preview / -DebugImages:  $pyShown -m pip install matplotlib)"
    exit 1
}
Write-Host "Python $pyVer ($pyShown) with numpy"

# ------------------------------------------------------------------ where the pack goes
if (-not $Name) {
    $Name = ([IO.Path]::GetFileNameWithoutExtension($Model).ToLowerInvariant() -replace '[^a-z0-9_-]+', '-') -replace '-{2,}', '-'
    $Name = $Name.Trim('-'); if ($Name.Length -gt 40) { $Name = $Name.Substring(0, 40).Trim('-') }
    if (-not $Name) { $Name = 'my-hands' }
}
# no trailing space or dot (Windows drops them: review N7)
if ($Name -notmatch '^[A-Za-z0-9]([A-Za-z0-9 _.-]{0,62}[A-Za-z0-9_-])?$') { Write-Host "-Name '$Name': use letters, digits, '-', '_' (a folder name)."; exit 1 }
if ($FfxvrHandPacks -contains $Name.ToLowerInvariant()) { Write-Host "-Name $Name is one of the shipped packs ($($FfxvrHandPacks -join ', ')): pick another name."; exit 1 }
if ($Title -match '"') { Write-Host '-Title may not contain a double quote.'; exit 1 }   # review N6: 5.1 would split it
if ($OutDir) { $out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir).TrimEnd('\', '/') }   # N6: no "...\" into Python
else {
    $game = Get-FfxvrGame
    $mod = [IO.Path]::Combine($game, 'fahrenheit', 'mods', 'ffxvr')
    if (-not (Test-Path -LiteralPath (Join-Path $mod 'ffxvr.dll'))) {
        Write-Host "The mod is not installed in $mod (no ffxvr.dll): install it first (tools\install-mod.ps1 or tools\build-mod.ps1), or give -OutDir."
        exit 1
    }
    $out = [IO.Path]::Combine($mod, 'hands', $Name)
}

# ------------------------------------------------------------------ run
$a = @($script, $Model, '--out', $out)
if ($Title)      { $a += @('--name', $Title) }
if ($Cut -gt 0)      { $a += ('--cut=' + $Cut.ToString($inv)) }
if ($CutRight -gt 0) { $a += ('--cut-right=' + $CutRight.ToString($inv)) }
if ($CutLeft -gt 0)  { $a += ('--cut-left=' + $CutLeft.ToString($inv)) }
if ($FlipRight)  { $a += '--flip-right' }
if ($FlipLeft)   { $a += '--flip-left' }
foreach ($t in $Tip) { $a += ('--tip=' + ($t -replace '\s', '')) }
if ($Height -gt 0)     { $a += ('--height=' + $Height.ToString($inv)) }
if ($ZUp)        { $a += '--z-up' }
if ($Only)       { $a += @('--only', $Only) }
if ($HandLength -gt 0) { $a += ('--hand-length=' + $HandLength.ToString($inv)) }
if ($KeepProportions) { $a += '--keep-proportions' }
if ($DebugImages) { $a += '--debug' }
if ($Preview) {   # a bare name: the importer puts it into the pack folder; a path: resolved here (refused there inside a git checkout)
    if (Split-Path -Parent $Preview) { $Preview = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Preview).TrimEnd('\', '/') }
    $a += @('--preview', $Preview)
}

Write-Host "Model: $Model"
Write-Host "Pack:  $out"
Write-Host ''
& $py @($pyPre + $a)
$rc = $LASTEXITCODE
Write-Host ''
if ($rc -ne 0) { Write-Host "The import failed (exit code $rc): see the message above."; exit $rc }
if ($OutDir) { Write-Host "Made the pack in $out (copy that folder to <game>\fahrenheit\mods\ffxvr\hands\ to use it)." }
else { Write-Host "Made hands\$Name. In game: F8 > VR hands (experimental) > hand model > rescan, then pick it." }
Write-Host 'It is made from your model: keep it on this PC, do not share the folder.'
exit 0
