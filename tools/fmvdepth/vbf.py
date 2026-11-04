#!/usr/bin/env python3
"""vbf.py - read files out of the game's FFX_Data.vbf archive, read-only, without extracting anything to disk.

SPDX-License-Identifier: MIT (part of the FFX VR repository).

The format (checked against the owner's copy on 3 Nov 2026; cross-check: github.com/michivi/vbf-fs):
    0x00 u32 magic 'SRYK' (0x4B595253)
    0x04 u32 header length (everything up to the first data block, block table included)
    0x08 u64 file count n
    n x 16 B   MD5 of each path (not needed for reading)
    n x 32 B   entries {u32 first_block, u32 (unknown, constant), u64 size, u64 data_offset, u64 name_offset}
    u32        name table size, COUNTING THESE 4 BYTES, then NUL-terminated lower-case paths
    u16 x ...  block table to the end of the header: the stored size of each 64 KiB block of every file, in order.
               A block whose stored size equals its full size (the last block of a file may be short), or 0 (= 65,536),
               is stored raw; any other block is a zlib stream that inflates to the full size.
A file's blocks are consecutive in the table (first_block ...) and in the archive (data_offset ...).

Usage (for checks; the converter imports it):
    python vbf.py <FFX_Data.vbf> list [prefix]       paths and sizes
    python vbf.py <FFX_Data.vbf> videolist [us|jp|asia]
    python vbf.py <FFX_Data.vbf> cat <path> > out    one file's bytes to stdout
"""
import array
import os
import struct
import sys
import zlib
from typing import NamedTuple

MAGIC = 0x4B595253
BLOCK = 65536
VIDEO_DIR = 'ffx_data/gamedata/ps3data/video/'
REGIONS = ('us', 'jp', 'asia', 'cn')


class VbfError(Exception):
    pass


class VbfEntry(NamedTuple):
    name: str          # the path as stored (lower case)
    first_block: int
    size: int          # inflated size in bytes
    offset: int        # archive offset of the first block


class VbfArchive:
    """The index of a .vbf (read once, ~10 MB for FFX_Data.vbf) and per-file streaming reads."""

    def __init__(self, path):
        self.path = path
        self.file_size = os.path.getsize(path)
        with open(path, 'rb') as f:
            head = f.read(16)
            if len(head) < 16:
                raise VbfError(f'{path}: too short for a VBF header')
            magic, hlen, n = struct.unpack('<IIQ', head)
            if magic != MAGIC:
                raise VbfError(f'{path}: not a VBF archive (magic {magic:#010x})')
            if hlen < 16 or hlen > self.file_size or n > (hlen // 48):
                raise VbfError(f'{path}: implausible header (length {hlen}, {n} files)')
            d = f.read(hlen - 16)
        if len(d) != hlen - 16:
            raise VbfError(f'{path}: header truncated')
        base = -16                                   # d[x + base] is file offset x
        o_ent = 16 + 16 * n
        o_nt = o_ent + 32 * n
        if o_nt + 4 > hlen:
            raise VbfError(f'{path}: entry table runs past the header')
        nts = struct.unpack_from('<I', d, o_nt + base)[0]
        if nts < 4 or o_nt + nts > hlen:
            raise VbfError(f'{path}: bad name table size {nts}')
        names = d[o_nt + 4 + base:o_nt + nts + base]
        o_bt = o_nt + nts
        nblocks = (hlen - o_bt) // 2
        self.blocks = array.array('H')
        self.blocks.frombytes(d[o_bt + base:o_bt + base + 2 * nblocks])
        if sys.byteorder != 'little':
            self.blocks.byteswap()
        self.entries = {}
        for i, (first, _unk, size, off, noff) in enumerate(struct.iter_unpack('<IIQQQ', d[o_ent + base:o_nt + base])):
            end = names.find(b'\0', noff)
            if noff >= len(names) or end < 0:
                raise VbfError(f'{path}: entry {i} has a bad name offset')
            name = names[noff:end].decode('utf-8', 'replace')
            nb = (size + BLOCK - 1) // BLOCK
            if first + nb > nblocks:
                raise VbfError(f'{path}: {name}: blocks past the block table')
            self.entries[name.lower()] = VbfEntry(name, first, size, off)
        self.header_length = hlen

    # ---------------------------------------------------------------- index
    def names(self, prefix=''):
        p = prefix.lower()
        return sorted(k for k in self.entries if k.startswith(p))

    def get(self, name):
        e = self.entries.get(name.lower().replace('\\', '/'))
        if e is None:
            raise VbfError(f'{name}: not in {os.path.basename(self.path)}')
        return e

    def stored_size(self, entry):
        """Bytes the file takes in the archive (sum of its stored block sizes)."""
        nb = (entry.size + BLOCK - 1) // BLOCK
        return sum((c if c else BLOCK) for c in self.blocks[entry.first_block:entry.first_block + nb])

    # ---------------------------------------------------------------- data
    def iter_chunks(self, entry, f=None):
        """Yield the file's bytes, one inflated 64 KiB block at a time (constant memory)."""
        own = f is None
        if own:
            f = open(self.path, 'rb')
        try:
            f.seek(entry.offset)
            done, i = 0, entry.first_block
            while done < entry.size:
                full = min(BLOCK, entry.size - done)
                c = self.blocks[i]
                clen = c if c else BLOCK
                raw = f.read(clen)
                if len(raw) != clen:
                    raise VbfError(f'{entry.name}: archive ends inside block {i}')
                if c == 0 or clen == full:
                    data = raw
                else:
                    try:
                        data = zlib.decompress(raw)
                    except zlib.error as e:
                        raise VbfError(f'{entry.name}: block {i} does not inflate ({e})') from None
                if len(data) != full:
                    raise VbfError(f'{entry.name}: block {i} is {len(data)} bytes, expected {full}')
                yield data
                done += full
                i += 1
        finally:
            if own:
                f.close()

    def read(self, entry, limit=64 << 20):
        """A small file's whole content (refuses files over `limit`: use iter_chunks for videos)."""
        if entry.size > limit:
            raise VbfError(f'{entry.name}: {entry.size} bytes, too large to read into memory')
        return b''.join(self.iter_chunks(entry))

    def stream_to(self, entry, w):
        """Write the file's bytes to a binary writable (a pipe), block by block. Returns the byte count."""
        n = 0
        for chunk in self.iter_chunks(entry):
            w.write(chunk)
            n += len(chunk)
        return n


# -------------------------------------------------------------------- the game's video lists
def video_list_path(region):
    return f'{VIDEO_DIR}{region}/ffx_videolist.txt'


def parse_video_list(text):
    """FFX_VideoList.txt: lines '<id>:,<file>,<file>' (CRLF). Returns [(id, file)] in file order, as written."""
    out = []
    for line in text.splitlines():
        parts = [p.strip() for p in line.split(',')]
        if len(parts) < 2 or not parts[0].endswith(':') or not parts[1]:
            continue
        try:
            vid = int(parts[0][:-1])
        except ValueError:
            continue
        out.append((vid, parts[1]))
    return out


def read_video_list(arc, region='us'):
    """The region's list as [(id, file name as listed, e.g. 'OPN_us.webm')]."""
    if region not in REGIONS:
        raise VbfError(f'unknown region {region!r} (one of {", ".join(REGIONS)})')
    e = arc.get(video_list_path(region))
    return parse_video_list(arc.read(e, limit=1 << 20).decode('ascii', 'replace'))


def all_videos(arc):
    """Every .webm directly in the video folder (not the region subfolders), lower-case paths."""
    return [k for k in arc.names(VIDEO_DIR) if k.endswith('.webm') and '/' not in k[len(VIDEO_DIR):]]


def video_entry(arc, file_name):
    return arc.get(VIDEO_DIR + file_name)


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 2
    arc = VbfArchive(argv[1])
    cmd = argv[2]
    if cmd == 'list':
        for k in arc.names(argv[3] if len(argv) > 3 else ''):
            print(f'{arc.entries[k].size:>12}  {k}')
    elif cmd == 'videolist':
        for vid, name in read_video_list(arc, argv[3] if len(argv) > 3 else 'us'):
            e = arc.entries.get((VIDEO_DIR + name).lower())
            print(f'{vid:>4}  {name:<24} {e.size if e else "MISSING":>12}')
    elif cmd == 'cat' and len(argv) > 3:
        arc.stream_to(arc.get(argv[3]), sys.stdout.buffer)
    else:
        print(__doc__)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
