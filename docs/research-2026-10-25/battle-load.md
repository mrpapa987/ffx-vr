# Battle load time: where it goes, how to measure it, what to cache (exe-engine, round 2, 25 Oct 2026)

> **Verified by exe-verifier (research/exe-verify-round2.md, Part C2); corrections folded in below.** Confirmed: the single
> serial loader, the 0x781020 wait on the random path (0x78105E, caller 0x790F93 pushes 0), the 0x671300 flush,
> ClusterManager behaviour, the countdown, LAA. Corrected: the wait-all caller list (now longer), the keep flag (active,
> not dormant), the 0x81FCD0 conditions, and **option (1)'s memory claim (refuted: chr buffers come from the main pool
> and LRU eviction leaks HD chr objects, so the flush skip needs our own bound)**.

Static analysis of FFX.exe (ImageBase 0x400000, DYNAMIC_BASE, **large-address-aware**). Addresses are `VA / FhUtil`
(FhUtil = VA - 0x400000; only FhUtil offsets are valid at runtime).
**H** = read in the code. **M** = the code says it, but one step is inferred. **L** = inference. **R** = needs the runtime session.
Companion: `research/battle-data-load.md` (exe-battle-ui) covers the battle-data side in detail: the encounter roll,
MsBattleExe, the state table, the chr cache, the return tasks. This file covers the loader, I/O, GPU-resource and timing side,
the measurement plan and the caching options. Sources: Ghidra export `re/out`; Fahrenheit runtime
`fahrenheit/src/runtime/{fileloader,cd,malloc}.cs` (commit ecb6702); FFX-60fps source.

---

## 0. Summary

1. **Every random battle loads two maps.** At battle start the field map is freed from the single map slot and the
   battle map is loaded (state 8/9). At battle end the battle map is freed and the field map is reloaded (MAPREG). Both map
   loads also flush the chr model/motion cache (`0x909F93`). Every encounter group in a field zone names the same battle
   map. So battle 2..n in one zone reloads exactly what battle 1 loaded and freed. **H**
2. **All loading runs serially on one thread, "AsyncLoadManager"** (`0x691180`, thread proc `0x691360`, queue object
   `[0xCCC88C]`): the PS2 "CD" reads (libmscd: btl files, the MAP1 archive, chr data), the HD map scene (`.phyre`), and
   every HD texture of the map's `TexList.txt` (one job per texture). Each job is a `ClusterManager::loadPCluster`, which
   means: open, read, decompress, deserialize, and create the D3D11 resources through the device on that thread. **H/M**
3. **While the loader is busy, the main loop keeps presenting frames but skips gameplay** (`0x81FCD0` -> `0x641BA0`). That
   part is waiting, not a hang. The **hard main-thread stalls** (no Present, black headset) are the synchronous
   "wait for all reads" calls `loader->vtbl[2](0)` = `0x76C490` -> `0x76E660`, which block in WaitForSingleObject(INFINITE)
   until the loader thread has drained its queue. **They sit in the battle path**: MsBattleExe `0x781020` (battle
   start), the teardown `0x781590` (battle end), and `0x781700`, `0x782050`, `0x782740`, `0x7848B0`, `0x786B00`, `0x79E6A0`,
   `0x88E070`, plus about 20 direct `call [0x2310C48]` sites (e.g. `0x76F850`, `0x770430`, `0x777F00`,
   `0x779730`, `0x77D670`, `0xA54AB0` <- `0x782090`) and the chr wrapper `0x82A5E0`, which chr loads call with 0 while an entry is
   still loading (`0x825A40`, `0x8369A0`, also `0x836870`, `0x83B670`, `0xA795B0`) (exe-verifier). Instrument `0x76C490`
   itself. This matches build 7's watchdog (main thread waiting in ntdll, app+0x3A0 = 0), but the exact frame is **R**.
4. **The texture pack makes each HD texture a separate loose-file open.** Fahrenheit's EFL (`FhFileLoaderModule.h_fopen` on
   `PStreamFile::ctor 0x607BC0`) answers every indexed path with `CreateFileW` on the loose file. An 18 GB pack can mean
   thousands of opens per map (with antivirus scanning on open), plus big BC textures to upload. **M** (cost **R**)
5. **The engine already caches HD clusters by path with a refcount.** `ClusterManager` `[0xCCCA4C]`, load `0x69B940` (cache
   hit = refcount++, no I/O), release `0x69BE90` / `0x69BDB0` (free at refcount < 1). An active "delayed release" keep flag
   (+0xC) gives HD chr clusters (`.dae`/`chr` paths, entry +8 set by `0x69BB80`) a 12 s / 3 s grace after each map free
   ("Chr cache timingChrDelayRelease"). Shaders are never freed. **H**
6. **Recommended approach** (section 4), in this order:
   - (1) exe-battle-ui's **skip of the chr-cache flush** on random-battle entry/exit, **with our own bound** (a real flush
     past ~80 entries or a memory threshold; the game's LRU eviction leaks HD chr objects);
   - (2) **retain HD clusters** (`btlmap/`, `map/`, `chr/`, `magic/`) by holding one extra ClusterManager reference until a real
     field-zone change, bounded by an address-space budget;
   - (3) a **RAM byte cache of the PS2 CD files** (`map*2+1` MAP1 archive, btl formation file) at the game file API;
   - (4) the user-side Defender exclusion for the texture pack folder (zero code);
   - (5) prefetch later.
   (1)-(3) remove work; none adds render-thread work or flushes. A one-session diagnostic build (section 3) decides how
   much each one is worth.

---

## 1. Timeline: encounter -> first interactive battle frame -> back to the field

Thread legend: **M** = main (Sg_MainLoop), **L** = AsyncLoadManager, **R** = Phyre render thread.

| # | Step | Where (VA) | Thr | What happens | Conf |
|---|---|---|---|---|---|
| 1 | Encounter roll | MsBattleEncountExe `0x780D10` (from Atel `0x871BC0`) | M | Walked distance plus RNG `0x7988F0`; on a hit `[0x112A8E2]`=1, field/group/formation `0x112C256/58/59` | H |
| 2 | Battle set-up | MsBattleExe `0x781020` (from `0x790C10`) | M | **First `loader->vtbl[2](0)` = sync wait for all pending reads** (when called with param_1 == 0); `Sg_SetExecFrames(1)` `0x82DA60`; battle map id -> `0x112C254`; btl buffer `0x83F0A0(size,0x10,5,"btlbin")`; party/Chr init; battle UI init `0x89B660`; **transition VFX** (`0x7829B0` -> `0x79E5E0` -> `0x7FD7F0`, samples a "FrameBuffer" capture; the CutBuffer shader `0x664440`); state = 4 | H (VFX M) |
| 3 | state 4 | MsBtlReadManage `0x783020` | M | wait for Atel idle `0x875180` | H |
| 4 | state 5/6 | `0x783020` | M->L | **async CD read** of the formation btl file (`loader->vtbl[0]` = `0x76B970`: queue of 64 at `0x1127C80`); callback `0x782FE0` -> state 7 | H |
| 5 | state 7 | MsBtlReadSetScene `0x783E20` | M | parse btl; **if battleMap == fieldMap: state 10 (skip the map load)**, else 8 | H |
| 6 | state 8 | `0x908600` -> `0x909A20(0x13, map)` | M | **free the field map** `0x909EA0`: map-scene teardown, `0x640090` (cluster delayed release), **chr-cache flush `0x840030` at `0x909F93`**, `0x6403B0`/`0x6403C0` (renderer map scene), **`0x645860` -> `0x670FE0` -> `0x671300(map TexList, -1)`: context kick plus full render flush `0x5BC3F0`, then ClusterManager release of every map texture**; then the async CD read of file `map*2+1` (MAP1 archive) into `0x90BBE0(size)`; flags `0x18DED08` \|= 0xA0 | H |
| 7 | state 9 | `0x908000` | M->L | when the read is done: `0x907F00` (MAP1 parts 0/1) -> `0x640210` graphicFieldMapLoad -> `0x65CE90`: **queues the HD map-scene job** `0x657820` (path from `0x659290`: `/FFX_Data/GameData/PS3Data/<ps2 map path>/...phyre`); then `0x6454A0` -> `0x664F60` **queues the TexList job** `0x6578B0` (`%s%sfp/tex/TexList.txt`) | H |
| 8 | (loader) | `0x6578B0` -> `0x665770` -> one job `0x666880` per texture | L | each: `<dir>/D3D11/<name>.dds.phyre` -> existence check (`0x5766D0/0x576980`) -> **`loadPCluster 0x69B940`** (cache hit = refcount++) -> register `0x656AE0` | H |
| 9 | state 0x22 | `0x783020` | M | wait `[0x112CA68]`==0 | H |
| 10 | states 10..0x16 | `0x7AD3F0` queue / `0x7AD190` | M->L | party and monster models and motions through the chr cache (`0x829EF0`; misses after the flush -> CD reads + HD chr clusters); wait `0x7AD180`; then state 1 = **battle live** | H |
| 11 | Battle end | `0x790C10` -> teardown `0x781590` | M | **sync wait-all again** (`vtbl[2]`), frees the btl buffer, battle effects (`0x79E7B0`), Chr arrays, battle fonts `0x642390(8)` | H |
| 12 | Return | `0x861160` -> `0x8615B0(battleMap)` | M | if battleMap != fieldMap: task "MAPREG" `0x861650` -> `0x908630` -> **`0x909A20` again (frees the battle map and its textures, flushes the chr cache), reload the field map** -> `0x908000` -> `0x909B90` (restore map state); "CHRREG" `0x861850` reloads the field actors (chr misses) | H |

Gameplay gating while L is busy: Sg_MainLoop returns early when `0x81FCD0()` is true. It has three conditions:
`0x641BF0`/`0x641BD0`, `0x641BC0`/`0x642610`, and loader-not-idle (`0x641BA0` -> `0x691630`) with `0x641CB0` (exe-verifier). So during steps 6-10 frames keep coming (the shatter, then a held or black scene) unless the
main thread is inside a sync wait. **H**

Other fixed waits: `[0x112A919]` = 0x3C (60 logic ticks?) is written in several states as a message/fade countdown, consumed at
the top of `0x783020` (`if (0x112A918 && 0x112A919) { 0x112A919--; return; }`): **a deliberate pause of up to 60 ticks
(about 2 s at 30 Hz) when `[0x112A918]` is set**. Which battles set 0x112A918 is not traced (**M**, worth logging). The
end-of-battle fades (`Sg_FadeInW 0x82CBA0`) are not traced (L).

### 1.1 The loader in detail
- **AsyncLoadManager** `[0xCCC88C]` (ctor `0x690830`, init `0x691A80` from `0x6419D0`): mutex +0x10, two task lists
  (+0x20/+0x2C counts, swapped at +0x38), events +8 (wake) / +0xC (batch done), thread "AsyncLoadManager"
  (`0x6F2E10(..,"AsyncLoadManager")`). The thread proc `0x691360` runs each task's vtbl +4 (run) and +8 (done), one at a
  time. Enqueue `0x6918B0` (takes the mutex, appends, SetEvent). Idle = `0x691630`. **H**
- The libmscd read queue `0x1127C80` (64 x 0x38) is served through it. The sync wait `0x76E660` polls the queue and waits on
  the loader's +0xC event. It only presents loading frames when `[[0xCC9CD8]+0x3A0]` is set (round 1). **H**
- Other named threads: "AsyncControllerTaskManager" (`0x68C960`), "FmodManager" (`0x6F9F23`), the Phyre render thread, the
  JobWorker pool (exe-verify). **H**

### 1.2 File I/O path (what a cache can hook)
- Game file API: `0x6796F0` exists -> `0x6F9D70`; **`0x679730` open** -> `PStreamFile::ctor 0x607BC0` (**hooked by
  Fahrenheit's EFL**) + isOpen `0x607F20` (**a failed open shows a MessageBox "error" and quits**); **`0x679800` read** ->
  `PStreamFile::read 0x608090`; `0x679570` size -> `0x6F9D50`; `0x679510` close -> `0x607D80` + dtor `0x607C10`. The PS2-era
  `sceOpen 0x62FA30` / `sceRead 0x62FC00` / `sceLseek 0x62F8E0` / `sceClose 0x62F610` are wrappers over these. **H**
- VBF: `BigFileStream::openFile 0x61BF10` (Fahrenheit logs misses); `.phyre` data in the VBF is zlib-compressed (round 1),
  inflated on the reading thread (L for the exact function).
- Fahrenheit EFL (`fileloader.cs`): a path index built at init from every mod's `efl\x\` folder. On a hit it opens the loose
  file with `CreateFileW(FILE_FLAG_SEQUENTIAL_SCAN)`; on a miss it opens from the VBF (and cross-loads from FFX-2's VBF).
  `FhCdInterfaceModule` (`cd.cs`) forces `CDfileSize_PC` to query the real size (allows replaced PS2 files). **H**

### 1.3 GPU side
- The device is created with flags 0 (thread-safe). HD clusters are created on the loader thread via the device (L: I did
  not locate the exact `CreateTexture2D(initial data)` site; the only direct `device->CreateTexture2D` in the decompile is the
  screenshot path `0x58EC50`).
- Releases happen on the main thread inside `0x671300`, after a full render flush. Phyre's own shader cache: clusters under
  `/Shaders/` are flagged (+0x14) and **never released** (`0x69BE90` skips them). So shaders are compiled or created once
  per session, not per battle. **H**

---

## 2. Where the time most likely goes, and how to measure it in one session

Likely order of cost, from the code (all **R**):
1. **HD textures of the two map loads** (TexList jobs), plus HD chr models and textures after the chr-cache flush. With the
   texture pack: per-file open (EFL `CreateFileW`, AV scan), read of large DDS data, cluster deserialize, D3D upload. All
   serial on one thread.
2. **Sync waits** (`0x76C490(0)`) in `0x781020` / `0x781590`: they turn any loader backlog (for example the field's TexList jobs
   still running when the encounter hits) into a hard stall.
3. Teardown of the outgoing map: `0x671300`'s full render flush, plus per-texture releases.
4. PS2 data reads (MAP1 archive, btl, chr): small, VBF-backed, likely in the OS file cache after the first battle.
5. The deliberate countdown `[0x112A919]` (if `[0x112A918]` is set) and the transition VFX length.

### 2.1 Diagnostic build: hooks and logs (all log-only; originals always run)
Install everything at module `init()` (Fahrenheit forbids late insertion on hooked functions). Timestamp with QPC; log the
thread id; use a lock-free ring flushed from the Present thread. **One "battle report" line per phase per battle** is enough.

| Probe | Address (FhUtil) | Hook type | Log |
|---|---|---|---|
| Battle state machine | poll `[0xD2A8E0]` (battle_state), `[0xD2A8E2]` (trigger), `[0xD2C254]` (battle map), `[0xD2A918/9]` (countdown) | post-main-loop hook (already exists: the job-join post-hook) | timestamp of every change: gives the state 4/5/7/8/9/0x22/10..0x16/1 durations directly |
| Loader busy | `[[0xCCC88C]+0x20]`, `+0x2C` (queue counts), renderer `[[0x8CB9D8]+0x1195]` (map loading) | same poll | busy/idle edges |
| **Sync wait-all** | `0x36C490` (`loader vtbl[2]`, cdecl(int)) | detour | duration, arg, **return address** (which caller: `0x781020`, `0x781590`, ...) |
| Battle start / teardown | `0x381020`, `0x381590` | detour | duration |
| Map load request / free / build | `0x509A20`, `0x509EA0`, `0x508000` | detour | duration; inside the free, separately `0x440030` (chr flush) and `0x271300` (texture-list unload incl. flush) |
| HD loads | `0x29B940` loadPCluster (thiscall, this=`[0x8CCA4C]`, path) | detour (Fahrenheit already hooks it for path normalisation: chain, init-time) | path, duration, **hit** (path already in the tree: duration < 0.1 ms, or check `0x29B7C0`-style lookup first), thread |
| HD releases | `0x29BE90` (by name), `0x29BDB0` (by ptr) | detour | count, freed or not |
| File opens / reads | `0x207BC0` PStreamFile ctor (chain after Fahrenheit's h_fopen), `0x208090` read | detour | path, open ms (EFL loose vs VBF: `handle_os` vs `handle_vbf` after the call), bytes, read ms |
| D3D resource creation | device vtable: CreateBuffer (3), CreateTexture2D (5), CreateShaderResourceView (7), Create*Shader (12, 15, ...) | vtable hook (the mod already owns the device hooks) | count and total ms per thread, per battle phase |
| Return tasks | `0x461650` (MAPREG), `0x461850` (CHRREG) | detour | duration |
| Stall sampler | watchdog: when no Present for 300 ms, SuspendThread(main), GetThreadContext, walk the EBP chain (game code keeps frame pointers) with VirtualQuery-guarded reads, ResumeThread | existing L0 watchdog + EBP walk | the game return addresses, to name the blocking function exactly |
| Memory | GetProcessMemoryInfo (PrivateUsage), VirtualQuery walk -> **largest free block**, total free VA | at each battle start/end | for the cache budget |

**Session script for the owner (about 15 min):** in one zone (e.g. Mi'ihen or Calm Lands), fight 3 random battles in a
row (no menu, no area change). Then rename the texture pack's `efl` folder off (or toggle its mod) and fight 2 more in the same
zone. Then make one area change. The report shows: battle 1 vs 2..3 (what is reloaded), pack vs no pack (the I/O and upload
share), and which sync wait stalls and for how long.

---

## 3. What is freed at battle end, what stays, and whether the same area reloads

- **Same battle map every time in a zone:** yes (encounter group -> one battlefield; exe-battle-ui 1.1). Reloaded every
  battle. **H**
- **Freed at battle start (field map):** map buffer and scene, the field map's HD textures (refcount -> 0 -> freed), all
  unlocked chr-cache entries (field NPC models and motions), the renderer map state. **H**
- **Freed at battle end:** btl buffer, battle effects, Chr arrays, battle fonts (group 8), then (MAPREG) the battle map and its
  HD textures and the chr cache again. Party/monster models and motions do not survive the flush. **H**
- **Stays resident:** shaders (never released), btl.bin, kernel and save data, menu atlases (not traced), and the chr
  cache's **locked** entries. **H/M**
- **The engine's own exception:** battleMap == fieldMap skips both loads (`0x783E20` state 10; `0x8615B0` else-branch).
  It is only used by some event battles. **H**
- **ClusterManager details** (for any cache work; confirmed by exe-verifier), `[0xCCCA4C]`: +0 delay state (init 2), +4 timer, +0x10 tree (std::map
  keyed by `0x67B7D0(path)` hash), +0x18 mutex. Entry (alloc `0x69ABA0`): +0 PCluster*, **+4 refcount**, +8, **+0xC keep
  (delayed release)**, +0x14 is-shader. `0x69AE80` (on map free via `0x640090`, while state == 2) marks keep on loaded
  entries; `0x69A6B0` frees keep-marked entries with refcount < 1; `0x69C010` (every frame, Sg_MainLoop -> `0x6442D0`) applies
  it after 3 s (state 1) or 12 s (state 0). Lookups: `0x69B7C0` (exists), `0x69B4B0` (get by name, refcount > 0). Force
  unload: `0x69B310` (ignores refcount; called from `0x65D780`). **H** (the exact state transitions over time: M)

---

## 4. Caching options, ranked by value / risk

### (1) Skip the chr-cache flush on random-battle map changes (exe-battle-ui's proposal) - value high, risk low-medium, effort S
- Hook `0x909EA0` (or patch the call at `0x909F93` to a conditional) so that `0x840030` is skipped when the map change is a
  random-battle entry (`[0x112A8E2]` == 1 path / battle_state 8) or its MAPREG return with the same field map id. Keep it on
  every other map load.
- Memory, **corrected by exe-verifier: the "fixed 17 MiB pool with LRU eviction" claim is refuted.** The allocator
  `0x83BDF0` is dead code (no references; I confirmed it in the disassembly). Chr buffers come from the main game pool
  (`0x687020` -> `0x6FB910`), and the table-full eviction `0x8409A0` frees the entry without releasing its HD character
  objects (+0x10/+0x14; it skips `0x637980`), so they leak. So the skip **needs a self-imposed bound**: let the real
  `0x840030` flush run when more than ~80 of the 100 entries are used, or when the largest free VA block drops below the
  budget, and always on a real zone change. **Decide only at the game's own call site `0x909F93` (inside the map free)**;
  never call `0x840030` from a hook at an arbitrary frame (it unloads every unlocked entry's HD data, and only during a map
  free is everything still drawn guaranteed to be locked; exe-battle-ui / exe-verifier).
- Side effect that helps: chr entries hold their HD clusters, so those textures stay alive too (their refcount stays > 0).
- Failure modes: the leak above if the table fills without our bound; stale entries keyed by id are impossible within one session (the files don't change). Risk = an entry whose GPU
  objects were freed by another path; I found none (`0x640090` / `0x6403B0` / `0x6403C0` / `0x645860` free map-owned objects).

### (2) Retain HD clusters across battles - value high (likely the biggest with the texture pack), risk low-medium, effort M
- **Mechanism (uses only engine functions):** detour `loadPCluster 0x69B940` (FhUtil `0x29B940`, thiscall
  `this=[0xCCCA4C]`). After the original returns a cluster whose path is under `/btlmap/`, `/map/`, `/chr/`, `/magic/` (or
  matches the active battle map's and field map's folders), call the original **once more** on the same path. That is a cache
  hit, refcount + 1, no I/O. Remember the path in a list. The game's own release then leaves refcount 1, and the cluster stays,
  so the next load of the same path is a pure hit.
- **Release policy:** on a real field-zone change (field map id at battle start != the field map now being loaded, or event_id
  change outside battle), on a save load/title, and when over budget: call `releasePClusterByName 0x69BE90(path)` for each
  remembered path (LRU order), **on the main thread outside the loader's busy window** (`0x691630` idle). That gives the same
  free path (and the same flush in `0x671300`) the game uses.
- **Do not use the keep flag (+0xC):** it is the engine's own short grace (set on map free by `0x640090` -> `0x69AE80`, applied
  after 3/12 s by the per-frame `0x69C010`), and it would free the entries anyway.
- **Re-entrancy and key (exe-verifier):** the tree key is only a hash of the normalised path (`0x67B7D0`). Our second call
  goes through Fahrenheit's chain and re-enters our own detour: guard it with a thread-local flag. Keep our own list of the
  *normalised* paths (as received after Fahrenheit's hook) for the later releases.
- **Budget, revised: effectively the LOW 2 GB.** Fahrenheit's `FhMallocModule` (`malloc.cs`) replaces the game's 0x30000000
  primary pool with a **0x8000000 (128 MB)** initial pool that grows by extra reservations (`h_mreserve`, counters at FhUtil
  `0x153CD44` reserved / `0x153CD48` committed). It deliberately allocates **bottom-up**, because the `op_*` persistent
  magic effects break with addresses above 0x7FFFFFFF ("pointer tagging/truncation bug"). So everything the game allocates,
  including the chr buffers (main pool `0x687020` -> `0x6FB910` -> `0x942B60`, no eviction) and the Phyre clusters we keep
  alive, competes for the low 2 GB; the 4 GB LAA space only helps our own allocations. Rules: (a) the mod's own caches (the
  byte cache in (3)) use `VirtualAlloc(MEM_TOP_DOWN)` above 2 GB, which is safe because the game never sees those pointers;
  (b) cluster retention and the chr-flush skip stay on only while Fahrenheit's committed counter (FhUtil `0x153CD48`) plus
  other low-VA use leaves a largest free block of at least 256 MB below 0x80000000; (c) log both counters per battle in
  the diagnostic build.
- (Old note) on 64-bit Windows the process has 4 GB of VA. Fahrenheit's malloc module shrinks the game's
  0x30000000 pool reservation to what is committed. GPU textures in VRAM do not consume process VA as such, but what the
  driver and Phyre keep on the CPU side per cluster (the cluster's deserialized memory, staging) is **R**. Rule: retain only
  while the largest free block > 512 MB and PrivateUsage < 2.5 GB; otherwise evict the oldest. Expected footprint: one
  battle map plus one field map plus monsters (tens to a few hundred MB at pack resolution; **R**).
- Failure modes: (a) double-release: always pair one extra release per extra acquire; never call `0x69B310` (force). (b) A
  cluster force-unloaded by the game (`0x65D780` path) while we still hold it: check with `0x69B7C0` before releasing. (c) Main
  vs loader thread: loadPCluster takes the manager mutex internally. Calling it from inside our detour on the loader thread
  is fine, because the mutex is released before the load.
- Interactions: Fahrenheit hooks the same function (`h_pcluster_ld`, path normalisation). Chain at init; our second call
  must go through the full chain (normalised path). FFX-60fps does not touch it. Nothing new runs on the render thread.

### (3) RAM byte cache of PS2 CD files - value medium, risk low-medium, effort M
- Cache the bytes of file `map*2+1` (MAP1 archive, per battle map and per field map) and of the per-formation btl file. Hook
  the game file API (`0x679730` open -> remember the path per handle; `0x679800` read -> on a cached path memcpy from RAM).
  For the CD queue path, hook at the libmscd read level (`0x76B970` request: id/buffer/callback) instead and complete it from
  RAM. Always **copy fresh bytes into the game's buffer**: the btl buffer is written by monster scripts (exe-battle-ui).
- Saves the read and decompression, not the scene build (that is (2)'s job). Size: a few MB per map. Budget-trivial.
- Failure modes: a missing-file MessageBox if we ever fake an open; never fake opens, only serve reads for handles that
  opened. Fahrenheit's EFL replacement files must win: cache by the normalised path after its hook.
- Lower priority than (2) because these files are small and likely already in the Windows file cache after the first read.

### (4) User-side: exclude the texture pack folder from real-time AV scanning - value potentially high, risk none, effort zero
- With thousands of small loose files opened per map load, Defender's on-open scan can dominate. Measure with the session
  script (pack vs no pack); if file opens dominate, recommend the exclusion in the README.

### (5) Prefetch during field play - value medium (also helps battle 1), risk medium, effort M-L
- After a field map finishes loading, when the loader is idle, read the zone's battlefield id from btl.bin (exe-battle-ui 1.1),
  derive its HD paths (its TexList and scene `.phyre`) and issue loadPCluster plus retain (as (2)). Battle 1 then hits the
  cache. Risks: competes with field streaming on the single loader thread (do it only when idle), memory in big zones.
  Do (2) first: it gives battles 2..n for free, and prefetch is "(2) seeded early".

### (6) Shorter deliberate waits / transition - value low-medium, risk low (opt-in), effort S-M
- Log `[0x112A918/9]` first. If the 60-tick countdown fires on random battles, an option can zero `0x112A919` when the loader
  is idle. The shatter VFX length is data-driven (`0x7FD7F0` VFX 8..11); leave it alone. In VR, a hold-last-frame or panel is
  better than a mono shatter.

### (7) A second map slot (keep the battle map built) - value high, risk high, effort L
- The single slot (`0x1934ED0` buffer, `0x191389C` built map) and the renderer's single map scene make this real engine work.
  (1)+(2)+(3) recover most of it (resources stay; only the cheap rebuild remains). **Not recommended.**

### FFX-60fps and VR interactions
- FFX-60fps paces Sg_MainLoop invocations; during loads Sg_MainLoop early-returns, so pacing is unaffected. `MsBattleExe` calls
  `Sg_SetExecFrames(1)` (`0x82DA60`), which FFX-60fps hooks and overrides per invocation (L: harmless).
- The keepalive / presenter work (vr-opportunities) is complementary: caching shortens the stalls, keepalive hides what is left.
  The sync waits (`0x76C490(0)`) are where the main thread blocks; the render thread is then idle in its ring wait.

---

## 5. Open questions (for the runtime session)
1. Which sync-wait caller stalls at battle start, and for how long? (Stall sampler plus the `0x36C490` detour.)
2. State 8/9 vs 10..0x16 vs MAPREG durations, with and without the texture pack.
3. Loader-thread time split: file opens vs reads vs loadPCluster deserialize vs D3D creation.
4. Process VA after 3 battles with (2) enabled (largest free block).
5. Does `[0x112A918]` get set on random battles (the 60-tick countdown)?
