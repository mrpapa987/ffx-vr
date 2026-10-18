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


## Confirmed live at runtime (16 Oct 2026, evening)

Verified in-process, not just in a RenderDoc capture. While walking back and
forth across one field screen, the view matrix changed 30 times a second (the
game's logic rate), yaw swung 61-89 deg, pitch tilted, and the position
dollied along a rail. On every trace line the position derived from the view
matrix (`-R^T t`) matched the inverse view's translation column to 0.01.

Layout, in the **1072 / 1232-byte per-object buffers** (VS slot 0):

| Byte | Contents |
|---|---|
| 64  | View (world -> view), translation in the last column |
| 256 | Projection |
| 320 | Inverse view (camera -> world); its translation column is the camera position |

The **~6992-byte buffers** are skinned characters: a different layout whose
blocks from +1344 on are bone matrices (dozens of distinct positions each).
Do not read the camera from those. Not every 1072-byte buffer carries a
camera either; validate with the orthonormal-rotation test at +64.

### How the data actually arrives, and why it took all day

- The per-object buffers are **DEFAULT usage, filled with `UpdateSubresource`**
  (~400k calls per short run across ~110 buffers). Not Map, not CreateBuffer
  with initial data.
- **The D3D11 runtime restores vtable slots it owns.** Our patches to Draw,
  DrawIndexed, DrawIndexedInstanced, DrawInstanced and UpdateSubresource were
  silently put back to the d3d11.dll originals; VSSetConstantBuffers, Map and
  Unmap were left alone. So those counters read zero for hours and we wrongly
  concluded the game never called them. Those functions are now **detoured
  inline** through Fahrenheit's MinHook (`FhCallHandle`/`FhCallLocation` with
  the address taken from the vtable), which the runtime cannot undo.
- The game draws with plain `DrawIndexed`/`Draw`, never the instanced forms.
- F9 dumps are taken standing still, so a 10-frame dump always shows one
  matrix. A frozen dump is not evidence of a stale buffer.
- The title screen presents as an identity camera at (0, 0, -1), yaw 180.

### The injection point

The `UpdateSubresource` detour sees the CPU bytes **before** they reach the
GPU. Rewriting the view/projection at +64/+256/+320 in a private copy and
forwarding that is enough to move the camera for every object drawn with it.
That is where per-eye matrices go.

## Full per-object buffer layout (verified against dumps, 16 Oct 2026)

The vertex shader does **not** use the view matrix directly; it uses
pre-multiplied products. Patching only +48/+64/+320 changes lighting (the
position at +48 feeds the view vector) and nothing else.

| Byte | Contents | Present in |
|---|---|---|
| +48  | camera position float3 (lighting) | all |
| +64  | view | all |
| +128 | projection, near-plane variant (z row `-1 -1`) | all |
| +192 | **proj(+128) × view** | all |
| +256 | projection, main (z row `-1 -2`) | all |
| +320 | inverse view | all |
| +480 | world (this object) | ~1 KB family |
| +544 | view × world | ~1 KB family |
| +608 | inverse world | ~1 KB family |
| +672 | **proj(+128) × view × world** | ~1 KB family |
| +736 | inverse world × inverse view | ~1 KB family |

The ~7 KB skinned buffers share everything through +320 and then hold bone
matrices from +1344 on instead of the world-derived blocks.

Moving the camera means recomputing every derived block from the new view.
The override does so, replacing a block only when its original equals the
product it is expected to be.

### Other layouts (verified 16 Oct 2026, dump beside a save sphere)

- **1872 B and up** (save-sphere family, skinned characters): camera blocks
  as above, but the world-derived set -- world, view×world, inverse world,
  proj×view×world, inverse(view×world) -- sits at **+640/+704/+768/+832/+896**
  instead of +480 onward. Bone matrices follow.
- **608 B**: the common layout shifted 16 bytes earlier (position +32, view
  +48, VP +176, inverse +304); no world blocks.
- **336 B**: only proj×view at +192, no view of its own. Patched by
  recognising the frame's known product.
- **192 B**: a mirrored camera (reflection/sprite pass) with no inverse;
  deliberately left alone.
- **256 B**: a world matrix whose translation is the camera position, i.e.
  something camera-attached (sky?). Left alone.

## Known limit of the override: CPU-projected UI (17 Oct 2026)

Camera override works across field, the save sphere (frame and orb), and
battle. What it cannot fix is anything the game positions by projecting a
world position through *its* camera on the CPU and then drawing in 2D: the
battle target cursor, damage numbers, enemy names, party highlights. Those
arrive at the GPU as screen coordinates, with no matrix to rewrite.

Fix belongs to the VR UI milestone: read the target's position from the
battle state via Fahrenheit and draw our own 3D marker. Not a rendering fix.

## Battle markers (17 Oct 2026): resolved by drawing our own

The game's cursors cannot be redirected from the rendering side: their
position is computed in the update phase and emitted as *vertex data* in a
batched 2D quad, replayed from a command list by one render dispatch chain
(`+2157C9 → +21C5FA → +1934D9/+1934F5 → Map`). No constant buffer changes
when the target changes. Every persistent copy of the camera in memory
(static matrix at `FFX.exe+0x88FD3C`, the 16-entry camera-state array,
stride 0x4E0) also feeds the renderer, so writing the override there
corrupts our own upload patching. Tried and withdrawn.

Our markers use, via Fahrenheit:
- `Globals.Battle.cur_window`: `sel_chrs | (sel_mons << 16)` is one 32-bit
  mask over combatant **ids** (party ids 0..15, monsters `20 + slot`);
  `cur_chr_id` is the acting character's id.
- `Globals.Battle.player_characters` / `monster_characters`: `Chr` arrays
  (stride 0xF90, sparse; garbage slots have `id > 100`), each with `Actor*`.
- `Actor.chr_pos_vec` is in the game's PS2-era space; the renderer's world
  space is **(-x, -y, z)**. Characters are ~20 units tall; markers at +25.
- Projection = game's main projection (+256) × the override view.
- All game-memory reads go through `ffxvr_safe_read` (SEH) because the
  battle pointers are stale outside battle.

Damage numbers, names and the party highlight can follow the same route.

## Second eye: how the frame is built, and what it took

Eye 2 is produced by re-issuing every draw with the render targets, depth
target and sampled render textures swapped for private "twins", and the
object's constant buffer re-uploaded with the eye-2 camera (view translated
along the camera's right axis). Clears, copies and resolves are mirrored
onto the twins. What made it finally work, in the order found:

- **The scene is rendered with 8x MSAA into two render targets** (3840x2160
  R8G8B8A8, plus an MSAA depth) and *resolved* with `ResolveSubresource`
  into plain textures; post-processing reads only the resolved copies, the
  depth and a downscale pyramid -- never the MSAA textures. Until the
  resolve was hooked and mirrored, eye 2's scene never left its MSAA twin
  and the final image was pixel-identical to eye 1 while every other
  counter looked right.
- The game uses VS constant buffer **slot 0 only**, all draws on the one
  immediate context, `OMSetRenderTargets` (the UAV variant is unused).
- The save sphere's orb is drawn **after** the first resolve, in the post
  chain, by a refraction pass sampling the resolved scene; a haze pass
  blends the backdrop the same way. With the resolve unmirrored those read
  the clear colour: "blue cube around the save sphere", "blue fog".
- **Alpha in the colour buffers is garbage** (0 over the backdrop and
  inside the refraction box). The game never shows it, but the overlay's
  ImGui preview alpha-blends, which reproduced the same blue artifacts from
  correct textures. The preview now goes through an opaque copy pass
  (runtime-compiled HLSL via `d3dcompiler_47`, alpha forced to 1). Headset
  layers must be submitted opaque.
- Frame order: ImGui (4 draws, backbuffer) -> shadow maps (4096^2 colour
  targets, no depth, skinned draws; one of them is static and never
  redrawn per frame) -> clears + resolve of the cleared targets -> scene
  pass (~170-200 draws, MRT, MSAA) -> resolve both targets -> post chain
  (full-res ping-pong, the MSAA target reused and re-resolved as a
  ping-pong buffer, pyramid 1920/960/480) -> UI -> backbuffer.
- Eye images for the headset: the first resolve after the scene pass gives
  a clean pre-post pair without the orb; the final backbuffer twin has
  everything including UI. Pick a point in the post chain before the UI
  when the compositor is built.

Diagnostics: F4 writes `img_*.bmp` (quarter size, plus alpha-only variants)
next to the native DLL; `tools/run-ffx.ps1` collects them with the logs.

## Headset (OpenXR) lessons

- Runtime: Virtual Desktop's VDXR (not SteamVR); 32-bit loader from the
  OpenXR.Loader NuGet package, headers from OpenXR.Headers; loaded by full
  path next to the mod DLL, entry points via xrGetInstanceProcAddr.
- **The runtime composites on the game's immediate context inside
  xrEndFrame**, i.e. inside our per-frame hook. Everything our hooks see
  during the OpenXR calls must be ignored (the re-entrancy guard covers the
  whole xr_frame call), or the runtime's targets get twinned, its constant
  buffers go through the override and its 2496x2688 eye texture is taken for
  the backbuffer -> GPU reset (DEVICE_REMOVED with reason DEVICE_REMOVED).
- The swapchain backbuffer has no shader-resource bind: eye 1 is copied into
  a plain texture first. Never create a view on it (d3d11 throws internally
  and the loader dumps core every frame).
- Session teardown must happen before ExitProcess (hooked), or the runtime
  crashes during DLL unload.
- Finite swapchain waits (250 ms) and a stall watchdog ("STALL in stage X")
  turn a hang into a log line.
- Each eye's layer must be submitted with the FOV it was rendered with:
  the headset's asymmetric per-eye FOV written into both projection
  variants (x/y scale and offset only), or the game's symmetric one.
