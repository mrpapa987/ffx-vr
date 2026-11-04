#!/usr/bin/env python3
"""fvd.py - the .fvd FMV depth file: writer, reader and codecs (numpy only; no torch).

SPDX-License-Identifier: MIT (part of the FFX VR repository). The format is specified in FORMAT.md next to this file;
keep the two in step (and bump VERSION for any change a reader would notice).

Usage (checks):
    python fvd.py <file.fvd>            header, index summary, a decode of every frame
"""
import ctypes
import os
import struct
import sys
import zlib

import numpy as np

MAGIC = b'FVD1'
VERSION = 1
HEADER_SIZE = 128
INDEX_ENTRY = 32
NAME_LEN = 48

# codec ids (header byte 24)
CODEC_STORED = 0
CODEC_ZLIB = 1          # zlib stream (RFC 1950), level 6: the portable fallback (tests, non-Windows)
CODEC_XPRESS_HUFF = 2   # Windows Compression API, COMPRESS_ALGORITHM_XPRESS_HUFF, buffer mode (no COMPRESS_RAW)
CODEC_NAMES = {CODEC_STORED: 'stored', CODEC_ZLIB: 'zlib', CODEC_XPRESS_HUFF: 'xpress_huff'}

# predictor ids (header byte 25)
PRED_NONE = 0
PRED_LEFT = 1

# model ids (header byte 26)
MODEL_FAKE = 0          # tests / pipeline checks: not depth
MODEL_VDA_SMALL = 1     # Video-Depth-Anything-Small (vits), Apache-2.0 weights
MODEL_NAMES = {MODEL_FAKE: 'fake', MODEL_VDA_SMALL: 'vda-small'}

# header flags (byte 27)
HF_COMPLETE = 0x01      # set only when the index and header were written at the end
HF_THUMBS = 0x02        # a luma thumbnail per frame follows the index

# frame flags (index entry u16)
FF_CUT = 0x0001         # first frame of a shot (the converter's cut detector; frame 0 always)
FF_STORED = 0x0002      # payload is stored (not compressed): compression did not make it smaller

_HDR = struct.Struct('<4sHHHHIII BBBB HH Q Q Q HH I Q Q 48s')
_IDX = struct.Struct('<QIHHffff')
assert _HDR.size == HEADER_SIZE and _IDX.size == INDEX_ENTRY


class FvdError(Exception):
    pass


# ==================================================================== predictor
def left_predict(q):
    """uint8 HxW -> residual bytes: r[0,0] = v[0,0]; r[y,0] = v[y,0] - v[y-1,0]; r[y,x] = v[y,x] - v[y,x-1] (mod 256)."""
    q = np.ascontiguousarray(q, dtype=np.uint8)
    r = np.empty_like(q)
    r[:, 1:] = q[:, 1:] - q[:, :-1]            # uint8 arithmetic wraps mod 256
    r[1:, 0] = q[1:, 0] - q[:-1, 0]
    r[0, 0] = q[0, 0]
    return r.tobytes()


def left_unpredict(b, w, h):
    r = np.frombuffer(b, dtype=np.uint8, count=w * h).reshape(h, w).astype(np.uint32)
    r[:, 0] = np.cumsum(r[:, 0]) & 255
    return (np.cumsum(r, axis=1) & 255).astype(np.uint8)


# ==================================================================== codecs
COMPRESS_ALGORITHM_XPRESS_HUFF = 4
ERROR_INSUFFICIENT_BUFFER = 122


class _Xpress:
    """XPRESS_HUFF through cabinet.dll (Windows 8+), ctypes. One compressor and one decompressor, reused."""

    def __init__(self):
        if os.name != 'nt':
            raise FvdError('XPRESS_HUFF needs Windows (cabinet.dll); use the zlib codec elsewhere')
        k = ctypes.WinDLL('cabinet', use_last_error=True)
        H = ctypes.c_void_p
        PSIZE = ctypes.POINTER(ctypes.c_size_t)
        for fn in ('CreateCompressor', 'CreateDecompressor'):
            getattr(k, fn).argtypes = [ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(H)]
            getattr(k, fn).restype = ctypes.c_int
        for fn in ('Compress', 'Decompress'):
            getattr(k, fn).argtypes = [H, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t, PSIZE]
            getattr(k, fn).restype = ctypes.c_int
        for fn in ('CloseCompressor', 'CloseDecompressor'):
            getattr(k, fn).argtypes = [H]
            getattr(k, fn).restype = ctypes.c_int
        self.k = k
        self.c = H()
        self.d = H()
        if not k.CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, None, ctypes.byref(self.c)):
            raise FvdError(f'CreateCompressor(XPRESS_HUFF) failed, error {ctypes.get_last_error()}')
        if not k.CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, None, ctypes.byref(self.d)):
            raise FvdError(f'CreateDecompressor(XPRESS_HUFF) failed, error {ctypes.get_last_error()}')
        self.buf = ctypes.create_string_buffer(1)

    def compress(self, data):
        need = len(data) + len(data) // 2 + 4096
        if len(self.buf) < need:
            self.buf = ctypes.create_string_buffer(need)
        out = ctypes.c_size_t(0)
        src = ctypes.c_char_p(data)
        if not self.k.Compress(self.c, src, len(data), self.buf, len(self.buf), ctypes.byref(out)):
            err = ctypes.get_last_error()
            if err != ERROR_INSUFFICIENT_BUFFER:
                raise FvdError(f'Compress(XPRESS_HUFF) failed, error {err}')
            self.buf = ctypes.create_string_buffer(out.value + 4096)
            if not self.k.Compress(self.c, src, len(data), self.buf, len(self.buf), ctypes.byref(out)):
                raise FvdError(f'Compress(XPRESS_HUFF) failed, error {ctypes.get_last_error()}')
        return ctypes.string_at(self.buf, out.value)

    def decompress(self, data, size):
        dst = ctypes.create_string_buffer(size)
        out = ctypes.c_size_t(0)
        if not self.k.Decompress(self.d, ctypes.c_char_p(data), len(data), dst, size, ctypes.byref(out)):
            raise FvdError(f'Decompress(XPRESS_HUFF) failed, error {ctypes.get_last_error()}')
        if out.value != size:
            raise FvdError(f'Decompress(XPRESS_HUFF) gave {out.value} bytes, expected {size}')
        return dst.raw

    def close(self):
        if self.c:
            self.k.CloseCompressor(self.c)
            self.c = ctypes.c_void_p()
        if self.d:
            self.k.CloseDecompressor(self.d)
            self.d = ctypes.c_void_p()


_xpress = None


def _get_xpress():
    global _xpress
    if _xpress is None:
        _xpress = _Xpress()
    return _xpress


def xpress_available():
    try:
        _get_xpress()
        return True
    except (FvdError, OSError, AttributeError):
        return False


def default_codec():
    return CODEC_XPRESS_HUFF if os.name == 'nt' else CODEC_ZLIB


def compress(codec, data):
    if codec == CODEC_STORED:
        return data
    if codec == CODEC_ZLIB:
        return zlib.compress(data, 6)
    if codec == CODEC_XPRESS_HUFF:
        return _get_xpress().compress(data)
    raise FvdError(f'unknown codec {codec}')


def decompress(codec, data, size):
    if codec == CODEC_STORED:
        out = data
    elif codec == CODEC_ZLIB:
        try:
            out = zlib.decompress(data)
        except zlib.error as e:
            raise FvdError(f'zlib: {e}') from None
    elif codec == CODEC_XPRESS_HUFF:
        out = _get_xpress().decompress(data, size)
    else:
        raise FvdError(f'unknown codec {codec}')
    if len(out) != size:
        raise FvdError(f'frame decodes to {len(out)} bytes, expected {size}')
    return out


# ==================================================================== header
class FvdHeader:
    __slots__ = ('version', 'header_size', 'width', 'height', 'frame_count', 'fps_num', 'fps_den', 'codec',
                 'predictor', 'model_id', 'flags', 'input_w', 'input_h', 'settings_hash', 'index_offset',
                 'thumbs_offset', 'thumb_w', 'thumb_h', 'reserved', 'src_size', 'src_offset', 'src_name')

    def __init__(self, **kw):
        for s in self.__slots__:
            setattr(self, s, kw.get(s, 0))
        if not self.src_name:
            self.src_name = ''

    def pack(self):
        name = self.src_name.encode('ascii', 'replace')[:NAME_LEN - 1]
        return _HDR.pack(MAGIC, self.version, self.header_size, self.width, self.height, self.frame_count,
                         self.fps_num, self.fps_den, self.codec, self.predictor, self.model_id, self.flags,
                         self.input_w, self.input_h, self.settings_hash, self.index_offset, self.thumbs_offset,
                         self.thumb_w, self.thumb_h, self.reserved, self.src_size, self.src_offset,
                         name.ljust(NAME_LEN, b'\0'))

    @classmethod
    def unpack(cls, b):
        if len(b) < HEADER_SIZE:
            raise FvdError('file too short for a header')
        v = _HDR.unpack_from(b)
        if v[0] != MAGIC:
            raise FvdError(f'not an .fvd file (magic {v[0]!r})')
        h = cls(version=v[1], header_size=v[2], width=v[3], height=v[4], frame_count=v[5], fps_num=v[6],
                fps_den=v[7], codec=v[8], predictor=v[9], model_id=v[10], flags=v[11], input_w=v[12],
                input_h=v[13], settings_hash=v[14], index_offset=v[15], thumbs_offset=v[16], thumb_w=v[17],
                thumb_h=v[18], reserved=v[19], src_size=v[20], src_offset=v[21],
                src_name=v[22].split(b'\0', 1)[0].decode('ascii', 'replace'))
        return h

    @property
    def fps(self):
        return self.fps_num / self.fps_den if self.fps_den else 0.0

    def as_dict(self):
        return {s: getattr(self, s) for s in self.__slots__}


class IndexEntry:
    __slots__ = ('offset', 'size', 'flags', 'p2', 'p98', 'lo', 'hi')

    def __init__(self, offset, size, flags, p2, p98, lo, hi):
        self.offset, self.size, self.flags = offset, size, flags
        self.p2, self.p98, self.lo, self.hi = p2, p98, lo, hi

    def pack(self):
        return _IDX.pack(self.offset, self.size, self.flags, 0, self.p2, self.p98, self.lo, self.hi)

    @classmethod
    def unpack_from(cls, b, off):
        o, s, f, _r, p2, p98, lo, hi = _IDX.unpack_from(b, off)
        return cls(o, s, f, p2, p98, lo, hi)


# ==================================================================== writer
class FvdWriter:
    """Streams frames into <path> (normally '<name>.fvd.part'); close() writes the index (+ thumbnails) and the final
    header with HF_COMPLETE. Memory: the index (32 B a frame) and thumbnails (tw*th B a frame) only.

    verify=True decodes every payload again right after compressing it and compares (cheap; catches a codec fault on
    a PC where it could not be tested)."""

    def __init__(self, path, width, height, fps_num, fps_den, codec=CODEC_ZLIB, predictor=PRED_LEFT,
                 model_id=MODEL_FAKE, input_w=0, input_h=0, settings_hash=0, src_name='', src_size=0, src_offset=0,
                 thumb_w=0, thumb_h=0, verify=True):
        if not (0 < width < 65536 and 0 < height < 65536):
            raise FvdError(f'bad frame size {width}x{height}')
        self.path = path
        self.h = FvdHeader(version=VERSION, header_size=HEADER_SIZE, width=width, height=height, fps_num=fps_num,
                           fps_den=fps_den, codec=codec, predictor=predictor, model_id=model_id, input_w=input_w,
                           input_h=input_h, settings_hash=settings_hash, thumb_w=thumb_w, thumb_h=thumb_h,
                           src_size=src_size, src_offset=src_offset, src_name=src_name,
                           flags=HF_THUMBS if thumb_w and thumb_h else 0)
        self.index = bytearray()
        self.thumbs = bytearray()
        self.count = 0
        self.payload_bytes = 0
        self.verify = verify
        self.f = open(path, 'wb')
        self.f.write(self.h.pack())                  # placeholder: HF_COMPLETE not set
        self.pos = HEADER_SIZE

    def add_frame(self, q, flags=0, p2=0.0, p98=0.0, lo=0.0, hi=0.0, thumb=None):
        h = self.h
        q = np.asarray(q)
        if q.shape != (h.height, h.width) or q.dtype != np.uint8:
            raise FvdError(f'frame {self.count}: expected uint8 {h.height}x{h.width}, got {q.dtype} {q.shape}')
        raw = left_predict(q) if h.predictor == PRED_LEFT else np.ascontiguousarray(q).tobytes()
        data = compress(h.codec, raw)
        flags &= ~FF_STORED
        if h.codec != CODEC_STORED and len(data) >= len(raw):
            data = raw
            flags |= FF_STORED
        if self.verify and not (flags & FF_STORED):
            if decompress(h.codec, data, len(raw)) != raw:
                raise FvdError(f'frame {self.count}: {CODEC_NAMES[h.codec]} round trip failed')
        self.f.write(data)
        self.index += IndexEntry(self.pos, len(data), flags, float(p2), float(p98), float(lo), float(hi)).pack()
        if h.flags & HF_THUMBS:
            t = np.asarray(thumb if thumb is not None else np.zeros((h.thumb_h, h.thumb_w)), dtype=np.uint8)
            if t.shape != (h.thumb_h, h.thumb_w):
                raise FvdError(f'frame {self.count}: thumbnail {t.shape}, expected {(h.thumb_h, h.thumb_w)}')
            self.thumbs += t.tobytes()
        self.pos += len(data)
        self.payload_bytes += len(data)
        self.count += 1

    def close(self):
        h = self.h
        h.frame_count = self.count
        h.index_offset = self.pos
        self.f.write(self.index)
        if h.flags & HF_THUMBS:
            h.thumbs_offset = self.pos + len(self.index)
            self.f.write(self.thumbs)
        h.flags |= HF_COMPLETE
        self.f.seek(0)
        self.f.write(h.pack())
        self.f.flush()
        os.fsync(self.f.fileno())
        self.f.close()
        self.f = None
        return h

    def abort(self):
        if self.f:
            self.f.close()
            self.f = None
        try:
            os.remove(self.path)
        except OSError:
            pass


# ==================================================================== reader
class FvdReader:
    """Random access to frames. Reads the header and index (32 B a frame) into memory; frames on demand."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, 'rb')
        try:
            self._open()
        except BaseException:
            self.close()
            raise

    def _open(self):
        """The checks of FORMAT.md, "Reader validation"."""
        path = self.path
        size = os.fstat(self.f.fileno()).st_size
        h = FvdHeader.unpack(self.f.read(HEADER_SIZE))
        if h.version != VERSION:
            raise FvdError(f'{path}: format version {h.version}, this reader knows {VERSION}')
        if not (h.flags & HF_COMPLETE):
            raise FvdError(f'{path}: incomplete (the converter did not finish it)')
        if h.header_size < HEADER_SIZE or h.width == 0 or h.height == 0:
            raise FvdError(f'{path}: bad header')
        if h.codec not in CODEC_NAMES or h.predictor not in (PRED_NONE, PRED_LEFT):
            raise FvdError(f'{path}: unknown codec {h.codec} or predictor {h.predictor}')
        iend = h.index_offset + h.frame_count * INDEX_ENTRY
        if h.index_offset < h.header_size or iend > size:
            raise FvdError(f'{path}: index outside the file')
        if h.flags & HF_THUMBS:
            if not h.thumb_w or not h.thumb_h or h.thumbs_offset < iend or \
                    h.thumbs_offset + h.frame_count * h.thumb_w * h.thumb_h > size:
                raise FvdError(f'{path}: thumbnails outside the file')
        self.h = h
        self.f.seek(h.index_offset)
        self._index = self.f.read(h.frame_count * INDEX_ENTRY)
        self.frame_bytes = h.width * h.height

    def __len__(self):
        return self.h.frame_count

    def entry(self, i):
        if not 0 <= i < self.h.frame_count:
            raise IndexError(i)
        e = IndexEntry.unpack_from(self._index, i * INDEX_ENTRY)
        if e.offset < self.h.header_size or e.offset + e.size > self.h.index_offset:
            raise FvdError(f'{self.path}: frame {i} outside the payload area')
        stored = bool(e.flags & FF_STORED) or self.h.codec == CODEC_STORED
        if e.size == 0 or e.size > self.frame_bytes or (stored and e.size != self.frame_bytes):
            raise FvdError(f'{self.path}: frame {i} has a bad payload size {e.size}')
        return e

    def frame(self, i):
        """Frame i as uint8 HxW (0 = far ... 255 = near, relative disparity normalised per shot)."""
        e = self.entry(i)
        self.f.seek(e.offset)
        data = self.f.read(e.size)
        raw = data if e.flags & FF_STORED else decompress(self.h.codec, data, self.frame_bytes)
        if len(raw) != self.frame_bytes:
            raise FvdError(f'{self.path}: frame {i} has {len(raw)} bytes')
        if self.h.predictor == PRED_LEFT:
            return left_unpredict(raw, self.h.width, self.h.height)
        return np.frombuffer(raw, dtype=np.uint8).reshape(self.h.height, self.h.width).copy()

    def thumb(self, i):
        h = self.h
        if not h.flags & HF_THUMBS:
            return None
        n = h.thumb_w * h.thumb_h
        self.f.seek(h.thumbs_offset + i * n)
        return np.frombuffer(self.f.read(n), dtype=np.uint8).reshape(h.thumb_h, h.thumb_w)

    def close(self):
        if self.f:
            self.f.close()
            self.f = None

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def read_header(path):
    """The header of an .fvd (complete or not), or None if it is not one."""
    try:
        with open(path, 'rb') as f:
            return FvdHeader.unpack(f.read(HEADER_SIZE))
    except (OSError, FvdError):
        return None


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    with FvdReader(argv[1]) as r:
        h = r.h
        print(f'{argv[1]}: {h.src_name} {h.width}x{h.height} {h.frame_count} frames {h.fps:.3f} fps, '
              f'codec {CODEC_NAMES.get(h.codec, h.codec)}, model {MODEL_NAMES.get(h.model_id, h.model_id)} '
              f'input {h.input_w}x{h.input_h}, settings {h.settings_hash:016x}')
        cuts = sum(1 for i in range(len(r)) if r.entry(i).flags & FF_CUT)
        for i in range(len(r)):
            r.frame(i)
        print(f'  {cuts} cuts; every frame decodes; payload {h.index_offset - h.header_size:,} bytes')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
