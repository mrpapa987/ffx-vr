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

## 14. Map / event identity, blitzball and chocobo leads (21 Oct 2026)

Globals (gva; Fahrenheit's `FhUtil` offsets are gva - 0x400000):

| gva | what | how known |
|---|---|---|
| 0x12FBC40 | current event / map id (int) - Fahrenheit `Globals.event_id` | strcmp'd against special cases 0x17, 0x5D, 0x79, 0x123 next to the "bltz" test at 0x873063 |
| 0x23CBC60 | current map name, NUL-terminated (`bltz0000`, `luca08`, `mihn..`, ...) | filled by a strcpy loop at 0x872FEF/0x873016 from the map-name table (`0x67AAC0(index)`), then `strncmp(.., "bltz")` at 0x873063; 15 readers |
| 0x67AAC0 | map index -> name string | called with the map index (esi) before the copy above |
| 0x874C00.. | blitzball map resolver: table `bltz0000 / 0005 / 0006 / 0200 / 0201` at 0xB5BF78.., picked by `0x6410E0()` (<= 0xA), `sins05` / `cn/` special cases | strings at raw 0x75AF78 |

So a mode detector can be: blitzball = map name starts with `bltz` (or
event_id in {0x17, 0x5D, 0x79, 0x123}: the Luca stadium events); any
per-area memory (diorama view per map) can key on `Globals.event_id`.

Debug leftovers (not needed, but they exist): a "Game parameter config"
menu with `Full Blitz`, `Blitz Cheat`, `Chocobo Game Debug Enable`,
`CharacterSwitching`, `JumpMap %s`, `SetBattle %s` (strings at raw
0x747C00-0x747F00, used around 0x6B7000-0x6BD100); the autosave chocobo
movie is `flash/chocobo.swf` created by 0x66F480 (FFX-60fps QOL-SAVEICON).
Animation sets are named `Field / FieldBattle / Swim / SwimBattle` (raw
0x747634..). The sphere grid's cursor is a model, `omd/CURSOR.omd`; the
battle cursor is not (it is a UI-pass sprite).

Nothing in Fahrenheit, FFX-60fps or the exe's strings describes the chocobo
mount's actor: whether Tidus stays `c101` with a riding motion and the
chocobo is a second actor glued to him, or the pair becomes one model, is a
runtime question (actor table log while riding).


## 15. The battle roster the game keeps (from Farplane, 21 Oct 2026)

[Farplane](https://github.com/topher-au/Farplane) is a memory editor for the
PC remaster. Its FFX offset table is stale, but it resolves offsets by byte
pattern (`Memory/OffsetScanner.cs`), and every pattern still matches our
exe, so the addresses below are confirmed against the current build.
`FhUtil.get_at<T>(off)` offsets are gva - 0x400000.

| gva | FhUtil off | what |
|---|---|---|
| 0x2310EA0 | 0x1F10EA0 | `PartyInBattleFlags`: 8 bytes by chr id, written at the **results screen** (0x783338, from entity +0xDC8). Not live - useless for following. |
| 0x2310EC4 | 0x1F10EC4 | `PartyGainedApFlags`, same story. |
| 0x11334D4 / 0x1134468 | 0xD334D4 / 0xD34468 | party / enemy entity list pointers (= Fahrenheit's `player_characters` / `monster_characters`); entities are 0xF90 bytes, 18 party slots (7 characters + aeons, chr ids 0..0x11) |
| 0x11307E8 | 0xD307E8 | `PartyList`: the menu party order, 8 bytes of chr id, 0xFF empty |
| 0x112A8F8 | 0xD2A8F8 | battle debug flags, one byte each: EnemyInvincible, PartyInvincible, EnemyInput, ?, **FreeCamera** (+4, the one Fahrenheit's `BtlDebugFlags` exposes), ... +8 DisableAttackAnimation, +0x23 DisplayBattlefieldGrid, +0x25 PlayerAlways1HP |
| 0x0CCC7C8 | 0x8CC7C8 | current room id (u16), used by Farplane's auto-free-camera mod to re-arm the flag on room change |
| 0x18763E0 / 0x16AD8A0 | 0x14763E0 / 0x12AD8A0 | blitzball game pointer / blitzball mode (for later) |

The useful find is not in Farplane's table at all - it fell out of reading
the code around the flags above:

**Field roster, live**, gva 0x112C895 (`FhUtil` 0xD2C895): **three bytes of
chr id, the characters currently on the field, 0xFF for an empty slot.**
Index = field slot (entity +0x4FE holds the same slot number). Maintained
by the game itself:

- battle setup 0x79C380: `PartyList[0..2]` go into the three slots, each
  entity gets +0x10 = 1 (active) and +0xDC8 = 1 (participated); the rest of
  `PartyList` is copied to the **reserve list** at 0x112C8A3 (17 bytes).
- character swap 0x7ADC40: finds the outgoing id in the field list and
  writes the incoming id in its place; the reverse in the reserve list; the
  incoming entity gets +0xDC8 = 1 (0x7ADD31) and copies the slot bytes
  +0x4FC..+0x4FE from the outgoing one.
- summon 0x7ADF80..: the field list is copied to the **saved party** at
  0x112C89C (7 bytes) and reserves to 0x112C8B4; every field member except
  the summoner is swapped out (swap with 0xFF), then the summoner is swapped
  for the aeon; the aeon entity's +0x6D1 = summoner's chr id. Magus Sisters
  are chr ids 0xF/0x10/0x11 and all three go in. Aeon chr ids are 8..0x11.
- dismiss 0x7AEED3: saved party copied back into the field list, reserves
  restored.

So "who is on the field" is three byte reads, with no distance heuristic,
and "is an aeon out" is `field[0] in 8..0x11`. The mod logs
`field=[..] saved=[..] reserve=[..]` in the `window:` line now (markers on)
so the next battle test can confirm it before `btl_follow` is switched over.

Entity layout from Farplane's `BattleEntityData` (0xF90): +0x4C name (64),
+0x8C help text (256), +0x18C scan text (512), +0x3E4 animation_start,
+0x3E6 freeze-animation byte, +0x3F0 animation_speed, +0x58E hp_max,
+0x5A6 strength.. (8 stat bytes), +0x5B6 overdrive current/max,
+0x5D0 hp_current / +0x5D4 mp_current, +0x5FE.. status bytes,
+0x6C0 action_defend, +0xF8F battle_row. Nothing about the camera or
the cursor sprite.


---

# Deep dive, 24 Oct 2026 (static, Ghidra + Fahrenheit definitions)

Source: `docs/research-2026-10-24/` (the verifier's corrections applied). Addresses are gva (ImageBase 0x400000) plus the FhUtil offset (gva - 0x400000). Conf.: H = read in the code; M = one step inferred; L = inference. DllCharacteristics 0x8140 includes DYNAMIC_BASE, so always relocate. Nothing below is confirmed at runtime yet.

## 16. Threads

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| 0x62A600 | 0x22A600 | framework init ("MainThread"). Starts the render thread, then DirectInput8 (0x624350) | H |
| 0x615140 | 0x215140 | creates **"PhyreEngineRenderThread"** (CreateThread 0x4300B0, entry 0x614710 → loop **0x616010** "RenderThread") | H |
| 0x616010 | 0x216010 | consumer: waits on an entry's event (INFINITE), runs interpreter 0x615550 → 0x614C60, releases the done semaphore | H |
| 0xCC96F8 | 0x8C96F8 | command-list ring object (16 entries × 0x14: list head/end, ready event, done-semaphore ptr; +0x150/+0x154 consumer/producer index; +0x15C free-slot semaphore 16/16) | H |
| 0x615210 | 0x215210 | producer "kick" (main thread). Called from context method 0x5B6C50. Up to 16 lists in flight | H |
| 0x615EA0 | 0x215EA0 | full flush (queues command 0x2C = SetEvent and waits). Wrappers 0x5BC3F0 (14 callers), 0x638F90, 0x638FB0 | H |
| 0x614C60 case 0x16 | 0x214C60 | **Present**: 0x593760 = `swapchain->Present(sync = [r+0x131C] != 0, 0)`. Encoded on the main thread by **0x5B6AF0** (from the PApplication step 0x627940 and the loading pumps 0x66B1E0/0x66B2E0) | H |
| [0xCDEDE4] | 0x8DEDE4 | job system. **"JobWorker"** threads (string 0xB4CF38, vtable 0xB4CF30) run 0x703A20 (dequeue 0x6DB8A0, run 0x703910). exe-verify.md prints this offset as "0x8CDEDE4", a typo; 0x8DEDE4 is right | H |
| 0x6DBF50 | 0x2DBF50 | job join. Wrapper **0xA44B30 / 0x644B30** (`mov ecx,[0xCDEDE4]; push 0; call 0x6DBF50; ret`, cdecl void()) | H |

**Consequences (H/M):**
- Every hooked D3D11 call, Present, ImGui and OpenXR run on the **render thread**. Game logic and draw
  recording run on the main thread.
- So game memory read at Present can belong to a later frame than the one being presented. Camera bytes
  captured from CB uploads travel inside the list and are safe.
- The main thread uses the immediate context itself only after a full flush. Example: the backbuffer
  re-acquire 0x599210 (section 21).
- Other threads also exist: PhyreEngineWorkerThread, PhyreEngineBulletThread and video threads.

## 17. Sg_MainLoop additions (extends section 3)

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| 0x820AE0 | 0x420AE0 | Sg_MainLoop(float dt) (Fahrenheit `d_Sg_MainLoop(float)`). FFX-60fps re-points the runner's CALL (0x822840) to a wrapper and **verifies this prologue** at init (`55 8B EC 81 EC A4 00 00 00 ...`, required). Do not hook its entry: if our hook lands first, FFX-60fps installs nothing | H (*synth*, ffx60 sigs.cpp/proxy.cpp) |
| 0x82153C | 0x42153C | `call 0x8ABD30` (menu / battle-UI draw), only if `[0x12FBBB4] != 0 && [0x12FB7D0] == 0`; else 0x8AB580(1) | H |
| 0x821617..0x821623 | 0x421617 | frame counter `[0x23CBBF0]`++ (FhUtil 0x1FCBBF0) | H |
| 0x821656 | 0x421656 | `call 0xA44B30`: the **unconditional job join**, after the battle-UI draw. Then the scenario-flag change log (0x787360), `0x63DDC0` (just `ret`), return | H |
| 0x82F881 | 0x42F881 | conditional join at the end of Ch_DrawMain (`if (DAT_01a85f54 < n*4 \|\| == 0)`) | H |
| 0x832E10 | 0x432E10 | Ch_CalcMain dispatches the per-actor update **0x833530** to the JobWorkers (0xA44B90 → 0x71E720) and returns without waiting. Joined at the next Ch_CalcMain (0xA44B80), 0x82F881, 0x821656 and the renderer (0x71E810) | H |
| 0xC496D4 | 0x8C96D4 | exec byte: 1 on logic invocations, 0 on FFX-60fps render-only ones | H/ext |

Main-thread order per invocation (H):
1. logic ticks (incl. battle 0x790C10, Ch_CalcMain)
2. viewport finalize 0x7BC110 (FFX-60fps rewrites the view right after it)
3. 3D record, Ch_DrawMain
4. op_ot_draw 0x7EC470
5. 0x8ABD30 (menu / battle UI)
6. frame counter ++
7. join 0x821656
8. return

Then the renderer frame 0x668930 records the lists, Present is encoded, and the lists are kicked to the
render thread.

**Clean post-join hook point:** a post-hook of 0xA44B30. Of its three callers (0x821656, 0x82F881 and
0x826737 inside FUN_008266F0), only the 0x821656 call comes after the counter increment. M (*synth*).

## 18. Actor additions (extends section 4)

| Offset | Meaning | Conf. |
|---|---|---|
| +0x5C..+0x64 | chr_scale_vec (read by 0x832760) | H |
| +0x6C..+0x74 | **chr_offset_vec**: added to +0x0C to form the root translation | H |
| +0x10C | pointer to an override matrix; when non-null, the root is copied from it | H |
| +0x158 | chr_rotation_rad: the heading used to build the root (0x832A0B) | H |
| +0x19C | parent actor for attachment: the root comes from the parent's bone `+0x328 + b*0x160 + 0x88` | H |
| +0x1D0 | **root matrix** (row vectors; translation = floats 12..14 = **+0x200..+0x208**). Built once per logic tick by **0x832760** (in the JobWorker job 0x833530) from scale, heading and **(+0x0C + +0x6C)**, or +0x10C, or the parent bone. Read by Ch_DrawMain (0x82EF26, 0x82EF85, 0x82FB46) and by the optpos builder. FFX-60fps SMOOTH-002 writes lerp(k-1, k, t) here for the draw and restores it at the next invocation's pre(). So this, not +0x0C, is the 60 fps drawn position. Even without FFX-60fps it differs from +0x0C by +0x6C (and completely for attached actors) | H (code) / M (FFX-60fps source, not its DLL) |
| +0x200..+0x208 | = root translation (also the last fallback of 0x8354F0) | H |
| +0x524 | chr_optpos[26] (vec4). **Computed** by **0x8350F0** in the same job: `optpos[i] = bone[i] (+0x328 + b*0x160 + 0x88) × root(+0x1D0) × local offset`, from the tick-k root. **Not interpolated** by FFX-60fps: it steps at 30 Hz. The section 4 row's "highest with X/Z != 0 is the head" is our heuristic; the game addresses entries by index (sections 19.3/19.4) | H/M |

| gva | FhUtil | Function | Conf. |
|---|---|---|---|
| 0x8354F0 | 0x4354F0 | `get_optpos(actor, idx, out)`: if optpos[idx] is all zero, use optpos[0]; if optpos[0].w (+0x530) == 0, use actor+0x200..+0x208 with w=1 | H |
| 0x8350F0 | 0x4350F0 | optpos builder (JobWorker) | H |
| 0x832760 | 0x432760 | root matrix builder (JobWorker) | H |

## 19. Battle UI and battle cursors

### 19.1 Order and gates

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| 0x89B360 | 0x49B360 | **battle-UI draw**, from 0x8ABD30 (main thread). Gated by `[0x133C948]` (battle UI alive; set 0x89B660, cleared 0x89AD90). Top: 0x780CC0 → **0x79F380** projection pass, then 0x79F660, popups 0x893290, party window 0x896450, gauges 0x8933C0. Then, if battle_state ∈ {1,2} and `[0x23CC08C] != 0 && [0xC58EF8] == 1`: **turn pointer 0x892590** (unless auto-battle), CTB 0x8A0590, then the window loop (for each window with state ≥ 3: `call [window+0x90]`), help 0x8936D0, Sensor 0x893A30, menu fingers 0x8FEB00 | H |
| 0x89AE20 | 0x49AE20 | battle-UI update per logic tick (from 0x790C10): window state machine, turn spin 0x890F30, CTB rebuild 0x89AFC0 | H |
| 0x112A8E0 | 0xD2A8E0 | Btl.battle_state (getter 0x7816F0): 1 = input/active, 0x1F..0x21 = ending | H |
| 0x23CC08C | 0x1FCC08C | **bitmask of chr ids currently choosing a command**: the true "cursors may be shown" gate. Zeroed on submit | H |
| 0x23CC088 | 0x1FCC088 | u16 chr currently choosing (0xFF when none, set by 0x89BB10). Set to 0xFF on submit **only when not auto-battle** | H |
| 0xC58EF8 | 0x858EF8 | must be 1 for command UI (written only by 0x89BC50; also hides the command windows) | H/M |
| 0x133D6E0 | 0xF3D6E0 | **auto-battle** toggle (0x657140, `flash/autobattle.swf`); suppresses the cursors and CTB and auto-confirms | H |

### 19.2 Per-chr screen anchors (written every battle-UI draw by 0x79F380 / 0x39F380)

For each Chr 0..0x1E with byte `Chr+0x10 != 0` (Chr = `[0x11334D4 / FhUtil 0xD334D4] + id*0xF90`; MsGetChr 0x794020):

| Chr field | Content | Conf. |
|---|---|---|
| +0xF34 / +0xF38 (int) | cursor anchor `optpos[Chr+0xDF9]`, raw 1920x1080 centred px, y down. Used by the target pointer (no behind-camera check) | H |
| +0xF3C / +0xF40 (int) | the same in PS2 512x416: x/3.75+256, y/(1080/416)+208. x = -0x200 when behind the camera. Used by the status icons | H |
| +0xF44 / +0xF48 | damage anchor: optpos[Chr+0x442], y -= Chr+0x4E8 (stat 261 / 100), PS2 space | H |
| +0xF4C / +0xF50 | the same, clamped to [0x1B..0x1E5] × [0x22..0x187] | H |
| +0xF54 / +0xF58 | monster gauge anchor, written by **0x795250** (not 0x79F380): ground minus Chr+0x4EC (stat 296 / 100) | H |
| +0xF6C | float, status-icon pulse phase | H |
| +0xDF9 | **stat_cursor_element** (stat 135, MsSetChrStatInfo 0x7B4B70 case 0x87; **default 1**, set by the Chr initialiser 0x79C110 at 0x79C1F4) | H |
| +0xDD9 | stat_cursor (stat 82): 1 = targetable | H |
| +0x442 / +0x4E8 / +0x4EC | damage-number element / damage height / limit-bar height | H |

### 19.3 World-to-screen

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| 0x6428F0 | 0x2428F0 | `cdecl float(int* out_xy, float* world_xyz)` (first argument = output; see the pushes at 0x8925F0..0x8925FD): `ecx = [0xCCB9D8]`, calls 0x6526A0(mode 1). Returns 1/w in ST0. Callers: 0x795250, 0x796C30, 0x79F290, 0x79F380 (×2), 0x892590 (×3) | H |
| 0x6526A0 | 0x2526A0 | thiscall (ret 0xC) on the renderer: (-x, -y, z, 1) × VP, ÷ w → `ftol(960*ndc.x)`, `ftol(-540*ndc.y)`, returns 1/w. When w < 0 it negates x/w and y/w (0x65281C..0x652834), so the result is x/abs(w), not mirrored. VP = cam+0x74, or, when `[0xCCB9C0] == 0` (pan-and-scan), a rebuild from inverse(*cam+0xB4) and 0x651650 (crop F9BC+F9C0). **No screen shake** | H |

### 19.4 Target pointer (the hand)

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| **0x89C870** | **0x49C870** | **draw target pointers**. cdecl void(), prologue `55 8B EC 83 EC 0C`. Returns at once if `[0x133D6E0] == 1`. For each bit i < 0x1C of **`[0x133D178]`**: x, y = Chr[i]+0xF34/F38 (via 0x795920), then `0x8C0670(x, y, i < 0x14 ? 4 : 5)`. Called only from the stub **0x8A84C0** (`call; xor eax,eax; ret`), which 0x8A1090 (from 0x89A860, open target window) installs at window+0x90. Read-only and draw-only: safe to skip | H |
| 0x8C0670 | 0x4C0670 | TkMn2DrawCrossCursor (Fh). Type 4 (party): quad x [ax-100, ax-32]; type 5 (monster): x [ax, ax+68] mirrored; y [ay-22, ay+23]. No bob (only type 0, the menu finger, bobs). Calls `0x903BB0(400, ..., RemapX2(68), RemapY2(45), u0, v0, u1, v1)`. 15 other callers (menus): do not patch | H |
| UV constants | | 0xB5F0FC = 0.552734375, 0xB5F108 = 0.619140625, 0xB5F0EC = 0.5107421875, 0xB5F100 = 0.5546875, 0xB5F0CC = 0.4521484375, 0xB5F0E8 = 0.49609375. On the 1024 atlas: **party (566,523)-(634,568), monster (566,463)-(634,508)**, drawn 68x45 UI units = 1:1 | H (atlas size M) |

### 19.5 Turn pointer (spinning triangle)

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| **0x892590** | **0x492590** | **draw turn pointer**. **cdecl void(int chr)** (the caller at 0x89B448..0x89B450 pushes `movsx [0x23CC088]` and cleans the stack). Fahrenheit's `FUN_00492590` handle (`d_UnknownFn`) has the wrong signature. Prologue `55 8B EC 83 EC 28` | H |
| 0x7950B0 | 0x3950B0 | anchor: `0x8354F0(actor, 1, &0x133FA68)`, then `y -= 2.0` (double at 0xB92270). If the chr has no actor, 0x133FA68 is left stale | H |
| 0x133FA08.. | 0xF3FA08 | vertex table (r, dy), set by 0x897860: (0, -0.3), (-1, -2.3), (+1, -2.3). `V_k = A + (sin θ·r_k, dy_k, cos θ·r_k)` (0x4CEAE0 sin, 0x4CEAC0 cos), each projected by 0x6428F0 (all must have 1/w > 0), then submitted by **0x901190** as a textureless degenerate quad | H |
| 0x133FA7C | 0xF3FA7C | θ (float). **0x890F30**: += 0.10471975 (double 0xB5EE38, 6°) per UI logic tick, -2π when > π. Only from 0x89AE98 while input is active, so 180°/s at 30 Hz | H |
| 0x133FA88 / 0x133F7D4 | 0xF3FA88 / 0xF3F7D4 | colours, u32 RGBA (0x80 = 1.0): **apex** = [0x133FA88] ("dum76"), **base** = [0x133F7D4] ("dum75"), loaded at init by `0x8AE360(2, "dumNN", &out)` | H |

### 19.6 Target selection state

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| 0x133D0D0 | 0xF3D0D0 | selection context (0x140 bytes): +0 default target, +2 command id, +0x10 target type (0 single, 1 group, 2 side, 3 self), candidate masks 0x133D0E4[8] / 0x133D134[8], validity masks 0x133D154.. | H |
| **0x133D178** | **0xF3D178** | **highlighted candidate mask** (bit = chr id; party 0..0x13, monsters 0x14..0x1B). Drawn by 0x89C870 | H |
| 0x133D1F4 | 0xF3D1F4 | single target id (0xFF if multi): drives the name, help and Sensor windows | H |
| 0x133D210..0x133D350 | 0xF3D210 | confirmed copy (memcpy in 0x89A330) | H |
| 0x23CC040 | 0x1FCC040 | command struct (0x48 bytes), registered by 0x792D60 | H |
| 0x112C8C8 | 0xD2C8C8 | Btl+0x1FF8: live mask mirror (0x792DE0; a change triggers the neck look-at 0x7969F0) | H/L |

Writers of 0x133D178:
- 0x89894C (lists), 0x899BB4..0x899E6E (memory cursor), 0x89A5F1 (auto-pick), 0x89AC8B (target-window open:
  memset), 0x89D50D (**every control tick**, cycling through the camera-relative pad map 0x780CE0);
- self-target commands set it to `1<<chr` with no window: 0x89C9AE, 0x89CE85, 0x8A241A, 0x8A27BA.

How the selection is cleared:
- **Nothing clears it on submit** (0x8975A0 zeroes 0x23CC08C, 0x133C93C and 0x133D74C..76C only).
- The only clear is 0x89ABD0 (from 0x89A87A, target-window open).
- 0x898130 is **dead code** (no references).
- Full reset 0x89BB10 (window array 0x780 bytes, index 0xFF, command struct) runs only at the next party input.
- **So BtlWindow +0x08/+0xD8 and 0x133D178 are stale during enemy turns.** Gate on `[0x23CC08C]`. (H)

BtlWindow (base 0x133C950 / 0xF3C950, stride 0xF0, 8 windows, stack top `[0x23CC092]`):
- +0x01 lifecycle (1 pushed, 2 init, 3 opening, 4 active, 5 popping, 6 closing, 0 free; draws for > 2)
- **+0x02 = 1 for a target window**
- +0x06 type, +0x08 chr, +0x0C command
- +0x42 selected index
- +0x90 draw callback (+0x94..+0xA0 the others)
- +0xD4 side, +0xD6 type, +0xD8 mask (copy of 0x133D178 at every control tick), +0xDC open time

(H)

### 19.7 Other battle UI

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| 0x8A0590 | 0x4A0590 | CTB panel: 16 rows, 48 apart; entries 0x1340478[0x20] → pool 0x13404F8 (stride 0x18). Data from 0x89ECE0 ← CTB preview 0x79A200 (includes the pending command). While targeting it marks the rows in the mask ([0x1340470]) | H |
| 0x896450 | 0x496450 | party HP/MP window (`[0xC58EF4] == 1`): highlights the rows in 0x133D178 | H |
| 0x79F660 | 0x39F660 | status icons above heads (type 3) and damage digits (damage list `[0x11360BC]`) → popup queue | H |
| 0x133F110 | 0xF3F110 | **popup queue**: 16-byte entries {s16 x, s16 y, int type, int value, rgba}, count 0x133F108, max 63. Drawn and cleared by **0x893290** → digits 0x8FB840 / popups 0x8FBCC0 → 0x903EE0(400) on battle.dds; glyph u = 444+49·(d%5), v = 254+65·(d/5), 49x65, drawn 34x45. **Damage digits are UI-pass 2D sprites** | H (UI-pass placement M) |
| 0x8933C0 | 0x4933C0 | monster gauges at Chr+0xF54/F58 (update 0x891220 / 0x897AF0) | H/M |
| 0x8E58E0 | 0x4E58E0 | CPU scissor for box/plate primitives: fully clipped rects are set to -100000 PS2 units (0xB368B8) and still drawn. This is the far-away (-376000, -260200) UI pair | M |
| 0x891AC0 | 0x491AC0 | TOBtlCtrlLimitTimer: state [0x133F77C]==2, remaining 0x133F790. The overdrive minigame UIs were **not located** | M |
| 0x781020 | 0x381020 | MsBattleExe: encounter transition = an effect from the common bank (`0x7FD7F0`, entries 8..11), may sample "FrameBuffer" | M |

## 20. 2D UI submit (all menus and battle UI)

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| **0x63EEE0** | 0x23EEE0 | central quad submit (one choke point; the return address names the element). CPU mesh: [3] pos float3 (x, y, 1), stride 12; [4] normal (0,0,1), stride 12; [5] colour float4 = byte/255, stride 16; **[6] UV float2 (u, 1-v), stride 8**; [7] u16 indices 0 1 3 3 1 2. D3D slots: 0 pos, 1 normal, 2 colour, **3 UV** (slots 0 and 3 from runtime picks). Routes to 0x63E930 when the Iggy player `[0xCCC838]` state < 2. Drops everything on skip-render frames | H (slot order M) |
| 0x67BA50 | 0x27BA50 | batch manager: two frame-alternating pools (flip 0x642320 at loop start, flush 0x6423B0). Single-texture meshes up to 12 quads, new mesh on a texture change. Multi-texture meshes (up to 6 textures) around text (0x643AF0(1/0)). **No layers: draw order = submit order** | H |
| 0x903BB0 / 0x903EE0 | 0x503BB0 / 0x503EE0 | TOMkpShapeXYWHUV / ...UVC2: sprite id → texture. Ids 0-198 and 200-398 meswin, **400-598 battle.dds**, 600-649 icon.dds, -1 battle_kuang. UVs pass through unscaled for 400-598 | H |
| 0x8AC8C0 / 0x8AC400 | 0x4AC8C0 / 0x4AC400 | TOGetShapTextureName / TOGetImageWH. **0x3F00 = `/FFX_Data/GameData/PS3Data/menu/D3D11/battle.dds.phyre`** (language variants `menu_us/`, ...), 0x3E00 meswin, 0x3DC0 icon, 0x3E80 battle_kuang | H |
| 0xC5C2C8 | 0x85C2C8 | battle.dds sprite table (110 sprites; entry = count + {u8 u, u8 v, s8 xoff, s8 yoff, u16 w, h, flipx, flipy} in PS2 texels, ×4 for HD). meswin 0xC5B460, icon 0xC5D578. Reader 0x905E90 | H |
| 0x6447E0 / 0x644820 | 0x2447E0 / 0x244820 | graphicUiRemapX2 (x·512/1920) / RemapY2 (y·416/1080) | H |

## 21. Swapchain, backbuffer, renderer frame

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| 0x595800 | 0x195800 | swap chain creation: BufferCount 1, DISCARD, flags 2, **BufferDesc.Format = 0x1C = 28 = DXGI_FORMAT_R8G8B8A8_UNORM** (`mov [esi+0x10],0x1C` at 0x59590F; _SRGB would be 29). exe-engine.md's "sRGB" is a mislabel. No MSAA backbuffer | H (*synth*) |
| **0x599210** | 0x199210 | backbuffer re-acquire: `void __thiscall(obj 0xC94F00, int w, int h)`, `ret 8`. OMSetRenderTargets(0) → release views → ResizeBuffers(count [+0xA0], w, h, fmt [+0x88], 2) → GetBuffer → RT object 0xCA310C, depth 0xCA347C → viewport → OMSetRenderTargets(1). Callers 0x62C320 (call) and 0x62C444 (jmp), after a full flush. **Main thread** | H (thread M) |
| 0x668930 | 0x268930 | renderer frame (main thread, records lists): binds the default RT, cam+0xB4 = r+0xFA04. **FMV branch**: if `[0xCDED3C]` is playing (+0x6D2 or +0x6D0) and `[0x112A008] != 0`, it draws the video quad straight into the backbuffer (0x6E7F00 → 0x692C40) plus Iggy, then returns: no camera, scene or UI pass. Early return if `[0x134082C] != 0` | H |

## 22. Loading

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| 0x76E660 | 0x36E660 | PS2-era synchronous read loop. If `[[0xCC9CD8]+0x3A0] != 0` (getter 0x641C40, its **only** caller is 0x76E6EC) and renderer bytes +0x1196/+0x1197 are 0, it polls at 1 ms and runs 0x642C00 → **0x66B1E0**. Otherwise WaitForSingleObject(INFINITE) (0x6F2D80): no Present | H |
| 0x66B1E0 | 0x26B1E0 | loading frame: icon/Iggy (0x6E50A0, 0x6E4EA0, 0xA267F0), encode Present, kick, app vtbl+0x28, pump messages, **Sleep(0x3C)** | H |
| [0xCC9CD8] | 0x8C9CD8 | PApplication singleton (0x623B90). +0x3A0 = "present while loading": set by 0x66FB30 and 0x643500 (loading/autosave icon), cleared by 0x66FB50 and at the end of boot (0x6416C0) | H |
| 0x783020 | 0x383020 | MsBtlReadManage: battle loading is a per-frame state machine (async reads via the loader vtable 0x76D0D0), so battle stalls may not be in 0x76E660 | M |

## 23. Cameras additions (extends section 7)

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| 0x1137000 + vp*0x1C0 | 0xD37000 | viewport block: +0xC0 enabled, +0xC1 camera index, +0x10/+0x14/+0x1C fov terms, +0x30/+0x50/+0x70 screen-offset pairs, +0x88 "special mode" float | H |
| 0x1137540 + idx*0xDA0 | 0xD37540 | camera block (FFX-60fps "cam_blocks" 0x1137570 is +0x30): eye +0x65C, target +0x40, up +0x00, roll +0xC90 → look-at 0x7C1BE0 | H (roles M) |
| 0x7BC110 | 0x3BC110 | finalize per viewport. Source `[0x231142C]` (script look-at) / `[0x23114C4]` (field camera 0x920C00) / block look-at | H |
| 0x2311440 / 0x2311480 | 0x1F11440 / 0x1F11480 | VIEW (PS2, row vectors) / VIEW→SCREEN (PS2 px); frame copies 0x230FCA0 / 0x230FC60 | H |
| 0x7BC980 | 0x3BC980 | commit: `vfov = 2·atan(448 / (2·vp[+0x14]·vp[+0x1C]))`; aspect ×(16/9)/(4/3) when **byte 0xCCB9C0 != 0** (Hor+). 0x639860 writes inverse(view), X/Y flipped, to r+0xFA04 | H |
| [0xCCB9D8]+0xDC | 0x8CB9D8 | PCameraPerspective: +0x04 view, +0x34 proj, +0x74 VP, +0xB4 → r+0xFA04, near +0xB8 = 1.0, far +0xBC = 65536, aspect +0x150, vfov +0x154. GL-style perspective 0x456B80 | H |
| 0x65E660 | 0x25E660 | refreshes cam+0x74 = this frame's (interpolated) view × clean projection, before the UI draw. UI anchors use the current view, **without shake** | H |
| r+0xFA34 / +0xFA38 | | screen shake/offset (0x642D60), folded only into the GPU projection by 0x6507D0 | H |
| 0xB421C8 | 0x7421C8 | table of 30 map ids with **pan-and-scan**: 0xCCB9C0 = 0, projection y·4/3 plus a moving crop r+0xF9BC (0x65D860, clamped [0.035, 0.295]); map id r+0xFAF4 | H |
| 0xC64CA0 | 0x864CA0 | current spell id (Magic.current_id); spells 0x98 / 0x259 use a split camera, 0x21C changes the near plane | H |

## 24. Input additions (extends section 9)

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| 0x62DBA0 | 0x22DBA0 | Phyre XInput pad poll: `XInputGetState([pad+0x3B0])`; sticks int>>7 at +0x370/+0x374 (L), +0x37C/+0x380 (R, y negated); buttons +0x398..+0x3A7. **Rumble: `XInputSetState(motors = pad+0x58 / +0x59 bytes × 65535)` on every poll** (setter 0x62CB70, gated by the vibration option 0x6329C0). The mod already IAT-hooks GetState; SetState can go the same way | H |
| 0x13270C0 | 0xF270C0 | PS2 pad word (u16): L2 1, R2 2, L1 4, R1 8, □ 0x10, confirm 0x20, cancel 0x40, △ 0x80, select 0x100, start 0x800, up 0x1000, right 0x2000, down 0x4000, left 0x8000 | ext (Fahrenheit) |
| 0x780CE0 | 0x380CE0 | camera-relative pad maps for target cycling (four tables, picked from the game camera) | H |

## 25. FMV, dialogue, overlays, identity

| gva | FhUtil | What | Conf. |
|---|---|---|---|
| [0xCDED3C] | 0x8DED3C | sFMVPlayerManager (PhyFMVPlayerManager; getter 0x6D73A0). Playing = byte +0x6D0 or +0x6D2. Frames go to a texture (callback 0x6D71B0) drawn by Video.cgfx (0x6DA300). List `/FFX_Data/GameData/PS3Data/Video/<region>/FFX_VideoList.txt` | H |
| 0x1326D70 | 0xF26D70 | TOMesWinWork[8] × 0x2C per bank (bank u16 0x1326B82): +0 x, +2 y (512x416), +0x1C anchor, set by the script handler 0x8580C0. No speaker field | H |
| 0x7EC470 | 0x3EC470 | op_ot_draw: 4 layers of registered draw callbacks; slot order 0x11A0960 (0xDA0960), entries 0x11A09E0 (0xDA09E0) {fn, a1, a2, u16 ttl}, registered by 0x7EB500. Drawn after 3D, before the menus (blitzball overlay lead) | H (blitz role L) |
| 0x112D67C | 0xD2D67C | scenario (story) flag u16 (0x787360) | H |
| r+0xFAF4 | | current map id (0x641DA0 / 0x65DE90) | H |

## 26. Hook table (for Fahrenheit detours)

| Target | FhUtil | Delegate | Prologue (MinHook-safe) | Notes |
|---|---|---|---|---|
| 0x89C870 target pointer | 0x49C870 | `cdecl void()` | 55 8B EC 83 EC 0C | may run once per window with state ≥ 3; dedupe |
| 0x892590 turn pointer | 0x492590 | `cdecl void(int chr)` | 55 8B EC 83 EC 28 | Fahrenheit's generated handle is the wrong signature |
| 0xA44B30 job join | 0x644B30 | `cdecl void()` | 8B 0D xx xx xx xx (mov ecx,[abs]) | post-hook = "after the join"; 3 callers |
| 0x599210 backbuffer re-acquire | 0x199210 | `thiscall void(nint self, int w, int h)` | 55 8B EC 83 EC 0C | main thread, after a flush; never touch D3D in the hook |
| 0x893290 popup draw | 0x493290 | (not yet decoded) | - | for 3D damage numbers later |
| 0x820AE0 Sg_MainLoop | 0x420AE0 | `void(float)` | 55 8B EC 81 EC A4 ... | **avoid**: FFX-60fps verifies these bytes at init |

Rules (H):
- Install game-function hooks at module `init()`; Fahrenheit forbids late insertion on an already-hooked
  function.
- No FFX-60fps row touches 0x89C870, 0x892590, 0xA44B30 or 0x599210. Its nearest patches are 0x89C9F5 and
  0x89B1D0.

## 27. ShadowManager: the character shadow atlas (25 Oct 2026, static + frame census)

FFX HD's character shadows are not a cascaded or scene shadow map. Each
character is drawn from straight above into its own tile of an atlas, and
the ground receives the tiles as CPU-built decal quads. Read from the decomp
(chunk_0069) and confirmed in three frame censuses in Luca (lc/lchb1300);
research note shadows-one-eye.md. "static" where not seen at runtime.

| gva | What |
|---|---|
| 0x6D18B0 | creates the targets: `ShadowManager_TempShadowMaptarget` x2 (+0x6C, +0x70); `ShadowManager_OriginalAdvShadowMap` (+0x60) and `ShadowManager_SecondAdvShadowMap` (+0x64), with +0x58 / +0x5C pointing at the second; `ShadowManager_dummyAdvShadowMap` 8x8 (+0x68). The size is `[0xC34408]`: 1024 in the file, 4096 at runtime at 4K. |
| 0x6D3000 | per actor (from 0x82E3E0 via 0x644A00): allocates the actor a tile in an **8x8 grid** (0x6CE3E0 / 0x6CE4F0; "Insert failed, maybe the shadow map is full"); builds the actor's light matrix (translation to the bounding-box centre at y = 0, a scale, and `Rx(-pi/2)` from `[0xB499A0]` = -1.5708: the light looks straight down); stores a per-tile matrix at +0x17E8 + tile * 0x40; sizes the tile viewport from the light-space bounds (0x6D2530, 10 or 20 px per unit); returns the tile's UV rectangle for the decal. |
| 0x6CBF20 | the caster loop. For each dirty tile: the tile camera (near `[0xB10F38]` = 0.1, far from the bounds, aspect = w/h, half-height = h/2: **orthographic**) and the tile viewport (0x5BC060 / 0x5BBDC0), then every mesh part of the actor with the character's own shader in the `ZPrePass` technique. A round-robin cursor at +0x3FC0 caps it at 32 tiles a call. |
| 0x831D10 | builds the receiver's ground quads on the CPU (one quad per actor; no shadow matrix in any constant buffer). |
| 0x65B8A0 | sets up the map shaders' "AdvShadowMap" inputs (`LightViewProjection`, `LVP2`, `MySMSampler2`, `mainChrYPos`). In the Luca census they bind the 8x8 dummy: environment shadows do not come from the atlas. |

`ShadowBlur.cgfx` holds the blur passes (`ShadowGaussianBlurX/Y`, `_Adv`,
`_FromDepth`); `ShadowDynamicMesh` / `ShadowDynamicMeshInstance` is the
receiver mesh.

**The frame** (runtime, D3D11): right after the backbuffer clear and before
the scene, into the atlas (4096x4096, format 28 RGBA8, one sample, **no
depth target**):
- one clear quad per used 512x512 tile (VS cb <= 512 B);
- the casters: per tile a viewport (e.g. 66x74 at 514,4020) and 4-24
  skinned draws of the character (slot-0 cb >= 1872 B, the normal
  skinned-character layout). They write colour silhouettes, not depth.
- a blur into the temp target (4096x4096) and back into a 256x256 sub-tile
  of each tile.

Then, in the scene pass after the opaque and alpha-tested geometry, **one**
receiver draw: 10 quads (60 vertices), VS cb 224 B (the 2D-layer layout;
its PV block is the scene camera's), blend on, z-write off, the atlas at PS
s0. Tiles persist across frames: a tile with no caster draws this frame is
still drawn from its old content.

**The caster constant buffer** is the skinned-character layout with the
light in it: the light's view at the view slot (+64), the light's
orthographic projection in the near-variant slot (+128: last row 0 0 0 1;
seen as x 0.2475, y 0.3191, z -0.0152 / -0.0015), and the **scene's
perspective projection left in the main slot** (+256). A test of the main
slot alone takes it for the scene camera; the mod's `light_proj` checks
both slots (design-notes, "Character shadows: the light camera").
