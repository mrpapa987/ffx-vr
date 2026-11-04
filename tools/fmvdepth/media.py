"""media.py - ffmpeg / ffprobe as subprocesses, fed from the VBF through a pipe (no video file is ever written).

SPDX-License-Identifier: MIT (part of the FFX VR repository).
"""
import json
import subprocess
import threading

import numpy as np


class MediaError(Exception):
    pass


def _popen(cmd, **kw):
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    try:
        return subprocess.Popen(cmd, creationflags=flags, **kw)
    except OSError as e:
        raise MediaError(f'cannot run {cmd[0]}: {e}') from None


def _close(f):
    try:
        if f:
            f.close()
    except OSError:
        pass


class _Feeder(threading.Thread):
    """Writes a VBF entry's bytes into a process's stdin, then closes it. A reader that stops early (ffprobe, a
    preview cut short) ends the copy quietly; any other error is kept for the caller."""

    def __init__(self, arc, entry, pipe):
        super().__init__(daemon=True)
        self.arc, self.entry, self.pipe = arc, entry, pipe
        self.error = None
        self.bytes = 0
        self.stop = False

    def run(self):
        try:
            for chunk in self.arc.iter_chunks(self.entry):
                if self.stop:
                    break
                try:
                    self.pipe.write(chunk)
                except (BrokenPipeError, OSError):
                    break
                self.bytes += len(chunk)
        except Exception as e:          # noqa: BLE001 - reported by the caller
            self.error = e
        finally:
            try:
                self.pipe.close()
            except OSError:
                pass


class _Drain(threading.Thread):
    """Collects a pipe (stderr) so the child never blocks on it; keeps the last 64 KiB."""

    def __init__(self, pipe):
        super().__init__(daemon=True)
        self.pipe = pipe
        self.data = b''

    def run(self):
        try:
            for line in iter(lambda: self.pipe.read(4096), b''):
                self.data = (self.data + line)[-65536:]
        except (OSError, ValueError):
            pass

    def text(self):
        return self.data.decode('utf-8', 'replace').strip()


def check_ffmpeg(ffmpeg):
    """The ffmpeg version line, and the option that makes it pass every decoded frame through exactly once
    ('-fps_mode passthrough' on 5.1+, '-vsync passthrough' before)."""
    try:
        out = subprocess.run([ffmpeg, '-hide_banner', '-version'], capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise MediaError(f'cannot run {ffmpeg}: {e}') from None
    ver = out.stdout.decode('utf-8', 'replace').splitlines()[:1]
    for opt in (['-fps_mode', 'passthrough'], ['-vsync', 'passthrough']):
        r = subprocess.run([ffmpeg, '-hide_banner', '-loglevel', 'error', '-f', 'lavfi', '-i',
                            'testsrc=size=32x18:rate=30:duration=0.2'] + opt + ['-f', 'null', '-'],
                           capture_output=True, timeout=60)
        if r.returncode == 0:
            return (ver[0] if ver else '?'), opt
    raise MediaError(f'{ffmpeg} could not run a test decode: {r.stderr.decode("utf-8", "replace").strip()[-400:]}')


def _ratio(s):
    try:
        a, b = s.split('/')
        a, b = int(a), int(b)
        return (a, b) if a > 0 and b > 0 else None
    except (ValueError, AttributeError):
        return None


def probe(arc, entry, ffprobe):
    """{'width', 'height', 'fps_num', 'fps_den', 'duration' (s, or None)} of the video stream, from the start of the
    file through a pipe."""
    p = _popen([ffprobe, '-v', 'error', '-select_streams', 'v:0', '-show_entries',
                'stream=width,height,r_frame_rate,avg_frame_rate:format=duration', '-of', 'json', '-i', 'pipe:0'],
               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    feed, err = _Feeder(arc, entry, p.stdin), _Drain(p.stderr)
    feed.start(); err.start()
    out = p.stdout.read()
    p.wait()
    feed.stop = True
    feed.join(); err.join()
    _close(p.stdout); _close(p.stderr)
    if feed.error:
        raise feed.error
    try:
        j = json.loads(out.decode('utf-8', 'replace') or '{}')
        st = j['streams'][0]
        w, h = int(st['width']), int(st['height'])
    except (ValueError, KeyError, IndexError, TypeError):
        raise MediaError(f'ffprobe could not read {entry.name}: {err.text()[-400:] or "no video stream"}') from None
    fr = _ratio(st.get('r_frame_rate')) or _ratio(st.get('avg_frame_rate')) or (30000, 1001)
    try:
        dur = float(j.get('format', {}).get('duration'))
    except (TypeError, ValueError):
        dur = None
    return {'width': w, 'height': h, 'fps_num': fr[0], 'fps_den': fr[1], 'duration': dur}


class FrameReader:
    """Iterates the video's frames as uint8 H x W x 3 RGB arrays at (w, h), decoded by ffmpeg from the VBF via pipes.
    BT.601 limited range in (the game's VP8 files carry no colour metadata), bicubic scaling, every decoded frame once.
    Use as a context manager (the processes are stopped on exit)."""

    def __init__(self, arc, entry, ffmpeg, w, h, passthrough_opt, extra_vf=''):
        self.w, self.h = w, h
        vf = f'scale={w}:{h}:flags=bicubic:in_color_matrix=bt601:in_range=tv'
        if extra_vf:
            vf = extra_vf + ',' + vf
        cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-nostats', '-i', 'pipe:0', '-map', '0:v:0',
               '-an', '-sn', '-vf', vf] + list(passthrough_opt) + ['-pix_fmt', 'rgb24', '-f', 'rawvideo', 'pipe:1']
        self.p = _popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=1 << 20)
        self.feed, self.err = _Feeder(arc, entry, self.p.stdin), _Drain(self.p.stderr)
        self.feed.start(); self.err.start()
        self.count = 0

    @property
    def bytes_fed(self):
        return self.feed.bytes

    def __iter__(self):
        n = self.w * self.h * 3
        rd = self.p.stdout
        while True:
            buf = rd.read(n)
            if not buf:
                break
            if len(buf) != n:
                raise MediaError(f'ffmpeg ended inside frame {self.count} ({len(buf)} of {n} bytes)')
            self.count += 1
            yield np.frombuffer(buf, dtype=np.uint8).reshape(self.h, self.w, 3)
        rc = self.p.wait()
        self.feed.join(); self.err.join()
        if self.feed.error:
            raise self.feed.error
        if rc != 0:
            raise MediaError(f'ffmpeg failed (exit {rc}): {self.err.text()[-600:]}')

    def close(self):
        self.feed.stop = True
        if self.p.poll() is None:
            try:
                self.p.kill()
            except OSError:
                pass
        try:
            self.p.stdout.close()
        except OSError:
            pass
        self.p.wait()
        self.feed.join(timeout=10)
        self.err.join(timeout=10)
        _close(self.p.stderr)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


class FrameWriter:
    """An ffmpeg encoder fed raw rgb24 frames on stdin (previews). H.264 (libx264) when the build has it, else MPEG-4."""

    def __init__(self, ffmpeg, path, w, h, fps_num, fps_den, encoder=None):
        enc = encoder or pick_encoder(ffmpeg)
        q = ['-c:v', 'libx264', '-preset', 'medium', '-crf', '18', '-pix_fmt', 'yuv420p'] if enc == 'libx264' else \
            ['-c:v', 'mpeg4', '-q:v', '3', '-pix_fmt', 'yuv420p']
        cmd = [ffmpeg, '-hide_banner', '-loglevel', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24',
               '-s', f'{w}x{h}', '-framerate', f'{fps_num}/{fps_den}', '-i', 'pipe:0', '-an'] + q + \
              ['-movflags', '+faststart', path]
        self.p = _popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.err = _Drain(self.p.stderr)
        self.err.start()
        self.encoder = enc

    def write(self, rgb):
        try:
            self.p.stdin.write(np.ascontiguousarray(rgb, dtype=np.uint8).tobytes())
        except (BrokenPipeError, OSError):
            self.p.wait()
            self.err.join()
            raise MediaError(f'the preview encoder stopped: {self.err.text()[-400:]}') from None

    def close(self):
        try:
            self.p.stdin.close()
        except OSError:
            pass
        rc = self.p.wait()
        self.err.join()
        _close(self.p.stderr)
        if rc != 0:
            raise MediaError(f'the preview encoder failed (exit {rc}): {self.err.text()[-400:]}')


_enc_cache = {}


def pick_encoder(ffmpeg):
    if ffmpeg not in _enc_cache:
        try:
            out = subprocess.run([ffmpeg, '-hide_banner', '-encoders'], capture_output=True, timeout=60).stdout
            _enc_cache[ffmpeg] = 'libx264' if b' libx264 ' in out else 'mpeg4'
        except (OSError, subprocess.TimeoutExpired):
            _enc_cache[ffmpeg] = 'mpeg4'
    return _enc_cache[ffmpeg]
