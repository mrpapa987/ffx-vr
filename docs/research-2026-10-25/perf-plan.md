# Performance plan: fps = headset refresh (90/90, then 120/120), and blitzball off 30 fps

perf-architect, 25 Oct 2026. Static analysis only. Code references are to branch `night` of the work repo
(c470af6 = `cursors` 7f38cbf + builds 8/9 + scripts pass). "native" = `src/native/ffxvr_native.cpp`,
"xr" = `src/native/ffxvr_xr.cpp`, "managed" = `src/managed/module.cs`, "fps60" = the FFX-60fps source in
`scratchpad/ffx60/repo` (I can't verify that the owner's DLL matches this source; its log lines below confirm it).
Confidence tags: **[code]** read in source, **[ext]** FFX-60fps source, **[team]** a teammate's finding, **[est]** my
estimate (no measurement yet), **[runtime?]** needs an in-game check.

---

## 0. Summary

Two facts change the picture:

1. **fps = refresh is mostly a settings problem, not a code problem.** The game only draws 60 fps because
   FFX-60fps is set to `fps=60`. The current fps60 accepts `fps=90` and `fps=120` (any multiple of 30 from 60 to 240),
   but above 60 it needs the game's **V-Sync off** [ext: `ship/fps60.ini`, `rate.cpp`, `RATE-001.cpp`]. Blitzball "settles
   at 30" because fps60's PERF-002 steps down to stock 30 when the mean frame time is over 55/60 of the rate (18.2 ms at
   60). At a GPU time of 19-20 ms that step-down is what happens [ext: `PERF-002.cpp`]. So blitz needs about 15 ms or
   less, with margin, to stay at 60.
2. **The game renders 2.4x the pixels the headset receives.** The eye swapchains are the runtime's recommended width at
   16:9, about 2496x1404 [code: xr 447-451]. The game renders at 3840x2160, and the copy pass then downsamples
   [code: xr 557]. Everything the mod does is resolution-independent. So the game's own resolution (2560x1440, the
   `-Profile vr` value) is the biggest single lever: roughly 0.44x the pixels for the scene and post-processing, in both eyes.

| # | Item | GPU saved, field | GPU saved, blitz | Confidence | Effort | Risk |
|---|---|---|---|---|---|---|
| S1 | Game resolution 3840x2160 -> 2560x1440 | 3.5-6 ms | 4-8 ms | medium [est] | settings | softer image (supersampling lost) |
| S2 | fps60 `fps=90` (or 120) + game V-Sync off + VD refresh to match | enables fps = refresh | same | high [ext] | settings | PERF-002 fallback cadence (sec. 4) |
| S3 | Mod MSAA 4x -> 2x (`msaa=2`) | 0.4-0.9 ms at 1440p (0.7-1.5 at 4K) | 1-2.5 ms | medium [est] | settings + reload | edge aliasing |
| S4 | HDAO off (GameSetting.ini `HDAO=0`, if honoured) | 0-2 ms | 0-2 ms | low [runtime?] | settings | less contact shadowing |
| D1 | Diagnostic build: GPU phase and per-target timestamps, eye-2 A/B, R23 would-skip tally, render-thread CPU | 0 | 0 | - | S-M | none |
| M2 | MSAA rule for the 4096x4096 targets (`msaa_big`) + per-mode MSAA (blitz = 2x) | 0 | 1-2.5 ms | low-medium | S | reflections alias |
| E0 | No eye-2 re-issue on frames shown on a mono screen | 0 in 3D | 40-50% of those frames | high [code] | S | none |
| R23 | Step 2: eye 2 shares view-independent targets (shadow maps, the blitz reflection) | 0.3-1.0 ms (+0.5-2 ms CPU) | 1.5-4 ms | low until D1 | M | stale one-eye shadows or reflections |
| R25 | Copy straight into the swapchain image; lazy swapchains 3-5 | 0.1-0.25 ms, about 120-180 MB VRAM | same | high | M | colour (double sRGB) |
| R17 | One native call per draw | 0 GPU, 0.1-0.3 ms CPU | same | medium | S | none |
| M4 | Auto-MSAA, applied at the game's own target recreation | keeps the budget | same | medium | M | colour/depth sample mismatch if latched wrong |

**Recommended order:** recipe now (S1+S2, then S3/S4) -> D1 -> M2 + E0 -> R23 step 2 (staged 2a-2c) -> R25 + R17 ->
M4. Section 7 has the settings recipe for 90/90.

---

## 1. Facts the model rests on

### 1.1 Pacing chain (why 60, why 30)
- The game logic runs at 29.97 Hz. fps60 draws N passes per logic tick, N = fps/30, and retargets the game's own
  limiter (SKS_WinMainLoop constant at 0x00B412D8) to N x 29.97 Hz: 59.94 / 89.91 / 119.88 Hz [ext: RATE-001].
  "Above 60 fps the game must also present without V-Sync ... or the swap chain holds the loop at the monitor's
  refresh rate" [ext]. In the headset the game still presents its monitor swapchain, so V-Sync on caps the loop
  at the monitor refresh (60 Hz on many monitors and on VD's virtual display).
- In the headset, our OpenXR loop runs inside Present on the render thread: `xr_tick` -> `xr_frame` ->
  xrEndFrame / xrWaitFrame / xrBeginFrame [code: native `ffxvr_begin_frame` step 9]. So xrWaitFrame paces the render
  thread to the display, and the game limiter paces the main thread. Rendered frame period = max(limiter period,
  CPU, GPU), rounded **up to whole display intervals**.
- PERF-002 (default on): it averages the frame time at Sg_MainLoop entry over windows of 120 frames. Two windows with
  mean > period / (55/60) step down one notch (240 ... 90 -> 60 -> stock 30). It retries after 20 s, then 40, 80 ...
  up to 640 s [ext]. Limits: 18.2 ms at 60, 12.1 ms at 90, 9.1 ms at 120. Because VR frames are quantised to display
  intervals, a fraction p of missed slots adds p x period to the mean. At 90 Hz, about 9% missed frames trips the
  step-down. **Margin matters more than the mean.**
- Today: 60 fps on a 72 Hz headset gives a 5:6 cadence, which is judder of non-head motion (design-notes "Two rates
  matter"). Blitz: 19-20 ms GPU -> PERF-002 -> stock 30 (33.4 ms frames, owner log).

### 1.2 What the GPU span measures today [code: native 4090-4125]
- `gpu` in the `perf:` line = timestamp t0 at the game's first clear (`gpu_timing_start_span`, from
  `ffxvr_on_clear` while `g_overlay_phase`), to t1 at our hook in Present (`gpu_timing_end_span`, **before** `xr_tick`).
- **Not included**:
  - our XR submit copies (`copy_release`: shader downsample to `mid` + CopyResource into the swapchain image, per layer);
  - Virtual Desktop's compositing, which runs **on the game's device context inside xrEndFrame** (native comment at
    xr_tick: "Virtual Desktop binds its 2496x2688 eye textures and draws");
  - the ImGui overlay (drawn between our hook and the next first clear).
- So the real GPU frame = span + about 1-2 ms [est]. Budget per refresh: 90 Hz = 11.1 ms -> **span target about
  8.5 ms**. 120 Hz = 8.33 ms -> **span target about 6 ms**. 60 fps (blitz) = 16.7 ms -> span about 14 ms.

### 1.3 How eye 2 is made [code: managed 640-658, native `ffxvr_on_draw`, `ffxvr_eye2_begin/end`]
- Every game draw on our context, outside the overlay phase, is drawn for eye 1, then re-issued once (three times for
  split hand parts): its RTVs/DSV are swapped for twins, the PS SRVs whose texture has a twin are swapped, and the eye-2
  image of every VS CB slot that has one is uploaded (`stereo_scope 0`).
- Post-processing, resolves (`ffxvr_on_resolve`) and copies (`ffxvr_on_copy`) are mirrored onto twins. Clears are
  mirrored (`ffxvr_on_clear_twin`). So eye 2 costs about one full extra frame of scene + post GPU work, plus mirrored
  clears and resolves.
- The UI pass: the eye-1 draw still lands in the scene target; the "eye-2" pass of a UI draw goes to the UI panel
  (`g_ui_rtv`). `ui_pass_begin` copies the resolved scene and its twin into `g_eye1_final`/`g_eye2_final` (two
  full-resolution copies).
- R23 step 1 is in: `g_e2_same_inputs` counts eye-2 re-issues with no swapped SRV and no eye-2 CB data that differs from
  eye 1's. The owner saw up to about 20-28% of eye-2 draws. The **per-target tally from G2-final was not implemented**
  (grep: only the global counter exists), so we don't know which targets those draws went to.

### 1.4 Passes [team: exe-engine, low-medium confidence]
- Shadows: Phyre `PShadowCaster` types NoShadow / PCFShadowMap / CascadedShadowMap / CombinedCascadedShadowMap
  (4 splits). Cascades are fitted to the **game's** view frustum on the CPU. The mod overrides the camera only in GPU
  constant buffers, never in the CPU camera the splits are fitted to. So the shadow pass is the same for both eyes:
  its CBs carry light matrices, which the camera recognition does not touch. Many scenes also use the simple `ChrY0Shadow`
  y=0 projected character shadow, drawn in the main pass (per eye, nothing to share).
- Blitz reflection: a Phyre water reflection is a mirrored main camera. Its CB fails the inverse check, so it gets no
  eye image (G2-proposal R23). So **today both eyes render the reflection from the game camera**, and the eye-2
  re-render is byte-identical work. Refraction = the main view: if its CB carries the recognised camera, it is per eye
  and not shareable. D1 tells which.
- Post: HDAO (BufferGen/Blur/BufferRender/Combine/Blur_USM, per-map `hdao_settings.bin`, an "HDAO Level" setting at
  0x6938D0), UnSharpMask, ColorCorrection, FXAA (PFXAA), the water-area history blend. All screen-space, so per eye.
  Unknown whether any run at reduced resolution.
- GameSetting.ini keys that exist in FFX.exe (.rdata 0xB7C7E0..): Resolution, ScreenMode, Quality, VSync, Anisotropic,
  **Shadow**, TextureQuality, **MSAA**, ColorCorrection, **PostAA**, **UnSharpMask**, **HDAO**. The mod found MSAA= ignored
  on the Fahrenheit launch path (native 100-104), so "honoured" must be checked key by key [runtime?]. The string
  `videoSettingMSAAHDAOPopUpMessageInit` (0x6FCCA0) hints that the game has an in-game MSAA/HDAO setting with a popup
  (open question Q3).

---

## 2. Frame-time budget model (field, today: 3840x2160, 4x MSAA, both eyes, span 11-13 ms)

All numbers are [est], made to add up to the measured span on an RTX 3080 Ti. D1 replaces them with measurements.

| Phase | Eye 1 | Eye 2 (re-issue) | Scales with resolution? | Notes |
|---|---|---|---|---|
| Shadow / other view-independent targets | 0.3-0.8 | 0.3-0.8 | no (fixed map sizes) | R23 target: the eye-2 copy is wasted |
| Scene opaque + transparent (4x MSAA) | 3.0-4.0 | 3.0-4.0 | yes, about linear | MSAA 4->2: -15-25% of this |
| Scene resolve(s) 4x -> 1x | 0.1-0.2 | 0.1-0.2 | yes | mirrored resolve |
| Post (HDAO?, bloom, colour, USM/FXAA, composite) | 0.8-1.8 | 0.8-1.8 | yes | per eye by nature |
| UI pass (scene target + panel) + eye-final copies | 0.3-0.6 | - | partly | 2 x 33 MB copies at 4K |
| **Span** | | | | **11-13 measured** |
| Outside the span: XR copies (2 eyes + UI panel + ImGui) | 0.3-0.6 | | no | R25 trims about a third |
| Outside the span: VD compositor in our context | 0.5-1.0 | | no | runtime |

Blitz adds about 7 ms: the 4096x4096 reflection/refraction targets at 4x MSAA (67 M samples each, about 2x a 4K 4x
scene target) plus their resolves, drawn for both eyes. If the reflection is half of that delta, R23 recovers about
1.5-2 ms and M2 about 1-2 ms.

**Projection with S1 (2560x1440) + S3 (2x):** resolution-scaled phases x 0.45-0.6, shadows unchanged ->
span ≈ 2 x (0.55) + 2 x (3.5 x 0.5 x 0.8) + 2 x (1.3 x 0.5) + 0.3 ≈ **5.5-7.5 ms** field. With R23, -0.3-0.8.
- 90 Hz fits with margin.
- 120 Hz is borderline: span 6 ms + 1.5 outside ≈ 7.5-8.3 against 8.33.
- Blitz: about 9-12 ms after S1+S3, so 60 is safe. 90 needs M2 + R23 as well.

### What D1 must measure to confirm (sec. 3)
1. The span split into phases (pre-scene / scene / post / UI) plus the two outside-span phases.
2. Eye-2 total: the span with eye 2 on vs off (A/B toggle).
3. Per-target GPU time, with draw counts, same-input counts and size/samples/clear behaviour. This is the R23 target list.
4. Render-thread CPU per frame (busy = frame − xr wait − xr end), and the time in our hooks. Main-thread frame time
   from fps60's `DIAG-FRAMETIME=1`. At 90/120 the CPU budget is 11.1 / 8.3 ms per thread.
5. For vr-opportunities: render-thread CPU with `ID3D11Multithread::SetMultithreadProtected` on vs off (their keepalive
   backstop `keepalive_mt_protect`).

---

## 3. D1: diagnostic build (no behaviour change unless a Debug toggle is used)

**D1a. GPU phase timestamps** (always on, cheap):
- Extend the existing ring (`k_gpuq_ring = 6`; one DISJOINT + t0/t1 per slot) with up to 12 timestamp queries per slot,
  stamped at these markers (each at most once per frame, a missing marker reads "-"):
  1. `T_FIRST_CLEAR` (existing t0);
  2. `T_SCENE_BIND`: first game bind of last frame's `g_scene_live_tex` as RTV0 (in `ffxvr_on_rendertargets`,
     `g_cur_rtv0_tex` compare);
  3. `T_SCENE_RESOLVED`: first `ffxvr_on_resolve` with `src == g_scene_live_tex`;
  4. `T_UI_BEGIN`: `ui_pass_begin`;
  5. `T_BB_FIRST`: first draw into `g_backbuffer`;
  6. `T_PRESENT` (existing t1);
  7. `T_XR_COPIED`: after `submit_set` in `xr_frame`;
  8. `T_XR_END`: after xrEndFrame returns. Stamp it in `xr_frame` via a host callback, or expose `g_h.ctx->End(q)`
     through `XrHost`.
- Read the slot that is about to be reused, with DONOTFLUSH (as today). Average over the 60-frame perf window.
- New perf line field: `gpu phases: pre X | scene X | post X | ui X || xr copy X | runtime X`.
- Span semantics stay as they are, so old and new logs compare.

**D1b. Per-target profile** (armed: Debug keepalive or a "GPU profile 5 s" button):
- One timestamp at every **game** render-target bind (`ffxvr_on_rendertargets`/`_uav`, not while `g_in_eye2`). The
  interval up to the next bind is attributed to that bind's RTV0 texture (or DSV texture if there is no RTV). Eye-2
  draws are re-issued inside the same run, so each interval holds both eyes' cost.
- Ring: 4 frames x 256 queries, overflow counted. Over 300 frames, aggregate per texture:
  `{w, h, fmt, samples, ms, draws, eye2 draws, same-input draws, clears (RTV/DSV full/partial), resolve src/dst, copy
  src/dst, bound as SRV count}`.
- Print the top 24 to `ffxvr_diag.log`. This is G2's "16-entry per-target tally", done properly.

**D1c. Eye-2 A/B (Debug, measurement only)**: `eye2_reissue = 0`. `ffxvr_on_draw` returns 0 passes, and xr_tick submits
eye 1 to both views (as with `g_blink` today). The headset then shows mono for the test. Δspan = the total eye-2 cost.
Restore on overlay close.

**D1d. R23 would-skip simulation (always counted, no skipping)**: the full step-2 decision of sec. 5.1, evaluated and
counted but never acted on. Counts: `would skip N draws`, `would materialise M (MB)`, `eligible targets K`. The perf
line gets `e2 share sim: skip N / mat M`. Zero risk, and it de-risks step 2.

**D1e. CPU**:
- `rt busy` = QPC frame − xr wait − xr end.
- `hooks` = QPC sum over `ffxvr_on_draw` + `eye2_begin`/`end` (already partly in `eye2 re-issue`).
- A managed sample: Stopwatch around 1 in 32 `h_drawindexed` calls, scaled up.
- Optional ini `mt_protect_test=1`: calls `SetMultithreadProtected(TRUE)` at init, for vr-opportunities' A/B.

**D1f.** Log `fps60`-relevant facts once: the backbuffer size, `g_msaa_last_*`, and the count of large-MSAA creations per
map change (the `msaa:` lines already exist; add a "burst" summary: N targets created within 2 s, at which map). This
answers Q2: does the game recreate its targets per area or per battle?

Effort: S-M. Risk: none (timestamps are async; D1b is armed only).

---

## 4. Settings stage (S1-S4): what each does

- **S1 resolution.** 2560x1440 ≥ the eye swapchain (about 2496x1404), so the low-resolution warning stays quiet (it
  compares the backbuffer with the eye size, native `lowres_check`).
  - Above the swapchain size, resolution only buys supersampling (anti-aliasing), not detail.
  - If the owner misses the 4K sharpness, use 2880x1620 (0.56x the pixels) or 3200x1800 (0.69x).
  - If VD's quality preset is raised (recommended width > 2560), raise the game resolution with it.
- **S2 pacing.**
  - In `fps60.ini`, set `fps=90` (or `fps=120`). In the game's Config page (or GameSetting.ini) set `VSync=0`.
  - In VD, set the refresh to match: 90 or 120 Hz.
  - fps60's log must say `rate: 90 fps requested (3 passes ...)` and `RATE-001: host limiter at 89.91 Hz`.
  - The limiter (89.91) is 0.1% slower than the display (90.00), so about one display slot every 11 s (8 s at
    119.88/120) gets the previous frame reprojected. Invisible with SSW on; a tiny hitch with SSW off. Accept it.
- **PERF-002 fallback cadence.**
  - At 90 Hz the step-downs are 60 (uneven 3:2, judder) and then stock 30 (even 3:1).
  - At 120 Hz the step-downs are 90 (uneven) and then 60 (even 2:1). fps60 steps one notch at a time, so a PC that
    can't hold 120 may sit at 90 with judder.
  - The fix is a later small fps60 option (allowed rates list, e.g. `fps_steps=120,60`). It would be a change to
    fps60, not to us.
- **Frame-counted timers in our mod** speed up 1.5-2x at 90/120 fps: hysteresis counts (`k_c2_nopass`, the 2D-frame
  hysteresis, `g_blink_frames`, the R15 hold of 30 frames, twin sweep 36000 frames). None is critical. Note it for a
  later pass that converts them to milliseconds.
- **Our turn, smoothing and anchor code** already use the measured `g_dt` and the +0x1D0 drawn root, which follow the
  rate. fps60 SMOOTH interpolates at t = 1/3, 2/3, 1 at N = 3 [ext: `pacing.cpp`].
- **S3 MSAA 2x**: `msaa=2` in the mod's ini (or the overlay radio). It takes effect when the game next creates its
  large MSAA targets: the next start, or the next recreation (Q2). 1x is **not** possible through the override,
  because the game's ResolveSubresource needs a multisampled source (native 103-104), and Texture2DMS SRVs would fail.
- **S4 HDAO**: `HDAO=0` in GameSetting.ini, if the key is honoured (compare `gpu` before and after in one spot). Also
  try `PostAA`/`UnSharpMask` = 0 for small savings. Shadow quality (`Shadow=`) changes map size: smaller maps also
  make R23's savings smaller.

---

## 5. Staged code designs

### 5.1 R23 step 2: eye 2 shares targets whose content is the same in both eyes

**Key insight (this lowers the risk the proposal assumed).** Give each twin a state `SH_YES` = "eye 2's content of this
texture *is* the game texture; the twin is stale". While a target is SH_YES:
- every write to the game texture is automatically right for eye 2, including paths we don't hook (UAV/compute,
  GenerateMips, UpdateSubresource/Map on textures, instanced draws, which we never re-issue);
- today those same writes are silently wrong for eye 2 (the twin misses them).

So sharing is strictly no worse for unhooked writes. The risk is confined to (a) a reader that uses `twin.tex` without
going through `eye2_tex()`, and (b) the moment a shared target stops being shared (materialisation).

**State, per Twin** (native 389-397): `uint8_t sh` (SH_NO / SH_YES), `bool never_share`, `uint16_t elig_streak`,
`uint32_t inelig_until`, plus per-frame `draws`/`same` counters (shared with D1b).
- `never_share` is set for:
  - the backbuffer twin;
  - textures with `mips > 1` or `array > 1`;
  - our own textures (eye finals, UI, ImGui; they are not in `g_twins`);
  - `g_scene_live_tex` and `g_scene_resolved` (never share the main scene: one non-identical draw would cost a full
    4K MSAA copy).
- New, recycled, failed or desc-mismatched twins (the B9R-2 test) -> `sh = SH_NO`, streak 0.

**Eligibility (learned, per target).** At begin_frame, for each twin touched last frame:
- `if (draws > 0 && same == draws && !never_share) elig_streak++ else { elig_streak = 0; }`;
- a target is **eligible** when `elig_streak >= 30 && g_frames >= inelig_until`.

So only targets whose every draw was same-input for 30 frames in a row (shadow maps, the blitz reflection) can share.
The main scene and post targets never qualify, because their draws carry eye images or sample SH_NO twins.

**Decision points** (all gated by ini `eye2_share`: 0 = off (default), 1 = on; D1d's simulation runs the same code with
the actions disabled):

1. `ffxvr_on_clear_twin` (native 5236):
   - Target SH_YES: any clear (full or partial) keeps SH_YES. **Skip the mirrored clear** (saves a clear).
   - Target SH_NO, eligible, and the clear is full -> SH_YES, skip the mirrored clear. "Full" means an RTV clear, or a
     DSV clear whose flags include STENCIL or whose format has no stencil (D32/D16/D32_FLOAT).
   - Otherwise mirror as today.
2. `ffxvr_on_draw`, **before the eye-1 draw** (managed calls on_draw first, then the original draw: managed 640-658).
   Move the 14-slot eye-image scan from `eye2_begin` to here and keep its result for `eye2_begin`.
   - `inputs_same` = not a UI/ImGui draw, not split, no VS CB slot with an eye image whose bytes differ from eye 1's,
     and no bound PS SRV (slots `< g_ps_srv_hi`) whose texture's twin is SH_NO.
   - Outputs = RTV0..n textures + the DSV texture.
   - If `inputs_same` and **every** output's twin is SH_YES: return `passes = 0`. Eye 2 is skipped and no managed
     eye2_begin/end runs, which also saves CPU. `g_e2_share_skips++`.
   - Otherwise, for each output twin with SH_YES: **materialise**. `{InEye2 g; CopyResource(twin, game);}` (legal for
     MSAA textures with identical desc). The game texture equals eye 2's content *right now*, because the eye-1 draw has
     not been issued. Then set SH_NO, `inelig_until = g_frames + 600`, streak 0, and count it with its bytes. Then the
     normal eye-2 path.
3. `ffxvr_on_resolve` (called after the game's resolve):
   - src SH_YES (or src untwinned) and dst eligible -> dst SH_YES. **Skip the mirrored resolve.**
   - src SH_YES and dst not eligible -> `ResolveSubresource(dt->tex, game src)`.
   - src SH_NO -> as today (the mirrored resolve rewrites the whole dst twin, so set dst SH_NO with no materialisation).
4. `ffxvr_on_copy` (after the game's copy):
   - Whole copy: as resolve: src same-in-both-eyes -> dst SH_YES if eligible (skip the mirror), else mirror from the
     game src.
   - Region copy into an SH_YES dst from an SH_NO src: materialise *after* the game's copy, then mirror the region from
     the src twin. Outside the region game == eye 2; inside, the mirror writes eye 2's source. So it is exact.
   - Region copy into an SH_YES dst from a same-in-both-eyes src: nothing to do.
5. **Every reader of eye-2 content** goes through `ID3D11Texture2D* eye2_tex(game)` (SH_YES ? game : twin.tex):
   - `twin_srv` (native 4874): return nullptr when SH_YES, so the game SRV stays bound and `any` stays false;
   - `ui_pass_begin` (4962), `snapshot_scene` (4928), `ffxvr_on_resolve`/`ffxvr_on_copy` sources, `ffxvr_measure`
     readbacks (5436-5453), the eye preview source (5968), the plain-SRV path (5988), xr_tick's `e2` (6399; the
     backbuffer is never_share anyway);
   - `grep -n "second\.tex\|->tex"` lists them all today: 12 sites.

**Stages:**
- **2a = D1d** (simulation only): run a field, a battle and blitz with `eye2_share` simulated. Expect "skip N" ≈ shadow
  pass draws, and in blitz the reflection pass. Expect "mat M" ≈ 0 per frame after warm-up. If M > 0 every frame,
  something is misclassified: stop there.
- **2b**: states + skip + materialise + the readers (decision points 1, 2 and 5). Toggle in the Debug overlay and the
  ini. Visual tests:
  - shadows in Besaid/Luca (cascades), character y=0 shadows, the blitz pool surface reflection, Macalania ice;
  - toggle it on and off while looking: there must be **no change at all in either eye**;
  - `gpu` and `eye2` drop.
- **2c**: resolve/copy propagation (decision points 3 and 4). Saves the mirrored resolves of shared targets.

**Expected gain** [est]:
- Field: eye 2's shadow pass, 0.3-0.8 ms GPU, plus skipped clears.
- Blitz: the reflection pass's eye-2 render + resolve, 1.5-4 ms, if the reflection CB indeed has no eye image (D1b and
  D1d tell).
- CPU: every skipped draw saves the managed eye2_begin/end + a draw call. At 300-800 skipped draws per frame, about
  0.5-2 ms on the render thread.

**Risks (visual corruption cases):**
- (1) A reader missed by `eye2_tex` -> eye 2 samples a stale twin: a shadow or reflection from minutes ago, or black,
  in one eye. Mitigated by the 12-site list, plus a debug assert: `twin_srv` is called for an SH_YES twin and returns a
  view other than null -> log.
- (2) A draw misclassified as same-input. For example a VS that reads the camera from a **VS SRV or a structured
  buffer** (we only scan CBs), or a GS/HS/DS stage. The same draw is identical in today's eye-2 path too, so the
  result does not change.
- (3) Stencil: handled by the full-clear rule.
- (4) Materialisation storms: blocked by `inelig_until`.
- (5) Future work that overrides the mirrored reflection camera per eye (correct stereo reflections) makes those draws
  "differ", and sharing turns itself off. Correct by construction.

### 5.2 MSAA: per-size, per-mode, then automatic

The override only acts when the game **creates** a large MSAA texture (`hk_CreateTexture2D`, native 3709).
Switching mid-frame is impossible without recreating the game's targets. The only safe switch points are the game's
own (re)creations: start-up, and possibly area or battle loads (Q2; D1f measures it).

- **M1 (now, settings)**: the existing global `msaa` (0 = game 8x / 4 / 2).
- **M2 (S)**: two additions to the override.
  - (a) `msaa_big`: a separate count for targets ≥ 4096 in both dimensions (the blitz reflection/refraction). Their
    colour and depth are the same size, so the rule can't create a colour/depth mismatch. Default 2.
  - (b) A per-mode value: in blitz (`_in_blitz`, managed 1946), managed sends `ffxvr_msaa_set(msaa_blitz)`. It applies
    only to targets created while in blitz (the 4096 pair, created on entry [runtime?]). The 4K scene targets keep their
    count unless the game recreates them.
- **Burst latch (M2 and M4, required).**
  - The first large-MSAA creation after ≥ 2 s without one latches `g_msaa_latched` (the count to use).
  - Every creation in that burst uses the latched value. The burst ends after 2 s with none.
  - This prevents a colour target at 4x and its depth at 2x if the setting changes mid-load (the engine checks:
    "MSAA types on selected color/depthStencil targets do not match", 0x5B6070).
  - Twins copy the game desc, so they follow. Old twins are swept as today.
- **M4 auto (M)**, after D1 has per-class numbers:
  - Class = {field, battle, blitz, cutscene/screen}. Budget `B = 1000/refresh − outside`, where outside is measured as
    `T_PRESENT -> T_XR_END` + overlay.
  - Per class keep the p90 span over 10 s at the current count c (skip 3 s after any load or burst).
  - **Step down** (4->2) for the next burst if p90 > 0.90 B for ≥ 3 s. Also when PERF-002 stepped fps60 down (visible to
    us as frame ≈ 2 display periods in more than 10% of frames).
  - **Step up** (2->4) if `p90 x k(c->up) < 0.75 B` for ≥ 20 s. `k` is learned per class from spans seen at both
    counts; initial value 1.25.
  - The 0.90/0.75 gap plus the burst latch is the hysteresis.
  - Persist the decision per class in the ini, so a session starts at the last good count. The overlay shows "next load:
    2x (field over budget)".
- **Not recommended**: forcing the game to recreate targets ourselves (ResizeBuffers via the backbuffer path 0x599210
  recreates only the backbuffer [team]). If the game's own in-game MSAA option (Q3) recreates targets safely at runtime,
  M4 could call that path instead. That needs an exe-engine trace first.

Expected: M2 1-2.5 ms in blitz (halved if R23 already skips eye 2 of the reflection). M4 keeps the budget, nothing
more. Risks: aliasing on reflections (mild on water); a wrong latch gives a D3D error in the game's check, mitigated by
the burst latch.

### 5.3 R25: one copy per layer, lazy swapchains (G4-final R25, now with engine facts)

Engine fact [team]: the game's backbuffer is `R8G8B8A8_UNORM_SRGB` (0x1C).
- Inside `copy_release` (xr 557, which R8 already owns):
  1. **Check the copy shader's colour path first.** Which SRV format does `ffxvr_copy_pass` sample `g_bb_copy` /
     `g_eye*_final` through (sRGB-decoding or raw UNORM)? Today's output is visually right through `mid` (UNORM) and a
     raw CopyResource into the sRGB image. So the bytes written to `mid` are gamma-encoded.
  2. If `images[k]` are TYPELESS, use a UNORM RTV per image, shader-copy straight in, and drop `mid` (same bytes as
     today). Otherwise use an `R8G8B8A8_UNORM_SRGB` RTV on the image, with the shader decoding to linear before the
     write (a `keep_srgb` flag in the copy pass; within 1 LSB). Log which path was taken.
  3. Create swapchains 3 (UI), 4 (ImGui) and 5 (stereo screen right) on first use (`make_eye(i)`). **Keep the
     `ever_released` check** for held layers: vr-opportunities' render-thread keepalive re-submits held layers and may
     only point at swapchains that have released at least once.
- **R25b (later, optional)**: downsample the eye images straight into the swapchain images at `ui_pass_begin` time, saving
  the two 4K eye-final copies. Swapchain acquire/wait/release is legal any time inside the open frame. The 3D markers
  (`draw_markers_both`) would then have to draw at swapchain resolution. Gain 0.1-0.3 ms. Not worth it before the rest.
- Gain: 0.1-0.25 ms GPU (2-3 fewer CopyResource of 14 MB layers), 120-180 MB VRAM. Risk: colour (double sRGB = washed
  out, or too dark). Test: gradients and dark scenes, headset vs monitor.
- vr-opportunities (agreed by message): R25 stays with me, done inside `copy_release` now. If the full presenter thread
  is ever built, its shared texture replaces the swapchain image as the shader-copy target.

### 5.4 R17: one native call per draw (G1-final)

As agreed: `ffxvr_on_draw_ex(which, n, start, base)` replaces `draw_ctx` + `draw_args` + `on_draw` for our context.
Optional `delegate* unmanaged` chain once a log line confirms it equals the trampoline address.
- With R23, a skipped draw returns 0 passes, so no eye2_begin/end call either.
- Gain 0.1-0.3 ms CPU per frame [G1 est]. Matters only if D1e shows the render thread near 8-11 ms.
- Risk none. Test: battle cursors still hidden, F9 pick.

### 5.5 E0: no eye 2 on frames shown on a mono screen

`xr_tick` forces `g_stereo` on in the headset (native 6375). So on frames shown on the **flat virtual screen**
(`g_xr_screen_now && !g_screen3d_now`: menus, 2D frames, the mono-screen modes), the whole eye-2 re-issue is still drawn
and then thrown away (only eye 1 is submitted, xr 612).
- Change: in `ffxvr_on_draw`, return 0 passes when the frame is decided to be a mono screen. That flag is decided at
  the previous xr_tick (`screen_next`), the same one-frame lag as `g_ovr_suspend`. On the switch back to 3D, the first
  frame must re-issue again (it does, since the flag clears at once).
- Gain: about 40-50% of the span on those frames. Matters for heavy 2D-screen frames and for a mono blitz screen if the
  owner ever picks it.
- Risk: none for 3D. The eye-2 preview in the overlay shows stale content on such frames (cosmetic).

### 5.6 Bigger ideas, assessed

| Idea | Verdict | Why |
|---|---|---|
| fps60 `fps=90/120` + V-Sync off | **Do now** (S2) | The only way to fps = refresh; already supported |
| Game resolution matched to the eye swapchain | **Do now** (S1) | 2.4x pixels for supersampling only |
| Skip eye 2 for distant sky/backdrop draws (parallax < 1 px) | **Reject** | The Quest 3 per-eye frusta are canted and asymmetric (tan l −1.376 r 0.839 vs l −0.839 r 1.376; design-notes blitz session). Even at infinity the two eye images differ in pixel position, so eye 1's pixels can't be reused without a reprojection pass |
| Eye 2 at reduced resolution for post only | **Reject** | Twins mirror the game desc. Post shaders carry texel-size constants (e.g. `g_ssao_mainTex_TexelSize`) and viewports. Every post draw would need rewritten constants and viewports. High risk for about 0.5 ms |
| Share low-resolution post buffers (bloom 1/8, 1/16) between eyes | **Low value** | Possible with R23's force-share, but small targets are cheap and halos would sit at screen depth in one eye |
| Fixed foveated / VRS | **Not available** | D3D11 without NVAPI VRS hooks; out of scope |
| Twin pool by size/format | Other team (load hitches) | Removes the 345 ms GPU spikes at target recreation (design-notes 22 Oct), not steady-state fps |
| Render-thread keepalive (vr-opportunities) | Theirs | Pacing unchanged in normal play (their message); D1e measures their MT-protect cost |
| Per-eye mirrored reflection camera (correct stereo reflections) | Later, correctness | Would make the reflection per eye, and R23 then stops sharing it automatically; costs the GPU R23 saves |

---

## 6. Gains, risks, order

Cumulative projection [est, field / blitz span]: today 11-13 / 19-20.
- -> S1: 6.5-8.5 / 12-15.
- -> S3: 6-7.5 / 10.5-13.
- -> M2: same / 9.5-12.
- -> R23: 5.5-7 / 7.5-10.
- -> R25/R17: -0.1-0.3 GPU, -0.5-2 CPU.

So: **90/90 in the field after the settings stage alone (likely). Blitz at 60 after S1 (likely). Blitz at 90 needs
M2 + R23 (uncertain). 120/120 field needs S1+S3+R23 and good luck with VD's compositor cost (borderline).**

| Order | Step | Gate to the next |
|---|---|---|
| 1 | Recipe (sec. 7) | `perf:` frame = 11.1 ms, xr wait > 1 ms, fps60 log shows no PERF-002 step-down for 10 min |
| 2 | D1 | Phase table, eye-2 Δ, per-target top list, CPU busy, in field + battle + blitz |
| 3 | M2 + E0 | Blitz span −1 ms or more, no colour/depth errors in the log |
| 4 | R23 2a (sim) -> 2b -> 2c | Sim: skip > 0, materialise ≈ 0. 2b: A/B toggles invisible in both eyes |
| 5 | R25 + R17 | Colour match; CPU busy down |
| 6 | M4 auto-MSAA | Per-class numbers from D1; latch never splits a burst |

Visual corruption cases to test after each code step:
- shadows (cascade edges) in both eyes;
- the blitz pool reflection in both eyes;
- the water-area history blend (it samples its own previous frame: must stay SH_NO, and will, because it samples a
  twinned SH_NO texture);
- Macalania ice and shiny floors;
- the first frame after a load (twins new -> SH_NO);
- a battle start (targets recreated? the latch);
- FMV frames (no eye 2 at all);
- the eye-2 preview and Measure still work.

---

## 7. Settings recipe the owner can try right away (90/90)

1. **fps60.ini**: `fps=90`. Keep `PERF-002=1`. Set `DIAG-FRAMETIME=1` for one session (main-thread frame-time
   histogram). The fps60 log must show `rate: 90 fps requested (3 passes ...)` and later
   `RATE-001: host limiter at 89.91 Hz`.
2. **Game V-Sync off**: GameSetting.ini `VSync=0`, or the game's Config page. Required above 60, or the monitor
   swapchain caps the loop.
3. **Game resolution 2560x1440**: `tools/game-settings.ps1 -Profile vr` (also writes MSAA=4, which the game ignores
   on this path; harmless). If the overlay shows the low-resolution warning, or VD's preset is above "High", use
   `-Width 2880 -Height 1620`.
4. **Mod MSAA**: keep `msaa=4` first. If `gpu` in the `perf:` line is above about 8.5 ms in the field, set `msaa=2`
   and restart.
5. **HDAO**: try `HDAO=0` in GameSetting.ini for one comparison in the same spot. Keep it if `gpu` drops and the look is
   acceptable.
6. **Virtual Desktop**: refresh **90 Hz**. Keep the current quality preset (recommended width about 2496). For judging
   smoothness set SSW to **Disabled**, so a missed frame shows as a hitch rather than ghosting. Go back to Automatic
   later if preferred.
7. **What to read** in `ffxvr_diag.log` `perf:` lines:
   - field `frame 11.1` with `max` under 25;
   - `gpu` ≤ about 8.5;
   - `xr ... wait` > 1 ms (headroom).
   In the fps60 log: no `PERF-002: 90 fps not held` lines.
8. **Fallbacks**:
   - If 90 doesn't hold in the field: VD **120 Hz with `fps=60`** gives an even 2:1 cadence (each frame shown twice,
     head motion still reprojected at 120). That alone removes today's 72 Hz 5:6 judder, with no GPU change.
   - For blitz: with S1 it should hold 60 (span about 12-15 ms). At 90 Hz, fps60 steps down to 60 there (3:2 cadence).
     Acceptable until M2/R23 land.
9. **Ideal next** (after R23/M2): VD 120 Hz + `fps=120` in the field, accepting PERF-002's 120 -> 90 -> 60 fallback.

---

## 8. Open questions

- **Q1 (D1 answers)**: Is the blitz refraction pass's CB recognised as the camera (per eye), or not (shareable like the
  reflection)? Which targets do the 20-28% same-input draws go to?
- **Q2 (D1f answers)**: Does the game recreate its large MSAA targets per area change, per battle, or only at start-up
  and on a resolution change? That decides how often M2/M4 can switch. The twin sweep comment says targets are reused
  across battles; design-notes 22 Oct saw recreation during some loads.
- **Q3 (exe-engine, later)**: What does the in-game MSAA/HDAO setting (`videoSettingMSAAHDAOPopUpMessageInit`, 0x6FCCA0;
  "HDAO Level" at 0x6938D0) do at runtime? Does it recreate the targets safely? If yes, M4 could switch without waiting
  for a load. Which GameSetting.ini keys does 0x62EE50 parse on the Fahrenheit launch path?
- **Q4 (owner)**: Is the installed fps60 the version with `fps=` multiples of 30 (its ini comment lists 60-240)?
