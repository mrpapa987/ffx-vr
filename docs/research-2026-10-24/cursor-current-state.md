# Battle cursors in the mod today: current state, conflicts, options (cursor-archaeologist, 24 Oct 2026)

Sources: the work repo at build 6 + review fixes (`git 9b097c3`):
`src/native/ffxvr_native.cpp` (cited as N:line), `src/managed/module.cs` (M:line), `src/native/ffxvr_xr.cpp`
(X:line), `docs/design-notes.md` (DN:line); Fahrenheit structs in
`fahrenheit/src/core/ffx/battle/btlui.cs`, `chr.cs`, `gstate.cs`.
There was no EXE work here. Confidence tags: **[code]** = the code says so; **[doc]** = the notes record a runtime
observation; **[infer]** = my reading, not verified.
Addresses: gva = Ghidra VA with ImageBase 0x400000 (confirmed: `objdump -p FFX.exe` gives ImageBase 00400000);
`FhUtil` offset = gva - 0x400000.

---

## (a) The pipeline today, step by step

### What the game draws [doc]
- **Target pointer:** one quad from the **1024x1024 BC3 atlas** (the same atlas as the CTB portraits and menu
  backgrounds), **68x45 UI units**, often batched with other sprites in one DrawIndexed (seen: 12 indices
  spanning 1200x130). UI space is a centred 1920x1080 layout, y down. (DN:875-881, 943-947, 968-972)
- **Turn pointer** (over the acting party member): a **textureless 6-index quad**. Its width runs 15..40 within a
  battle (a "spinning billboard" whose width is the visible spin), its height 27..40, and the size changes between
  battles, so it scales with the game camera's distance. (DN:956-960, 993-996)
- Both are drawn in the **UI pass**: textured quads from a 12 MB ring vertex buffer, slot-0 stream stride 12
  (x y z), ortho cb0 (the same for every UI draw), stream 1 reportedly "(0,0,1)/(0.5,0.5,1) for every sprite",
  stream 2 a colour, no PS constant buffers. The atlas UV source is **unknown**. (DN:984-989; but see conflict 7:
  that reading predates the R48 fix.)
- Not the cursor, but nearby: the CTB panel (a 128x288 quad of the 1024 sheet, portraits 70x46 at x 692..802,
  rows 48 apart), the enemy-name bar (1600x55 strip, 8192x4096 sheet), the command cursor (a textureless ~20-unit
  triangle). Damage numbers are **not** UI-pass draws (seven missed picks; thought to be scene-pass draws on the
  2048x4096 font sheets). (DN:865-881, 947-955, 979-983, 1014-1018)

### What we read from game memory (managed, every frame, Present thread)
`render_imgui_inner` M:2090 -> `draw_markers` M:1976 -> `draw_markers_inner` M:1998 -> `mark` M:2047 -> `push_markers` M:1970.
1. Battle gate: `Globals.Battle.btl` (FhUtil 0xD2A8D0, gva 0x112A8D0), `battle_state` (+0x10) != 0. M:2001-2002 [code]
2. Current window: `Globals.Battle.cur_window` = `windows` (FhUtil 0xF3C950, gva 0x133C950, stride 0xF0) +
   byte at FhUtil 0x1FCC092 (gva 0x23CC092). M:2004 [code, Fahrenheit gstate.cs:51-52]
3. **Targets** = `BtlWindow.sel_chrs` (+0xD8, u16) | `sel_mons` (+0xDA, u16) << 16, a mask over combatant ids
   (`Chr.id` +0xC); every party entity (18 slots, FhUtil 0xD334D4) and monster entity (16 slots, 0xD34468) whose id
   bit is set gets a red marker (0xFF4040FF). M:2035-2039 [code]
4. **Turn** = the party entity whose `Chr.id == BtlWindow.cur_chr_id` (+0x08) gets a cyan marker (0xFF40E0FF);
   party only, first match. M:2040-2044 [code]
5. **Not checked:** `BtlWindow.state` (+0x01), `type`, whether a target-selection sub-window is open, or whether
   the game is drawing a pointer at all. The marker is drawn whenever the mask bit is set. [code] (Whether the
   mask and cur_chr_id are cleared when the game hides its pointers is unknown. See Q5/Q6.)
6. **Marker position** (`mark` M:2054-2060): the actor-table position `Actor.chr_pos_vec` (+0xC; the logic position,
   30 Hz) for x/z; the y is the "head" = the lowest actor-space Y among the 26 `chr_optpos` sub-positions (+0x524)
   within 400 units of the root, else root - 60, plus the `_marker_height` slider (default 6). Converted to render
   space (negate X and Y). [code]
7. Two outputs from the same point: (i) `_mk_pos/_mk_col` -> `ffxvr_markers_set` N:5324 (alpha 0xFE in the colour
   tags the turn kind, N:5326; managed never sets 0xFE, see conflict 9); (ii) a **2D ImGui triangle + text label**
   on the foreground draw list, projected by `ffxvr_project` N:1779 (the centre-eye override view when the
   override is on, else the game camera). Neither depends on the headset; nothing checks `_xr_on`. M:2063-2077 [code]

### What we detect and hide (native, UI pass, per draw)
Managed `h_drawindexed` M:627 -> native `ffxvr_on_draw` N:3920 -> (hide block N:3989-4069) -> managed `draw_pieces` M:675.
Gating: `hide_cursor` (signature list) and `hide_pointers` (pointer rule) are pushed **on only in battle with
the headset on** (`push_view_rules` M:1952-1953). The hide block runs for UI-pass draws (`g_ui_draw_now`, N:3965)
or flat-mode UI draws (N:3971), and only for DrawIndexed with an index count that is a multiple of 6 (N:3999).
1. **One shared vertex read per draw** (`ui_quads_cached` N:2037 -> `ui_sprite_xy` N:2151): up to 32 quads
   (`k_ui_max_q` N:593), read from the stream's last Map pointer (VirtualQuery-checked, SEH-guarded), starting at
   base vertex + start/6*4 (`ui_first_v` N:609; R48 in build 6). A start that is not a multiple of 6 is not read,
   so that draw gets no hiding. [code]
2. **Hide list** (signature: texture size, quad w/h ranges; 16 entries; ini `hide_list`): defaults
   `{1024,1024, 66-69 x 43-46}` (target pointer) and `{textureless, 8-36 x 24-34}` (turn pointer) (M:97). It
   matches **anywhere on screen**, with no position test (N:4002-4020). A hit also asks the pointer rule which actor
   the quad is over, but only to tag it for the art capture (N:4015-4016). [code]
3. **Pointer rule** (`quad_is_pointer` N:2109), for quads the list did not take, on textureless or 1024x1024 draws
   (N:4023-4034):
   - `actors_project_ui` N:2052, once per frame at the first UI draw: projects `g_world_live` (translations of
     the skinned world matrices seen in this frame's scene pass, `record_buffer` N:2996-3005: cb of 1872..16383
     bytes, matrix at +640) through **`g_cam`** = the **game's own** view/projection, recorded from the game's
     upload bytes before our override rewrites them (`camera_from_write` N:2970, `ffxvr_on_updatesub` N:4239 records
     and then calls `maybe_override`). [code]
   - A quad is a pointer if it is 1..90 wide, 8..90 high, **not inside a window background** recorded *earlier this
     frame* (quads >= 100x100 from the 1024/8192 sheets or textureless; `ui_backgrounds_note` N:2071,
     `quad_on_window` N:2083), its centre is within 55 units of an actor's projected column, and its bottom is
     30 below to 420 above the projected root. It must also not be part of a run of glyphs (`quad_in_run` N:2096).
     The nearest actor is returned as an index into `g_world_live` (R44). [code]
4. Hidden quads -> runs -> up to 8 skip ranges (N:4041-4050; runs past the 8th stay visible). All quads hidden
   -> the draw is skipped (return -1). Managed draws the gaps (`draw_pieces` M:675), for the UI texture and for
   any eye-2 reissue. [code]
5. **Art capture** for every quad that is really hidden (N:4051-4056): `ptr_sprite_capture` N:637, only when
   `markers_game_art` is on (`g_ptr_reuse`). [code]
6. Old experiment knobs still live: `ui_hide` 0-5 (hide every 1024 draw, every textureless draw, ...)
   N:3992/4061, M:1947-1950, default 0. [code]
7. Diagnostics on the same data: hover highlight (`ui_hover_collect` N:2123, F11; red = the pointer rule would
   hide it, green cross = projected actor root), F9 pick (`ui_pick_check` N:2173), census, atlas dump
   (N:3977-3988). [code]

### What we draw instead (native, at Present)
`ffxvr_begin_frame` N:3691 -> `xr_tick` N:5736 -> (only on a frame with a captured UI pass, N:5752/5762-5764)
`draw_markers_both` N:5332 -> `draw_markers_eye` N:5252 per eye -> `ptr_sprites_release` N:3742.
1. The eye images are `g_eye1_final` / `g_eye2_final`: the scene resolved **before** the UI pass (`ui_pass_begin`
   N:4458-4475). The UI pass goes to `g_ui_tex`, a **mono** quad layer (X:646). So the game's 2D cursors live on
   the flat panel; our markers go into the stereo projection layer, beneath the panel. [code]
2. Per eye: V from `build_override_view` on `g_cam` (eye shift hack 0 / 1e-6, N:5337-5338), P from the eye's FOV
   tangents (N:5255-5259). [code]
3. `draw_ptr_sprites_eye` N:5166 (if game art is on): for each captured sprite with a world position, the **nearest
   marker within 40 units in the ground plane**, not already used. Projects the marker through this eye's VP into
   "UI units" (NDC * 960/540), builds a quad of the **captured UI size**, bottom-centred on that point, binds the
   game's VS/PS/layout/blend/cb0/SRV/sampler and streams 1-7 at the quad's offset, and draws 6 indices
   `0 1 2 2 1 3`. No depth. [code]
4. Markers that got no sprite: a camera-facing downward triangle of size `2.5 * marker_scale`, drawn twice for
   both windings, with no depth (z forced to mid-range), alpha-blended (N:5265-5308). [code]

### Battle follow and anchor (context, same memory) [code]
`push_player` M:1813 follows a chr id from the **field roster** (FhUtil 0xD2C895, 3 bytes) and resolves it to an
actor through the party entity list. `push_battle_anchor` M:1788 uses the centroid of the party and visible
monsters (actor table). Neither uses the selection mask. The pointer rule runs on rendered skinned roots instead.

---

## (b) Where we fight ourselves

1. **Two sources of truth for "who is targeted / whose turn".** Our markers come from game memory
   (`sel_chrs|sel_mons`, `cur_chr_id`, M:2035-2044). The hiding comes from the geometry of what the game drew
   (hide list + pointer rule, N:3999-4034). The two are **never cross-checked** [code]. Failure modes [infer]:
   - The game draws a pointer and the rule hides it, but the mask has no bit for that actor (or the art pairs with
     no marker): the pointer **vanishes** in the headset with no replacement.
   - The mask has a bit but the game shows no pointer (outside target selection, during animations, a stale mask):
     **our marker shows when the game's would not**. We never test `BtlWindow.state` or `type`.
   - The rule misses a pointer (start index not on a quad, more than 32 quads, more than 8 runs, the window-background
     test, the 90-unit cap when the camera is close): **both** the flat game pointer on the panel **and** our 3D marker.
2. **Two sources of actor positions.** Markers use the actor table (logic position, 30 Hz, plus the chr_optpos head
   heuristic). The pointer rule and the art capture use `g_world_live` (rendered skinned roots, interpolated by
   FFX-60fps). They are reconciled by "nearest within 40 units" (N:5202-5203). The doc notes that the rendered
   model can be one logic step away from the stored position (N:570-573). [code]
3. **Three projections of the same idea.** The game's camera (`actors_project_ui`, mirrors the game), the centre-eye
   override (`ffxvr_project` -> the ImGui triangles), and the per-eye override (`draw_markers_both`). Only the
   last is right for the headset. [code]
4. **Duplicate 2D markers.** `mark()` always draws an ImGui triangle and label (M:2072-2076). On the monitor
   (headset off) the game's own cursor is **not** hidden (headset-only gating, M:1952-1953), so with `markers_on`
   (default true) the monitor shows the game cursor plus our triangle. In the headset the same triangles land in
   the ImGui layer, which is shown on a panel when the overlay is open or there are 12 or more ImGui draws
   (N:5758, M:1617). That is a third, flat copy at a centre-eye position on a panel. [code; visibility in the
   headset is an inference]
5. **Two hide mechanisms that overlap.** The size list (default on) predates the position rule and covers the
   same two pointers, but with **no position test**:
   - any 1024-atlas quad of 66-69 x 43-46 anywhere is hidden. The CTB portrait is 70x46, so the margin is 1 unit
     (that already bit once, DN:1010-1012);
   - any textureless quad of 8-36 x 24-34 anywhere in battle is hidden (risk: small textureless menu elements; the
     command-cursor triangle is "~20 units", borderline [infer]).
   The pointer rule's own guard against windows depends on draw order: a background counts only if it was drawn
   **earlier in the frame** (N:2073-2085). [code]
6. **The art experiment re-uses what we hide and then throws away where the game put it.** Art is captured only
   for quads that the hide rules cut out (N:4051), so it is coupled to the heuristics. The game's own placement
   (the quad's UI position = the game's anchor through its camera) is discarded. The art is re-placed at **our**
   memory-derived marker instead (N:5202-5209), so it needs both sources of truth to agree.
7. **Why the experiment "drew nothing" is a bug story, not a UV story.** The 22 Oct verdict was "UVs are not in
   streams 1-2" (DN:1037-1043). But:
   - until build 3 the captured sprites were released at the top of `begin_frame`, **before** `xr_tick` could draw
     them (R43). The sprite's actor index pointed into the wrong list (R44). The reissue had never run (DN:1616-1620);
   - the stream dump behind "streams 1 and 2 hold the same values for every sprite" read **"the first quad" at the
     bound offset**, ignoring base vertex and quad index (baseline `ui_pick_check`: `ssrc = p + soff`; census
     `streams:` likewise). Build 6 (R48) fixed every reader to start at base vertex + quad. A batched ring-buffer
     draw read that way returns the same vertices for every sprite. **That observation is not evidence**. It needs
     one fresh F9 pick on build 6 (it dumps streams 1-5 at the picked quad's own vertices, N:2224-2253). [code; the
     conclusion is my inference]
8. **The art path would still be wrong after the fixes** [infer from code]:
   - Aspect: the game's VS with its 1920x1080 ortho cb0 maps UI units onto the 16:9 eye image. With the headset FOV
     on (`g_xr_use_fov`, N:5850) that image covers a near-square FOV, so a 68x45 sprite comes out at about half its
     width relative to its height.
   - Fixed screen size, taken from the game camera's distance (not ours).
   - Only VS cb0, PS SRV 0 and sampler 0 are captured (N:641-644). If the UV or atlas rect lives in a VS SRV,
     VS cb1+ or a PS cb, the reissue samples whatever is bound at Present.
   - At most 6 sprites (N:625): an "all enemies" target gets a mix of art and triangles.
   - The pairing ignores kind: a target sprite can take the turn marker on the same actor.
9. **The kind tag is dead.** Native decodes alpha 0xFE as the turn kind (N:5326), but managed always passes alpha
   0xFF (M:2042), and native overwrites alpha to 0xFF anyway. `Marker.kind` is never read. Side note: the turn
   colour 0xFF40E0FF decodes as R=FF G=E0 B=40 in both ImGui (ABGR) and the native unpack (N:5271). That is
   **yellow**, not the "cyan" the notes and brief describe. [code]
10. **The marker height is a guess, and the game knows the answer.** The "head" heuristic + clearance slider
   (M:2054-2056) versus the game's own anchor. Our rule's tolerance (bottom 30 below to 420 above the root) shows
   we do not know the game's anchor. Yet every frame we hold the game's quad and the game's camera, which is enough
   to back-project its anchor (see E1). [code/infer]
11. **Markers only on captured-UI frames** (N:5762-5764): a frame whose UI pass is not recognised has no markers,
   while its game cursors (if any) were not hidden either. [code]

---

## (c) Data we have vs. lack, per architecture

| | Have | Lack |
|---|---|---|
| **A** hide + our 3D markers (default) | Target mask, cur_chr_id, actor positions (actor table and skinned roots), game camera, per-eye views. A working hide pipeline (list + rule + ranges). A working marker pass. | The truth of when the game shows a pointer (gate on window state?). The game's anchor point (head heuristic instead). A look that matches the game (triangle, no art). Confidence that the hide rules have no collateral. |
| **B** game sprite as a 3D billboard | The atlas texture at runtime (pointer known per draw; "Dump UI atlases" writes it as .dds, N:1990). The quad's UI size (68x45). The marker pass to extend (needs a textured shader + sampler). The turn pointer is textureless, so it is just a coloured quad with a width oscillation: reproducible from captured w/h and stream-2 colour. | **The atlas UV rect of the target pointer** (never extracted. The dump-and-cut plan of 22 Oct was not done). Whether the art animates (frames? palette?). The turn pointer's colour/gradient and spin law. Licensing aside, a shipped cut-out avoids runtime UVs. |
| **C** game draws, we re-place its quads per eye | Per quad: its UI rect, the actor it is over (rule), the game camera, the full draw state (capture path exists and now runs). The ability to draw it per eye in the projection layer (draw_ptr_sprites_eye). An option to back-project the anchor: the ray from the game camera through the quad's bottom-centre, intersected with the actor's vertical axis, gives the game's 3D anchor height without EXE work. | Proof that the captured streams carry the UVs (one build-6 pick). Aspect correction (scale x by the eye/16:9 tangent ratio, or substitute our own VS with the game's PS/SRV). Depth sizing (convert UI size to world size at the game camera's distance, then project per eye). A mono UI panel cannot carry disparity: per-eye placement has to be in the eye images (as now) or a per-eye quad layer pair (as the stereo screen does). |
| **D** hook the game's world-to-screen | Nothing yet. The game projects on the CPU (the quads arrive in screen units in the ring). The camera blocks are known (battle camera blocks gva 0x1137570; renderer camera +0xDC, VP at +0x74). | The function, its callers, its input 3D point and output struct. Even with it, the UI pass is drawn once into one texture, so the hook gives one position per frame. It is best used to **read** the anchor and identity (feeding A/B/C), not to make the game draw per eye, unless the UI pass is re-issued per eye (heavy). |
| **E1** "the game's quad is the source of truth" | Everything in C. Drop the memory mask for *placement*: each detected pointer becomes a 3D marker at its back-projected anchor, coloured by kind (1024 sprite = target, textureless = turn). | Accuracy of the rule (false positives become false 3D markers). Kind is inferred from the texture. |
| **E2** hide at the source | | An EXE switch or call site to skip drawing the pointers (a byte flag or an early return in the pointer draw routine). It would replace all D3D heuristics with one patch (`g_patches[]` facility). Needs the routine from Q1/Q4. |
| **E3** selection from memory, hiding by identity | Mask + cur_chr_id. | Tie each hidden quad to the mask (hide only quads over actors the mask names, plus the turn actor). It closes conflict 1 for free, with what we already have. |

My read [infer]: the cheapest consistent design is **A + E3 + E1's anchor**, with B for the look. Hide only
pointer quads over actors the game memory names. Place our marker at the anchor back-projected from the game's
own quad (falling back to the head heuristic). Draw the art from a cut-out texture (B) or the reissue (C) once
the UV question is settled. Q1-Q5 below decide whether an EXE hook (D/E2) makes the heuristics unnecessary.

---

## (d) Questions for the EXE reverse engineers (exe-battle-ui)

Addresses we already have (gva / FhUtil): Btl 0x112A8D0 / 0xD2A8D0 (battle_state +0x10); BtlWindow array
0x133C950 / 0xF3C950, stride 0xF0, current index byte 0x23CC092 / 0x1FCC092; BtlWindow +0x01 state, +0x06 type,
+0x08 cur_chr_id, +0xD8 sel_chrs, +0xDA sel_mons; party/monster entity list pointers 0x11334D4 / 0x1134468
(entities 0xF90 bytes, Chr +0x0 actor*, +0xC id, +0xE chr_id, +0x16 stat_visible); field roster 0x112C895;
actor table [0x23C44E4], stride 0x880 (pos +0xC, sub-positions +0x524); renderer [0xCCB9D8], camera +0xDC (VP +0x74);
battle camera blocks 0x1137570.

1. **Target pointer placement.** Which function emits the target-pointer sprite (1024-atlas quad, 68x45 UI units)?
   Which 3D point does it project (actor root + a fixed offset? a per-monster cursor height in the entity or the
   monster data? a bone or sub-position index into +0x524?), and through which camera (renderer +0xDC / +0x74,
   the battle camera block, or a copy taken at another time)? Give the projection function's address and its
   output space (PS2 pixels, then scaled to 1920x1080?).
2. **Shared world-to-screen for battle UI.** Is there one project-to-screen helper used by the target pointer, the
   turn pointer, damage numbers, "Miss", status pop-ups, and enemy names or help? List its callers. Does each caller
   store the screen x/y in a struct we could read (or overwrite) before the sprite is written to the vertex ring?
   Are damage numbers drawn in screen space at all, or as 3D billboards in the scene pass? Our picks say they are
   not UI-pass draws.
3. **Sprite atlas UVs and texture.** How does the 2D sprite batcher pass the atlas rectangle to the GPU: a vertex
   stream (which slot/offset), a VS constant or texture buffer, or computed in the shader from an id? Where is the
   sprite table (sprite id -> texture page, u, v, w, h) for the target pointer, and which file in the game data holds
   that atlas page, so we can load the exact art ourselves?
4. **Turn pointer.** Which function draws the textureless pointer over the acting character? How is the spin made
   (width = w0*|cos(angle)| on the CPU? where is the angle kept and advanced?), what sets its colour and size (the
   camera distance?), which 3D point anchors it, and what gates its visibility (window state, cur_chr_id, a flag)?
5. **Selection state.** Who writes BtlWindow +0xD8/+0xDA (sel_chrs/sel_mons) and +0x08 (cur_chr_id), and when are
   they cleared? Is there a separate "cursor is on X right now" state (the highlighted candidate while choosing)
   versus the confirmed command target? Which `state` / `type` / sub-window values mean "the target pointer is
   being drawn"? Is `cur_chr_id` stale during monster turns?
6. **A source-level off switch (E2).** Is there a flag, or a single call site, that suppresses the pointer draws
   (a debug flag near 0x112A8F8, or a test at the top of the pointer routine) that we could patch, instead of
   hiding quads in D3D?
7. **Multi-target and batching.** For "all enemies" targeting, does the game draw one pointer per target in one
   loop? Is that loop the one that also emits neighbouring sprites into the same batch (it explains the 12-index
   batched draw)? Is a pointer ever drawn for an actor not in the sel mask (e.g., the highlighted candidate)?
8. **CTB panel.** Which function draws the CTB list (portraits on the 1024 atlas, 70x46)? Where is the turn-order
   array it reads? Does it mark the selected target's portrait (a highlight we would want to keep or mirror)?
9. **Camera snapshot timing.** Does the UI projection use the same view/projection the GPU receives in the per-object
   constant buffers (what our `g_cam` records), or an interpolated or earlier copy (FFX-60fps interpolates)? A
   one-frame skew would explain any pointer-rule misses on moving cameras.
10. **The 66/54-index far-away pair** (DN:867, 881-882: first vertices at (-376000, -260200) in UI units). Which
   routine emits it, and is it projected through the battle camera (a world-anchored UI element behind the camera
   or off-screen)?
