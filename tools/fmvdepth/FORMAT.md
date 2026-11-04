# The .fvd file (FMV depth), version 1

One file per game video: `<game>\fahrenheit\mods\ffxvr\fmvdepth\<name>.fvd`, `<name>` being the video's file name
without `.webm`, lower-cased (`OPN_us.webm` -> `opn_us.fvd`). It holds one 8-bit relative depth map per video frame,
each compressed on its own, so a reader can fetch any frame with one `ReadFile` and one `Decompress`. Written by
`convert.py`, read by `fvd.py`; this page is what the mod's 32-bit C++ reader follows.

The data is made on the player's PC from the player's own copy of the game. It is never shipped, packaged or committed.

All integers are little-endian and unsigned; floats are IEEE-754 binary32. Offsets are absolute file offsets (u64:
files stay well under 4 GB, END_us is about 0.4 GB, but use 64-bit seeks: `SetFilePointerEx`).

## Layout

```
[0, 128)                 header
[128, index_offset)      frame payloads, frame 0 first, back to back
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
| 4 | u16 | version | 1. A reader rejects any other version. |
| 6 | u16 | header_size | 128 (the payloads start here; a later version may grow the header) |
| 8 | u16 | width | depth map width: 320 for 16:9 videos, 240 for 4:3 |
| 10 | u16 | height | 180 |
| 12 | u32 | frame_count | N: one depth map per decoded video frame, in decode order |
| 16 | u32 | fps_num | 30000 for every FFX video |
| 20 | u32 | fps_den | 1001 |
| 24 | u8 | codec | 0 stored, 1 zlib (tests only), **2 XPRESS_HUFF** (what the converter writes on Windows) |
| 25 | u8 | predictor | 0 none, **1 left** (see below) |
| 26 | u8 | model_id | 0 fake (pipeline test, not depth: ignore such a file), 1 Video-Depth-Anything-Small |
| 27 | u8 | flags | bit 0 HF_COMPLETE, bit 1 HF_THUMBS |
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
| 80 | char[48] | src_name | the video's file name as in the archive's list, ASCII, NUL-padded (`OPN_us.webm`) |

`src_size` and `src_offset` let the runtime notice a stale file (a game patch that changed the video): compare them with
the VBF's own entry, or simply with the frame count it sees.

## Index entry (32 bytes, frame i at index_offset + 32*i)

| offset | type | field | meaning |
|---:|---|---|---|
| 0 | u64 | offset | the payload's file offset (>= header_size; offset + size <= index_offset) |
| 8 | u32 | size | the payload's size in bytes |
| 12 | u16 | flags | bit 0 FF_CUT: the first frame of a shot (frame 0 always). bit 1 FF_STORED: the payload is not compressed (it did not get smaller) |
| 14 | u16 | reserved | 0 |
| 16 | f32 | p2 | 2nd percentile of this frame's model output (relative disparity, arbitrary units) |
| 20 | f32 | p98 | 98th percentile |
| 24 | f32 | lo | the values the 8-bit map was made with: `q = 255 * clamp((d - lo) / (hi - lo), 0, 1)` |
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

**The values.** 0 = far ... 255 = near: the model's relative inverse depth (disparity), normalised per shot. They are
affine-invariant (no metric meaning): the runtime picks the relief's strength and pivot itself; `p2`/`p98`/`lo`/`hi`
tell it how much the model's raw range moved (e.g. to damp a frame whose range collapsed).

## Thumbnails

`thumb_w x thumb_h` BT.601 luma bytes per frame (the video frame at the model input size, box-filtered), row-major. For
a content check of which frame is on screen (compare with a reduction of the captured video image) if the decoder's
frame counter ever disagrees with the video.

## Frame numbering

Frame i is the i-th frame the decoder outputs (ffmpeg, every frame once, `-fps_mode passthrough`). The game's player
counts uploaded frames (`player+0x100`); the frame on screen is that count minus one (docs/design-notes.md, "FMV depth converter").

## Changes

A change a reader would notice bumps `version`. Version 1: 3 Nov 2026.
