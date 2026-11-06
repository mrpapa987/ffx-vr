# FFX.exe battle UI, battle cursors first (exe-battle-ui, 24 Oct 2026)

Static analysis only. Sources: the Ghidra export in `re/out` (decompiled code from `decomp/chunk_*.c`), objdump of
`re/bin/FFX.exe` (ImageBase 0x400000, checked in the PE header), my own xref database (all `.reloc` absolute refs
plus every E8/E9 rel32 call, `research/exe-battle-ui-work/xref.pkl`, built by `buildx.py`, queried with `x.py`),
the Fahrenheit defs (`re/fahrenheit-src/src/core/ffx/**`; function names quoted from `call.cs` are marked *[Fh]*),
and answers from exe-engine (marked *[engine]*).

**Address convention:** `VA / FhUtil`, where FhUtil = VA - 0x400000. **Confidence:** **H** = I read it in the
code (decompile plus disassembly where it mattered). **M** = the code says it, but a step is inferred (a name, a
meaning, a runtime value). **L** = inference only.

Helper tools are in `research/exe-battle-ui-work/`: `fn.py <VA>` prints a function, `gf.py <regex>` greps the
decompiled code with function context, `x.py <VA>` lists callers and data refs, `sprtab.py` dumps a sprite table,
`rd.py` reads constants, and `dis.txt` is the full objdump.

---

## 0. The answer in one picture

```
Sg_MainLoop (main thread) ... 0x8ABD30 -> 0x89B360  battle UI draw (once per Sg_MainLoop invocation)
  0x780CC0 -> 0x79F380  per-frame projection pass: for each Chr 0..30
                          cursor anchor  = Actor.chr_optpos[Chr+0xDF9]  -> 0x6428F0 (world->screen)
                             -> Chr+0xF34/F38 (raw, 1920x1080 centred ints), Chr+0xF3C/F40 (PS2 512x416)
                          damage anchor  = optpos[Chr+0x442], y -= Chr+0x4E8 -> Chr+0xF44/F48 (+clamped F4C/F50)
           -> 0x79F660  queue status icons over heads and damage/heal digits (popup queue 0x133F110)
  0x893290              draw the popup queue (digits on battle.dds, "Miss" etc.)
  0x896450              party HP/MP window (highlights the rows in the target mask)
  0x8933C0              per-monster gauges at Chr+0xF54/F58
  if [0x23CC08C]!=0 && [0xC58EF8]==1:          (a party member is choosing a command)
     0x892590(chr=[0x23CC088])   TURN POINTER: a 3D triangle spun about the vertical axis, projected per vertex
     0x8A0590                    CTB panel (marks the targeted rows while targeting)
     for each BtlWindow with state>2: call window+0x90
          target window: +0x90 = 0x8A84C0 -> 0x89C870  TARGET POINTER: for each bit of [0x133D178]
                         read Chr+0xF34/F38 -> TkMn2DrawCrossCursor 0x8C0670(x,y, 4 party / 5 monster)
                         -> TOMkpShapeXYWHUV 0x903BB0(400 = battle.dds, 68x45, atlas UV rect)
     0x8936D0 help window, 0x893A30 enemy info (Sensor/Scan) window
  menu finger cursors (queue 0x133F7B4, max 4) -> 0x8FEB00
every 2D sprite -> 0x63EEE0 (central quad submit: pos / normal / colour / UV streams, batched per texture)
```

---

## 1. Answers to the archaeologist's questions (cursor-current-state.md section d)

### Q1. Target pointer placement — H
- **Emitter:** `0x89C870 / 0x49C870` (no Ghidra name; my label: `BtlDrawTargetCursors`). Its only caller is a 3-instruction stub
  at `0x8A84C0 / 0x4A84C0` (`call 0x89C870; xor eax,eax; ret`). Ghidra missed the stub as a function; it is the target
  window's **draw callback**, installed at `+0x90` of the BtlWindow by `0x8A1090` (instruction `0x8A10BE`, `mov [eax+0x90],0x8A84C0`).
  `0x8A1090` is called only from `0x89A860` (open the target-selection window).
- **Code (decompiled):**
  ```c
  if (DAT_0133d6e0 != 1)                               // not auto-battle (see Q6)
    for (i = 0, bit = 1; i < 0x1c; i++, bit <<= 1)
      if (DAT_0133d178 & bit) {                         // current candidate mask
        FUN_00795920(i, &x, &y);                        // x = Chr[i]+0xF34, y = Chr[i]+0xF38
        TkMn2DrawCrossCursor(x, y, i < 0x14 ? 4 : 5);  // 0x8C0670 [Fh]
      }
  ```
- **3D anchor:** `Actor.chr_optpos[Chr.stat_cursor_element]`. That is `actor + 0x524 + 16*(signed char)Chr[+0xDF9]`, a
  **world-space** point, so it is neither root plus offset nor a head heuristic. It is fetched by `0x8354F0 / 0x4354F0`
  (`get optpos(actor, idx, out)`). Fallbacks inside it: if `optpos[idx]` is all zero, it uses `optpos[0]`; if that is
  zero too (w at +0x530 == 0), it uses `actor+0x200..+0x208`. If `Chr.actor == 0`, it uses `0x794C20` (Chr position plus a
  height term). `Chr+0xDF9` is set by battle script stat **135 STAT_CURSOR_ELEMENT** (`MsSetChrStatInfo` [Fh]
  `0x7B4B70`, case 0x87). So every monster's script picks a sub-position (bone/attach point) for its cursor. **H**
- **Projection, done once per frame for every Chr 0..30:** `0x79F380 / 0x39F380`, called from `0x780CC0` at the very top
  of the battle-UI draw `0x89B360`. For each Chr with byte `Chr+0x10 != 0`:
  - `Chr+0xF34 / +0xF38` (int) = raw projection, **1920x1080 centred pixels, y down** (x = ndc.x*960, y = -ndc.y*540).
  - `Chr+0xF3C / +0xF40` (int) = the same in PS2 menu space: x/3.75+256, y/(1080/416)+208. x = **-0x200** if the point is behind the camera.
  - The target pointer uses the **raw** pair F34/F38, with no behind-camera check.
- **World-to-screen function:** `0x6428F0 / 0x2428F0` = `mov ecx,[0xCCB9D8]; push 1; call 0x6526A0`. `0x6526A0` (thiscall on the
  renderer) takes the input (x,y,z), negates it to (-x,-y,z,1) (PS2 to Phyre axes) and multiplies by a VP. If byte
  `[0xCCB9C0] == 0`, the VP is rebuilt as inverse(*cam+0xB4) x proj (`0x651650`, with the 4:3 pan-and-scan crop terms).
  Otherwise it is `cam+0x74` directly. It divides by w, scales by the fixed 1920x1080 (`0x640DB0` returns 0x780 x 0x438),
  rounds to ints with `_ftol` and returns 1/w. Here cam = `[[0xCCB9D8]+0xDC]`. Per exe-engine, cam+0xB4 points at r+0xFA04
  (the camera-to-world committed this frame) and battle is the 0xCCB9C0=1 case (cam+0x74). **H** for the code, **M** for
  which branch battle takes.
- **Sprite placement relative to the anchor**, from the disassembly of `0x8C0670`, in UI units (1920x1080 centred):
  - **party target (type 4):** quad x ∈ [x_raw-100, x_raw-32], y ∈ [y_raw-22, y_raw+23]. The hand sits left of the anchor, pointing right.
  - **monster target (type 5):** quad x ∈ [x_raw, x_raw+68], y ∈ [y_raw-22, y_raw+23], with the art mirrored (u0>u1).
  - It does not bob: types 4/5 have no time term. Only type 0, the menu finger, bobs with cos(t).
  - Consequence for the mod's pointer rule: the quad is **beside** the anchor and centred about 0.5 units below
    it, **not above the actor's root column**. The anchor is the hand's tip: the quad's right edge + 32 (party) or its
    left edge (monster), and its vertical centre - 0.5.
- **Output space:** the projection outputs the 1920x1080 centred space directly. TOMkpShapeXYWHUV then works in PS2 units
  (it converts back with x*3.75-960, y*2.596-540, `0x63EEE0`), so the ring-buffer vertices end up in the same 1920x1080 UI units.

### Q2. A shared world-to-screen helper, and where its outputs are stored — H
- **Yes:** `0x6428F0`. Every battle-UI world anchor goes through it. Callers: `0x79F380` (cursor and damage anchors, every
  frame), `0x795250` and `0x796C30` (monster gauge anchor), `0x79F290` (on-demand cursor projection, no static callers),
  and `0x892590` (turn pointer, 3 vertices). (exe-engine also lists a separate PS2-style path, `0x7C0070`/`0x7BBA50`/`0x7059B0`,
  used by `0x890B20`/`0x890B70` and some effects.)
- **The screen positions are stored in a readable place every frame**, written in `0x79F380` before any sprite is built:

  | Chr field | content |
  |---|---|
  | +0xF34 / +0xF38 | cursor anchor, raw 1920x1080 centred (int). Used by the target pointer |
  | +0xF3C / +0xF40 | cursor anchor in PS2 512x416 (int), -0x200 when behind the camera. Used by the status icons |
  | +0xF44 / +0xF48 | damage-number anchor in PS2 space (int) |
  | +0xF4C / +0xF50 | the same, clamped to [27..485] x [34..391] |
  | +0xF54 / +0xF58 | gauge anchor (written by `0x795250`, PS2 space) |
  | +0xF6C | float, colour-pulse phase of the status icons |
  Chr = `[0x11334D4 / 0xD334D4] + id*0xF90`. Ids 0..0x1E all live in one array; `MsGetChr` [Fh] is `0x794020`.
- **Damage numbers are screen-space UI sprites, not 3D billboards (H for the code path).** `0x79F660` walks the damage
  list (`[0x11360BC]`, a linked list) and queues digits at Chr+0xF44/F48 (or the clamped F4C/F50) into the **popup queue**
  `0x133F110` (16-byte entries {s16 x, s16 y, int type, int value, rgba}, count `0x133F108`, max 63), via `0x899A40`.
  `0x893290` (called from `0x89B360`) draws them: digits via `0x8FB840` -> `TOMkpShapeXYWHUVC2 0x903EE0(400, ...)` on
  **battle.dds**, glyph u = 444+49*(d%5), v = 254+65*(d/5), 49x65 texels, drawn 34x45 UI units. Types 0x3A..0x3D ("Miss"
  etc.) go via `0x8FBCC0`. It also queues the status icons above heads (type 3, at Chr+0xF3C-9*i, Chr+0xF40-0x13).
  So the CPU submits damage numbers through the same quad path, `0x63EEE0`, and they sit on the **same 1024 atlas as the
  target pointer**. exe-engine confirmed (H, code) that the batch manager `0x67BA50` has no layers: draw order = submit
  order, so the digits are drawn **in the UI pass**, early, merged into battle.dds meshes of up to 12 quads with neighbouring
  sprites. The missed picks are therefore pick limits (merged draws, only the first quad / SRV0 inspected, the 32-quad read,
  the digits' short lifetime), not a scene draw. One F9 pick that lists every quad of each battle.dds draw on a frame with
  digits would settle it. Text uses multi-texture meshes (up to 6 textures), so SRV0 is not always the glyph sheet. *[engine]*
- **Names:** the enemy name in the help bar and the Sensor/Scan window `0x893A30(id=[0x133D1F4], 0xB3, 0xF0)` are at
  **fixed** screen positions. They are not world-anchored.

### Q3. Sprite UVs, the sprite table, the atlas file — H
- **The batcher** `0x63EEE0 / 0x23EEE0` (central quad submit; arguments: an 8-float-per-vertex quad struct, a texture name,
  "PS2 coords" flag, primitive kind). It writes into the dynamic mesh returned by `0x67BA50` (thiscall; the mesh object is
  at `+0x94` of the result). Four separate vertex arrays:

  | mesh field | stream | per vertex |
  |---|---|---|
  | `[3]` | position | float3 (x, y, 1.0), stride 12 |
  | `[4]` | **normal, constant (0,0,1)**, stride 12 |
  | `[5]` | colour | float4 = byte/255 (0x80 -> 0.502), stride 16 |
  | `[6]` | **UV** | float2 (u, 1-v), stride 8 |
  | `[7]` | indices | u16 `0 1 3 3 1 2` per quad |

  So the "stream 1 = (0,0,1), stream 2 = (0.5,0.5,…)" seen in the mod's dumps are the **normal** and the **colour**, as
  the CPU builds them. **The atlas UVs are a float2 per vertex in slot 3** (stride 8, v flipped). Slot order 0 pos /
  1 normal / 2 colour / 3 UV; slot 3 as UV also matches a runtime pick in design-notes ("stream 3, stride 8: the atlas UVs").
  *[engine]* **H**
- **Batching (H):** `0x67BA50` appends to the current mesh when the texture name equals the previous one (`+0x5B88`), up
  to 12 sprites per mesh (`+0x5B78 < 0xC`). A texture change starts a new draw. Draw order is preserved, so consecutive
  battle.dds sprites (CTB rows, pointer, digits) share draws. A multi-texture mode (`+0x5B8C == 1`, up to 5 textures
  per mesh, "Multi TextureName") also exists. **M**
- **Texture ids** (PS2 TBP values) map to files via `TOGetShapTextureName` [Fh] `0x8AC8C0` and to PS2-texel sizes via
  `TOGetImageWH` [Fh] `0x8AC400`. **0x3F00 = `/FFX_Data/GameData/PS3Data/menu/D3D11/battle.dds.phyre`** (256x256 PS2
  texels; the HD file is the 1024x1024 BC3 atlas; language variants exist under `menu_us/`, `menu_fr/` and so on), 0x3E00 = meswin.dds,
  0x3DC0 = icon.dds, 16000 (0x3E80) = battle_kuang.dds, 0x3E81-0x3E86 = texture / ffx_bg / menu_new / stonetexture / summonbg.
  Sprite ids passed to the `TOMkp*` helpers: 0-198 and 200-398 meswin, **400-598 battle.dds**, 600-649 icon.dds, -1 battle_kuang.
- **The target pointer's rects do not come from the sprite table.** They are float constants in `0x8C0670`, already
  normalised to the 1024 atlas:
  - **party target:** u 0.552734 .. 0.619141, v 0.510742 .. 0.554688 -> texels **(566,523)-(634,568)**, 68x45
    (constants `0xB5F0FC`, `0xB5F0EC`, `0xB5F108`, `0xB5F100`)
  - **monster target:** u 0.619141 -> 0.552734 (mirrored), v 0.452148 .. 0.496094 -> texels **(566,463)-(634,508)**
    (`0xB5F108`, `0xB5F0CC`, `0xB5F0FC`, `0xB5F0E8`)
  - drawn 68x45 UI units: w = graphicUiRemapX2(68), h = RemapY2(45), back-converted by the submit, i.e. **1:1 texels to UI pixels**.
- **Sprite tables** (for everything else): `0x905E90(table, idx)`; battle.dds table at `0xC5C2C8 / 0x85C2C8` (110 sprites;
  entry = u32 count + count x {u8 u, u8 v, s8 xoff, s8 yoff, u16 w, u16 h, u16 flipx, u16 flipy} in PS2 texels, so x4 for
  the HD atlas). meswin table `0xC5B460`, icon table `0xC5D578`. The full dump of the battle table can be printed with
  `sprtab.py c5c2c8`. It is drawn by `0x9034B0`.

### Q4. Turn pointer — H
- **Draw function:** `0x892590 / 0x492590`, called once from `0x89B360` (site `0x89B450`) with chr = `(short)[0x23CC088]`.
- **It is a 3D triangle**, not a billboard. Anchor A = `optpos[1]` of the chr's actor with y -= 2.0 (`0x7950B0`; game
  y points down, so this is 2 units up). Three vertices in world space:
  `V_k = A + (sin(θ)*r_k, dy_k, cos(θ)*r_k)`, with (r,dy) = (0,-0.3), (-1,-2.3), (+1,-2.3) (globals `0x133FA08..0x133FA2C`,
  set in `0x897860`). So the apex is 2.3 above optpos[1] and the base edge, 2 units wide, is 4.3 above it. Each vertex is projected with
  `0x6428F0`. If all three are in front (1/w > 0), `0x901190` submits a textureless triangle (as a degenerate quad, which
  explains the 6-index textureless draw). The "width oscillation" is the base edge turning about the vertical axis.
- **Spin:** θ = float `0x133FA7C / 0xF3FA7C`. It is advanced by `0x890F30`: +0.10472 rad (6°) per battle-UI update tick,
  wrapped by -2π when > π. That runs in `0x89AE20` (battle UI update, called from the battle logic `0x790C10`) only while
  input is active. So it runs at the logic rate, not the render rate.
- **Colour:** vertex 0 (apex) = u32 `[0x133FA88]`, vertices 1-2 = u32 `[0x133F7D4]` (RGBA bytes, 0x80 = 1.0). They are
  loaded at battle-UI init from named layout entries "dum76" / "dum75" (`0x8AE360(2,"dumNN",&out)`), so they are data,
  not constants. **M** for which vertex gets which colour.
- **Size:** fixed in world units, so its screen size scales with camera distance (matches the mod's observation).
- **Visibility gate** (in `0x89B360`): `[0x133C948] != 0` (battle UI alive) && Btl.battle_state ∈ {1,2} &&
  `[0x23CC08C] != 0` (bitmask of chr ids currently in command input) && `[0xC58EF8] == 1` && `[0x133D6E0] == 0` (not auto-battle).

### Q5. Selection state: who writes and clears it — H
Target-selection context = block `0x133D0D0 / 0xF3D0D0`, 0x140 bytes (memset when a target window opens: `0x898130`, `0x89ABD0`):

| VA | meaning |
|---|---|
| 0x133D0D0 (u16) | default/initial target chr id |
| 0x133D0D2 (u16) | command id |
| 0x133D0E0 | target type: 0 single (cycle list), 1 per group, 2 whole side, 3 fixed self |
| 0x133D0E4[8] / 0x133D134[8] | candidate masks, party / monster side |
| 0x133D154/168/16C/170 | validity masks (stat_cursor, alive, ...), built in `0x898330` and friends |
| **0x133D178 (u32)** | **current highlighted candidate mask** (bit = Chr id; party 0..0x13, monsters 0x14..0x1B). This is what the pointer draws |
| 0x133D17C / 0x133D180[] | party candidate list count / masks |
| 0x133D1D0 / 0x133D1D4[] | monster candidate list count / masks |
| 0x133D1F4 (u16) | single target id (0xFF if multi). Drives the name, help and Sensor windows |
| 0x133D1F6 / 1F8 / 1FC / 1FE / 200 | active list count / list ptr / index / "can switch side" flag / side |
| **0x133D210..0x133D350** | **confirmed copy** of the whole block, made when the player confirms (`0x89A330`, the `memcpy` of 0x50 dwords) |

- **Writers of BtlWindow +0xD8** (u32 = sel_chrs | sel_mons<<16), all with value = `0x133D178`: `0x898640` (build lists,
  first candidate), `0x89A860` (open target window), `0x89D390` (**every tick** in the target window's control state:
  left/right cycles the index, mapped through the camera-relative table `0x780CE0`; up/down switches side), `0x899AC0`
  (memory cursor / auto). It is cleared (=0) by `0x898130` / `0x89ABD0` at the next target-window open. **It is not cleared on confirm
  or submit.** `0x8975A0` (submit) clears `[0x23CC08C]` and sets `[0x23CC088] = 0xFF`, but leaves `0x133D178`, the window stack
  `0x133C950` and the index `[0x23CC092]` stale until the next party input starts (`0x89BB10` memsets the 0x780-byte
  window array and sets index 0xFF). **So during monster turns and animations, `cur_window` (+0x08 chr, +0xD8 mask) is stale.**
- **Highlighted vs confirmed:** `0x133D178` / window+0xD8 = highlighted. The confirmed selection is the copy at
  `0x133D210` and the command struct at `0x23CC040` (0x48 bytes), registered by `0x792D60`. Btl also mirrors the live mask:
  `0x792DE0` writes `[0x112C8C8]` = Btl+0x1FF8 (and +0x1FFC = previous), and on a change it triggers the neck look-at
  (`0x7969F0`) and probably a camera nudge (`0x797BC0(...,0x19,...)`, **L**).
- **BtlWindow field map** (stride 0xF0, base `0x133C950`; stack top index `[0x23CC092]`, -1/0xFF = empty):
  `+0x01` lifecycle state (1 pushed, 2 init, 3 opening, 4 active/control, 5 popping, 6 closing, 0 free; ticked in
  `0x89AE20`: 2 calls +0x94, 3 calls +0x98 until it returns 1, 4 calls +0x9C, 6 calls +0xA0), **`+0x02` = 1 for a
  target-selection window**, +0x06 type, +0x08 (s16) chr id, +0x0C command id, +0x20 command list, +0x42 selected index (for
  target windows: the candidate index), +0x88 / +0x90..+0xA0 callbacks (+0x90 draw), +0xD4 target side, +0xD6 target type,
  +0xD8 mask, +0xDC open time, +0xE0 seconds in menu. Draws run only for state > 2 (`0x89B360` loop).
- **"Pointer drawn" in state terms:** some window in the stack has `+0x90 == 0x8A84C0` (in practice the top one,
  `+0x02 == 1`) with `+0x01 ∈ {3,4,5,6}`, **and** the global gates of Q4 except the auto-battle one, which is checked inside
  `0x89C870`. Then one pointer per bit of `[0x133D178]` below 0x1C.
- **cur_chr_id:** the chr choosing a command is `[0x23CC088] / 0x1FCC088` (u16, 0xFF when none; set in `0x89BB10`).
  BtlWindow+0x08 holds the same id but goes stale (see above). Gate on `[0x23CC08C] != 0`.

### Q6. Source-level off switch — H
Both routines are leaf draw functions. They only read state and then submit sprites, so they are safe to skip:
- **Target pointer:** patch `0x89C870 / 0x49C870` to `C3` (ret; it is cdecl, no arguments; the original bytes are `55 8B EC 83 EC 0C`),
  or NOP the call at `0x8A84C0` (`E8 AB 43 FF FF` -> 5x `90`). Do **not** patch `TkMn2DrawCrossCursor 0x8C0670`: it has
  15 other callers (menus).
- **Turn pointer:** patch `0x892590 / 0x492590` to `C3` (cdecl, the caller cleans the argument; original `55 8B EC 83 EC 28`),
  or NOP the 5-byte call at `0x89B450`.
- Better than a patch: **detour** both on the main thread (Fahrenheit method hooks), record their inputs, and call the
  original only when the headset is off (see section 4).
- Existing flags that would also hide them are **not** usable as switches. `[0xC58EF8]` (written only by
  `0x89BC50`, presumably a script call) also hides all command windows. `[0x133D6E0]` is the **auto-battle** toggle
  (toggled in `0x657140`, which loads `flash/autobattle.swf`). With it set, the game synthesises confirm presses
  (`0x89D390`: `if (d6e0==1) pad=0x20`).
- No debug flag near `0x112A8F8` (Btl+0x28 BtlDebugFlags) touches the cursors. **H** (no reads of it in the UI range).

### Q7. Multi-target, batching, stray pointers — H/M
- One loop in `0x89C870` draws one pointer per set bit, so "all enemies" gives N consecutive battle.dds sprites. They are
  appended to the same mesh as long as the texture does not change (up to 12 per draw). Neighbours in the same draw are
  whatever battle.dds sprite was submitted just before: the CTB rows and markers (`0x8A0590` runs just before the
  windows loop). That explains the batched 12-index draws. **M**
- The pointer is drawn only for bits of `0x133D178`. That equals window+0xD8 at every control tick. They can differ for
  at most the opening ticks (state 3, before control runs), where `0x89A330`'s auto-pick can set `0x133D178` without
  re-copying it to +0xD8. That is a **one-to-few-tick** mismatch. **M**
- A pointer for an actor behind the camera: F34/F38 have no w check, so the quad can land at a mirrored position. **M**

### Q8. CTB panel — H
- **Draw:** `0x8A0590 / 0x4A0590` (called from `0x89B360`, site `0x89B469`, only while input is active and not auto-battle).
  16 rows, 48 UI units apart (`local_18 += 0x30`). Entries are pointers `0x1340478[0x20]` into a pool at `0x13404F8`
  (stride 0x18: +1 chr id, +8 ctb value, +0xC row, +0x10/+0x12 anim offsets, +0x14 anim fn).
- **Data:** built every UI tick by `0x89ECE0(?, mask)` from `0x79A200` (the CTB simulation, *including the pending
  command* from `0x23CC040` via `0x792E50`, i.e. the turn-order preview). Called from `0x89AFC0`.
- **Highlight:** the mask passed is `0x133D178` when the top window is a target window, else the bit of the acting chr.
  Rows of chrs in the mask get the anim fn `0x8A2AD0` (others `0x8A2BA0`). While targeting (`window+0x02==1`) the panel also
  draws sprite 0x1D6 (battle.dds #70) and a `0x903EE0(400,...)` marker on every row whose chr is in `[0x1340470]`
  (= that mask). **So yes, the CTB marks the selected targets.**
- Related: the **party HP/MP window** `0x896450` (rows 0x34 apart) also highlights the party rows in `0x133D178`
  (sprite -1 = battle_kuang), and in case 3 the acting chr `[0x23CC088]`.

### Q9. Camera snapshot timing — H (with exe-engine)
- The UI projects on the **main thread** inside Sg_MainLoop (`0x8ABD30 -> 0x89B360`), after the per-viewport camera
  finalize `0x7BC110`. FFX-60fps interpolates the view in place there, so the cursor anchors use the same
  (interpolated) view as the GPU's scene of that invocation. *[engine]*
- Differences from the GPU's VP: (a) the UI VP has **no screen shake** (`r+0xFA34/38` is folded only into the render-time
  projection `0x6507D0`). (b) There is **no one-tick lag**: exe-engine checked that, earlier in the same draw section,
  `0x711910 -> 0x639590 -> 0x65E660` recomputes `cam+0x74` from this frame's (interpolated) view and the clean projection
  (`0x6720C0`), before `0x8ABD30` runs (an earlier lag warning was withdrawn; **H** *[engine]*). On the ~30 pan-and-scan maps the
  UI also adds the crop offset r+0xF9C0, which the GPU does not. (c) The mod's D3D hooks, Present included, run on the
  **render thread**, concurrently with the main thread's next invocation. So any memory read at Present (Chr+0xF34,
  0x133D178, cameras) can belong to frame n+1 while frame n's quads are drawn. *[engine]*
- **Implication:** read cursor state on the main thread (a detour of `0x89C870`/`0x892590`, or a post-hook of `0x79F380`)
  and hand a frame-stamped snapshot to the render thread.

### Q10. The far-away 66/54-index pair — M
`0x8E58E0 / 0x4E58E0`, the CPU scissor clip for box/plate primitives, **does not drop** fully clipped rects. It sets all four
coordinates to **-100000** (`0xB368B8`) and still returns 1. (The textured path `0x8E5A20` returns 0 and drops them.) -100000
PS2 units go through the submit's x*3.75-960 / y*2.596-540 to (-375960, -260155). Subtract the sprites' small offsets and
you get the observed (-376000, -260200). Callers: `0x8F4B20` (`TODrawCrossBoxXYWHC2` [Fh]), `0x8E5E90`, `0x8F4DF0`,
`0x8FB090`, `0x906420`. So the pair is window frames/plates (most likely scrolled-out list rows of the command window)
clipped away by the scissor. It is not world-anchored. (66/54 indices = 11/9 quads per box.)

---

## 2. Broader battle UI

| What | Where (VA / FhUtil) | Notes | Conf. |
|---|---|---|---|
| Battle UI draw (master) | `0x89B360 / 0x49B360`, from `0x8ABD30` | order shown in section 0. Gated by `[0x133C948]` (set at init `0x89B660`, cleared at end `0x89AD90`) | H |
| Battle UI update (per logic tick) | `0x89AE20`, from battle logic `0x790C10` | window state machine, help window ctrl, enemy-info flags, turn spin, CTB rebuild `0x89AFC0` | H |
| Per-chr projection pass | `0x79F380` (via `0x780CC0`) | Chr+0xF34..F50 (Q2) | H |
| Status icons over heads | `0x79F660` (first loop) | Chr+0x616 & 0x4000 and visible; icons from `0x79FC00`, 9 PS2 px apart, pulse phase Chr+0xF6C; queued type 3 | H |
| Damage / heal digits, Miss etc. | `0x79F660` (second loop) -> queue `0x133F110` -> `0x893290` -> `0x8FB840` / `0x8FBCC0` | digit colours: 0 white, 1 (0x4A,0xA0,0x58) green; positions PS2 at Chr+0xF44/F48; per-chr height = stat 261 STAT_DAMAGE_NUM_POS (Chr+0x4E8 = value/100), element = Chr+0x442 (stat_num_print_element) | H |
| Monster gauges over enemies | `0x8933C0` (draw), `0x891220` / `0x897AF0` (update), anchor `0x795250` -> Chr+0xF54/F58 | ground-y minus stat 296 STAT_LIMIT_BAR_POS (Chr+0x4EC = value/100), with a fallback on the optpos[cursor_element] height. The ratio comes from `0x795550/0x795590` (a value / max). Probably the monster overdrive ("limit") bar. **M** | H/M |
| Target pointer | `0x89C870` -> `0x8C0670` | Q1 | H |
| Turn pointer | `0x892590` | Q4 | H |
| Menu finger cursor | queue `0x133F7B4` (max 4, `0x8999F0`), drawn by `0x8FEB00` at the end of `0x89B360` | type 0 = finger (meswin sprite), bobbing with cos(t). Types 4/5 = scroll arrows | H |
| CTB panel | `0x8A0590`; data `0x89ECE0` | Q8 | H |
| Party HP/MP/limit window | `0x896450` | gated by byte `[0xC58EF4] == 1`; 4 layouts by `[0x133D782]` | H |
| Command window | `TOBtlDrawCommandWindow` [Fh] `0x89F370`; window init `0x899870` / `0x8A10D0` (per-type layout) / `0x8A0DE0` (callbacks) | window stack Q5; finger at the selected row; Yojimbo pay window `0x8A87D0` | H |
| Help bar / target name | `TOBtlCtrlHelpWin` [Fh] `0x8912E0` (ctrl), `0x8936D0` (draw) | fixed screen position | M |
| Enemy info (Sensor/Scan) | `0x893A30(id,0xB3,0xF0)`; flag `[0x133C93E]` set by `0x891480` | fixed position | M |
| Battle message windows | queues `0x133D6F4`, `0x133D708`, `0x133D734`; draws `0x892740`, `0x895660`, `0x897240`; bar bg `0x8A1CE0` | "first strike", "got item"... (`TOBtlDraw*MessageWindow` [Fh] `0x892870..0x895320`) | M |
| Overdrive timer | `TOBtlCtrlLimitTimer` [Fh] `0x891AC0`: state `[0x133F77C]==2`, elapsed `0x133F788`, remaining `0x133F790` | the minigame UIs themselves (Lulu's stick spin, Auron's inputs, Wakka's reels) were **not located**; see the DEAD END on the board | M |
| Auto-battle | `[0x133D6E0]`, toggled in `0x657140` (`flash/autobattle.swf`, `0x6F7700`) | suppresses the turn pointer, CTB, target pointer and help; auto-confirms | M/H |
| Encounter transition (shatter) | `MsBattleExe` [Fh] `0x781020`: `0x7829B0(Btl.screen_transition +0x105)` -> type 0..3 and SE (0x31/0x65/0x3F) -> `0x79E5E0` -> `0x7FC590` -> `0x7FD7F0(bank [0x119FE98], 0, type+8, handle)` | a VFX from a common effect bank (entries 8..11). Effects can sample the "FrameBuffer" texture (`0x7FA510`), presumably the captured screen that shatters. Not drawn by the 2D UI. | M |
| Battle end | `Btl.battle_state` (+0x10) = 0x1F..0x21 stops only the Doom countdown icons in `0x79F660` (its tests jump to the node loop at 0x79F845: the damage digits keep drawing; corrected 6 Nov, dmg3d review S1); `TkBtlEndGetText` [Fh] `0x8BDA60`; results = menu system | not explored further | L |
| Summon / aeon UI | command window type 5 (Summon) and the Aeon rows in the party window; `TkMenuCtrlSummon` [Fh] `0x8CC330` is the field menu | no world-anchored aeon UI found | L |

Chr fields named on the way (offsets in the 0xF90 entity): +0x10 (byte, slot used; gates projection), +0x442 damage-number
element, +0x4E8 damage-number height (stat 261 / 100), +0x4EC limit-bar height (stat 296 / 100), +0xDD9 stat_cursor (stat 82;
=1 means it can be targeted; read by the candidate builder `0x898330` via `0x794CA0`), +0xDF9 stat_cursor_element
(stat 135), +0xF34..+0xF58 screen anchors (Q2), +0xF6C icon pulse phase.

---

## 3. Corrections to what the mod assumes today

1. **Stream 1/2 are the normal (0,0,1) and the colour (0x80/255 = 0.502).** The UVs are a separate float2 stream (v flipped).
   The 22 Oct verdict "UVs not in streams 1-2" is correct, but the reason is now known: they are in another stream.
2. **The target pointer is beside the anchor, not above the root.** Monsters: the quad's left edge is at the anchor.
   Party: the right edge is 32 left of it. Vertically it spans -22..+23 around the anchor. The anchor is the monster's
   cursor sub-position `optpos[Chr+0xDF9]`, which can be anywhere on the body (chest, head, a floating part).
3. **The turn pointer is a world-space triangle** with known geometry, colours and spin angle. It can be reproduced in
   stereo exactly.
4. **`cur_window` is stale outside input** (no clearing on submit). The truth for "the game is drawing pointers now" is
   `[0x23CC08C] != 0` plus a target window open (`+0x02 == 1`, state 3..6) plus not auto-battle.
5. **The `markers_on` "cyan" turn colour is yellow** (archaeologist). The game's own colours are at `[0x133FA88]`/`[0x133F7D4]`.
6. **Damage numbers go through the same 2D submit** on battle.dds and are drawn in the UI pass. The missed picks are pick
   limits (merged draws), not evidence of 3D billboards.

---

## 4. Implications for the cursor architecture

**Hook points (all main thread, inside `0x89B360`):**

| hook | what it gives | risk |
|---|---|---|
| detour `0x89C870` (void, cdecl) | called iff a target window is drawing. At entry: mask `[0x133D178]`, ids, raw anchors Chr+0xF34/F38, 3D anchors optpos[Chr+0xDF9]. Skipping the original removes **every** target-pointer quad | low: 1 caller (stub `0x8A84C0`), read-only |
| detour `0x892590(int chr)` (cdecl) | called iff the turn pointer is due; chr = arg = `[0x23CC088]`; angle `[0x133FA7C]`, colours `[0x133FA88]`/`[0x133F7D4]`, anchor optpos[1] - (0,2,0) | low: 1 call site `0x89B450`, read-only |
| post-hook `0x79F380` (optional) | all per-chr anchors for the frame (cursor plus damage) | low |
| optional: detour `0x893290` | the popup queue (digits, icons) before it is drawn and cleared, to re-place damage numbers in 3D | medium (63-entry queue format known; the per-type layouts need care) |

Snapshot the detour data with the frame counter (`0x23CBBF0`, *[engine]*) and hand it to the render thread (Q9).

- **A — hide plus our 3D markers: feasible, and much better with E2.** Replace the hide list and pointer rule for these two
  cursors with the detours (in the headset, don't call the originals; on the monitor, call them). The markers then come from
  the same truth the game uses: same gate, same mask, same anchor (`optpos[Chr+0xDF9]` for targets, the exact triangle for
  the turn). It needs: the two detours, memory reads of Actor+0x524 (+16*Chr[0xDF9]) and the existing per-eye marker
  pass. Risk: low. This closes conflicts 1, 2, 3, 5, 10 and 11 of cursor-current-state.md.
- **B — game sprite as a 3D billboard: feasible now.** The exact UV rects are known (party (566,523)-(634,568); monster
  (566,463)-(634,508), drawn mirrored) on battle.dds (1024). Either capture the SRV the game binds for its battle.dds draws
  (the texture is identifiable: it is the 1024 BC3 atlas bound for CTB/pointer/digits) or load the .phyre file
  (language-dependent path: `menu/` vs `menu_us/`...). Place the quad in world space next to the anchor with the game's
  offset converted to world units at the anchor's depth (68x45 UI px at the game camera's distance), or keep a fixed
  angular size. The turn pointer needs no art: draw the triangle in 3D with the game's colours and θ. Risk: low/medium
  (art sourcing, a textured shader in the marker pass).
- **C — re-place the game's own quads per eye: feasible but unnecessary.** It needs the UV stream (now known to be the
  float2 stream), aspect correction and depth sizing. Everything C buys, B gets more simply from memory, without
  heuristics. Risk: medium (draw-state capture, batching with neighbours).
- **D — hook world-to-screen: not needed as a hook.** The positions are already stored per frame (Chr+0xF34..F50), and
  the 3D inputs are directly readable (optpos[cursor_element]). Hooking `0x6428F0` itself would affect every caller.
  Use D only as a read of `0x79F380`'s outputs, for validation.
- **E1 — back-project from the game's quad: obsolete.** The anchor is known exactly. If kept as a fallback, use the
  correct offsets (section 3.2).
- **E2 — hide at the source: feasible and recommended** (as detours rather than byte patches: `0x89C870`, `0x892590`).
- **E3 — hide by identity: obsolete** once E2 is in; the detour hides exactly the game's pointers and nothing else.

**Recommendation: E2 + A, with B for the look.** Detour `0x89C870` and `0x892590` on the main thread. In the headset,
skip the originals and publish a frame-stamped snapshot: target mask and ids with
optpos[Chr+0xDF9] anchors; turn chr, θ and colours. The native pass then draws (i) for each target, a battle.dds
hand billboard (or the current triangle until the art is wired) beside the anchor, and (ii) the turn triangle as true 3D
geometry. Remove the size-signature hide list and the pointer rule for these two cursors (keep the rule only as a
diagnostic). On the monitor, call the originals so the flat game is untouched.

---

## 5. Other useful findings for the VR project

- **One choke point for all 2D UI:** `0x63EEE0`. Its caller (return address) identifies the element: `0x903BB0`/`0x903EE0`
  callers include CTB, digits and pointers; `0x901190` is the turn triangle. A detour here could tag sprites by origin, or drop
  or redirect any element **before** it reaches the vertex ring. The texture name comes in as an argument.
- **Every world-anchored battle UI element has a readable 3D source:** cursor anchor `optpos[Chr+0xDF9]`, damage anchor
  `optpos[Chr+0x442] - Chr+0x4E8` (y), gauge anchor `ground - Chr+0x4EC`, turn `optpos[1] - 2`. With these, damage numbers,
  status icons and enemy gauges can all be re-placed in 3D next to the actors, instead of living on the flat panel.
- **Popup queue** `0x133F110` (count `0x133F108`, cleared after `0x893290`): screen-space digits and icons with type,
  value and rgba. It is a natural source for 3D damage numbers.
- **Turn-order preview:** the CTB list at `0x1340478` (16 entries) already includes the pending command. It could be
  mirrored in a VR-side panel.
- **State gates usable by the mod:** `[0x23CC08C]` (input active, chr bitmask), `[0x23CC088]` (inputting chr),
  `[0x133C948]` (battle UI alive), `Btl.battle_state` (`0x112A8E0`; 1 = input/active, 0x1F..0x21 = ending),
  `[0x133D6E0]` (auto-battle), `[0x23CC092]` + `0x133C950` (window stack, valid only while `[0x23CC08C]` != 0).
- **The UI's projection lacks screen shake**, so a world-placed marker from optpos will look steadier than the shaking
  scene does. That is arguably better in VR anyway.
- **The left/right target-cycling direction depends on the camera** (`0x780CE0` picks one of 4 pad maps). If the VR
  camera differs from the game camera, cursor movement may feel mirrored. The table at `0x89D390` local_28 shows the four maps.
- **The game nudges the neck/camera on a target change** (`0x792DE0` -> `0x7969F0` neck look-at, `0x797BC0`, **L**). This
  is relevant to "battle follow".
- **Clipped UI rects at -100000** (`0x8E58E0`): a cheap filter in the UI capture (skip any quad with |x| > 100000).

---

## 6. Corrections after review by exe-verifier (24 Oct, see research/exe-verify.md; spot-checked)

These override the text above where they differ.
1. **`0x898130` is dead code** (no rel32 or absolute refs; I confirmed it in my xref DB). The only clear of the
   0x133D0D0 block is `0x89ABD0` (from `0x89A87A`). **H**
2. **More writers of `0x133D178`:** `0x89C9AE` (in `0x89C900`), `0x89CE85` (`0x89CDE0`), `0x8A241A` (`0x8A22F0`) and `0x8A27BA` (`0x8A2690`).
   Each sets it to `1<<chr` for self-target commands, with no target window open. None of them clears it.
   "Not cleared on submit" stands, and so does "the mask alone is not a draw gate". **H**
3. `0x8975A0` sets `[0x23CC088] = 0xFF` only when `[0x133D6E0] == 0`. **Under auto-battle the id stays set.** **H**
4. **The Chr initialiser `0x79C110` writes `Chr+0xDF9 = 1`** (`0x79C1F4`), so the default cursor anchor is **optpos[1]**,
   the same point the turn pointer uses. Scripts override it with stat 135. **H**
5. Behind the camera (w<0), `0x6526A0` negates x/w and y/w (`0x65281C..0x652834`), so the result is x/|w|, not mirrored.
   The pointer is still wrong in that case, because `0x89C870` does no w check. (Corrects Q7's "mirrored".) **H**
6. Turn-pointer colours are **H**: vertex 0 (apex) = `[0x133FA88]`, vertices 1-2 = `[0x133F7D4]`.
7. Q2 table: `0x79F380` writes only F34..F50. F54/F58 come from `0x795250`.
8. **Important for section 4: optpos (+0x524) is not smoothed by FFX-60fps.** It is computed in the JobWorker job
   `0x833530 -> 0x8350F0` from the logic-tick root. SMOOTH-002 interpolates only actor +0x1D0. So a marker anchored at
   optpos steps at the logic rate (30 Hz) relative to the smoothed (60 fps) bodies. The game's own 2D pointer has the same
   stepping (it projects the same optpos). In VR it is more visible. **Mitigation:** anchor at
   `rendered_root + (optpos[idx] - logic_root)`, where rendered_root is actor +0x1D0 (or the mod's `g_world_live` root) and
   logic_root is actor +0x0C. Or low-pass the anchor over 2 frames. **M** (the verifier's code reading; I have not re-traced the job).
9. **Hook details:** Fahrenheit's generated handle for `FUN_00492590` uses `d_UnknownFn`, which takes no arguments. The
   detour needs its own cdecl `void(int chr)` delegate, or calling the original passes a garbage chr. Skipping `0x89C870`
   also changes the batching and index counts of the neighbouring battle.dds draws (CTB and digits merge differently).
   That is harmless for the game, but any D3D-side signature rules keyed on those draws will see different index counts.

### 6.1 Anchor formula, refined (exe-verifier, 24 Oct; M, not re-traced by me)
This replaces the mitigation in 6.8.
- **Reference point:** optpos is built from the canonical root, whose translation is `chr_pos_vec (+0x0C) + chr_offset_vec (+0x6C)`
  (`0x832760`). Use `anchor = rendered_root(+0x1D0) + (optpos[idx] - (+0x0C + +0x6C))`.
- **Rotation and scale:** that formula is translation-only. The exact form is `optpos x inv(root_tick) x root_drawn`, with
  root_tick rebuilt from scale +0x5C, heading +0x158 and the translation above. You cannot read root_tick from +0x1D0 at
  detour time on logic frames, because it already holds SMOOTH-002's lerp. The translation-only form is fine except
  during fast spins.
- **Attached actors** (+0x19C != 0): +0x1D0 comes from the parent's bone, so use the raw optpos.
- **Sanity check:** with SMOOTH off, or on render-only frames, the correction should come out at about zero.
- **Simplest robust alternative:** use the raw optpos plus a 2-frame low-pass, or snap to the mod's rendered skinned root
  (`g_world_live`) for x/z and keep the optpos height above the root.
