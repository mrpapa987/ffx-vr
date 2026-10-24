# G4 final: D3D resource lifetime, hook hygiene, OpenXR correctness (round 3)

Line numbers are from the 9a8715e snapshot. "native" is ffxvr_native.cpp, "xr" is ffxvr_xr.cpp and
"cs" is module.cs. I checked every objection in G4-critique.md against the code, design-notes and
spec text, and every one holds. Every item is **AGREED**; nothing is disputed. Each answer is under
its item.

## Answers to the main objections

1. **R11(b) would black out 2D screens: accepted, gate withdrawn.** design-notes 55-60 says 2D
   frames have "no clear [that] ends the overlay phase" and draw "straight to the backbuffer", and
   496-503 says the save screen draws nothing at all. So `g_bb_bound_by_game` would stay false on
   every title, menu, loading, FMV and save-screen frame. That means e1 = null and zero layers, i.e.
   a black screen. My gate also had a hole of its own: in a save-screen frame right after a resize,
   xr_tick runs before anything binds the new buffer. It is replaced by the critic's **destruction
   sentinel** (below), which nulls `g_backbuffer` exactly when the texture dies. G2-final's
   begin_frame step 11 "G4: g_bb_bound_by_game = false" is **dropped**; please strike it.
2. **Zero layers is a black frame by spec: accepted.** xrEndFrame clears the display when no layers
   are given. Today every submit failure already does this, but R8's retry path and R15 would make it
   more frequent. R8 now includes a "hold last frame" mechanism (designed once in xr_frame), and R15
   uses it. The R8 log-throttle bug and the repeated 250 ms waits are both fixed.
3. **R9 map-age prune: accepted, dropped.** A VirtualQuery liveness check is added as the backstop
   that still works under the stage-0 debugger (SEH doesn't).
4. Smaller points, all accepted:
   - The stale `g_bb_w/h` cache is fixed by the sentinel, plus a refresh at every tick and every bind.
   - The eager release of the backbuffer twin is replaced by a release two frames later.
   - R12.4 reverts to "count only".

## begin_frame (fits G2-final's agreed order)

- Step 7: `twin_srv_trim()` (R12), followed by `bb_dead_twin_release()` (R11b).
- Step 9: `xr_tick()` with no early return (R15).
- Step 11: no G4 addition (the bb gate is withdrawn).
- No other per-frame work.

## ABI

eye2_begin/eye2_end and every existing export keep their signatures. There is one new export,
`void ffxvr_xr_debug(int drop_image, int sim_timeouts)` (R8 test hook + R15 checkbox). Managed loads
it as optional (R13 TryGetExport) and calls it from the expanded Debug header. Under G1's rule this
bumps `ffxvr_abi()`; G1 should count it. ffxvr_xr.h gains
`void xr_debug(int sim_timeouts, bool drop_image)` (internal, not exported).

---

## R8 - Swapchain wait timeout (+ hold the last frame)

Final verdict: **fix now**, in two build steps: R8a (state machine), then R8b (hold the last frame).
Status: **AGREED.**

Answers:
- Log throttle: count first, log second (accepted).
- Retry waits: 250 ms only for the first wait after an acquire; 2 ms on retries (accepted). A long
  VD stall then costs the monitor about 2 ms per frame, not 250.
- The `stage()` strings stay (accepted).
- The test hook simulates timeouts instead of using `timeout = 0` (accepted; with three images a
  zero wait usually succeeds).
- Hold the last frame (accepted, R8b). To avoid a left-new/right-old projection, every swapchain a
  layer needs is acquired *and* waited before any copy.

Final design (xr):
```cpp
enum ScState : uint8_t { SC_IDLE, SC_ACQUIRED, SC_WAITED };
struct Eye { ...existing...; ScState st = SC_IDLE; uint32_t idx = 0; uint32_t retries = 0; bool ever_released = false; };
int g_sim_timeouts = 0; bool g_drop_image = false;              // xr_debug()

// Acquire if idle, wait if acquired. Never releases. True when the image is waited (ours to write).
bool prepare(int i) {
    Eye& e = g_eye[i];
    if (e.st == SC_IDLE) { stage("acquire"); XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        if (XR_FAILED(p_xrAcquireSwapchainImage(e.sc, &ai, &e.idx))) return false; e.st = SC_ACQUIRED; e.retries = 0; }
    if (e.st == SC_ACQUIRED) {
        stage("wait image");
        XrResult r;
        if (g_sim_timeouts > 0) { g_sim_timeouts--; r = XR_TIMEOUT_EXPIRED; }
        else { XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO }; wi.timeout = e.retries ? 2000000 : 250000000; r = p_xrWaitSwapchainImage(e.sc, &wi); }
        if (r != XR_SUCCESS) {                                       // TIMEOUT_EXPIRED or an error: stays acquired, wait again next frame
            e.retries++; ++g_wait_timeouts;
            if (g_wait_timeouts <= 5 || e.retries % 300 == 1) g_h.diag("xr: swapchain %d wait %s (try %u)", i, res_str(r), e.retries);
            return false;
        }
        if (e.retries) g_h.diag("xr: swapchain %d wait recovered after %u tries", i, e.retries);
        e.st = SC_WAITED;
    }
    return true;
}
void copy_release(int i, ID3D11Texture2D* src) {                     // only for SC_WAITED
    Eye& e = g_eye[i]; stage("copy");
    if (src && g_h.copy(src, e.mid_rtv, e.w, e.h, i == 3 || i == 4)) g_h.ctx->CopyResource(e.images[e.idx], e.mid);
    stage("release image"); XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    if (XR_SUCCEEDED(p_xrReleaseSwapchainImage(e.sc, &ri))) e.ever_released = true;
    e.st = SC_IDLE;
}
// All or nothing: prepare every needed swapchain (no short-circuit), then copy and release all.
// If any is not ready, nothing is released, and the ready ones stay SC_WAITED into the next frame (legal).
bool submit_set(const int* ids, ID3D11Texture2D* const* srcs, int n) {
    bool all = true; for (int k = 0; k < n; ++k) all &= prepare(ids[k]);
    if (!all) return false;
    for (int k = 0; k < n; ++k) copy_release(ids[k], srcs[k]);
    return true;
}
```
Steps:
- **R8a** replaces `submit_eye` with this code (`submit_eye(i, src)` = `submit_set(&i, &src, 1)`).
  The base layer uses `submit_set({0,1})` or `{2}` / `{2,5}`; UI (3) and ImGui (4) are on their own.
  The behaviour on failure is unchanged from today (layer dropped, zero layers if the base failed).
  This alone is shippable.
- **R8b** adds a file-scope `Held` with the last *fresh* base layer (`proj` + `pv[2]`, or
  `quad`/`quad_r`), the `use_screen`/`stereo` flags and `uiq`/`imq`, plus `age` and `have_*` flags.
  Per frame in xr_frame:
  1. If `g_should_render && g_render_views_valid && eye1 && !g_drop_image`, try `submit_set` for the
     base. On success, build the base layer as today and store it in `Held` (`age = 0`).
  2. Otherwise, if `g_should_render && held.have_base && held.age < 30`, every swapchain it
     references has `ever_released`, and no space change happened since, re-send the held base
     layer (`age++`, with `g_held_frames++` for the heartbeat). The held `pv[i].pose` are the poses
     those images were rendered with; displayTime is the new one.
  3. UI and ImGui quads are submitted fresh if their own `submit_set` succeeds, otherwise held
     (same age rule), and only when a base layer goes out this frame.
  4. If no base layer is available, end with zero layers (as today).
- `destroy_eyes()` resets `st/idx/retries/ever_released`. xr_stop and the
  `REFERENCE_SPACE_CHANGE_PENDING` handler clear `Held`.
- `!g_should_render` frames keep today's zero layers.

Interactions:
- R15 uses step 2 for its no-image frames.
- R25 (later) rewrites `copy_release`.
- G2 owns `fail()` dedup; this item has its own throttle and doesn't call `fail()`.

Smoke test:
1. Normal play in 3D, the screen, the stereo screen, the UI panel and the ImGui layer: no change.
2. Debug "simulate 20 swapchain timeouts": the headset holds the last image (no black flash) and the
   log shows "try 1" ... "recovered after N tries".
3. Same with 60: about 30 frames held, then black, then recovery.
4. VD hiccups (change the quality setting mid-game, Wi-Fi edge, take the headset off for 10-20 s):
   the layers come back, and the monitor frame rate doesn't collapse during the stall (perf line).

Effort: R8a **S**, R8b **M**.

---

## R9 - Stale UI vertex pointers

Final verdict: **fix now**: purge on CreateBuffer, a per-frame VirtualQuery liveness check, no age
window and no time prune. Status: **AGREED.**

Answers: the time prune is dropped (it was an age window). The liveness check and the atomic gate
are accepted. Cap: 512 entries, evicting the oldest by last UI draw (in practice never reached).

Final design (native):
- `struct VbMap { void* p; UINT size; uint32_t frame; uint32_t last_draw; uint32_t checked_frame = ~0u; bool readable = false; };`
- hk_Map (3186): store `p`/`size`/`frame` and set `checked_frame = ~0u`, so a new pointer is
  re-checked.
- `std::atomic<bool> g_ui_vbs_any` is set at the first insert into `g_ui_vbs` (1994, 2064, 2108).
- `hk_CreateBuffer` gets one edit with R16:
  - first, the device filter;
  - then today's CB record;
  - then `if (g_ui_vbs_any.load(relaxed) && SUCCEEDED(hr) && *out) { lock; if (g_ui_vbs.erase(*out) | g_vb_last_map.erase(*out)) g_vb_purged++; }`;
  - and the comment: "never call CreateBuffer while holding g_mutex (this hook locks it)".
- `static bool range_readable(const void* p, size_t len)`: the VirtualQuery loop factored out of
  `ffxvr_safe_read` (1648-1664). safe_read then calls it.
- `static bool vb_live(VbMap& m)`:
  `if (m.checked_frame != g_frames) { m.checked_frame = g_frames; m.readable = range_readable(m.p, m.size); if (!m.readable) g_vb_unreadable++; } return m.readable;`
  It is called where the record is used (ui_sprite_xy 1996, ui_pick_check 2066, ui_sprite_rect
  2111), under the existing lock. If it fails, the read is skipped (rect unknown). That is about one
  syscall per UI buffer per frame.
- The lookups also set `last_draw = g_frames`. On insert, if `g_vb_last_map.size() > 512`, erase the
  entry with the smallest `last_draw`.
- Diagnostics: `vb_purged`, `vb_unreadable`, `rect_age_max` (the maximum `g_frames - frame` at a
  successful read), shown on G2's heartbeat line.

Smoke test:
1. Battle with hide_cursor and hide_pointers on: hidden-draw and "rect unknown" counts as before.
2. Ten-plus area changes, battle entries and exits, Save Sphere then battle: the heartbeat shows
   `vb_purged` (0 or small) and `vb_unreadable` = 0, and `rect_age_max` gives the data for any
   future decision.
3. F9 pick and F10 slow motion on the target cursor still report a rect.

Effort: **S.**

---

## R10 - Pick stack overflow

Final verdict: **fix now**. Status: **AGREED.**
- `guarded_read_floats(p, out, nf)` with `nf = min(sstride/4, 4)` replaces 2073-2075.
- The `vb_live` check (R9) covers this path too.
- `guarded_read_xy` gets the comment "writes 8 floats".

Smoke test: F9 pick on the HP window and the target cursor. The `v1(stride ..)` values are as
before, with no crash.

Effort: **S.**

---

## R11 - Raw tracked D3D views

Final verdict: **fix now**:
- (a) eye2_begin/end on the real bindings;
- (b) the backbuffer through a destruction sentinel;
- (c) bind-time caches for RTV0 and `ps_tex()` for the UI paths.

Status: **AGREED.**

Answers:
- (a) The ordering comments, `om_swapped`, `srv_mismatch` and G2's tweak 1 (resolve t0 once) are
  accepted.
- (b) The sentinel is adopted and the gate withdrawn. The cache is refreshed at every tick and bind.
  The eager twin release is replaced by a release two frames later.
- (c) The desc cache (also G2's tweak 2), the 4619 fix, and KEEP-UAV restore in the copy and marker
  passes are accepted.

### (a) eye2_begin / eye2_end (G4 owns them; native 4208-4331)

```cpp
struct E2Saved { ID3D11RenderTargetView* rtv[8]; ID3D11DepthStencilView* dsv; UINT nrtv;
                 ID3D11ShaderResourceView* srv[16]; UINT nsrv; bool om_swapped, srv_swapped, valid; };
static E2Saved g_e2s;
UINT g_ps_srv_hi = 1;                                        // max(start+i+1) over non-null game binds (on_ps_srvs)
uint32_t g_e2_track_mismatch = 0, g_e2_srv_mismatch = 0, g_e2_rtv_over4 = 0;
static void e2_save();      // OMGetRenderTargets(8), nrtv = last non-null + 1, PSGetShaderResources(0, g_ps_srv_hi);
                            // count rtv0 != g_cur_rtv[0] and srv[i] != g_cur_ps_srv[i] (diagnostics)
static void e2_release();   // Release all saved views; valid = false
static void e2_restore() {  // MUST run while g_in_eye2 is still true (the OM restore passes the managed detour -> on_rendertargets_uav)
    if (g_e2s.om_swapped)  g_ctx->OMSetRenderTargetsAndUAVs(g_e2s.nrtv, g_e2s.rtv, g_e2s.dsv, g_e2s.nrtv, D3D11_KEEP_UNORDERED_ACCESS_VIEWS, nullptr, nullptr);
    if (g_e2s.srv_swapped) g_ctx->PSSetShaderResources(0, g_e2s.nsrv, g_e2s.srv);
    e2_release();
}
```

eye2_begin:
- After the cheap `g_stereo/g_ctx/g_device` tests, call `e2_save()`. Every `return 0` calls
  `e2_release()`. The bail at 4224 becomes `nrtv == 0 && !dsv`.
- `t0 = nrtv ? (rtv[0] == g_cur_rtv[0] ? g_cur_rtv0_tex : texture_of(rtv[0])) : nullptr` is used for
  `rtvs[0]` (via `twin_of(t0)->rtv`), `to_bb` and `gt`. Twins for slots 1-7 come from
  `twin_rtv(rtv[i])`, and the DSV twin from `twin_dsv(dsv)`.
- **Set `g_in_eye2 = true` before the first bind** (comment at the site). Then
  `OMSetRenderTargetsAndUAVs(nrtv, twins, dsv_twin, nrtv, KEEP, ...)` and `om_swapped = true`.
- The SRV loop runs over `nsrv` real views; `if (any) { PSSetShaderResources(0, nsrv, srvs); srv_swapped = true; }`.
  `g_saved_ps_srv` is removed.
- UI/ImGui branch: `e2_save()`, bind the one target, `om_swapped = true`; blend save as today.
- Split pass 0: `e2_save()` only; nothing is swapped, so the end only releases.

eye2_end:
- `e2_restore()` replaces 4315 and 4323 and runs **before** `g_in_eye2 = false` (comment at the
  site). The UI branch's blend restore and the CB restore (4324-4328) are unchanged.

This also covers **R42 (4 RTVs, UAVs)**: up to 8 RTVs are restored and PS UAVs are kept.
`g_e2_rtv_over4` counts binds with n > 4 in on_rendertargets.

### `ffxvr_on_ps_srvs` (G4 owns it; includes G2's R3 gate)

```cpp
if (g_in_eye2) return;
for (unsigned i = 0; i < n && start + i < 16; ++i) {
    ID3D11ShaderResourceView* v = views ? static_cast<ID3D11ShaderResourceView*>(views[i]) : nullptr;
    g_cur_ps_srv[start + i] = v; if (v && start + i + 1 > g_ps_srv_hi) g_ps_srv_hi = start + i + 1;
}
if (views && g_stereo && snap_wanted())                                      // G2 R3
    for (unsigned i = 0; i < n; ++i) if (views[i] && snapshot_if_scene(texture_of(static_cast<ID3D11View*>(views[i])), 1)) break;
```

### (b) Backbuffer: destruction sentinel

```cpp
static const GUID kBbSentinel = { /* fixed random GUID */ };
ID3D11Texture2D* volatile g_backbuffer;                              // was a plain pointer (382); may be nulled from another thread
volatile LONG g_bb_died = 0; ID3D11Texture2D* volatile g_bb_dead = nullptr; uint32_t g_bb_dead_frame = 0;
UINT g_bb_w = 0, g_bb_h = 0; DXGI_FORMAT g_bb_fmt = DXGI_FORMAT_UNKNOWN;
struct BbSentinel final : IUnknown {                                 // owned by the texture's private data; holds NO ref on it
    volatile LONG refs = 0; ID3D11Texture2D* tex;
    explicit BbSentinel(ID3D11Texture2D* t) : tex(t) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** p) override { if (r == __uuidof(IUnknown)) { *p = this; AddRef(); return S_OK; } *p = nullptr; return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override {                     // the final release runs when the texture is destroyed (ResizeBuffers / swapchain release), any thread
        const LONG n = InterlockedDecrement(&refs);
        if (n == 0) { if (InterlockedCompareExchangePointer((PVOID volatile*)&g_backbuffer, nullptr, tex) == tex) { g_bb_dead = tex; InterlockedIncrement(&g_bb_died); } delete this; }
        return (ULONG)n;
    }
};
static void bb_attach(ID3D11Texture2D* t) {                          // at identification (1784), before g_backbuffer = t
    IUnknown* have = nullptr; UINT sz = sizeof(have);
    if (SUCCEEDED(t->GetPrivateData(kBbSentinel, &sz, &have)) && have) { have->Release(); return; }   // already watched: replacing would fire the old one
    auto* s = new BbSentinel(t); if (FAILED(t->SetPrivateDataInterface(kBbSentinel, s))) delete s;
}
```
- Identification (1781-1785): `bb_attach(t); g_backbuffer = t;` and cache `g_bb_w/h/fmt` from `d`.
  The cache is also refreshed when the known backbuffer is seen bound (1787), and at 5406's GetDesc
  every tick.
- Dereference sites keep `if (g_backbuffer)`:
  - 5395 and 5406-5408: per-tick GetDesc kept. While non-null, the pointer is alive.
  - 3875 uses `g_bb_w/g_bb_h`, with no dereference.
  - 4614-4631 (measure/dump) keep their null test.
- After the sentinel fires, the next bind by anyone (game or overlay) re-identifies the buffer, even
  at the same address.
- `bb_dead_twin_release()` at begin_frame step 7: if `g_bb_dead` and
  `g_frames >= g_bb_dead_frame + 2` (`g_bb_dead_frame` is stamped on the first frame it's seen
  non-null), and the key is no longer `g_backbuffer`, release and erase that twin. Then clear
  `g_bb_dead` and log "bb sentinel fired (N)". Two frames later, ImGui has rendered every draw list
  that could hold its SRV.
- Threading assumption, written as a comment: ResizeBuffers runs on the render thread (the immediate
  context isn't thread-safe anyway). The sentinel only does interlocked stores.
- Fallback, if testing shows the sentinel doesn't fire on a resolution change (no "bb sentinel fired"
  line): keep today's per-tick GetDesc and ship only the 3875 cache.

### (c) Bind-time caches

- In on_rendertargets: `g_cur_rtv0_tex = t` (already computed at 1780), and one `GetDesc` per bind
  gives `g_cur_rtv0_w/h/samples` (null/0 when n == 0).
- Use the cache instead of a dereference in on_draw at 3714, 3726, 3738, 3741, 3744, 3858, 3861,
  3869-3876, 3896, 3905-3906, and in 4418/4427.
- `ps_tex(slot)` (`PSGetShaderResources(slot, 1)` → `texture_of` → Release) replaces
  `texture_of(g_cur_ps_srv[..])` at 1968, 2012, 3624-3625, 3751, 3763, 3828, 3922, 3931.
- 4619: `g_snap_tex->GetDesc` (our own texture, same size and format) instead of `g_snap_from`.
- Accepted as within-frame, low risk: 4179 (`g_scene_resolved`) and 5447 (`g_scene_live_tex`).
- The copy pass (4781/4804) and the marker passes (4892/4952, 5000/5024) save 8 RTVs and restore with
  `OMSetRenderTargetsAndUAVs(n, ..., n, KEEP, ...)`.

Interactions:
- R12 (same functions; trim at step 7).
- R15 (sentinel-null frames have no image and are held).
- G2's R3 gate (in on_ps_srvs above) and R18/R23 (t0, `any`).

Smoke test:
1. Normal 3D, battle, cutscene, stereo screen: eye 2 identical. The heartbeat shows
   `e2_track_mismatch` / `srv_mismatch` (evidence) and `rtv_over4`. The perf line's `eye2` and
   `frame` are within noise.
2. **2D walk with the ImGui layer on:** title, main menu, load screen, loading, an FMV, the save
   screen (Save Sphere), the in-game menu. None is black in the headset.
3. Resolution change in game settings, fullscreen↔windowed, alt-tab: "bb sentinel fired" in the
   diag, at most a held frame in the headset, no crash. A resize while the save screen is open.
4. Ten-plus area changes.
5. Cursor hiding counts unchanged.

Effort: **M.**

---

## R12 - twin_srv eviction, failed twins, fail() leak, missing depth twin

Final verdict: **fix now**. Status: **AGREED.**

Answers:
- (1) The ordering statement was wrong (the preview is recorded before begin_frame, cs 1953 vs
  1957). The invariant is now written down.
- (2) Backoff accepted.
- (4) Count only (accepted).

Final design:
1. `twin_srv` at ≥ 256 sets `g_twin_srv_trim = true` and keeps adding. `twin_srv_trim()` at step 7
   releases and clears every entry **except the `nullptr` key**, with the comment "ImGui holds
   srvs[nullptr] across begin_frame (ffxvr_eye2_final_srv, cs 1953)". It counts
   `g_twin_srvs_dropped`.
2. Failed record = desc fields + `fail_frame` + `retry_after` (600, doubling per failure up to
   36000). twin_of: for a failed record, return it while the desc matches and
   `g_frames - fail_frame < retry_after`; otherwise erase and retry (a different desc retries at
   once). Failed records don't refresh `last_used`. The `twin FAILED` diag is capped at 20 lines.
3. The `fail` lambda releases `t.rtv` and `t.dsv` before `t.tex`.
4. `g_e2_no_depth_twin++` when the game has a DSV and its twin is null. Behaviour is unchanged (the
   draw still goes out without depth). It goes on the heartbeat's eye-2 line.

Smoke test:
- A long session: the heartbeat shows `twin srvs dropped` and `no_depth_twin` = 0, and the eye
  preview keeps working.
- Optional test build with the limit at 8: trims every frame, eye 2 and the preview stay correct.

Effort: **S-M.**

---

## R14 - Stale fields in reused ring slots

Final verdict: **fix now**. Status: **AGREED.**
- `ring_take(buf, size, kind)` resets `cb/size/frame/kind/has_world/pal_hands/split` and calls
  `eye1b/eye2b.clear()`. It is used at 2711, 2757 and 2781. Land this before G2's R20, which edits
  2780-2787.
- Consumers check sizes: split passes only when `eye1b.size() == size` (3959, 4230, 4306).

Smoke test: 3D, backdrop rooms and a battle. If reachable: split hands on, then off, then a
backdrop room.

Effort: **S.**

---

## R15 - No image means no xr_frame

Final verdict: **fix now**. Status: **AGREED.**

Answer: zero layers is black by spec (accepted). No-image frames use R8b's hold (at most 30
frames), then zero layers.

Final design:
- native 5420: no early return. The blink (5422) already needs `e1 == g_bb_copy`; the sequence dump
  becomes `if (e1 && g_seq_left > 0)`.
- `xr_debug`'s `drop_image` forces `e1 = nullptr` (Debug checkbox).
- xr 499: `ok` also needs `eye1 != nullptr`. The no-image path is R8b step 2.
- Before R8b lands, R15 still ships: the frame gets zero layers (as any failure does today), and
  events, wait, begin and locate keep running. Dependencies: G2's `fail()` dedup (R7b), and G3's
  `leave_xr_state()` at xr_tick's exit.

Smoke test:
- "Drop eye image" checkbox: up to 30 frames held, then black. The status stays "focused", VD shows
  no hang, recentre and the Quest menu work. Unticking resumes.
- Resolution change (sentinel-null frame) as in R11.

Effort: **S** (on top of R8b).

---

## R16 - Hook install hygiene

Final verdict: **fix now**: idempotent `patch`, `unpatch_all` on a failed init (restoring only slots
that still hold our pointer), and the device filter in both device hooks. **Won't fix**: unpatching at
exit. Status: **AGREED.**

The hk_CreateBuffer edit is shared with R9 (filter first). VDXR uses `g_device` (xr 363), so it
passes the filter as today.

Smoke test: the overlay status shows "hooked", the patched list has 5 entries, `msaa:` lines are
for the game's targets only, and the MSAA override still works after an area change.

Effort: **S.**

---

## R25 - Double copy, six swapchains

Final verdict: **fix later**, with the blitzball performance work or the presenter thread. Status:
**AGREED.**

Additions for the later design (accepted):
- Request `XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT`, keeping `mid` as the fallback if that fails.
- Or render straight through an SRGB RTV with a copy shader that decodes sRGB→linear (no typeless
  images needed, within 1 LSB).
- Create the swapchains for layers 4 and 5 only when first used.
- Implement this inside R8's `copy_release`.

Smoke test (later): colour match in headset vs monitor, `gpu` and `vram` in the perf line.

Effort: **M.**

---

## R41 - Lost position tracking

Final verdict: **fix now**. Status: **AGREED.**
- Substitute the positions before `memcpy(g_render_views)` (xr 604): hold the last valid head centre
  and put the eyes at ±IPD/2 along each view's own orientation.
- `kIpdMin = 0.04f, kIpdMax = 0.09f, kIpdDefault = 0.063f`.
- Reset `g_have_last_pos` in xr_stop and on `REFERENCE_SPACE_CHANGE_PENDING` (LOCAL, xr 454-458).
- One diag line per tracking-loss episode, with the held IPD.

Smoke test: cover the Quest's cameras for 5-10 s in 3D. `eye sep` stays about 0.06 m, the world
doesn't jump, and position resumes when uncovered. Recentre while covered works.

Effort: **S.**

---

## R42 (parts)

- **Export argument checks**: **fix now**. Status: **AGREED.**
  - `world_positions`, `ui_hide_list`, `draw_skip`, `ui_hide_hits`, `ui_hover_rects`, `markers_set`,
    `screen3d_status`, `perf_status` (`buf == null || n <= 0`), `vrpad_status` get null/size guards.
  - Signatures are unchanged, so there's no ABI bump.
  - Smoke test: the overlay pages using them still show data. Effort **S**.
- **eye2_end 4 RTVs / UAVs**: **fix now**, inside R11(a), plus `g_e2_rtv_over4`. Status: **AGREED.**
- **xr_stop blocks up to 1 s**: **fix now**. Status: **AGREED.**
  - An auto-reset `g_watchdog_wake` event replaces `Sleep(1000)` (xr 36). It is created before
    CreateThread (xr 424); xr_stop calls SetEvent and joins, then closes the event.
  - Smoke test: headset toggle off/on in the overlay; the perf line's `max` shows no ~1000 ms spike.
  - Effort **S**.
- **xr_stop never requests exit**: **defer** the graceful request-exit. **Fix now (S)**: call
  `xrEndSession` at xr 288 only when `g_state == XR_SESSION_STATE_STOPPING`; poll_events already ends
  the session on STOPPING. Status: **AGREED.**

---

## Final table

| Item | Final verdict | Status | Effort |
|---|---|---|---|
| R8 | fix now: R8a state machine, then R8b hold the last frame (≤30) | AGREED | S + M |
| R9 | fix now: purge on create + VirtualQuery liveness, no age/prune | AGREED | S |
| R10 | fix now | AGREED | S |
| R11 | fix now: real bindings (a), sentinel (b), bind-time caches (c) | AGREED | M |
| R12 | fix now: deferred trim (keeps srvs[nullptr]), backoff retry, leak, count no-depth | AGREED | S-M |
| R14 | fix now | AGREED | S |
| R15 | fix now (holds via R8b) | AGREED | S |
| R16 | fix now; unpatch at exit won't fix | AGREED | S |
| R25 | fix later | AGREED | M |
| R41 | fix now | AGREED | S |
| R42 exports / 8 RTVs / watchdog wake / EndSession only in STOPPING | fix now | AGREED | S |
| R42 graceful request-exit | defer | AGREED | M |

Build order:
1. R10, R14, R16+R9, R42 exports.
2. R8a, R15, R41, watchdog wake, EndSession (one VD session).
3. R12.
4. R11, alone, with the 2D walk.
5. R8b.
