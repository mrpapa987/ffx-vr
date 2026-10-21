# FFX.exe: what we know about the executable

A reference for everything learned by reading and patching the game binary,
so the next round does not start from zero. Addresses are **Ghidra-base
virtual addresses** (ImageBase 0x400000, as the FFX-60fps project and the
Fahrenheit sources use them) unless marked *rt* (runtime, relocated). The
native helper converts with `gva_to_rt()` (= exe base + gva - 0x400000).
Dates say when a fact was established; "static" means read from the
disassembly and not yet confirmed at runtime.

## 1. The binary

| | |
| --- | --- |
| File | `FFX.exe` (Steam, FINAL FANTASY X/X-2 HD Remaster), 32-bit |
| sha256 | `0537b2a1...` (full hash in `logs/`; FFX-60fps' sig scanner checks the same build) |
| PE timestamp | 2026-09-10 (a 2026 Steam rebuild; the code matches the addresses FFX-60fps documents, so the layout is the long-standing one) |
| ImageBase | 0x400000; relocated at runtime (seen at 0x00760000) - always relocate, never hardcode rt addresses |
| `.text` | VA 0x401000, raw offset 0x400, size 0x70AA7B |
| `.rdata` | VA 0xB0C000 (strings, float constants) |
| `.data` | VA 0xC0A000, size 0x19CC2C4 (all the game globals below live here) |
| Security cookie | `0xC613D8` (functions start with `mov eax,[0xC613D8]; xor eax,ebp`) |

Disassembling offline (no Ghidra/IDA needed for spot reads):

    objdump -D -b binary -mi386 -Mintel --adjust-vma=0x400c00 \
        --start-address=0x82ED50 --stop-address=0x82F300 FFX.exe

`--adjust-vma=0x400c00` = VA 0x401000 - raw 0x400, so printed addresses are
gva. A full-text dump (2.8 M lines) with `grep` is faster than any tool for
"who writes `[reg+0x198]`" questions. Direct `call` cross-references are a
5-byte scan: `E8 rel32` with `target = gva + 5 + rel32`.

## 2. Constraints that shape every runtime experiment

- **Fahrenheit stage 0 is a debugger.** Any access violation in the process
  is fatal even inside `__try` (SEH never gets it), and hardware breakpoints /
  single-step exceptions never reach a vectored handler of ours. So: never
  probe memory by faulting; `VirtualQuery` first (`ffxvr_safe_read`); find
  readers/writers by static cross-reference, not by data breakpoints.
- Managed exceptions escaping a native->managed callback fail fast with exit
  `0xC0000409`; keep `try/catch` at the boundary (`render_imgui`).
- Code patches are fine: write with `VirtualProtect`, verify the original
  bytes first, `FlushInstructionCache` (the `g_patches[]` facility in
  `ffxvr_native.cpp`; every patch is restorable and logged in `ffxvr_diag.log`).
- Inline hooks: FFX-60fps hooks `Sg_MainLoop` and several `Ch_*` functions with
  MinHook; Fahrenheit's `FhCallHandle<T>` does the same. Both coexist.
- The game thread that renders is the one calling `Present`; our per-frame
  work runs there (`ffxvr_begin_frame`).

## 3. Main loop and draw order

`Sg_MainLoop` 0x820AE0 (FFX-60fps naming), run from a backlog runner at
0x822840. Its draw section (around 0x821280-0x821360) is:

    0x82BE80          (pre-draw)
    0x908480          field pass 0  (= 0x908D00, then 0x90BF10(ctx, 0))
    0x9084A0          field pass 1  (= 0x90BF10(ctx, 1))
    0x82BE20          (pre-character)
    0x82ED50          Ch_DrawMain - unless 0x641CC0() != 0 (debug camera mode)
    0x821A40          (post)
    0x7BC110, 0x82C7B0, 0x843260/0x8432D0/0x843360, 0x8223A0, 0x63E920 ...

Whole passes are gated by byte flags in `.data`: `0x12FBBB2`, `0x12FBBB6`,
`0x12FBBB9`, `0x12FBBBA` (static; which is which is untested - a one-byte
toggle is a cheap way to find out, and a cheap bisect tool). FFX-60fps' own
skip-render flags are `0x12FB7D0`, `0x12FB7D4`, `0x12FB7D8` (`[0x12FB7D8] != 0`
also skips the model-attach work in `Ch_DrawMain`). Frame counter
`0x23CBBF0`; vblank counters `0x12FB7F0/F4`; exec-frames byte `0xC496D4`.
Current map/room id is read through `0x641DA0()` (compared against constants
like 0x24F9, 0x1C0E, 0x1B84 in `Ch_DrawMain` for per-map special cases).

## 4. Actors (characters)

Table pointer `[0x23C44E4]`, count `[0x23C44E0]`, stride **0x880**. Before a
field is loaded the memory is 0xCDCDCDCD fill: only `chr_enabled == 1` with a
plausible name pointer counts as live (`live()` in `module.cs`). Fahrenheit's
`actor.cs` names the first fields; what we added:

| Offset | Meaning | Source |
| --- | --- | --- |
| +0x00 | `chr_id` (u16): 1 = player, 0x1xxx party, 0x2xxx NPC, 0x5xxx field objects, ... | Fahrenheit |
| +0x02 | `chr_enabled` (u16) | Fahrenheit |
| +0x04 | `chr_name` (char*): model name, Tidus = "c101" | Fahrenheit |
| +0x0C | position (vec4); render space negates X and Y | Fahrenheit / measured |
| +0x154 | speed, +0x158 rotation | probe |
| +0x174 | **cull distance** (float; copied from the template at 0x828758, set by script setter 0x82B171; 5000 in Besaid) | static |
| +0x178 | distance to the game camera, written every frame by `Ch_VisibleTest` | static + runtime |
| +0x180 | **hide byte**: non-zero = not drawn. bit 0x08 = no part of the model in the camera frustum (0x82E2F5); bit 0x20 = beyond cull distance (0x82E32C); 0x01 / 0x12 set by map-specific code (0x82F43E, 0x82F47F); whole byte from a script table `0x23C44C0` by model handle (0x82F547) | static + runtime |
| +0x181..+0x183 | related bytes (+0x183 is set per map/actor-id in `Ch_DrawMain`) | static |
| +0x194 | `chr_flags`: bit 0x800 = "model needs the loaded check", 0x1000 = set by `Ch_UpdateAllActors` when the actor is skipped, 0x2000 = tested in `Ch_DrawMain`, 0x1000000 = motion-step flag, 0x40000000/0x7FFFFFFF copied between actors (0x8288xx) | static |
| +0x198 | second flag word: bit 0x01 = has been drawn once (0x82FA08), 0x02/0x04/0x08/0x100 have setters (0x824CC0, 0x82B130, 0x82B970, 0x830D10), 0x10/0x40 set together when alpha is 0, **0x20 = model released (0x827700), a permanent hide**, 0x80/0x200/0x400/0x800 set in 0x8252xx/0x8272xx | static |
| +0x19C | pointer to a linked/attached actor (checked for id 0x207A) | static |
| +0x1BC, +0x1C0 | model part tables (counts at +6 / +8 of the +0x1C0 struct, part entries 0x38 apart off +0x1BC) | static |
| +0x1D0 | root transform submitted to the draw (FFX-60fps interpolates it) | FFX-60fps |
| +0x1E0.. | scale-ish floats used for a magnitude | static |
| +0x328 | table of per-part matrices (0x160 stride, +0x88) | static |
| +0x330, +0x33C, +0x348 | shade / scale floats passed to the material (0x63BB30) | static |
| +0x354 | **alpha** (0 = model hidden by `Ch_DrawMain`) | static + probe |
| +0x360/+0x364/+0x36C/+0x370 | fade / scale variants used per map | static |
| +0x4E4 | a magnitude multiplied by a constant | static |
| +0x524.. | `chr_optpos` (26 sub-positions; the highest with X/Z != 0 is the head) | Fahrenheit / measured |
| +0x694..+0x6A0 | world position used for the camera distance (same as +0x0C family) | static |
| +0x698, +0x568 | texture-animation step args (`Ch_SkeletonJob`) | FFX-60fps |
| +0x808/+0x80C | floats set by 0x824CA0 | static |
| +0x824 | `chr_var1` (0x83EA00 sets -1 for enabled actors) | Fahrenheit |
| +0x830 | **model handle** (the model object; +0x14 of it is the "visible" byte set by 0x63C390) | static + runtime |
| +0x838 | second model handle (shadow / attachment) | static |

### Functions

| gva | Name / role |
| --- | --- |
| 0x82ED50 | **Ch_DrawMain**. Three walks of the actor table. Loop 1: skip if `!enabled`, `+0x180 != 0`, `+0x198 & 0x20`; per-part matrix uploads (0x63C430 / 0x63C3E0 / 0x63C500). Loop 2: sets `+0x198 |= 0x50` when alpha is 0. Loop 3: `0x8314C0(actor)` (cloth matrix), `edi = 0x82E220(actor)`, `0x831480(actor)`, map-specific +0x183 tweaks, then **hide** (`0x63C390(model, 0)`) when `edi != 0`, or `+0x354 == 0` after `0x830240(actor)`, or `+0x180 != 0`; else `0x63C390(model, 1)` and the submit `0x63BB30(model, actor, 4 floats)` + per-part `0x63BC80`, and `0x63BC50` for id 0x5033. |
| 0x82E220 | **Ch_VisibleTest(actor)** -> 1 = hidden. Clears +0x180 bits 0x08/0x20 and +0x194 bit 0x10000000; camera position via `0x7BD470(1, &pos)`; stores distance at +0x178; hidden if +0x180 already set; if +0x194 & 0x800 and `0x63B230(model) == 0` -> bit 0x08; if `0x641CC0() == 0` and +0x174 < distance -> bit 0x20; map 0x24F9 tests distance against +0x174 * 0.5 (`0xB92220`) under `0x787340()` flags. |
| 0x63B230 | model-level frustum check: walks the model's parts, returns 1 at the first part whose node `0x6603C0` says is visible. |
| 0x63C390 | `(model, visible)`: sets model+0x14 and links/unlinks every part's scene node (`0x66D910(part, vis)`: node+0x1C = vis ? part : 0). |
| 0x63BB30 | looks up the part material by name ("MaterialColour", `0xB41980`) and writes the shade/scale floats: the colour/alpha path, not the draw itself. |
| 0x827700 | releases an actor's model: sets +0x198 bit 0x20 and `0x63C390(model, 0)`. |
| 0x8334xx | **Ch_UpdateAllActors** (logic): skips actors with +0x180 != 0 and +0x181 == 0 (hidden actors are not animated), calls 0x834350 per actor, maintains +0x194 bit 0x1000. |
| 0x832E10 | main character update (calls `Ch_StepMotion` 0x838C90 at 0x833273). |
| 0x833390 | Ch_NoSkelPass; 0x832BF6 Ch_SkeletonJob bone loop; 0x837AC0 Ch_SetMotion; 0x822DE0 Ch_ClothSimulate; 0x832DB0 clears +0x194 bits per actor (all FFX-60fps). |
| 0x83EA00 | sets `chr_var1 = -1` for every enabled actor. |
| 0x7DFDE0 | debug print (varargs), harmless. |

## 5. The renderer (PhyreEngine)

Renderer object pointer `[0xCCB9D8]` (`this` of the 0x65xxxx-0x67xxxx
methods). A second global `[0xCCC81C]`, `[0xCCCA58]` (mode byte), `[0xCCB9C2]`.

| Offset in renderer | Meaning |
| --- | --- |
| +0xD0 | render context (submits go through it: `0x5B9A70(ctx, node)`, `0x5B9A90(ctx, node, a, 0, +0x13C)`, `0x5BABD0(ctx, frustum)`) |
| +0xDC | **main camera object**; its view-projection matrix at **+0x74** is the frustum every bounding test uses; +0x04 is its transform (`0x6720C0` recomputes) |
| +0x238 | debug camera (used when +0x10D8C == 1; its own frustum at +0x10D94, FOV at +0x150/+0x154) |
| +0x654 | second camera (frustum +0x74 used for node list 3) |
| +0x38CC | node list 1 (0xC00 max), count +0xF8CC; buckets at +0xF8D0..+0xF954 |
| +0x385C / +0x3854 | node list 2 (pointer / count) |
| +0x3874 / +0x386C | node list 3 (pointer / count) |
| +0x3898 | another list (+0x400 growth, 0x66C690/0x66C700) |
| +0x4064 | byte set by 0x654540 |
| +0x10D8C | **debug camera mode** (setter 0x654550, callers 0x638D38/0x638D5A; getter 0x6517D0; `0x641CC0()` returns `(mode == 1)`); when 1 the engine uses camera +0x238 and skips the model-part frustum test |
| +0x10D90 | companion value (setter 0x654570) |
| +0xF9C6 | byte selecting a shader table (`0xC16B74` vs `0xC169D4` at ctx+0x48) |

Scene nodes: `node+0x0C` = transform (0x451080 copies / builds a matrix from
it, `0x4CA360(out, frustum, m)` multiplies), `node+0x10` = ?, `node+0x1C` =
owning object when linked (0 = unlinked). Objects register through the
virtual draw dispatcher `0x6A52F0(this=object, flags)` -> `0x656C10(node,
flag)` appends to list 1 (or the +0xF8D0 bucket when the flag byte is 0).

**Culling - every site found (19 Oct 2026, static):**

| gva | What |
| --- | --- |
| 0x663A30 | **bounding test** `(this=node, matrix*) -> al`; thiscall, `ret 4`; 0x80 bytes of locals; transforms the node bounds and tests the 4x4. The only primitive. |
| 0x663CC0 | wrapper: node transform (0x451080 + 0x4CA360) then 0x663A30; `ret 4`. |
| 0x652D35, 0x652EBC | scene draw: lists 1 and 2 tested with 0x663CC0 against camera +0xDC, submitted with 0x5B9A70. (`test al,al; je` at 0x652D3A `84 C0 74 0D`, 0x652EC1 `84 C0 74 14`.) |
| 0x6505D6 | list 3 tested with 0x663A30 directly against camera +0x654, submitted with 0x5B9A90. |
| 0x6603C0 | per model part `(this=renderer, part)`: returns 1 without testing when +0x10D8C != 0 or the part has no node, else 0x663CC0(node, camera->+0x74). `jne` at 0x6603D1 (`75 1B`). Only caller: 0x63B230. |
| 0x6C8236 / 0x6C8294 / 0x6C83B0 | "any part visible" aggregate loops over an object's parts (flags on the stack) |
| 0x9F3275 | another traversal (lights or particles), skips the node on a miss (`je` rel32 at 0x9F327C) |
| 0x6527A1, 0x65E6ED | copy the frustum matrix (0x453340) for the sites above; no test of their own |

Patches in `g_patches[]` (all verified-byte, restorable):

| # | Site | Bytes | Effect |
| --- | --- | --- | --- |
| 0, 1 | 0x6157A0 `75 35`, 0x6157B8 `75 1D` -> nop | render-command interpreter draw-model skips | no visible effect (the cull is upstream) |
| 2 | 0x82E32A `75 1C` -> `EB 1C` | distance cull | characters never beyond cull distance |
| 3 | 0x82E378 `74 B2` -> nop | map-0x24F9 distance case | same |
| 4 | 0x6603D1 `75 1B` -> `EB 1B` | model-part frustum test answers visible | no bit 0x08 |
| 5 | 0x663CC0 entry `55 8B EC 81 EC` -> `B0 01 C2 04 00` | wrapper returns visible | lists 1, 2 and model parts |
| 6 | 0x663A30 entry `55 8B EC 81 EC` -> `B0 01 C2 04 00` | the primitive returns visible | every site, incl. list 3 and 0x9F3275 |

"keep characters drawn" = 2+3+4; "keep all geometry drawn" = 5+6. Status of
the runtime test: 2-5 applied and verified ON in the 18 Oct run with no
visible change reported; 6 added after, untested. What no patch reaches:
objects the game never registers (decided upstream by the field system or
scripts) and actors hidden by the script table / +0x180 values other than
0x08/0x20.

## 6. Render-command interpreter

`0x615550(this, list_head, list_end)` (frame 0x4084 bytes): a `switch` at
0x615738 over 44 command types (byte table 0x615A94, jump table 0x615A80).
Type 1 = draw model (case 0x615752): checks two per-part byte flags (the `jne`s
above), `call 0x61C400` (render-state setup, ending in `0x5934C0` = the
model draw / constant-buffer upload; upload call sites gva +0x193312 /
+0x1935B4 from base), then `push esi; push 0xC94F00; call 0x6144F0`
(`0xC94F00` = the renderer context used by 143 functions - too broad to
xref). Callers: a wrapper 0x615500 (list at `[arg]`, `[arg+4]`) and a
double-buffered ring at 0x616089 (5-dword entries at `obj+0x150`, lists at
`+0x10/+0x14`, reset after use).

## 7. Cameras

| gva / address | What |
| --- | --- |
| 0x01137000 | **field camera blocks**, stride 0x1C0, 3 viewports (FFX-60fps). +0x10 horizontal projection scale (nominal 0.8; `P0 = k*field`), +0x14 aspect factor 0.9333, +0x1C focal length in PS2 pixels (512 / 800 / 900 / 1024 by area; `P5 = field/240`). Larger = narrower. Scaling them changed nothing we could see (the cull does not read them; the GPU uses our matrices). |
| 0x7BC980 | MsCommitCameraFovAspect; 0x639860 view commit (FFX-60fps) |
| 0x02311440 | "the view the draw reads" (sources 0x0231142C, 0x023114C4) (FFX-60fps) |
| 0x011370DC | per-viewport camera tween slots (indexed by 0x7BBFC0) (FFX-60fps) |
| 0x01137570 | **battle camera blocks** (indexed by 0x7BF940, 0xDA0 stride by the sig) (FFX-60fps) - start here for battle camera control |
| 0x7BD470(idx, float3*) | game camera position getter (index 1 in `Ch_VisibleTest`) |
| 0x920C00 / 0x920E40 | camera builders; 0x921BE0 minimap root (`0x193BF80`) |
| 0x787340 / 0x787360 / 0x780C90 / 0x786xxx | state getters consulted per actor in `Ch_DrawMain` (flag byte, a u16 id, a bool) |

The constant-buffer side (what the GPU actually gets, and what the override
rewrites) is in `camera-found.md`: view at +64/+48, near projection +64 after
that, main projection +192, inverse view +256 of the per-object buffer family.

## 8. Field system

Context at `0x1876E00`. `0x90BF10(ctx, pass)` is the per-pass draw entry
(pass 0 also runs `0x908D00`): manages a flag word at `ctx+0x67F0C` (bit 0x100
on map 0x8A), light setup (`0x6426C0`/`0x6426A0` over `ctx+0x68788` lights at
`ctx+0x68812`, stride 0xB4), fog on map 0x132 (light 9 from `ctx+0x68E40`),
then on pass 0 `0x90DD80`, `0x90D260`, `0x90DA90` (matrix / light packets,
PS2-style tags 0x30000000 / 0x11000000 / 0x51000000 written at `ctx+0xBA3E0..`)
and a walk of **field objects** at `ctx+0x9D9C8` (stride 0x720, count
`ctx+0x9D9C4`): bit 0 of each entry enables it, `0x90BDB0(ctx, i, k)` resolves
its texture by name (sprintf with the format at `0xB670A8`, then
`0x643A40`), types 0x10 / 0x100 go to `0x916C80` / `0x916D40`. Candidate home
for the sky / backdrop sheets; untested.

Painted-backdrop rooms (hut interiors) are a different thing again: 1456-byte
constant buffers with a dummy camera (see `design-notes.md`, "Painted-backdrop
rooms").

## 9. Input

`XINPUT9_1_0.dll!XInputGetState` and DINPUT8 are imported; the IAT entry is
patched for the left-stick remap (`install_xinput_hook`). Numpad 0 is the
FFX-60fps turbo key.

## 10. Save data

Not in the exe work. Saves are plain files
`Documents\SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster\FINAL FANTASY X\ffx_NNN`
(OneDrive-redirected Documents on this machine); `FhApi.Saves.load(slot)` is
tied to the save-menu flow; the export tool is `tools/export-save-to-steam.ps1`
(the base game did not list the exported slot - unresolved).

## 11. Leads for other goals (from the reading, untested)

- **First-person body hide at the engine level:** hook `0x63C390` (or the
  submit `0x63BB30`) and force `visible = 0` for the actor whose model handle
  (+0x830) belongs to "c101" in first person. Replaces the draw-radius hide and
  its NPC collateral. (`+0x354 = 0` also hides, but scripts may rewrite it.)
- **Drive the engine camera instead of rewriting uploads:** renderer+0xDC
  (+0x74 view-projection, +0x04 transform, 0x6720C0 to recompute). Culling,
  particle facing, lighting and the field packets would all follow.
- **Debug free camera:** renderer+0x10D8C = 1 (setter 0x654550), camera at
  +0x238; also disables the distance cull and gates `Ch_DrawMain`.
- **Pass toggles** `0x12FBBB2/B6/B9/BA` for cheap bisects of whole passes.
- **Spectator / "player has control":** nothing found yet; `0x787340()`
  flag byte and `0x641DA0()` map id are the state getters `Ch_DrawMain` uses.

## 12. Tooling

- Workspace copies for static work: the exe and `text.txt` (full objdump) in
  the analysis scratch area; `hits.txt` (108 references to the camera blocks),
  `hits880.txt` (139 sites using the 0x880 actor stride), per-function dumps
  `f*.txt`.
- Runtime evidence: `ffxvr_diag.log` (`patch ...` lines, `perf:` with GPU
  time), the module log (`actor table:` on demand, `actors:` every 10 s with
  hide bytes), `ffxvr_frame.log` (frame census),
  `ffxvr_stacks.log` (stack census: which game function uploads a buffer).
- FFX-60fps (`_ext/FFX-60fps`, Apache-2.0) is the best external source of
  names and signatures: `src/x/sigs.cpp` (symbols), `src/x/rows/SMOOTH-00*.cpp`
  (Ch_DrawMain, Ch_StepMotion, Ch_UpdateAllActors, Ch_ClothSimulate,
  Ch_SeqFrame), `MAG-021.cpp` (Ch_SetMotion), `TEXANIM-001.cpp`.

## 13. VBF layout and the character shaders (21 Oct 2026)

`data\FFX_Data.vbf` (20.7 GB): header `SRYK`, u32 header size (0x967AC8), u32
file count (71979); then 16-byte MD5s per file; then 32-byte entries
`{u32 index, u32 total_blocks, u64 size, u64 offset, u64 name_offset}`; then
u32 name-table size and the NUL-separated names (`ffx_ps2/...`,
`ffx_data/gamedata/...`). Files are stored raw at `offset` (no per-file
compression at the archive level). Reading one is a seek + read.

The character shaders live at
`ffx_data/gamedata/ps3data/shaders/d3d11/phyrechrlitshader.fx#<md5>.phyre`
(222 variants) plus `chreasyshadow`, `chry0shadow`, `characterbtmtotop`.
Each `.phyre` is a sequence of zlib streams; inflated (7.9 MB for one) it is
a Phyre asset (`RYHPT` header) holding ~770 DXBC blobs. Their RDEF reflection
settles the skinning facts the VR-hands work needed:

- `$Globals` carries `BoneTransforms`: **float4x4[80]** (class 3 = column
  class, 4x4), 5120 bytes, at **+1344, +1360 or +1456** depending on the
  variant (buffer sizes 6912..7312; the same size can use two offsets, e.g.
  7104). The mod now locates the palette by content (`palette_start`).
- Input semantics `SkinnableVertex`, `SkinnableNormal`, `SkinWeights`,
  `SkinIndices`; macro `SKINNING_ENABLED`. Phyre's `EvaluateSkinPosition`
  sums `weight * BoneTransforms[index]` into a float3x4 and applies it with
  w = 1, which is why blending is linear in world space and a zero matrix
  sends a vertex toward the world origin (seen in-game).
- Consequence used by the hands: a **NaN matrix** in a slot makes every
  vertex that references that slot NaN (even with weight 0), and NaN
  triangles are dropped by the rasterizer - a clean cut at a seam. Slot 0
  must never be cut (padding indices point there).

