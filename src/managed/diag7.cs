// SPDX-License-Identifier: MIT

namespace FFXVR;

// Build 7: battle-cursor and engine diagnostics, LOG ONLY (design-notes "Build 7: battle-cursor and engine diagnostics
// (log only)"; research SYNTHESIS.md stage C1 and section 6). Nothing here changes what the game or the headset shows:
// every hooked game function is always called, and the only drawing is the opt-in "show cursor anchors" dots on the
// monitor (Debug > Build 7). The ini switch diag_build7 (default 0 since the loads build) gates the logging, the native stall
// watchdog and the XInputSetState hook's log (the hook itself also follows haptics). The four game-function hooks belong to
// the features that use them (loads build): the target and turn pointers to cursor_source=game (C2), the job join to
// smooth_anchor, the backbuffer resize (build 9's invalidation signal) to nothing, it is always installed; each is also
// installed with diag_build7=1. All at launch.
//
// Threads: the four game-function detours run on the game's MAIN thread (the job join, the battle-UI draw, the resize).
// They only copy values into two-slot seqlocks and count; no strings, no logging, no D3D. The render thread
// (Present, diag7_present) reads the snapshots and writes the log lines into ffxvr_diag.log through ffxvr_diag_line.
public sealed unsafe partial class FFXVRModule {

    private bool   _diag7 = false;                // ini diag_build7 (default 0 since the loads build: the logs; the hooks follow their features)
    private string _d7_status = "not set up";
    private int    _d7_native_on = 0;             // what ffxvr_diag7_set was last told
    private delegate* unmanaged[Cdecl]<int, void> _fn_diag7_set;
    private delegate* unmanaged[Cdecl]<byte*, void> _fn_diag_line;
    private delegate* unmanaged[Cdecl]<int> _fn_main_note;
    private delegate* unmanaged[Cdecl]<float*, int, void> _fn_motion_cols;
    private delegate* unmanaged[Cdecl]<int, int, int, void> _fn_bb_resized;   // build 9 (R11b): the 0x599210 hook tells native a backbuffer re-acquire begins (0) / is done (1)

    // -- the game functions (FhUtil offsets: the exe is DYNAMIC_BASE) --------------------------------------------------
    private const int k7_tgt  = 0x49C870;   // target pointer draw: cdecl void(), called from the target window's draw stub (0x4A84C0)
    private const int k7_turn = 0x492590;   // turn pointer draw: cdecl void(int chr). NOT Fahrenheit's generated d_UnknownFn (no argument)
    private const int k7_join = 0x644B30;   // job-join wrapper: cdecl void(). Sg_MainLoop's post-join call is the first after the frame counter moved
    private const int k7_rsz  = 0x199210;   // backbuffer re-acquire (ResizeBuffers + GetBuffer): thiscall void(obj, int w, int h), ret 8
    private const int k7_stub = 0x4A84C0;   // the target window's +0x90 draw callback (calls k7_tgt)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]    private delegate void d7_void();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]    private delegate void d7_turn(int chr);
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] private delegate void d7_resize(nint self, int w, int h);
    private d7_void?   _h7_tgt,  _o7_tgt,  _h7_join, _o7_join;
    private d7_turn?   _h7_turn, _o7_turn;
    private bool _h7_tgt_ok, _h7_turn_ok, _d7_last_ok;   // C2-5: the cursor hooks were queued (not refused, unresolved or failed)
    private d7_resize? _h7_rsz,  _o7_rsz;
    private nint _a7_tgt, _a7_turn, _a7_join, _a7_rsz;   // absolute addresses, resolved once at init (no FhCallLocation module lookup later)
    // B7-1: the originals are resolved through Fahrenheit's chain table, which is not locked for readers. Our render thread
    // resolves all four at the top of render_imgui_inner, before try_install_hooks (our own late insertions), so after that no
    // detour touches the table. A detour that runs before the first Present (the join and the resize can) resolves on the
    // main thread instead (d7_resolve_main): it retries until Fahrenheit has written the chain entry (it does so right after
    // enabling the hook) and is excluded from our render-thread insertions by the _d7_inserting / _d7_resolving handshake.
    private int _d7_inserting, _d7_resolving; private bool _d7_resolved_rt;

    // -- snapshots (written on the main thread, read at Present) -----------------------------------------------------
    private struct D7Anchor { public int id, idx, fallback, flags, chr10, sx, sy; public Vector3 opt, tl, td; public float heading; public nint actor; }   // flags: 1 attached, 2 override matrix, 4 index outside 0..25
    private struct D7TgtHdr { public uint stamp, mask, cmd, gate, autob, alive, stub_states; public int n, skipped, stubs, bstate; }
    private struct D7Turn   { public uint stamp, apex, bas; public int chr, chr_glob, ok, skipped; public float theta; public Vector3 game_a; public D7Anchor a; }   // game_a: the game's own A (0xF3FA68), read after the original; skipped: C2 skipped the original, so game_a is stale
    private struct D7Join   { public uint stamp; public int exec, valid, head_ok, flags, pvalid; public Vector3 pos, off, td, head, ppos, poff, ptd; public nint actor, parent; }   // smooth anchor: head (game space, push_player's rule), flags 1 attached (+0x19C), 2 override matrix (+0x10C), 4 the head was folded
    private readonly D7TgtHdr[] _d7h = new D7TgtHdr[2]; private readonly D7Anchor[] _d7t = new D7Anchor[2 * 28]; private int _d7_seq;
    private readonly D7Turn[]   _d7u = new D7Turn[2];   private int _d7u_seq;
    private readonly D7Join[]   _d7j = new D7Join[2];   private int _d7j_seq;
    private nint _d7_follow_actor;                       // push_player's pick (render thread), read by the join hook
    // main-thread bookkeeping (plain ints: one writer; the render thread only reads them for logs)
    private uint _d7_tgt_last_stamp = uint.MaxValue, _d7_join_last_stamp = uint.MaxValue; private int _d7_tgt_calls_frame, _d7_tgt_calls_max;
    private int  _d7_tgt_calls_auto, _d7_tgt_calls_total, _d7_turn_calls_total, _d7_join_calls_total, _d7_join_post_total, _d7_rsz_calls, _d7_main_faults;
    private int  _d7_rsz_w, _d7_rsz_h, _d7_join_mtid; private uint _d7_rsz_frame; private bool _d7_rsz_main;

    // -- render-thread state ----------------------------------------------------------------------------------------
    private D7TgtHdr _r7h; private readonly D7Anchor[] _r7t = new D7Anchor[28], _r7t_tmp = new D7Anchor[28]; private D7Turn _r7u; private D7Join _r7j;
    private int  _r7_seq, _r7u_seq, _r7j_seq, _r7_seq_prev, _r7u_seq_prev, _r7j_seq_prev, _r7_since_t = 999, _r7_since_u = 999, _r7_since_j = 999;
    private bool _d7_hello, _d7_dots, _d7_in_btl, _d7_btl_elems; private uint _d7_btl_frame0; private int _d7_faults_logged;
    private int  _d7_seen_tgt, _d7_seen_turn, _d7_seen_join, _d7_seen_rsz, _d7_rsz_seen;
    private uint _d7_key_mask = 0; private int _d7_key_turn = -1; private bool _d7_key_active; private double _d7_line_t, _d7_sec_t; private int _d7_lines_sec;
    private readonly float[] _d7_hd_last = new float[32], _d7_hd_delta = new float[32]; private readonly bool[] _d7_hd_seen = new bool[32]; private readonly uint[] _d7_hd_stamp = new uint[32];
    private readonly int[] _d7_hist_c = new int[8], _d7_hist_u = new int[8], _d7_hist_j = new int[8]; private int _d7_presents, _d7_t_calls0, _d7_u_calls0, _d7_j_calls0, _d7_j_post0; private uint _d7_frame0; private double _d7_hist_t;
    private ulong _d7_ps_m0 = ulong.MaxValue, _d7_ps_m1 = ulong.MaxValue; private double _d7_ps_t; private int _d7_ps_lines;

    private static string I(FormattableString f) => FormattableString.Invariant(f);
    /// <summary>Text for ImGui.Text* (and SetTooltip): Hexa passes our string to ImGui as a printf FORMAT, so a '%' in it prints
    /// stack garbage, or fail-fasts the game (UCRT's invalid-parameter handler, 0xC0000409: no catch stops it). Rule: every
    /// NON-LITERAL argument (a field, a helper's return value, a native or file or game string, an exception message) goes
    /// through this, unless it is a pure number format; literal text writes "%%" (docs/release.md section 5).</summary>
    private static string esc(string? s) => (s ?? "").Replace("%", "%%");

    /// <summary>At module init (Fahrenheit queues hooks added here and applies them after every module has initialised;
    /// it refuses a late hook on an already-hooked function). Needs the native helper for the guarded reads and the log.</summary>
    private void diag7_init() {
        if (!_native_ok || _fn_safe_read == null || _fn_diag7_set == null) { _d7_status = "off (the native helper did not load)"; _logger.Info("build 7 diagnostics: " + _d7_status); return; }
        // Loads build: each hook under the feature that needs it (and all of them with diag_build7=1), decided at launch
        // The job join is always hooked (chocobo re-review MF-1): the shadow pool guard installs from it, on the main thread
        // (shadow_cam.cs, sg_join_install). Its snapshot work (d7_join_record) still runs only with diag_build7 or smooth_anchor.
        bool want_cur = _diag7 || _cursor_game_cfg, want_join = true;
        if (_diag7) { _fn_diag7_set(1); _d7_native_on = 1; }
        _h7_rsz = h7_rsz; new FhCallLocation("FFX.exe", k7_rsz).try_resolve(out _a7_rsz);
        if (want_cur) { _h7_tgt = h7_tgt; _h7_turn = h7_turn; new FhCallLocation("FFX.exe", k7_tgt).try_resolve(out _a7_tgt); new FhCallLocation("FFX.exe", k7_turn).try_resolve(out _a7_turn); }
        if (want_join) { _h7_join = h7_join; new FhCallLocation("FFX.exe", k7_join).try_resolve(out _a7_join); }
        string one(string name, nint addr, Func<bool> f) {
            if (addr == 0) { _logger.Error($"build 7 diagnostics: hook {name}: FFX.exe not found"); return name + " NOT RESOLVED"; }
            try { bool ok = f(); if (ok) _d7_last_ok = true; _logger.Info($"build 7 diagnostics: hook {name} {(ok ? "queued" : "REFUSED")}"); return name + (ok ? " queued" : " REFUSED"); }
            catch (Exception e) { _logger.Error($"build 7 diagnostics: hook {name}: {e}"); return name + " FAILED (" + e.GetType().Name + ")"; }
        }
        // The join is queued last of these four: Fahrenheit applies queued hooks in order, and the join is the one the main thread
        // calls from the first frame on. (Since LD-7 the load hooks follow it in the queue, so it is no longer our last insertion
        // in commit(); since the loads build the resize hook is queued whatever diag_build7 says, ahead of the join and the load
        // hooks: a commit failure there would drop those too, which leaves the lock release unarmed, the safe direction. The
        // status text and core.log show it.)
        string a = "target pointer off (cursor_source=legacy and diag_build7=0 at launch)", b = "turn pointer off (the same)", c = "job join not hooked";
        if (want_cur) {
            _d7_last_ok = false; a = one("target pointer 0x49C870", _a7_tgt, () => new FhCallHandle<d7_void>(new FhCallLocation((void*)_a7_tgt)).hook(this, _h7_tgt!)); _h7_tgt_ok = _d7_last_ok;
            _d7_last_ok = false; b = one("turn pointer 0x492590", _a7_turn, () => new FhCallHandle<d7_turn>(new FhCallLocation((void*)_a7_turn)).hook(this, _h7_turn!)); _h7_turn_ok = _d7_last_ok;
        }
        string d = one("backbuffer resize 0x199210", _a7_rsz, () => new FhCallHandle<d7_resize>(new FhCallLocation((void*)_a7_rsz)).hook(this, _h7_rsz!));
        if (want_join) { _d7_last_ok = false; c = one("job join 0x644B30", _a7_join, () => new FhCallHandle<d7_void>(new FhCallLocation((void*)_a7_join)).hook(this, _h7_join!)); _h7_join_ok = _d7_last_ok; }   // the smooth anchor needs it; queued last
        _d7_status = $"{(_diag7 ? "logging on" : "logging off (diag_build7=0)")}; hooks: {a}, {b}, {d}, {c}. \"queued\" is not \"installed\": Fahrenheit applies them after init (a MinHook failure is in core.log; a success is logged only at Debug level). The proof is each hook's \"called for the first time\" line in ffxvr_diag.log with diag_build7=1 (the target and turn pointers only in a battle)";
    }

    private static T? d7_chain<T>(nint addr, T h) where T : Delegate => new FhCallHandle<T>(new FhCallLocation((void*)addr)).chain_from(h).fnptr;
    /// <summary>B7-1, render thread: all four originals, once, at the top of render_imgui_inner (before try_install_hooks).</summary>
    private void diag7_resolve() {
        if (_d7_resolved_rt) return;
        _d7_resolved_rt = true;
        try {
            if (_h7_tgt  != null && _o7_tgt  == null) System.Threading.Volatile.Write(ref _o7_tgt,  d7_chain(_a7_tgt,  _h7_tgt));
            if (_h7_turn != null && _o7_turn == null) System.Threading.Volatile.Write(ref _o7_turn, d7_chain(_a7_turn, _h7_turn));
            if (_h7_rsz  != null && _o7_rsz  == null) System.Threading.Volatile.Write(ref _o7_rsz,  d7_chain(_a7_rsz,  _h7_rsz));
            if (_h7_join != null && _o7_join == null) System.Threading.Volatile.Write(ref _o7_join, d7_chain(_a7_join, _h7_join));
        }
        catch (Exception e) { _logger.Error($"build 7 diagnostics: resolving the originals: {e}"); }   // the detours then resolve on their own thread
    }
    /// <summary>B7-1, a detour's own thread, only when the render thread has not resolved it yet (before the first Present).
    /// It must return the original: it waits out our render-thread hook insertions, and retries a miss (Fahrenheit writes the
    /// chain entry just after enabling the hook) or an exception from a concurrent table write, until it succeeds.</summary>
    // Residual risk (build7-review.md, "Re-review of 684f482"): Fahrenheit's get_fnptr takes its table lock (_lock_chains)
    // and calls Marshal.GetDelegateForFunctionPointer with no try/finally around the Set(). If a torn unlocked _chain read
    // (only possible while a FOREIGN late hook insertion writes the table) hands it a 0 pointer, the throw leaves that lock
    // held for good, and the next retry here (and every later Fahrenheit resolution or insertion) blocks forever. So "retry
    // on exception" only covers throws outside that locked region. The same risk applies to the D3D detours' resolution.
    // Fahrenheit alpha12 (#366) locks readers and releases on a throw (a `lock` statement), so this risk is gone there; the
    // handshake stays until the port has been soak-tested (design-notes, Fahrenheit port).
    private T d7_resolve_main<T>(ref T? slot, nint addr, T h) where T : Delegate {
        for (int spin = 0; ; spin++) {
            for (int w = 0; System.Threading.Volatile.Read(ref _d7_inserting) != 0; w++) System.Threading.Thread.Sleep(w < 50 ? 0 : 1);
            System.Threading.Interlocked.Increment(ref _d7_resolving);   // full fence: pairs with install_draw_detours' Exchange
            T? r = null;
            if (System.Threading.Volatile.Read(ref _d7_inserting) == 0) { try { r = d7_chain(addr, h); } catch { } }
            System.Threading.Interlocked.Decrement(ref _d7_resolving);
            if (r != null) { System.Threading.Volatile.Write(ref slot, r); return r; }
            System.Threading.Thread.Sleep(spin < 50 ? 0 : 1);
        }
    }
    /// <summary>Called by install_draw_detours around its late hook insertions (render thread).</summary>
    private void diag7_insert_begin() { System.Threading.Interlocked.Exchange(ref _d7_inserting, 1); for (int w = 0; System.Threading.Volatile.Read(ref _d7_resolving) != 0; w++) System.Threading.Thread.Sleep(w < 50 ? 0 : 1); }
    private void diag7_insert_end() { System.Threading.Volatile.Write(ref _d7_inserting, 0); }

    // -- the detours (main thread). The original always runs, first; recording faults are counted, never thrown. ------
    // Cursor stage C2: the two cursor detours skip the game's draw while native says the headset shows our markers
    // (ffxvr_cursor_skip: decided at the frame boundary, published interlocked; latched per game frame by c2_skip_now, so
    // both detours of one game frame agree). Both functions only draw (exe-verify), so
    // skipping loses nothing else. The snapshot is recorded either way; it is what our markers are built from.
    private void h7_tgt() {
        d7_void o = System.Threading.Volatile.Read(ref _o7_tgt) ?? d7_resolve_main(ref _o7_tgt, _a7_tgt, _h7_tgt!);
        bool skip = c2_skip_now();
        if (!skip) o();
        _d7_tgt_calls_total++;   // C2-6: counted whatever the recording state (the dead-hook checks read it)
        if (_diag7 || _cursor_game_cfg) { try { d7_tgt_record(); } catch { _d7_main_faults++; } }
    }
    private void h7_turn(int chr) {
        d7_turn o = System.Threading.Volatile.Read(ref _o7_turn) ?? d7_resolve_main(ref _o7_turn, _a7_turn, _h7_turn!);
        bool skip = c2_skip_now();
        if (!skip) o(chr);
        _d7_turn_calls_total++;   // C2-6
        if (_diag7 || _cursor_game_cfg) { try { d7_turn_record(chr, skip); } catch { _d7_main_faults++; } }
    }
    private void h7_join() {
        d7_void o = System.Threading.Volatile.Read(ref _o7_join) ?? d7_resolve_main(ref _o7_join, _a7_join, _h7_join!);
        o();
        if (_sr_stall_req) { _sr_stall_req = false; System.Threading.Thread.Sleep(3000); }   // stickrep Debug: "stall the main thread 3 s" (the keepalive test; main thread, after the original)
        sg_join_install();   // the shadow pool guard, once, on the main thread between frames (shadow_cam.cs)
        dmg3d_join();        // 3D damage numbers: the nodes the battle UI just drew (dmg3d.cs; one bool read while off)
        if (_diag7 || _smooth_anchor) { try { d7_join_record(); } catch { _d7_main_faults++; } }   // the smooth anchor needs the snapshot too
    }
    private void h7_rsz(nint self, int w, int h) {
        d7_resize o = System.Threading.Volatile.Read(ref _o7_rsz) ?? d7_resolve_main(ref _o7_rsz, _a7_rsz, _h7_rsz!);
        // Build 9 (R11b): a hard invalidation signal next to native's destruction sentinel, whatever diag_build7's logging state
        // (interlocked stores only): "begins" before the call, "done" after it; native acts on completed re-acquires only (B9-2).
        // With diag_build7=0 at launch this hook does not exist and the sentinel alone does the job.
        var bbr = _fn_bb_resized; if (bbr != null) bbr(w, h, 0);
        o(self, w, h);
        if (bbr != null) bbr(w, h, 1);
        if (_diag7) {
            _d7_rsz_w = w; _d7_rsz_h = h; _d7_rsz_frame = FhUtil.get_at<uint>(0x1FCBBF0); _d7_rsz_main = _d7_join_mtid != 0 && Environment.CurrentManagedThreadId == _d7_join_mtid;
            System.Threading.Volatile.Write(ref _d7_rsz_calls, _d7_rsz_calls + 1);
        }
    }

    /// <summary>One battle combatant's cursor anchor as the game computes it (0x8354F0 with its fallbacks), plus the roots
    /// for the stage-2 correction. force_idx = the sub-position to use (the turn pointer always uses 1), -1 = Chr+0xDF9.
    /// False when the Chr has no actor (the game then anchors at 0x794C20; skipped and counted) or a read fails.</summary>
    private bool d7_anchor(nint chrs, int id, int force_idx, ref D7Anchor t) {
        t = default; t.id = id; t.idx = -1; t.fallback = -1;
        if (chrs == 0 || id < 0 || id >= 0x1F || _fn_safe_read == null) return false;
        byte* chr = (byte*)chrs + id * 0xF90;
        if (!read(chr, out nint actor) || actor == 0) return false;
        read(chr + 0x10, out byte c10); read(chr + 0xDF9, out sbyte ce); read(chr + 0xF34, out int sx); read(chr + 0xF38, out int sy);
        t.chr10 = c10; t.sx = sx; t.sy = sy; t.idx = force_idx >= 0 ? force_idx : ce;
        byte* a = stackalloc byte[0x6C4];   // actor +0x00..+0x6C4: through optpos[25]
        if (_fn_safe_read((void*)actor, a, 0x6C4) == 0) return false;
        Vector4 v = default;
        if (t.idx >= 0 && t.idx <= 25) v = *(Vector4*)(a + 0x524 + 16 * t.idx); else t.flags |= 4;
        Vector4 o0 = *(Vector4*)(a + 0x524);
        if (v.X != 0f || v.Y != 0f || v.Z != 0f || v.W != 0f) { t.fallback = 0; t.opt = new Vector3(v.X, v.Y, v.Z); }
        else if (o0.W != 0f) { t.fallback = 1; t.opt = new Vector3(o0.X, o0.Y, o0.Z); }
        else { t.fallback = 2; t.opt = *(Vector3*)(a + 0x200); }
        t.tl = *(Vector3*)(a + 0x0C) + *(Vector3*)(a + 0x6C);   // logic root: chr_pos_vec + chr_offset_vec (what 0x832760 builds +0x1D0 from)
        t.td = *(Vector3*)(a + 0x200);                          // drawn root: translation of the +0x1D0 matrix
        t.heading = *(float*)(a + 0x158); t.actor = actor;   // C3: compared with the actor first person looks out of
        if (*(uint*)(a + 0x19C) != 0) t.flags |= 1;
        if (*(uint*)(a + 0x10C) != 0) t.flags |= 2;
        return true;
    }

    private void d7_tgt_record() {
        if (FhUtil.get_at<uint>(0xF3D6E0) == 1) { _d7_tgt_calls_auto++; return; }   // auto-battle: the original returned at once and drew nothing
        uint stamp = FhUtil.get_at<uint>(0x1FCBBF0);   // the game frame counter (not yet incremented for this invocation)
        if (stamp == _d7_tgt_last_stamp) { if (++_d7_tgt_calls_frame > _d7_tgt_calls_max) _d7_tgt_calls_max = _d7_tgt_calls_frame; return; }   // once per frame
        _d7_tgt_last_stamp = stamp; _d7_tgt_calls_frame = 1; if (_d7_tgt_calls_max < 1) _d7_tgt_calls_max = 1;
        int s = (_d7_seq + 1) & 1;                     // the slot the reader is not on
        ref D7TgtHdr h = ref _d7h[s];
        h.stamp = stamp; h.mask = FhUtil.get_at<uint>(0xF3D178) & 0x0FFFFFFF;   // the highlighted candidates: exactly what the game draws
        h.cmd = FhUtil.get_at<uint>(0x1FCC08C); h.gate = FhUtil.get_at<uint>(0x858EF8); h.autob = FhUtil.get_at<uint>(0xF3D6E0); h.alive = FhUtil.get_at<uint>(0xF3C948);
        h.bstate = FhUtil.get_at<sbyte>(0xD2A8E0);
        nint chrs = FhUtil.get_at<nint>(0xD334D4);
        int n = 0, skipped = 0;
        for (int i = 0; i < 28; i++) {
            if ((h.mask & (1u << i)) == 0) continue;
            if (d7_anchor(chrs, i, -1, ref _d7t[s * 28 + n])) n++; else skipped++;
        }
        h.n = n; h.skipped = skipped;
        // the windows whose draw callback is the target-pointer stub, and their lifecycle state (+0x01)
        uint stub = (uint)((nint)FhUtil.ptr_at<byte>(0) + k7_stub); h.stubs = 0; h.stub_states = 0;
        for (int w = 0; w < 8; w++) {
            if (FhUtil.get_at<uint>(0xF3C950 + w * 0xF0 + 0x90) != stub) continue;
            h.stub_states |= (uint)(FhUtil.get_at<byte>(0xF3C950 + w * 0xF0 + 0x01) & 0xF) << (4 * h.stubs); h.stubs++;
        }
        System.Threading.Volatile.Write(ref _d7_seq, _d7_seq + 1);
    }

    private void d7_turn_record(int chr, bool skipped) {
        int s = (_d7u_seq + 1) & 1;
        ref D7Turn u = ref _d7u[s];
        u.stamp = FhUtil.get_at<uint>(0x1FCBBF0); u.chr = chr; u.chr_glob = FhUtil.get_at<short>(0x1FCC088);
        u.theta = FhUtil.get_at<float>(0xF3FA7C); u.apex = FhUtil.get_at<uint>(0xF3FA88); u.bas = FhUtil.get_at<uint>(0xF3F7D4);
        u.ok = d7_anchor(FhUtil.get_at<nint>(0xD334D4), chr, 1, ref u.a) ? 1 : 0;   // 0x7950B0: optpos[1] of the chr's actor, then y -= 2
        u.game_a = FhUtil.get_at<Vector3>(0xF3FA68);   // what 0x7950B0 just wrote for this draw (stale if the chr has no actor, or if C2 skipped the draw)
        u.skipped = skipped ? 1 : 0;
        System.Threading.Volatile.Write(ref _d7u_seq, _d7u_seq + 1);
    }

    private void d7_join_record() {
        _d7_join_calls_total++;
        if (_fn_main_note == null || _fn_main_note() == 0) return;   // the main thread's joins only (the first caller names it, for the watchdog)
        if (_d7_join_mtid == 0) _d7_join_mtid = Environment.CurrentManagedThreadId;
        uint stamp = FhUtil.get_at<uint>(0x1FCBBF0);
        if (stamp == _d7_join_last_stamp) return;   // the first join after the counter moved (0x821617) is Sg_MainLoop's post-join call (0x821656)
        _d7_join_last_stamp = stamp; _d7_join_post_total++;
        int s = (_d7j_seq + 1) & 1;
        ref D7Join j = ref _d7j[s];
        j.stamp = stamp; j.exec = FhUtil.get_at<byte>(0x8496D4); j.valid = 0; j.head_ok = 0; j.flags = 0;   // exec byte: VA 0xC496D4 (FFX-60fps sigs.cpp; read at 0x82D7F4)
        nint act = System.Threading.Volatile.Read(ref _d7_follow_actor); j.actor = act;
        if (act != 0) {
            byte* b = stackalloc byte[0x6C4];   // through optpos[25] (+0x524 + 26 * 16)
            byte* q = stackalloc byte[0x20C];   // riding: the parent through its drawn root (+0x200..+0x208)
            j.pvalid = 0; j.parent = 0;
            if (_fn_safe_read((void*)act, b, 0x6C4) != 0) {
                j.pos = *(Vector3*)(b + 0x0C); j.off = *(Vector3*)(b + 0x6C); j.td = *(Vector3*)(b + 0x200); j.valid = 1;
                if (*(uint*)(b + 0x19C) != 0) {   // attached: the root comes from the parent's bone (riding: the chocobo; ride.cs)
                    j.flags |= 1; j.parent = *(nint*)(b + 0x19C);
                    if (_fn_safe_read((void*)j.parent, q, 0x20C) != 0) { j.ppos = *(Vector3*)(q + 0x0C); j.poff = *(Vector3*)(q + 0x6C); j.ptd = *(Vector3*)(q + 0x200); j.pvalid = 1; }
                }
                if (*(uint*)(b + 0x10C) != 0) j.flags |= 2;   // an override matrix replaces the root
                // the head as push_player picks it: the shared rule (FphWalk.HeadPick) with push_player's reach for this frame
                // (published before _d7_follow_actor; sa_apply puts this head in the view on almost every frame, review of 9b406b9 B);
                // folded with the actor position if it ever looks relative
                int hk = FphWalk.HeadPick(new ReadOnlySpan<Vector4>(b + 0x524, 26), System.Threading.Volatile.Read(ref _fp_reach_live), out _);
                if (hk >= 0) { Vector4 sp = *(Vector4*)(b + 0x524 + 16 * hk); j.head = new Vector3(sp.X, sp.Y, sp.Z); j.head_ok = 1; }
                if (j.head_ok != 0 && (j.flags & 1) == 0 && MathF.Abs(j.head.X - j.pos.X) + MathF.Abs(j.head.Z - j.pos.Z) > 100f) { j.head += j.pos; j.flags |= 4; }   // never for an attached actor (world already); flag 4: folded (sa_apply undoes it for a displaced body, eye review SF1)
            }
        }
        System.Threading.Volatile.Write(ref _d7j_seq, _d7j_seq + 1);
    }

    // -- render thread -------------------------------------------------------------------------------------------------
    private bool d7_read_tgt() {
        for (int attempt = 0; attempt < 2; attempt++) {
            int s1 = System.Threading.Volatile.Read(ref _d7_seq); if (s1 == 0) return false;
            int sl = s1 & 1; D7TgtHdr h = _d7h[sl]; int n = Math.Clamp(h.n, 0, 28);
            for (int i = 0; i < n; i++) _r7t_tmp[i] = _d7t[sl * 28 + i];
            System.Threading.Interlocked.MemoryBarrier();
            if (System.Threading.Volatile.Read(ref _d7_seq) != s1) continue;   // the writer came round to this slot: again
            _r7h = h; _r7h.n = n; Array.Copy(_r7t_tmp, _r7t, n); _r7_seq = s1; return true;
        }
        return false;
    }
    private static bool d7_read<T>(T[] slots, ref int seq, ref T into, ref int seen) where T : struct {
        for (int attempt = 0; attempt < 2; attempt++) {
            int s1 = System.Threading.Volatile.Read(ref seq); if (s1 == 0) return false;
            T v = slots[s1 & 1];
            System.Threading.Interlocked.MemoryBarrier();
            if (System.Threading.Volatile.Read(ref seq) != s1) continue;
            into = v; seen = s1; return true;
        }
        return false;
    }
    /// <summary>A game-space point through the game's own camera (the CB-captured view and projection, never the override)
    /// into UI units, the 1920x1080 centred space of Chr+0xF34/F38.</summary>
    private static bool d7_project(float* c, Vector3 g, out float ux, out float uy) {
        float x = -g.X, y = -g.Y, z = g.Z;   // game (PS2) space -> render space
        float e0 = c[0] * x + c[1] * y + c[2] * z + c[3], e1 = c[4] * x + c[5] * y + c[6] * z + c[7], e2 = c[8] * x + c[9] * y + c[10] * z + c[11];
        float* P = c + 16;
        float c0 = P[0] * e0 + P[1] * e1 + P[2] * e2 + P[3], c1 = P[4] * e0 + P[5] * e1 + P[6] * e2 + P[7], c3 = P[12] * e0 + P[13] * e1 + P[14] * e2 + P[15];
        ux = uy = 0f;
        if (c3 <= 1e-4f) return false;
        ux = c0 / c3 * 960f; uy = -c1 / c3 * 540f;
        return true;
    }
    private void d7_log(string line) {
        if (_fn_diag_line == null) return;
        int n = Math.Min(line.Length, 4000); byte* b = stackalloc byte[n + 1];
        for (int i = 0; i < n; i++) { char ch = line[i]; b[i] = ch >= 32 && ch < 127 ? (byte)ch : (byte)'?'; }
        b[n] = 0; _fn_diag_line(b);
    }
    private static int d7_bucket(int lag) => lag < 0 ? 0 : lag >= 6 ? 7 : lag + 1;   // [<0] [0] [1] .. [5] [6+]
    private static string d7_hist(int[] h) => I($"<0:{h[0]} 0:{h[1]} 1:{h[2]} 2:{h[3]} 3:{h[4]} 4:{h[5]} 5:{h[6]} 6+:{h[7]}");
    private static Vector3 d7_render(Vector3 g) => new Vector3(-g.X, -g.Y, g.Z);

    /// <summary>Once per frame, before begin_frame. Keeps the native side's switch in step with the ini (an edit while the
    /// game runs turns the logging on or off; the game hooks stay as they were installed at launch).</summary>
    private void diag7_present() {
        int want = _diag7 ? 1 : 0;
        if (want != _d7_native_on && _fn_diag7_set != null) { _fn_diag7_set(want); _d7_native_on = want; }
        if (!_diag7 || _fn_diag_line == null || _fn_safe_read == null) return;
        try { diag7_present_inner(); }
        catch (Exception e) { if (_d7_faults_logged++ < 3) _logger.Error($"build 7 diagnostics: {e}"); }
    }

    private void diag7_present_inner() {
        double now = ImGui.GetTime();
        uint frame = FhUtil.get_at<uint>(0x1FCBBF0);
        if (!_d7_hello) { _d7_hello = true; d7_log("build 7 diagnostics: " + _d7_status); _d7_hist_t = now; _d7_frame0 = frame; }
        // 6.12: each hook's first call proves it is in (MinHook applied it, nothing else displaced it)
        void first(ref int seen, int total, string name) { if (seen == 0 && total > 0) { seen = 1; d7_log(I($"build 7: {name} hook called for the first time (game frame {frame})")); } }
        first(ref _d7_seen_tgt, _d7_tgt_calls_total, "target pointer (0x49C870)"); first(ref _d7_seen_turn, _d7_turn_calls_total, "turn pointer (0x492590)");
        first(ref _d7_seen_join, _d7_join_calls_total, "job join (0x644B30)");     first(ref _d7_seen_rsz, _d7_rsz_calls, "backbuffer resize (0x199210)");

        sbyte bst = FhUtil.get_at<sbyte>(0xD2A8E0);
        if (bst != 0 && !_d7_in_btl) { _d7_btl_elems = false; _d7_btl_frame0 = frame; Array.Clear(_d7_hd_seen); Array.Clear(_d7_hd_delta); if (_fn_diag7_set != null) _fn_diag7_set(3); }   // a battle starts: re-arm the per-battle rumble log (6.11)
        _d7_in_btl = bst != 0;

        bool got_t = d7_read_tgt(), got_u = d7_read(_d7u, ref _d7u_seq, ref _r7u, ref _r7u_seq), got_j = d7_read(_d7j, ref _d7j_seq, ref _r7j, ref _r7j_seq);
        _r7_since_t = got_t && _r7_seq != _r7_seq_prev ? 0 : Math.Min(_r7_since_t + 1, 999); _r7_seq_prev = _r7_seq;
        _r7_since_u = got_u && _r7u_seq != _r7u_seq_prev ? 0 : Math.Min(_r7_since_u + 1, 999); _r7u_seq_prev = _r7u_seq;
        _r7_since_j = got_j && _r7j_seq != _r7j_seq_prev ? 0 : Math.Min(_r7_since_j + 1, 999); _r7j_seq_prev = _r7j_seq;
        int lag_t = _r7_seq != 0 ? (int)(frame - _r7h.stamp) : 999, lag_u = _r7u_seq != 0 ? (int)(frame - _r7u.stamp) : 999, lag_j = _r7j_seq != 0 ? (int)(frame - _r7j.stamp) : 999;

        // 6.2: how far ahead of the image the main thread is, as a 30 s histogram
        _d7_presents++;
        if (_r7_since_t == 0) _d7_hist_c[d7_bucket(lag_t)]++;   // only Presents that brought a new snapshot (B7-3): a stale one's lag only grows
        if (_r7_since_u == 0) _d7_hist_u[d7_bucket(lag_u)]++;
        if (_r7_since_j == 0) _d7_hist_j[d7_bucket(lag_j)]++;
        if (now - _d7_hist_t >= 30.0) {
            d7_log(I($"timing (30 s): presents {_d7_presents}, game frames advanced {frame - _d7_frame0}, post-join snapshots {_d7_join_post_total - _d7_j_post0} (join calls {_d7_join_calls_total - _d7_j_calls0}), target-pointer calls {_d7_tgt_calls_total - _d7_t_calls0} (max {_d7_tgt_calls_max} in one frame; under auto-battle so far {_d7_tgt_calls_auto}), turn-pointer calls {_d7_turn_calls_total - _d7_u_calls0}, main-thread record faults {_d7_main_faults} | lag in game frames at Present, target snapshot: {d7_hist(_d7_hist_c)} | turn snapshot: {d7_hist(_d7_hist_u)} | post-join snapshot: {d7_hist(_d7_hist_j)} (Presents with a new snapshot only)"));
            _d7_hist_t = now; _d7_presents = 0; _d7_frame0 = frame; _d7_j_post0 = _d7_join_post_total; _d7_j_calls0 = _d7_join_calls_total; _d7_t_calls0 = _d7_tgt_calls_total; _d7_u_calls0 = _d7_turn_calls_total; _d7_tgt_calls_max = 0;
            Array.Clear(_d7_hist_c); Array.Clear(_d7_hist_u); Array.Clear(_d7_hist_j);
        }

        float* cam = stackalloc float[48];
        bool cam_ok = _fn_get_camera != null && _fn_get_camera(cam) != 0;
        bool act_t = _r7_since_t <= 3, act_u = _r7_since_u <= 3;   // a snapshot this frame or the last few: the game drew that cursor

        // 6.3: per id, the heading change between its last two logic values (the rotation-error estimate's input)
        if (act_t) for (int i = 0; i < _r7h.n; i++) { D7Anchor t = _r7t[i]; int id = t.id & 31; if (!_d7_hd_seen[id]) { _d7_hd_seen[id] = true; _d7_hd_last[id] = t.heading; _d7_hd_stamp[id] = _r7h.stamp; continue; } float dh = t.heading - _d7_hd_last[id]; if (dh != 0f && float.IsFinite(dh) && MathF.Abs(dh) < 100f) { while (dh > MathF.PI) dh -= 2f * MathF.PI; while (dh < -MathF.PI) dh += 2f * MathF.PI; _d7_hd_delta[id] = MathF.Abs(dh); _d7_hd_last[id] = t.heading; _d7_hd_stamp[id] = _r7h.stamp; } else if (dh == 0f && (int)(_r7h.stamp - _d7_hd_stamp[id]) >= 2) _d7_hd_delta[id] = 0f; }   // B7-4: no turn for a whole logic tick -> no rotation error

        // 6.6: every combatant's cursor element, once per battle (does any boss use a sub-position other than 1?)
        if (act_t && !_d7_btl_elems) {
            _d7_btl_elems = true; nint chrs = FhUtil.get_at<nint>(0xD334D4); var sb = new System.Text.StringBuilder("cursor elements (Chr+0xDF9, for Chr+0x10 != 0):");
            for (int id = 0; id < 0x1C && chrs != 0; id++) { byte* c = (byte*)chrs + id * 0xF90; if (read(c + 0x10, out byte c10) && c10 != 0 && read(c + 0xDF9, out sbyte ce)) sb.Append(I($" {id:X2}:{ce}")); }
            if (_r7u_seq != 0 && (int)(_r7u.stamp - _d7_btl_frame0) >= 0) sb.Append(I($" | turn pointer colours apex 0x{_r7u.apex:X8} base 0x{_r7u.bas:X8} (RGBA bytes, 0x80 = full; turn snapshot of game frame {_r7u.stamp})"));
            else sb.Append(" | turn pointer colours: no turn snapshot in this battle yet (they are on every turn: line)");
            d7_log(sb.ToString());
        }

        // 6.1 / 6.3 / 6.6: a cursor: line on every change and once a second while a cursor is drawn (at most 8 a second)
        uint key_mask = act_t ? _r7h.mask : 0; int key_turn = act_u ? _r7u.chr : -1; bool active = act_t || act_u;
        bool change = key_mask != _d7_key_mask || key_turn != _d7_key_turn || active != _d7_key_active;
        if (now - _d7_sec_t >= 1.0) { _d7_sec_t = now; _d7_lines_sec = 0; }
        if ((change || (active && now - _d7_line_t >= 1.0)) && _d7_lines_sec < 8) {
            _d7_lines_sec++; _d7_line_t = now; _d7_key_mask = key_mask; _d7_key_turn = key_turn; _d7_key_active = active;
            uint cmd_now = FhUtil.get_at<uint>(0x1FCC08C);   // [0x23CC08C]: party members in command input, read at Present
            var sb = new System.Text.StringBuilder(I($"cursor: {(change ? "change" : "1 s")} frame {frame}, cmd input now 0x{cmd_now:X} | "));
            if (!active) sb.Append(I($"none drawn (target snapshot {_r7_since_t} Presents old, lag {lag_t}; turn {_r7_since_u} Presents old, lag {lag_u})"));
            if (act_t) {
                sb.Append(I($"targets: stamp {_r7h.stamp} lag {lag_t} mask 0x{_r7h.mask:X7} n {_r7h.n} no-actor {_r7h.skipped} | gates cmd 0x{_r7h.cmd:X} 0xC58EF8={_r7h.gate} auto={_r7h.autob} ui={_r7h.alive} battle_state={_r7h.bstate} | target windows {_r7h.stubs} (states"));
                for (int k = 0; k < _r7h.stubs && k < 8; k++) sb.Append(I($" {(_r7h.stub_states >> (4 * k)) & 0xF}"));
                sb.Append(I($"), calls this frame max {_d7_tgt_calls_max}"));
                for (int i = 0; i < _r7h.n; i++) {
                    D7Anchor t = _r7t[i]; Vector3 cor = t.td - t.tl; float hd = MathF.Sqrt((t.opt.X - t.tl.X) * (t.opt.X - t.tl.X) + (t.opt.Z - t.tl.Z) * (t.opt.Z - t.tl.Z));
                    sb.Append(I($" || id 0x{t.id:X2} {(t.id < 0x14 ? "party" : "mon")} idx {t.idx} fb {t.fallback} opt ({t.opt.X:F2} {t.opt.Y:F2} {t.opt.Z:F2}) Tl ({t.tl.X:F2} {t.tl.Y:F2} {t.tl.Z:F2}) Td ({t.td.X:F2} {t.td.Y:F2} {t.td.Z:F2}) corr {cor.Length():F3} rot-err {hd * _d7_hd_delta[t.id & 31] * 0.5f:F3} att {t.flags & 1} ovr {(t.flags >> 1) & 1}{((t.flags & 4) != 0 ? " BAD-IDX" : "")} game scr ({t.sx} {t.sy})"));
                    if (cam_ok && d7_project(cam, t.opt, out float ux, out float uy)) sb.Append(I($" ours-game ({ux - t.sx:F1} {uy - t.sy:F1}) px"));
                    else sb.Append(cam_ok ? " (behind the game camera)" : " (no camera)");
                }
            }
            if (act_u) {
                D7Anchor a = _r7u.a; Vector3 A = a.opt - new Vector3(0f, 2f, 0f);   // game y points down: 2 units up
                sb.Append(I($" | turn: chr {_r7u.chr} (0x1FCC088 = {_r7u.chr_glob}{(_r7u.chr != _r7u.chr_glob ? " MISMATCH" : "")}) stamp {_r7u.stamp} lag {lag_u} theta {_r7u.theta:F3} apex 0x{_r7u.apex:X8} base 0x{_r7u.bas:X8}"));
                if (_r7u.ok != 0) {
                    Vector3 ga = _r7u.game_a;
                    sb.Append(I($" ours A ({A.X:F2} {A.Y:F2} {A.Z:F2}) game A ({ga.X:F2} {ga.Y:F2} {ga.Z:F2}){(_r7u.skipped != 0 ? " (game draw skipped by C2: stale)" : "")} ours-game {(A - ga).Length():F3} fb {a.fallback} corr {(a.td - a.tl).Length():F3} att {a.flags & 1} ovr {(a.flags >> 1) & 1}"));
                    if (cam_ok && d7_project(cam, A, out float ux, out float uy)) sb.Append(I($" ours ({ux:F1} {uy:F1})"));
                }
                else sb.Append(" (no actor)");
            }
            d7_log(sb.ToString());
        }

        // 6.1 visual: the game's anchors as dots on the monitor (Debug > Build 7; never in the headset)
        if (_d7_dots && !_xr_on && cam_ok) {
            Vector2 ds = ImGui.GetIO().DisplaySize; ImDrawListPtr dl = ImGui.GetForegroundDrawList();
            Vector2 px(float x, float y) => new Vector2((x + 960f) / 1920f * ds.X, (y + 540f) / 1080f * ds.Y);   // UI units -> monitor pixels
            if (act_t) for (int i = 0; i < _r7h.n; i++) {
                D7Anchor t = _r7t[i];
                dl.AddCircle(px(t.sx, t.sy), 9f, 0xFF00FFFFu, 0, 2f);   // yellow ring: where the game placed its anchor (Chr+0xF34/F38)
                if (d7_project(cam, t.opt, out float ux, out float uy)) dl.AddCircleFilled(px(ux, uy), 5f, 0xFFFF00FFu);   // magenta: the anchor through the game camera
            }
            if (act_u && _r7u.ok != 0 && d7_project(cam, _r7u.game_a, out float tx, out float ty)) dl.AddCircleFilled(px(tx, ty), 5f, 0xFFFFFF00u);   // cyan: the turn triangle's anchor A, the game's own (0xF3FA68)
        }

        // 6.4: the Motion log's extra columns are pushed by push_player now (sa_motion_cols, with the smooth anchor's columns).

        // 6.8: the game re-acquiring its backbuffer (the backbuffer: line that follows is the native identification)
        int rc = System.Threading.Volatile.Read(ref _d7_rsz_calls);
        if (rc != _d7_rsz_seen) { _d7_rsz_seen = rc; d7_log(I($"resize (0x599210 ResizeBuffers + GetBuffer): {_d7_rsz_w}x{_d7_rsz_h} at game frame {_d7_rsz_frame} (now {frame}), call #{rc}, on the main thread {(_d7_rsz_main ? "yes" : "no/unknown")}")); }

        // 6.9: pan-and-scan maps, on every map change (always) and every 5 s (at most 300 of those a session). The map name is
        // compared as its 16 raw bytes, so no string is built per frame (B7-15).
        ulong m0 = FhUtil.get_at<ulong>(0x1FCBC60), m1 = FhUtil.get_at<ulong>(0x1FCBC68);
        bool map_changed = m0 != _d7_ps_m0 || m1 != _d7_ps_m1;
        if (map_changed || (now - _d7_ps_t >= 5.0 && _d7_ps_lines < 300)) {
            string map = map_name();
            _d7_ps_m0 = m0; _d7_ps_m1 = m1; _d7_ps_t = now; if (!map_changed) _d7_ps_lines++;
            byte ps = FhUtil.get_at<byte>(0x8CB9C0); nint r = FhUtil.get_at<nint>(0x8CB9D8);
            float crop = float.NaN; int mapid = -1; if (r != 0) { read((byte*)r + 0xF9BC, out crop); read((byte*)r + 0xFAF4, out mapid); }
            int view = _fn_xr_screen_now != null ? _fn_xr_screen_now() : -1;
            string cams = cam_ok ? I($"P[0] {cam[16]:F4} P[5] {cam[21]:F4} P[2] {cam[18]:F4} P[6] {cam[22]:F4}") : "no camera";
            d7_log(I($"pan-scan: map '{map}' 0x8CB9C0={ps} (0 = 4:3 pan-and-scan) r+0xF9BC {crop:F3} r+0xFAF4 {mapid} | game {cams} (as uploaded) | view {(view == 0 ? "3D" : view == 1 ? "virtual screen" : view == 2 ? "stereo screen" : "?")}{(_xr_on ? " (headset)" : " (monitor)")}"));
        }
    }

    /// <summary>Developer > Logging & status > Build 7 diagnostics: the status and the anchor-dot toggle.</summary>
    private void diag7_ui() {
        if (!ImGui.TreeNode("Build 7 diagnostics (log only)")) return;
        ImGui.TextWrapped($"{(_diag7 ? "on" : "off")}: {esc(_d7_status)}");
        ImGui.Text($"calls: target pointer {_d7_tgt_calls_total}, turn pointer {_d7_turn_calls_total}, job join {_d7_join_calls_total} (post-join {_d7_join_post_total}), resize {_d7_rsz_calls}; record faults {_d7_main_faults}");
        ImGui.Checkbox("show cursor anchors (monitor)", ref _d7_dots);
        ImGui.SameLine(); ImGui.TextDisabled("magenta: the game's anchor through the game camera; yellow ring: where the game put it; cyan: the turn triangle's anchor");
        ImGui.TextDisabled("Lines in ffxvr_diag.log (diag_build7=1): cursor:, timing:, pan-scan:, stall, xinput set:, backbuffer:, resize. The logging and the stall watchdog follow the ini at once; the game-function hooks follow cursor_source / smooth_anchor (or diag_build7) at launch.");
        ImGui.TreePop();
    }
}
