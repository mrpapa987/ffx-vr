# FFX.exe engine notes for VR (exe-engine, 24 Oct 2026)

Static analysis only. Sources: the Ghidra export `re/out` (decomp chunks, functions.tsv, strings.tsv), the
objdump text `ffxexe/text.txt` (same exe, `cmp` identical), Fahrenheit `re/fahrenheit-src` (commit ecb6702),
FFX-60fps source `ffx60/repo` (Apache-2.0, read for reference). My helper scripts are in
`research/exe-engine-work/` (`fn.py ADDR` prints a function's decompile, callers and callees; `rd.py` reads
constants from the PE).

**Addresses.** "gva" = VA with ImageBase 0x400000 (PE header confirmed). **FhUtil** = gva - 0x400000 (what
`FhUtil.get_at` and `gva_to_rt` use). Example: gva 0x2311440 is FhUtil 0x1F11440.
**Confidence.** [code] = the decompile or disassembly says so; [infer] = my reading of it; [ext] = from
FFX-60fps or Fahrenheit source; [runtime?] = needs an in-game check.

---

## 0. The one thing that changes several plans: there are two threads (plus job workers)

> **Correction (exe-verifier, research/exe-verify.md; spot-checked):** there are also **JobWorker** threads (string
> 0xB4CF38, loop 0x703A20). Ch_CalcMain 0x832E10 hands the per-actor update 0x833530 to them (0xA44B90 -> 0x71E720)
> without waiting. That job writes actor +0x1D0 (0x832760) and optpos +0x524 (0x8350F0). Joins (0x6DBF50): the
> next Ch_CalcMain (0xA44B80), the end of Ch_DrawMain (0x82F881, conditional), Sg_MainLoop 0x821656 (after the
> battle-UI draw) and the renderer 0x71E810. So reads of +0x1D0/optpos taken after Sg_MainLoop 0x821656 are
> joined. Reads taken earlier in the loop are only as safe as the game's own (0x79F380). The exe is DYNAMIC_BASE
> (DllCharacteristics 0x8140): use FhUtil offsets only.


| What | Where | Evidence |
|---|---|---|
| **PhyreEngine render thread** "PhyreEngineRenderThread" / "RenderThread" | created by 0x615140 (FhUtil 0x215140) at framework init 0x62A600; thread proc 0x614710 -> loop 0x616010 | strings 0xB3F14C / 0xB3F100 (xref 0x615140, 0x616010); `push 0x614710` at 0x6151A3 [code] |
| Command-list ring (16 entries of 0x14 bytes) | object = return of 0x614410 = gva 0xCC96F8; +0x0C state (1 running, 2 quit); entries at +0x10 + i*0x14 {+0 list head, +4 list end, +8 "ready" event, +0x10 done-semaphore ptr}; +0x150 consumer index, +0x154 producer index; +0x15C free-slot semaphore (16/16), +0x16C pool of 16 sync events (mask +0x18C) | 0x615140, 0x616010, 0x615210 [code] |
| Producer (main thread) "kick" | 0x615210(list, end, done_sem): waits for a free slot, fills the entry, SetEvent. Called from the context's vtable method 0x5B6C50 (context +0x4C/+0x50 list, +0x44 -> semaphore at +0xE0 (16/16), which caps lists in flight at 16, not 1) | disasm 0x615210..0x61525B, 0x5B6C50..0x5B6C99, 0x5B8DA0 [code] |
| Consumer | 0x616010: wait for the entry's event (INFINITE), run the interpreter 0x615550 on it, release the done semaphore | [code] |
| Full flush ("wait for the render thread to go idle") | 0x615EA0: queues a list holding only command 0x2C (callback 0x614700 = SetEvent) and waits for it. Wrappers 0x5BC3F0 (14 callers incl. the PApplication step 0x627940 before resize), 0x638F90, 0x638FB0 | [code] |
| **Present** | render command **0x16** in the interpreter switch 0x614C60 -> 0x593760: `swapchain->Present(sync = [r+0x131C] != 0, 0)`. Encoded on the main thread by 0x5B6AF0 (writes 0x16 into the current list) | [code] |

The main thread (`MainThread`, named in 0x62A600) runs the game: PApplication step 0x627940 -> animate
(vtable +0x10 = 0x42F520 -> Sg_MainLoop runner 0x822840 -> **Sg_MainLoop 0x820AE0**) -> viewport renders ->
render (vtable +0x20 = 0x42F930 -> 0x642BB0 -> **renderer frame 0x668930**) -> encode Present 0x5B6AF0 -> kick
-> post (vtable +0x28 = 0x42F4F0 -> `graphicDynGeoMemManagerSwapBuffer` 0x63FF70(1)). App vtable at gva
0xB0D998. [code]

**Consequences** [infer, high]:
- Every D3D11 call we hook (DrawIndexed, Map, UpdateSubresource, OMSet*, **Present**) runs on the render thread.
  `ffxvr_begin_frame`, OpenXR, ImGui and Fahrenheit's ImGui also run there. The game logic does not.
- While the render thread executes frame n, the main thread may already be building frame n+1 (or more; up to
  16 lists can be in flight). So **game memory read at Present** (actor table, BtlWindow masks, Chr+0xF34,
  0x2311440) may belong to a later frame than the one being drawn. The camera we capture from constant-buffer
  uploads is safe: it travels inside the command list.
- A synchronous load on the main thread leaves the render thread **idle**, blocked in WaitForSingleObject at
  0x616010. That is why the headset goes black: no list, no Present, no `xr_tick`.
- The main thread sometimes drives the immediate context itself after a full flush. Example: resize 0x599210
  (OMSetRenderTargets(0), ResizeBuffers, GetBuffer, recreating the backbuffer RT at 0xCA310C and depth at 0xCA347C),
  called from 0x62C260/0x62C440 after 0x5BC3F0. So a few of our D3D hooks can fire on the main thread. [code
  for the resize path; other main-thread context users are not enumerated]

---

## 1. Cameras

### 1.1 The per-frame build (all modes: field, battle, events)
Order inside Sg_MainLoop's draw section (0x8211D2..): for viewport i (loop `0x7BC110(i)`, i = 0..2, twice over):

| Step | gva (FhUtil) | What | Conf. |
|---|---|---|---|
| Viewport block | 0x1137000 (0xD37000) + vp*0x1C0 | +0xC0 enabled byte, +0xC1 camera index, +0x10/+0x14/+0x1C fov terms (below), +0x30/+0x50/+0x70 screen-offset pairs, +0x88 "special mode" float, tween slots +0xDC (4 x 0x10, 0x7BDA80) | [code] 0x7BC110 |
| Camera block | **0x1137540 (0xD37540) + idx*0xDA0** (FFX-60fps "cam_blocks" 0x1137570 is +0x30 into it) | look-at inputs: eye +0x65C, target +0x40, up/base +0x00, roll +0xC90; +0xCA8..+0xCB4 fog/colour change set | [code] call `0x7C1BE0(&0x2311440, blk+0x65C, blk+0x40, blk, blk[+0xC90])`; eye/target roles [infer] from the look-at maths |
| Camera finalize | **0x7BC110** (0x3BC110) | picks the source: `[0x231142C]` (src2, a script look-at from globals 0x1136FC8/FD8/FE8, roll 0x1136FC4), `[0x23114C4]` (src3 = the field map camera 0x920C00, which commits its own view), else the block look-at | [code] |
| VIEW matrix | **0x2311440 (0x1F11440)**, 64 B, PS2 convention, row vectors, world->camera | written by 0x7C1BE0; getter 0x7C0070 | [code] |
| VIEW->SCREEN matrix | **0x2311480 (0x1F11480)** = PS2 "view screen" (pixels; scale = focal; centre 2048-style constants at 0xC0A004) | 0x7C3D60(&0x2311480, viewport block); getter 0x7BBA50 | [code] |
| Copies for the frame | 0x230FCA0 (view) and 0x230FC60 (screen); 0x230FF60 = view x screen (0x762550) | Sg_MainLoop 0x8211E0.. | [code] |
| Screen offset (shake / scroll) | renderer **+0xFA34/+0xFA38** = ((vp+0x70 + vp+0x30 - vp+0x50 + 0x1136FB8) * 2 / 512, (vp+0x74 - vp+0x34 - vp+0x54 + 0x1136FBC) * 2 / 416); 0x1136FB8/FBC written by 0x7BC720 (script) and the field camera 0x920C00 (its block +0xB8/+0xBC) | 0x642D60, 0x642D40 | [code] |
| FOV / aspect | **0x7BC980** MsCommitCameraFovAspect: `vfov = 2*atan( 448 / (2*vp[+0x14]*vp[+0x1C]) )` (alt. x0.75 when 0x1136FC0 == 2), `aspect = (512/(2*vp[+0x10])) / (448/(2*vp[+0x14]))` = 4:3 at the nominal 0.8 / 0.9333 | disasm 0x7BC980..0x7BCA24, constants 512 / 448 / 2.0 [code] |
| Commit | 0x6395E0 -> 0x66CDA0 (fov -> cam+0x154; per-map overrides via r+0xFAF4: 0xEC1 -> 0.471, 0x107/0xF48 for the second camera), 0x6395C0 -> 0x66CBE0 (aspect -> cam+0x150; **x16/9 / (4/3) when byte 0xCCB9C0 != 0** = Hor+ widescreen), then **0x639860**: inverse of 0x2311440 with X/Y negated (PS2 -> Phyre), stored as 3x4 at **renderer +0xFA04** | [code] |

Renderer = `[0xCCB9D8]` (FhUtil 0x8CB9D8). Main camera = `[r+0xDC]`, a `PCameraPerspective` (class size
0x158). Field offsets come from the reflection table 0x4537D0 (+4 for the vtable) [code]:

| cam + | field |
|---|---|
| 0x04 | m_viewMatrix (4x4) |
| 0x34 | m_projectionMatrix |
| 0x74 | m_viewProjectionMatrix (the frustum every cull uses, known) |
| 0xB4 | m_localToWorldMatrix **pointer**; 0x668930 sets it to r+0xFA04 every frame |
| 0xB8 / 0xBC | near / far = **1.0 / 65536.0** (0x65F450 init; setters 0x645F00 near (used by 0x7BC110 on one map/spell), 0x645E90 far) |
| 0x150 / 0x154 | aspect / vertical fov (radians; the default from the constructor is 0.785398) |

`0x6720C0(cam)` = "VP = inverse(*localToWorld) x proj". Perspective builder 0x456B80: GL style,
m10 = (n+f)/(n-f), m11 = -1, m14 = 2nf/(n-f) (clip z in -1..1). [code]

**GPU camera** (render, main thread, after Sg_MainLoop): 0x6507D0 folds the screen offset r+0xFA34/38 into
cam+0x34 as a clip-space translation. If `r+0xF9C7 == 0 && [0xCCB9C0] == 0` it also applies the 4:3
pan-and-scan crop (y*4/3, y offset 1/3 - 2*r+0xF9BC), then recomputes the VP (0x6720C0). [code]

**4:3 pan-and-scan maps** [code + infer]: byte 0xCCB9C0 = 0 on the 30 map ids in the table at 0xB421C8
(0x65CBF0; also cleared by 0x64DC00 and 0x65D240, set by 0x66FAD0). r+0xF9BC = the vertical crop position;
0x65D860 computes it each frame by projecting the followed point (0x6526A0 mode 0), clamps it to [0.035, 0.295],
with per-map special cases keyed by **r+0xFAF4 = current map id** (getter 0x65DE90 / 0x641DA0). This is the
remaster's vertical scroll over painted 4:3 backgrounds. **VR relevance**: on those maps the game's projection
is not a plain perspective (it has an extra y scale and a moving y offset), which matters to anything that
decodes the game projection (g_cam FOV estimates, auto-depth, the stereo screen).

### 1.2 World-to-screen for UI
- PS2-style path [code]: `0x7C0070(out)` = VIEW, `0x7BBA50(out)` = SCREEN, `0x7059B0(out, screen, view)` =
  multiply. Used by 0x890B20/0x890B70 (battle UI area), 0x7614A0, 0x761C40, 0x716AC0 (op_ot_draw), 0x800670,
  0x925440, 0x729AB0, 0x711910. Output in PS2 screen pixels. Remap helpers: graphicUiRemapX2 0x6447E0 = x*512/1920,
  RemapY2 0x644820 = y*416/1080 (0x644800 = y*416/720).
- HD path (found by exe-battle-ui, consistent with the above): 0x6428F0 -> **0x6526A0**(renderer, out, world,
  mode). Mode 1: if `[0xCCB9C0] == 0` it rebuilds the VP from inverse(*cam+0xB4) (= the current r+0xFA04) and
  0x651650 (cam+0x34 with the crop using F9BC+F9C0); otherwise it uses **cam+0x74 directly**. Output = 1920x1080
  centred via 0x640DB0; returns 1/w. [code]
- cam+0x74 at UI time [code, high]: the camera's virtual +4 is **0x456D10** (vtable at gva 0xB11240:
  [0] 0x456500, [1] 0x456D10). It rebuilds a clean cam+0x34 from fov/aspect/near/far (0x456B80) on every
  fov/aspect set in 0x7BC980. Then, still in Sg_MainLoop's draw section, 0x711910 -> 0x639590 -> **0x65E660**
  sets cam+0xB4 = r+0xFA04 and recomputes cam+0x74 (0x6720C0) = **this frame's (interpolated) view x the clean
  projection**. 0x65E660 returns a copy with shake + crop added for its own caller; cam+0x74 itself stays clean.
  This runs only when [0x12FB8F9] == 0; the other branch is 0x761C40. So both UI paths project with the
  current view and **no screen shake**; the GPU projection gets the shake later in 0x6507D0. UI anchors
  therefore sit still while the scene shakes (they lag the shake, not the camera).

### 1.3 30 Hz logic vs drawn frames (FFX-60fps) [ext + code]
- `Sg_GetCurExecFrames` 0x82D7E0 returns the logic ticks to run this invocation (exec byte **0xC496D4**, FhUtil
  0x8C96D4). FFX-60fps pacing sets it to 1 on logic invocations and 0 on render-only ones (pacing.cpp). So at
  Present, `exec byte == 0` = "this frame was render-only" (but see the race in section 0).
- SMOOTH-001 patches the three CALLs to 0x7BC110 (sites 0x821189, 0x82135E, 0x821380). On each pass it writes
  lerp(tick k-1, tick k, t) (t = (pass+1)/N) into 0x2311440, re-commits it (0x639860) and also interpolates the
  FOV fields. It **restores the canonical view at the next invocation's pre()**, so for the whole invocation
  (the UI code and the render 0x668930 included) the game sees the interpolated view. Cuts: > 500 units or
  60 degrees, plus a "jump" rule (4x the previous step and > 20 units / 10 degrees).
- SMOOTH-002 interpolates actor **+0x1D0 (root matrix, row-vector, translation at floats 12..14)** in a stub
  around Ch_DrawMain, with the restore again deferred to the next pre(). **Actor +0x0C (chr_pos_vec) is never
  touched: it stays 30 Hz.** SMOOTH-003 sub-steps the skeleton on render-only passes (Ch_StepMotion 0x838E16
  `SAR EDI,1` patch).
- The master switch `SMOOTH` defaults to 0 in code but is 1 in the shipped fps60.ini; F7 toggles it at runtime.
- **+0x1D0 is not just +0x0C** [code, confirmed in 0x832760, where the actor is addressed as short*]: the
  translation = chr_pos_vec +0x0C **plus chr_offset_vec +0x6C**; the rotation and scale come from +0xAC/+0x5C;
  a matrix at +0x10C replaces it when set; attached actors (+0x19C) derive it from the parent's bone
  (+0x328 + idx*0x160 + 0x88). So +0x1D0 differs from +0x0C even without FFX-60fps.
- **First-person anchor** [infer, high]: the interpolated value the GPU draws is actor+0x1D0 *during and after*
  the draw of invocation k, until invocation k+1's pre(). The 30 Hz steps come from reading +0x0C. Read +0x1D0
  instead and snapshot it on the main thread (Fahrenheit post-main-loop hook, or a hook on Sg_MainLoop's exit)
  together with 0x2311440, tagged with the frame counter 0x23CBBF0. Sg_MainLoop exit comes after the job
  join at 0x821656, so it is race-free there. Then use the snapshot whose Present is
  being executed (section 5, "pairing").

### 1.4 Other camera facts
- Spell-specific camera cases in the finalize: `0x9DA3B0()` = `[0xC64CA0]` = Fahrenheit `Magic.current_id`
  (FhUtil 0x864CA0). Ids 0x98 and 0x259 use viewport 1 with a split camera (0x639B30); 0x21C changes the near
  plane. [code]
- r+0x10D8C ("debug camera mode", known) is also set in normal play: 0x7BC980 -> 0x638D40 sets it when viewport
  float +0x88 == 256.0, and the camera +0x238 then gets its own frustum (0x638AA0). So "debug mode" is really an
  alternate-camera mode some scenes use. [code]
- The script camera opcodes are Atel namespace 0x6000..0x6089 (Fahrenheit `call_atel.cs` 1808..; handler table
  FhUtil 0x843988, gva 0xC43988); handlers 0x7B8330..0x7B9CA0. [ext]
- Blitzball: same pipeline. The game camera is the director camera the stereo screen uses.

---

## 2. Frame and render loop

### 2.1 Sg_MainLoop 0x820AE0 (main thread), simplified [code]
1. `graphicVideoUpdate` 0x645DF0(dt) (FMV), 0x6411E0 (== 2: an early-out 2D/loading mode).
2. Backlog / skip-render flags 0x12FB7D0/D4/D8 (set while catching up).
3. Logic ticks x N (0x82D7E0): 0x82D180, 0x790C10 (battle), 0x88D3D0, 0x8AB910 (menu logic), op_loop_ffx_before
   0x7FF770, 0x787E00, 0x7BE110, 0x91A8C0, **Ch_CalcMain 0x832E10**, 0x83F2E0.
4. Draw build (if not skip-render): for each viewport, 0x7BC110 finalize -> view/screen copies ->
   0x90A950/0x90A980/0x90A140 (field), 0x835EB0/0x835E90 (characters) -> 0x82C7B0(vp) -> field pass 0 (0x908480)
   -> **Ch_DrawMain 0x82ED50** -> field pass 1 (0x9084A0). Pass gates 0x12FBBB1/B2/B6/B9/BA (known).
5. 0x843260/0x8432D0/0x843360, 0x8223A0, `graphicDrawMainMenuWaterEffect` 0x63E920 (when [0x25D5EBC] != 0).
6. While 0x12FBBB8 is set: `op_loop_ffx_after` 0x7FF680, 0x780D00 (battle), 0x88D4F0, 0x7EB580, **op_ot_draw
   0x7EC470** (4 overlay layers of registered callbacks, below).
7. 0x76EF80, 0x82C0C0, 0x82CE90, field 0x908550, event 0x872CA0, **menu / battle-UI draw 0x8ABD30** (0x8AB580
   while skip-render), 0x88D4E0, 0x7E6560, 0x8207F0, 0x7FF670.
8. Frame counter **0x23CBBF0**++ (FhUtil 0x1FCBBF0), 0x9DAC60, 0x63D9C0, ...
All of this only **records** draws (Phyre nodes, dyn-geo meshes, 2D sprites). The D3D work comes later, on the
render thread.

### 2.2 Renderer frame 0x668930 (main thread, records command lists) [code, partial]
- At the top: bind the default RT, full viewport, command 6 (0x5BADA0, colour 0,0,0,1), depth/blend defaults;
  **cam+0xB4 = r+0xFA04** (and the 2nd/3rd/debug cameras to r+0xFA3C / r+0x7B0 / r+0x10D94).
- **FMV branch**: `sFMVPlayerManager` = [0xCDED3C] (getter 0x6D73A0). If it is playing (byte +0x6D2 or +0x6D0)
  and [0x112A008] != 0: draw the video quad (0x6E7F00 -> 0x692C40 into the backbuffer), the Iggy overlay
  0xA267F0, **return**. No camera, no scene, no UI pass. [code]
- An early return when [0x134082C] != 0 (0x653060).
- Otherwise the scene passes (0x6507D0 camera, node lists, shadow / reflection targets via 0x5BB130 set-RT and
  0x5BB5B0), post (0x6C57E0.., 0x6C3580..), the 2D lists (0x6953A0, 0x6B6xxx / 0x6BAxxx = per-layer sprite
  lists), the loading icon 0x6E4EA0, Iggy 0xA267F0. I did not map every pass; the census already gives the D3D
  order.

### 2.3 Why 2D-only frames have no clear and no camera [infer]
FMV, title and Iggy screens take early branches of 0x668930 that never run the scene path (which is where the
depth clear that opens our UI pass lives). The FMV frame draws one video quad straight into the backbuffer.

### 2.4 Loading stalls: what the engine already has [code]
- The PS2-era synchronous read loop **0x76E660** (libmscd.c, "read_task_buff"): while reads are pending it polls
  the read event. **Only if `[[0xCC9CD8]+0x3A0] != 0`** (and renderer bytes +0x1196/+0x1197 are 0) does it call
  0x642C00 -> **0x66B1E0**: that function draws the loading-icon / Iggy layer (0x6E50A0, 0x6E4EA0), encodes a
  Present (0x5B6AF0), kicks, pumps window messages and **Sleep(60)**. Otherwise it blocks in
  WaitForSingleObject(INFINITE) (0x6F2D80) and nothing is presented.
- [0xCC9CD8] = the PApplication singleton (0x623B90). Its +0x3A0 is the "present while loading" flag: set by
  0x66FB30 and by 0x643500 (show the loading/autosave icon, [0xCE517C] = icon object), cleared by 0x66FB50 and
  at the end of boot (0x6416C0).
- So the autosave chocobo and the boot load present frames, but ordinary loads (battle start, the airship menu)
  do not. [infer: which loop the 1.6 s stall is in is **runtime?**: battle loading is the per-frame state machine
  MsBtlReadManage 0x783020 (async reads through the loader vtable 0x76D0D0), so the long frame may be Phyre asset
  or texture creation rather than 0x76E660.]

---

## 3. UI system (my part; exe-battle-ui owns the battle widgets)
- Menu logic runs in PS2 space 512x416 (RemapX2/Y2 above). Sprites go through `TOMkpShapeXYWHUV` 0x903BB0
  (texture id -> UV normalisation via 0x8AC400 / 0x8AC8C0; ids 400.., 16000 = font, 0x3E82/0x3E83/0x3E86
  special sheets) into **0x63EEE0**, the central quad submit. It has 4 verts of x,y,u,v + rgba (0x80 = 1.0) and
  batches per texture name through 0x67BA50 ("NoTexture" for untextured). exe-battle-ui decoded the streams:
  [3] pos, [4] normal, [5] colour float4, [6] UV float2, [7] u16 indices. [code]
- When `[0xCCC838]` (Iggy/flash UI player) has state < 2, 0x63EEE0 routes the quad to 0x63E930 instead (Iggy-era
  path). 0x63EEE0 also drops every sprite on skip-render frames ([0x12FB7D0] != 0). [code]
- **Batch manager** (`this` of 0x67BA50) [code]: two frame-alternating pools; +0x5B7C flips 0/1 at the start of
  each Sg_MainLoop (0x642320 -> 0x67DFC0) and is reset; the flush is at the end (0x6423B0 -> 0x67FAA0). Each pool has
  a "Single_TextureName" list (+0x15E4 / +0x423C: one texture per mesh, up to 12 quads, a new mesh on a texture
  change) and a "Multi_TextureName" list (+0x15EC / +0x4244: **up to 6 textures per mesh**, +0x5844 = textures
  used). Multi mode (+0x5B8C) is switched on and off around **text** by 0x643AF0(1/0) -> 0x67E4D0. Its callers
  are the font makers 0x905AB0 (ToMakeBtlEasyFont), 0x905970 (ToMakeBtlEasyEdgeFont), 0x904BA0, 0x8E4AF0,
  0x8E4E60, 0x8E75C0, 0x8E92A0, 0x8EC2F0, 0x8ECA90, 0x8FC7C0. So a glyph mesh binds several textures, and SRV0 is
  not necessarily the glyph's sheet. There is **no per-sprite layer or pass**: param_5 of 0x63EEE0 (stored at
  record [10]) is 0 from both 0x903BB0 and 0x903EE0, so draw order = submit order within the frame's pool.
- **Damage digits** (answer to exe-battle-ui) [code]: 0x893290 -> 0x8FB840 (digits 0..9) / 0x8FBCC0 (popup ids
  0x3A..0x3D) -> TOMkpShapeXYWHUVC2 **0x903EE0** -> `0x63EEE0(quad, tex 400 = battle.dds, 1, 0, 0)`. That is the
  same call, arguments and batch class as the target pointer (0x903BB0 -> `0x63EEE0(.., 1, 0, 0)`). Upstream:
  MsNumberRegist 0x79FA00 -> 0x79FC60 fills a 0x80 x 0x14 pool at [0x11360B8]. The code gives no reason for them
  to be outside the UI pass: same pool, same pass, early in the battle UI order, merged into one battle.dds mesh
  with neighbouring battle.dds sprites (12 quads max). [infer] So the missed picks are more likely the pick's
  limits (the first quad or SRV0 of a merged draw, the 32-quad read, short-lived digits) than a different pass.
  One F9 pick on a frame with digits, listing every quad of each battle.dds draw, settles it [runtime?].
- **D3D input slots** of the UI meshes [doc + code]: slot 0 = position float3 (stride 12, z = 1), slot 1 = normal
  float3 (0,0,1), slot 2 = colour float4 (byte/255; 0x80 = 0.5), slot 3 = UV float2 (stride 8, v flipped =
  1 - v). Slots 0 and 3 are from the mod's own runtime picks (design-notes "Cursor vertices" and "Blitzball,
  second session": "stream 3, stride 8: the atlas UVs"). The order and contents match the CPU record at mesh+0x94
  ([3] pos stride 0xC, [5] colour stride 0x10, then normal/UV) written by 0x63EEE0. The Phyre vertex declaration
  itself was not decoded.
- Dyn-geo double buffer: `graphicDynGeoMemManagerSwapBuffer` 0x63FF70 at the end of each PApplication step
  (0x42F4F0) and at the first draw frame (Sg_MainLoop). Two segments alternate per frame (FFX-60fps
  DIAG-DYNGEO). [code + ext]
- **Dialogue windows**: TOMesWinWork array **0x1326D70** (FhUtil 0xF26D70), 8 windows x 0x2C per bank, bank index
  u16 at 0x1326B82 (getter 0x86BE90 = Fahrenheit AtelGetMesWinWork). The script opcode handler **0x8580C0** sets
  +0x00 x (s16), +0x02 y (s16), +0x1C anchor byte, in 512x416 space, with hard-coded per-event fixes (event_id
  0x17, 0xAB, 0xD4). +0x08/+0x0C text pointer (0x857870/0x857A00/0x857C60), +0x10, +0x18, +0x20 misc. So the
  window position is **authored per script call; the engine does not know the speaker**. [code]
  Anchoring near the NPC: the speaker is most likely the actor owning the Atel worker that issued the call
  (Fahrenheit `Atel.current_worker` FhUtil 0xF270E4, gva 0x13270E4). [infer]
- Save screen: Fahrenheit's own ImGui (known). The game's save state words are at FhUtil 0x8CB994 / 0x8CB998
  (Fahrenheit save_pal.cs). [ext]
- Iggy (RAD Game Tools flash UI, `iggy_w32.dll`, 40 imports): the loading icon and the chocobo autosave movie
  (0x66F480). It draws in a separate pass (0xA267F0 / 0x6E4EA0) that does not go through 0x63EEE0. [code]

---

## 4. FMV, blitzball, input, identity, skeleton

**FMV** [code]: `PhyFMVPlayerManager` (Virtuos), singleton [0xCDED3C]; the video list is
`/FFX_Data/GameData/PS3Data/Video/<region>/FFX_VideoList.txt` (0x6D82F0); "start to play video:%s" is at 0x6D7430;
decoded frames go to a texture through updateTextureCallback (0x6D71B0) and are drawn by `Video.cgfx` (0x6DA300).
`graphicInitFMVPlayer` 0x641690, `graphicVideoUpdate` 0x645DF0. Separate **in-scene texture videos**
(texVideoLuca, texVideoLchb, 0x66FB70: the Luca screens). **VR**: the video frame is a texture bound for one
full-screen draw. Grabbing that SRV gives the movie at native resolution for a cinema quad, independent of the
backbuffer.

**Blitzball** [code + infer]: no new camera code. The likely home of the camera-less overlay run is
**op_ot_draw 0x7EC470**: four layers (0, 0x20, 0x40, 0x60) of registered draw callbacks, slot order at
**0x11A0960** (FhUtil 0xDA0960, 0x80 bytes, 0xFF = free), entries at **0x11A09E0** (FhUtil 0xDA09E0) of 0x10 bytes
{fn, arg1, arg2, u16 lifetime}, registered by 0x7EB500 (through 0x7EB300/350/3A0/3F0, called via pointer
tables). It is drawn after the 3D scene and before the menu draw. That fits the "NO IMAGE" scene draws before the
UI pass. **Test**: dump that table during a match; the callbacks name the scoreboard and radar draws. Low-medium
until the dump is done.

**Input** [code]: Phyre XInput pad 0x62DBA0: `XInputGetState([pad+0x3B0])`. Sticks are stored as int >> 7 at
+0x370/+0x374 (L x/y) and +0x37C/+0x380 (R, y negated); buttons as bytes +0x398..+0x3A7. The rumble is sent
**every poll**: `XInputSetState(motors = pad+0x58/+0x59 bytes * 65535)`. Setter 0x62CB70, gated by the
vibration option in 0x6329C0 (called from 0x630E20). The game-side PS2 pad word: Fahrenheit FhInput raw = FhUtil
**0xF270C0** (gva 0x13270C0), u16 PS2 bits (L2 1, R2 2, L1 4, R1 8, square 0x10, confirm 0x20, cancel 0x40,
triangle 0x80, select 0x100, start 0x800, up 0x1000, right 0x2000, down 0x4000, left 0x8000). Pad bits getter
0x888D70(0,0); Sg_MainLoop tests 0x8000|0x100|0x20 (a soft-reset chord). DirectInput8 is created in 0x624350, called
from the framework init 0x62A600 right after the render thread starts (keyboard/mouse). [ext for the raw word]

**Identity** [code + ext]: event_id 0x12FBC40 (known); map name 0x23CBC60 (known); **map id r+0xFAF4** (0x641DA0);
scenario (story) flag u16 **0x112D67C** (getScenerioFlag 0x787360; Sg_MainLoop logs changes); current spell id
0xC64CA0; field-object list ctx 0x1876E00 (known); sphere grid open flag FhUtil 0x1685F70.

**Skeleton** [ext + known]: actor +0x1D0 root (interpolated while drawn), +0x328 part matrices (stride 0x160,
known), Fahrenheit `chr_neck_matrix_0/1` +0x42C/+0x46C (battle targeting only), neck limits +0x414..+0x428,
chr_optpos +0x524 (26 vec4; battle cursor anchors index into it, see exe-battle-ui). Motion clock fields
+0x714..+0x7DC (FFX-60fps SMOOTH-003: M_START 0x718, M_END 0x71C, M_CLOCK 0x740, M_DONE 0x7DC; header +0x1C0,
dirty +0x878). GPU bone palette = `BoneTransforms` float4x4[80] (known).

---

## 5. Implications for the remaining fix plan

**Presenter thread.** Engine view [infer, high]:
- Our OpenXR loop already lives on a thread that is *not* the logic thread (the Phyre render thread). The stall
  is the main thread failing to feed it. A presenter thread still needs to exist, because the render thread
  sleeps INFINITE in 0x616010 while the main thread loads.
- **A cheaper first milestone, no second device**: make the game present during loads itself. Set
  `[[0xCC9CD8]+0x3A0] = 1` (and patch the `push 0x3C` Sleep in 0x66B1E0 down to a few ms) so that 0x76E660 runs
  0x66B1E0 frames. Each one reaches our Present hook with no scene draws, and R8b's hold re-submits the last
  eye pair with its own poses (head-tracked reprojection). It uses the game's own synchronisation (the frame is a
  normal command list). **Caveat**: it only helps stalls inside 0x76E660. Before building, run one stall capture:
  log `[[0xCC9CD8]+0x3A0]` and the main thread's EIP from a watchdog (SuspendThread/GetThreadContext, no
  exceptions, so it is safe under the stage-0 debugger) when no Present has arrived for 300 ms.
- If the stall is elsewhere: the full design (own D3D11 device, shared textures) stays right. Do NOT run
  OpenXR/D3D work on the game's immediate context from a timed-out render-thread wait. The main thread may own
  the context at that moment (the resize path, other post-flush users).
- Pacing: the main thread may be several frames ahead of the render thread (16-deep ring). The presenter design's
  "game waits on a frame event" must block the *main* thread (e.g. in the PApplication step), not the render
  thread, or it will add latency instead of removing it.

**R8b (hold last frame)**: compatible, and needed by the option above. Treat "Present with zero scene draws and
no camera upload since the last Present" as "no new image". The loading-pump frames (0x66B1E0) and FMV frames
both look like that; FMV must still go to the virtual screen, so check the FMV singleton bytes ([0xCDED3C]+0x6D0 /
+0x6D2) to tell them apart.

**R11 (eye-2 bindings, backbuffer sentinel)**: facts for the design [code]:
- The swapchain has BufferCount 1, SwapEffect DISCARD, flags 2 (ALLOW_MODE_SWITCH), format 0x1C (RGBA8 sRGB),
  and SampleDesc.Count from the 4th argument of 0x595800 (1 from its only caller: the backbuffer is never MSAA;
  MSAA lives on offscreen targets). Device flags 0 (multithread-safe device, single immediate context).
- The backbuffer is re-acquired in **0x599210 on the main thread** after a full flush: OMSetRenderTargets(0),
  release views, ResizeBuffers, GetBuffer, new RT object at 0xCA310C, depth at 0xCA347C. Our OMSet hooks see that
  call on the main thread; the sentinel must not assume the render thread. A cheap hard signal: hook 0x599210
  (or the vtable call ResizeBuffers) and invalidate g_backbuffer and the twins there.
- FMV frames draw straight into the backbuffer (0x692C40); eye 2 should not be re-issued for them.

**R25 (double copy, six swapchains)**: nothing engine-side blocks it. The eye images are ours; the game's
backbuffer format is sRGB (0x1C), so the copy-shader sRGB path in G4-final is the matching one.

**R17 / R23**: engine facts only. Per frame, the main thread submits the whole scene again even on render-only
FFX-60fps passes (the node lists are rebuilt), so identical eye-2 passes (shadows, reflections) really recur
each frame. R23's counter should show it. The blitz 4096x4096 targets are render targets the scene path binds
via 0x5BB130.

**Anchor / markers (for the battle work)**: read actor **+0x1D0** for drawn positions. Read selection masks
and Chr+0xF34 on the main thread (snapshot at Sg_MainLoop exit). Pair a snapshot with a Present by sequence:
count Present encodes on the main thread (hook 0x5B6AF0, which also catches the loading-pump frames) and
Presents executed on the render thread; the ring is FIFO.

---

## 6. VR opportunities, ranked by value / effort

| # | Idea | Value | Effort | Basis |
|---|---|---|---|---|
| 1 | **Smooth first-person / follow anchor**: read actor +0x1D0 (drawn root) instead of +0x0C, snapshotted at main-loop exit | high (removes 30 Hz eye steps) | S | 1.3 [ext+code] |
| 2 | **Loading-black fix v0**: force app+0x3A0 during loads + R8b hold; one diagnostic run first (watchdog EIP) | high | S-M | 2.4 [code], stall location runtime? |
| 3 | **Rumble -> Quest haptics**: IAT-hook XInputSetState (motors already 0..65535, sent every poll; respects the in-game vibration option) | medium | S | 4 Input [code] |
| 4 | **FMV at native resolution on the cinema quad**: grab the Video.cgfx SRV during the one FMV draw; detect FMV via [0xCDED3C]+0x6D0/+0x6D2 | medium | S-M | 2.2, 4 FMV |
| 5 | **Drop the shake in VR, keep it optional**: the shake is only the clip-space translation r+0xFA34/38 (and 0x1136FB8/FBC). Our override already discards it; for the stereo-screen or seat modes, zero r+0xFA34/38 after 0x642D60 (or skip the 0x6507D0 fold) for a comfort toggle | medium (comfort) | S | 1.1 [code] |
| 6 | **Know when the camera cut**: hook 0x7BC110 exit (as FFX-60fps does) for a main-thread, per-viewport view with the source tag (src2 script look-at / src3 field camera / block look-at) and the camera index (vp+0xC1); a cut = index or source change, or the FFX-60fps jump rule. Better than inferring cuts from CB bytes | medium | S | 1.1 |
| 7 | **Pan-and-scan maps**: on maps with 0xCCB9C0 == 0, get the vertical crop r+0xF9BC so the headset projection and the stereo screen can undo or match it (and the painted-backdrop rooms) | medium (correctness on 30 maps) | S | 1.1 |
| 8 | **Dialogue near the speaker**: TOMesWinWork 0x1326D70 has the authored position and the open state; tie a window to the actor of the issuing Atel worker and place the panel there; keep the authored offset as the fallback | medium | M (speaker heuristic needs runtime checks) | 3 |
| 9 | **Blitzball overlay**: dump op_ot layers (0x11A09E0) during a match to name the scoreboard/radar callbacks, then route exactly those draws to the UI layer instead of the "first blended draw after the resolve" heuristic | medium | S to find, M to route | 4 |
| 10 | **Spell / summon camera awareness**: [0xC64CA0] (current spell id) and r+0x10D8C (alternate-camera mode) tell when a spell takes over the camera; good triggers for "director" policy or a blink | low-medium | S | 1.4 |
| 11 | **Driving the engine camera** (r+0xFA04 / 0x2311440 after finalize) so culling, UI projection and lighting follow the head | high for free-look, but every subsystem (UI anchors, battle UI pointers) would move with it | M-L | 1.1-1.2 |

**Must avoid** [infer]:
- Using the immediate context from any thread but the render thread (the main thread uses it after flushes).
- Treating game memory read at Present as belonging to the frame being presented.
- Writing 0x2311440 / r+0xFA04 from the render thread (the main thread owns them; FFX-60fps already rewrites them).
- Forcing app+0x3A0 outside loads: 0x66B1E0 pumps window messages and sleeps; leave it to the read loop.
