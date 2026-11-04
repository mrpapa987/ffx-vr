# The .fvd file (FMV depth), version 1

One file per game video: `<game>\fahrenheit\mods\ffxvr\fmvdepth\<name>.fvd`, `<name>` being the video's file name
without `.webm`, lower-cased (`OPN_us.webm` -> `opn_us.fvd`). It holds one 8-bit relative depth map per video frame,
each compressed on its own, so a reader can fetch any frame with one `ReadFile` and one `Decompress`. Written by
`convert.py`, read by `fvd.py`; this page is what the mod's 32-bit C++ reader follows.

The data is made on the player's PC from the player's own copy of the game. It is never shipped, packaged or committed.

All integers are little-endian and unsigned; floats are IEEE-754 binary32. Offsets are absolute file offsets (u64:
files stay well under 4 GB, END_us is about 0.4 GB, but use 64-bit seeks: `SetFilePointerEx`). Every u64 is 8-byte
aligned and every u32 / f32 4-byte aligned, so naturally aligned C structs match the layout on 32-bit MSVC without
`#pragma pack` (add `static_assert(sizeof(header) == 128)` and `== 32` for the index entry anyway).

**Open files with `FILE_SHARE_READ | FILE_SHARE_DELETE`.** The converter may run while the game runs; it finishes each
file by renaming `<name>.fvd.part` over `<name>.fvd`, which fails on Windows while another process holds the old file
open without share-delete (that video's conversion would then be lost).

## Layout

```
[0, header_size)         header (128 bytes in version 1 files as written)
[header_size, index_offset)   frame payloads, frame 0 first, back to back (find each through the index)
[index_offset, +32*N)    index: one 32-byte entry per frame
[thumbs_offset, +T*N)    thumbnails (only when header flag HF_THUMBS): T = thumb_w*thumb_h bytes per frame
```

The writer streams the payloads, then writes the index and thumbnails, then rewrites the header with `HF_COMPLETE`.
A file without that flag is unfinished (the converter only renames `<name>.fvd.part` to `<name>.fvd` once complete,
so it should never be seen; treat it as missing).

## Header (128 bytes)

| offset | type | field | value / meaning |
|---:|---|---|---|
| 0 | char[4] | magic | `FVD1` |
| 4 | u16 | version | 1: the **major** version. A reader refuses any version it does not know (see "Versions"). |
| 6 | u16 | header_size | 128 in every file written so far. A reader requires >= 128 and ignores header bytes past 128. |
| 8 | u16 | width | depth map width: 320 for 16:9 videos, 240 for 4:3 |
| 10 | u16 | height | 180 |
| 12 | u32 | frame_count | N: one depth map per decoded video frame, in decode order |
| 16 | u32 | fps_num | 30000 for every FFX video |
| 20 | u32 | fps_den | 1001 |
| 24 | u8 | codec | 0 stored, 1 zlib (tests only), **2 XPRESS_HUFF** (what the converter writes on Windows) |
| 25 | u8 | predictor | 0 none, **1 left** (see below) |
| 26 | u8 | model_id | 0 fake (pipeline test, not depth: ignore such a file), 1 Video-Depth-Anything-Small |
| 27 | u8 | flags | bit 0 HF_COMPLETE, bit 1 HF_THUMBS; other bits: ignore |
| 28 | u16 | input_w | the model's input size, e.g. 924 x 518 (information only) |
| 30 | u16 | input_h | |
| 32 | u64 | settings_hash | the converter's settings (the manifest's `settings_hash`, hex) |
| 40 | u64 | index_offset | |
| 48 | u64 | thumbs_offset | 0 without HF_THUMBS |
| 56 | u16 | thumb_w | 16 for 16:9, 12 for 4:3 (0 without thumbnails) |
| 58 | u16 | thumb_h | 9 |
| 60 | u32 | reserved | 0 |
| 64 | u64 | src_size | the video's size in bytes (inflated) in FFX_Data.vbf |
| 72 | u64 | src_offset | the video's data offset in FFX_Data.vbf |
| 80 | char[48] | src_name | the video's file name as the VBF stores it: **lower-case** ASCII (`opn_us.webm`), at most 47 characters, the rest NUL (so there is always at least one NUL: byte 127 is 0) |

`src_name` is lower-case, while the game's own name for the video (FFX_VideoList.txt, the player's name buffer at
mgr+0x4F0) is mixed case (`OPN_us.webm`): **compare names case-insensitively** (`_stricmp` / `CompareStringOrdinal(...,
TRUE)`). `src_size` and `src_offset` let the runtime notice a stale file (a game patch that changed the video): compare
them with the VBF's own entry, or simply with the frame count it sees.

## Index entry (32 bytes, frame i at index_offset + 32*i)

| offset | type | field | meaning |
|---:|---|---|---|
| 0 | u64 | offset | the payload's file offset (>= header_size; offset + size <= index_offset) |
| 8 | u32 | size | the payload's size in bytes |
| 12 | u16 | flags | bit 0 FF_CUT: the first frame of a shot (frame 0 always). bit 1 FF_STORED: the payload is not compressed (it did not get smaller). Other bits: ignore |
| 14 | u16 | reserved | 0 |
| 16 | f32 | p2 | 2nd percentile of this frame's model output (relative disparity, arbitrary units) |
| 20 | f32 | p98 | 98th percentile |
| 24 | f32 | lo | the values the 8-bit map was made with (see "The values" below) |
| 28 | f32 | hi | (lo / hi are p2 / p98 smoothed within the shot, reset at a cut) |

END_us (24,828 frames) has a 0.79 MB index: read it whole at open.

## Payload

The frame's `width*height` bytes, row-major from the top-left, after the predictor, then compressed:

- **codec 2, XPRESS_HUFF:** the exact output of the Windows Compression API's `Compress()` for a compressor made with
  `CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF /* 4 */, NULL, &h)` (buffer mode: **not** `COMPRESS_RAW`). Decode
  with `CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, NULL, &d)` once, then per frame
  `Decompress(d, payload, size, out, width*height, &got)` and check `got == width*height` (cabinet.dll / cabinet.lib,
  Windows 8 and later, works in 32-bit processes).
- codec 1, zlib: an RFC 1950 zlib stream (test files only).
- codec 0, or FF_STORED set: the bytes as they are.

**Left predictor (predictor 1).** With `v` the depth map and `r` the stored bytes (all arithmetic mod 256):
`r[0][0] = v[0][0]`; `r[y][0] = v[y][0] - v[y-1][0]` for y > 0; `r[y][x] = v[y][x] - v[y][x-1]` for x > 0. To decode:

```c
for (y = 0; y < h; y++) {
    uint8_t *row = out + y * w;
    if (y > 0) row[0] = (uint8_t)(row[0] + row[-w]);    /* column 0: from the row above */
    for (x = 1; x < w; x++) row[x] = (uint8_t)(row[x] + row[x - 1]);
}
```

**The values.** 0 = far ... 255 = near: the model's relative inverse depth (disparity), normalised per shot. With `d`
the model's value for a pixel, `r = hi - lo`, and `eps = max(1e-6, |hi| * 1e-4)`:

    if r < eps: r = eps                      (a flat frame: every pixel is about lo)
    q = floor(clamp(255 * (d - lo) / r + 0.5, 0, 255))      (rounded to nearest, half up)

So `d ~ lo + q / 255 * (hi - lo)` inside the range, to within half a step; values outside [lo, hi] are clamped to 0 or
255. A frame with `hi - lo < eps` carries no depth worth showing: treat it as flat. The values are affine-invariant (no
metric meaning): the runtime picks the relief's strength and pivot itself; `p2`/`p98`/`lo`/`hi` tell it how much the
model's raw range moved (e.g. to damp a frame whose range collapsed).

## Thumbnails

`thumb_w x thumb_h` bytes per frame, row-major, for a content check of which frame is on screen (compare with the same
reduction of the captured video image) if the decoder's frame counter ever disagrees with the video. Exactly:

1. the video frame as RGB at the model's input size (`input_w x input_h`; ffmpeg bicubic, BT.601 limited range in);
2. every second pixel in both directions (`rgb[::2, ::2]`; the converter does this whenever the input is at least 144
   lines high, which it always is at input sizes 392 and 518);
3. luma `Y = 0.299 R + 0.587 G + 0.114 B` (float);
4. box-filtered to 64x36 (48x36 for 4:3); output pixel `j` of an axis of n inputs and m outputs averages inputs
   `floor(j*n/m)` up to (not including) `floor((j+1)*n/m)`;
5. box-filtered the same way to `thumb_w x thumb_h` (16x9 or 12x9);
6. rounded to nearest (+0.5, truncated) and clamped to 0..255.

The converter's cut detector compares the 64x36 stage of consecutive frames, so it is a fuzzy match by nature: compare
with a tolerance (e.g. the smallest mean absolute difference over +-3 frames), not for equality.

## Reader validation

A reader checks all of these before it uses a value, with 64-bit arithmetic (`frame_count * 32` overflows 32 bits
beyond 134M frames), and treats a file that fails any of them as missing (flat screen, one log line). `fvd.py`'s
`FvdReader` does the same.

At open (`S` = the file size):
1. `S >= 128`, magic `FVD1`, `version == 1` (any other: refuse, it is a format this reader does not know);
2. `flags & HF_COMPLETE`;
3. `header_size >= 128`; `width > 0`, `height > 0` (at most 65535 each by type; the converter writes 320 or 240 x 180,
   and a runtime may simply require that); `codec` in {0, 1, 2} (a runtime without zlib refuses 1), `predictor` in {0, 1};
4. `index_offset >= header_size` and `index_offset + 32 * frame_count <= S`;
5. with HF_THUMBS: `thumb_w > 0`, `thumb_h > 0`, `thumbs_offset >= index_offset + 32 * frame_count` and
   `thumbs_offset + frame_count * thumb_w * thumb_h <= S`.

Per frame, before reading its payload:
6. `offset >= header_size` and `offset + size <= index_offset`;
7. `0 < size <= width * height`; when FF_STORED is set or the codec is 0, `size == width * height`; otherwise
   (compressed) `size < width * height` - the writer guarantees it, since a frame that does not shrink is stored. A
   reader may accept a compressed `size == width * height` too: it is never written. So one buffer of
   `width * height` bytes per open file holds any payload, and one more holds the decoded frame;
8. after decompression: exactly `width * height` bytes came out.

## Versions

`version` is the major version. Anything an existing reader would misread bumps it, and readers refuse versions they
do not know. Additions an old reader can safely ignore keep version 1, and may only: set flag bits that are ignored
today (header byte 27, index flags bits 2-15), use reserved fields that are 0 today, or append header bytes after
byte 128 (with `header_size` > 128; payloads are found through the index, never at `header_size`). Version 1 readers
therefore require `header_size >= 128`, not `== 128`.

## Frame numbering

Frame i is the i-th frame the decoder outputs (ffmpeg, every frame once, `-fps_mode passthrough`). The game's player
counts uploaded frames (`player+0x100`); the frame on screen is that count minus one (docs/design-notes.md, "FMV depth converter").

## Changes

Version 1: 3 Nov 2026.
