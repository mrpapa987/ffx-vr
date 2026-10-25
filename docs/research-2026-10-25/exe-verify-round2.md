# Round 2: file loader, address space, and a check of the load-time docs (exe-verifier, 25 Oct 2026)

**Sources:**
- the Ghidra export (`re/out`) and my own objdump (`research/exe-verify-work/dis.txt`)
- **upstream Fahrenheit at the pinned commit ecb6702** (`research/exe-verify-work/fh-upstream`, a read-only clone of `github.com/fahrenheit-crew/fahrenheit`; the files I quote are saved as `research/exe-verify-work/fileloader_ecb6702.cs`, `cd_ecb6702.cs` and `malloc_ecb6702.cs`)
- `re/fahrenheit-src` (core only; the runtime modules are not in it)

Addresses are given as `VA / FhUtil`, with FhUtil = VA - 0x400000. **Confidence:** H = read in the code, M = one step is inferred, L = inference only.

---

## Part A. What a mod can do with the game's file reads

### A1. The game's two read paths (H)

**1. By name: Phyre `PStreamFile`.** The object is `{+0 HANDLE handle_os, +4 VFile* handle_vbf, ...}`. Its methods:

| Method | VA / FhUtil | Convention | Behaviour |
|---|---|---|---|
| ctor | `0x607BC0 / 0x207BC0` | thiscall(path, read_only, p3, p4, p5) | |
| openFile | `0x607F40 / 0x207F40` | | |
| getFileSize | `0x607DC0 / 0x207DC0` | thiscall | `GetFileSizeEx`, or VBF `0x61ACB0` |
| read | `0x608090 / 0x208090` | thiscall(buf, size) | `ReadFile`, or VBF `0x61AA60` |
| seek | `0x6080E0` | | `SetFilePointer`, or VBF `0x61AC30` |
| skip | `0x608120` | | |
| write | `0x608160` | | VBF files: refused |
| closeFile | `0x607D80 / 0x207D80` | | |

Every method branches on `handle_vbf != 0`. So the object has exactly two backends: a VBF entry, or a native OS HANDLE.

**2. PS2 "CD" semantics.** The PS2 assets (the `ffx_ps2/...` battle and field data) are addressed by numeric id through `cdrom.mdg/.fid/sizetbl` (`CdEnvInit 0x76C560`). The read queue is `read_task_buff [0x1128ADC]`. These files are opened through `sceOpen` = `0x62FA30` → `0x679730` → PStreamFile ctor `0x607BC0`, so they land in path 1 as well (**H**). For example, `CDfileSize_PC 0xA428A0` calls `0x62FA30(name,1)`. Whether the actual *reads* of the read task also go through PStreamFile is for Part C.

**How a VBF read works** (`0x61AA60` → `0x61AA90`, H):
1. **Block-window check.** If the requested range is inside the VFile's current block window, it is served by `memcpy`.
2. **Disk read.** Otherwise `0x61B8D0` runs CRT `_fseeki64` + `fread` of 64 KB blocks. A per-block u16 size table says how large each stored block is; a value of 0 means a stored (uncompressed) 0x10000 block.
3. **Decompress.** `0x61BA80` runs zlib (`0x405E20` → `0x406230`, the "incorrect header check" strings).
4. **Threading.** All of this runs **synchronously on the calling thread**. `CreateThreadpoolWork(0x61B8D0)` and `CreateThreadpoolWait(0x61BA80)` exist (chunk_0056), but in the `0x61BFE0` path both are called directly.

**Consequence for a cache.** After the first load, Windows' file cache (standby list) usually still holds the compressed VBF bytes. A second load of the same battle then pays mainly for:
- zlib decompression of every 64 KB block
- CRT fread/memcpy
- the engine-side work after the read: parsing, Phyre cluster fixup, and creating D3D textures and buffers

A cache of the *file bytes* removes only the first two. The split between them is a runtime question. The two load-time docs (Part C) should say where the time actually goes.

### A2. What Fahrenheit's file loader does (`FhFileLoaderModule`, runtime/fileloader.cs @ecb6702, H)

**Hooks.** Installed at `init`:
- `PStreamFile_ctor` (`0x207BC0`)
- `ClusterManager_loadPCluster` (`0x29B940`)
- `ClusterManager_getPClusterByName` (`0x29B4B0`)
- `PStreamFile_SetStreamPrefix` (`0x207D40`)
- `BigFileStream_setStreamPrefix` (`0x21C3A0`)
- `BigFileStream_openFile` (`0x21BF10`; it only logs files that were not found)
- `fiosUnifyFilename` (`0x279820`)

**Path handling.** It shortens the stream prefix from `../../..` to `/` and re-implements path normalisation: `host0:` → `/ffx_ps2`, `GCM` → `D3D11`, `.cgfx.phyre` → `.fx.phyre`. Paths are limited to 0x100 bytes.

**Redirect.** It indexes every mod's `efl\x` and `efl\x2` tree **once, at launch**, into a dictionary from the upper-cased VBF path to the absolute path on disk. Later mods in the load order supersede earlier ones. When the game constructs a PStreamFile for an indexed path, the loader sets `handle_vbf = null` and `handle_os = CreateFileW(path, FILE_READ_DATA, FILE_SHARE_READ, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN)`. From then on the game reads the loose file through its own native branch: `ReadFile` and `GetFileSizeEx`, synchronously.

**Cross-load.** If the file is not in the primary VBF, it opens the other game's VBF (`data\FFX2_Data.vbf`) as a second BigFileStream and serves the file from there.

**CD sizes** (`FhCdInterfaceModule`, cd.cs). `check_ex_file_size` (`0x76D660 / 0x36D660`) is forced to return 0. `CDfileSize_PC` (`0xA428A0 / 0x6428A0`) runs with `Cd.module` set to 0, so the size comes from the replacement file rather than `sizetbl`. Without this, a replaced PS2 file whose size differs from the original crashes the game.

**What it does not do:**
- no read-ahead
- no asynchronous I/O
- no in-memory serving
- no caching

Loose files are read with the same blocking calls as before, just from a different file. The 18 GB texture pack therefore costs one `CreateFileW` per file plus uncompressed reads. Those reads may be faster than decompressing VBF blocks, but each file is larger.

### A3. What a mod can add (H for the mechanism, M for safety)

- **Intercept: yes.** Any of the PStreamFile methods above, `BigFileStream_openFile`, `CDfileSize_PC` and `loadPCluster` can be hooked with `FhCallHandle`.
  - **Signatures:** Fahrenheit's handles are `internal` to core, so a mod declares its own delegates and `FhCallLocation("FFX.exe", offset)`, exactly as module.cs already does for D3D11.
  - **Install timing:** install the hooks during `init`. Fahrenheit refuses a late hook on a function that already has one, and `FhFileLoaderModule` already hooks the ctor.
  - **Chain order:** hooks queued at init are applied in load order. Whether our hook runs before or after the EFL hook depends on the mod load order, so a cache must sit *outside* EFL: key on the path the game asked for and let EFL resolve inside it. Alternatively, key on the final PStreamFile state after the ctor returns.
- **Redirect: yes.**
  - **Static:** ship files in `efl\x`. They are indexed at launch, so files added later are not seen in that session.
  - **Dynamic:** in our own ctor hook.
- **Serve from memory: not provided, but feasible.**
  - **Hooks needed:** hook the ctor (after the original, record `this` → cache entry), getFileSize, read, seek, skip and close. Then serve bytes from a buffer whenever `this` is in our table.
  - **Never fake the open itself** (correction to my first draft). `0x679730` (the game's file open) checks `isOpen 0x607F20` = `handle_os != -1 || handle_vbf != 0`. On false it shows `MessageBoxW("error")` and starts quitting. So a "memory" object with `handle_vbf = 0, handle_os = -1` would kill the game the first time any caller bypasses our read hook.
  - **Safe form:** let the real open happen (EFL loose file or VBF entry, so isOpen is true and the size is right), then serve `read`/`seek`/`skip` from the cached buffer for that `this`. This saves the zlib inflate and the disk/OS-cache copy, not the open.
  - **Opens not worth faking:** a VBF open is a hash lookup, and an EFL open is one `CreateFileW` (with an AV scan, see Part C L-items).
  - **Other readers:** `0x607E00` (a static path/attribute helper) and anything reading HANDLEs directly would bypass the hooks. Any reader that bypasses PStreamFile needs its own check (Part C will tell whether battle loads have one).
  - **Cheaper alternative:** a memory file the OS serves (a temp file with `FILE_ATTRIBUTE_TEMPORARY`, handed out as `handle_os` the way EFL does). It costs no process address space, but it is not guaranteed to stay in RAM.
- **Asynchronous: only by the mod's own threads.** A mod thread could prefetch the next battle's files (read them into the cache, or simply read them once so the OS cache is warm).
  - Calling the game's VBF code (`FhVbfFileScope`, i.e. ctor/read/close) from a non-game thread is **not proven safe**. `BigFileStream` and `BigFileHandle` hold CRITICAL_SECTIONs, which suggests concurrent use was planned, but each VFile has a single 1 MB block buffer (`VFile.ptr_buf_0x100000`), and VFile slots are a fixed pool of 32 per handle.
  - A safer prefetch reads the VBF with the mod's own reader: open the `.vbf` file separately, parse the header, inflate. The format is public (Virtuos big file: MD5 name hashes, block table, zlib) but not implemented in Fahrenheit.
- **Which data actually helps** (the round-2 design question): see Part C.

## Part B. Address-space budget of the 32-bit game process

**Facts (H):**
- **4 GB of address space.** FFX.exe has `IMAGE_FILE_LARGE_ADDRESS_AWARE` (Characteristics 0x122) and DYNAMIC_BASE. On 64-bit Windows the process therefore has **4 GB of user address space** (0x00010000..0xFFFEFFFF).
- **Image:** SizeOfImage 0x237D000 (35.5 MB): .text 7.0 MB, .data 25.8 MB (mostly BSS), .reloc 1.3 MB.
- **Game memory pool.** `_malloc_pool_init 0x6FB9B0 / 0x2FB9B0` tries to **reserve 0x3000_0000 (768 MB)** and steps down by 16 MB until it succeeds (`iVar2 = 0x30000000; ... iVar2 -= 0x1000000`). It records `[0xCE9020]` (base) and `[0xCE9024]` (size). Further reservations go through `0x9439A0 / 0x5439A0` (`VirtualAlloc(MEM_RESERVE, PAGE_NOACCESS)`), with running totals at `[0x193CD44]` (reserved) and `[0x193CD48]` (committed), FhUtil `0x153CD44 / 0x153CD48`.
- **Fahrenheit's `FhMallocModule`** (malloc.cs) **replaces the pool init with a 0x0800_0000 (128 MB) pool** and reserves the rest on demand. Its reason is the FMV "green screen" failures: ASLR'd DLLs fragment the address space and leave no contiguous block for FMVs. It also allocates **bottom-up on purpose**, because `op_*` magic effects mis-handle addresses above 0x7FFF_FFFF. The source comments warn:
  - the pool size doubles as the upper bound of one allocation, so too small a pool sends the allocator into a reserve loop that can exhaust the address space;
  - addresses above 2 GB trip an effect bug.
- **PS2 emulation heap:** `0x83C200` allocates 0x110_0000 (17 MB) once.
- **VBF reader:** a 1 MB block buffer per open VFile, at most 32 per BigFileHandle. Fahrenheit opens a second BigFileStream (the cross-load VBF), so up to 2 x 32 MB if every slot is in use (M).

**Peak usage can't be measured statically.** No log in the repo or uploads records virtual-size or private-bytes figures. The big unknown consumers:
- the 32-bit CoreCLR (Fahrenheit plus our managed module: GC segments and the JIT)
- the D3D11 user-mode driver (resource mappings, shader caches; dynamic and staging buffers are CPU-mapped, e.g. the 12 MB UI ring buffer)
- the in-process OpenXR runtime (VDXR) and our native DLL
- Phyre's CPU-side copies of loaded clusters (texture data before upload, and whether it is kept); the 2.5x texture pack inflates these

**Estimate (L), for planning only:** image + pool + heaps + CLR + driver + XR comes to roughly 1.2-2.0 GB at a battle with the texture pack, leaving about 2 GB of nominal headroom. **Contiguity, not total size, is the binding limit:**
1. FMVs need large contiguous blocks.
2. Everything Fahrenheit allocates bottom-up must stay below 2 GB.
3. A big cache placed low would crowd the game's own allocations.

**Measurement recipe (one runtime check, so we stop guessing):**
1. From the managed module, every few seconds, walk the address space with `VirtualQuery` and log the total MEM_FREE, the **largest free block**, the free space below and above 0x8000_0000, and MEM_COMMIT private.
2. Also log `[FhUtil 0x153CD44]` (reserved) and `[0x153CD48]` (committed).
3. Capture these at the peaks: in a battle with the texture pack, on a summon, during an FMV, and at the airship menu.

**Rules for a cache:**
- **Do not keep it in the game's heap, and not below 2 GB.** Use `VirtualAlloc(MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN)` in medium chunks (1-4 MB), so the cache lives in the top half that Fahrenheit's bottom-up allocator does not use. The game never sees these pointers (we memcpy out of them), so the above-2 GB effect bug does not apply.
- **Budget:** cap the cache at about 256-512 MB until the measurement says otherwise. Evict on area change.
- **Better still, keep it out of the process entirely:** prefetch or warm the OS file cache (zero address space; RAM is used only if free), or use a mapped section whose views are mapped on demand.

---

## Part C. Verification of the round-2 load-time docs

I checked the claims a design would rest on, in both docs as of 23:15 (battle-data-load.md including its section 6 update).

### C1. battle-data-load.md (exe-battle-ui)

| # | Claim | Verdict | Evidence / correction |
|---|---|---|---|
| B1 | One battle map per encounter group | **CONFIRMED** | `0x7810A5`: `0x79D210(field, group)`; `0x7810C1`: `mov word [0x112C254], hi*0x100 + lo` from bytes group+1/+2. It does not depend on the formation |
| B2 | Random battles go through `MsBattleExe(0, field, group, formation)` | **CONFIRMED** | `0x790F70`: `cmp byte [0x112A8E2],0; je`; pushes formation/group/field, `push 0`, `call 0x781020` at `0x790F93` |
| B3 | State 7 → 10 if battleMap == fieldMap, else 8 | **CONFIRMED** | `0x783E20`: `DAT_0112a8e0 = (0x88D690() == DAT_0112c254)*2 + 8` |
| B4 | State 8 → `0x908600(map & 0x3FF)` → `0x909A20` → `0x909EA0` (free incl. chr flush at `0x909F93`, if `[0x1934EC4] > 0`) | **CONFIRMED** | `[0x1934EC4]` is incremented by every map build (`0x908064`, `0x9080B1`, `0x909517`, `0x909579`) and zeroed after the flush, so the flush runs on effectively every map free. The `0x88C7F0/800/820(5)` calls in `0x909EA0` are empty stubs or getters, so nothing frees a "group 5" arena there |
| B5 | Flush `0x840030` callers: `0x909F93` and `0x840E40` | **CONFIRMED** | `0x840E40` is a thunk called only from `0x84F550`, the debug "< CHR >" menu, so in normal play the map free is the only flush. (The 12 ".data refs" to 0x840030 are byte coincidences inside `{u16 0x30, u16 0x84}` table records) |
| B6 | "Memory-pressure eviction already exists: chr allocator `0x83BDF0` → `0x827260` → `0x8409A0` on OOM" | **REFUTED** | `0x83BDF0` has **no references at all** (no rel32, no absolute): it is dead code. Model and motion buffers come from **`0x687020` → `0x6FB910` → `0x942B60`, the main game pool** (`0x829EF0`, `0x82A4C0`: `FUN_00687020((size+0x8F)&~0x7F - 0x10)`). That allocator has no eviction: on failure it returns NULL and the wrapper `memset`s it, which crashes. Entry free `0x840EC0` uses `0x6FB8B0` (pool free), or `0x83F090` → `0x6FB6C0` for id 0x903E7 |
| B7 | "Chr pool is a fixed 17 MiB arena (`0x83C200`); skipping the flush is bounded; no address-space growth" | **REFUTED** | The 17 MiB heap from `0x83C200` serves only the dead allocator. Chr data lives in the game pool, which Fahrenheit's FhMallocModule grows on demand. **The only bound is the 100-entry table** (`0x840560`: when full it calls `0x8409A0`) |
| B8 | `0x8409A0` "evicts the lowest-priority, oldest unlocked entry" | **PARTLY, and important** | Tie-break = **lowest buffer address**, not age. More important: it frees **only the buffer** (`0x840EC0`: +0xC, id = -1). It does **not** release the HD character object at **+0x10/+0x14** (`0x637980`, "Virtuos Error: try to unload a nonexistent character...") or the +0x18/+0x1C pool blocks and +0x20 object that the flush releases (`0x840030`; `0x840950` always returns 0). `0x840680` then zeroes +0x10..+0x20 when the slot is reused. **So every eviction leaks an HD character together with its cluster references and VRAM.** When nothing is evictable it returns 1, and `0x840560` gives up without registering the new entry |
| B9 | The model loader skips the read if the entry is present or loading | **CONFIRMED** | `0x829EF0`: `if (0x83FFE0(1,id) != 1 && != 0) { load }` |
| B10 | Return `0x8615B0`: MAPREG if battleMap != current, else restore only | **CONFIRMED** | decompile matches |
| B11 | Teardown `0x781590` frees the listed objects, after a sync wait-all | **CONFIRMED** | starts with `if (battle_state) { 0x76D0D0()->vtbl[2](0); ... }` |
| B12 | "Skip the flush: risk low" | **PARTLY** | No dangling pointers found: buffers are in the game pool, not a map-lifetime arena, and `0x640090`/`0x6403B0`/`0x6403C0`/`0x645860` do not touch chr entries. But per B7/B8 the residency is bounded only by 100 entries, and once full each new model evicts with a leak. **Fix:** when we skip the flush, keep our own budget. Call the real flush `0x840030` ourselves when the used-entry count passes a threshold (e.g. 80) or on a zone change. Never rely on `0x8409A0` |
| B13 | The 60-tick countdown | **CONFIRMED** (as exe-engine) | `[0x112A918]` is written only by the battle-script handlers `0x7C6E20` (toggle) and `0x7CE450` (set). So it is per-battle script data; whether random formations set it is runtime |

### C2. battle-load.md (exe-engine)

| # | Claim | Verdict | Evidence / correction |
|---|---|---|---|
| L1 | All loading runs serially on one thread, "AsyncLoadManager" (`[0xCCC88C]`, thread `0x691180` → proc `0x691360`, enqueue `0x6918B0`, idle `0x691630`) | **CONFIRMED** | String `0xB46FB8` is referenced by `0x691180`. `0x691360` runs each queued task's vtbl+4 then +8, one at a time. Idle = both list counts (+0x20, +0x2C) zero. **The CD reads are routed through it:** `0x76B970` (loader vtbl[0]) → `0x642950` → `0x6918B0`. `0x2310C24 = 0x641360() = [0xCCC88C]+0xC` (set in `0x76C560`) |
| L2 | Sync wait-all = vtbl[2] = `0x76C490(0)` → `0x76E660`; `0x781020` calls it on the random path | **CONFIRMED** | `[0x2310C48] = 0x76C490` (`0x76CFF0`). In `0x781020`, the wait at `0x78105E` runs only when `param_1 == 0`, and the random trigger at `0x790F93` pushes 0 (B2). **The caller list is not complete:** ~20 direct `call [0x2310C48]` sites exist (`0x76F850`, `0x770430`, `0x777F00`, `0x779730`, `0x77D670`, `0xA54AB0` ← `0x782090`), plus the chr-side wrapper **`0x82A5E0` (`jmp [0x2310C48]`)**, called with 0 by `0x825A40` and `0x8369A0` when a chr entry is still *loading* (lookup == 1), and by `0x836870`, `0x83B670`, `0xA795B0`. The stall sampler will catch whichever fires |
| L3 | Map free → `0x645860` → `0x670FE0` → `0x671300(TexList, -1)`: kick + full flush `0x5BC3F0`, then `0x69BE90` per texture | **CONFIRMED** (the -1 branch's loop is **M**) | `0x671300` begins with `ctx->vtbl[2]()` then `call 0x5BC3F0`. So each map free is also a main-thread wait for the render thread to drain (up to 16 lists) |
| L4 | ClusterManager: a hit is refcount++ with no I/O; release frees at refcount < 1 and keep == 0; shaders never freed | **CONFIRMED** | `0x69B940` (thiscall, `ret 4`): normalise path (`0x679820`), hash `0x67B7D0`; under the mutex a hit does `inc [entry+4]` and returns `*entry` (PCluster*). On a miss the mutex is **released during the load** (`0x4406C0`) and the entry is inserted only afterwards; failure returns 0. `0x69BE90`: skips +0x14 (shader) entries; else refcount--; frees if < 1 and +0xC == 0 |
| L4a | Plan (2): "call the original once more = cache hit, refcount+1" | **CONFIRMED mechanically, with caveats** | (a) Call it only after the first call has returned (a miss inserts at the end). (b) The tree key is the **hash** only. (c) Calling the full Fahrenheit chain from inside our own detour re-enters our hook, so add a re-entrancy guard. (d) Release on the main thread while L is idle, as proposed |
| L4b | The keep flag (+0xC) is "dormant" | **PARTLY** | `0x640090` runs on every map free (`0x909EA0`). Except for event ids 0x17/0x9F or `[0xCCB468]`, it calls `0x69AE80`, which in state 2 sets keep = (+8 != 0 && +0x10 == 0) on all entries and switches to state 0. `0x69BB80` sets +8 for `.dae` + `chr` paths. So **HD chr clusters already get a 12 s (state 0) / 3 s (state 1) grace** after each map free (freed by `0x69A6B0` from `0x69C010`). Runtime will tell whether this matters (it raises the memory peak during the battle-map load) |
| L5 | Gameplay is skipped while the loader is busy (`0x81FCD0`) | **PARTLY** | `0x820CE9: call 0x81FCD0; jne 0x8216B9` (skips to near the end of Sg_MainLoop). `0x81FCD0` returns 1 for `!loaderIdle && renderer[+0x123A0]`, **and also** for two other conditions (`0x641BF0`/`0x641BD0`, `0x641BC0`/`0x642610`). "Frames keep presenting" follows from the PApplication step still running the renderer (**M**) |
| L6 | The countdown at the top of `0x783020` | **CONFIRMED** | `cmp byte [0x112A918],bl` at `0x783023`; 0x3C is written to `0x112A919` in states 10, 0xC and 0xF |
| L7 | The exe is LAA | **CONFIRMED** | Characteristics 0x122 (same file as the uploaded FFX.exe) |
| L8 | Option (1): "the chr pool is a fixed 17 MiB arena with LRU eviction, so no address-space growth" | **REFUTED** | See B6-B8 |
| L9 | A failed open shows a MessageBox and quits | **CONFIRMED** | `0x679730`: `if (!0x607F20()) { ... MessageBoxW(.., L"error", 0x10); DAT_00ccc7bc = 1; 0x42F4D0(); }` |
| L10 | The file API and EFL description (§1.2) | **CONFIRMED** | `0x679800` → `0x608090`; `0x679510` close; sceOpen `0x62FA30` → `0x679730` (Part A1) |

### C3. Consequences for the round-2 design
1. **"Skip the chr flush" is still the cheapest win, but it needs its own bound.** Count the used entries (100 at `0x1302F98`, `+0xC != 0`). When skipping, flush for real past ~80 entries and on every zone change. Without that, a long session in one zone fills the table, and each eviction leaks an HD character (VRAM and clusters).
2. **Holding extra ClusterManager references (plan 2) is mechanically sound.** It interacts with the existing chr delayed release (L4b), and it needs the re-entrancy guard and the 2 GB-aware budget from Part B.
3. **A byte cache must not fake opens** (L9, Part A3). Serve reads only.
4. **The hard stalls are wider than 9 call sites** (L2). Instrument `0x76C490` itself (FhUtil `0x36C490`), which catches every caller (the wrapper `0x82A5E0` too), rather than the listed functions.
