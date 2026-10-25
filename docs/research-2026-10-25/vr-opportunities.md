# VR improvements beyond load times and GPU perf: ranked designs (vr-opportunities, round 2, 24-25 Oct 2026)

Static analysis only (Ghidra export re/out, objdump of re/bin/FFX.exe, Fahrenheit ecb6702, FFX-60fps source), plus
the code on branch `cursors` (7f38cbf). Nobody can run the game here.

**Addresses.** VA uses image base 0x400000. **FhUtil = VA - 0x400000** (the exe is DYNAMIC_BASE, so use FhUtil only).
**Confidence:** [code] = the decompile or disassembly says so; [infer] = my reading; [runtime?] = needs one in-game check.
**Coordination.** exe-engine answered three questions on the stalls (section 1.2; credited inline). perf-architect
(R25, pacing) agreed the split: R25 stays theirs, and nothing here changes xrWaitFrame pacing in normal play.

---

## 0. Ranking

| Rank | Item | Value | Effort | Risk | Why this rank |
|---|---|---|---|---|---|
| **1** | **Loading black screens: render-thread keepalive** (item 2; section 1) | high (the owner's most visible VR defect; also covers the airship menu) | S-M (1 day) | medium (VDXR behaviour, unknown main-thread context users; both have backstops) | No second device and no new OpenXR thread. One call-site patch plus two small pre-hooks. |
| **2** | **Rumble -> Quest haptics** (item 1; section 2) | medium-high (cheap immersion; the game rumbles on hits, summons, Sin) | S (half a day) | low | Hook already installed. New finding: the PC port sends rumble as on/off; we can recover the PS2 intensity. |
| **3** | **3D damage numbers and status pop-ups** (item 5, pick 1; section 3) | high in battle (the digits float off their targets on the head-locked panel) | M (1-2 days) | medium (pairing the queue with the damage list) | Same pattern as cursor C2/C3, which the owner called "beautiful". All data sources are now known. |
| **4** | **Battle diorama auto-framing, incl. bosses** (item 5, pick 2; section 4) | medium-high | S-M | low | Managed only. Uses the per-Chr heights found in round 1. |
| **5** | **Per-area view memory** (item 5, pick 3; section 5) | medium | S | low | Managed only, uses the build-8 settings file. |
| 6 | **FMV on the virtual screen: instant switch, crop, correct aspect** (item 3; section 6) | medium-low (the movies already show; this fixes edges and aspect) | S (crop) / M (native YUV, not recommended) | low | A native-resolution grab gains little at the screen's angular size and loses the subtitles and skip prompt drawn after the video. |
| 7 | **Dialogue boxes near the speaker** (item 4; section 7) | medium (first person), low (diorama) | M-L | medium-high (speaker heuristic, box rect) | The worker->actor link is now known exactly ([code]). Needs a runtime survey before building; do v0 (field talk only) first. |
| 8 | Camera-cut / spell-camera signals; swimming eye height; overdrive aids; full presenter (tier B) | low-medium each | S-M / M / ? / L | - | Section 8 has notes and what to capture. |

Suggested order: 1 (with its diagnostics) -> 2 -> 4 -> 5 -> 3 -> 6 -> 7 (survey first). Items 2, 4 and 5 are independent
and could share one build. Item 3 should go to whoever owns the battle UI in round 2 (it reuses C3's billboard path).

---

## 1. Loading black screens: the render-thread keepalive (rank 1)

### 1.1 Why the old plans don't apply
- The build-7 watchdog saw the main thread waiting in ntdll/win32u with `app+0x3A0 = 0`, so the "present during loads"
  flag trick (0x76E660 -> 0x66B1E0) does not fire for these stalls.
- exe-engine's static answer (24 Oct, round 2), quoted in short. The best candidate is the libmscd synchronous wait-all:
  - The call chain is loader vtable[2] = 0x76C490(0) -> 0x76E660.
  - It blocks in WaitForSingleObject(INFINITE) on the AsyncLoadManager batch-done event ([0xCCC88C]+0xC).
  - It is called from MsBattleExe 0x781020 (battle setup), 0x781590 (teardown), 0x781700, 0x782050, 0x782740,
    0x7848B0, 0x786B00, 0x79E6A0 and 0x88E070.
  - All loading runs serially on ONE AsyncLoadManager thread (0x691180, proc 0x691360): CD reads, map scene jobs,
    TexList texture jobs and ClusterManager loads. D3D11 resources for loads are created there through the device,
    **not on the render thread**. No load path pushes a long list to the render thread.
  - So during a stall the **render thread is idle in its ring wait**, except right after a texture-list unload
    flush (0x671300 -> kick + 0x5BC3F0).
  - Confidence: medium, not runtime-proven.
- The render thread is "PhyreEngineRenderThread", loop 0x616010. Our Present hook, xr_frame and OpenXR all run on it.
  With nothing in the ring it blocks INFINITE, so no Present arrives, no xrEndFrame is called, and VDXR blanks.
- The 22 Oct presenter design (own device, shared textures, its own OpenXR thread) is right for decoupling the display
  rate (tier B, section 8.4), but the black screens do not need it.

### 1.2 The idea
Make the render thread's idle wait a **timed** wait. While the ring stays empty longer than a threshold, the render
thread itself runs a "held frame":
- xrEndFrame re-sends the last base layer (R15's held layer, which points at swapchain images already released), with
  the poses it was rendered from, so the compositor reprojects it, head-tracked;
- then xrWaitFrame -> xrBeginFrame -> xrLocateViews -> `xr_apply_poses`.

What this gives:
- All OpenXR calls stay on the one thread that makes them today. No ownership hand-off, no xrWaitFrame from two
  threads, no VDXR thread-affinity risk.
- Our code makes **no D3D call** in a held frame. Only VDXR's xrEndFrame touches the immediate context internally;
  design-notes observed VD binding its eye textures and drawing on the game's context.
- The ring wait is outside the interpreter 0x615550, so the render thread holds no half-built game state at that point.
- The one remaining concurrency question is the main thread (or the async loader) using the immediate context at that
  moment. It is handled by a gate and a backstop (1.4).

### 1.3 Hook points
| What | VA | FhUtil | Bytes / signature | Notes |
|---|---|---|---|---|
| **Ring wait call site** in the render loop 0x616010 | **0x616060** | **0x216060** | `E8 3B A2 E1 FF` = `call 0x4302A0` (PEvent::wait: `push -1; push [ecx]; call [WaitForSingleObject]; xor eax,eax; ret`) | ECX = &entry.ready (`lea ecx,[esi+0x18]` at 0x616057). Patch only this call site, NOT 0x4302A0, which the main thread uses too (e.g. in 0x615EA0). [code] |
| Full flush (main thread) | 0x615EA0 | 0x215EA0 | `55 8B EC 83 EC 4C`: 6 clean bytes, fastcall(ring) | Fills the ring entry itself (cmd 0x2C, callback 0x614700 = SetEvent) and waits; it does not go through 0x615210. Callers 0x5BC3F0, 0x5BC400, 0x638F90, 0x638FB0, 0x615F40. [code] |
| Kick (main thread) | 0x615210 | 0x215210 | `55 8B EC 56 8B F1`: 6 clean bytes, thiscall(ring, list, end, sem) | Only caller: the context vtable method 0x5B6C50. [code] |
| Interpreter (optional diagnostic) | 0x615550 | 0x215550 | - | Entry/exit counter to prove the render thread is idle during stalls. Not needed for the fix. |

**Patching the call site.**
- 0x616060 is 8-byte aligned, and the 5 patched bytes lie inside the qword 0x616060..0x616067. Write the new
  `E8 rel32` with one `InterlockedCompareExchange64` on that qword, after VirtualProtect. The render thread runs that
  instruction every frame; an aligned 8-byte CAS cannot tear it.
- Verify before patching. The old qword must decode to `E8` + a target equal to base+0x302A0. On a mismatch (another mod
  got there first) log "keepalive: call site not as expected, off" and do nothing.
- FFX-60fps does not touch 0x616010 (grep of its source: no hits).

### 1.4 The replacement function (native, ffxvr_native.cpp)
```
// fastcall: ECX = PEvent* (first member HANDLE); returns 0 like 0x4302A0; preserves ebx/esi/edi/ebp (ABI).
static int __fastcall rt_ring_wait(void* ev) {
    const HANDLE h = *(HANDLE*)ev;
    for (;;) {
        if (!keepalive_armed()) { WaitForSingleObject(h, INFINITE); return 0; }        // XR off / setting off: the game's behaviour
        const LONG since = now_ms() - g_present_ms;                                   // g_present_ms already kept by diag7_frame()
        const DWORD to = g_keepalive_on ? 0 : (since >= k_ka_threshold ? 0 : k_ka_threshold - since);
        if (WaitForSingleObject(h, to) != WAIT_TIMEOUT) { keepalive_leave(); return 0; }
        keepalive_tick();                                                             // blocks about one display period in xrWaitFrame
    }
}
```
- `keepalive_armed()`: `g_xr_want && xr_active() && running && g_frames > 0 && setting keepalive=1`.
- `k_ka_threshold`: `max(25 ms, 1.6 x the measured game frame time)`. At 60 fps that is 26.7 ms, about 1.9 display
  periods at 72 Hz. Normal frames never trip it.
- In keepalive mode the ring is polled with a 0 ms wait between ticks, so the first list after the stall runs at most
  one display period later.

`keepalive_tick()` (render thread):
1. **Gate.** If `g_main_owns_ctx` is set, Sleep(2) and return.
   - The flag is set in a **pre-hook of 0x615EA0** (main thread, before the 0x2C list is queued).
   - It is cleared in a **pre-hook of 0x615210** (the next ordinary kick); a depth counter keeps a kick inside a flush
     from clearing it.
   - Why it is race-free: the flush's 0x2C list can only run after the render thread has returned from our tick. So
     whenever a held frame starts after a flush, the flag is already set.
   - exe-engine confirms this is the only main-thread context pattern they have seen: resize 0x599210, and 0x671300's
     releases, both after 0x5BC3F0.
2. **Backstop** for context users nobody has enumerated (exe-engine could not rule out an UpdateSubresource on the
   async loader thread):
   - At XR start: `ID3D11Multithread::SetMultithreadProtected(TRUE)` on the game's context (QueryInterface from the
     immediate context; Windows 10+). Also create one `ID3DDeviceContextState`:
     `ID3D11Device1::CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &s)`,
     with fl = the game device's feature level.
   - Around xrEndFrame and xrBeginFrame in the tick: `mt->Enter(); ctx1->SwapDeviceContextState(s, &prev); ...;
     ctx1->SwapDeviceContextState(prev, nullptr); mt->Leave();`
   - Effect: any other thread's context calls wait for the whole block, and whatever VDXR binds can never leak into a
     half-set game state.
   - Do **not** hold the lock across xrWaitFrame.
   - ini `keepalive_mt_protect` (default 1). The cost is a critical section on every context call. perf-architect's
     D1 diagnostic build measures render-thread CPU per frame with `mt_protect_test=1` vs 0 (research/perf-plan.md
     sec. 3, D1e). D1a's T_XR_END timestamp also shows VD's compositing cost on our context.
3. **Held frame** (a new `xr_hold_frame()` in ffxvr_xr.cpp; it reuses xr_frame's tail):
   - poll_events;
   - if `g_frame_open`: xrEndFrame with the held base layer only. Use the projection layer with `g_held.pv` (the poses
     those images were rendered from) or the screen quad, depending on the last frame. **No 30-frame age cap in
     keepalive mode.** Drop the UI and ImGui layers: a stale menu panel during a load is worse than none;
   - xrWaitFrame;
   - xrBeginFrame;
   - xrSyncActions and xrLocateViews;
   - return the views. The tick then calls `xr_apply_poses(out)` and `xr_input_get(&g_vrpad)`.
   - Result: the first frame after the stall renders with a fresh head pose, and the controllers do not freeze.
   - The existing `g_held.have` / `ever_released` rules decide whether anything can be held. If nothing can, send an
     empty frame, as today.
4. **Dim** (optional, no D3D):
   - At instance creation, enable `XR_KHR_composition_layer_color_scale_bias` if the runtime lists it [runtime?: VDXR
     support unknown].
   - After 1.0 s of keepalive, ramp the held layer's colorScale to 0.4 over 0.5 s; restore it on the first fresh frame.
   - Without the extension: no dim (a loading card can come later from a static swapchain made once at session start).
5. Counters for the heartbeat and overlay: keepalive episodes, held frames, the longest episode, ticks skipped by the
   gate. Log one line per episode: `keepalive: 97 frames held over 1.35 s (gate skips 0)`.

`keepalive_leave()` (first list after an episode): reset the dim and log the episode line. The game's next Present runs
xr_frame normally. `g_frame_open` is true from the tick's BeginFrame, and `g_render_views` came from the last tick's
locate. These are the poses the post-stall frame was rendered with, since the camera override reads g_xr_* at
CB-upload time on this same thread, after the tick.

**Pacing (answer to perf-architect).**
- In normal play the ring is never empty longer than the threshold, so xrWaitFrame keeps pacing the game 1:1 inside
  Present. The fps = refresh recipe is unaffected.
- The game's own 0x66B1E0 loading frames (Present plus Sleep(60)) also work: keepalive fills the 60 ms gaps between
  them.

### 1.5 What it does not cover
- A stall **inside** the interpreter (a list that blocks on the render thread), or a blocked DXGI Present.
  exe-engine: none known, except the brief flush in 0x671300.
- The diagnostic: extend the build-7 stall watchdog so that, at a stall, it also logs whether the render thread is
  inside 0x615550 (the counter above) and walks the main thread's EBP chain. Game functions keep frame pointers, so the
  0x76E660 / 0x781020 / 0x671300 frames should show (exe-engine's suggestion).

### 1.6 Effort, risk, tests
- **Effort.** About 150 lines native (patch, wait, tick, the MT/state backstop) plus about 60 in ffxvr_xr.cpp
  (`xr_hold_frame`, extension check, dim), and the ini key and counters. 1 day including review.
- **Risks**, with mitigations:
  - VDXR dislikes re-sent layers for seconds. The R15 hold already re-sends up to 30 frames, so this is moderate.
    Fallback: cap the hold at N seconds, then send the virtual screen black.
  - A context user not covered by the gate: covered by the MT backstop.
  - MT protection cost: measured, and it has an ini switch.
  - The call site differs on another exe: verified before patching.
- **Tests:**
  1. **Simulated stall.** Debug button "stall the main thread 3 s": a Sleep(3000) inside the job-join post-hook
     0x644B30 (main thread, already hooked by build 7). Expect: the headset keeps the last frame, head-tracked, with no
     black; the log shows `keepalive: ~216 frames held over 3.0 s` at 72 Hz; the game resumes normally.
  2. A random battle in an area (Besaid / Kilika) and Cid's airship destination menu: no black. The log shows an
     episode per transition.
  3. Resolution change / alt-enter in the menus while in VR (the 0x599210 resize path, main thread after a flush): no
     crash, and the gate counter rises if a tick coincided.
  4. Autosave chocobo / boot (0x66B1E0 path): smooth.
  5. Perf: CPU ms per frame on the render thread with `keepalive_mt_protect` 0 and 1 (field and blitzball scenes).
  6. Recentre during a stall (Meta button): the reference follows; `g_held.have` is cleared by the space change, so it
     goes black only until the next fresh frame.

---

## 2. Rumble -> Quest controller haptics (rank 2)

### 2.1 What the exe does (new detail; [code] unless marked)
- **Game side (PS2 pad emulation).** Pad work block at **VA 0x1330288 + port*0x100** (FhUtil **0xF30288**), from
  0x888EC0:
  - +0x9D = vibration enable. It is copied from the **in-game Vibration option**, the savedata config word
    VA 0x113079C / FhUtil 0xD3079C (save_data FhUtil 0xD2CA90 + 0x3D0C), **bit 4 (0x10)**, by 0x872480 / 0x8724F0
    via 0x8894D0. Fahrenheit names that bit `config_vibrate`.
  - +0xFE = large motor and +0xFF = small motor, set once per logic tick by 0x872BD0 (and 0x8739A0) from the vibration
    player's outputs **VA 0x1328271 (large) / 0x1328270 (small)**, mute byte 0x1328272 (FhUtil 0xF28271 / 0xF28270 /
    0xF28272), times 2 (0x875E10). So the values are 0..254.
- 0x888F90: if +0x9D, call 0x630E20 -> **0x6329C0** (FhUtil 0x2329C0). That checks a per-port enable in the app object
  and `[input+0xA8] != 0` (the same flag 0x631320 that picks pad vs keyboard glyphs for the FMV skip prompt, so
  probably "a gamepad is the active input" [infer]), then 0x62CB70 writes the Phyre pad +0x58 = large, +0x59 = small
  [code; argument order M].
- **Phyre pad poll 0x62DBA0** (FhUtil 0x22DBA0) calls `XInputSetState(pad[+0x3B0], {L = round(+0x58 * 65535.0),
  R = round(+0x59 * 65535.0)})` **every poll**. The constant at 0xB170D8 is the double 65535.0, and the product is
  truncated to 16 bits. So **any nonzero byte b becomes 65536 - b**: 65535 for b = 1, 65282 for b = 254. The PC port
  rumbles on/off at full strength. The PS2 intensity is still recoverable: `b = v ? 65536 - v : 0`. The build-7
  `xinput set:` log lines should show exactly such values (a cheap runtime check).
- Thread: the pad poll is a Phyre input-object method (no direct callers; vtable). It most likely runs on the main
  thread [infer]. The design below does not depend on which thread it is.

### 2.2 Design
**Capture.** Keep the build-7 IAT hook (`install_xinput_set_hook`) but install it regardless of `diag_build7` when
`haptics != 0`. In `xi_set_state_hook`, for pad 0 only (the VR pad's slot):
`g_rumble_l = v->wLeftMotorSpeed; g_rumble_r = v->wRightMotorSpeed; g_rumble_tick = GetTickCount();` as aligned 32-bit
stores (no lock), then pass the call through to the real XInput so a physical pad still rumbles.

**Fallback source.** If no SetState call has arrived for 500 ms while the session runs (for example the +0xA8 gate is
closed because the game thinks the keyboard is in use), read the game side directly at xr_tick:
- `enable = [0xF30288+0x9D]`
- `b_large = [0xF30288+0xFE]`
- `b_small = [0xF30288+0xFF]`

This still respects the in-game option, because +0x9D is the option bit. Byte reads; a race is harmless.

**Apply (render thread only).** All xrApplyHapticFeedback calls stay in `xr_tick` after `xr_frame`. They never run on
the input thread: VDXR's thread safety for haptics is unknown, and xrSyncActions runs on the render thread. Per hand:
```
b      = decode(v)                                   // 65536 - v, or the fallback byte
a      = (b == 0) ? 0 : clamp(gain * powf(b / 254.f, gamma), a_min, 1)   // defaults gain 0.8, gamma 0.5, a_min 0.15
mode "pc": a = (v != 0) ? gain : 0                    // faithful to what a PC pad feels
if a == 0:            if active: xrStopHapticFeedback(hand); active = false
else if !active || |a - a_last| > 0.05 || now >= renew_at:
                       xrApplyHapticFeedback(amplitude a, duration 150 ms, frequency UNSPECIFIED)
                       a_last = a; renew_at = now + 100 ms; active = true
```
- XInput motors are levels and OpenXR haptics are pulses. Renewing every 100 ms with 150 ms pulses gives a continuous
  buzz without flooding VD's network channel (about 10 messages/s per hand while rumbling).
- Add `XRFN(xrStopHapticFeedback)` to the function list (it is not loaded today).
- **Mapping:** left motor (large, low frequency) -> **left controller**; right motor (small, high frequency) -> **right
  controller**. This mirrors a pad's grips.
- Option `haptics_mode`: 0 off, 1 split (default), 2 both (max of both to both hands), 3 pc (on/off).
- Optional: give the small motor `frequency = 320 Hz` and the large `80 Hz`, if a runtime test shows VDXR honours
  frequency (Quest Touch Plus LRAs).
- **The in-game Vibration option is respected for free** (+0x9D gates the game side). Our own master switch is
  `haptics` (default 1), plus `haptics_gain`. The overlay shows the live motor bytes and the decoded amplitude.
- **VR pad mode:** works whether or not the VR pad is active. If a physical pad is connected and the VR pad is off,
  `haptics_when_vrpad_off` (default 0) keeps the Quest controllers quiet so only the pad rumbles.
- Shutdown / suspend: on session stop, focus loss (state != FOCUSED) or leaving VR, stop both hands.

**Mod-side haptic events** (cheap, optional, same function):
- A 30 ms tick on the target cursor moving to a new candidate: the C2 snapshot's mask 0x133D178 changes.
- A 2 x 40 ms double tick when a party member's turn starts: the turn snapshot's chr changes to a party id and the
  gate opens.

They are good VR feel, off-able (`haptics_ui`, default 1), and add no new hooks.

### 2.3 Effort, risk, tests
- **Effort** S: about 80 lines native, a few ini keys and an overlay row.
- **Risk** low. VD's haptics latency or pulsing artefacts are tuned by the renew period.
- **Tests:**
  1. Debug "buzz left/right" still works (existing buttons).
  2. Start a battle and take a heavy hit or cast a summon. Expect left/right buzzes, and the log (keep the build-7
     line) shows values like 65282, which decode to b = 254.
  3. Turn the in-game Vibration option off: no buzz.
  4. With an Xbox pad connected: both the pad and the controllers rumble (or only the pad with
     `haptics_when_vrpad_off=1` and the VR pad off).
  5. Sin's attack FMV and the Besaid temple quake (scripted rumble).
  6. Leave a long rumble running and press the Meta button: the buzz stops when focus is lost.

---

## 3. 3D damage numbers and status pop-ups (rank 3; item 5 pick 1)

**Problem.** Damage and heal digits, "Miss", and status icons over heads are 2D sprites the game places by projecting
the target through **its** camera. In the headset they land on the head-locked UI panel, far from the targets (VR
diorama and first person don't use the game camera). Same class of problem as the cursors, which C2/C3 solved.

**Data** [code; round 1 + mine]:
- Damage list head **[VA 0x11360BC]** (FhUtil 0xD360BC). It is a linked list (next at +0x10). Per node:
  - +0 bit 0x80 = use the clamped anchor; +1 kind (0 damage white, 1 heal green (0x4A,0xA0,0x58), 2 ...);
  - +2 chr id; +3 / +6 / +7 animation step and length; +4 digit / glyph; +5 = skip; +8/+0xA s16 extra offset.
  - The bounce table is **VA 0xC424B0** (3 bytes per step: dx, dy, alpha).
- 0x79F660 (FhUtil 0x39F660) turns each node into a screen position. The base anchor is Chr+0xF44/F48 (or the clamped
  +0xF4C/F50), and the node's offsets are added. It pushes into the **popup queue VA 0x133F110** (FhUtil 0xF3F110):
  16-byte entries {s16 x, s16 y, int kind, int glyph, u8 r,g,b,a}, with the count as an **s16 at VA 0x133F108**,
  max 63, via 0x899A40.
  - The first loop of 0x79F660 also queues **status icons** (kind 3) at Chr+0xF3C/F40 - 0x13.
  - Both loops skip off-screen entries (x in [0,0x200], y in [0,0x1A0]).
- **0x893290** (FhUtil 0x493290) draws the queue and then zeroes the count. Its only call site is **VA 0x89B3CB**
  (FhUtil 0x49B3CB) inside the battle UI draw 0x89B360 (main thread).
  - Its prologue is `57 E8 rel32`, with a relative call inside the first 5 bytes. So use a **call-site retarget at
    0x89B3CB** (`E8 rel32`, single site) rather than an entry detour.
- 3D anchors per Chr (round 1): damage = optpos[Chr+0x442] with y -= Chr+0x4E8; status icons = the cursor anchor
  optpos[Chr+0xDF9]. Use the smoothed form `actor+0x1D0 + (optpos - (actor+0x0C + actor+0x6C))` (SYNTHESIS 2.3).
- Chr i = `[VA 0x11334D4] + i*0xF90` for i < 0x1F (0x794020).

**Design** (C2/C3 pattern):
1. **Main-thread stub** at the 0x89B3CB call site, a naked/cdecl void() stub:
   - Snapshot the queue count and entries.
   - Walk the damage list with the same visibility test as 0x79F660, so each queue entry pairs with its node. Status
     icons pair with the first loop's Chr order.
   - For each entry record: chr, kind, glyph, rgba, and the **screen offset from that Chr's base anchor**
     (entry.xy - anchor.xy, PS2 units).
   - Then, in the headset with `dmg3d=1`, set the count to 0 before calling the original. 0x893290 then still runs its
     setup 0x903300, draws nothing and clears the queue. On the monitor, call it unchanged.
   - Publish the snapshot through the existing two-slot seqlock, frame-stamped. Take each referenced Chr's 3D anchor
     like this: the stub runs before the job join at 0x821656, so read optpos here, as the game's own 0x79F380 does,
     and take +0x1D0 from the smooth-anchor code's post-join snapshot.
2. **Native draw** (per eye, where C3 draws its battle.dds billboards):
   - For each entry, draw a battle.dds quad with the glyph's atlas rect. The digit and popup rects come from
     0x8FB840 / 0x8FBCC0; C3's atlas capture already has battle.dds.
   - Position: billboard at the 3D anchor, plus the screen offset converted to world units at a fixed angular size
     (`dmg_scale`, default ~1.6 deg per 10 PS2 px at the viewer's distance), so the numbers stay readable in the
     diorama and in first person.
   - Alpha and colour from the entry.
   - Depth: on top (like C3), with a slight push toward the viewer.
3. Pairing failure (count mismatch): fall back to the nearest base anchor in screen space, log `dmg3d: pairing
   fallback`, and never drop entries silently.

- **Effort** M (1-2 days, mostly the pairing and the glyph rects). **Risk** medium: the node layout for kinds other
  than 0/1, and multi-target overlap. The pairing removes the overlap ambiguity.
- **Tests:**
  1. A multi-target spell (Fira on 3 enemies): one number over each enemy, in the headset, in both diorama and first
     person.
  2. Heal (green) on the party.
  3. Miss / status icons (Poison, Sleep).
  4. The monitor is unchanged.
  5. `dmg3d=0` brings back the panel behaviour.
  6. Log line count: entries vs paired.
- Coordination: exe-battle-ui owns the battle UI. Hand the design over if they take it in round 2.

---

## 4. Battle diorama auto-framing, including bosses (rank 4; item 5 pick 2)

**Problem.** The battle diorama centres on the combatants' centroid at the user's last scale and distance (design-notes
19 Oct). Big bosses (Sin's fin, Geosgaeno, Evrae, Yunalesca's last form) overflow the view or stand inside it; small
fights waste the table.

**Design** (managed, module.cs battle-anchor code, which already collects the battle lists):
- Per frame (smoothed), compute the combatants' bounding box from:
  - actor roots (party slots and visible monsters, as the anchor already does);
  - each monster's height: Chr+0x4EC (stat 296 / 100, the gauge height) or Chr+0x4E8 (stat 261 / 100, the damage-number
    height), whichever is larger, in game units [code: round-1 offsets].
- From the box, set the diorama's **seat distance and world scale** so the box fills about 60 deg horizontally at the
  default pitch. Keep the floor clamp. The user's grab offset becomes a relative trim on top (stored as factors, not
  absolutes).
- Boss detection is not needed: the box does it. Optionally keep a per-battle memory keyed by the battle id (Btl struct)
  so a re-fought boss keeps the user's trim.
- Snap at battle start (already done for the anchor). Slow time constant (1 s) for mid-battle changes (monsters
  joining, Sin's parts dying).

- **Effort** S-M. **Risk** low (managed only; the existing anchor override and `g_gest_scale` are the outputs).
- **Tests:**
  1. A small fight (2 fiends): close table.
  2. Geosgaeno / Sinspawn Gui: the whole boss in view with no clipping.
  3. Evrae (airship, moving): the frame stays stable.
  4. A grab-trim persists through the battle.
  5. `battle_autoframe=0` gives the old behaviour.

---

## 5. Per-area view memory (rank 5; item 5 pick 3)

**Design** (managed):
- Key: map id `[[FhUtil 0x8CB9D8] + 0xFAF4]` (renderer +0xFAF4, as pan-scan uses) plus `current_room_id`
  (save_data FhUtil 0xD2CA90 + 0x0), and the view mode (fp_mode 1/2/3, battle or field).
- Value:
  - diorama: offset `g_gest_move`, scale, pitch, yaw relative to the area's first heading;
  - third person: distance and height.
- Save on change, debounced 2 s, into a separate `ffxvr_areas.ini` next to the build-8 settings file (same atomic
  write and `.bak` rules; one line per key).
- Restore on area change, after the existing view_defaults / reset, before the first frame of the new area. Home/reset
  clears the entry.
- Also the user's choice of view per area ("this area in first person"), from the backlog line.

- **Effort** S. **Risk** low. The only decision is the key granularity: map id alone may be too coarse for big maps;
  room id refines it.
- **Tests:** adjust the diorama in Besaid village, walk to the beach and back: it is restored. Restart the game:
  restored from the file. Reset clears it.

---

## 6. FMV on the virtual screen (rank 6; item 3)

**What the exe does** [code]:
- `sFMVPlayerManager` singleton **[VA 0xCDED3C]** (FhUtil 0x8DED3C); playing = byte +0x6D2 (or +0x6D0). The renderer
  gate also requires [VA 0x112A008] != 0.
- The decoder is **WebM / VP8** ("WebM Project VP8 Decoder v1.4.0"; PVideoPlaybackWin32 reading and decoding threads).
  It uploads **three planes**: `YChannel` (mgr+0x38C), `UChannel` (+0x400), `VChannel` (+0x474), with float params
  `imageWidth` (+0x6F4) and `imageHeight` (+0x6F8), bound in 0x6D86E0.
- `Video.cgfx` converts YUV to RGB in its pixel shader. The mesh is mgr+0x388, drawn by **0x6D7570** with its own 2D
  camera (+0x574) after a **black clear** (letterbox), from the FMV branch 0x653960 of the renderer frame 0x668930.
- After it, **0x6E7F00** (this = [r+0x10DE0]) draws the skip-button prompt (keyboard or pad glyphs by 0x631320), and
  0x692C40 blits. Iggy (0xA267F0) draws on top.

So "the video texture" is three planes, not an RGB texture. A native grab would need our own YUV->RGB shader (BT.601
vs 709 and the range unknown), and it would **lose the subtitles and the skip prompt** drawn after the video.

**Today:** an FMV frame has no 3D camera, so after the 2-frame hysteresis it goes to the virtual screen. The screen
shows the backbuffer copy (the 4K backbuffer stretched into the 2560x1440 screen swapchain), on a 16:9 quad 2.5 m wide
at 2.5 m (about 53 deg, about 1300 headset pixels across). The video at 1080p or below already exceeds what that quad
resolves, so a native grab gains little sharpness.

**Recommended design (S):**
1. **Instant switch:**
   - When `[0x8DED3C]+0x6D2 || +0x6D0` (read at xr_tick; a race is harmless), go to the screen with no hysteresis.
   - Skip eye-2 work for those frames (exe-engine: FMV draws straight into the backbuffer).
   - Do not blink.
2. **Crop to the video and correct the aspect:**
   - Compute the video rect inside the backbuffer as an aspect fit of imageWidth x imageHeight
     ([runtime?] confirm with one `seq` BMP dump that the game letterboxes rather than stretches).
   - Copy only that rect into the screen swapchain. `g_h.copy` gets a source rect; the copy shader already takes a
     stretch, so add UV scale and bias.
   - Size the quad as `width x width * vh/vw`.
   - This also keeps the movies right when the game renders a non-16:9 backbuffer (the "render taller" sharpness
     experiment). Today the quad is always 9/16 of the width.
3. **Cinema preset** (optional): `fmv_screen_width` / `fmv_screen_dist` (default 4 m at 3 m, about 67 deg), used only
   while an FMV plays. The 2560x1440 screen swapchain matches that size well.
4. Not recommended (M, low value): the native YUV path (detect the draw with three R8 SRVs of WxH and W/2xH/2 during an
   FMV, then our own BT.601 shader into an RGBA WxH target). Revisit only if the cinema preset at 90+ deg shows softness.

- **Effort** S (half a day). **Risk** low.
- **Tests:**
  1. The opening FMV ("Listen to my story") and Sin's attack on Zanarkand: correct aspect, no bars inside the quad,
     subtitles visible, skip prompt visible.
  2. The FMV -> in-engine transition: no flash of eye 2.
  3. With a 3840x3600 backbuffer (the experiment): the FMV is not stretched.

---

## 7. Dialogue boxes near the speaker (rank 7; item 4)

**Facts** [code]:
- The position opcode helper **0x8580C0** (FhUtil 0x4580C0; prologue `55 8B EC 83 EC 08`, 6 clean bytes) is called by
  four opcode handlers (0x85A8A0, 0x85B280, 0x8607A0, 0x860B00). It pops (anchor byte, y, x, window index) from the
  Atel stack with `0x86DF00(param_1, param_3)` = Fahrenheit `AtelPopStackInteger(AtelBasicWorker* work, AtelStack*)`.
  So **param_1 is the issuing worker**.
- Worker -> actor: **`AtelGetCharHandle` 0x86AE80 = `return worker->[+0x9C]`** (an Actor*, null when the worker has no
  character). It is a plain field read, so managed code can read worker+0x9C directly.
- The window record is TOMesWinWork[bank*8 + idx] at VA 0x1326D70 (stride 0x2C, bank u16 at 0x1326B82). It has
  +0x00 x, +0x02 y (512x416), +0x1C anchor byte, and +0x14 status (Fahrenheit). The text opcodes 0x857870 / 0x857A00 /
  0x857C60 have clean prologues and the same (worker, ., stack) shape.
- The authored position is itself a strong hint. Designers placed each box next to the speaker's head **for the
  authored camera**. And the game knows that camera.

**Is "the actor whose script opened the window" workable?**
- **For field conversations, yes.** Talking to an NPC runs that NPC's talk entry on its own worker, so worker+0x9C is
  the NPC [infer, high].
- **For cutscenes, unknown.** Event dialogue may be issued from a director/master worker (+0x9C null, or the "event
  controller" actor) [runtime?].
- So use the worker as the first guess and **validate it against the authored position**:
  1. In the 0x8580C0 detour (main thread), record {window idx, x, y, anchor, worker, actor = worker+0x9C, frame}.
  2. At draw time, project candidate heads (the worker's actor first, then every visible actor) through the
     **game's** camera (VIEW 0x2311440 and the game projection, as the C1 diagnostics already do).
  3. Accept the actor whose head projects within about 120 PS2 px of the box's anchor edge, with a bonus for the
     worker's actor. Otherwise: no speaker (keep the box on the head-locked panel).

**Design (v0, first person and field only):**
1. Detour 0x8580C0 (call the original first). Snapshot as above into a small ring of 8 window slots.
2. **The box rect** is needed to cut the box out of the UI pass [runtime?]. TOMesWinWork has no width or height. Find
   it from the UI quads: the window-frame sprites drawn near (x,y) in the UI pass. The UI pick tool (F9) on one dialogue
   box gives the frame texture and quad pattern; then a per-frame scan of UI quads within the box anchor's
   neighbourhood gives the bounding rect.
3. **Layers:**
   - Copy the box rect from the UI texture (CopySubresourceRegion) into a new swapchain (index 6, 1024x512).
   - Clear that rect in the head-locked UI copy (ClearView with a rect, D3D11.1).
   - Submit swapchain 6 as a world-locked quad in LOCAL space, at the speaker's head (smoothed +0x1D0 head height) plus
     the authored side offset, facing the viewer, about 0.9 m wide at 1.5-2 m.
4. Fallbacks: no speaker, an event (cutscene) in progress, or the box rect not found -> today's head-locked panel.

**Survey before building** (one evening, log only):
- Add the detour and log `meswin: idx x y anchor worker actor(chr id / name) event_id`.
- Play 10 field conversations and 2 cutscenes (Besaid village, the Kilika boat).
- If the worker's actor matches the visible speaker in more than 90% of field talk, build v0. Cutscenes decide whether
  v1 (the screen-proximity rule only) is worth it.

- **Effort** M-L (survey S, rect discovery M, layers M). **Risk** medium-high: rect discovery, boxes that span two
  speakers, and readability at distance.
- **Tests (v0):** talk to 5 NPCs in Besaid in first person: each box appears at the NPC's head, readable. Walk away
  mid-conversation: the box stays at the NPC. In a cutscene: the panel behaviour is unchanged.

---

## 8. Others (rank 8): notes for later

### 8.1 Camera cuts and spell cameras as signals
- Today's `detect_cut` (40 units / 25 deg from CB bytes) feeds recentre and blink in the **seat** and stereo-screen
  modes only. The diorama and first person ignore the game camera, and battles use the diorama centroid.
- An engine signal is cleaner. Post-hook the viewport finalize **0x7BC110** (FhUtil 0x3BC110) to read the canonical
  (30 Hz) camera per viewport plus the source tags (FFX-60fps reads them at the signature 0x7BC3D1).
  - A cut = camera index or source change, or the FFX-60fps jump rule.
  - An entry detour is compatible with FFX-60fps: it retargets the three CALL sites and signature-checks the body, not
    the prologue.
- **Spell id** `[VA 0xC64CA0]` (FhUtil 0x864CA0; ids 0x98 / 0x259 split camera, 0x21C near plane) and r+0x10D8C
  (alternate camera) say a spell owns the camera. In seat mode, switch to the "director" policy, or freeze the seat
  heading, for the spell's duration.
- Value low-medium now that the diorama is the default battle view. S effort.

### 8.2 Swimming eye height
- No clean "swimming" flag was found statically. Candidates to log at the Besaid lagoon and in Baaj:
  - save_data **+0x28 `is_cam_underwater`, +0x29 `is_map_underwater`** (FhUtil 0xD2CAB8 / 0xD2CAB9; Fahrenheit names;
    no absolute-address readers, so probably written by an Atel opcode);
  - actor **+0x194 bit 0x20000000** (picks SWIM1 / SWIM2 footstep sounds in 0x835860);
  - swim-kick events: 0x835860 called with (event & 0xFF) == 4 for the player actor.
- One Motion-log session with these columns decides it. Then the design in design-notes (eye height slew to a swimming
  value) is S.

### 8.3 Overdrive input aids
- The minigame code is still not located (round-1 dead end). The VR pad maps the thumbsticks 1:1, so Lulu's spin should
  already work with the right stick [runtime?].
- Only build aids (for example a "spin" gesture) if the owner reports trouble.

### 8.4 Full presenter thread (tier B)
- Still the right design for display-rate submission and pose-based **stick-turn smoothing**, the remaining
  non-head-motion judder.
- Shape: own D3D11 device on the same LUID, session bound to it, eye images crossing as keyed-mutex shared textures.
- The shared texture replaces `mid` / the swapchain image as the copy target. perf-architect's R25 shader copy then
  writes the shared texture, and the presenter does the single CopyResource, so the copy count stays the same.
- xrWaitFrame / BeginFrame / EndFrame / SyncActions all move to the presenter. The game thread asks it for the latest
  predicted views. Pacing: the game is then paced only by FFX-60fps's limiter (RATE-001), not by xrWaitFrame.
- L effort. Do it only after keepalive and fps = refresh are in, if stick-turn judder remains the top complaint.

---

## 9. Open questions / runtime checks (cheapest first)
1. The build-7 `xinput set:` values: expect 65536 - b (for example 65282). Confirms the decode (section 2).
2. Keepalive's simulated-stall test, then a real battle transition (section 1.6). Add the EBP-chain walk to the stall
   watchdog in the same build.
3. One `seq` BMP dump during an FMV: letterbox or stretch (section 6).
4. The meswin survey (section 7).
5. Swimming columns in the Motion log (section 8.2).
6. Does VDXR list `XR_KHR_composition_layer_color_scale_bias`? (Log the instance extensions once at start.)
