// SPDX-License-Identifier: MIT

namespace FFXVR;

// Battle load diagnostics + model cache option (design-notes "Battle load diagnostics + model cache option"; research
// battle-load.md 2.1 and battle-data-load.md section 7; exe-verify-round2 Part C, whose corrections are applied; the
// review build-load-review.md checked the nine prologues, conventions and threads against the disassembly).
//
// diag_load (default 1; hooks at the next launch): per battle, the phases of the way in and the way back, timed with QPC,
// one summary line each in ffxvr_diag.log ("load: battle N ..."), with the chr cache's entries in use, the game pool's
// reserved / committed counters and the address space (total free, largest free block, largest free block below 2 GB).
// battle_cache_models (default 0; applies at once): skip the chr model/motion cache flush (0x840030, called in normal play
// only from the map free 0x909EA0 at 0x909F93) when that map free is part of a RANDOM battle's own map load (battle_state 9:
// the state-8 code writes 9 before it calls the map load, LD-1) or of the reload of the same field map on its way back, AND the
// cache holds at most k_ld_max_in used entries at the way in (room for the battle's own models, LD-2) / k_ld_max_out on the
// way back, AND the largest free block below 2 GB is at least k_ld_min_low_mb. Otherwise the game's own flush runs, at that same call site:
// the decision is taken only there (exe-verify-round2 C1 B6-B8, battle-data-load.md 7): the chr buffers come from the main
// game pool, which never evicts, and the table-full eviction 0x8409A0 leaks the HD character, so the cache must never be
// allowed to fill; and only at a map free are live actors' entries known to be locked. A zone change, a script battle, a
// save load or the title never match and always flush.
//
// Hooks (FhUtil offsets; every original always runs, except the flush when the option skips it; recording never throws):
//   0x381590 battle teardown        cdecl void()            (main)   the way back starts (only when battle_state != 0)
//   0x509A20 map load               cdecl void(kind,map,p3) (main)   map request: decides the flush skip for the nested free
//   0x509EA0 map free               cdecl void()            (main)
//   0x508000 map build              cdecl void()            (main)   the read is done; queues the HD scene and TexList jobs
//   0x440030 chr cache flush        cdecl void()            (main)   skipped only under battle_cache_models (see above)
//   0x36C490 loader wait-all        cdecl int(int mode)     (main)   mode != 1 blocks until the loader thread is idle; ~25 callers
//                                                                    ([0x2310C48] calls, the chr wrapper 0x82A5E0): each wait logs
//                                                                    its caller from a stack walk (ffxvr_game_callers)
//   0x429EF0 chr model load         cdecl void(int)         (main)
//   0x42A4C0 chr motion load        cdecl void(uint,int)    (main)
//   0x29B940 loadPCluster           thiscall PCluster*(this, path)   (loader thread mostly; chained with Fahrenheit's hook)
// NOT hooked: MsBattleExe 0x381020 and the return 0x461160. FFX-60fps's QOL-RETRY row sig-scans both prologues (a MinHook
// jump there would make it refuse, or collide with its own patch). The battle start is seen in the wait detour instead:
// MsBattleExe(mode 0) calls the loader's wait-all (0x781068) BEFORE it clears the request byte [0x112A8E2] (1 = random
// encounter, 2 = script battle request 0x781C90) at 0x78107E. Sg_MainLoop's entry is never touched.
public sealed unsafe partial class FFXVRModule {

    private bool _diag_load = true;            // ini diag_load (hooks at the next launch)
    private bool _battle_cache_models = false; // ini battle_cache_models (option 1: keep chr models across random battles in one zone)
    private string _ld_status = "not set up";
    private bool _ld_queued, _ld_hooked, _ld_any;   // all nine queued / at least one (the summary lines run with what is there, LD-8)
    private const int k_ld_max_in = 60, k_ld_max_out = 80, k_ld_min_low_mb = 256;   // the option's own bound: used chr entries (of 100) at the way in (the battle then adds its own) / the way back; largest free block below 2 GB
    private int _ld_seen_mask;                 // LD-8 (Interlocked.Or / Volatile.Read): bit per hook, set at its first call (1 teardown, 2 map load, 4 map free, 8 build, 16 flush, 32 wait, 64 model, 128 motion, 256 loadPCluster)
    private bool _ld_seen_logged; private bool _ld_resolved_rt;
    private int _ld_fill_peak = -1, _ld_fill_live = -1, _ld_fill_tear = -1, _ld_fill_tick; private bool _ld_table_full;   // LD-2: the table in battle (render thread)
    private int _ld_va_low_mb = -1;            // the largest free block below 2 GB at the last battle start / end (render thread; -1 = not measured: no skip)
    private string _ld_va_in = "", _ld_va_out = "";
    private delegate* unmanaged[Cdecl]<uint*, int, int> _fn_game_callers;   // the stack's return addresses inside FFX.exe, as Ghidra VAs
    private delegate* unmanaged[Cdecl]<uint*, void> _fn_va_stats;           // MB: total free, largest free, largest free below 2 GB, free below 2 GB

    private const int kl_teardown = 0x381590, kl_mapload = 0x509A20, kl_mapfree = 0x509EA0, kl_build = 0x508000, kl_flush = 0x440030;
    private const int kl_wait = 0x36C490, kl_model = 0x429EF0, kl_motion = 0x42A4C0, kl_pcl = 0x29B940;
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]    private delegate int dl_v();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]    private delegate int dl_i(int a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]    private delegate int dl_ii(int a, int b);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]    private delegate int dl_iii(int a, int b, int c);
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] private delegate nint dl_pcl(nint self, nint path);
    // Every detour returns the original's eax (int / pointer), so a function Ghidra calls void loses nothing either way.
    private dl_v?   _hl_teardown, _ol_teardown, _hl_mapfree, _ol_mapfree, _hl_build, _ol_build, _hl_flush, _ol_flush;
    private dl_i?   _hl_wait, _ol_wait, _hl_model, _ol_model;
    private dl_ii?  _hl_motion, _ol_motion;
    private dl_iii? _hl_mapload, _ol_mapload;
    private dl_pcl? _hl_pcl, _ol_pcl;
    private nint _al_teardown, _al_mapload, _al_mapfree, _al_build, _al_flush, _al_wait, _al_model, _al_motion, _al_pcl;

    // -- running totals (Interlocked: 64-bit values written by the main and loader threads, read at Present) --------------
    private enum LT { pcl_n, pcl_t, pcl_main_n, pcl_fast_n, model_n, model_t, motion_n, motion_t, wait_n, wait_t, free_n, free_t,
                      flush_n, flush_t, skip_n, build_n, build_t, teardown_t, N }
    private readonly long[] _lt = new long[(int)LT.N];
    private readonly long[] _lt_base_in = new long[(int)LT.N], _lt_base_out = new long[(int)LT.N];   // taken on the main thread at the battle start / teardown
    private void lt_add(LT k, long v) => System.Threading.Interlocked.Add(ref _lt[(int)k], v);
    private long lt(LT k) => System.Threading.Interlocked.Read(ref _lt[(int)k]);
    private void lt_snap(long[] into) { for (int i = 0; i < (int)LT.N; i++) into[i] = System.Threading.Interlocked.Read(ref _lt[i]); }
    private long d_in(LT k) => lt(k) - _lt_base_in[(int)k];
    private long d_out(LT k) => lt(k) - _lt_base_out[(int)k];

    // -- main-thread events (one writer; the render thread reads them once the phase is over) --------------------------------
    private int  _ld_main_tid, _ld_depth_map, _ld_depth_free, _ld_depth_teardown, _ld_faults;
    private volatile int _ld_start_seq, _ld_tear_seq;   // published after the times below (main thread)
    // main-thread times (LD-6: written and read with Volatile, atomic for long on 32-bit)
    private long _ld_t_start, _ld_t_start_wait, _ld_t_wait_end, _ld_t_tear, _ld_t_mapreq, _ld_t_retreq, _ld_t_retbuild, _ld_t_free_in, _ld_t_free_out;
    private volatile bool _ld_in_start_wait;   // LD-4: MsBattleExe's wait is under way
    private int  _ld_kind;                     // 1 random encounter, 2 script battle request, 0 unknown (MsBattleExe without the wait); main thread only (LD-3)
    private int  _ld_kind_tear = -1;           // LD-3: the teardown count when _ld_kind was set: the kind belongs to the battle that follows it, no other
    private int  _ld_field_at_start = -1;      // the field map id when the battle's own map load ran
    private bool _ld_cur_entry;                // the map load under way is a battle's own (battle_state 8): its free / flush belong to the way in
    private bool _ld_ret_pending, _ld_skip_now, _ld_flush_skipped_in, _ld_flush_skipped_out, _ld_would_skip_in, _ld_would_skip_out;
    private string _ld_refused_in = "", _ld_refused_out = "";   // why the option let the flush run although the map load matched
    private int  _ld_fill_in0 = -1, _ld_fill_in1 = -1, _ld_fill_out0 = -1, _ld_fill_out1 = -1, _ld_locked;
    // the blocking waits: a small log of (context, ticks), indexed by a running count
    private readonly int[] _ld_wctx = new int[16]; private readonly long[] _ld_wtick = new long[16]; private readonly uint[] _ld_wcall = new uint[32]; private volatile int _ld_wn;
    private int _ld_wn_in, _ld_wn_out;

    // -- render thread ----------------------------------------------------------------------------------------------------
    private int  _ld_phase;                    // 0 idle, 1 way in (until battle_state 1), 2 battle, 3 way back
    private int  _ld_seen_start, _ld_seen_tear, _ld_battle_n, _ld_idle_frames, _ld_countdown;
    private long _ld_t_live, _ld_rstart; private int _ld_rkind, _ld_logged_tear;   // the render thread's copy of the kind (LD-3); the teardown already logged (LD-6)
    private readonly long[] _ld_t_state = new long[64];
    private string _ld_field_name = ""; private int _ld_field_id;
    private bool _ld_checked_waitptr;
    private static double ms(long ticks) => ticks * 1000.0 / System.Diagnostics.Stopwatch.Frequency;
    private static long vr(ref long x) => System.Threading.Volatile.Read(ref x);
    private static void vw(ref long x, long v) => System.Threading.Volatile.Write(ref x, v);
    private void seen(int bit) { if ((System.Threading.Volatile.Read(ref _ld_seen_mask) & bit) == 0) System.Threading.Interlocked.Or(ref _ld_seen_mask, bit); }
    private int seen_mask => System.Threading.Volatile.Read(ref _ld_seen_mask);
    private static long now() => System.Diagnostics.Stopwatch.GetTimestamp();

    /// <summary>At init: from init, after diag7_init (so AFTER build 7's job join, which the smooth anchor needs: Fahrenheit's
    /// commit stops at the first failing insertion, and a failure among these nine must not take the join with it, LD-7).</summary>
    private void ld_init() {
        if (_ld_queued) return;
        _ld_queued = true;
        if (!_diag_load) { _ld_status = "off (ffxvr.ini diag_load=0): no load-timing hook; battle_cache_models needs them too"; _logger.Info("load diagnostics: " + _ld_status); return; }
        if (!_native_ok || _fn_safe_read == null || _fn_diag_line == null) { _ld_status = "off (the native helper did not load)"; _logger.Info("load diagnostics: " + _ld_status); return; }
        _hl_teardown = hl_teardown; _hl_mapload = hl_mapload; _hl_mapfree = hl_mapfree; _hl_build = hl_build; _hl_flush = hl_flush;
        _hl_wait = hl_wait; _hl_model = hl_model; _hl_motion = hl_motion; _hl_pcl = hl_pcl;
        new FhCallLocation("FFX.exe", kl_teardown).try_resolve(out _al_teardown); new FhCallLocation("FFX.exe", kl_mapload).try_resolve(out _al_mapload);
        new FhCallLocation("FFX.exe", kl_mapfree).try_resolve(out _al_mapfree);   new FhCallLocation("FFX.exe", kl_build).try_resolve(out _al_build);
        new FhCallLocation("FFX.exe", kl_flush).try_resolve(out _al_flush);       new FhCallLocation("FFX.exe", kl_wait).try_resolve(out _al_wait);
        new FhCallLocation("FFX.exe", kl_model).try_resolve(out _al_model);       new FhCallLocation("FFX.exe", kl_motion).try_resolve(out _al_motion);
        new FhCallLocation("FFX.exe", kl_pcl).try_resolve(out _al_pcl);
        int ok = 0, all = 0;
        string one(string name, nint addr, Func<bool> f) {
            all++;
            if (addr == 0) { _logger.Error($"load diagnostics: hook {name}: FFX.exe not found"); return name + " NOT RESOLVED"; }
            try { bool r = f(); if (r) ok++; _logger.Info($"load diagnostics: hook {name} {(r ? "queued" : "REFUSED")}"); return name + (r ? " queued" : " REFUSED"); }
            catch (Exception e) { _logger.Error($"load diagnostics: hook {name}: {e}"); return name + " FAILED (" + e.GetType().Name + ")"; }
        }
        string s = string.Join(", ", new[] {
            one("teardown 0x781590",  _al_teardown, () => new FhCallHandle<dl_v>(new FhCallLocation((void*)_al_teardown)).hook(this, _hl_teardown!)),
            one("map load 0x909A20",  _al_mapload,  () => new FhCallHandle<dl_iii>(new FhCallLocation((void*)_al_mapload)).hook(this, _hl_mapload!)),
            one("map free 0x909EA0",  _al_mapfree,  () => new FhCallHandle<dl_v>(new FhCallLocation((void*)_al_mapfree)).hook(this, _hl_mapfree!)),
            one("map build 0x908000", _al_build,    () => new FhCallHandle<dl_v>(new FhCallLocation((void*)_al_build)).hook(this, _hl_build!)),
            one("chr flush 0x840030", _al_flush,    () => new FhCallHandle<dl_v>(new FhCallLocation((void*)_al_flush)).hook(this, _hl_flush!)),
            one("wait-all 0x76C490",  _al_wait,     () => new FhCallHandle<dl_i>(new FhCallLocation((void*)_al_wait)).hook(this, _hl_wait!)),
            one("chr model 0x829EF0", _al_model,    () => new FhCallHandle<dl_i>(new FhCallLocation((void*)_al_model)).hook(this, _hl_model!)),
            one("chr motion 0x82A4C0", _al_motion,  () => new FhCallHandle<dl_ii>(new FhCallLocation((void*)_al_motion)).hook(this, _hl_motion!)),
            one("loadPCluster 0x69B940", _al_pcl,   () => new FhCallHandle<dl_pcl>(new FhCallLocation((void*)_al_pcl)).hook(this, _hl_pcl!)),
        });
        _ld_hooked = ok == all; _ld_any = ok > 0;
        _ld_status = (_ld_hooked ? "queued" : "INCOMPLETE (battle_cache_models stays off)") + $": {s}";
    }

    /// <summary>Render thread, before try_install_hooks (with diag7_resolve): every original, once (B7-1's pattern).</summary>
    private void ld_resolve() {
        if (!_ld_queued || _ld_resolved_rt) return;
        _ld_resolved_rt = true;   // once, like diag7_resolve (LD-13); a miss resolves on its own thread
        void one<T>(ref T? slot, nint addr, T? h) where T : Delegate {
            if (h == null || slot != null) return;
            try { T? r = d7_chain(addr, h); if (r != null) System.Threading.Volatile.Write(ref slot, r); } catch (Exception e) { _logger.Error($"load diagnostics: resolving an original: {e.Message}"); }
        }
        one(ref _ol_teardown, _al_teardown, _hl_teardown); one(ref _ol_mapload, _al_mapload, _hl_mapload); one(ref _ol_mapfree, _al_mapfree, _hl_mapfree);
        one(ref _ol_build, _al_build, _hl_build);          one(ref _ol_flush, _al_flush, _hl_flush);          one(ref _ol_wait, _al_wait, _hl_wait);
        one(ref _ol_model, _al_model, _hl_model);          one(ref _ol_motion, _al_motion, _hl_motion);       one(ref _ol_pcl, _al_pcl, _hl_pcl);
    }

    // -- the detours ----------------------------------------------------------------------------------------------------------
    private int hl_teardown() {
        dl_v o = System.Threading.Volatile.Read(ref _ol_teardown) ?? d7_resolve_main(ref _ol_teardown, _al_teardown, _hl_teardown!);
        bool end = false; long t0 = now();
        try { seen(1); _ld_main_tid = Environment.CurrentManagedThreadId; end = FhUtil.get_at<sbyte>(0xD2A8E0) != 0; if (end) ld_mark_teardown(t0); } catch { _ld_faults++; }   // state 0: it returns at once (also its call inside MsBattleExe)
        _ld_depth_teardown++;
        int r;
        try { r = o(); } finally { _ld_depth_teardown--; }
        if (end) lt_add(LT.teardown_t, now() - t0);
        return r;
    }
    private int hl_mapload(int kind, int map, int p3) {
        dl_iii o = System.Threading.Volatile.Read(ref _ol_mapload) ?? d7_resolve_main(ref _ol_mapload, _al_mapload, _hl_mapload!);
        try { seen(2); _ld_main_tid = Environment.CurrentManagedThreadId; ld_map_request(kind, map); } catch { _ld_faults++; _ld_skip_now = false; }
        _ld_depth_map++;
        int r;
        try { r = o(kind, map, p3); } finally { _ld_depth_map--; _ld_skip_now = false; }
        return r;
    }
    private int hl_mapfree() {
        dl_v o = System.Threading.Volatile.Read(ref _ol_mapfree) ?? d7_resolve_main(ref _ol_mapfree, _al_mapfree, _hl_mapfree!);
        long t0 = now(); bool mine = _ld_depth_map > 0;
        try { seen(4); if (mine) { int fill = ld_cache_fill(out _ld_locked); if (_ld_cur_entry) _ld_fill_in0 = fill; else _ld_fill_out0 = fill; } } catch { _ld_faults++; }
        _ld_depth_free++;
        int r;
        try { r = o(); } finally { _ld_depth_free--; }
        long dt = now() - t0; lt_add(LT.free_n, 1); lt_add(LT.free_t, dt);
        if (mine) { try { int fill = ld_cache_fill(out _ld_locked); if (_ld_cur_entry) { vw(ref _ld_t_free_in, dt); _ld_fill_in1 = fill; } else { vw(ref _ld_t_free_out, dt); _ld_fill_out1 = fill; } } catch { _ld_faults++; } }
        return r;
    }
    private int hl_build() {
        dl_v o = System.Threading.Volatile.Read(ref _ol_build) ?? d7_resolve_main(ref _ol_build, _al_build, _hl_build!);
        long t0 = now();
        try { seen(8); ld_mark_build(t0); } catch { _ld_faults++; }
        int r = o();
        lt_add(LT.build_n, 1); lt_add(LT.build_t, now() - t0);
        return r;
    }
    private int hl_flush() {
        dl_v o = System.Threading.Volatile.Read(ref _ol_flush) ?? d7_resolve_main(ref _ol_flush, _al_flush, _hl_flush!);
        seen(16);
        // Option 1: only the flush inside OUR map free inside OUR map load, decided at that map load (ld_map_request); the map load
        // and map free hooks must have been seen working (LD-8: "queued" is not "installed")
        if (_ld_skip_now && _battle_cache_models && _ld_hooked && (seen_mask & 6) == 6 && _ld_depth_map > 0 && _ld_depth_free > 0) {
            int fill = -1; try { fill = ld_cache_fill(out _); } catch { _ld_faults++; }
            int low = _ld_va_low_mb, max = _ld_cur_entry ? k_ld_max_in : k_ld_max_out;
            string why = fill < 0 ? "cache unreadable" : fill > max ? $"{fill} entries > {max}" : low < 0 ? "address space not measured" : low < k_ld_min_low_mb ? $"largest free block below 2 GB {low} MB < {k_ld_min_low_mb}" : "";
            if (why.Length == 0) {
                lt_add(LT.skip_n, 1);
                if (_ld_cur_entry) { _ld_flush_skipped_in = true; _ld_ret_pending = true; } else _ld_flush_skipped_out = true;   // LD-R1
                return 0;
            }
            if (_ld_cur_entry) _ld_refused_in = why; else _ld_refused_out = why;   // the game's flush runs below: the bound is kept
        }
        long t0 = now();
        int r = o();
        lt_add(LT.flush_n, 1); lt_add(LT.flush_t, now() - t0);
        return r;
    }
    private int hl_wait(int mode) {
        dl_i o = System.Threading.Volatile.Read(ref _ol_wait) ?? d7_resolve_main(ref _ol_wait, _al_wait, _hl_wait!);
        if (mode == 1) return o(mode);   // the poll form (0x76C490 with 1 only checks the queue): not a wait
        int ctx = 0; long t0 = now(); uint c0 = 0, c1 = 0;
        try {
            seen(32); _ld_main_tid = Environment.CurrentManagedThreadId; ctx = ld_wait_begin(t0);
            var gc = _fn_game_callers; if (gc != null) { uint* cs = stackalloc uint[2]; int n = gc(cs, 2); if (n > 0) c0 = cs[0]; if (n > 1) c1 = cs[1]; }   // the caller (and its caller), as Ghidra VAs; 0 = the walk did not reach FFX.exe
        } catch { _ld_faults++; }
        if (ctx == 1) _ld_in_start_wait = true;   // LD-4: the render thread's "ended before live" timer waits for this
        int r;
        try { r = o(mode); } finally { if (ctx == 1) { vw(ref _ld_t_wait_end, now()); _ld_in_start_wait = false; } }
        try { long dt = now() - t0; lt_add(LT.wait_n, 1); lt_add(LT.wait_t, dt); int i = _ld_wn; _ld_wctx[i & 15] = ctx; _ld_wtick[i & 15] = dt; _ld_wcall[(i & 15) * 2] = c0; _ld_wcall[(i & 15) * 2 + 1] = c1; _ld_wn = i + 1; if (ctx == 1) vw(ref _ld_t_start_wait, dt); } catch { _ld_faults++; }
        return r;
    }
    private int hl_model(int id) {
        dl_i o = System.Threading.Volatile.Read(ref _ol_model) ?? d7_resolve_main(ref _ol_model, _al_model, _hl_model!);
        seen(64); long t0 = now(); int r = o(id);
        lt_add(LT.model_n, 1); lt_add(LT.model_t, now() - t0);
        return r;
    }
    private int hl_motion(int a, int b) {
        dl_ii o = System.Threading.Volatile.Read(ref _ol_motion) ?? d7_resolve_main(ref _ol_motion, _al_motion, _hl_motion!);
        seen(128); long t0 = now(); int r = o(a, b);
        lt_add(LT.motion_n, 1); lt_add(LT.motion_t, now() - t0);
        return r;
    }
    private nint hl_pcl(nint self, nint path) {
        dl_pcl o = System.Threading.Volatile.Read(ref _ol_pcl) ?? d7_resolve_main(ref _ol_pcl, _al_pcl, _hl_pcl!);
        seen(256); long t0 = now(); nint r = o(self, path);
        long dt = now() - t0;
        lt_add(LT.pcl_n, 1); lt_add(LT.pcl_t, dt);
        if (Environment.CurrentManagedThreadId == _ld_main_tid) lt_add(LT.pcl_main_n, 1);
        if (dt * 10000 < System.Diagnostics.Stopwatch.Frequency) lt_add(LT.pcl_fast_n, 1);   // < 0.1 ms: a ClusterManager hit (no I/O)
        return r;
    }

    // -- main-thread helpers --------------------------------------------------------------------------------------------------
    /// <summary>The blocking wait's context: 1 = MsBattleExe's (battle_state 0 and a pending request: the battle starts here),
    /// 2 = the teardown's, 3 = inside a map load, else 10 + battle_state.</summary>
    private int ld_wait_begin(long t0) {
        sbyte bst = FhUtil.get_at<sbyte>(0xD2A8E0);
        if (_ld_depth_teardown > 0) return 2;
        if (_ld_depth_map > 0) return 3;
        byte req = FhUtil.get_at<byte>(0xD2A8E2);
        if (bst == 0 && (req == 1 || req == 2)) {   // 0x781068: the request byte is cleared only after this wait (0x78107E)
            _ld_kind = req; _ld_kind_tear = _ld_tear_seq;   // LD-3: valid for this battle only (until its teardown)
            vw(ref _ld_t_start, t0); vw(ref _ld_t_start_wait, 0); vw(ref _ld_t_wait_end, 0);
            ld_reset_in(); _ld_ret_pending = false; _ld_field_at_start = -1;
            lt_snap(_lt_base_in); _ld_wn_in = _ld_wn;
            _ld_start_seq++;
            return 1;
        }
        return 10 + bst;
    }
    /// <summary>LD-5: the way-in values (main thread): at a battle start seen in the wait, at the battle's own map request, and at
    /// the teardown (for the next battle, whose start may not be seen in the wait).</summary>
    private void ld_reset_in() {
        _ld_flush_skipped_in = _ld_would_skip_in = false; _ld_refused_in = ""; _ld_fill_in0 = _ld_fill_in1 = -1; vw(ref _ld_t_free_in, 0); vw(ref _ld_t_mapreq, 0);
    }
    private void ld_mark_teardown(long t0) {
        _ld_kind = 0;   // LD-3: the battle is over; the next one needs its own start
        vw(ref _ld_t_tear, t0); vw(ref _ld_t_retreq, 0); vw(ref _ld_t_retbuild, 0);
        _ld_flush_skipped_out = _ld_would_skip_out = false; _ld_refused_out = ""; _ld_fill_out0 = _ld_fill_out1 = -1; vw(ref _ld_t_free_out, 0);   // LD-5
        ld_reset_in();
        lt_snap(_lt_base_out); _ld_wn_out = _ld_wn;
        _ld_tear_seq++;
    }
    /// <summary>A map load request: times it, and decides whether option 1 may skip the chr flush in its map free. Only:
    /// (a) the battle's own map load (battle_state 9 at this call, LD-1; kind 0x13 / 0x1A as 0x908600 / 0x9083A0 pass; a RANDOM
    /// encounter started since the last teardown, LD-3; the map asked for = the battle map), or (b) on the way back (battle_state
    /// 0), the first map load after that battle's teardown, for exactly the field map that was current at (a). Anything else (an
    /// area change, a script battle, a save load, the title) flushes as the game does, and ends the pending return.</summary>
    private void ld_map_request(int kind, int map) {
        long t = now(); sbyte bst = FhUtil.get_at<sbyte>(0xD2A8E0); _ld_cur_entry = bst == 9;
        int field = FhUtil.get_at<short>(0xD2CA94);              // the current field map: word [MsGetSaveEventAddress() + 4] (0x785240 returns 0x112CA90); MAPREG passes the same word
        int btlmap = FhUtil.get_at<short>(0xD2C254) & 0x3FF;     // Btl.battlefield_id; the state-8 code loads (id & 0x3FF)
        bool skip = false;
        if (bst == 9) {
            ld_reset_in(); vw(ref _ld_t_mapreq, t);
            if (_ld_kind == 1 && _ld_kind_tear == _ld_tear_seq && (kind == 0x13 || kind == 0x1A) && map == btlmap) { _ld_field_at_start = field; _ld_ret_pending = false; skip = true; _ld_would_skip_in = true; }   // LD-R1: the way back is armed only if the entry skip really happens (hl_flush)
            else _ld_ret_pending = false;
        }
        else if (bst == 0 && _ld_tear_seq != 0 && vr(ref _ld_t_retreq) == 0 && vr(ref _ld_t_tear) != 0) {   // the first map load after the teardown: the way back
            vw(ref _ld_t_retreq, t);
            if (_ld_ret_pending && map == _ld_field_at_start) { skip = true; _ld_would_skip_out = true; }
            _ld_ret_pending = false;
        }
        else _ld_ret_pending = false;
        _ld_skip_now = skip;
    }
    private void ld_mark_build(long t0) {
        if (_ld_depth_map > 0) return;
        if (FhUtil.get_at<sbyte>(0xD2A8E0) == 0 && vr(ref _ld_t_retreq) != 0 && vr(ref _ld_t_retbuild) == 0) vw(ref _ld_t_retbuild, t0);
    }
    /// <summary>The chr cache: occupied entries of the 100-entry pointer table (0x1302F98, entry +0xC buffer), and the locked ones (+5).</summary>
    private int ld_cache_fill(out int locked) {
        int n = 0; locked = 0;
        for (int i = 0; i < 100; i++) {
            nint e = FhUtil.get_at<nint>(0xF02F98 + 4 * i);
            if (e == 0 || !read((byte*)e + 0x0C, out uint buf) || buf == 0) continue;
            n++; if (read((byte*)e + 5, out byte lk) && lk != 0) locked++;
        }
        return n;
    }

    // -- render thread: phases and the summary lines -------------------------------------------------------------------------
    private void ld_present() {
        if (!_ld_any || _fn_diag_line == null) return;   // LD-8: with whatever hooks are in
        try { ld_present_inner(); } catch (Exception e) { if (_ld_faults++ < 3) _logger.Error($"load diagnostics: {e}"); }
    }
    private void ld_present_inner() {
        if (!_ld_checked_waitptr) {   // the loader's function table at 0x2310C40: +8 must be the wait we hooked (MsBattleExe calls [0x2310C48])
            nint fp = FhUtil.get_at<nint>(0x1F10C48);
            if (fp != 0) { _ld_checked_waitptr = true; d7_log(I($"load diagnostics: loader wait-all pointer [0x2310C48] = 0x{(long)fp:X} (hooked 0x{(long)_al_wait:X}): {(fp == _al_wait ? "match" : "MISMATCH: the battle-start and teardown waits will not be seen")}")); }
        }
        long t = now(), f = System.Diagnostics.Stopwatch.Frequency;
        sbyte bst = FhUtil.get_at<sbyte>(0xD2A8E0);
        int ss = _ld_start_seq, ts = _ld_tear_seq;
        if (_ld_phase == 0 || _ld_phase == 3) {
            if (ss != _ld_seen_start || (_ld_phase == 0 && bst > 1 && bst < 0x17)) {   // a battle starts (seen in the wait, or by its state if no wait was seen)
                if (_ld_phase == 3) ld_log_out(t, "(the next battle started)");
                bool by_wait = ss != _ld_seen_start; _ld_seen_start = ss;
                _ld_rkind = by_wait ? _ld_kind : 0;   // LD-3: the render thread keeps its own copy and never writes the main thread's
                if (!by_wait) { _ld_rstart = t; lt_snap(_lt_base_in); _ld_wn_in = _ld_wn; } else _ld_rstart = vr(ref _ld_t_start);
                _ld_phase = 1; _ld_battle_n++; Array.Clear(_ld_t_state); _ld_countdown = 0;
                _ld_fill_peak = _ld_fill_live = _ld_fill_tear = -1; _ld_table_full = false; _ld_fill_tick = 0;
                _ld_field_name = map_name(); _ld_field_id = FhUtil.get_at<short>(0xD2CA94); _ld_va_in = ld_memory();
            }
        }
        if (_ld_phase == 1 || _ld_phase == 2) {   // LD-2: the table during the battle (sampled every 30 Presents, and at live / teardown)
            if (++_ld_fill_tick >= 30) { _ld_fill_tick = 0; ld_sample_fill(); }
        }
        if (_ld_phase == 1) {
            if (bst >= 0 && bst < 64 && _ld_t_state[bst] == 0) _ld_t_state[bst] = t;
            if (FhUtil.get_at<byte>(0xD2A918) != 0) _ld_countdown = Math.Max(_ld_countdown, FhUtil.get_at<byte>(0xD2A919));
            long we = vr(ref _ld_t_wait_end), since = Math.Max(_ld_rstart, we);
            if (bst == 1) { _ld_t_live = t; _ld_fill_live = ld_sample_fill(); ld_log_in(); _ld_phase = 2; _ld_seen_tear = ts; }
            else if (bst == 0 && !_ld_in_start_wait && t - since > f) { ld_log_in(); _ld_phase = 0; }   // ended before it went live (LD-4: never during the start wait, and counted from its end)
        }
        else if (_ld_phase == 2) {
            if (ts != _ld_seen_tear) { _ld_seen_tear = ts; _ld_phase = 3; _ld_idle_frames = 0; _ld_fill_tear = ld_sample_fill(); _ld_va_out = ld_memory(); }
            else if (bst == 0 && t - _ld_t_live > 2 * f) _ld_phase = 0;   // over without a teardown seen (no way-back line)
        }
        else if (_ld_phase == 3) {
            // back on the field: the field build ran (or no reload came within 3 s) and the loader thread has been idle for 10 Presents
            long tear = vr(ref _ld_t_tear);
            bool built = vr(ref _ld_t_retbuild) != 0 || (vr(ref _ld_t_retreq) == 0 && t - tear > 3 * f);
            _ld_idle_frames = built && bst == 0 && ld_loader_idle() ? _ld_idle_frames + 1 : 0;
            if (_ld_idle_frames >= 10) { ld_log_out(t, ""); _ld_phase = 0; }
            else if (t - tear > 60 * f) { ld_log_out(t, "(timed out after 60 s)"); _ld_phase = 0; }
        }
    }
    /// <summary>LD-2: the chr table's used entries now; keeps the peak and flags a full table (the leaking 0x8409A0 eviction runs then).</summary>
    private int ld_sample_fill() {
        int n = ld_cache_fill(out _);
        if (n > _ld_fill_peak) _ld_fill_peak = n;
        if (n >= 100 && !_ld_table_full) { _ld_table_full = true; d7_log(I($"load: battle {_ld_battle_n}: chr cache TABLE FULL (100 entries in use; battle_cache_models {(_battle_cache_models ? "on" : "off")})")); }
        return n;
    }
    /// <summary>Address space and the game pool, once per battle start and end (render thread): sets _ld_va_low_mb for the option.</summary>
    private string ld_memory() {
        uint* v = stackalloc uint[4]; v[0] = v[1] = v[2] = v[3] = 0;
        var vs = _fn_va_stats; if (vs != null) { vs(v); _ld_va_low_mb = (int)v[2]; }
        uint res = FhUtil.get_at<uint>(0x153CD44), com = FhUtil.get_at<uint>(0x153CD48);   // the game pool's running totals (Fahrenheit's FhMallocModule)
        return I($"VA free {v[0]} MB (largest {v[1]} MB; below 2 GB {v[3]} MB, largest {v[2]} MB) | game pool reserved {res >> 20} MB committed {com >> 20} MB");
    }
    /// <summary>AsyncLoadManager [0xCCC88C]: both task-list counts (+0x20, +0x2C) zero.</summary>
    private bool ld_loader_idle() {
        nint m = FhUtil.get_at<nint>(0x8CC88C);
        return m != 0 && read((byte*)m + 0x20, out int a) && read((byte*)m + 0x2C, out int b) && a == 0 && b == 0;
    }
    private string ld_waits(int from) {
        int to = _ld_wn; if (to - from > 16) from = to - 16;
        var sb = new System.Text.StringBuilder();
        for (int i = from; i < to && i - from < 6; i++) {
            int c = _ld_wctx[i & 15];
            string name = c == 1 ? "MsBattleExe" : c == 2 ? "teardown" : c == 3 ? "map load" : I($"state {c - 10}");
            uint c0 = _ld_wcall[(i & 15) * 2], c1 = _ld_wcall[(i & 15) * 2 + 1];
            sb.Append(sb.Length == 0 ? "" : ", ").Append(I($"{name} {ms(_ld_wtick[i & 15]):F0} (from 0x{c0:X}{(c1 != 0 ? I($" < 0x{c1:X}") : "")})"));
        }
        if (to - from > 6) sb.Append(I($", +{to - from - 6} more"));
        return sb.ToString();
    }
    private long tst(int s) => s >= 0 && s < 64 ? _ld_t_state[s] : 0;
    private void ld_log_in() {
        if (!_ld_seen_logged) { _ld_seen_logged = true; d7_log(I($"load diagnostics: hooks seen working so far 0x{seen_mask:X} of 0x1FF (1 teardown, 2 map load, 4 map free, 8 build, 16 flush, 32 wait, 64 model, 128 motion, 256 loadPCluster)")); }
        long t0 = _ld_rstart, live = _ld_t_live != 0 && _ld_phase == 1 && FhUtil.get_at<sbyte>(0xD2A8E0) == 1 ? _ld_t_live : now();
        long mq = vr(ref _ld_t_mapreq), req = mq != 0 && mq > t0 ? mq : 0, s10 = tst(10);
        double total = ms(live - t0), a = req != 0 ? ms(req - t0) : s10 != 0 ? ms(s10 - t0) : 0, b = req != 0 ? ms(vr(ref _ld_t_free_in)) : 0;
        double c = req != 0 && s10 != 0 ? ms(s10 - req) - b : 0, d = s10 != 0 ? ms(live - s10) : 0;
        string kind = _ld_rkind == 1 ? "random" : _ld_rkind == 2 ? "script" : "start not seen in the wait";
        string flush = _ld_flush_skipped_in ? "SKIPPED (battle_cache_models)" : _ld_refused_in.Length != 0 ? $"ran: {_ld_refused_in}" : _ld_would_skip_in ? "ran (skippable)" : req != 0 ? "ran" : "no map load";
        d7_log(I($"load: battle {_ld_battle_n} ({kind}) in {_ld_field_name} (field map {_ld_field_id}, battle map {FhUtil.get_at<short>(0xD2C254) & 0x3FF}): total {total:F0} ms = to the map load {a:F0} (start wait {ms(vr(ref _ld_t_start_wait)):F0}) + field free {b:F0} (chr flush {flush}) + map load {c:F0} + models {d:F0} ({d_in(LT.model_n)} model / {d_in(LT.motion_n)} motion loads, {ms(d_in(LT.model_t) + d_in(LT.motion_t)):F0} ms on the main thread)")
             + I($" | HD clusters {ms(d_in(LT.pcl_t)):F0} ms ({d_in(LT.pcl_n)}, {d_in(LT.pcl_main_n)} on the main thread, {d_in(LT.pcl_fast_n)} hits < 0.1 ms) | blocking waits {ms(d_in(LT.wait_t)):F0} ms ({d_in(LT.wait_n)}: {ld_waits(_ld_wn_in)})")
             + I($" | states: 4 {st_ms(4, t0)} 5 {st_ms(5, t0)} 7 {st_ms(7, t0)} 9 {st_ms(9, t0)} 0x22 {st_ms(0x22, t0)} 10 {st_ms(10, t0)} live {ms(live - t0):F0}{(_ld_countdown > 0 ? I($" | countdown 0x112A919 up to {_ld_countdown} ticks") : "")} | chr cache {fill(_ld_fill_in0)}->{fill(_ld_fill_in1)} entries in use at the field free ({_ld_locked} locked; of 100), {fill(_ld_fill_live)} at live | battle_cache_models {(_battle_cache_models ? "on" : "off")} | at the start: {_ld_va_in}"));
    }
    private string st_ms(int s, long t0) => tst(s) != 0 ? I($"+{ms(tst(s) - t0):F0}") : "-";
    private static string fill(int n) => n < 0 ? "?" : n.ToString(System.Globalization.CultureInfo.InvariantCulture);
    private void ld_log_out(long t, string note) {
        int ts = _ld_tear_seq; long t0 = vr(ref _ld_t_tear); if (t0 == 0 || ts == _ld_logged_tear) return;
        _ld_logged_tear = ts;   // LD-6: the render thread keeps its own "logged" mark instead of zeroing a main-thread field
        long req = vr(ref _ld_t_retreq), bld = vr(ref _ld_t_retbuild);
        double total = ms(t - t0), a = req != 0 ? ms(req - t0) : 0, b = req != 0 ? ms(vr(ref _ld_t_free_out)) : 0, c = req != 0 ? ms(t - req) - b : 0;
        string flush = req == 0 ? "no field reload (battle map = field map?)" : _ld_flush_skipped_out ? "SKIPPED (battle_cache_models)" : _ld_refused_out.Length != 0 ? $"ran: {_ld_refused_out}" : _ld_would_skip_out ? "ran (skippable)" : "ran";
        d7_log(I($"load: battle {_ld_battle_n} way back: total {total:F0} ms = teardown {ms(d_out(LT.teardown_t)):F0} + to the field reload {a:F0} + battle-map free {b:F0} (chr flush {flush}) + reload to idle {c:F0} (build at {(bld != 0 ? I($"+{ms(bld - t0):F0}") : "-")}; {d_out(LT.model_n)} model / {d_out(LT.motion_n)} motion loads)")
             + I($" | HD clusters {ms(d_out(LT.pcl_t)):F0} ms ({d_out(LT.pcl_n)}, {d_out(LT.pcl_fast_n)} hits) | blocking waits {ms(d_out(LT.wait_t)):F0} ms ({d_out(LT.wait_n)}: {ld_waits(_ld_wn_out)}) | chr cache: peak in battle {fill(_ld_fill_peak)}{(_ld_table_full ? " (TABLE FULL seen)" : "")}, {fill(_ld_fill_tear)} at the teardown, {fill(_ld_fill_out0)}->{fill(_ld_fill_out1)} at the battle-map free | at the end: {_ld_va_out} {note}"));
    }
}
