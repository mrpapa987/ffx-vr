#!/usr/bin/env python3
"""convert.py - make 8-bit relative depth for every frame of the game's FMVs, on your own PC, from your own copy.

SPDX-License-Identifier: MIT (this script; part of the FFX VR repository). What it writes is derived from the game's
videos: it stays on your PC. Never share, upload or package it (the mod's tools never do).

Usually run through tools\\convert-fmv-depth.ps1, which sets up Python, torch, Video-Depth-Anything and its Small
weights first. Directly:
    python convert.py --vbf <game>/data/FFX_Data.vbf --out <game>/fahrenheit/mods/ffxvr/fmvdepth
                      --vda-dir <VDA checkout> --weights video_depth_anything_vits.pth [options]

Per video, streaming and in constant memory (nothing but the .fvd is written):
    VBF reader -> ffmpeg (pipe, rgb24 at the model's input size) -> Video-Depth-Anything-Small in windows of 32 frames
    (fp16, CUDA) -> each depth map shrunk to 320x180 (240x180 for 4:3) -> per-shot 8-bit normalisation (p2/p98, EMA,
    reset at cuts) -> left predictor -> XPRESS_HUFF -> <name>.fvd.part, renamed to <name>.fvd when complete.
manifest.json in the output folder records each video (status, source size and VBF offset, model, settings, frames,
seconds); a video whose .fvd is complete and matches is skipped, so an interrupted run resumes per video.

Options:
    --region us|jp|asia   which game video list (default us: what an English game plays)
    --all                 every .webm in the archive instead (95 files)
    --only A,B            just these (e.g. --only SIN_us,BTS_us; case and '.webm' optional)
    --input-size 518|392  the model's input height (default 518; 392 is faster and needs less VRAM)
    --force               convert again even if done
    --preview             after converting, render a side-by-side check video of each --only video (preview.py)
    --frame N             with --preview: also a PNG of frame N (video | depth)
    --gray                previews in grey instead of a colour map
    --edge-filter none|snap   the optional light edge filter (default none)
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
from window import windowed_depth  # noqa: E402

CONVERTER_VERSION = '0.1.1'
ALGO_VERSION = 1          # bump when the depth a given input produces changes (forces reconversion)
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
        'out_h': depthproc.OUT_H,
        'precision': 'fp32' if a.fp32 else 'fp16',
        'attention': a.attention,
        'norm': {'alpha': a.ema, 'pcts': [2.0, 98.0]},
        'cut': {'threshold': a.cut_threshold, 'ratio': a.cut_ratio},
        'edge_filter': a.edge_filter,
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
    est = int(round(info['duration'] * info['fps_num'] / info['fps_den'])) if info['duration'] else None
    model.prepare(in_w, in_h, out_w, out_h)
    model.reset_peak()
    cuts = depthproc.CutDetector(sw, sh, a.cut_threshold, a.cut_ratio)
    norm = depthproc.ShotNormalizer(alpha=a.ema)
    log(f'{entry.name}: {sw}x{sh} {info["fps_num"]}/{info["fps_den"]} fps, ~{est or "?"} frames, '
        f'{entry.size / 2**20:.1f} MB -> model {in_w}x{in_h} -> {out_w}x{out_h}')
    side = {}
    t0 = time.perf_counter()
    w = fvd.FvdWriter(part, out_w, out_h, info['fps_num'], info['fps_den'], codec=a.codec_id, predictor=fvd.PRED_LEFT,
                      model_id=fvd.MODEL_VDA_SMALL if model.model_name == 'vda-small' else fvd.MODEL_FAKE,
                      input_w=in_w, input_h=in_h, settings_hash=settings_hash, src_name=entry.name.split('/')[-1].lower(),
                      src_size=entry.size, src_offset=entry.offset, thumb_w=cuts.tw, thumb_h=cuts.th)
    ok = False
    reader = None
    try:
        reader = media.FrameReader(arc, entry, ff['ffmpeg'], in_w, in_h, ff['passthrough'])

        def frames():
            for i, f in enumerate(reader):
                side[i] = cuts(f)
                yield f
        n_cuts = 0
        for idx, depth in windowed_depth(frames(), model):
            if idx != w.count:
                raise ConvertError(f'internal: frame {idx} came out as frame {w.count}')
            cut, thumb = side.pop(idx)
            if a.edge_filter == 'snap':
                depth = depthproc.edge_snap(depth)
            q, p2, p98, lo, hi = norm(depth, cut)
            w.add_frame(q, fvd.FF_CUT if cut else 0, p2, p98, lo, hi, thumb)
            n_cuts += bool(cut)
            if progress:
                el = time.perf_counter() - t0
                frac = entry.size * min(reader.bytes_fed / max(entry.size, 1), 1.0)
                progress.update(entry.name.split('/')[-1], w.count, est, w.count / el if el > 0 else 0.0, frac,
                                model.peak_vram())
        if side:
            raise ConvertError(f'internal: {len(side)} frames read but not written')
        if w.count == 0:
            raise ConvertError(f'{entry.name}: ffmpeg decoded no frames')
        h = w.close()
        ok = True
    finally:
        if reader:
            reader.close()
        if not ok:
            w.abort()
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
          'width': out_w, 'height': out_h, 'input': [in_w, in_h], 'cuts': n_cuts,
          'src': [sw, sh], 'fps': [info['fps_num'], info['fps_den']], 'peak_vram_mb': model.peak_vram() // 2**20,
          'estimated_frames': est}
    if progress:
        progress.update(entry.name.split('/')[-1], h.frame_count, h.frame_count, h.frame_count / max(secs, 1e-6),
                        entry.size, model.peak_vram(), final=True)
    if est and abs(est - h.frame_count) > 2:
        log(f'  note: {h.frame_count} frames decoded, the container\'s duration suggests {est}')
    log(f'  done: {h.frame_count} frames, {n_cuts} cuts, {secs:.0f} s ({h.frame_count / max(secs, 1e-6):.1f} frames/s), '
        f'{st["fvd_bytes"] / 2**20:.1f} MB ({st["fvd_bytes"] / h.frame_count / 1024:.1f} KB a frame), '
        f'VRAM peak {model.peak_vram() / 2**30:.2f} GB')
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
    p.add_argument('--input-size', type=int, default=518, choices=[518, 392])
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
    p.add_argument('--edge-filter', default='none', choices=['none', 'snap'])
    p.add_argument('--codec', default='auto', choices=['auto', 'xpress', 'zlib'])
    p.add_argument('--cut-threshold', type=float, default=28.0)
    p.add_argument('--cut-ratio', type=float, default=3.0)
    p.add_argument('--ema', type=float, default=0.9)
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
    log = Log(None if a.list else os.path.join(out, 'convert.log'))
    if a.preview_note:
        log(f'  note: {a.preview_note}')
    scope = f'--only {a.only}' if a.only else ('--all' if a.all else f'the {a.region} video list')
    total_mb = sum(e.size for _n, e in todo) / 2**20
    log(f'FFX VR FMV depth converter {CONVERTER_VERSION}: {len(todo)} videos ({scope}), {total_mb:,.0f} MB of video')
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
    man = load_manifest(out)
    work = []
    for n, e in todo:
        if not a.force and is_done(out, man['files'].get(out_name(n)), e, shash):
            continue
        work.append((n, e))
    skipped = len(todo) - len(work)
    if skipped:
        log(f'  {skipped} already done with these settings (skipped; --force converts them again)')
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
                if a.input_size > 392:
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
                   width=st['width'], height=st['height'], input=st['input'], cuts=st['cuts'], src=st['src'],
                   fps=st['fps'], peak_vram_mb=st['peak_vram_mb'],
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
