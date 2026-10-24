# Battle cursors and the FFX.exe deep dive: synthesis and plan (24 Oct 2026)

For the owner. Plain language first, technical detail after. This combines `cursor-current-state.md`,
`exe-battle-ui.md`, `exe-engine.md`, `exe-verify.md` (the verifier's corrections win wherever the authors
differ) and `BOARD.md`, plus a few checks of my own (marked **[synth]**). Addresses are `VA / FhUtil`
(FhUtil = VA - 0x400000, what `FhUtil.get_at` and `FhCallLocation("FFX.exe", off)` take). The exe is
DYNAMIC_BASE, so only FhUtil offsets are safe at runtime. Nothing has been run in game. Every runtime claim
below is a prediction with a test attached.

---

## In short

- **The fight:** we hide the game's cursors by guessing from shapes on screen. Separately, we draw our own
  markers from memory the game never clears, at a height we invent. The game-art experiment then tried to
  stitch the two guesses back together.
- **The fix:** the game draws each of its two battle cursors in one small, self-contained function, once per
  frame, on its own thread. It hands that function exactly who is pointed at and where. We hook those two
  functions:
  - **headset on:** we write down what the game was about to draw and skip it. Our own markers (red target
    triangle, your current turn marker) go in 3D at the exact points the game points at.
  - **monitor:** the game draws as normal.
  - No more guessing from shapes, and no stale selection.
- **Plan:** a log-only diagnostic build first. Then the switch-over, with a one-line way back. Then the game's
  own look as an option. Cleanup comes last. Some of the remaining fix builds slot in between (order in
  section 5).
- **Bonus finds:**
  - the first-person / follow position steps at 30 Hz. A smoother value exists, and it is a strong candidate
    for the walking ghosting.
  - a game flag that makes the game itself show frames during some loads.
  - rumble can reach the Quest controllers.
  - the FMV can be grabbed at native resolution.
  - correction: the game's backbuffer format is UNORM, not sRGB.

---

## 1. Diagnosis: why the current approach fights itself

1. **Two sources of truth that never meet.** Our markers decide who is targeted from game memory (the
   window's `sel_chrs|sel_mons` mask and `cur_chr_id`). The hiding decides what to remove from the *shapes*
   of quads on screen (a size list plus a "small quad near an actor" rule). Nothing compares the two. So a
   pointer can vanish with no marker, a marker can appear where the game shows none, or both can show when
   the rule misses.
2. **The memory we read goes stale.** The game never clears the selection when you confirm a command. The
   window's mask and chr id keep their values through enemy turns and animations (verified: no write clears
   them; the only reset is at the next party input). The game's own gate is different: "a party member is
   choosing a command" (`[0x23CC08C] != 0`) and a target window is drawing. We never test either.
3. **We assumed the wrong anchor.**
   - The game points at a per-monster "cursor element": one of the actor's 26 sub-positions
     (`optpos[Chr+0xDF9]`), default index 1, which monster scripts can change (battle stat 135).
   - Its hand sprite sits *beside* that point, not above the root. Party targets get the hand to the left,
     monster targets to the right, mirrored.
   - Our pointer rule looked for a quad above the actor's root column, and our marker uses a guessed "head"
     plus a slider.
4. **The hidden-quad experiment ("markers_game_art") was built on the previous three.**
   - It captured the hand only when the guess hid it.
   - It threw away where the game had put it, then re-placed it at our guessed marker.
   - It needed the UVs, and they are in a fourth vertex stream (slot 3), not in streams 1-2. Streams 1-2 are
     a constant normal and the colour.
   - So it could not have worked as built, and aspect, size and pairing problems were waiting behind that.
5. **We fight it in the wrong place.** We undo the game's drawing after the fact, in D3D, with heuristics.
   The game has exactly two small, read-only functions that draw these cursors:
   - `0x89C870` for the target hand and `0x892590` for the turn triangle;
   - each has one call site and runs on the game's main thread;
   - at that moment they hold everything we want: who, where, colours, spin angle.
   Hooking them replaces every heuristic with the game's own answer.
6. **We also read at the wrong time.** All our memory reads happen at Present, on the render thread. The
   game's main thread may already be building the next frame, so what we read can be a frame ahead of the
   image being shown.

---

## 2. Recommended battle-cursor architecture

### 2.1 The two hooks (install in `init()`, main thread at runtime)

| | Target pointer (the hand) | Turn pointer (spinning triangle) |
|---|---|---|
| Function | `0x89C870` / FhUtil **0x49C870** | `0x892590` / FhUtil **0x492590** |
| Signature | `cdecl void()` (Fahrenheit's `d_UnknownFn` is right) | **`cdecl void(int chr)`**. Fahrenheit's generated handle uses `d_UnknownFn` and is **wrong**: calling the original through it would read a garbage chr. Declare `[UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate void d_TurnPtr(int chr)` |
| Caller | stub `0x8A84C0` (window +0x90 draw callback) from the window loop in `0x89B360`; may run once per window with state >= 3 (in practice one). Deduplicate per frame | one call site `0x89B450`, chr = `(short)[0x23CC088]` |
| Game's gate (already applied when it is called) | battle UI alive `[0x133C948]`, battle_state ∈ {1,2}, `[0x23CC08C] != 0`, `[0xC58EF8] == 1`; inside: returns at once under auto-battle (`[0x133D6E0] == 1`) | same globals, plus not auto-battle (checked by the caller) |
| Prologue | `55 8B EC 83 EC 0C`: safe for MinHook | `55 8B EC 83 EC 28`: safe |
| Skipping the original loses | only the hand quads (all callees are read-only or draw-only) | only the triangle (plus a private scratch global); the spin is advanced elsewhere, so it does not freeze |
| FFX-60fps | no row touches it | no row touches it |

Install with `new FhCallHandle<d>(new FhCallLocation("FFX.exe", 0x49C870)).hook(this, handler)` at module
`init()`. Fahrenheit forbids adding a hook late to an already-hooked function. (Our D3D hooks go in late
because nothing else hooks those functions.)

**Headset on (`_xr_on` and the new setting `cursor_source = hooks`):** record the snapshot and do NOT call
the original. **Otherwise:** record the snapshot (cheap, useful for logs) and call the original, so the
monitor game is untouched.

### 2.2 The snapshot (written in the detour, read at Present)

Per call, copy values (never keep pointers):

- **stamp:** the game frame counter `[FhUtil 0x1FCBBF0]`. At this point it has not yet been incremented for
  this invocation; the increment comes later, at `0x821617`.
- **targets** (target detour): `mask = [FhUtil 0xF3D178] & 0x0FFFFFFF` (bits 0..0x1B = Chr ids; party
  0..0x13, monsters 0x14..0x1B). This is the *highlighted candidate* mask, exactly what the game draws. For
  each set bit i:
  - `Chr = [FhUtil 0xD334D4] + i*0xF90`, `actor = Chr+0x0`;
  - `idx = (signed char)Chr[+0xDF9]`; `kind = i < 0x14 ? party : monster`;
  - `optpos[idx]` = `actor + 0x524 + 16*idx`, with the game's own fallbacks (`0x8354F0`): all zero → `optpos[0]`;
    `optpos[0].w == 0` → `actor+0x200..0x208` (the root translation, see below);
  - logic root `T_logic = actor+0x0C + actor+0x6C` (chr_pos_vec + chr_offset_vec), drawn root
    `T_drawn = actor+0x200..+0x208` (floats 12..14 of the +0x1D0 matrix);
  - flags: attached `actor+0x19C != 0`, override matrix `actor+0x10C != 0`;
  - for validation only: the game's own screen anchor `Chr+0xF34/+0xF38` (ints, 1920x1080 centred).
  - If `Chr.actor == 0`, the game uses another position (`0x794C20`). We skip that target and count it.
- **turn** (turn detour): `chr` (the argument), `θ = [FhUtil 0xF3FA7C]`, apex colour `[FhUtil 0xF3FA88]`,
  base colour `[FhUtil 0xF3F7D4]` (RGBA bytes, 0x80 = full), the same anchor data for `optpos[1]` of that
  chr's actor.

**Hand-off:** a two-slot seqlock in managed code. The main thread writes slot `seq&1`, then
`Volatile.Write(seq+1)`. The render thread copies the slot and re-checks `seq`. **Expiry:** at Present a
snapshot counts as live only while its stamp is within N frames of the current game frame counter (N measured
in stage 1; expected 1-2). So when the game stops drawing a cursor (command confirmed, enemy turn,
auto-battle), our marker disappears within a frame or two. That fixes the stale-selection bug by design.

### 2.3 The anchor formula (and how sure we are)

The game's anchor `optpos[idx]` is computed at 30 Hz from the logic-tick root, inside a job on a JobWorker
thread. FFX-60fps smooths only the root matrix at actor +0x1D0, not optpos. So a marker placed at raw optpos
(as the game's own 2D pointer is) steps at 30 Hz against bodies drawn at 60. That is barely visible on a
monitor and more visible in VR.

**Stage 2 formula (translation-only):**

```
T_logic = actor+0x0C.xyz + actor+0x6C.xyz        (chr_pos_vec + chr_offset_vec; what 0x832760 builds the root from)
T_drawn = actor+0x200, +0x204, +0x208            (translation of the +0x1D0 root matrix, as drawn this frame)
anchor  = optpos[idx].xyz - T_logic + T_drawn    if actor+0x19C == 0 and actor+0x10C == 0
anchor  = optpos[idx].xyz                        otherwise (attached actor / override matrix: +0x1D0 is not built from +0x0C)
turn:     A = anchor(optpos[1]); A.y -= 2.0; apex = A + (0, -0.3, 0); base = A + (±1 rotated by θ, -2.3, 0)  (game y points down)
render space = (-x, -y, z)
```

**Exact form:** `anchor = optpos · inv(M_tick) · M_drawn` (row vectors). `M_tick` is rebuilt the way
`0x832760` builds it:
- scale `+0x5C..+0x64`
- heading `+0x158` (I confirmed both reads in `0x832760`) **[synth]**
- translation `T_logic`

`M_tick` cannot be read back from +0x1D0, because on logic frames that already holds the FFX-60fps lerp.

**Confidence and uncertainty:**
- **High:**
  - the anchor index and its fallbacks;
  - the default index 1;
  - the triangle geometry, colours and spin;
  - `T_logic` = +0x0C + +0x6C;
  - the attached / override cases.
- **Medium:**
  - that +0x1D0 holds the lerped root at detour time. SMOOTH-002 writes the lerp around Ch_DrawMain and
    restores it only at the next invocation's `pre()`; our detours run after Ch_DrawMain. This is read from
    the FFX-60fps source, not its DLL;
  - the exact order of scale, rotation and translation in `M_tick`.
- **Error of the translation-only form:** about (horizontal distance of the anchor from the root) × (heading
  change between the logic tick and the drawn frame). That is at most half a tick's turn, and only during
  fast spins. Stage 1 logs this estimate.
- **Runtime self-check:** with FFX-60fps smoothing off (F7), `T_drawn - T_logic` must be about 0 for
  non-attached actors. If it is not, the formula is wrong and we fall back.

**Fallbacks, in order:**
1. raw optpos (the same stepping the game's pointer has);
2. raw optpos with a 2-frame low-pass;
3. snap x/z to the mod's rendered skinned root (`g_world_live`) and keep the optpos height above the root.

Stage 1 decides with logs; stage 2 ships with a setting `marker_anchor_smooth = 1/0`.

### 2.4 What gets drawn (native, at Present, per eye, as today)

- **Default look (your current markers):**
  - **red triangle** for each target, tip at the game's anchor lifted by `marker_height` (new default 2.0,
    the game's own turn-pointer lift; was 6 on top of a head guess);
  - **turn marker** at the game's triangle position: apex `A - 0.3` in y, base `A - 2.3`.
  - Note: the turn colour in code is `0xFF40E0FF`. In both ImGui (ABGR) and the native unpack that is R=FF
    G=E0 B=40, a warm yellow, not cyan. Keep whatever you see; just say if it should change.
  - Optional A/B: `marker_anchor = game | top`. `top` is today's highest-sub-position rule, for monsters whose
    cursor element is mid-body.
- **Optional "game look" (stage 3, toggle):**
  - the turn triangle as true 3D geometry: the game's three vertices, its two colours (apex / base), spun by
    its θ. θ only advances at 30 Hz, so we spin at 6°/tick × 30 = 180°/s at display rate, phase-locked to the
    game's θ;
  - the target hand as a camera-facing billboard from **battle.dds** (the 1024x1024 BC3 atlas):
    - party rect texels (566,523)-(634,568);
    - monster rect (566,463)-(634,508), drawn mirrored (u0 > u1);
    - the hand's pointing tip sits at the anchor: monster quad to the right of it, party quad to the left
      with a 32-px gap;
    - 68:45 aspect, world-sized via the existing `marker_scale`.
  - The atlas is found at runtime: the 1024x1024 SRV of a UI-pass draw whose slot-3 UVs contain a known
    battle.dds rect. The pointer rects appear in monitor play; the damage-digit rects
    (u = 444+49·(d%5), v = 254+65·(d/5), 49x65) appear in every fight.
  - This needs a small textured shader in the marker pass, because the existing pass is untextured.
- **Monitor:** the originals run. No ImGui triangles any more (they duplicated the game cursor on the monitor
  and leaked onto the ImGui panel in the headset).

### 2.5 What is removed, what stays

| Item | Fate |
|---|---|
| Markers-from-memory path (`draw_markers_inner` / `mark` reading `cur_window` +0x08/+0xD8, head heuristic) | **Replaced** in stage 2 (kept behind `cursor_source = legacy` until stage 4, then deleted) |
| ImGui 2D marker triangles and labels (`mark()` M:2072-2076) | **Removed** in stage 2. A debug-only "show anchors" dot stays for stage-1 style checks |
| Hide-list defaults for the two cursors (`{1024, 66-69 x 43-46}`, `{textureless, 8-36 x 24-34}`) | **Off** in battle from stage 2 when the hooks are live (otherwise they only produce false positives, e.g. 1 unit from the 70x46 CTB portraits). Default entries **deleted** in stage 4. The ini may still hold them: drop exactly those two values on load |
| Pointer rule (`quad_is_pointer`, `actors_project_ui` for hiding) | **Off** from stage 2; kept only as the F11 diagnostic until stage 4, then deleted or left as a diagnostic |
| `markers_game_art` capture and reissue (`ptr_sprite_capture`, `draw_ptr_sprites_eye`, `g_ptr_reuse`) | **Retired** in stage 4 (stage 3's art path replaces it) |
| `ui_hide` 0-5 experiment knobs | Retired in stage 4 |
| Hide-list mechanism itself, skip ranges (R39/R48), UI capture, F9 pick, census, atlas dump | **Stay**: general tools, useful for any other element (and for 3D damage numbers later) |
| Menu finger cursor (command window) | **Stays on the panel**: it belongs to the menus, which live on the panel |
| Dead kind tag (alpha 0xFE) | Made real in stage 2: pass the kind explicitly so native can draw the turn marker differently |

### 2.6 Threading and race notes

- The detours run on the **main thread** inside the battle-UI draw `0x89B360`, after `0x79F380` has
  projected the very same optpos values. So we read them at the same point the game does. That is no worse
  than the game, but not provably race-free: the JobWorker writes +0x1D0 and optpos, and the join at the end
  of Ch_DrawMain is conditional. The unconditional join is later, at `0x821656`. Copy the values, never hold
  pointers.
- **The render thread only reads the snapshot.** Never touch D3D or the immediate context from the detours.
  The main thread uses the context only after a full flush, for example during resize.
- The snapshot may be one game frame ahead of the image being presented. For cursors that is acceptable.
  Stage 1 measures it, and section 4.1's work adds exact pairing if needed.
- Skipping `0x89C870` changes how neighbouring battle.dds sprites batch: CTB rows and digits merge into
  different draws with different index counts. The game does not care, but any D3D rule keyed on draw sizes
  will see different numbers. Another reason to switch the old rules off in stage 2.

### 2.7 Risks and fallbacks

| Risk | Mitigation |
|---|---|
| Hook install fails or clashes with another mod | `cursor_source` falls back to `legacy` automatically (today's path) and the overlay shows a red line |
| Wrong turn-pointer delegate | Use `void(int)`; stage 1 logs the chr argument against `[0x23CC088]` |
| A cursor-element index points at an unused sub-position | The game's own fallbacks (optpos[0], then the root translation) are reproduced |
| Behind the camera | The game's 2D pointer goes wrong there (no w check); our 3D marker is correct |
| Attached / script-driven actors (aeons, Magus Sisters, floating parts) | Raw optpos for attached actors / override matrices; watch the stage-1 correction log during summons |
| Snapshot expiry too tight or too loose | N is from stage-1 data; one setting |
| Auto-battle | The game draws no cursors; neither do we (the detour is not called / returns early) |
| Translation-only anchor drifts during spins | Logged error estimate; exact matrix form only if the logs show it matters |

---

## 3. Staged implementation plan for the cursor work

Each stage is one build, testable by playing one or two battles. Files: `src/managed/module.cs` (M),
`src/native/ffxvr_native.cpp` (N).

### Stage C1: hooks plus snapshot plus logs only (the game still draws, our old path unchanged)

- **M `init()`:** install both detours (correct delegates). Each handler records the snapshot and **always**
  calls the original.
- **M** new `cursor_snapshot` struct, seqlock and handlers. Log a `cursor:` line on every change (mask, turn
  chr), and once a second while active:
  - stamp, mask, and per target: id, kind, idx, optpos, fallback used, `T_drawn - T_logic`, the rotation
    error estimate, attached / override flags, Chr+0xF34/F38;
  - turn: chr argument vs `[0x23CC088]`, θ, both colours in hex;
  - calls per frame of the target detour.
- **M render side** (`render_imgui_inner`): project each snapshot anchor (raw optpos) through the game camera
  (`_fn_project` with the override off), compare with Chr+0xF34/F38 (2D px), and log the difference plus
  `current game frame - stamp`.
- **M** debug toggle "show cursor anchors": an ImGui dot at each projected anchor (monitor).
- **Test (monitor, headset off):** in a battle with the toggle on, the dot should sit at the **tip of the
  game's hand**:
  - left edge of the hand for a monster;
  - 32 px right of the hand's right edge for a party member;
  - about 2 units under the spinning triangle's apex for the acting member.
  Cycle targets, pick "all enemies", confirm a command, let an enemy act: the `cursor:` lines must stop
  within a frame or two of the game's cursor disappearing. Press F7 (FFX-60fps smoothing off): the
  correction must drop to about 0. Toggle auto-battle once.
- **Pass if:** the dot matches the game's pointer in every case, the diff is a few px when the camera is
  still, and the stamp lag is constant. The same build also carries the other diagnostics (section 6).

### Stage C2: our markers from the snapshot, game cursors skipped in the headset

- **M handlers:** if `_xr_on && cursor_source == hooks`, do **not** call the original.
- **M `draw_markers_inner`:** build `_mk_pos/_mk_col` from the snapshot (expiry rule, anchor formula,
  `marker_anchor_smooth`, `marker_height` default 2.0, kind passed properly). Delete the ImGui triangles.
- **M `push_view_rules`:** when the hooks are live, push `hide_cursor = 0` and `hide_pointers = 0` in battle.
- **N `ffxvr_markers_set`:** take the kind explicitly (ABI bump to 6). **N `draw_markers_both`:** unchanged
  geometry.
- **Settings:** `cursor_source = hooks|legacy` (auto-legacy if a hook failed), `marker_anchor_smooth`,
  `marker_anchor = game|top`.
- **Test (headset):**
  - no game hand or triangle on the panel;
  - red markers on exactly the highlighted target(s), moving as you cycle; "all enemies" marks all;
  - the turn marker over the member choosing a command;
  - nothing after you confirm, during enemy turns, or under auto-battle;
  - CTB portraits and damage digits never flicker or vanish;
  - on the monitor (headset off), the game's cursors look as always, with no extra triangles;
  - A/B `marker_anchor_smooth` while a monster moves (a stepping marker vs a steady one);
  - flip `cursor_source = legacy` to confirm the old path still works as a fallback.

### Stage C3 (optional): the game's look as a toggle

- **N:**
  - a textured, alpha-blended billboard path in the marker pass (own VS/PS, no game state captured);
  - battle.dds found by slot-3 UV signature (section 2.4) and cached by resource pointer;
  - the 3D spinning triangle with the game's colours and θ.
- **M:** `marker_style = ours|game`; colours and θ passed from the snapshot.
- **Test:** toggle in a battle. The hand points at the same spot as the red triangle did, is not squashed,
  and stays readable at distance. The triangle spins smoothly at the game's speed and colours.

### Stage C4: cleanup

- Delete the legacy marker path, the two hide-list defaults (with the ini migration), the pointer rule from
  the hide block (or leave it under F11 only), `markers_game_art`, `ptr_sprite_capture`,
  `draw_ptr_sprites_eye` and the `ui_hide` knobs. Update design-notes.
- **Test:** a normal session with several battles, area changes, the F9 pick. The perf line should show less
  per-draw work in battle (this also helps R17).

---

## 4. Other high-value findings, ranked by value / effort

| # | Finding | Value | Effort | What to do |
|---|---|---|---|---|
| 1 | **Follow / first-person anchor steps at 30 Hz** (4.1) | high (movement ghosting) | S-M | diagnose in the C1 build, then switch the anchor to the drawn root |
| 2 | **Loading black screens**: the game itself presents during loads only if `[[app]+0x3A0]` is set (4.2) | high | S (diag) + S-M | find where the stall is first |
| 3 | **Render-thread reads a frame ahead** (4.3) | medium-high | M | move position reads to a main-thread snapshot (shared with 1 and the cursors) |
| 4 | **Rumble → Quest haptics**: the game sends `XInputSetState` every poll; we already IAT-hook `XInputGetState` and have `xr_haptic` | medium | S | IAT-hook `XInputSetState`, map motors (low → left, high → right) to `xr_haptic` |
| 5 | **Backbuffer format is UNORM, not sRGB** **[synth]** | medium (R25 correctness) | none now | see section 5 (R25) |
| 6 | **FMV at native resolution**: one full-screen draw of the video texture (Video.cgfx); playing = `[0xCDED3C]+0x6D0/+0x6D2` | medium | S-M | grab that SRV for the cinema screen; also a clean FMV signal for R8b and R11 |
| 7 | **3D damage numbers, status icons, enemy gauges**: all have readable 3D sources and a popup queue (`0x133F110`) | medium-high (VR feel) | M | after the cursor work, same pattern (hook `0x893290`, skip in the headset, draw in 3D) |
| 8 | **Pan-and-scan maps** (4.4) | low-medium | S (diag) | one log line; probably harmless today |
| 9 | **Camera-relative target cycling**: left/right in target selection uses one of four pad maps chosen from the game camera (`0x780CE0`). With a VR view from another side, cycling may feel reversed | low-medium | S | note for the battle-follow work; a fix would pick the table from the VR heading |
| 10 | **Camera cut signal**: hook `0x7BC110` exit for a main-thread view plus the source/index tag | medium | S | better than inferring cuts from CB bytes; later |
| 11 | Clipped UI rects at -100000 PS2 units (`0x8E58E0`) | low | S | skip quads with abs(x) > 100000 in UI readers (cheap noise filter) |
| 12 | Dialogue window positions (`0x1326D70`), op_ot overlay registry (blitzball scoreboard lead, `0x11A09E0`), spell id `0xC64CA0` | medium/low | S-M | leads for later |

### 4.1 The first-/third-person anchor and the 30 Hz step

- **Today:** `push_player` (M:1927) sets the eye anchor from `chr_pos_vec` (+0x0C), the 30 Hz logic
  position, and the head offset from the optpos head rule. Both are read **at Present on the render thread**.
  The world around you is drawn at 60 from the smoothed root.
  - That is the "eye moves in 30 Hz steps while the world moves at 60" case that design-notes (22 Oct)
    suspected for walking ghosting.
  - Build 4's anchor smoothing only blurs the step; it does not remove it. Builds 1-4 fixed the stick-turn
    part.
- **The smooth value exists:** the translation of the drawn root matrix, `actor+0x200..+0x208` (+0x1D0
  floats 12..14). FFX-60fps writes lerp(k-1, k, t) there for the draw and restores it at the next
  invocation's `pre()`.
  - Note: it also includes `chr_offset_vec` (+0x6C), and for attached actors it comes from the parent's
    bone. Both are arguably more correct for "where the body is drawn".
- **But do not read it at Present.** At that moment the main thread may be in the next invocation:
  - before `pre()` the value is the lerp;
  - after it, the canonical tick;
  - during the JobWorker job, being written.
  A Present-time read of +0x1D0 could jitter *worse* than +0x0C.
- **Do it on the main thread, after the job join:**
  - **Preferred: a post-hook of `0xA44B30` / FhUtil 0x644B30** (the job-join wrapper: `mov ecx,[0xCDEDE4];
    push 0; call 0x6DBF50; ret`, `cdecl void()`, 6-byte `mov` prologue, safe to hook). It has three callers:
    `0x821656` (Sg_MainLoop, after the battle-UI draw), `0x82F881` (end of Ch_DrawMain, conditional) and
    `0x826737`. Snapshot after each call; the last one in an invocation is the post-join one. It is also the
    only one taken after the frame counter increment at `0x821617`, which identifies it.
  - **Not recommended: hooking Sg_MainLoop's entry** (FhUtil 0x420AE0). FFX-60fps verifies Sg_MainLoop's
    prologue bytes from its own init thread (`sigs_resolve_all`, "required"). If our MinHook jump lands
    first, FFX-60fps logs "a required signature failed verification - NOTHING INSTALLED" and the game drops
    to 30 fps. The install order between Fahrenheit's module init and FFX-60fps's thread is not under our
    control. **[synth, from ffx60/repo src/x/proxy.cpp and sigs.cpp]**
  - The snapshot holds the followed actor's `T_drawn`, `+0x0C`, the head optpos (with the section 2.3
    correction), the game frame counter and the exec byte `[0xC496D4]`.
- **Plan:**
  - **A1 (in the C1 diagnostic build):** extend the Motion log (Numpad 5) with `T_drawn` read in the
    post-join snapshot and at Present, next to the +0x0C value and the rendered skinned root. **Expected:**
    +0x0C changes every other line, the snapshot `T_drawn` every line, and the Present-time `T_drawn` erratically.
  - **A2 (own build):** `push_player` and `push_battle_anchor` take positions from the snapshot (setting
    `anchor_source = drawn|logic` for A/B). **Test:** walk and run in first person and in the diorama, with VD
    SSW off (plain steps) and on; compare `drawn` and `logic`.

### 4.2 Loading black screens

- The PS2-era read loop `0x76E660` presents loading frames (`0x66B1E0`: icon/Iggy layer, Present,
  `Sleep(60)`) only while `[[FhUtil 0x8C9CD8]+0x3A0] != 0`. The PApplication object pointer is at
  `0xCC9CD8` (FhUtil 0x8C9CD8); the flag lives at +0x3A0 of that object. Otherwise it blocks forever on the read event,
  and with no Present there is no `xr_tick`: the headset goes black.
  - The getter `0x641C40` has **one** caller (`0x76E6EC`) **[synth]**, so the flag matters only inside that
    loop.
  - Setting it mid-stall does not help: once the loop is in the INFINITE wait, it is not re-checked. It must
    be set before the read starts.
- **Not proven:** that the battle-start and airship stalls are in that loop. Battle loading is a per-frame
  state machine (`0x783020`), so the 1.6 s frame may be asset or texture creation instead.
- **Plan:**
  - **L0 (diag, in the C1 build):** a watchdog line when no Present arrives for 300 ms (details in
    section 6, Q5).
  - **L1 (only if L0 shows `0x76E660`):** keep app+0x3A0 set while in the field / battle, so ordinary loads
    present.
    - Optionally shorten the `Sleep(0x3C)` in `0x66B1E0`.
    - Treat those frames as "no new image", so R8b holds the last 3D frame instead of switching to the flat
      screen on the loading frame. Tell them apart by an entry hook of `0x66B1E0` / FhUtil 0x26B1E0, or by
      "no scene, no camera, flag set, FMV not playing".
  - **If L0 shows a stall elsewhere:** the presenter-thread design stands.

### 4.3 Render-thread vs main-thread reads in the mod today

Everything managed reads game memory in `render_imgui_inner`, i.e. inside Present on the render thread. The
reads, and how much the lag matters:

| Read (module.cs) | What | Matters? |
|---|---|---|
| `push_player` M:1813-1937 (+0x0C, optpos head, actor pick) | eye anchor (first person, diorama, follow) | **yes**: may be a frame ahead and at 30 Hz. Fix: 4.1 |
| `push_battle_anchor` M:1788 (party/monster centroid from +0x0C) | battle table centre | yes, but smoothed by 325 ms when tuned. Same fix |
| `draw_markers_inner` M:1998 (window mask, positions) | markers | replaced by C2 |
| btl follow roster `0xD2C895`, entity lists, Btl state, map name `0x1FCBC60`, event id, save-screen state `0x8CB994`, blitz pointers | mode decisions | no: slow-changing state, a frame does not matter |
| `actors_auto_log`, `blitz_auto_log`, census | logs | no (but note that logged positions may be a frame ahead of the image) |

The camera we override travels inside the command list (captured from CB uploads), so it is always in step
with the image. Only memory reads are affected.

### 4.4 Pan-and-scan maps (30 maps)

- On these maps byte `[0xCCB9C0]` (FhUtil 0x8CB9C0) = 0. The game keeps a 4:3 frame and fills 16:9 by
  scaling y by 4/3, with a moving vertical crop `r+0xF9BC` that follows the player (`0x65D860`).
- **Effect on us [synth, from the code]:**
  - In 3D headset mode with the FOV override (default), our projection rewrite replaces P[0], P[2], P[5]
    and P[6] with the headset frustum (N:1540-1542, 2410-2412). Crop and shake are both overwritten:
    **no effect**.
  - The FOV fallback in `xr_tick` (`ty = 1/|P[5]|`, N:5773) would be 3/4 of the true value on those maps.
    That only matters for frames where our FOV was not applied.
  - The stereo screen and the virtual screen show the game's own framing, which is what the monitor shows
    (correct).
  - The old pointer rule's `g_cam` projection differs slightly from the UI's (the UI adds a second crop term
    F9C0). That is irrelevant once C2 lands.
  - Many of these are painted-backdrop rooms that go to the virtual screen anyway.
- **One diagnostic line settles it** (section 6, Q9).

---

## 5. Updates to the remaining fix plan, and the proposed order

### 5.1 How the findings change builds 7-9 and the scripts pass

**R11 (eye-2 bindings + backbuffer sentinel), the planned build 7:**
- **Correct the sentinel's threading comment.** G4-final says "ResizeBuffers runs on the render thread". In
  fact the backbuffer is re-acquired by `0x599210` (FhUtil 0x199210) on the **main** thread:
  - after a full render-thread flush (`0x5BC3F0`), so the render thread is idle;
  - the sequence: OMSetRenderTargets(0) → release views → ResizeBuffers → GetBuffer → new RT object
    `0xCA310C` → OMSetRenderTargets(1).
  The sentinel's interlocked-only design is still right. But our `OMSetRenderTargets` hook, the backbuffer
  identification and `bb_attach` will run **on the main thread** during that call. Check that nothing there
  assumes the render thread (frame-stamped caches, `g_in_eye2`).
- **A hard invalidation signal is available:**
  - `0x599210` is `void __thiscall(obj = 0xC94F00, int w, int h)`, `ret 8` **[synth]**, prologue
    `55 8B EC 83 EC 0C`;
  - callers: `0x62C320` (call) and `0x62C444` (tail jmp).
  - A log-only detour (managed ThisCall delegate `void(nint self, int w, int h)`) goes into the C1 diagnostic
    build to cross-check the sentinel during the resolution-change test.
  - If the sentinel never fires, this detour (setting an interlocked flag that `begin_frame` consumes)
    replaces G4's "keep per-tick GetDesc" fallback.
- **Facts for the design:**
  - BufferCount 1, DISCARD, no MSAA backbuffer.
  - **Format 28 = `DXGI_FORMAT_R8G8B8A8_UNORM`**. `0x595800` writes `mov [esi+0x10], 0x1C` into the swap
    chain desc's `BufferDesc.Format` **[synth]**. exe-engine's "0x1C (RGBA8 sRGB)" is a mislabel: _SRGB is
    29 / 0x1D.
  - FMV frames draw straight into the backbuffer, so eye 2 must not re-issue them. `[0xCDED3C]+0x6D0/+0x6D2`
    is a clean FMV test.

**Settings file (R21, R22, R27-R33), the planned build 8:** no conflict with the findings. New keys from the
cursor and anchor work (`cursor_source`, `marker_anchor_smooth`, `marker_anchor`, `marker_style`,
`anchor_source`) must follow R27's clamping and R32's invariant-culture rules. Stage C4 needs a one-time
migration that drops the two old cursor entries from a saved `hide_list`.

**R8b (hold last frame) + B5-2, the planned build 9:**
- Make the hold age a **time** cap (e.g. 500 ms) rather than 30 frames. Loading-pump frames arrive at about
  16 fps, where 30 frames would be about 1.9 s.
- Add an explicit "no new image" input that L1 can drive later (loading-pump frames).
- FMV frames must never be held: use the FMV flag.
- B5-2 (escalate a hard-failing swapchain to "swapchain N stuck" plus xr_stop after about 300 tries) is
  unchanged.

**R25 (later):** with the backbuffer UNORM holding gamma-encoded bytes:
- today's path (UNORM intermediate, then a bit copy into an sRGB swapchain image) is correct;
- G4's single-copy alternative ("copy shader decodes sRGB → linear, writes through an SRGB RTV") is the
  matching one.
- Do **not** sample through an SRGB view as well, or the decode happens twice.
- The diag build logs the backbuffer desc once to confirm.

**R17 (later):** C2/C4 remove the pointer rule and hide list from battle draws, which cuts some of the
per-draw work R17 targets. Re-measure after C4.

**R23 step 2 (later):** the engine rebuilds the whole scene every frame, even on FFX-60fps render-only passes,
so identical eye-2 passes really recur. The R23 counter will show how many.

**Scripts pass (paths.ps1 helpers, R5, S1-S9):** unaffected. It does not touch the game build, so it can run
in parallel at any time before the next zip goes out.

**Hands items (R42c, R45-R47), R26:** unaffected.

### 5.2 Proposed order

| # | Build | Contents | Why here |
|---|---|---|---|
| 7 | **Diagnostic build (no visible change)** | C1 (cursor hooks, snapshot, logs, anchor dots), A1 (post-join snapshot plus Motion-log columns), L0 (300 ms stall watchdog), log-only `0x599210` detour, one-shot logs (backbuffer desc, pan-and-scan, turn colours) | Your main pain is the cursors, and every later step depends on these answers. The originals always run, so the risk is only "do the hooks coexist with Fahrenheit and FFX-60fps", and that is exactly what we want to learn first. One session of play (a few battles, a walk, a load, a resolution change) answers most of section 6 |
| 8 | **C2**: our markers from the game's hooks | section 3 | The visible fix, on proven hooks, with `cursor_source = legacy` as the way back |
| 9 | **R11** alone | as agreed, plus the threading correction; its sentinel test is cross-checked by the build-7 log | Already designed. Independent of the cursor code. Keeps its own build so a regression points at one change |
| 10 | **A2**: drawn-root anchor from the main-thread snapshot | section 4.1 | The likely walking-ghosting fix. Reuses C2's snapshot hand-off, so it comes after C2 |
| 11 | **Settings file** (R21, R22, R27-R33) | as agreed, plus the new keys | Low risk. By now the new keys exist, so they get the same hardening once |
| 12 | **R8b + B5-2** | as agreed, plus time-based age, the FMV exemption and a "no new image" input | Needed before L1 |
| 13 | **L1**: present during loads (only if build 7 showed `0x76E660`) | app+0x3A0 plus hold | Depends on R8b. Otherwise the presenter-thread design |
| 14 | **C3**: optional game look | section 3 | Pure look. After the robustness work, and only if you want it |
| 15 | **C4**: cleanup | section 3 | After C2/C3 have been played enough that the legacy path is not needed |
| 16 | **Rumble → Quest haptics** | `XInputSetState` IAT hook | Small, fun, independent; could also slot anywhere after 11 |
| - | Scripts pass | G5 order | Any time before the next zip; it is not a game build |
| later | R17 (re-measure after C4), R23 step 2, R25 (UNORM-aware), R26, hands items, 3D damage numbers, FMV cinema SRV | | |

Why not R11 first, as planned? It is a correctness fix for rare events (resize, alt-tab, 2D screens), while
the cursors are what you hit in every battle. Build 7 also gathers R11's cross-check data at no extra cost. If
you prefer to keep R11 next, nothing breaks: swap 7/8 with 9. The only hard dependencies are
C1 → C2 → C3 → C4, A1 → A2 (after C2's hand-off), R8b → L1 and L0 → L1.

---

## 6. Open questions for the in-game diagnostic run (build 7)

Each item says exactly what to log. All go to `ffxvr_diag.log`, throttled.

1. **Does the snapshot match the screen?**
   - Log a `cursor:` line on change plus once a second while active: stamp, mask (hex), and per target:
     id, kind, idx = Chr+0xDF9, optpos[idx] xyz, fallback used (0/1/2), T_logic, T_drawn, correction length,
     attached / override flags, Chr+0xF34/F38.
   - Render side: our projection of raw optpos through the game camera minus Chr+0xF34/F38 (px), and
     `[0x23CC08C]`.
   - Visual: the anchor dot at the hand's tip (monitor).
2. **How far ahead is the main thread at Present?** Log a 30 s histogram of
   (game frame counter `[FhUtil 0x1FCBBF0]` read at Present) - (latest cursor stamp), and the same against
   the post-join snapshot stamp. Also count Presents vs post-join snapshots per 30 s.
3. **The anchor correction:**
   - per target, `|T_drawn - T_logic|` and the rotation error estimate (horizontal distance of the optpos
     from the root × heading change of +0x158 since the previous logic tick);
   - log with FFX-60fps smoothing on and off (F7);
   - expected: about 0 with smoothing off; up to half a step with it on.
4. **Follow anchor (ghosting):** add columns to the Motion log (Numpad 5): `T_drawn` from the post-join
   snapshot, `T_drawn` read at Present, +0x0C, the exec byte `[FhUtil 0x8C96D4]`, plus the existing rendered
   skinned root and yaws. Walk in a straight line, then run, for 3 s.
5. **Where do the loading stalls sit?** When no Present has arrived for 300 ms (watchdog thread), log once
   per stall, all without exceptions (safe under Fahrenheit's stage-0 debugger):
   - `[[FhUtil 0x8C9CD8]+0x3A0]`;
   - renderer bytes `[[FhUtil 0x8CB9D8]+0x1196]` / `+0x1197`;
   - `Btl.battle_state` (`[FhUtil 0xD2A8E0]`), the map name;
   - the main thread's EIP (SuspendThread / GetThreadContext / ResumeThread);
   - the first 16 dwords on its stack that fall inside FFX.exe `.text`, as FhUtil offsets.
   The main thread id is captured once from the post-join hook. Test: a random battle start, talking to Cid
   for the airship menu, an area change.
6. **Turn pointer data:**
   - `[FhUtil 0xF3FA88]` / `[FhUtil 0xF3F7D4]` (hex) once per battle;
   - θ every second;
   - the detour's chr argument vs `[FhUtil 0x1FCC088]`;
   - the number of target-detour calls per frame, with the calling window's +0x01 state;
   - the monsters' Chr+0xDF9 values (does any boss use a non-1 element?).
7. **Backbuffer:** on identification, log the DXGI desc of the backbuffer (format number, size, samples). The
   prediction is format 28.
8. **0x599210:** on each call, log w and h and the frame number; then whether "bb sentinel fired" follows
   (after R11 lands). Test: change the resolution in the game settings, toggle fullscreen / windowed,
   alt-tab.
9. **Pan-and-scan:** on every map change and every 5 s, log:
   - `[FhUtil 0x8CB9C0]`;
   - `r+0xF9BC` (float), the map id `r+0xFAF4` (renderer `[FhUtil 0x8CB9D8]`);
   - `g_cam` P[0], P[5], P[2] and P[6] as uploaded (before our rewrite);
   - whether the frame is 3D, screen or backdrop.
   Visit one painted-backdrop room and one 3D map.
10. **Slot-3 UVs and battle.dds (for C3):** one F9 pick on a CTB-panel or damage-digit draw with the headset
    off and the target hand visible. It should list stream 3 (stride 8) float2 UVs per quad, including the
    hand's rect (u 0.5527-0.6191, v flipped), and the SRV's texture desc (1024x1024 BC3?). The same pick
    settles whether digits are UI-pass draws (list every quad of each battle.dds draw).
11. **Rumble:** log the first 20 `XInputSetState` calls per battle (pad index, both motor values) to confirm
    that the game sends non-zero values with the in-game vibration option on. Logging only needs the IAT
    hook that rumble would use anyway.
12. **Coexistence:** FFX-60fps's log must still say "fps60 ready" with our three game-function hooks
    installed, and `core.log` must show each hook installed. Report the in-game fps with the overlay's perf
    line.
