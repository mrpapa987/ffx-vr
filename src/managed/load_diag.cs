// SPDX-License-Identifier: MIT

namespace FFXVR;

// Battle load diagnostics (design-notes "Battle load diagnostics + model cache option" and "HD cluster cache (phase 1)";
// research battle-load.md 2.1, battle-data-load.md section 7 and hd-cluster-cache-spec.md section 4; exe-verify-round2
// Part C, whose corrections are applied; the review build-load-review.md checked the first nine prologues, conventions
// and threads against the disassembly).
//
// diag_load (default 1; hooks at the next launch): per battle, the phases of the way in and the way back, timed with QPC,
// one summary line each in ffxvr_diag.log ("load: battle N ..."), with the HD cluster loads split into open / read / parse
// / GPU create and the pool bytes of the misses, the chr cache's entries in use, the game pool's reserved / committed
// counters and the address space (total free, largest free block, largest free block below 2 GB), plus the native
// low-2 GB reservation report ("va:" lines) at each battle start and end and at a zone change. The HD cluster cache
// (hd_cache.cs) rides on these hooks. battle_cache_models (the chr-flush skip) is retired: its ini key is read and ignored.
//
// Hooks (FhUtil offsets; every original always runs exactly once; recording never throws):
//   0x381590 battle teardown        cdecl void()            (main)   the way back starts (only when battle_state != 0)
//   0x509A20 map load               cdecl void(kind,map,p3) (main)   map request: classifies the nested free for the HD cache
//   0x509EA0 map free               cdecl void()            (main)   after it returns: the HD cache's releases and budget
//   0x508000 map build              cdecl void()            (main)   the read is done; queues the HD scene and TexList jobs
//   0x440030 chr cache flush        cdecl void()            (main)   timed only
//   0x36C490 loader wait-all        cdecl int(int mode)     (main)   mode != 1 blocks until the loader thread is idle; ~25 callers
//                                                                    ([0x2310C48] calls, the chr wrapper 0x82A5E0): each wait logs
//                                                                    its caller from a stack walk (ffxvr_game_callers)
//   0x429EF0 chr model load         cdecl void(int)         (main)
//   0x42A4C0 chr motion load        cdecl void(uint,int)    (main)
//   0x29B940 loadPCluster           thiscall PCluster*(this, path)   (loader thread mostly; chained with Fahrenheit's hook): the
//                                                                    window for the segments below, and the HD cache's acquire
//   0x1766D0 PStreamReaderFile ctor thiscall(this, path), ret 4      the open inside 0x4406C0 (in a window: a miss)
//   0x208090 PStreamFile::read      thiscall uint(this, buf, size), ret 8   (Fahrenheit hooks the ctor 0x207BC0, not this)
//   0x041620 PCluster platform init thiscall(this), ret     the GPU objects; idempotent (+0x44 bit 1); after loadPCluster, via 0x623580
//   0x2FB910 / 0x2FB8B0 pool alloc / free   cdecl void*(size) / void(raw)   only with diag_cluster_bytes: net bytes in a window
//                                                                    (block = *(raw - 0xC) - 0x10), on the loading thread only
// The five new ones were checked: standard prologues, no branch into the patched bytes, no FFX-60fps signature over them,
// none hooked by Fahrenheit. They do nothing outside a window but read a thread-local int.
// Load lock build (load_prof.cs): with diag_load_prof on at launch, two probes queued last (0x465AB0 the texture section,
// 0x58FC20 the PTexture2D create); loadPCluster's outermost calls mark the native profiler's window, the battle start and
// the teardown open its phases (they also arm load_lock_release), and each "load:" line is followed by a "prof:" block.
// The "GPU create" segment (0x441620) is shown as "post fixups": the textures are created inside parse (0x58FC20).
// NOT hooked: MsBattleExe 0x381020 and the return 0x461160. FFX-60fps's QOL-RETRY row sig-scans both prologues (a MinHook
// jump there would make it refuse, or collide with its own patch). The battle start is seen in the wait detour instead:
// MsBattleExe(mode 0) calls the loader's wait-all (0x781068) BEFORE it clears the request byte [0x112A8E2] (1 = random
// encounter, 2 = script battle request 0x781C90) at 0x78107E. Sg_MainLoop's entry is never touched.
public sealed unsafe partial class FFXVRModule {

    private bool _diag_load = true;            // ini diag_load (hooks at the next launch)
    private string _ld_status = "not set up";
    private bool _ld_queued, _ld_hooked, _ld_any;   // the nine core hooks all queued (the HD cache needs them) / at least one (the summary lines run with what is there, LD-8)
    private bool _ld_open_q, _ld_read_q, _ld_gpu_q, _ld_bytes_q;   // HC-11: which segment hooks queued (for the expected-mask line); _ld_bytes_q = both pool hooks
    private int _ld_seen_mask;                 // LD-8 (Interlocked.Or / Volatile.Read): bit per hook, set at its first call (1 teardown, 2 map load, 4 map free, 8 build, 16 flush, 32 wait, 64 model, 128 motion, 256 loadPCluster, 512 open, 1024 read, 2048 GPU create, 4096 alloc, 8192 free)
    private bool _ld_seen_logged; private bool _ld_resolved_rt;
    private int _ld_fill_peak = -1, _ld_fill_live = -1, _ld_fill_tear = -1, _ld_fill_tick; private bool _ld_table_full;   // LD-2: the table in battle (render thread)
    private string _ld_va_in = "", _ld_va_out = "";
    // The battle start's address-space walk (VirtualQuery over the whole space, 1-10 ms) no longer runs on the render thread
    // in the Present where the start is seen: that Present opens the battle transition (the glass break's first frames).
    // It runs on the main thread in MsBattleExe's wait-all, just before that wait blocks (_ld_va_in_main, published before
    // _ld_start_seq); a start not seen in the wait reads it at live instead. The opt-in ntdll report (diag_va_log) is put
    // off to live while a transition window is open.
    private string? _ld_va_in_main;
    private bool _ld_va_in_defer, _ld_va_rep_defer;
    private string _ld_va_why = "";            // BT-8: why the start's walk was put off (no main-thread walk: not seen in the wait, or it failed)
    private int _ld_va_late;                   // BT-8: Presents left until the put-off walk / report (30 after live: not on the transition's release frame)
    private delegate* unmanaged[Cdecl]<uint*, int, int> _fn_game_callers;   // the stack's return addresses inside FFX.exe, as Ghidra VAs
    private delegate* unmanaged[Cdecl]<uint*, void> _fn_va_stats;           // MB: total free, largest free, largest free below 2 GB, free below 2 GB

    private const int kl_teardown = 0x381590, kl_mapload = 0x509A20, kl_mapfree = 0x509EA0, kl_build = 0x508000, kl_flush = 0x440030;
    private const int kl_wait = 0x36C490, kl_model = 0x429EF0, kl_motion = 0x42A4C0, kl_pcl = 0x29B940;
    private const int kl_open = 0x1766D0, kl_read = 0x208090, kl_gpu = 0x41620, kl_alloc = 0x2FB910, kl_free = 0x2FB8B0;
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]    private delegate int dl_v();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]    private delegate int dl_i(int a);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]    private delegate int dl_ii(int a, int b);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]    private delegate int dl_iii(int a, int b, int c);
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] private delegate nint dl_pcl(nint self, nint path);   // also the open (0x5766D0)
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] private delegate uint dl_read(nint self, nint buf, uint size);
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] private delegate int  dl_this(nint self);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]    private delegate nint dl_alloc(uint size);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]    private delegate int  dl_free(nint raw);
    // Every detour returns the original's eax (int / pointer), so a function Ghidra calls void loses nothing either way.
    private dl_v?   _hl_teardown, _ol_teardown, _hl_mapfree, _ol_mapfree, _hl_build, _ol_build, _hl_flush, _ol_flush;
    private dl_i?   _hl_wait, _ol_wait, _hl_model, _ol_model;
    private dl_ii?  _hl_motion, _ol_motion;
    private dl_iii? _hl_mapload, _ol_mapload;
    private dl_pcl? _hl_pcl, _ol_pcl, _hl_open, _ol_open;
    private dl_read? _hl_read, _ol_read; private dl_this? _hl_gpu, _ol_gpu; private dl_alloc? _hl_alloc, _ol_alloc; private dl_free? _hl_free, _ol_free;
    private nint _al_teardown, _al_mapload, _al_mapfree, _al_build, _al_flush, _al_wait, _al_model, _al_motion, _al_pcl, _al_open, _al_read, _al_gpu, _al_alloc, _al_free;

    // -- running totals (Interlocked: 64-bit values written by the main and loader threads, read at Present) --------------
    private enum LT { pcl_n, pcl_t, pcl_main_n, pcl_fast_n, model_n, model_t, motion_n, motion_t, wait_n, wait_t, free_n, free_t,
                      flush_n, flush_t, build_n, build_t, teardown_t,
                      pcl_miss_n, pcl_miss_t, open_t, read_t, read_b, gpu_n, gpu_t, alloc_b, elig_n,   // the HD cluster segments (hd_cache.cs windows)
                      hc_added, hc_held_hits, hc_saved, hc_t, hdr_ok, hdr_vram_b, hdr_bad, hc_skip_up,                     // the HD cache: acquires, hits on held clusters, hits only it made possible, main-thread time at map frees
                      pr_plat_n, pr_plat_t, pr_pre_n, pr_pre_t, pr_texc_n, pr_texc_t, pr_texc_b, pr_big_n, pr_big_t,   // load lock build: the probes (load_prof.cs)
                      pr_s0_n, pr_s0_parse, pr_s0_pre, pr_s0_texc, pr_s1_n, pr_s1_parse, pr_s1_pre, pr_s1_texc,           // ... per file-size bucket (contiguous: n, parse, pre, texc)
                      pr_s2_n, pr_s2_parse, pr_s2_pre, pr_s2_texc, pr_s3_n, pr_s3_parse, pr_s3_pre, pr_s3_texc, N }
    private readonly long[] _lt = new long[(int)LT.N];
    private readonly long[] _lt_base_in = new long[(int)LT.N], _lt_base_out = new long[(int)LT.N];   // taken on the main thread at the battle start / teardown
    private void lt_add(LT k, long v) => System.Threading.Interlocked.Add(ref _lt[(int)k], v);
    private long lt(LT k) => System.Threading.Interlocked.Read(ref _lt[(int)k]);
    private void lt_snap(long[] into) { for (int i = 0; i < (int)LT.N; i++) into[i] = System.Threading.Interlocked.Read(ref _lt[i]); }
    private long d_in(LT k) => lt(k) - _lt_base_in[(int)k];
    private long d_out(LT k) => lt(k) - _lt_base_out[(int)k];

    // -- main-thread events (one writer; the render thread reads them once the phase is over) --------------------------------
    private int  _ld_main_tid, _ld_render_mtid, _ld_depth_map, _ld_depth_free, _ld_depth_teardown, _ld_faults;
    private volatile int _ld_start_seq, _ld_tear_seq;   // published after the times below (main thread)
    // main-thread times (LD-6: written and read with Volatile, atomic for long on 32-bit)
    private long _ld_t_start, _ld_t_start_wait, _ld_t_wait_end, _ld_t_tear, _ld_t_mapreq, _ld_t_retreq, _ld_t_retbuild, _ld_t_free_in, _ld_t_free_out;
    private volatile bool _ld_in_start_wait;   // LD-4: MsBattleExe's wait is under way
    private int  _ld_kind;                     // 1 random encounter, 2 script battle request, 0 unknown (MsBattleExe without the wait); main thread only (LD-3)
    private int  _ld_kind_tear = -1;           // LD-3: the teardown count when _ld_kind was set: the kind belongs to the battle that follows it, no other
    private bool _ld_cur_entry;                // the map load under way is a battle's own (battle_state 9): its free / flush belong to the way in
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
        if (!_diag_load) { _ld_status = "off (ffxvr.ini diag_load=0): no load-timing hook; the HD cluster cache needs them too"; _hc_status = "off (diag_load=0)"; _logger.Info("load diagnostics: " + _ld_status); return; }
        if (!_native_ok || _fn_safe_read == null || _fn_diag_line == null) { _ld_status = "off (the native helper did not load)"; _hc_status = "off (no native helper)"; _logger.Info("load diagnostics: " + _ld_status); return; }
        _hl_teardown = hl_teardown; _hl_mapload = hl_mapload; _hl_mapfree = hl_mapfree; _hl_build = hl_build; _hl_flush = hl_flush;
        _hl_wait = hl_wait; _hl_model = hl_model; _hl_motion = hl_motion; _hl_pcl = hl_pcl;
        _hl_open = hl_open; _hl_read = hl_read; _hl_gpu = hl_gpu;
        if (_diag_cluster_bytes) { _hl_alloc = hl_alloc; _hl_free = hl_free; }
        new FhCallLocation("FFX.exe", kl_teardown).try_resolve(out _al_teardown); new FhCallLocation("FFX.exe", kl_mapload).try_resolve(out _al_mapload);
        new FhCallLocation("FFX.exe", kl_mapfree).try_resolve(out _al_mapfree);   new FhCallLocation("FFX.exe", kl_build).try_resolve(out _al_build);
        new FhCallLocation("FFX.exe", kl_flush).try_resolve(out _al_flush);       new FhCallLocation("FFX.exe", kl_wait).try_resolve(out _al_wait);
        new FhCallLocation("FFX.exe", kl_model).try_resolve(out _al_model);       new FhCallLocation("FFX.exe", kl_motion).try_resolve(out _al_motion);
        new FhCallLocation("FFX.exe", kl_pcl).try_resolve(out _al_pcl);
        new FhCallLocation("FFX.exe", kl_open).try_resolve(out _al_open);         new FhCallLocation("FFX.exe", kl_read).try_resolve(out _al_read);
        new FhCallLocation("FFX.exe", kl_gpu).try_resolve(out _al_gpu);
        if (_diag_cluster_bytes) { new FhCallLocation("FFX.exe", kl_alloc).try_resolve(out _al_alloc); new FhCallLocation("FFX.exe", kl_free).try_resolve(out _al_free); }
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
        // The segment hooks, queued after the nine (Fahrenheit's commit stops at the first failing insertion: a failure here must
        // not take the core ones, which the cache needs, with it); the two pool hooks last.
        int ok9 = ok;
        string s2 = one("open 0x5766D0", _al_open, () => new FhCallHandle<dl_pcl>(new FhCallLocation((void*)_al_open)).hook(this, _hl_open!));
        _ld_open_q = ok > ok9; ok9 = ok;
        s2 += ", " + one("read 0x608090", _al_read, () => new FhCallHandle<dl_read>(new FhCallLocation((void*)_al_read)).hook(this, _hl_read!));
        _ld_read_q = ok > ok9; ok9 = ok;
        s2 += ", " + one("GPU create 0x441620", _al_gpu, () => new FhCallHandle<dl_this>(new FhCallLocation((void*)_al_gpu)).hook(this, _hl_gpu!));
        _ld_gpu_q = ok > ok9;
        if (_diag_cluster_bytes) {
            int ok12 = ok;
            s2 += ", " + one("pool alloc 0x6FB910", _al_alloc, () => new FhCallHandle<dl_alloc>(new FhCallLocation((void*)_al_alloc)).hook(this, _hl_alloc!));
            s2 += ", " + one("pool free 0x6FB8B0", _al_free, () => new FhCallHandle<dl_free>(new FhCallLocation((void*)_al_free)).hook(this, _hl_free!));
            _ld_bytes_q = ok == ok12 + 2;
        }
        else s2 += ", pool alloc / free off (diag_cluster_bytes=0: the bytes read stand in for the resident size)";
        // Load lock build: the two probes last of all (a refusal here cannot take any hook above with it; LL-13: Fahrenheit's queue
        // is global, so a probe failing at commit would still drop the hooks of a mod queued after ours)
        if (_diag_load_prof != 0) {
            _hl_plat = hl_plat; _hl_texc = hl_texc;
            new FhCallLocation("FFX.exe", kl_plat).try_resolve(out _al_plat); new FhCallLocation("FFX.exe", kl_texc).try_resolve(out _al_texc);
            int okp = ok;
            s2 += ", " + one("texture section 0x465AB0", _al_plat, () => new FhCallHandle<dl_plat>(new FhCallLocation((void*)_al_plat)).hook(this, _hl_plat!));
            s2 += ", " + one("texture create 0x58FC20", _al_texc, () => new FhCallHandle<dl_texc>(new FhCallLocation((void*)_al_texc)).hook(this, _hl_texc!));
            _pr_q = ok > okp;
            _pr_status = ok == okp + 2 ? "both probes queued" : _pr_q ? "only one probe queued (see the load diagnostics status)" : "NOT queued (see the load diagnostics status)";
        }
        else { s2 += ", probes off (diag_load_prof=0)"; _pr_status = "off (diag_load_prof=0 at launch)"; }
        _ld_any = ok > 0;
        _ld_status = (_ld_hooked ? "queued" : "INCOMPLETE (the HD cluster cache stays off)") + $": {s}; segments: {s2}";
        hc_init_targets();
        int va = _diag_va_log && _fn_va_watch != null ? _fn_va_watch(1) : 0;   // HC-6: the ntdll patch only with diag_va_log; else the patch-free region diff
        _logger.Info($"load diagnostics: HD cluster cache {_hc_status}; ntdll reservation log {(_diag_va_log ? (va != 0 ? "on" : "requested but the patch was refused") : "off (diag_va_log=0; using the region diff)")}");
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
        one(ref _ol_open, _al_open, _hl_open);             one(ref _ol_read, _al_read, _hl_read);             one(ref _ol_gpu, _al_gpu, _hl_gpu);
        one(ref _ol_alloc, _al_alloc, _hl_alloc);          one(ref _ol_free, _al_free, _hl_free);
        one(ref _ol_plat, _al_plat, _hl_plat);             one(ref _ol_texc, _al_texc, _hl_texc);   // load lock build: the probes
    }

    // -- the detours ----------------------------------------------------------------------------------------------------------
    private int hl_teardown() {
        dl_v o = System.Threading.Volatile.Read(ref _ol_teardown) ?? d7_resolve_main(ref _ol_teardown, _al_teardown, _hl_teardown!);
        bool end = false; long t0 = now();
        try { seen(1); end = FhUtil.get_at<sbyte>(0xD2A8E0) != 0; if (end) ld_mark_teardown(t0); } catch { _ld_faults++; }   // state 0: it returns at once (also its call inside MsBattleExe)
        _ld_depth_teardown++;
        int r;
        try { r = o(); } finally { _ld_depth_teardown--; }
        if (end) lt_add(LT.teardown_t, now() - t0);
        return r;
    }
    private int hl_mapload(int kind, int map, int p3) {
        dl_iii o = System.Threading.Volatile.Read(ref _ol_mapload) ?? d7_resolve_main(ref _ol_mapload, _al_mapload, _hl_mapload!);
        try { seen(2); if (_ld_main_tid == 0) _ld_main_tid = Environment.CurrentManagedThreadId; ld_map_request(kind, map); } catch { _ld_faults++; _hc_p_valid = false; }   // HC-R-5: the main thread id is captured once, here (0x909A20's only callers: the battle state code and MAPREG)
        _ld_depth_map++;
        int r;
        try { r = o(kind, map, p3); } finally { _ld_depth_map--; _hc_p_valid = false; }   // its map free has used it (or there was none)
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
        long t1 = now();
        try { hc_after_free(mine); } catch { _ld_faults++; }   // the HD cache: releases (after its own render flush), budget, the HDC: line
        lt_add(LT.hc_t, now() - t1);
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
            seen(32); ctx = ld_wait_begin(t0);   // HC-R-5: the wait-all has ~25 callers; it no longer names the main thread
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
        if (t_hc_extra != 0) return o(self, path);   // the HD cache's own second call (a hit), or anything re-entering during it: straight through
        seen(256);
        bool outer = t_hc_depth == 0;                // a load nested inside a load belongs to the outer one's window
        if (outer) { try { hc_win_open(); } catch { _ld_faults++; } }
        bool on_main = outer && _ld_main_tid != 0 && Environment.CurrentManagedThreadId == _ld_main_tid;
        if (outer) lp_mark(on_main, 1);              // load lock build: the native profiler's window (and the lock release's "a load is in progress")
        long t0 = now();
        t_hc_depth++;
        nint r;
        try { r = o(self, path); } finally { t_hc_depth--; if (outer) lp_mark(on_main, 0); }
        long dt = now() - t0;
        try {
            lt_add(LT.pcl_n, 1); lt_add(LT.pcl_t, dt);
            if (Environment.CurrentManagedThreadId == _ld_main_tid) lt_add(LT.pcl_main_n, 1);
            if (dt * 10000 < System.Diagnostics.Stopwatch.Frequency) lt_add(LT.pcl_fast_n, 1);   // < 0.1 ms: a ClusterManager hit (no I/O)
            if (outer) hc_after_load(o, self, path, r, dt);   // the segments; the cache's acquire (one more call of o, a hit)
        } catch { _ld_faults++; }
        return r;
    }
    // The segments, only inside a window of this thread (t_hc_win: 1 the load, 2 after it until the cluster's GPU create).
    private nint hl_open(nint self, nint path) {
        dl_pcl o = System.Threading.Volatile.Read(ref _ol_open) ?? d7_resolve_main(ref _ol_open, _al_open, _hl_open!);
        seen(512);
        if (t_hc_win != 1) return o(self, path);   // also the existence checks before loadPCluster (0x666880)
        t_hc_miss = true; long t0 = now(); nint r = o(self, path); t_hc_open += now() - t0;
        return r;
    }
    private uint hl_read(nint self, nint buf, uint size) {
        dl_read o = System.Threading.Volatile.Read(ref _ol_read) ?? d7_resolve_main(ref _ol_read, _al_read, _hl_read!);
        seen(1024);
        if (t_hc_win != 1) return o(self, buf, size);
        lp_first_read();   // load lock build: the texture section's first mip read ends its lock wait + temporary-buffer part
        long t0 = now(); uint r = o(self, buf, size); t_hc_read += now() - t0;
        uint got = r != 0 && r <= size ? r : size;   // HC-12: the bytes actually read when the return is a count (ReadFile path); else the request
        t_hc_readb += got;
        try { hc_header_bytes(buf, got); } catch { _ld_faults++; }   // the cluster header's sizes, by stream offset
        return r;
    }
    private int hl_gpu(nint self) {
        dl_this o = System.Threading.Volatile.Read(ref _ol_gpu) ?? d7_resolve_main(ref _ol_gpu, _al_gpu, _hl_gpu!);
        seen(2048);
        if (t_hc_win != 2 || self != t_hc_c) return o(self);   // only the cluster this thread just loaded
        bool real = false;
        try { real = read((byte*)self + 0x44, out byte fl) && (fl & 2) == 0; } catch { _ld_faults++; }   // bit 1: already created (a hit)
        long t0 = now(); int r = o(self); long dt = now() - t0;
        try { if (real) { lt_add(LT.gpu_n, 1); lt_add(LT.gpu_t, dt); } hc_win_close(true); } catch { _ld_faults++; }
        return r;
    }
    private nint hl_alloc(uint size) {
        dl_alloc o = System.Threading.Volatile.Read(ref _ol_alloc) ?? d7_resolve_main(ref _ol_alloc, _al_alloc, _hl_alloc!);
        bool big = size >= (1u << 20) && t_pr_in_plat == 1;   // load lock build: the texture section's temporary buffer (2x the mips, 0xCD-filled)
        long ta = big ? now() : 0;
        nint r = o(size);
        if (big) { lt_add(LT.pr_big_n, 1); lt_add(LT.pr_big_t, now() - ta); }
        seen(4096);
        if (t_hc_win != 0 && r != 0) { try { if (read((byte*)r - 0xC, out uint h) && h >= 0x10) t_hc_alloc += h - 0x10; } catch { _ld_faults++; } }   // the block header's rounded size + 0x10
        return r;
    }
    private int hl_free(nint raw) {
        dl_free o = System.Threading.Volatile.Read(ref _ol_free) ?? d7_resolve_main(ref _ol_free, _al_free, _hl_free!);
        seen(8192);
        if (t_hc_win != 0 && raw != 0) { try { if (read((byte*)raw - 0xC, out uint h) && h >= 0x10) t_hc_alloc -= h - 0x10; } catch { _ld_faults++; } }   // before the free: the header is still there
        return o(raw);
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
            ld_reset_in();
            lt_snap(_lt_base_in); _ld_wn_in = _ld_wn;
            string? va = null; try { va = ld_memory(); } catch { _ld_faults++; }   // main thread, before the wait blocks (not the render thread's transition frames)
            System.Threading.Volatile.Write(ref _ld_va_in_main, va);
            lp_phase(1);   // load lock build: the entry phase (profiler samples, the lock release armed) until the entry's load: line
            _ld_start_seq++;
            return 1;
        }
        return 10 + bst;
    }
    /// <summary>LD-5: the way-in values (main thread): at a battle start seen in the wait, at the battle's own map request, and at
    /// the teardown (for the next battle, whose start may not be seen in the wait).</summary>
    private void ld_reset_in() {
        _ld_fill_in0 = _ld_fill_in1 = -1; vw(ref _ld_t_free_in, 0); vw(ref _ld_t_mapreq, 0);
    }
    private void ld_mark_teardown(long t0) {
        _ld_kind = 0;   // LD-3: the battle is over; the next one needs its own start
        vw(ref _ld_t_tear, t0); vw(ref _ld_t_retreq, 0); vw(ref _ld_t_retbuild, 0);
        _ld_fill_out0 = _ld_fill_out1 = -1; vw(ref _ld_t_free_out, 0);   // LD-5
        ld_reset_in();
        lt_snap(_lt_base_out); _ld_wn_out = _ld_wn;
        lp_phase(2);   // load lock build: the way-back phase until the way-back load: line
        _ld_tear_seq++;
    }
    /// <summary>A map load request: times it (the battle's own map load at battle_state 9, LD-1; the first one after a teardown =
    /// the way back), and tells the HD cache what its nested map free means (hc_map_request).</summary>
    private void ld_map_request(int kind, int map) {
        long t = now(); sbyte bst = FhUtil.get_at<sbyte>(0xD2A8E0); _ld_cur_entry = bst == 9;
        int field = FhUtil.get_at<short>(0xD2CA94);              // the current field map: word [MsGetSaveEventAddress() + 4] (0x785240 returns 0x112CA90); MAPREG passes the same word
        int btlmap = FhUtil.get_at<short>(0xD2C254) & 0x3FF;     // Btl.battlefield_id; the state-8 code loads (id & 0x3FF)
        if (bst == 9) { ld_reset_in(); vw(ref _ld_t_mapreq, t); }
        else if (bst == 0 && _ld_tear_seq != 0 && vr(ref _ld_t_retreq) == 0 && vr(ref _ld_t_tear) != 0) vw(ref _ld_t_retreq, t);   // the first map load after the teardown: the way back
        hc_map_request(kind, map, bst, field, btlmap);
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
        if (_ld_render_mtid == 0) _ld_render_mtid = Environment.CurrentManagedThreadId;   // HC-R-5: Present runs on the render thread
        if (!_ld_any || _fn_diag_line == null) return;   // LD-8: with whatever hooks are in
        try { ld_present_inner(); } catch (Exception e) { if (_ld_faults++ < 3) _logger.Error($"load diagnostics: {e}"); }
    }
    private void ld_present_inner() {
        hc_present();   // the HD cache's HDC: lines (one per map free, written on the main thread)
        if (_ld_va_late > 0 && --_ld_va_late == 0) ld_va_in_late(false);   // BT-8
        if (!_ld_checked_waitptr) {   // the loader's function table at 0x2310C40: +8 must be the wait we hooked (MsBattleExe calls [0x2310C48])
            nint fp = FhUtil.get_at<nint>(0x1F10C48);
            if (fp != 0) { _ld_checked_waitptr = true; d7_log(I($"load diagnostics: loader wait-all pointer [0x2310C48] = 0x{(long)fp:X} (hooked 0x{(long)_al_wait:X}): {(fp == _al_wait ? "match" : "MISMATCH: the battle-start and teardown waits will not be seen")}")); }
        }
        long t = now(), f = System.Diagnostics.Stopwatch.Frequency;
        sbyte bst = FhUtil.get_at<sbyte>(0xD2A8E0);
        int ss = _ld_start_seq, ts = _ld_tear_seq;
        if (_ld_phase == 0 && ts != _ld_seen_tear) { _ld_seen_tear = ts; lp_phase(-2); }   // LL-8: a teardown with no way back to follow (the battle ended before live): its phase ends
        if (_ld_phase == 0 || _ld_phase == 3) {
            if (ss != _ld_seen_start || (_ld_phase == 0 && bst > 1 && bst < 0x17)) {   // a battle starts (seen in the wait, or by its state if no wait was seen)
                if (_ld_phase == 3) ld_log_out(t, "(the next battle started)");
                bool by_wait = ss != _ld_seen_start; _ld_seen_start = ss;
                _ld_rkind = by_wait ? _ld_kind : 0;   // LD-3: the render thread keeps its own copy and never writes the main thread's
                if (!by_wait) { _ld_rstart = t; lt_snap(_lt_base_in); _ld_wn_in = _ld_wn; lp_phase(1); } else _ld_rstart = vr(ref _ld_t_start);   // load lock build: a start not seen in the wait opens the entry phase here
                _ld_phase = 1; _ld_battle_n++; Array.Clear(_ld_t_state); _ld_countdown = 0;
                _ld_fill_peak = _ld_fill_live = _ld_fill_tear = -1; _ld_table_full = false; _ld_fill_tick = 0;
                _ld_field_name = map_name(); _ld_field_id = FhUtil.get_at<short>(0xD2CA94);
                string? va = by_wait ? System.Threading.Volatile.Read(ref _ld_va_in_main) : null;   // the main thread's walk (ld_wait_begin)
                System.Threading.Volatile.Write(ref _ld_va_in_main, null);
                _ld_va_in_defer = va == null; _ld_va_late = 0;
                _ld_va_why = by_wait ? "the main thread's walk at the start failed" : "the start was not seen in the wait";
                _ld_va_in = va ?? I($"(put off: {_ld_va_why}; logged on its own line 30 Presents after live)");
                if (bt_active()) _ld_va_rep_defer = true;   // the glass break is on screen: the ntdll report (opt-in) waits for live
                else { _ld_va_rep_defer = false; va_report(I($"battle {_ld_battle_n} start")); }   // the low-2 GB reservations since the last report, by caller
            }
        }
        if (_ld_phase == 1 || _ld_phase == 2) {   // LD-2: the table during the battle (sampled every 30 Presents, and at live / teardown)
            if (++_ld_fill_tick >= 30) { _ld_fill_tick = 0; ld_sample_fill(); }
        }
        if (_ld_phase == 1) {
            if (bst >= 0 && bst < 64 && _ld_t_state[bst] == 0) _ld_t_state[bst] = t;
            if (FhUtil.get_at<byte>(0xD2A918) != 0) _ld_countdown = Math.Max(_ld_countdown, FhUtil.get_at<byte>(0xD2A919));
            long we = vr(ref _ld_t_wait_end), since = Math.Max(_ld_rstart, we);
            if (bst == 1) { _ld_t_live = t; _ld_fill_live = ld_sample_fill(); ld_log_in(); _ld_phase = 2; _ld_seen_tear = ts; if (_ld_va_in_defer || _ld_va_rep_defer) _ld_va_late = 30; }   // BT-8: not on the release frame
            else if (bst == 0 && !_ld_in_start_wait && t - since > f) { ld_va_in_late(true); ld_log_in(); _ld_phase = 0; }   // no transition on screen: at once, into the line   // ended before it went live (LD-4: never during the start wait, and counted from its end)
        }
        else if (_ld_phase == 2) {
            if (ts != _ld_seen_tear) { _ld_seen_tear = ts; _ld_phase = 3; _ld_idle_frames = 0; _ld_fill_tear = ld_sample_fill(); _ld_va_out = ld_memory(); va_report(I($"battle {_ld_battle_n} end")); }
            else if (bst == 0 && t - _ld_t_live > 2 * f) _ld_phase = 0;   // over without a teardown seen (no way-back line)
        }
        else if (_ld_phase == 3) {
            // back on the field: the field build ran (or no reload came within 3 s) and the loader thread has been idle for 10 Presents
            long tear = vr(ref _ld_t_tear);
            bool built = vr(ref _ld_t_retbuild) != 0 || (vr(ref _ld_t_retreq) == 0 && t - tear > 3 * f);
            _ld_idle_frames = built && bst == 0 && ld_loader_idle() ? _ld_idle_frames + 1 : 0;
            if (_ld_idle_frames >= 10) { ld_log_out(t, ""); _ld_phase = 0; try { hc_settled(false); } catch { _ld_faults++; } }   // the way back settled: the HD cache's pool check
            else if (t - tear > 60 * f) { ld_log_out(t, "(timed out after 60 s)"); _ld_phase = 0; try { hc_settled(true); } catch { _ld_faults++; } }   // HC-T-2: the timeout path too
        }
    }
    /// <summary>BT-8: the start's walk when the main thread did not take it, and the put-off ntdll report. `now` (an end before
    /// live): into the "load:" line about to be written; else 30 Presents after live, on a line of its own.</summary>
    private void ld_va_in_late(bool now) {
        if (_ld_va_in_defer) {
            _ld_va_in_defer = false;
            if (now) _ld_va_in = ld_memory() + I($" (read at the end: {_ld_va_why})");
            else d7_log(I($"load: battle {_ld_battle_n}: address space for \"at the start\", read 30 Presents after live ({_ld_va_why}): {ld_memory()}"));
        }
        if (_ld_va_rep_defer) { _ld_va_rep_defer = false; va_report(I($"battle {_ld_battle_n} start (put off: battle transition)")); }
    }
    /// <summary>LD-2: the chr table's used entries now; keeps the peak and flags a full table (the leaking 0x8409A0 eviction runs then).</summary>
    private int ld_sample_fill() {
        int n = ld_cache_fill(out _);
        if (n > _ld_fill_peak) _ld_fill_peak = n;
        if (n >= 100 && !_ld_table_full) { _ld_table_full = true; d7_log(I($"load: battle {_ld_battle_n}: chr cache TABLE FULL (100 entries in use)")); }
        return n;
    }
    /// <summary>Address space and the game pool, once per battle start and end (render thread).</summary>
    private string ld_memory() {
        uint* v = stackalloc uint[4]; v[0] = v[1] = v[2] = v[3] = 0;
        var vs = _fn_va_stats; if (vs != null) vs(v);
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
        if (!_ld_seen_logged) { _ld_seen_logged = true; int exp = 0x1FF | (_ld_open_q ? 0x200 : 0) | (_ld_read_q ? 0x400 : 0) | (_ld_gpu_q ? 0x800 : 0) | (_ld_bytes_q ? 0x3000 : 0) | (_pr_q ? 0xC000 : 0); d7_log(I($"load diagnostics: hooks seen working so far 0x{seen_mask:X} of the queued 0x{exp:X} (1 teardown, 2 map load, 4 map free, 8 build, 16 flush, 32 wait, 64 model, 128 motion, 256 loadPCluster, 512 open, 1024 read, 2048 post fixups, 4096 pool alloc, 8192 pool free, 16384 texture section, 32768 texture create)")); }
        long t0 = _ld_rstart, live = _ld_t_live != 0 && _ld_phase == 1 && FhUtil.get_at<sbyte>(0xD2A8E0) == 1 ? _ld_t_live : now();
        long mq = vr(ref _ld_t_mapreq), req = mq != 0 && mq > t0 ? mq : 0, s10 = tst(10);
        double total = ms(live - t0), a = req != 0 ? ms(req - t0) : s10 != 0 ? ms(s10 - t0) : 0, b = req != 0 ? ms(vr(ref _ld_t_free_in)) : 0;
        double c = req != 0 && s10 != 0 ? ms(s10 - req) - b : 0, d = s10 != 0 ? ms(live - s10) : 0;
        string kind = _ld_rkind == 1 ? "random" : _ld_rkind == 2 ? "script" : "start not seen in the wait";
        string flush = req != 0 ? I($"{ms(d_in(LT.flush_t)):F0} ms") : "no map load";
        d7_log(I($"load: battle {_ld_battle_n} ({kind}) in {_ld_field_name} (field map {_ld_field_id}, battle map {FhUtil.get_at<short>(0xD2C254) & 0x3FF}): total {total:F0} ms = to the map load {a:F0} (start wait {ms(vr(ref _ld_t_start_wait)):F0}) + field free {b:F0} (chr flush {flush}) + map load {c:F0} + models {d:F0} ({d_in(LT.model_n)} model / {d_in(LT.motion_n)} motion loads, {ms(d_in(LT.model_t) + d_in(LT.motion_t)):F0} ms on the main thread)")
             + " | " + ld_hd(_lt_base_in) + I($" | blocking waits {ms(d_in(LT.wait_t)):F0} ms ({d_in(LT.wait_n)}: {ld_waits(_ld_wn_in)})")
             + I($" | states: 4 {st_ms(4, t0)} 5 {st_ms(5, t0)} 7 {st_ms(7, t0)} 9 {st_ms(9, t0)} 0x22 {st_ms(0x22, t0)} 10 {st_ms(10, t0)} live {ms(live - t0):F0}{(_ld_countdown > 0 ? I($" | countdown 0x112A919 up to {_ld_countdown} ticks") : "")} | chr cache {fill(_ld_fill_in0)}->{fill(_ld_fill_in1)} entries in use at the field free ({_ld_locked} locked; of 100), {fill(_ld_fill_live)} at live | at the start: {_ld_va_in}"));
        lp_report(1, I($"battle {_ld_battle_n} entry ({_ld_field_name}, battle map {FhUtil.get_at<short>(0xD2C254) & 0x3FF})"), _lt_base_in);   // load lock build: the prof block, then the entry phase ends
    }
    /// <summary>The HD cluster part of a summary line, since `b` (the battle start's or the teardown's baseline): the loads,
    /// the misses' segments (parse = the rest of the miss time: deserialize, fixups, the tree), the GPU creates, the pool bytes
    /// the misses kept, and the cache.</summary>
    private string ld_hd(long[] b) {
        long d(LT k) => lt(k) - b[(int)k];
        long n = d(LT.pcl_n), mi = d(LT.pcl_miss_n), mt = d(LT.pcl_miss_t), op = d(LT.open_t), rd = d(LT.read_t);
        string seg = mi > 0 ? I($"; the {mi} misses {ms(mt):F0} ms = open {ms(op):F0} + read {ms(rd):F0} ({d(LT.read_b) >> 20} MB) + parse {ms(Math.Max(0, mt - op - rd)):F0}") : "; no misses";
        string gpu = (seen_mask & 2048) != 0 ? I($"; post fixups (0x441620; the textures are created inside parse) {ms(d(LT.gpu_t)):F0} ms ({d(LT.gpu_n)})") : "";
        string bytes = (_ld_bytes_q && (seen_mask & 4096) != 0 ? I($"; the misses kept {d(LT.alloc_b) >> 10} KB of the game pool") : "")
                     + I($"; header sizes for {d(LT.hdr_ok)} of {mi} misses ({d(LT.hdr_vram_b) >> 20} MB vram; {d(LT.hdr_bad)} out of bounds)");
        return I($"HD clusters {n} loads ({n - mi} hits, {d(LT.pcl_main_n)} on the main thread; {d(LT.elig_n)} cacheable, not yet held) {ms(d(LT.pcl_t)):F0} ms{seg}{gpu}{bytes}")
             + I($" | HD cache {(_hd_cluster_cache && _hc_ready ? "on" : "off")}: {d(LT.hc_saved)} hits saved ({d(LT.hc_held_hits)} loads of held clusters, {d(LT.hc_added)} newly held, {d(LT.hc_skip_up)} skipped: the pool's transient block was up), held {_hc_held_n} (vram {_hc_held_vram_mb} MB) after the last map free, {ms(d(LT.hc_t)):F0} ms of ours at map frees");
    }
    private string st_ms(int s, long t0) => tst(s) != 0 ? I($"+{ms(tst(s) - t0):F0}") : "-";
    private static string fill(int n) => n < 0 ? "?" : n.ToString(System.Globalization.CultureInfo.InvariantCulture);
    private void ld_log_out(long t, string note) {
        int ts = _ld_tear_seq; long t0 = vr(ref _ld_t_tear); if (t0 == 0 || ts == _ld_logged_tear) { lp_phase(-2); return; }   // LL-8: no way-back line, but the phase ends
        _ld_logged_tear = ts;   // LD-6: the render thread keeps its own "logged" mark instead of zeroing a main-thread field
        long req = vr(ref _ld_t_retreq), bld = vr(ref _ld_t_retbuild);
        double total = ms(t - t0), a = req != 0 ? ms(req - t0) : 0, b = req != 0 ? ms(vr(ref _ld_t_free_out)) : 0, c = req != 0 ? ms(t - req) - b : 0;
        string flush = req == 0 ? "no field reload (battle map = field map?)" : I($"{ms(d_out(LT.flush_t)):F0} ms");
        d7_log(I($"load: battle {_ld_battle_n} way back: total {total:F0} ms = teardown {ms(d_out(LT.teardown_t)):F0} + to the field reload {a:F0} + battle-map free {b:F0} (chr flush {flush}) + reload to idle {c:F0} (build at {(bld != 0 ? I($"+{ms(bld - t0):F0}") : "-")}; {d_out(LT.model_n)} model / {d_out(LT.motion_n)} motion loads)")
             + " | " + ld_hd(_lt_base_out) + I($" | blocking waits {ms(d_out(LT.wait_t)):F0} ms ({d_out(LT.wait_n)}: {ld_waits(_ld_wn_out)}) | chr cache: peak in battle {fill(_ld_fill_peak)}{(_ld_table_full ? " (TABLE FULL seen)" : "")}, {fill(_ld_fill_tear)} at the teardown, {fill(_ld_fill_out0)}->{fill(_ld_fill_out1)} at the battle-map free | at the end: {_ld_va_out} {note}"));
        lp_report(2, I($"battle {_ld_battle_n} way back{(note.Length > 0 ? " " + note : "")}"), _lt_base_out);   // load lock build: the prof block, then the way-back phase ends
    }
}
