"""Test helpers: a synthetic VBF archive writer (MIT, part of the FFX VR repository)."""
import hashlib
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PKG = os.path.dirname(HERE)
if PKG not in sys.path:
    sys.path.insert(0, PKG)

BLOCK = 65536


def write_vbf(path, files, compress_every=2):
    """files: {path: bytes}. Blocks: every `compress_every`-th block is stored raw even if it would compress, the rest
    are zlib streams when that is smaller (a full raw block is stored with size 0, as in the game's archive)."""
    names = sorted(files)
    n = len(names)
    name_tab = b''
    noffs = []
    for nm in names:
        noffs.append(len(name_tab))
        name_tab += nm.lower().encode() + b'\0'
    blocks, data_parts, entries = [], [], []
    pos_data = 0
    for nm in names:
        d = files[nm]
        first = len(blocks)
        off_rel = pos_data
        for bi, i in enumerate(range(0, len(d), BLOCK)):
            raw = d[i:i + BLOCK]
            z = zlib.compress(raw, 6)
            if bi % compress_every == compress_every - 1 or len(z) >= len(raw):
                stored, size = raw, (0 if len(raw) == BLOCK else len(raw))
            else:
                stored, size = z, len(z)
            assert size != len(raw) or stored is raw
            blocks.append(size)
            data_parts.append(stored)
            pos_data += len(stored)
        entries.append((first, len(d), off_rel))
    hlen = 16 + 16 * n + 32 * n + 4 + len(name_tab) + 2 * len(blocks)
    out = bytearray(struct.pack('<IIQ', 0x4B595253, hlen, n))
    for nm in names:
        out += hashlib.md5(nm.lower().encode()).digest()
    for (first, size, off_rel), noff in zip(entries, noffs):
        out += struct.pack('<IIQQQ', first, 3675126, size, hlen + off_rel, noff)
    out += struct.pack('<I', len(name_tab) + 4) + name_tab
    out += struct.pack('<%dH' % len(blocks), *blocks)
    assert len(out) == hlen
    with open(path, 'wb') as f:
        f.write(out)
        for p in data_parts:
            f.write(p)
