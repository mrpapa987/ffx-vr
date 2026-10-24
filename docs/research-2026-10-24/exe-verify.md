# Independent check of the FFX.exe findings (exe-verifier, 24 Oct 2026)

I checked the claims in `exe-battle-ui.md` and `exe-engine.md` that the cursor design depends on. I did not write
those reports. I did not use the authors' helper scripts or their xref database. My evidence comes from:
- my own objdump of `re/bin/FFX.exe` (`research/exe-verify-work/dis.txt`)
- a byte scanner that finds every E8/E9/0F8x rel32 branch and every absolute 4-byte reference in any section (`refs.py`)
- a constant reader (`rd.py`)
- the Ghidra decompile in `re/out/decomp`
- Fahrenheit `re/fahrenheit-src` (ecb6702)
- the FFX-60fps source in `ffx60/repo`. The source is here, even though the brief said we can't see it. I cannot check that the shipped DLL matches it.

Addresses are given as `VA / FhUtil` (FhUtil = VA - 0x400000). The PE header gives ImageBase 0x400000 with
DllCharacteristics 0x8140, which **includes DYNAMIC_BASE**. So the exe can be rebased at runtime: hard-coded VAs are
only valid after the rebase, and FhUtil offsets are the safe form.

Verdicts: **CONFIRMED**, **PARTLY** (true, with a correction or caveat), **REFUTED**, **UNVERIFIABLE-STATICALLY**.

---

## 1. 0x89C870 is the target-pointer draw callback, reached from one call site — CONFIRMED

- **References.** A full-image scan found exactly one reference to 0x89C870: `call 0x89C870` at `0x8A84C0`. There are no absolute refs.
  - `0x8A84C0` is the stub `E8 AB 43 FF FF / 33 C0 / C3` (call, then return 0).
  - The only absolute ref to the stub is `0x8A10C4`, inside `mov dword [eax+0x90],0x8A84C0` (`0x8A1090`).
  - `0x8A1090` is called only from `0x89A8A5` (inside `0x89A860`).
- **Dispatch.** The stub is run by the window loop in `0x89B360`:
  ```
  89b476: cmp byte [esi-0x8f],3 ; jl skip     ; window+0x01 (state) >= 3
  89b47f: mov ecx,[esi]  ; test ecx,ecx       ; window+0x90
  89b485: lea eax,[esi-0x90] ; push eax ; call ecx ; add esp,4
  ```
  - The loop walks 8 windows (`0x133C950..0x133D160`, stride 0xF0).
  - The whole block is gated by `[0x23CC08C]!=0 && [0xC58EF8]==1`, and before that by `battle_state ∈ {1,2}` and `[0x133C948]!=0`.
  - **Caveat:** in theory the callback runs once for *every* window whose +0x90 is the stub. In practice there is one target window, but a detour should not assume exactly one call per frame.
- **Body (disassembly 0x89C870..0x89C8F8).**
  ```
  cmp dword [0x133d6e0],1 ; je ret            ; auto-battle -> nothing
  edi=0, esi=1
  loop: test [0x133d178],esi ; je next
        push &y ; push &x ; push edi ; call 0x795920   ; x=Chr[i]+0xF34, y=Chr[i]+0xF38
        call 0x9046d0                                   ; <- just `ret` (empty PS2 leftover)
        push (edi<0x14 ? 4 : 5) ; push float y ; push float x ; call 0x8C0670 ; add esp,0xc
  next: rol esi,1 ; inc edi ; cmp edi,0x1c ; jl loop
  ```
  - `0x795920` reads `[eax+0xf34]` and `[eax+0xf38]` of `MsGetChr(id)`. `0x794020` returns `[0x11334D4] + id*0xF90` for ids below 0x1F.
- **Quad placement.** `0x8C0670` (TkMn2DrawCrossCursor [Fh]) computes it. The constants were read from the PE:
  - a = 1920 x 1/512 = 3.75, and b = 1080/416.
  - Type 4 (party): `x_ps2 = x/3.75 + 256 - RemapX2(100)` and `y_ps2 = y/b + 208 - RemapY2(22)`.
  - Type 5 (monster): same, but with no -100 term.
  - Both call `0x903BB0(400, x, y, RemapX2(68), RemapY2(45), u0, v0, u1, v1)`. Its rectangle is (x,y)-(x+w,y+h) (`faddp` at 0x903BE4/0x903BF9).
  - `0x63EEE0` (PS2 flag = 1) converts back with `x*3.75-960` and `y*2.596154-540`.
  - Result: party x ∈ [ax-100, ax-32]; monster x ∈ [ax, ax+68] with u0 = 0.619 > u1 = 0.553 (mirrored); y ∈ [ay-22, ay+23] for both. No time term for types 4/5. **All exactly as claimed.**

## 2. Anchor = Actor.chr_optpos[Chr+0xDF9]; Chr+0xDF9 set by stat 135 — CONFIRMED (plus a default)

- **Where the anchor is projected.** `0x79F380` at `0x79F412`:
  ```
  movsx eax, byte [edi+0xdf9] ; push eax ; push esi(actor) ; call 0x8354F0
  ```
  If the Chr has no actor, it calls `0x794C20(id,&out)` instead.
- **Fallbacks in `0x8354F0`.** If x, y, z and w of `actor+0x524+16*idx` are all 0, it uses optpos[0] (+0x524). If `+0x530` (optpos[0].w) is also 0, it uses `actor+0x200..0x208` with w=1. As claimed.
- **Fahrenheit agrees:** `Actor.chr_optpos` @0x524 (26 x Vector4) and `Chr.stat_cursor_element` @0xDF9.
- **Writers of byte +0xDF9 (whole exe).** There are only two:
  - `0x7B5836`: `mov [esi+0xdf9],bl` in MsSetChrStatInfo `0x7B4B70 / 0x3B4B70` (Fahrenheit agrees). The decompile shows it as `case 0x87` = stat 135.
  - **`0x79C1F4`: `mov byte [esi+0xdf9],1` in the Chr initialiser `0x79C110`. Not mentioned in the report.** The default cursor anchor is therefore **optpos[1]**, the same point the turn pointer uses. Monster scripts override it with stat 135.
- **New caveat for the design: optpos is computed at 30 Hz and is not smoothed by FFX-60fps.**
  - optpos is filled by `0x8350F0`, which runs inside the per-actor job `0x833530` (see 7): `optpos[i] = bone[i] (+0x328+b*0x160+0x88) x root(+0x1D0) x local offset`.
  - The job runs during Ch_CalcMain, from the canonical tick-k root.
  - SMOOTH-002 interpolates only `+0x1D0` (`ROOT = 0x1D0`, SMOOTH-002.cpp:16). optpos is never interpolated.
  - So a marker placed at optpos, including the game's own pointer anchor, sits at tick k. On logic frames the actor's body is drawn at lerp(k-1, k, t). Expect up to half a tick of relative step while an actor moves. **M** (inference from code plus the FFX-60fps source).

## 3. 0x892590: turn pointer is 3D geometry above optpos[1], rotated by [0x133FA7C], gated — CONFIRMED

- **Anchor.** `0x7950B0(chr, &0x133FA68)` = `0x8354F0(actor, 1, out)` followed by `out.y -= 2.0` (`[0xB92270]`=2.0).
- **Vertices.** `V_k = A + (sin θ * r_k, dy_k, cos θ * r_k)`, with θ = `[0x133FA7C]`.
  - `0x4CEAE0` = `_CIsin`, `0x4CEAC0` = `_CIcos`.
  - The table is written by `0x897860` (an fld/fst sequence I traced by hand): (r,dy) = (0,-0.3), (-1,-2.3), (+1,-2.3), constants `0xB49998`=-0.3, `0xB92340`=-1, `0xB5F048`=-2.3.
- **Projection and submit.** Each vertex goes through `call 0x6428F0`. A vertex continues only if 1/w > 0, otherwise the function bails out. Then `0x901190(x0,y0,x1,y1,x2,y2,[0x133F7D4],[0x133FA88])`.
  - **The colour assignment is now H, not M:** in `0x901190`, vertex 0 (apex) takes param_8 = `[0x133FA88]` ("dum76"); vertices 1-2 take param_7 = `[0x133F7D4]` ("dum75").
  - Submit: `0x63EEE0(...,tex 0, PS2 flag 0, kind 1, 0)`, i.e. already in 1920x1080 centred units.
- **Spin.** `0x890F30`: θ += 0.10471975 (`0xB5EE38`), then θ -= 2π if θ > π. It is called only from `0x89AE98` in `0x89AE20` (UI update, called from battle logic `0x790D74`), and only if `battle_state==1 && [0x23CC08C]!=0 && [0xC58EF8]!=0`.
- **Gate (0x89B360).** `[0x133C948]!=0`, then battle_state (`0x7816F0` = `[0x112A8E0]`) ∈ {1,2}, then `[0x23CC08C]!=0 && [0xC58EF8]==1`, then `[0x133D6E0]==0` (0x89B43F). Matches.
- **Precision note.** This is "3D" only in that the vertices are world-space. The result is a screen-space 2D triangle in the UI pass, not geometry in the scene. "Above" assumes PS2 y-down (`0x6526A0` negates x and y into Phyre space), which is consistent.

## 4. 0x6428F0 is the shared projection; 0x79F380 writes Chr+0xF34..0xF58 each frame — PARTLY

- **0x6428F0: confirmed.**
  - The code is `mov ecx,[0xCCB9D8]; push 1; push [ebp+0xC]; push [ebp+8]; call 0x6526A0; ret` (cdecl, 2 arguments, returns 1/w in ST0).
  - `0x6526A0` is thiscall (`ret 0xC`). It builds (-x,-y,z,1), multiplies, and divides by w. It outputs `ftol(1920*ndc.x*0.5)` and `ftol(-ndc.y*1080*0.5)` and returns 1/w.
  - Mode 1 uses `cam+0x74`, unless `[0xCCB9C0]==0`, in which case it rebuilds from `*(cam+0xB4)` and `0x651650`.
  - Callers (8 sites): `0x795250`, `0x796C30`, `0x79F290`, `0x79F380` (x2), `0x892590` (x3).
- **Not seen in the report:** when w < 0, `0x65281C..0x652834` **negates** x/w and y/w. So a point behind the camera lands at x/|w| rather than mirrored, and exe-battle-ui Q7's "mirrored position" is not what the code does. The pointer is still drawn off-target, because 0x89C870 has no w check.
- **0x79F380: the range is wrong.** It writes F34, F38, F3C, F40, F44, F48, F4C, F50 (dwords 0x3CD..0x3D4), not F54/F58.
  - F34/F38 = the raw ints. F3C = x/3.75+256, or -0x200 if 1/w <= 0. F40 = y/b+208. F44/F48 = damage anchor. F4C/F50 = clamped to [0x1B..0x1E5] x [0x22..0x187].
  - **F54/F58 are written by `0x795250`** (gauge anchor), as the battle-UI report itself says in Q2.
  - Only Chr 0..0x1E with byte `+0x10 != 0` are processed.
- **"Each frame" holds with conditions.** The chain is `0x89B360 -> 0x780CC0`, and `0x780CC0` runs `0x79F380` only if battle_state != 0. `0x89B360` runs only if `[0x133C948]`. `0x8ABD30` is called at `0x82153C` only if `[0x12FBBB4]!=0 && [0x12FB7D0]==0` (not skip-render).

## 5. Selection state is never cleared on command submit — CONFIRMED (with corrections)

- **Submit `0x8975A0`** (one caller, `0x89A365`, inside `0x89A330`):
  - zeroes `[0x23CC08C]`, `0x133C93C` and `0x133D74C..76C`
  - registers the command (`0x792D60(&0x23CC040,...)`)
  - sets `[0x23CC088]=0xFF` **only if `[0x133D6E0]==0`** (under auto-battle the inputting-chr id stays set)
  - does not touch `0x133D178`, the window array `0x133C950`, or `[0x23CC092]`.
- **Every write to `0x133D178` in the exe** (by instruction). None of them is a clear on submit:
  - `0x89894C` (0x898640, build lists)
  - `0x899BB4..0x899E6E` (0x899AC0, memory cursor)
  - `0x89A5F1` (0x89A330, auto-pick after a push)
  - `0x89AC8B` (0x89ABD0, target-window open: memset of the 0x140 block, then lists)
  - `0x89D50D` (0x89D390, control tick)
  - **not listed in the report:** `0x89C9AE` (0x89C900), `0x89CE85` (0x89CDE0), `0x8A241A` (0x8A22F0) and `0x8A27BA` (0x8A2690). These set `0x133D178 = 1<<chr` (and the command struct) for self-target commands, without a target window.
- **Correction:** `0x898130` (listed as a memset site) has **no references** (no rel32, no absolute). It is dead code. The only clear is `0x89ABD0`, called only from `0x89A87A` (the target-window open).
- **Window +0xD8 writers** (`[reg+0x133CA28]`): `0x89899B`, `0x899CCE`, `0x89A898`, `0x89AC11` (=0), `0x89D564`, plus dead `0x898171`. Matches.
- **Full reset:** `0x89BB10` (memset window array 0x780, index 0xFF, command struct 0x48) runs only when `battle_state==1 && [0x23CC08C]==0`, i.e. at the next party input.
- **So the mod's stale markers follow from this:** after submit, `cur_window` (by `[0x23CC092]`) +0x08 / +0xD8 and `0x133D178` keep their values through enemy turns. The correct gate is `[0x23CC08C]!=0`, which the draw loop itself uses.

## 6. Atlas UV claims — PARTLY (CPU side confirmed; texture file and GPU slot cannot be checked statically)

- **Confirmed: id 400 maps to battle.dds.**
  - In `0x903BB0`, `id-0x190 <= 0xC6` → `0x8AC8C0(0x3F00)`. For 0x3F00 that function returns the battle.dds pointer of the language set (`0xC59D58` → `/FFX_Data/GameData/PS3Data/menu/D3D11/battle.dds.phyre`; `0xC59E50` → `menu_us/...`).
  - For ids 400-598 the UVs pass through unscaled (`id-0x258 > 0x31`).
- **Confirmed: UV constants.**
  - `0xB5F0FC`=0.552734375, `0xB5F108`=0.619140625, `0xB5F0EC`=0.5107421875, `0xB5F100`=0.5546875, `0xB5F0CC`=0.4521484375, `0xB5F0E8`=0.49609375.
  - On a 1024 atlas these are party (566,523)-(634,568) and monster (566,463)-(634,508). They are exact multiples of 1/1024.
- **Confirmed: CPU vertex streams in `0x63EEE0`.**
  - mesh[3] = pos (x, y, 1.0), stride 12
  - mesh[4] = (0,0,1), stride 12
  - mesh[5] = colour, int/255.0 (`0xB170C0`), stride 16
  - mesh[6] = UV (u, 1-v), stride 8
  - mesh[7] = u16 indices 0,1,3,3,1,2
  - So the UV is the 4th CPU array.
- **Cannot be checked statically:**
  - That battle.dds is 1024x1024 BC3. The file is not here; the UVs are normalised, so the texel figures assume 1024.
  - Which D3D input slot holds the UV array. That needs the input layout or shader, or one runtime dump.

## 7. Two threads; render thread 0x615140/0x616010; Present = command 0x16 -> 0x593760; logic on main thread — PARTLY

- **Confirmed: the render thread.**
  - `0x615140` (called from `0x62AA40` in framework init `0x62A600`, the "MainThread" string) creates "PhyreEngineRenderThread" with `CreateThread` (`0x4300B0`), entry `0x614710` → `0x616010`.
  - `0x616010` names itself "RenderThread", waits, then runs the interpreter `0x615550` → `0x614C60`.
  - `case 0x16: 0x593760(); 0x430210(1,0)`. `0x593760` = `swapchain->vtbl[8]` (Present)`(sync = [r+0x131C]!=0, 0)`.
  - The only other ref to 0x593760 is the thunk `0x6148B0` (`jmp`), which has no references.
  - Present is encoded by `0x5B6AF0` from `0x627940` (PApplication step) and `0x66B1E0`/`0x66B2E0` (loading).
  - Sg_MainLoop's only static ancestors are `0x822840` ← `0x42F520` (app vtable `0xB0D998`+0x10, called by `0x627940` ← `0x623660`) and `0x820680`.
  - The render thread's static call graph (282 functions) does not reach Sg_MainLoop, `0x89B360`, `0x89C870` or `0x892590`.
- **Correction: there are more than two threads, and some game logic runs off the main thread.**
  - The job system object is `[0xCDEDE4]` (FhUtil 0x8CDEDE4). Its "JobWorker" threads (string `0xB4CF38`, vtable `0xB4CF30`) run `0x703A20`: an endless loop that dequeues `0x6DB8A0` and runs `0x703910`. They are started through the generic thread class (`CreateThread(0x679DB0)` → vtbl[1]).
  - **Ch_CalcMain `0x832E10` dispatches the per-actor update asynchronously:** `0xA44B90` → `0x71E720(0x833530, batch...)` and returns without waiting.
  - `0x833530` → `0x832760` **writes actor +0x1D0**; `0x8350F0` **writes optpos +0x524**.
  - The joins (`0x6DBF50`) are:
    - the next Ch_CalcMain start (`0xA44B80`)
    - a **conditional** join at the end of Ch_DrawMain (`0x82F881`, `if (DAT_01a85f54 < n*4 || DAT_01a85f54 == 0)`)
    - Sg_MainLoop at `0x821656`, **after** the battle-UI draw `0x82153C`
    - the renderer `0x71E810` (from `0x642BB0`/`0x668930`)
  - The FFX-60fps source names the same "skeleton job worker threads" (SMOOTH-003.cpp: dispatch 0x00A44B90, sync 0x00A44B80).
  - Other threads exist too: PhyreEngineWorkerThread, PhyreEngineBulletThread, video threads.
- **Consequence for the detours:** they read optpos/+0x1D0 at the same point the game's own `0x79F380` does, so they are no worse than the game. They are not provably race-free when the Ch_DrawMain join is skipped. Copy the values rather than holding pointers.

## 8. Actor +0x1D0 = interpolated drawn root (FFX-60fps); +0x0C = 30 Hz logic position — PARTLY

- **What the game does** (code, H):
  - `+0x1D0` is the actor's 4x4 world matrix. `0x832760` builds it once per logic tick (inside the JobWorker job from Ch_CalcMain) from scale `+0x5C`, heading `+0x158`, and translation **`+0x0C` (chr_pos_vec) + `+0x6C` (chr_offset_vec)** at floats 12..14.
  - If `+0x10C` is non-null it copies that matrix instead. If attached (`+0x19C` parent), it is derived from the parent's bone (`+0x328 + b*0x160 + 0x88`).
  - So even without FFX-60fps, the translation of +0x1D0 is not +0x0C: it adds chr_offset_vec, and attached actors differ completely.
  - The game reads +0x1D0 in Ch_DrawMain (`0x82EF26`, `0x82EF85`, `0x82FB46`) and when building optpos (`0x8350F0`).
- **What FFX-60fps does: confirmed against its source, not its binary.** `SMOOTH-002.cpp` has `ROOT = 0x1D0`. It patches the Ch_DrawMain call sites, writes the lerp into the root, and restores at the next invocation's `pre()`. It never touches +0x0C (no reference). Whether the shipped `fps60` DLL matches this source cannot be checked here.
- **Not interpolated:** optpos (+0x524). See 2.

## 9. Loading loop 0x76E660 presents loading frames gated by [[0xCC9CD8]+0x3A0] — CONFIRMED

- `0x641C40` = `[0x623B90()]+0x3A0 != 0`, and `0x623B90` = `mov eax,[0xCC9CD8]`.
- **Flag clear:** `0x6F2D80(DAT_02310c24)` = `WaitForSingleObject(INFINITE)`, so nothing is presented.
- **Flag set:** it polls with 1 ms waits (`0x6F2D50`). It calls `0x642C00` → `0x66B1E0` only when renderer bytes `+0x1197` and `+0x1196` are both 0.
- `0x66B1E0` draws the icon/Iggy layer (`0x6E50A0`, `0x6E4EA0`, `0xA267F0`), encodes Present (`0x5B6AF0`), kicks, calls app vtbl+0x28 (`0x42F4F0`), pumps messages (`0x642860`) and calls `Sleep(0x3C)`.
- `0x66B1E0` also has a `+0x3A1` path, but `0x76E660` gates on `+0x3A0` only.

## 10. 0x599210 re-acquires the backbuffer (ResizeBuffers + GetBuffer) — CONFIRMED

`0x599210` (thiscall) does, in order:
1. `ctx->vtbl[0x84/4=33]` OMSetRenderTargets(0,0,0)
2. release views (`0x59C650` x2)
3. `swap->vtbl[13]` **ResizeBuffers**`(count=[+0xA0], w, h, fmt=[+0x88], flags 2)`
4. `swap->vtbl[9]` **GetBuffer**`(0, IID@0xB31470, &tex)`
5. wrap into the RT object `0xCA310C` and depth `0xCA347C`, then set the viewport
6. OMSetRenderTargets(1, ...) again

Callers: `0x62C260` (app vtbl `0x42F9A0`) and `0x62C440`. `0x62C440` runs the flush `0x5BC3F0` first (0x62C4ED); for the `0x62C260` path the flush is elsewhere (per exe-engine, in the PApplication step). Main thread: **M** (app methods; not traced to the message pump).

---

## Hookability of the two detour targets

| | 0x89C870 / FhUtil 0x49C870 (target pointer) | 0x892590 / FhUtil 0x492590 (turn pointer) |
|---|---|---|
| First bytes | `55 / 8B EC / 83 EC 0C` (6 bytes, 3 whole instructions), then `83 3D E0 D6 33 01 01` | `55 / 8B EC / 83 EC 28` (6 bytes), then `A1 D8 13 C6 00` (cookie) |
| 5-byte jmp | Safe. MinHook relocates 6 bytes (3 instructions) to the trampoline; nothing position-dependent. Internal branches go to 0x89C888 / 0x89C8C8 / 0x89C8E0 / 0x89C8F5, none into +0..+5. The scanner found no rel32 or absolute refs to 0x89C871..0x89C875 | Safe. Branches go to 0x892727 / 0x892729 / 0x89272A; no refs to 0x892591..0x892595. The bytes before 0x892590 are the previous function's jump table (entries 0x89233B...), none pointing here |
| Convention | cdecl (equivalently stdcall: no arguments). Plain `ret`; preserves esi/edi; reads no `[ebp+8]` | **cdecl, 1 argument**: the caller does `movsx eax,word [0x23CC088]; push eax; call; add esp,4` (0x89B448); the callee reads `[ebp+8]`, plain `ret` |
| Arguments | none (the stub receives the window pointer but drops it; `ecx` = the callback address, unused) | `int chr_id` (sign-extended u16) |
| Return | eax is junk; the stub replaces it with 0; the dispatcher ignores it | none used; the caller does not read eax/ST0 |
| Call sites | 1 (stub 0x8A84C0, from window+0x90; the stub may run once per window with state >= 3) | 1 (0x89B450) |
| FFX-60fps | No row touches it (nearest patch: 0x89C9F5 in 0x89C900) | No row touches it |
| Fahrenheit | `call_4.g.cs` has `FUN_0049c870` typed `d_UnknownFn` (cdecl void()); it is the right signature | `call_4.g.cs` has `FUN_00492590` as `d_UnknownFn`: **wrong signature**. Calling the original through that handle pushes no argument, and the original then reads garbage chr from the stack. Declare `[UnmanagedFunctionPointer(Cdecl)] delegate void d(int chr)` |
| Skipping: what else is lost | Only the pointer quads. Every callee is read-only or draw-only: `0x795920` reads; `0x9046D0` is `ret`; `0x8C0670` → `0x903BB0` → `0x8E5A20` (clip, writes only through its arguments) + `0x63EEE0` (append to the dyn-geo mesh). No sound, no window or input state, no other sprites | Only the triangle, plus a write to the scratch global `0x133FA68..74`, which only this function reads. The spin θ is advanced elsewhere (`0x890F30`, logic tick), so skipping does not freeze it |
| Caveats | (a) Pointer quads share batched meshes with neighbouring battle.dds sprites (CTB rows). Skipping them changes index counts and grouping of the following draws, so any D3D-side heuristic keyed on draw sizes must be rechecked. (b) It is called while the window is closing too (state 5/6) | (a) If the chr has no actor, `0x7950B0` leaves `0x133FA68` stale and the triangle is drawn at the old anchor. (b) Under auto-battle the call is skipped by the caller |

MinHook (Fahrenheit's `FhCallHandle.hook`, `handles.cs` → `MH_CreateHook`) handles both prologues. Install the hooks at module init so they are queued; Fahrenheit forbids late insertion on an already-hooked function. Use FhUtil offsets: the exe is DYNAMIC_BASE.
