# convert-fmv-depth.ps1 - make depth for the game's FMVs, on THIS PC, from YOUR copy of the game.
#
# Runs tools\fmvdepth\convert.py: each video of the game's list is read straight out of data\FFX_Data.vbf (nothing is
# extracted), decoded by ffmpeg and run through Video-Depth-Anything-Small on your NVIDIA GPU. It writes one .fvd per video
# (8-bit relative depth, 320x180 per frame) and manifest.json into <game>\fahrenheit\mods\ffxvr\fmvdepth\.
# Nothing in the game's data is changed. The depth is made from Square Enix's videos: it stays on this PC. Never share or
# upload it, or the previews (the mod's tools never package or delete that folder).
#
# First run: sets up its own Python environment (several GB) under %LOCALAPPDATA%\ffxvr\:
#   fmvdepth-venv\     Python venv with PyTorch (CUDA 12.4), numpy, OpenCV (headless), einops, easydict, tqdm (pinned)
#   fmvdepth-vda\      Video-Depth-Anything's code (Apache-2.0) at the pinned commit, from GitHub
#   fmvdepth-models\   the Small model's weights (Apache-2.0, ~110 MB) from Hugging Face, with their SHA-256
# Later runs reuse them. Delete %LOCALAPPDATA%\ffxvr\fmvdepth-* to remove them (the depth itself stays in the mod folder).
#
# Needs: Windows 10/11, an NVIDIA GPU with a current driver (8 GB VRAM or more; tested design: RTX 3080 Ti, 12 GB),
# Python 3.10 - 3.13 (py launcher), ffmpeg + ffprobe (winget install Gyan.FFmpeg), ~1.5 GB free for the depth (US list) and
# ~5 GB for the environment. Time: about 1-2 hours for the US list on a 3080 Ti class GPU (an estimate until measured).
#
# Run with (from the repository):
#   powershell -ExecutionPolicy Bypass -File .\tools\convert-fmv-depth.ps1 -Preview           (S0: SIN_us + BTS_us and previews)
#   powershell -ExecutionPolicy Bypass -File .\tools\convert-fmv-depth.ps1                    (every video of the US list)
# Options:
#   -Only SIN_us,BTS_us   just these videos (names as in the game's list; case and .webm optional)
#   -All                  every .webm in the archive (95 files, ~2x the US list)
#   -Region us|jp|asia    which game video list (default us)
#   -InputSize 518|392    the model's input height (default 518; 392: faster, less VRAM)
#   -Preview              afterwards, side-by-side check videos (video | depth) of the -Only videos (default SIN_us,BTS_us)
#                         into fmvdepth\preview\. PRIVATE: they show the game's videos.
#   -Frame 600            with -Preview: also a PNG of that frame
#   -Gray                 previews in grey instead of a colour map
#   -Force                convert again even if done (otherwise finished videos with the same settings are skipped)
#   -List                 show what would be converted, and stop
#   -SetupOnly            set up the environment, download code and weights, and stop
#   -Ffmpeg <path>        ffmpeg.exe (or its folder) if it is not on PATH; ffprobe.exe must be next to it
#   -Python <exe>         the Python used to make the venv (default: py -3, then py -3.13 ... -3.10, python)
#   -OutDir <folder>      write the depth there instead of the mod folder (never inside a git checkout)
# Windows PowerShell 5.1 and PowerShell 7.

param(
    [string[]]$Only = @(),
    [switch]$All,
    [ValidateSet('us', 'jp', 'asia')][string]$Region = 'us',
    [ValidateSet(518, 392)][int]$InputSize = 518,
    [switch]$Preview,
    [int]$Frame = -1,
    [switch]$Gray,
    [switch]$Force,
    [switch]$List,
    [switch]$SetupOnly,
    [string]$Ffmpeg = '',
    [string]$Python = '',
    [string]$OutDir = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'paths.ps1')
$ProgressPreference = 'SilentlyContinue'      # 5.1's Invoke-WebRequest progress bar makes downloads many times slower

# ------------------------------------------------------------------ pins (keep in step with tools\fmvdepth\vda_model.py)
$VdaCommit   = '4f5ae23172ba60fd7bc11ef671cca678842c7072'   # DepthAnything/Video-Depth-Anything, 7 Oct 2025
$VdaZipUrl   = "https://github.com/DepthAnything/Video-Depth-Anything/archive/$VdaCommit.zip"
# Only the Small model: Apache-2.0. The Base and Large weights are CC-BY-NC-4.0 and are never offered.
$WeightsName = 'video_depth_anything_vits.pth'
$WeightsUrl  = "https://huggingface.co/depth-anything/Video-Depth-Anything-Small/resolve/main/$WeightsName"
# TODO(pin): the SHA-256 of video_depth_anything_vits.pth. Hugging Face could not be reached when this was written, so it
# is not known yet. Until it is set here, the first download's hash is recorded in fmvdepth-models\<name>.sha256 and
# printed, and later runs check the file against that record. Once the owner has a good download, paste its hash here
# (lower case, 64 hex digits) and pin the URL to that Hugging Face revision instead of 'main'.
$WeightsSha256 = ''
$WeightsMinMB = 50; $WeightsMaxMB = 250     # 28.4M parameters x 4 bytes = ~114 MB; anything else is not the file
$TorchIndex  = 'https://download.pytorch.org/whl/cu124'
$TorchPins   = @('torch==2.6.0', 'torchvision==0.21.0')
$PyPiPins    = @('numpy==2.1.3', 'opencv-python-headless==4.10.0.84', 'einops==0.8.0', 'easydict==1.13', 'tqdm==4.67.1')
$DepsSpec    = "v1; $($PyPiPins -join ' '); $($TorchPins -join ' ') from $TorchIndex"

$root    = Join-Path $env:LOCALAPPDATA 'ffxvr'
$venv    = Join-Path $root 'fmvdepth-venv'
$vdaDir  = Join-Path $root 'fmvdepth-vda'
$models  = Join-Path $root 'fmvdepth-models'
$here    = Join-Path $PSScriptRoot 'fmvdepth'
$convert = Join-Path $here 'convert.py'
foreach ($f in 'convert.py', 'vbf.py', 'fvd.py', 'depthproc.py', 'window.py', 'media.py', 'vda_model.py', 'preview.py') {
    if (-not (Test-Path -LiteralPath (Join-Path $here $f))) { Write-Host "tools\fmvdepth\$f is missing: check out the whole repository again."; exit 1 }
}
$OnlyList = (@($Only) | Where-Object { $_ }) -join ','   # -Only a,b in a session arrives as an array, with -File as one string
if ($All -and $OnlyList) { Write-Host '-All and -Only do not go together.'; exit 1 }
if ($Frame -ge 0) { $Preview = $true }

function Fail([string]$msg) { Write-Host ''; Write-Host "ERROR: $msg"; exit 1 }
function Step([string]$msg) { Write-Host ''; Write-Host "[$msg]" }

Write-Host 'FFX VR - FMV depth converter'
Write-Host 'The depth is made from the videos of YOUR copy of the game, on this PC. It is private: never share it.'

# 'Continue' from here: in Windows PowerShell 5.1 a native program's stderr under 'Stop' (even redirected) is a terminating
# error, and pip and Python write progress and warnings there.
$ErrorActionPreference = 'Continue'

# ------------------------------------------------------------------ the game
Step 'game'
$game = Get-FfxvrGame
$vbf  = [IO.Path]::Combine($game, 'data', 'FFX_Data.vbf')
if (-not (Test-Path -LiteralPath $vbf)) { Fail "no data\FFX_Data.vbf in $game. Is the game fully installed? (Steam: Properties > Installed Files > Verify)" }
Write-Host "  $game"
Write-Host ('  data\FFX_Data.vbf  {0:N1} GB (read only)' -f ((Get-Item -LiteralPath $vbf).Length / 1GB))
if ($OutDir) {
    $out = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($OutDir).TrimEnd('\', '/')
} else {
    $mod = [IO.Path]::Combine($game, 'fahrenheit', 'mods', 'ffxvr')
    if (-not (Test-Path -LiteralPath (Join-Path $mod 'ffxvr.dll'))) {
        Fail "the mod is not installed in $mod (no ffxvr.dll): install it first (tools\build-mod.ps1 or install-mod.ps1), or give -OutDir."
    }
    $out = Join-Path $mod $FfxvrFmvDepthDir
}
Write-Host "  output: $out"
if ((Get-FfxvrGameProcess).Count -gt 0) { Write-Host '  NOTE: the game is running. That works (the archive is only read), but the GPU is shared: close it for full speed.' }
Assert-FfxvrWritable $out

# ------------------------------------------------------------------ the GPU
Step 'GPU'
$smi = (Get-Command nvidia-smi -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
if (-not $smi) { $p = Join-Path $env:SystemRoot 'System32\nvidia-smi.exe'; if (Test-Path -LiteralPath $p) { $smi = $p } }
if (-not $smi) { Fail 'nvidia-smi was not found: the converter needs an NVIDIA GPU with its driver installed (https://www.nvidia.com/drivers).' }
$gpu = @(& $smi --query-gpu=name,memory.total,driver_version --format=csv,noheader,nounits 2>$null)
if ($LASTEXITCODE -ne 0 -or $gpu.Count -eq 0 -or -not "$($gpu[0])".Trim()) { Fail 'nvidia-smi found no working NVIDIA GPU.' }
$g = "$($gpu[0])" -split ',\s*'
$vramGB = 0.0; [void][double]::TryParse($g[1], [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$vramGB); $vramGB /= 1024
Write-Host ('  {0}, {1:N1} GB, driver {2}' -f $g[0].Trim(), $vramGB, $g[2].Trim())
$drv = 0.0; [void][double]::TryParse($g[2].Trim(), [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$drv)
if ($drv -gt 0 -and $drv -lt 528) { Write-Host '  WARNING: this driver is older than the CUDA 12.4 build of PyTorch expects (528 or newer). Update it if torch reports no GPU.' }
if ($vramGB -gt 0 -and $vramGB -lt 7.5 -and $InputSize -eq 518) { Write-Host '  NOTE: under 8 GB of VRAM: if it runs out of memory, use -InputSize 392.' }

# ------------------------------------------------------------------ ffmpeg
Step 'ffmpeg'
$ffmpegExe = $null
if ($Ffmpeg) {
    $c = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Ffmpeg)
    if (Test-Path -LiteralPath $c -PathType Container) { $c = Join-Path $c 'ffmpeg.exe' }
    if (Test-Path -LiteralPath $c) { $ffmpegExe = $c }
} else {
    $ffmpegExe = (Get-Command ffmpeg -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
}
if (-not $ffmpegExe) {
    Write-Host '  ffmpeg was not found. Install it with:  winget install Gyan.FFmpeg'
    Write-Host '  then open a NEW PowerShell window (for the PATH) and run this again, or pass -Ffmpeg <path to ffmpeg.exe>.'
    exit 1
}
$ffprobeExe = Join-Path (Split-Path -Parent $ffmpegExe) 'ffprobe.exe'
if (-not (Test-Path -LiteralPath $ffprobeExe)) {
    $ffprobeExe = (Get-Command ffprobe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
}
if (-not $ffprobeExe) { Fail "ffprobe.exe is not next to $ffmpegExe nor on PATH (a full ffmpeg build has both: winget install Gyan.FFmpeg)." }
$fv = @(& $ffmpegExe -hide_banner -version 2>$null)
if ($LASTEXITCODE -ne 0 -or $fv.Count -eq 0) { Fail "$ffmpegExe does not run." }
Write-Host "  $ffmpegExe"
Write-Host "  $($fv[0])"

# ------------------------------------------------------------------ Python + venv
Step 'Python'
# A candidate counts if it runs and is 3.10 - 3.13 (the versions the pinned torch has wheels for). Windows' "python" can be
# the Microsoft Store stub, which prints nothing and fails: that is skipped. (No double quotes in -c code: Windows
# PowerShell 5.1 does not escape them for a native program.)
function Test-Python([string]$exe, [string[]]$pre) {
    try {
        $o = & $exe @($pre + @('-c', 'import sys; print(sys.version_info[0], sys.version_info[1])')) 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $o) { return $null }
        $v = ("$o".Trim() -split '\s+')
        if ($v.Count -ge 2 -and [int]$v[0] -eq 3 -and [int]$v[1] -ge 10 -and [int]$v[1] -le 13) { return ($v[0] + '.' + $v[1]) }
    } catch { }
    return $null
}
$vpy = Join-Path $venv 'Scripts\python.exe'
if (Test-Path -LiteralPath $vpy) {
    $vv = Test-Python $vpy @()
    if (-not $vv) { Fail "the venv at $venv does not run a Python 3.10 - 3.13. Delete that folder and run this again." }
    Write-Host "  venv: $venv (Python $vv)"
} else {
    $cands = @()
    if ($Python) { $cands += [pscustomobject]@{ Exe = $Python; Pre = @() } }
    else {
        foreach ($v in '-3', '-3.13', '-3.12', '-3.11', '-3.10') { $cands += [pscustomobject]@{ Exe = 'py'; Pre = @($v) } }
        $cands += [pscustomobject]@{ Exe = 'python'; Pre = @() }
        $cands += [pscustomobject]@{ Exe = 'python3'; Pre = @() }
    }
    $py = $null; $pyPre = @(); $pyVer = $null
    foreach ($c in $cands) {
        $ver = Test-Python $c.Exe ([string[]]$c.Pre)
        if ($ver) { $py = $c.Exe; $pyPre = [string[]]$c.Pre; $pyVer = $ver; break }
    }
    if (-not $py) {
        Write-Host '  Python 3.10 - 3.13 was not found (PyTorch 2.6 has no wheels for other versions).'
        Write-Host '  Install it: winget install Python.Python.3.12   (or https://www.python.org/downloads/, tick "Add python.exe to PATH")'
        Write-Host '  Then open a new PowerShell window and run this again (or pass -Python <path to python.exe>).'
        exit 1
    }
    $free = (Get-PSDrive -Name ($env:LOCALAPPDATA.Substring(0, 1)) -ErrorAction SilentlyContinue).Free
    if ($free -and $free -lt 8GB) { Write-Host ('  WARNING: {0:N1} GB free on {1}: the environment needs about 5 GB.' -f ($free / 1GB), $env:LOCALAPPDATA.Substring(0, 2)) }
    Write-Host "  making a venv with Python $pyVer ($((@($py) + $pyPre) -join ' ')): $venv"
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    & $py @($pyPre + @('-m', 'venv', $venv))
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $vpy)) { Fail "could not make the venv at $venv." }
}
$env:PYTHONNOUSERSITE = '1'     # nothing from the user's own site-packages leaks into the venv
$marker = Join-Path $venv 'ffxvr-deps.txt'
$have = if (Test-Path -LiteralPath $marker) { [IO.File]::ReadAllText($marker).Trim() } else { '' }
if ($have -ne $DepsSpec) {
    Write-Host '  installing the pinned packages (PyTorch with CUDA is a ~2.5 GB download; this takes a while) ...'
    & $vpy -m pip install --disable-pip-version-check --no-input @PyPiPins
    if ($LASTEXITCODE -ne 0) { Fail 'pip could not install numpy / opencv / einops / easydict / tqdm (see above).' }
    & $vpy -m pip install --disable-pip-version-check --no-input @TorchPins --index-url $TorchIndex
    if ($LASTEXITCODE -ne 0) { Fail "pip could not install PyTorch from $TorchIndex (see above)." }
    [IO.File]::WriteAllText($marker, $DepsSpec, $FfxvrUtf8)
} else { Write-Host '  packages: as pinned' }
$tv = @(& $vpy -c 'import torch, numpy, cv2, einops, easydict, tqdm; print(torch.__version__, torch.version.cuda, torch.cuda.is_available())' 2>&1)
if ($LASTEXITCODE -ne 0) { Write-Host ($tv -join "`n"); Fail "the venv's packages do not import. Delete $venv and run this again." }
$tvl = "$($tv[-1])".Trim()
Write-Host "  torch $tvl"
if ($tvl -notmatch 'True$') { Fail 'PyTorch sees no CUDA GPU. Update the NVIDIA driver; if that does not help, delete the venv folder and run this again.' }

# ------------------------------------------------------------------ Video-Depth-Anything code (Apache-2.0), pinned
Step 'Video-Depth-Anything'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
$vdaMarker = Join-Path $vdaDir 'ffxvr-commit.txt'
$vdaHave = if (Test-Path -LiteralPath $vdaMarker) { [IO.File]::ReadAllText($vdaMarker).Trim() } else { '' }
if ($vdaHave -ne $VdaCommit) {
    $zip = Join-Path $root 'vda-download.zip'
    $tmp = Join-Path $root 'vda-extract'
    Write-Host "  downloading commit $($VdaCommit.Substring(0, 7)) from GitHub ..."
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    try { Invoke-WebRequest -UseBasicParsing -Uri $VdaZipUrl -OutFile $zip -ErrorAction Stop }
    catch { Fail "the download of $VdaZipUrl failed: $($_.Exception.Message)" }
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
    try { Expand-Archive -LiteralPath $zip -DestinationPath $tmp -Force -ErrorAction Stop }
    catch { Fail "could not unpack ${zip}: $($_.Exception.Message)" }
    $inner = @(Get-ChildItem -LiteralPath $tmp -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'video_depth_anything') })
    if ($inner.Count -ne 1) { Fail "the download does not look like Video-Depth-Anything (no video_depth_anything folder in $tmp)." }
    if (Test-Path -LiteralPath $vdaDir) { Remove-Item -LiteralPath $vdaDir -Recurse -Force }
    Move-Item -LiteralPath $inner[0].FullName -Destination $vdaDir
    [IO.File]::WriteAllText($vdaMarker, $VdaCommit, $FfxvrUtf8)
    Remove-Item -LiteralPath $zip -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
}
# The files we import are checked against their SHA-256 at the pinned commit (tools\fmvdepth\vda_model.py).
& $vpy (Join-Path $here 'vda_model.py') verify $vdaDir
if ($LASTEXITCODE -ne 0) { Fail "the Video-Depth-Anything code in $vdaDir is not the pinned commit. Delete that folder and run this again." }

# ------------------------------------------------------------------ the Small weights (Apache-2.0)
Step 'model weights'
New-Item -ItemType Directory -Force -Path $models | Out-Null
$wPath = Join-Path $models $WeightsName
$wRec  = "$wPath.sha256"
if (-not (Test-Path -LiteralPath $wPath)) {
    Write-Host "  downloading $WeightsName (Video-Depth-Anything-Small, Apache-2.0) from Hugging Face ..."
    $part = "$wPath.part"
    try { Invoke-WebRequest -UseBasicParsing -Uri $WeightsUrl -OutFile $part -ErrorAction Stop }
    catch { Remove-Item -LiteralPath $part -Force -ErrorAction SilentlyContinue; Fail "the download of $WeightsUrl failed: $($_.Exception.Message)" }
    $mb = (Get-Item -LiteralPath $part).Length / 1MB
    if ($mb -lt $WeightsMinMB -or $mb -gt $WeightsMaxMB) {
        Remove-Item -LiteralPath $part -Force
        Fail ("the download is {0:N1} MB, not the ~110 MB weights file (an error page?). Try again later." -f $mb)
    }
    $h = (Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($WeightsSha256 -and $h -ne $WeightsSha256) {
        Remove-Item -LiteralPath $part -Force
        Fail "the downloaded weights have SHA-256 $h, expected $WeightsSha256. Not used."
    }
    Move-Item -LiteralPath $part -Destination $wPath -Force
    if (-not $WeightsSha256) {
        [IO.File]::WriteAllText($wRec, "$h  $WeightsName`r`n", $FfxvrUtf8)
        Write-Host "  first download: SHA-256 $h"
        Write-Host "  recorded in $wRec; later runs check the file against it."
        Write-Host '  (TODO for the repository: pin this hash as $WeightsSha256 in tools\convert-fmv-depth.ps1.)'
    }
}
$wHash = (Get-FileHash -LiteralPath $wPath -Algorithm SHA256).Hash.ToLowerInvariant()
$expect = $WeightsSha256
if (-not $expect -and (Test-Path -LiteralPath $wRec)) { $expect = ([IO.File]::ReadAllText($wRec).Trim() -split '\s+')[0].ToLowerInvariant() }
if (-not $expect) {
    [IO.File]::WriteAllText($wRec, "$wHash  $WeightsName`r`n", $FfxvrUtf8)
    Write-Host "  no recorded hash: recorded $wHash"
    $expect = $wHash
}
if ($wHash -ne $expect) { Fail "$wPath has SHA-256 $wHash, expected $expect. Delete it (and $WeightsName.sha256 if you are sure) and run this again." }
Write-Host ('  {0}  {1:N1} MB  sha256 {2}{3}' -f $wPath, ((Get-Item -LiteralPath $wPath).Length / 1MB), $wHash, $(if ($WeightsSha256) { ' (pinned)' } else { ' (recorded; not pinned yet)' }))

if ($SetupOnly) { Write-Host ''; Write-Host 'Set up. Run without -SetupOnly to convert.'; exit 0 }

# ------------------------------------------------------------------ run
Step 'convert'
$drive = (Split-Path -Qualifier $out -ErrorAction SilentlyContinue)
if ($drive) {
    $free = (Get-PSDrive -Name $drive.TrimEnd(':') -ErrorAction SilentlyContinue).Free
    if ($free -and $free -lt 3GB -and -not $OnlyList) { Write-Host ('  WARNING: {0:N1} GB free on {1}; the US list needs about 1.5 GB.' -f ($free / 1GB), $drive) }
}
$a = @('-X', 'utf8', $convert, '--vbf', $vbf, '--out', $out, '--vda-dir', $vdaDir, '--weights', $wPath,
       '--weights-sha256', $wHash, '--ffmpeg', $ffmpegExe, '--ffprobe', $ffprobeExe, '--input-size', "$InputSize",
       '--region', $Region)
if ($OnlyList) { $a += @('--only', $OnlyList) }
if ($All)     { $a += '--all' }
if ($Force)   { $a += '--force' }
if ($List)    { $a += '--list' }
if ($Preview) { $a += '--preview' }
if ($Frame -ge 0) { $a += @('--frame', "$Frame") }
if ($Gray)    { $a += '--gray' }
$env:PYTHONUNBUFFERED = '1'
& $vpy @a
$rc = $LASTEXITCODE
Write-Host ''
if ($rc -eq 3) {
    Write-Host 'Out of GPU memory. Run again with -InputSize 392 (finished videos are kept and skipped).'
} elseif ($rc -eq 130) {
    Write-Host 'Stopped. Run the same command again to carry on (finished videos are skipped).'
} elseif ($rc -ne 0) {
    Write-Host "The converter reported a problem (exit code $rc): see the messages above and $out\convert.log."
} else {
    Write-Host "Done. Depth in $out (private: made from your game copy; never share it)."
    if ($Preview) { Write-Host "Previews in $out\preview\ - PRIVATE: they show the game's videos. Watch them here; never share or upload them." }
}
exit $rc
