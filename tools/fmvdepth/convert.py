#!/usr/bin/env python3
"""convert.py - make 8-bit relative depth for every frame of the game's FMVs, on your own PC, from your own copy.

SPDX-License-Identifier: MIT (this script; part of the FFX VR repository). What it writes is derived from the game's
videos: it stays on your PC. Never share, upload or package it (the mod's tools never do).

Usually run through tools\\convert-fmv-depth.ps1, which sets up Python, torch, Video-Depth-Anything and its Small
weights first. Directly:
    python convert.py --vbf <game>/data/FFX_Data.vbf --out <game>/fahrenheit/mods/ffxvr/fmvdepth
                      --vda-dir <VDA checkout> --weights video_depth_anything_vits.pth [options]

Per video, streaming (memory: about two shots' worth, see process_video; long shots spill to a temp file):
    VBF reader -> ffmpeg (pipe, rgb24 at the model's input size) -> Video-Depth-Anything-Small in windows of 32 frames
    (fp16, CUDA; restarted at every cut; a flash is not a cut) -> depth edges sharpened at the model's resolution and
    shrunk to 320x180 (240x180 for 4:3) without averaging across them -> one shot buffered -> one depth range per shot segment (robust percentiles of all its frames; dissolves and dips
    through black split a shot, cross-faded) -> 8 bits -> left predictor -> XPRESS_HUFF -> <name>.fvd.part, renamed to
    <name>.fvd when complete.
manifest.json in the output folder records each video (status, source size and VBF offset, model, settings, frames,
seconds); a video whose .fvd is complete and matches is skipped, so an interrupted run resumes per video.

Options:
    --region us|jp|asia   which game video list (default us: what an English game plays)
    --all                 every .webm in the archive instead (95 files)
    --only A,B            just these (e.g. --only SIN_us,BTS_us; case and '.webm' optional)
    --input-size 518|714|392  the model's input height (default 518, what the model was trained at; 714: finer
                          detail, ~2x the model time and VRAM; 392: faster, less VRAM)
    --flip                flip averaging: every window is also run on the mirrored frames, the two averaged (2x
                          the model time; less noise and no left/right bias)
    --force               convert again even if done
    --preview             after converting, render a side-by-side check video of each --only video (preview.py)
    --frame N             with --preview: also a PNG of frame N (video | depth)
    --gray                previews in grey instead of a colour map
    --edge-filter refine|none   depth edges at the model's resolution before the shrink (default refine: a forced
                          foreground/background weighted median, the near side grown 2 px across and 1 px up and
                          down, a shrink that never averages across a step); none: a plain area shrink
    --threads N           CPU threads for the edge path (default up to 8; the output does not depend on it)
    --temporal on|off     the centred, edge-preserving temporal filter within each shot (default on)
    --model vda-small|fake    fake: no torch / GPU, a pipeline check whose output is NOT depth
    --list                show what would be converted and stop
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import depthproc  # noqa: E402
import fvd  # noqa: E402
import media  # noqa: E402
import vbf  # noqa: E402
import vda_model  # noqa: E402
from window import shot_windows  # noqa: E402

CONVERTER_VERSION = '0.2.0'
ALGO_VERSION = 2          # bump when the depth a given input produces changes (forces reconversion)
FLASH_LEN = 4             # a raw cut whose picture comes back within this many frames is a flash, not a cut
BLANK_STD = 2.5           # a frame whose 64x36 luma has a smaller standard deviation is blank (levels)
NORM_PCTS = (0.1, 99.9)   # a shot segment's depth range: these percentiles of all its frames' pixels...
NORM_HEADROOM = 0.02      # ... widened by this share of the range at each end
TEMPORAL = {'radius': 3, 'passes': 2, 'sigma_t': 2.0, 'sigma_n': 8.0 / 255, 'sigma_y': 0.05}   # --temporal on
EDGES = {'tau': 0.05, 'sigma': 0.2, 'grow': (2, 1)}      # --edge-filter refine (depthproc.refine_and_shrink)
MANIFEST = 'manifest.json'
EXIT_OOM = 3
PREVIEW_DEFAULT = 'SIN_us,BTS_us'

PRIVATE_TXT = """This folder holds depth data that FFX VR's converter (tools/fmvdepth/convert.py) made on this PC from the
videos in YOUR copy of FINAL FANTASY X HD Remaster.

It is derived from Square Enix's videos: it is for your own use on this PC. Do not share it, upload it, put it in a mod,
a release or a repository. FFX VR never ships, packages or deletes it: tools/package-release.ps1 refuses it, and
install-mod.ps1 (also -Uninstall -Purge) leaves this folder in place.

Made on: {date}
To remove it: delete this folder ({folder}).
"""


class ConvertError(Exception):
    pass


# ==================================================================== helpers
def real(path):
    """The absolute path with junctions and symbolic links resolved (also for a folder that does not exist yet: its
    nearest existing parent is resolved)."""
    return os.path.realpath(os.path.abspath(path))


def _git_above(p):
    while True:
        if os.path.exists(os.path.join(p, '.git')):
            return p
        up = os.path.dirname(p)
        if up == p:
            return None
        p = up


def in_git_checkout(path):
    """The checkout `path` is in, by its literal (absolute) spelling or with links resolved, or None. Both: a junction
    inside a checkout pointing elsewhere still writes into the working tree; one elsewhere pointing into a checkout
    still writes into the repository."""
    return _git_above(os.path.abspath(path)) or _git_above(real(path))


def _inside(c, p):
    c, p = os.path.normcase(c), os.path.normcase(p)
    return c == p or c.startswith(p.rstrip('\\/') + os.sep)


def is_inside(child, parent):
    """child is parent or below it, comparing the literal and the resolved spellings of both."""
    cs = {os.path.abspath(child), real(child)}
    ps = {os.path.abspath(parent), real(parent)}
    return any(_inside(c, p) for c in cs for p in ps)


def check_out_dir(out, vbf_path):
    g = in_git_checkout(out)
    if g:
        raise ConvertError(f'--out {out} is inside a git checkout ({g}): depth made from the game\'s videos must never '
                           'go near a repository. Use the mod folder (<game>/fahrenheit/mods/ffxvr/fmvdepth).')
    data_dir = os.path.dirname(os.path.abspath(vbf_path))
    if is_inside(out, data_dir):
        raise ConvertError(f'--out {out} is inside the game\'s data folder: never write there.')


def remove_stale_spills(folder, min_age_s=3 * 3600):
    """Delete leftover shot spill files (depthproc.SPILL_PREFIX*) older than min_age_s: a spill file lives only while
    one shot is being converted and is deleted when closed, also on Ctrl+C or a killed process, but not after a power
    cut or a crash of Windows. (The age test leaves alone a file another converter run is using right now.)"""
    now = time.time()
    n = 0
    try:
        names = os.listdir(folder)
    except OSError:
        return 0
    for f in names:
        if f.startswith(depthproc.SPILL_PREFIX):
            p = os.path.join(folder, f)
            try:
                if os.path.isfile(p) and now - os.path.getmtime(p) > min_age_s:
                    os.remove(p)
                    n += 1
            except OSError:
                pass
    return n


def write_private_note(folder):
    os.makedirs(folder, exist_ok=True)
    p = os.path.join(folder, 'PRIVATE-do-not-share.txt')
    if not os.path.exists(p):
        with open(p, 'w', encoding='utf-8', newline='\r\n') as f:
            f.write(PRIVATE_TXT.format(date=datetime.date.today().isoformat(), folder=folder))


RESERVED = {'con', 'prn', 'aux', 'nul'} | {f'com{i}' for i in range(10)} | {f'lpt{i}' for i in range(10)}
NAME_RE = re.compile(r'[A-Za-z0-9_]{1,40}\.webm')         # used with fullmatch (a trailing newline does not pass)


def check_name(file_name):
    """A video name we are willing to turn into a file name: letters, digits, '_' + '.webm', not a Windows device name."""
    if not NAME_RE.fullmatch(file_name) or file_name[:-5].lower() in RESERVED:
        raise ConvertError(f'{file_name!r}: not a name the converter writes a file for')


def out_name(file_name):
    """'OPN_us.webm' -> 'opn_us' (the runtime looks up fmvdepth/<name lower-cased>.fvd)."""
    b = os.path.basename(file_name)
    if b.lower().endswith('.webm'):
        b = b[:-5]
    return b.lower()


def settings_of(a, model, weights_sha):
    s = {
        'algo': ALGO_VERSION,
        'model': model.model_name,
        'weights_sha256': weights_sha or '',
        'input_size': a.input_size,
        'windows': 'per-shot',
        'out_h': depthproc.OUT_H,
        'flip': bool(a.flip),
        'precision': 'fp32' if a.fp32 else 'fp16',
        'attention': a.attention,
        'norm': {'mode': 'segment', 'pcts': list(NORM_PCTS), 'headroom': NORM_HEADROOM, 'transitions': 1},
        'cut': {'threshold': a.cut_threshold, 'ratio': a.cut_ratio, 'flash_len': FLASH_LEN, 'blank_std': BLANK_STD,
                'same_picture': [0.98, 0.1]},
        'edge_filter': a.edge_filter,
        'edges': {'tau': EDGES['tau'], 'sigma': EDGES['sigma'], 'grow': list(EDGES['grow'])} if a.edge_filter == 'refine'
        else 'area',
        'temporal': dict(TEMPORAL, sigma_n=8.0) if a.temporal == 'on' else 'off',
        'codec': fvd.CODEC_NAMES[a.codec_id],
        'predictor': 'left',
    }
    h = int(hashlib.sha256(json.dumps(s, sort_keys=True).encode()).hexdigest()[:16], 16)
    return s, h


def fmt_time(s):
    if s is None or s != s or s < 0:
        return '?'
    s = int(s + 0.5)
    return f'{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}' if s >= 3600 else f'{s // 60}:{s % 60:02d}'


class Log:
    def __init__(self, path=None):
        self.path = path
        self.tty = sys.stdout.isatty()
        self.cr = False

    def __call__(self, msg='', status=False):
        if status and self.tty:
            sys.stdout.write('\r' + msg[:150].ljust(150)[:150])
            sys.stdout.flush()
            self.cr = True
            return
        if self.cr:
            sys.stdout.write('\n')
            self.cr = False
        print(msg, flush=True)
        if self.path and not status:
            try:
                with open(self.path, 'a', encoding='utf-8') as f:
                    f.write(f'{datetime.datetime.now():%Y-%m-%d %H:%M:%S}  {msg}\n')
            except OSError:
                pass


# ==================================================================== manifest
def load_manifest(out):
    p = os.path.join(out, MANIFEST)
    try:
        with open(p, encoding='utf-8') as f:
            m = json.load(f)
        if isinstance(m, dict) and isinstance(m.get('files'), dict):
            return m
    except (OSError, ValueError):
        pass
    return {'format': 1, 'files': {}}


def save_manifest(out, m):
    m['converter_version'] = CONVERTER_VERSION
    m['updated'] = datetime.datetime.now().isoformat(timespec='seconds')
    p = os.path.join(out, MANIFEST)
    tmp = p + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(m, f, indent=1, sort_keys=True)
    os.replace(tmp, p)


def is_done(out, rec, entry, settings_hash):
    """The manifest says done for this exact source and settings, and the .fvd is complete and says the same."""
    if not rec or rec.get('status') != 'done':
        return False
    if rec.get('source_size') != entry.size or rec.get('vbf_offset') != entry.offset:
        return False
    if rec.get('settings_hash') != f'{settings_hash:016x}':
        return False
    h = fvd.read_header(os.path.join(out, out_name(entry.name) + '.fvd'))
    return bool(h and h.flags & fvd.HF_COMPLETE and h.settings_hash == settings_hash and h.src_size == entry.size
                and h.src_offset == entry.offset and h.frame_count == rec.get('frames'))


# ==================================================================== one video
class Progress:
    def __init__(self, log, total_bytes):
        self.log = log
        self.total_bytes = max(total_bytes, 1)
        self.done_bytes = 0
        self.t0 = time.perf_counter()
        self.last = self.t0

    def update(self, name, n, est, fps, frac, vram, final=False):
        now = time.perf_counter()
        if not final and now - self.last < (1.0 if self.log.tty else 30.0):
            return
        self.last = now
        done = self.done_bytes + frac
        el = now - self.t0
        eta_all = el * (self.total_bytes - done) / done if done > 0 else None
        eta_v = (est - n) / fps if est and fps > 0 else None
        msg = (f'  {name}: {n}/{est or "?"} frames  {fps:5.1f} frames/s  ETA {fmt_time(eta_v)}'
               f'  | all {100.0 * done / self.total_bytes:5.1f}%  ETA {fmt_time(eta_all)}  | VRAM peak {vram / 2**30:.2f} GB')
        self.log(msg, status=not final)


class PipeConfig:
    """The per-video depth settings process_video() needs (from the command line, see pipe_config)."""

    def __init__(self, src_w, src_h, out_w, out_h, cut_threshold=28.0, cut_ratio=3.0, flash_len=4, blank_std=2.5,
                 pcts=(0.1, 99.9), headroom=0.02, spill_dir=None, edge_filter='none', temporal=None, edges=None,
                 threads=1, flip=False):
        self.src_w, self.src_h, self.out_w, self.out_h = src_w, src_h, out_w, out_h
        self.flip = bool(flip)            # flip averaging: each window also run mirrored (2x model time)
        self.edge_filter = edge_filter    # 'refine' (refine_and_shrink at the model's resolution) or 'none' (area)
        self.edges = dict(EDGES if edges is None else edges)
        self.temporal = temporal          # None (off) or temporal_bilateral's keyword arguments
        self.threads = max(1, int(threads))
        self.cut_threshold, self.cut_ratio, self.flash_len, self.blank_std = cut_threshold, cut_ratio, flash_len, blank_std
        self.pcts, self.headroom, self.spill_dir = tuple(pcts), headroom, spill_dir


def pipe_config(a, src_w, src_h, out_w, out_h, spill_dir=None):
    return PipeConfig(src_w, src_h, out_w, out_h, a.cut_threshold, a.cut_ratio, FLASH_LEN, BLANK_STD,
                      NORM_PCTS, NORM_HEADROOM, spill_dir, a.edge_filter,
                      dict(TEMPORAL) if a.temporal == 'on' else None, EDGES, a.threads, a.flip)


class DepthAtOutput:
    """window.windowed_depth's model: the depth model's maps (at its own resolution, or already at the output size),
    then in numpy: with flip, the same window run again on the mirrored frames and averaged (depthproc.flip_merge);
    per frame, the edge path (depthproc.refine_and_shrink) or a plain area shrink to the stored size. Padding slots
    (copies of a shot's last frame, never emitted) get the cheap area shrink.

    Overlapped (submit, used by windowed_depth): the model runs on one thread ("gpu": torch releases the GIL while the
    GPU works), each window's edge pass on another that spreads its frames over `threads` pool threads (numpy releases
    the GIL in its array loops). windowed_depth submits window k+1 before it takes window k, so the GPU computes the
    next window while the CPU sharpens this one: the time is max(model, edges), not their sum."""

    def __init__(self, model, c):
        from concurrent.futures import ThreadPoolExecutor
        self.model, self.c = model, c
        self.pool = ThreadPoolExecutor(max_workers=c.threads) if c.threads > 1 else None
        self.gpu = ThreadPoolExecutor(max_workers=1, thread_name_prefix='fvd-model')
        self.cpu = ThreadPoolExecutor(max_workers=1, thread_name_prefix='fvd-edges')
        self.maps_full = 0      # edge-path maps made (for the log)
        self.maps_pad = 0       # padding slots given the area shrink

    def one(self, d):
        c = self.c
        if d.shape == (c.out_h, c.out_w):
            return d
        if c.edge_filter == 'refine':
            e = c.edges
            return depthproc.refine_and_shrink(d, c.out_w, c.out_h, tau=e['tau'], sigma=e['sigma'], grow=e['grow'])
        return depthproc.area_resize(d, c.out_w, c.out_h)

    def _raw(self, frames):
        d = np.asarray(self.model(frames), dtype=np.float32)
        if self.c.flip:
            m = np.asarray(self.model([np.ascontiguousarray(f[:, ::-1]) for f in frames]), dtype=np.float32)
            d = depthproc.flip_merge(d, m[:, :, ::-1])
        return d

    def _finish(self, d, real):
        real = [True] * len(d) if real is None else list(real)
        idx = [i for i, r in enumerate(real) if r]
        out = [None] * len(d)
        maps = list(self.pool.map(self.one, [d[i] for i in idx])) if self.pool else [self.one(d[i]) for i in idx]
        for i, m in zip(idx, maps):
            out[i] = m
        c = self.c
        for i, r in enumerate(real):
            if not r:
                out[i] = d[i] if d[i].shape == (c.out_h, c.out_w) else depthproc.area_resize(d[i], c.out_w, c.out_h)
        self.maps_full += len(idx)
        self.maps_pad += len(d) - len(idx)
        return np.stack(out)

    def __call__(self, frames, real=None):
        return self._finish(self._raw(frames), real)

    def submit(self, frames, real):
        raw = self.gpu.submit(self._raw, list(frames))
        return self.cpu.submit(lambda: self._finish(raw.result(), real))

    def close(self):
        for ex in (self.gpu, self.cpu, self.pool):
            if ex is not None:
                ex.shutdown(wait=True, cancel_futures=True)
        self.pool = None


def _count(stats, k, n=1):
    if stats is not None:
        stats[k] = stats.get(k, 0) + n


def process_video(frames, model, c, stats=None):
    """The depth pipeline of one video, without file or ffmpeg I/O (numpy only; `model` is the only GPU step).

    frames: the video's RGB frames at the model's input size, in order. model: the depth model (its maps at its own
    resolution, or at the output size): DepthAtOutput makes the stored-size maps from them.
    Yields (q, flags, p2, p98, lo, hi, thumb) for every frame in order, one shot late (a shot's range needs all of it).

    Producer thread: tag_frames (cuts with the flash and same-picture tests, blank frames) -> shot_windows (VDA
    restarted at every cut; the model one window ahead of DepthAtOutput's edge pass: forced fg/bg weighted median,
    near side grown 2 px across and 1 px up/down, shrink without averaging across a step) -> ShotBuffer, handed over
    a shot at a time. This generator (the caller's thread) meanwhile finishes the shot before: find_transitions +
    shot_ranges (one range per segment; dissolves cross-faded) -> normalised n -> temporal_bilateral (centred +-3
    frames, 2 passes, never across a cut or a segment start; c.temporal None: off) -> 8 bits. So the next shot's
    model work does not wait for this one's finish.

    Memory for the longest shot (it is all kept until the shot ends): its 320x180 float maps (230 KB a frame; past
    2,400 frames spilled to a temporary file in c.spill_dir), each frame's Tag (a 57.6 KB luma guide, the 9 KB 64x36
    luma, the thumbnail), and at the shot end the transition test's and the range's temporaries (~0.2 MB a frame): a
    20,000-frame shot would need ~3-3.5 GB of RAM and ~4 GB of spill. Two shots can be alive at once (one finishing,
    one being made). The model-resolution maps never accumulate (a window or two: 61 MB at 518, 116 MB at 714, x2
    with flip).
    stats (a dict), if given, collects counts: frames, shots, flash_frames, blank_frames, dissolves, fades, dips,
    segments, flat_frames, clipped (pixels at 0 or 255)."""
    import queue
    import threading
    det = depthproc.CutDetector(c.src_w, c.src_h, c.cut_threshold, c.cut_ratio)
    tags = {}
    dm = DepthAtOutput(model, c)
    shots = queue.Queue(maxsize=1)
    stop = threading.Event()
    END = object()

    def put(x):
        while not stop.is_set():
            try:
                shots.put(x, timeout=0.2)
                return True
            except queue.Full:
                pass
        return False

    def produce():
        try:
            for item in _make_shots(frames, dm, c, det, tags, stop):
                if not put(item):
                    item[1].close()
                    return
            put(END)
        except BaseException as e:  # noqa: BLE001 - handed to the caller's thread
            put(e)

    th = threading.Thread(target=produce, name='fvd-shots', daemon=True)
    th.start()
    try:
        while True:
            x = shots.get()
            if x is END:
                break
            if isinstance(x, BaseException):
                raise x
            start, buf, st, pp = x
            try:
                yield from _finish_shot(c, stats, buf, st, pp)
            finally:
                buf.close()
    finally:
        stop.set()
        while True:                     # free what the producer handed over but nobody will finish
            try:
                x = shots.get_nowait()
            except queue.Empty:
                break
            if isinstance(x, tuple):
                x[1].close()
        th.join()
        dm.close()
    if tags:
        raise ConvertError(f'internal: {len(tags)} frames read but not written')


def _make_shots(frames, model, c, det, tags, stop):
    """The producer: yields (start, ShotBuffer, tags, p2/p98) per shot."""

    def items():
        guide = (c.out_w, c.out_h) if c.temporal and c.temporal.get('sigma_y', 0) > 0 else None
        for i, (f, t) in enumerate(depthproc.tag_frames(frames, det, c.flash_len, c.blank_std, guide_size=guide)):
            if stop.is_set():
                return
            tags[i] = t
            yield i, f
    for start, shot in shot_windows(items(), model, is_start=lambda x: tags[x[0]].cut, frame_of=lambda x: x[1]):
        buf = depthproc.ShotBuffer(spill_dir=c.spill_dir)
        try:
            pp = []
            for i, d in shot:
                if i != start + len(buf):
                    raise ConvertError(f'internal: frame {i} came out as frame {start + len(buf)}')
                buf.append(d)
                pp.append(np.percentile(buf[len(buf) - 1], (2.0, 98.0)))
                if stop.is_set():
                    raise KeyboardInterrupt
            st = [tags.pop(start + k) for k in range(len(buf))]
        except BaseException:
            buf.close()
            raise
        yield start, buf, st, pp


def _finish_shot(c, stats, buf, st, pp):
    """One shot's ranges, temporal filter and 8 bits: yields process_video's tuples for its frames."""
    lo, hi, info = depthproc.shot_ranges(buf, st, c.pcts, c.headroom)
    _count(stats, 'shots')
    _count(stats, 'segments', len(info['starts']))
    for tr in info['transitions']:
        _count(stats, tr['kind'] + 's')
    seg_of = np.searchsorted(np.asarray(info['starts']), np.arange(len(buf)), side='right') - 1

    def normalised():
        for k in range(len(buf)):
            rng = max(hi[k] - lo[k], depthproc.range_eps(hi[k]))
            yield ((buf[k] - np.float32(lo[k])) * np.float32(1.0 / rng), st[k].guide,
                   not (st[k].blank or st[k].flash), seg_of[k])
    ns = (depthproc.temporal_bilateral(normalised(), **c.temporal) if c.temporal else
          (x[0] for x in normalised()))
    for k, n in enumerate(ns):
        t = st[k]
        if info['flat'][k]:       # an all-blank segment: flat (lo = hi), written as a constant 0
            q = np.zeros((c.out_h, c.out_w), np.uint8)
        else:
            q = np.clip(n * np.float32(255.0) + np.float32(0.5), 0, 255).astype(np.uint8)
        fl = fvd.FF_CUT if k == 0 else 0
        if info['blend'][k]:
            fl |= fvd.FF_BLEND
        if t.blank or t.flash:
            fl |= fvd.FF_FLASH
            _count(stats, 'blank_frames' if t.blank else 'flash_frames')
        if info['flat'][k]:
            _count(stats, 'flat_frames')
        _count(stats, 'frames')
        _count(stats, 'clipped', int(np.count_nonzero((q == 0) | (q == 255))))
        yield q, fl, float(pp[k][0]), float(pp[k][1]), float(lo[k]), float(hi[k]), t.thumb


def convert_one(arc, entry, out_dir, model, a, settings_hash, ff, log, progress=None):
    """Convert one video into <out_dir>/<name>.fvd. Returns a stats dict. Leaves no .part behind on failure."""
    name = out_name(entry.name)
    final = os.path.join(out_dir, name + '.fvd')
    part = final + '.part'
    if os.path.exists(part):
        os.remove(part)
    info = media.probe(arc, entry, ff['ffprobe'])
    sw, sh = info['width'], info['height']
    in_w, in_h = depthproc.model_input_size(sw, sh, a.input_size)
    out_w, out_h = depthproc.output_size(sw, sh)
    tw, th = depthproc.thumb_size(sw, sh)
    est = int(round(info['duration'] * info['fps_num'] / info['fps_den'])) if info['duration'] else None
    model.prepare(in_w, in_h, out_w, out_h, full=a.edge_filter == 'refine')
    model.reset_peak()
    cfg = pipe_config(a, sw, sh, out_w, out_h, spill_dir=out_dir)
    log(f'{entry.name}: {sw}x{sh} {info["fps_num"]}/{info["fps_den"]} fps, ~{est or "?"} frames, '
        f'{entry.size / 2**20:.1f} MB -> model {in_w}x{in_h} -> {out_w}x{out_h}')
    t0 = time.perf_counter()
    w = fvd.FvdWriter(part, out_w, out_h, info['fps_num'], info['fps_den'], codec=a.codec_id, predictor=fvd.PRED_LEFT,
                      model_id=fvd.MODEL_VDA_SMALL if model.model_name == 'vda-small' else fvd.MODEL_FAKE,
                      input_w=in_w, input_h=in_h, settings_hash=settings_hash, src_name=entry.name.split('/')[-1].lower(),
                      src_size=entry.size, src_offset=entry.offset, thumb_w=tw, thumb_h=th,
                      segment_range=ALGO_VERSION >= 2)
    ok = False
    reader = None
    vs = {}
    try:
        reader = media.FrameReader(arc, entry, ff['ffmpeg'], in_w, in_h, ff['passthrough'])
        for q, flags, p2, p98, lo, hi, thumb in process_video(reader, model, cfg, vs):
            w.add_frame(q, flags, p2, p98, lo, hi, thumb)
            if progress:
                el = time.perf_counter() - t0
                frac = entry.size * min(reader.bytes_fed / max(entry.size, 1), 1.0)
                progress.update(entry.name.split('/')[-1], w.count, est, w.count / el if el > 0 else 0.0, frac,
                                model.peak_vram())
        if w.count == 0:
            raise ConvertError(f'{entry.name}: ffmpeg decoded no frames')
        h = w.close()
        ok = True
    finally:
        if reader:
            reader.close()
        if not ok:
            w.abort()
    n_shots = vs.get('shots', 0)
    # check the finished file before it counts
    try:
        with fvd.FvdReader(part) as r:
            if len(r) != h.frame_count:
                raise ConvertError(f'{part}: re-read gives {len(r)} frames')
            for i in sorted({0, len(r) // 2, len(r) - 1}):
                r.frame(i)
        os.replace(part, final)
    except BaseException:
        try:
            os.remove(part)
        except OSError:
            pass
        raise
    secs = time.perf_counter() - t0
    st = {'frames': h.frame_count, 'seconds': round(secs, 1), 'fvd_bytes': os.path.getsize(final),
          'width': out_w, 'height': out_h, 'input': [in_w, in_h], 'shots': n_shots,
          'src': [sw, sh], 'fps': [info['fps_num'], info['fps_den']], 'peak_vram_mb': model.peak_vram() // 2**20,
          'estimated_frames': est}
    if progress:
        progress.update(entry.name.split('/')[-1], h.frame_count, h.frame_count, h.frame_count / max(secs, 1e-6),
                        entry.size, model.peak_vram(), final=True)
    if est and abs(est - h.frame_count) > 2:
        log(f'  note: {h.frame_count} frames decoded, the container\'s duration suggests {est}')
    for k in ('segments', 'dissolves', 'fades', 'dips', 'flash_frames', 'blank_frames', 'flat_frames'):
        st[k] = vs.get(k, 0)
    st['clipped_pct'] = round(100.0 * vs.get('clipped', 0) / max(h.frame_count * out_w * out_h, 1), 3)
    log(f'  done: {h.frame_count} frames, {n_shots} shots ({max(n_shots - 1, 0)} cuts), {secs:.0f} s ({h.frame_count / max(secs, 1e-6):.1f} frames/s), '
        f'{st["fvd_bytes"] / 2**20:.1f} MB ({st["fvd_bytes"] / h.frame_count / 1024:.1f} KB a frame), '
        f'VRAM peak {model.peak_vram() / 2**30:.2f} GB')
    log(f'    segments {st["segments"]} (dissolves {st["dissolves"]}, dips {st["dips"]}), fades '
        f'{st["fades"]}, flash frames {st["flash_frames"]}, blank frames {st["blank_frames"]}, flat frames '
        f'{st["flat_frames"]}, pixels clipped {st["clipped_pct"]:.3f} %')
    return st


# ==================================================================== selection
def select_videos(arc, a):
    """[(file name, entry)] in conversion order. A name that fails check_name, or a second video that would write the
    same .fvd, is skipped with a warning (the rest of the run goes on)."""
    out, seen = [], {}
    for n, e in _select(arc, a):
        try:
            check_name(n)
        except ConvertError as ex:
            print(f'  WARNING: skipped {ex}')
            continue
        k = out_name(n)
        if k in seen:
            if seen[k] != n:
                print(f'  WARNING: skipped {n}: {seen[k]} already writes {k}.fvd')
            continue
        seen[k] = n
        out.append((n, e))
    return out


def _select(arc, a):
    if a.only:
        allv = {k[len(vbf.VIDEO_DIR):].lower(): k for k in vbf.all_videos(arc)}
        out, seen = [], set()
        for raw in re.split(r'[,;\s]+', a.only):
            n = raw.strip().lower()
            if not n:
                continue
            if not n.endswith('.webm'):
                n += '.webm'
            if n not in allv:
                raise ConvertError(f'--only: no video {raw.strip()!r} in the archive (python vbf.py <vbf> videolist)')
            if n not in seen:
                seen.add(n)
                e = arc.get(allv[n])
                out.append((e.name.split('/')[-1], e))
        return out
    if a.all:
        return [(k.split('/')[-1], arc.get(k)) for k in vbf.all_videos(arc)]
    out, seen = [], set()
    for _vid, n in vbf.read_video_list(arc, a.region):
        if n.lower() in seen:
            continue
        seen.add(n.lower())
        e = arc.entries.get((vbf.VIDEO_DIR + n).lower())
        if e is None:
            print(f'  (the {a.region} list names {n}, which is not in the archive: skipped)')
            continue
        out.append((n, e))
    return out


# ==================================================================== main
def parse_args(argv):
    p = argparse.ArgumentParser(description='FFX VR: FMV depth converter (see the top of convert.py)')
    p.add_argument('--vbf', required=True, help='<game>/data/FFX_Data.vbf')
    p.add_argument('--out', required=True, help='<game>/fahrenheit/mods/ffxvr/fmvdepth')
    p.add_argument('--region', default='us', choices=['us', 'jp', 'asia'])
    p.add_argument('--all', action='store_true')
    p.add_argument('--only', default='')
    p.add_argument('--input-size', type=int, default=518, choices=[518, 392, 714])
    p.add_argument('--flip', action='store_true', help='flip averaging: every window also mirrored (2x model time)')
    p.add_argument('--force', action='store_true')
    p.add_argument('--preview', action='store_true')
    p.add_argument('--frame', type=int, default=-1)
    p.add_argument('--gray', action='store_true')
    p.add_argument('--list', action='store_true')
    p.add_argument('--model', default='vda-small', choices=['vda-small', 'fake'])
    p.add_argument('--vda-dir', default='')
    p.add_argument('--weights', default='')
    p.add_argument('--weights-sha256', default='', help='recorded by the launcher; computed here if not given')
    p.add_argument('--fp32', action='store_true', help='full precision (slower, more VRAM)')
    p.add_argument('--attention', default='sdpa', choices=['sdpa', 'plain'])
    p.add_argument('--edge-filter', default='refine', choices=['refine', 'none'])
    p.add_argument('--threads', type=int, default=min(8, os.cpu_count() or 1),
                   help='CPU threads for the edge path (default: up to 8; the output does not depend on it)')
    p.add_argument('--temporal', default='on', choices=['on', 'off'], help='the centred temporal filter (default on)')
    p.add_argument('--codec', default='auto', choices=['auto', 'xpress', 'zlib'])
    p.add_argument('--cut-threshold', type=float, default=28.0)
    p.add_argument('--cut-ratio', type=float, default=3.0)
    p.add_argument('--ffmpeg', default='ffmpeg')
    p.add_argument('--ffprobe', default='')
    p.add_argument('--check-out', action='store_true', help='only check that --out is an allowed place, write nothing')
    a = p.parse_args(argv)
    if a.all and a.only:
        p.error('--all and --only do not go together')
    if a.frame >= 0 and not a.preview:
        p.error('--frame goes with --preview')
    if not a.ffprobe:
        d, b = os.path.split(a.ffmpeg)
        a.ffprobe = os.path.join(d, b.lower().replace('ffmpeg', 'ffprobe')) if d else 'ffprobe'
    a.codec_id = {'auto': fvd.default_codec(), 'xpress': fvd.CODEC_XPRESS_HUFF, 'zlib': fvd.CODEC_ZLIB}[a.codec]
    a.preview_note = ''
    if a.preview and not a.only and not a.all:
        a.only = PREVIEW_DEFAULT
        a.preview_note = (f'--preview without --only: converting (if needed) and previewing {PREVIEW_DEFAULT} only'
                          + (f' (--region {a.region} is not used)' if a.region != 'us' else ''))
    elif a.preview and a.all:
        a.preview_note = f'--preview with --all: converting every video, then previewing {PREVIEW_DEFAULT} only'
    a.preview_names = a.only or PREVIEW_DEFAULT
    return a


def make_model(a, log):
    if a.model == 'fake':
        log('model: FAKE (pipeline check: the output is not depth; do not use it in the game)')
        return vda_model.FakeModel(), ''
    if not a.vda_dir or not a.weights:
        raise ConvertError('--vda-dir and --weights are needed (convert-fmv-depth.ps1 passes them)')
    bad = vda_model.verify_vda(a.vda_dir)
    if bad:
        raise ConvertError('the Video-Depth-Anything checkout is not the pinned commit:\n    ' + '\n    '.join(bad[:6]) +
                           '\n  Delete that folder; convert-fmv-depth.ps1 downloads it again.')
    sha = a.weights_sha256.lower() or vda_model.sha256_file(a.weights)
    m = vda_model.VdaSmall(a.vda_dir, a.weights, fp32=a.fp32, attention=a.attention)
    log(f'model: Video-Depth-Anything-Small ({os.path.basename(a.weights)} sha256 {sha[:16]}...), {m.describe()}')
    return m, sha


def main(argv=None):
    a = parse_args(sys.argv[1:] if argv is None else argv)
    out = os.path.abspath(a.out)
    try:
        check_out_dir(out, a.vbf)
    except ConvertError as e:
        print(f'ERROR: {e}')
        return 1
    if a.check_out:
        print(f'--out {out}: allowed')
        return 0
    if a.codec_id == fvd.CODEC_XPRESS_HUFF and not fvd.xpress_available():
        print('ERROR: XPRESS_HUFF (Windows cabinet.dll) is not available here; the game\'s reader needs it. '
              '(--codec zlib is for tests only.)')
        return 1
    try:
        arc = vbf.VbfArchive(a.vbf)
        todo = select_videos(arc, a)
        pv = select_videos(arc, argparse.Namespace(only=a.preview_names, all=False, region=a.region)) if a.preview else []
    except (vbf.VbfError, ConvertError, OSError) as e:
        print(f'ERROR: {e}')
        return 1
    if not todo:
        print('Nothing selected.')
        return 1
    if not a.list:                      # --list writes nothing
        os.makedirs(out, exist_ok=True)
        write_private_note(out)
        remove_stale_spills(out)
    log = Log(None if a.list else os.path.join(out, 'convert.log'))
    if a.preview_note:
        log(f'  note: {a.preview_note}')
    scope = f'--only {a.only}' if a.only else ('--all' if a.all else f'the {a.region} video list')
    total_mb = sum(e.size for _n, e in todo) / 2**20
    log(f'FFX VR FMV depth converter {CONVERTER_VERSION} (algo {ALGO_VERSION}): {len(todo)} videos ({scope}), '
        f'{total_mb:,.0f} MB of video')
    log(f'  output: {out}  (private: made from your game copy, never share it)')
    if a.list:
        for n, e in todo:
            print(f'  {n:<24} {e.size / 2**20:8.1f} MB')
        return 0

    try:
        ver, passthrough = media.check_ffmpeg(a.ffmpeg)
        log(f'  {ver}')
        model, wsha = make_model(a, log)
    except (media.MediaError, ConvertError) as e:
        print(f'ERROR: {e}')
        return 1
    except Exception as e:  # noqa: BLE001 - torch / the weights: a DLL that does not load, a bad download, no GPU
        print(f'ERROR: could not load the model: {type(e).__name__}: {e}')
        return 1
    ff = {'ffmpeg': a.ffmpeg, 'ffprobe': a.ffprobe, 'passthrough': passthrough}
    settings, shash = settings_of(a, model, wsha)
    log(f'  settings fingerprint {shash:016x}: input {a.input_size}, edges {a.edge_filter}, temporal {a.temporal}, '
        f'flip {"on" if a.flip else "off"}, cut-aware windows, one depth range per shot segment (a video converted '
        f'with other settings or another algo is converted again)')
    man = load_manifest(out)
    work = []
    changed = 0
    for n, e in todo:
        rec = man['files'].get(out_name(n))
        if not a.force and is_done(out, rec, e, shash):
            continue
        if rec and rec.get('status') == 'done' and rec.get('settings_hash') != f'{shash:016x}':
            changed += 1
        work.append((n, e))
    skipped = len(todo) - len(work)
    if skipped:
        log(f'  {skipped} already done with these settings (skipped; --force converts them again)')
    if changed:
        log(f'  {changed} converted before with other settings or an older converter: converting them again')
    prog = Progress(log, sum(e.size for _n, e in work))
    done = failed = 0
    frames_total, peak = 0, 0
    t_all = time.perf_counter()
    rc = 0
    for n, e in work:
        key = out_name(n)
        rec = {'status': 'running', 'source': n, 'vbf_path': e.name, 'source_size': e.size, 'vbf_offset': e.offset,
               'model': model.model_name, 'weights_sha256': wsha, 'settings': settings,
               'settings_hash': f'{shash:016x}', 'converter_version': CONVERTER_VERSION,
               'started': datetime.datetime.now().isoformat(timespec='seconds')}
        man['files'][key] = rec
        save_manifest(out, man)
        try:
            st = convert_one(arc, e, out, model, a, shash, ff, log, prog)
        except KeyboardInterrupt:
            rec.update(status='interrupted')
            save_manifest(out, man)
            log('Stopped (Ctrl+C). Finished videos are kept; run again to carry on.')
            return 130
        except Exception as ex:  # noqa: BLE001 - every failure is recorded, OOM is explained
            if vda_model.is_oom(ex):
                rec.update(status='failed', error='out of GPU memory')
                save_manifest(out, man)
                log('')
                log(f'ERROR: the GPU ran out of memory (VRAM) at --input-size {a.input_size} on {n}.')
                if a.input_size > 518:
                    log('  Run again with --input-size 518 (convert-fmv-depth.ps1 -InputSize 518, the default).')
                elif a.input_size > 392:
                    log('  Run again with --input-size 392 (convert-fmv-depth.ps1 -InputSize 392): faster, less VRAM.')
                log('  Close other programs that use the GPU (the game, the VR streamer, browsers) first.')
                return EXIT_OOM
            rec.update(status='failed', error=f'{type(ex).__name__}: {ex}'[:500])
            save_manifest(out, man)
            log(f'ERROR: {n}: {type(ex).__name__}: {ex}')
            failed += 1
            rc = 1
            prog.done_bytes += e.size
            continue
        rec.update(status='done', frames=st['frames'], seconds=st['seconds'], fvd_bytes=st['fvd_bytes'],
                   width=st['width'], height=st['height'], input=st['input'], shots=st['shots'], src=st['src'],
                   fps=st['fps'], peak_vram_mb=st['peak_vram_mb'],
                   transitions={k: st[k] for k in ('segments', 'dissolves', 'fades', 'dips', 'flash_frames',
                                                   'blank_frames', 'flat_frames', 'clipped_pct')},
                   finished=datetime.datetime.now().isoformat(timespec='seconds'))
        save_manifest(out, man)
        prog.done_bytes += e.size
        done += 1
        frames_total += st['frames']
        peak = max(peak, st['peak_vram_mb'])
    el = time.perf_counter() - t_all
    disk = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out) if f.endswith('.fvd'))
    log('')
    log(f'Converted {done}, skipped {skipped}, failed {failed}: {frames_total:,} frames in {fmt_time(el)}'
        f'{f" ({frames_total / el:.1f} frames/s)" if el > 0 and frames_total else ""}; peak VRAM {peak / 1024:.2f} GB; '
        f'{disk / 2**30:.2f} GB of .fvd in {out}')
    if a.preview:
        import preview
        pdir = os.path.join(out, 'preview')
        for n, e in pv:
            f = os.path.join(out, out_name(n) + '.fvd')
            if not os.path.exists(f):
                log(f'  no preview of {n}: it has no .fvd')
                continue
            try:
                preview.render(arc, e, f, pdir, a.ffmpeg, passthrough, gray=a.gray, log=log)
                if a.frame >= 0:
                    preview.frame_png(arc, e, f, a.frame, pdir, a.ffmpeg, passthrough, gray=a.gray, log=log)
            except KeyboardInterrupt:
                log('Stopped (Ctrl+C) during the previews. The depth files are complete.')
                return 130
            except (media.MediaError, fvd.FvdError, OSError, ValueError) as ex:
                log(f'ERROR: preview of {n}: {ex}')
                rc = 1
    return rc


if __name__ == '__main__':
    sys.exit(main())
