# Battle data: encounter set-up, load, return to the field, and what could stay resident (exe-battle-ui, round 2)

Static analysis of FFX.exe (ImageBase 0x400000). Addresses are given as `VA / FhUtil` (FhUtil = VA - 0x400000).
**H** = read in the code. **M** = the code says it, but a step is inferred. **L** = inference.
Tools: `research/exe-battle-ui-work/` (`fn.py`, `gf.py`, `x.py`, `dis.txt`). Names marked [Fh] come from Fahrenheit's `call.cs`.
exe-engine leads on timing, I/O cost and GPU resource creation. This file covers the battle-data side.

---

## 0. Summary

- **One battle map per encounter group.** The encounter group is chosen by the field script, and its btl.bin record
  holds a single `battlefield` id. So every random battle in a given field map **zone** uses the same battle map (H).
  The formations (monster sets) vary.
- **Battle start:** a per-formation btl file is read (small, async). Then the **battle map is loaded into the single
  "map" slot**: the field map is freed first. Then the party and monster models and motions are loaded through the chr
  cache (`proj/src/chr/cache.c`).
- **Return to the field:** a full teardown, then the field map is reloaded into the same slot (task "MAPREG"), and the field
  actors are re-registered (task "CHRREG").
- **Every map load flushes the chr model/motion cache** (`0x909F93: call 0x840030`). So battle 2 reloads the monster and
  party models and motions that battle 1 had just loaded, and the field return reloads the field's models (H).
- **The engine already has a "same map, skip the load" path in both directions** (`0x783E20`, `0x8615B0`). It only fires
  when the battle map id equals the field map id (some event battles) (H).
- **Cheapest win: skip the cache flush** (one 5-byte call) on battle-entry and battle-exit map changes within the same
  field map. It is memory-safe by design: the chr allocator evicts LRU cache entries when it runs out of memory
  (`0x83BDF0 -> 0x8409A0`) (H). That keeps models and motions resident across battle 2..n and across the field returns.
- **Bigger win:** keep the battle map resident. That needs a second map slot (engine work), or at least an I/O-level
  byte cache of the map archive and btl file (exe-engine's area).

---

## 1. Encounter choice and battle set-up

### 1.1 Data that is always resident
- **btl.bin** (encounter tables). It is pointed to by Btl+0xF4.. (Fahrenheit `Btl`): `0x112A9C4` ptr_btl_bin,
  **`0x112A9C8` field records** (14 bytes each: s16 field id, s16 offset into the encounter data, s16 first encounter index,
  char name[8]), `0x112A9CC` encounter data base, `0x112A9D0` field count. These are read by MsBtlListField [Fh]
  `0x79D190`, MsBtlListFieldNum [Fh] `0x79D1C0` (field id -> index), MsBtlListEncount [Fh] `0x79D170` (field -> group list),
  and MsBtlListGroup [Fh] `0x79D210` (group record). **H**
- **Group record** (Fahrenheit `BtlBinGroup`): u8 formation_count, **s16 battlefield (+1)**, u8 grace (+3),
  u8 total_weight (+4), then {u8 index, u8 weight} formations. **H**

### 1.2 Roll and trigger (field side)
- `MsBattleEncountExe` [Fh] **`0x780D10 / 0x380D10`**(field id, group, step). Its only caller is the Atel field script
  machine `0x871BC0`. So **the field script picks the group (zone)** each frame and passes the distance walked. **H**
- The encounter level `[0xC421D8]` (0 = off, 1 = normal, 2 = x tenfold, the booster) scales the step. The distance accumulates in
  Btl.walked_dist (+0x108 = `0x112A9D8`). Danger = group grace (+3). Two RNG draws `0x7988F0(0)` / `0x7988F0(1)`
  decide the trigger and the formation (weights). **H**
- On a trigger: **Btl.battle_trigger (+0x12) `0x112A8E2` = 1**, Btl.field_idx (+0x1986) `0x112C256`, group_idx (+0x1988)
  `0x112C258`, formation_idx (+0x1989) `0x112C259`. Then `ResetEncountExe` [Fh] `0x780FF0`. **H**
- The next battle-logic tick: `0x790C10` sees `battle_trigger` and calls **`MsBattleExe` [Fh] `0x781020`(0, field, group, formation)**. **H**
  Scripted battles also go through `0x781020` (from `0x781700`, `0x782090`, `0x7C6110`, `0x7C6D80`) or through MsBattleLabelExe [Fh] `0x781C90`.

### 1.3 `MsBattleExe` 0x781020: set-up
- Encounter index = field record +4 + (sum of the formation counts of the earlier groups) + formation (`0x79D0E0`), stored in
  **`0x112A8EC`** (Btl+0x11C). **H**
- **Battle map id = group battlefield**, stored in **Btl.battlefield_id (+0x1984) `0x112C254`**. Bit 0x400 selects an
  alternative map kind (0x1A instead of 0x13; `0x9083A0`). **H**
- It allocates the btl buffer: `0x83F0A0(size,0x10,5,"btlbin")` (memory group 5). For event battles it reuses `0x112A988`. It also
  re-initialises dozens of Btl fields (the zeroing block in `0x781020`). It builds the formation name `"%s_%02d"` into
  Btl.field_name (+0x198A `0x112C25A`). **H**
- Then: Chr/party set-up (`0x793530`, `0x799FB0`, `0x79F9B0`, `0x798E90`, `0x7906A0`), battle-UI init `0x89B660`, and the
  **encounter transition** (`0x7829B0(Btl.screen_transition +0x105)` -> effect type 0..3 and SE -> `0x79E5E0` -> `0x7FC590`
  -> `0x7FD7F0(bank [0x119FE98], 0, type+8, ...)`, a VFX that can sample the "FrameBuffer" texture, i.e. the
  "shatter"). Finally **battle_state (Btl+0x10, `0x112A8E0`) = 4** and the first `MsBtlReadManage` tick. **H** (M for the shatter = VFX 8..11)

### 1.4 The load state machine: `MsBtlReadManage` [Fh] **0x783020 / 0x383020** (each logic tick, from `0x790C10`), on `0x112A8E0`

| state | action | conf. |
|---|---|---|
| 4 | wait until `0x875180()` == 0 (Atel idle) | H |
| 5 | loader kind 0xE (`0x781550`); name of file `0x112A8EC` copied to `0x23112E0`; **async read of the per-formation btl file** into `0x112A9A8` (loader `0x2310C40` = `0x76B970`, the libmscd.c read queue `0x1127C80`), callback `0x782FE0` sets state 7; **loads battle font textures** (texture group 8 = Font font_0_0/0_1 and shadow_0_0/0_1, `0x642300(8,...)` -> `0x67D180`) | H |
| 6 | wait | H |
| 7 | **`MsBtlReadSetScene` [Fh] 0x783E20**: parses the btl file (+4 / +8 / +0xC / +0x10 sub-blocks -> `0x112A9AC`, `0x112A9B4`, `0x112A9C0`, `0x112A9B0`); chr set-up (`0x79C110` Chr init, `0x7AC680`, `0x7824E0`); `0x865DC0(map)`; then **state = (battleMap == currentFieldMap) ? 10 : 8**. The current map = word [MsGetSaveEventAddress [Fh] `0x785240` + 4] (`0x88D690`) | H |
| 8 | `0x643490(1)`; **map load `0x908600(map & 0x3FF)` -> `0x909A20(0x13, map)`**: first `0x909EA0` = **free the current map, including the chr cache flush**, then async-read file `map*2+1` (a "MAP1" archive) into a buffer from `0x90BBE0` (pool 0x40, `0x687020`), callback `0x9097C0`, flags `0x18DED08 |= 0xA0 / 0x2000` | H |
| 9 | wait until `0x18DED08` &0x20 and &0x40 are clear -> `0x908000` (**build the map**: `0x907F00` sub-scenes 0/1 of the MAP1 archive, `0x640210`; prints "RAM Inuse") -> state 0x22 | H |
| 0x22 | wait `[0x112CA68]`==0 -> 10 | H |
| 10..0xC | party: motion set-up, **model/motion load requests `0x7AD3F0`** into the queue `0x112BEA0` (32 x 12 bytes), served by `0x7AD190`: type 0 = model -> `0x829EF0` (chr cache, see 1.5), type 2 = motions `0x82A4C0`/`0x836A50`; wait `0x7AD180`==0 | H |
| 0xE/0xF | **monsters** (Chr 0x14..0x1B): model `Chr+4` and motions `Chr+8` via `0x7AD3F0`; wait | H |
| 0x10..0x14 | `0x783D80(...)`, more party motions | H |
| 0x15/0x16 | start messages; when quiet: `0x8337F0`, **battle_state = 1 (battle live)** | H |

### 1.5 Chr model/motion cache: `proj/src/chr/cache.c` — H
- 100 entries, pointer table `0x1302F98`, entries 0x28 bytes in the pool `0x1301FF8` (init `0x840910`). Entry: +0 id, +4 kind (1 = CHRDATA
  model, 2 = motion, 3/4 others), +5 **in-use lock**, +6 loading, +8 priority/age bits, +0xC buffer, +0x10.. GPU objects.
- Lookup `0x83FFE0(kind,id)` returns 2 (absent), 1 (loading) or 0 (present). The model loader `0x829EF0` **skips the read if the
  entry is present** ("[%s:%s]CHRDATA", "SG:RomRead"). Lock `0x840DD0` (+5=1) / unlock `0x840D70` (+5=0, kept cached).
- **Flush `0x840030`**: frees every entry that is not locked and not loading (GPU objects via `0x637980`/`0x6FB8B0`/`0x640E70`).
  Called from **`0x909EA0` at `0x909F93`** (map free, when `[0x1934EC4]` > 0) and from `0x840E40`.
- **Memory-pressure eviction already exists:** the chr allocator `0x83BDF0`, when out of memory, calls `0x827260` and then
  **`0x8409A0`** (evicts the lowest-priority, oldest unlocked entry) and retries. `0x8409A0` also runs when the table is full (`0x840560`).

---

## 2. What is freed on battle end, and the return to the field

### 2.1 End of battle (in `0x790C10`)
When battle_state == 0x21 or the end type `[0x112A8E3]` is 5 or 6: `0x7BA860(2)`, `0x81EC20(0)`, `0x821AE0(1)`, `0x8AB590`, `0x88D690`,
`0x798B70`, `0x785FC0` (writes the party state back to the save data unless `[0x112A91A]`), then **teardown `0x781590`**,
then **`0x861160`(encounter_type, field<<16|formation, battleMap `0x112C254`, end type)**. **H**

### 2.2 Teardown `0x781590 / 0x381590` frees (H)
loader close (`vt+8`), `0x643490(0)`, the btl buffer (`0x783E00`: `0x83F090(buf,5)`), `0x782750`, `0x79F990`, `0x799F70`,
`0x79DDE0`, `0x7A0370`, **battle effects** (`0x79E7B0` "op_et_battle_effect_free"), `0x792200`, battle UI end `0x89AD90`,
`0x7821F0`, `0x782640(1)`, **all Chr actors** (`0x793480` -> `0x8266F0`, for Chr 0..0x5C), per-monster buffers (`0x797A90`),
`0x787A40`, **the Chr arrays themselves** (`0x793500`: `[0x11334D4]`, `[0x113446C]`), **battle fonts** (`0x642390(8)`),
`0x7A8F20`; battle_state = 0.

### 2.3 Return `0x861160 / 0x461160` (event system)
It queues event tasks with `0x8751B0(0, fn, name)`: "CAMERA" `0x861800`, (optional "MOVIE"), "SNDCLR" `0x862120`, "SNDREG"
`0x862220`, then **`0x8615B0(battleMap)`**:
- if battleMap != the current field map: queue **"MAPREG" `0x861650`**. It waits for the map flags, then `0x908630(fieldMap)` ->
  `0x909A20(0x13, ...)` (**frees the battle map and flushes the chr cache again**), waits, `0x908000` (build), `0x909B90`
  (restores the saved map state `0x1931230..` -> `0x18DF140..`), `0x919DA0`, `0x729A10`, `0x90A2B0`, `0x919A40`. **H**
- else: no reload. It only restores (`0x909B90`, `0x873E60`, `0x919DA0`, `0x729A10`, `0x90A2B0`). **H**

Then "CHRREG" `0x861850` re-registers the field actors (model loads `0x829EF0`, waits `0x82A040`, so these are cache misses after
the flush), optional "EFFDONE", then a done task. If no special mode: `0x821750(0x40010002)` and `0x8AF5C0(2)`, otherwise
"DONE2". End type 6 (game over) takes the other branch: `0x42FA70(1)`, `0x643900(0,0,0,1.0)`, `0x643940(3)`. **H** for the
task list. **M** for what each task does beyond its name. Fades: `Sg_FadeInW` [Fh] `0x82CBA0`. The battle-end fade timing
is not traced (L).

### 2.4 What survives a battle
- btl.bin, kernel data, save data (the party state is written back), the field event/Atel state (the script resumes),
  the menu atlases (battle.dds etc. are loaded by name through the UI batcher; not traced whether they are freed). **M**
- Locked chr-cache entries (+5==1) survive the flush. Unlocked ones are freed. **H**
- The field map: **does not survive**. It is freed at battle start (state 8) and reloaded on return, unless battleMap == fieldMap. **H**

---

## 3. What could stay resident across consecutive battles in one field map

| item | today | keep resident? | how / what to reset | risk |
|---|---|---|---|---|
| **chr models + motions** (party, monsters, field actors) | flushed on every map load (`0x909F93`) | **yes**: skip the flush on battle-entry and battle-exit map changes while the field map id is unchanged | NOP or skip the call at `0x909F93` (E8 xx xx xx xx) conditionally; let `0x8409A0` handle pressure; flush normally on real area changes | **low**: the allocator evicts on pressure by design. Watch the 100-entry cap (eviction is automatic too) |
| **battle font textures** (group 8) | loaded at state 5, freed at teardown | yes | skip `0x642390(8)` at teardown and the reload if loaded (need a check that `0x67D180` reuses by name: **runtime?**) | low |
| **battle map** (MAP1 archive and its built scenes) | freed and re-read each time (single map slot `0x1934ED0`/`0x191389C`) | **only with engine work**: a second slot, or an I/O byte cache of file `map*2+1` (saves the read, not the scene build) | key = battleMap id; invalidate on area change | medium/high (scene/GPU objects are owned by the map slot; exe-engine to size the build cost) |
| per-formation btl file | re-read each time | **byte cache only** (copy fresh bytes into the new buffer) | the monster AI scripts live in this buffer (Chr+0xF78/+0xF7C point into it) and may keep work variables, so **never reuse the live buffer**; re-copy pristine bytes | low if re-copied |
| Chr arrays, Btl fields, battle UI, effects | freed and re-initialised | **no**: cheap, and they hold battle state | keep the existing init (`0x781020`, `0x783E20`, `0x89B660`) | — |
| field map on return | reloaded (MAPREG) | yes, if the field map were kept in a second slot | then the existing "same map" restore path (`0x8615B0` else-branch) already does the right re-init | medium/high (same engine work as the battle map) |

**State to reset rather than reload:** Btl struct fields (done in `0x781020`), Chr arrays (re-allocated and re-initialised by
`0x79C110`), the btl buffer contents (fresh copy), battle effects (bank reset `0x79E7B0`), UI state (`0x89BB10`, `0x89B660`),
map state (`0x909B90` restore). With the "skip flush" change none of these changes. Only resource residency does.

**Risks:**
- **RNG:** the encounter and formation rolls use the RNG streams `0x7988F0(0/1)`, which are call-based, not time-based. Faster
  loads do not change them, unless some RNG is advanced per frame during the load states (not seen in the
  0x783020 states; **M**). Speedrun manipulation could notice if frame counts matter.
- **Scripted and boss battles:** these usually have their own battle maps, so a map-id-keyed cache simply misses. Event battles
  (`0x781C90`, `[0x112C9D9]` != 0) use the preloaded buffer `0x112A988`. Keep the "skip flush" rule to *random* battles
  (`0x780D10` trigger path, `[0x112A8E2]` == 1) to be safe.
- **Save/load and menus:** a load from the main menu or an area change goes through other map loads (`0x908630` from MAPREG),
  and the flush must happen there (key on the field map id at battle start vs the field map being loaded).
- **Memory:** a 32-bit process. Models are kept only until evicted. If the chr pool is small, eviction just restores today's
  behaviour.

---

## 4. Hooks for a smoother VR transition

| signal | where | use |
|---|---|---|
| **encounter decided** (earliest) | `[0x112A8E2]` becomes 1 inside `0x780D10` (or detour `0x780D10`'s return path); field/group/formation in `0x112C256/58/59` | pre-position the camera/diorama; start a "hold" before the shatter |
| **battle start** | detour `MsBattleExe 0x781020` (args: event?, field, group, formation); battle map id in `0x112C254` right after | the shatter VFX starts here (`0x79E5E0`); it samples a captured *mono* "FrameBuffer". In VR, consider a panel or skipping it (**M**) |
| loading phases | `0x112A8E0` = 5/6 (btl read), **8/9 (map free and load: the field is gone, so hold the last frame)**, 10..0x16 (models), **1 = live** | drive the last-frame hold and the black-screen avoidance precisely; know when the battle camera is valid (state 1) |
| battle positions before models | Btl.ptr_pos_def (+0xE0, `0x112A9B0`) / BtlArea party/enemy positions (Fahrenheit `BtlArea`), parsed in state 7 | place the VR rig / diorama for the battle before the first battle frame |
| battle end | `0x790C10` end path -> `0x781590` (teardown) -> `0x861160` (return; args include battleMap and end type) | start the hold again; the field camera returns after the MAPREG/CHRREG tasks complete (task done hooks `0x861650` case 5 / `0x861850`) |
| map slot freed | `0x909EA0` (every map load) | a universal "scene is about to vanish" signal: snapshot the last good stereo frame here |

---

## 5. Questions for exe-engine / runtime
1. How long do state 8/9 (map read and build `0x908000`) take, compared with states 10..0x16 (chr cache misses) and MAPREG on
   return? That decides whether "skip the flush" alone gives most of the win.
2. Does `0x640090` (Phyre applyDelayedRelease, called just before the flush in `0x909EA0`) or `0x6403B0`/`0x6403C0`/`0x645860` free
   textures that the chr entries reference? (The flush skip must not leave dangling GPU objects.)
3. How big is pool 0 (the chr allocator `0x83BDF0`, pool param_2) versus typical battle model sizes? That is, how often would
   eviction kick in?

---

## 6. Answers from exe-engine and the chr pool size (update)
- **The map build is cheap on the main thread** *[engine]*. `0x908000` only queues jobs: the HD map scene job `0x657820` and one
  TexList job per texture (`0x666880`, ClusterManager `0x69B940`). They run serially on the single AsyncLoadManager thread, which also
  does the CD reads, while Sg_MainLoop skips gameplay and keeps presenting. **So the cost is I/O + decode + D3D creation on that
  thread** (every HD texture is its own CreateFileW through Fahrenheit's EFL). Hard main-thread stalls come from the
  libmscd sync wait-all (`0x76C490(0)` -> `0x76E660`), called from `0x781020`, `0x781590`, `0x781700`, `0x782050`, `0x782740`, `0x7848B0`,
  `0x786B00`, `0x79E6A0`, `0x88E070`. Two of these are in my start/end path (`0x781020` and the teardown `0x781590`).
- **The map's HD textures are released at every map load** (`0x909EA0` -> `0x645860` -> `0x670FE0` -> `0x671300`: a render flush, then
  ClusterManager release per texture; refcount 0 frees it) *[engine]*. `0x640090` is the ClusterManager delayed-release, not the
  chr cache. Skipping the chr flush keeps the clusters that the chr entries reference (their refcounts stay > 0). **H** *[engine]*
- **Chr pool size (mine, H):** pool 0 of the chr heap is a **fixed 0x1100000-byte (17 MiB) arena**, allocated once by
  `0x83C200` (`0x686FF0(0x1100000, 0x10)`, "heap=%x-%x"; called from init `0x820680` and `0x822490`). It holds the PS2-format chr/motion
  data (the HD clusters live elsewhere). So skipping the flush is bounded: once 17 MiB of chr data is resident, the allocator
  evicts LRU entries (`0x8409A0`) and today's behaviour returns. No address-space growth comes from this part.
- **Combined plan (with exe-engine):** (1) skip the chr flush at `0x909F93` for random-battle entry/exit with an unchanged field
  map (models, motions and their HD clusters stay); (2) exe-engine holds an extra reference on the HD clusters of the battle map
  (btlmap/) and the field map, released on a real field-map change; (3) cache the PS2 map/btl bytes at the loader level (file
  map*2+1, and the per-formation btl file, copied fresh). Identity key: battle map `0x112C254` + field map word [`0x785240()`+4].

---

## 7. CORRECTION (exe-verifier, round 2, Part C1; checked by me): the "skip the chr flush" design must manage the cache itself

**Withdrawn:** "the chr allocator 0x83BDF0 evicts on OOM" and "chr pool = a fixed 17 MiB arena, so no address-space growth"
(sections 0, 1.5, 3 and 6). My own xref DB shows **0x83BDF0 has no callers**: it is dead code. The 17 MiB pool 0 exists, but chr
data does not use it.
- **Model and motion buffers come from the main game pool:** `0x829EF0` / `0x82A4C0` -> `0x687020((size+0x8F)&~0x7F - 0x10)` ->
  `0x6FB910` -> `0x942B60`. That allocator has no eviction; on failure it returns NULL.
- **The only bound is the 100-entry table:** `0x840560` calls `0x8409A0` when the table is full.
- **`0x8409A0` leaks:** it frees only the +0xC buffer (`0x840EC0` -> `0x6FB8B0`), never the HD character at +0x10/+0x14
  (`0x637980`) or +0x18/+0x1C/+0x20, all of which the real flush `0x840030` releases. `0x840680` then zeroes those fields on slot reuse,
  so **every eviction leaks an HD character, its clusters and its VRAM**. Its tie-break is the lowest buffer address, not age.
  If nothing is evictable, `0x840560` silently fails to register the entry.
- **[0x1934EC4]** is incremented by every map build, so today the flush effectively runs on every map free. In normal play
  `0x909F93` is its only caller (`0x840E40` is reached only from the debug "< CHR >" menu `0x84F550`).

**Revised design:** skip the call at `0x909F93` only for random-battle entry/exit with an unchanged field map, **and run
the real flush `0x840030` ourselves**:
- on every field-zone change;
- whenever the table holds more than about 80 used entries (count `+0xC != 0` over the 100 pointers at `0x1302F98`);
- optionally on a memory watermark.

**Never let `0x8409A0` run in normal play.** The extra memory is bounded by the entries we allow, not by any pool.
Everything else in sections 1-5 stands (the verifier confirmed B1-B5 and B9-B11).

Addendum (exe-engine, after section 7): besides the wait-all sites in section 6, there are about 20 more (`call [0x2310C48]`, and the chr
wrapper `0x82A5E0`, used while a chr entry is still loading). For timing, instrument `0x76C490` itself, not individual callers.
A free-VA threshold is a second trigger for our own `0x840030` flush (besides the >80-entry rule and zone changes). *[engine]*

**Timing rule (exe-verifier, accepted):** make the skip-or-flush decision **only at the `0x909F93` call site** (inside `0x909EA0`, during a
map free). Never call `0x840030` from a mod hook at an arbitrary frame. The flush frees the HD character of every unlocked entry, and
only at a map free is it established that live actors hold their entries locked. So "more than 80 entries" means "at the next map
free, flush instead of skipping". The extra memory is then bounded by about 80 entries plus one battle's worth of models.

**Memory gate (exe-engine, accepted):** Fahrenheit's FhMallocModule replaces the game's primary pool with a growing 128 MB pool,
allocated bottom-up and **below 0x80000000** (persistent op_* magic effects break above it; LAA does not help game allocations).
Counters: FhUtil `0x153CD44` (reserved) and `0x153CD48` (committed). So the flush skip (and exe-engine's cluster retention) stays on
**only while the largest free VA block below 2 GB is at least 256 MB** (a VirtualQuery walk, once per battle). The ~80-entry cap and the
forced flush on zone change still apply. The decision is taken at `0x909F93`. Diagnostic build: log the committed counter and the
largest free low block at every battle start and end, to measure the real growth per battle. *[engine]*
