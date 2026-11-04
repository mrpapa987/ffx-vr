#!/usr/bin/env python3
"""preview.py - the FMV depth quality check (stage S0): the video next to its depth, as an MP4 or one frame as a PNG.

SPDX-License-Identifier: MIT (this script; part of the FFX VR repository). A preview shows the game's video: it is
derived from your game copy, PRIVATE, for your eyes only. Never share or upload it. It is written into the depth
folder's preview\\ subfolder (with PRIVATE-do-not-share.txt), never into a repository.

Needs numpy and ffmpeg only (no torch): it reads finished .fvd files.
    python preview.py --vbf <game>/data/FFX_Data.vbf --dir <game>/fahrenheit/mods/ffxvr/fmvdepth SIN_us BTS_us
        [--frame N]   also <name>_frame<N>.png (video frame N | depth frame N)
        [--gray]      grey depth instead of the colour map (near = bright)
        [--ffmpeg <ffmpeg.exe>]
Usually: convert-fmv-depth.ps1 -Preview (converts SIN_us and BTS_us first if needed).
"""
import argparse
import os
import struct
import sys
import zlib

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

import depthproc  # noqa: E402
import fvd  # noqa: E402
import media  # noqa: E402
import vbf  # noqa: E402

PREVIEW_H = 360
GAP = 4

PRIVATE_TXT = """These previews show the videos of YOUR copy of FINAL FANTASY X HD Remaster next to the depth FFX VR's converter
made from them (tools/fmvdepth/preview.py), on this PC.

They are derived from Square Enix's videos: for your own quality check only. Do not share them, upload them, post them,
put them in a bug report, a mod, a release or a repository.

To remove them: delete this folder.
"""

WARNING = ('Previews are PRIVATE: they show the game\'s own videos. Look at them on this PC; never share, upload or post '
           'them.')

# near (255) bright, far (0) dark: anchors in the style of the "inferno" map
_ANCHORS = np.array([[0, 0, 4], [31, 12, 72], [85, 15, 109], [136, 34, 106], [186, 54, 85], [227, 89, 51],
                     [249, 140, 10], [249, 201, 50], [252, 255, 164]], np.float32)


def colormap_lut():
    x = np.linspace(0, len(_ANCHORS) - 1, 256)
    lut = np.empty((256, 3), np.float32)
    for c in range(3):
        lut[:, c] = np.interp(x, np.arange(len(_ANCHORS)), _ANCHORS[:, c])
    return np.clip(lut + 0.5, 0, 255).astype(np.uint8)


LUT = colormap_lut()


def depth_rgb(q, w, h, gray=False):
    big = np.clip(depthproc.resize_bilinear(q, w, h) + 0.5, 0, 255).astype(np.uint8)
    if gray:
        return np.repeat(big[:, :, None], 3, axis=2)
    return LUT[big]


def side_by_side(rgb, q, gray=False):
    h, w = rgb.shape[:2]
    out = np.zeros((h, 2 * w + GAP, 3), np.uint8)
    out[:, :w] = rgb
    out[:, w + GAP:] = depth_rgb(q, w, h, gray)
    return out


def write_png(path, rgb):
    rgb = np.ascontiguousarray(rgb, dtype=np.uint8)
    h, w = rgb.shape[:2]
    raw = b''.join(b'\0' + rgb[y].tobytes() for y in range(h))

    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def _prepare(pdir):
    os.makedirs(pdir, exist_ok=True)
    p = os.path.join(pdir, 'PRIVATE-do-not-share.txt')
    if not os.path.exists(p):
        with open(p, 'w', encoding='utf-8', newline='\r\n') as f:
            f.write(PRIVATE_TXT)


def _size(r):
    h = PREVIEW_H
    w = int(round(r.h.width * h / r.h.height / 2.0)) * 2
    return w, h


def render(arc, entry, fvd_path, pdir, ffmpeg, passthrough, gray=False, log=print):
    """<pdir>/<name>_preview.mp4: video | depth, the whole video, at the video's frame rate."""
    _prepare(pdir)
    name = os.path.splitext(os.path.basename(fvd_path))[0]
    out = os.path.join(pdir, f'{name}_preview.mp4')
    with fvd.FvdReader(fvd_path) as r:
        w, h = _size(r)
        enc = media.FrameWriter(ffmpeg, out + '.part.mp4', 2 * w + GAP, h, r.h.fps_num, r.h.fps_den)
        n = 0
        try:
            with media.FrameReader(arc, entry, ffmpeg, w, h, passthrough) as vid:
                for i, rgb in enumerate(vid):
                    if i >= len(r):
                        break
                    enc.write(side_by_side(rgb, r.frame(i), gray))
                    n += 1
            enc.close()
        except BaseException:
            try:
                enc.close()
            except media.MediaError:
                pass
            if os.path.exists(out + '.part.mp4'):
                os.remove(out + '.part.mp4')
            raise
        os.replace(out + '.part.mp4', out)
        if n != len(r):
            log(f'  note: the video gave {n} frames, the .fvd has {len(r)}')
    log(f'preview: {out}  ({n} frames, {enc.encoder})')
    log('  ' + WARNING)
    return out


def frame_png(arc, entry, fvd_path, n, pdir, ffmpeg, passthrough, gray=False, log=print):
    """<pdir>/<name>_frame<N>.png: video frame n | depth frame n."""
    _prepare(pdir)
    name = os.path.splitext(os.path.basename(fvd_path))[0]
    with fvd.FvdReader(fvd_path) as r:
        if not 0 <= n < len(r):
            raise ValueError(f'--frame {n}: {name} has frames 0..{len(r) - 1}')
        w, h = _size(r)
        q = r.frame(n)
        e = r.entry(n)
    rgb = None
    with media.FrameReader(arc, entry, ffmpeg, w, h, passthrough, extra_vf=f'select=eq(n\\,{n})') as vid:
        for f in vid:
            rgb = f
            break
    if rgb is None:
        raise ValueError(f'ffmpeg gave no frame {n} for {entry.name}')
    out = os.path.join(pdir, f'{name}_frame{n}.png')
    write_png(out, side_by_side(rgb, q, gray))
    log(f'preview: {out}  (p2 {e.p2:.4g}, p98 {e.p98:.4g}{", cut" if e.flags & fvd.FF_CUT else ""})')
    log('  ' + WARNING)
    return out


def main(argv=None):
    p = argparse.ArgumentParser(description='FMV depth preview (private: shows the game\'s videos)')
    p.add_argument('--vbf', required=True)
    p.add_argument('--dir', required=True, help='the fmvdepth folder with the .fvd files')
    p.add_argument('names', nargs='+')
    p.add_argument('--frame', type=int, default=-1)
    p.add_argument('--gray', action='store_true')
    p.add_argument('--ffmpeg', default='ffmpeg')
    a = p.parse_args(argv)
    sys.path.insert(0, HERE)
    import convert
    if convert.in_git_checkout(a.dir):
        print(f'ERROR: {a.dir} is inside a git checkout: previews show the game\'s videos and never go there.')
        return 1
    arc = vbf.VbfArchive(a.vbf)
    _ver, passthrough = media.check_ffmpeg(a.ffmpeg)
    pdir = os.path.join(os.path.abspath(a.dir), 'preview')
    rc = 0
    for raw in a.names:
        n = raw.lower()[:-5] if raw.lower().endswith('.webm') else raw.lower()
        f = os.path.join(a.dir, n + '.fvd')
        if not os.path.exists(f):
            print(f'{n}: no {f} (convert it first)')
            rc = 1
            continue
        hd = fvd.read_header(f)
        if not hd or not hd.flags & fvd.HF_COMPLETE:
            print(f'{n}: {f} is not a complete .fvd')
            rc = 1
            continue
        e = arc.get(vbf.VIDEO_DIR + hd.src_name)
        if a.frame >= 0:
            frame_png(arc, e, f, a.frame, pdir, a.ffmpeg, passthrough, a.gray)
        else:
            render(arc, e, f, pdir, a.ffmpeg, passthrough, a.gray)
    return rc


if __name__ == '__main__':
    sys.exit(main())
