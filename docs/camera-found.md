# The camera, found

Resolved 16 October 2026 from a RenderDoc capture of Besaid. This supersedes
the PhyreEngine-internals approach in the project history: the camera is
reachable directly from the vertex shader constant buffers.

## Where it lives

Vertex shader constant block 0. Every 3D draw binds a large per-object buffer
(~1748-1776 floats, about 7 KB). The first few hundred bytes are per-frame
camera data, identical across all of them; the rest is per-object.

| Byte offset | Contents | Buffers agreeing |
| --- | --- | --- |
| 64 | **View matrix** (world to view) | 34 of 55 |
| 128 | Projection, near-plane variant | 34 |
| **256** | **Projection matrix** (main) | **55 of 55** |
| 320 | Inverse view: the camera's world transform | 34 |

## The matrices, as captured

View matrix at byte 64:

    -0.9960  -0.0000   0.0910   221.4260
     0.0220   0.9700   0.2420   -86.0490
    -0.0880   0.2430  -0.9660   264.1290
     0.0000   0.0000   0.0000     1.0000

Projection at byte 256:

     2.2590   0.0000   0.0000   0.0000
     0.0000   4.0170   0.0000   0.0000
     0.0000   0.0000  -1.0000  -2.0000
     0.0000   0.0000  -1.0000   0.0000

## Conventions - the detail that cost us an evening

**Translation is in the last COLUMN** (elements 3, 7, 11), not the last row.
Every earlier analysis of the constant-buffer captures tested for translation
in the last row, so it would have reported nothing even had the camera been
present. Column-vector convention: `v_view = M * v_world`.

Camera world position from the view matrix is therefore `-R^T * t`, where R is
the upper 3x3 and t the last column.

## Cross-checks

Two independent derivations agree, which is what makes this trustworthy rather
than a plausible-looking coincidence:

- The 3x3 at byte 320 is the exact transpose of the 3x3 at byte 64.
- `-R^T t` from the view matrix gives (245.68, 19.28, 255.82); the translation
  column at byte 320 reads (245.66, 19.31, 255.89). Same point.

So byte 64 is the view matrix and byte 320 is its inverse, the camera's own
world transform. Either can be read; byte 320 gives the camera position
without any arithmetic.

From the projection matrix: **vertical FOV 28.0 degrees, horizontal 47.8,
aspect 1.778 (16:9)**. The 16:9 aspect is baked into the projection, which
matters for VR - each eye needs its own aspect and FOV.

## What this unlocks

Writing a modified view matrix into byte 64 of that buffer before each draw
makes the game render from wherever we choose. That is the whole mechanism for
the diorama and fixed-viewpoint modes:

1. Read the view and projection at the start of a frame.
2. For each eye, compute a new view (view \* eye offset) and a new projection
   (per-eye FOV and aspect from OpenXR).
3. Write them back and let the game draw twice.

The near-plane variant at byte 128 means more than one projection is in play
per frame, so both will need substituting, and we should check whether the
extra rotation found at byte 768 (12 buffers, camera at 251.80, -12.31,
325.89) is a shadow or reflection pass that needs its own treatment.

## Caveat

These offsets come from one capture of one scene. They should be re-verified in
a battle and a cutscene before anything depends on them, and they are specific
to the "P4" binaries - a future game patch could move them, exactly as the
October patch moved Fahrenheit's hardcoded addresses.
