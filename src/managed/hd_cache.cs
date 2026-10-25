// SPDX-License-Identifier: MIT

namespace FFXVR;

// HD cluster cache, phase 1 (design-notes "HD cluster cache (phase 1)"; research hd-cluster-cache-spec.md, with its section 9
// corrections; exe-verify-round2 C4). The engine's ClusterManager already caches HD clusters by path with a refcount: a load
// of a path it holds is a hit (refcount + 1, no I/O), and a cluster is destroyed when its refcount drops below 1. Every random
// battle frees the field map's textures and loads the battle map's, and frees those again on the way back, so the same ~115
// textures are read, parsed and uploaded twice per battle. Here we hold ONE extra reference per eligible texture cluster,
// so the game's own release leaves it alive and its next load is a hit.
//
// - Acquire (load_diag.cs hl_pcl, on the loading thread, inside our loadPCluster detour): after the game's own load returned
//   a cluster whose path is eligible (ends with .dds.phyre and has /map/, /chr/ or /magic/ (battle maps live under /map/); never /menu/ (the force
//   unload 0x69B310), /shaders/ (never freed anyway) or /event/), we call the chain once more with the same path: a hit by
//   construction (the entry is in the tree, the caller holds it), so refcount + 1 and no I/O. A thread-local flag passes that
//   call (and anything re-entering during it) straight through. At most one held reference per cluster (a re-hit only
//   refreshes its stamp); one acquire at a time (a CAS flag: a second thread skips instead of waiting). No allocation, no
//   lock, no logging on that thread: the slots are preallocated arrays published with Volatile / Interlocked.
// - Release (only here, on the main thread, right after the game's map free 0x909EA0 returned): first what 0x671300 does
//   before its own releases (kick the context's partly recorded list, ctx->vtbl[2], then the full render flush 0x5BC3F0), so
//   no queued command list still uses what is freed; then, per slot, the identity check getPClusterByName(normalised path) ==
//   our pointer (else the slot is forgotten), and ClusterManager::releasePCluster BY POINTER 0x69BDB0 (refcount-aware: it frees
//   only if the game holds none). Never by name (0x69BE90 hashes its raw argument), never the force unload.
// - Which map free (the map load request 0x909A20 classifies, the free acts): the battle's own map load (battle_state 9) of a
//   RANDOM battle from the field we hold = ENTRY (keep; admit); of a script battle = keep, admit /magic/ only (its battle map
//   is usually unique); the way back (0x909A20 at battle_state 0 = MAPREG) to the same field map = EXIT (keep; admit);
//   anything else = ZONE: release all, then admit the new field's loads. Map frees outside 0x909A20 (area changes, save
//   loads, the title, events: 0x9093B0 / 0x909680 / 0x909E60) are always ZONE.
// - Budget (each map free; retuned from session 1, see the constants): two caps, VRAM (min 1 GB, 25 % of the budget; no
//   admission above 75 % use) and the game-pool bytes the held set pins (min 8 MB, half the pool's free part), oldest first
//   over either; admission while a 129 MB block is free below 2 GB (the game's transient 128 MB pool growth) and, per
//   cluster, only while that transient block is down for its whole load (HC-T-1: residue in it would keep the game from
//   freeing it); release all when the block is gone with the transient block down; at most 260 held.
// - Off (hd_cluster_cache=0, the default for the first A/B): nothing is admitted, and anything still held is released at the
//   next map free. The diagnostics (eligible paths, the HDC: line per map free) run either way.
public sealed unsafe partial class FFXVRModule {

    private bool _hd_cluster_cache = false;    // ini hd_cluster_cache (admission at once; releases at the next map free)
    private bool _diag_cluster_bytes = false;  // HC-2: ini diag_cluster_bytes (hooks at the next launch; default 0: the Phyre header read gives the size). The global pool hooks are a hot path, measurement only
    private bool _diag_va_log = false;          // HC-6: ini diag_va_log (hooks at the next launch; default 0: the patch-free region diff covers it). On: the ntdll NtAllocateVirtualMemory patch names the caller

    private const int  k_hc_max = 260, k_hc_path = 256;   // HC-4: one field + one battle map, not 400
    // Retune from session 1 (kl/klyt0000, 9 battles). Two separate budgets, and a low-2 GB gate sized to what the game needs.
    // - Low 2 GB: during each battle load the game reserves one transient 128 MB pool growth block (kernelbase+0x13D130 on the
    //   loader thread) and frees it afterwards. It needs one free block of that size; 1 MB of slack on top. Below that the cache
    //   must give its room back; at or above it the cache may hold. HC-T-1: a cluster is admitted only if the pool's reserved
    //   size equals the session base both when its load window opened and at the acquire (the transient block down).
    private const int  k_hc_low_need_mb = 129;
    // - Game pool: after a load a texture cluster leaves almost nothing in the game pool (session 1: 0-6 MB for ~82 misses per
    //   battle); the pool runs ~250 of 256 MB committed, so the cache may pin only a few MB of it, and never more than half the
    //   free part (res - com + held) / 2. Unmeasured (diag_cluster_bytes=0): k_hc_pool_est per cluster.
    private const long k_hc_pool_cap = 8L << 20; private const int k_hc_pool_est = 64 << 10;
    // - VRAM: the texture data lives there (3.4-4.4 GB of an 11.3 GB budget in session 1). Held VRAM <= min(1 GB, 25 % of the
    //   budget); no admission while use is above 75 % of the budget. Unmeasured: the average of the measured, else 4 MB.
    private const long k_hc_vram_cap = 1024L << 20; private const int k_hc_vram_cap_pct = 25, k_hc_vram_stop_pct = 75, k_hc_vram_est = 4 << 20;
    private const int  kh_release = 0x29BDB0, kh_getbyname = 0x29B4B0, kh_fios = 0x279820, kh_flush = 0x1BC3F0;   // call targets (FhUtil), not hooked by us
    private delegate* unmanaged[Thiscall]<nint, nint, void> _hc_release;     // ClusterManager::releasePCluster(this, PCluster*) (Fahrenheit does not hook it)
    private delegate* unmanaged[Thiscall]<nint, byte*, nint> _hc_getbyname;  // its entry: Fahrenheit's h_pcluster_get normalises, then the game's lookup (refcount > 0 only)
    private delegate* unmanaged[Cdecl]<byte*, byte*, int, void> _hc_fios;    // fiosUnifyFilename(src, dst, size) at its entry: what loadPCluster hashes (0x69B968)
    private delegate* unmanaged[Thiscall]<nint, void> _hc_flush;            // the full render flush 0x5BC3F0 (ECX = the context, as 0x671350 passes; it does not use it)
    private bool _hc_ready;                    // the core load hooks queued and the four call targets resolved (ld_init)
    private string _hc_status = "not set up";

    // the slots: state 0 free, 1 being filled (acquire), 2 live, 3 being released (main thread)
    private readonly int[]  _hc_state  = new int[k_hc_max];
    private readonly nint[] _hc_ptr    = new nint[k_hc_max];
    private readonly int[]  _hc_bytes  = new int[k_hc_max];   // GAME-POOL bytes it pins, measured at its load by the pool hooks (-1 = not measured: k_hc_pool_est)
    private readonly int[]  _hc_vram   = new int[k_hc_max];   // VRAM bytes: the Phyre header's texture + vertex + index sizes, else the file bytes read (-1 = not measured: the average)
    private readonly int[]  _hc_stamp  = new int[k_hc_max];   // the map-free count when last loaded (oldest first)
    private readonly byte[] _hc_orphan = new byte[k_hc_max];  // 1: at the last map free only our reference held it, so its next hit is one the cache saved
    private readonly byte[] _hc_path   = new byte[k_hc_max * k_hc_path];
    private int _hc_hi, _hc_adding;            // slots [0, _hc_hi) were ever used; the one-acquire-at-a-time flag
    private volatile int  _hc_mode;            // 0 admit nothing (before the first map free), 1 the eligible paths, 2 /magic/ only (a script battle)
    private volatile int  _hc_cap_kb;          // the game-pool cap from the last map free, KB (0: no admission)
    private volatile int  _hc_vcap_mb;         // the VRAM cap from the last map free, MB (0: no admission)
    private volatile bool _hc_admit_ok;        // the low-2 GB gate at the last map free (largest block >= k_hc_low_need_mb, or the transient pool block up)
    private int _hc_stamp_now;                 // one per map free (main thread writes, Volatile)
    // main thread
    private int _hc_zone = -1;                 // the field map whose clusters we hold (-1: whatever loads next)
    private bool _hc_p_valid, _hc_p_release; private int _hc_p_zone, _hc_p_mode; private string _hc_p_label = "";   // set at the map load request, used by its map free
    private readonly byte[] _hc_scan = new byte[0x6000 + 0x400];   // the manager's entry pool (+0x1C, 1024 x 0x18) and its used flags (+0x601C)
    private int _hc_mism, _hc_flush_fail, _hc_no_mgr, _hc_by_ptr;   // HC-1 / HC-14: name mismatch, flush put off, no manager, released by pointer with the name check failed
    private bool _hc_pool_valid, _hc_mism_logged;   // HC-1: the _hc_scan copy is current; the first mismatch was logged
    // the game pool's reserved size (main thread writes; Volatile for the render thread's settled check)
    private long _hc_res_base = -1;            // the lowest seen at a map free: the pool without its transient growth block
    private long _hc_res_hw = -1;              // the highest seen at a map free while the cache held nothing (the transient block is normal)
    private const long k_hc_growth_step = 128L << 20;   // HC-T-3: the game's one known transient block; growth = reserved > base + this
    private volatile bool _hc_settled_armed;   // HC-T-2: a cache-off (empty) way back has settled at the base: the settled test may brake
    private long _hc_pool_cap_hold = -1;       // HC-T-8: the pool cap computed with the transient block down, held over block-up map frees
    private volatile bool _hc_brake_req; private volatile string _hc_brake_req_why = "";   // set by the settled-field check (render thread), applied at the next map free
    private int _hc_low_empty = -1;            // the largest free block below 2 GB at the last map free with nothing held and the transient block down
    private volatile int _hc_low_last = -1; private volatile bool _hc_transient_up;   // for the overlay
    private volatile bool _hc_disabled;        // HC-1 / HC-R-9: an unrecoverable mismatch, or a second pool growth while holding: stop admitting for the session
    private volatile bool _hc_brake;           // HC-R-9: a pool growth while holding: released all, admission paused until the next zone change
    private int _hc_growths; private volatile string _hc_brake_reason = "";
    private volatile string _hc_off_reason = "";
    private volatile string _hc_reason = "nothing released yet";
    private volatile int _hc_held_n, _hc_held_kb, _hc_held_vram_mb, _hc_orphan_n;   // for the overlay, from the last map free (after its releases)
    private volatile int _hc_vram_mb, _hc_vram_budget_mb;         // HC-4: the graphics-memory readout at the last map free
    // main thread -> render thread: one line per map free (the render thread writes the log)
    private readonly string?[] _hc_msg = new string?[8]; private readonly bool[] _hc_msg_zone = new bool[8]; private volatile int _hc_msg_w; private int _hc_msg_r;
    // HC-7: sample loadPCluster paths across the session (loading thread -> render thread): path + verdict, logged with their fios form
    private const int k_hc_samp = 12;
    private readonly byte[] _hc_samp = new byte[k_hc_samp * k_hc_path]; private readonly int[] _hc_samp_verdict = new int[k_hc_samp];
    private int _hc_samp_w, _hc_samp_r, _hc_samp_tick;   // _hc_samp_w: Interlocked writer(s), Volatile.Read reader
    private delegate* unmanaged[Cdecl]<int, int> _fn_va_watch;
    private delegate* unmanaged[Cdecl]<byte*, void> _fn_va_report;
    private delegate* unmanaged[Cdecl]<byte*, int, byte*, uint*, int> _fn_va_regions;   // HC-6: the patch-free region diff (main thread)

    // per thread (the loading thread, in practice): one outermost loadPCluster, through that cluster's GPU create (0x441620)
    [ThreadStatic] private static int  t_hc_depth, t_hc_extra, t_hc_win, t_hc_slot;   // win: 0 none, 1 the load, 2 after it until its GPU create
    [ThreadStatic] private static bool t_hc_miss, t_hc_was_miss;
    [ThreadStatic] private static long t_hc_open, t_hc_read, t_hc_readb, t_hc_alloc, t_hc_alloc_load, t_hc_res_open;   // HC-T-1: the pool's reserved size when the window opened
    // the Phyre cluster header, captured by stream offset across the window's reads (session 1: a first-read-only capture got nothing)
    [ThreadStatic] private static long t_hc_hdr_off; [ThreadStatic] private static int t_hc_hdr_got; [ThreadStatic] private static uint t_hc_hdr_mark, t_hc_hdr_ib, t_hc_hdr_vb, t_hc_hdr_tex;
    [ThreadStatic] private static nint t_hc_c;

    /// <summary>At init (ld_init): the four call targets. The cache needs them and the core load hooks.</summary>
    private void hc_init_targets() {
        nint a = 0, b = 0, c = 0, d = 0;
        new FhCallLocation("FFX.exe", kh_release).try_resolve(out a); new FhCallLocation("FFX.exe", kh_getbyname).try_resolve(out b);
        new FhCallLocation("FFX.exe", kh_fios).try_resolve(out c);    new FhCallLocation("FFX.exe", kh_flush).try_resolve(out d);
        if (a == 0 || b == 0 || c == 0 || d == 0) { _hc_status = "off: FFX.exe call targets not resolved"; return; }
        _hc_release = (delegate* unmanaged[Thiscall]<nint, nint, void>)a; _hc_getbyname = (delegate* unmanaged[Thiscall]<nint, byte*, nint>)b;
        _hc_fios = (delegate* unmanaged[Cdecl]<byte*, byte*, int, void>)c; _hc_flush = (delegate* unmanaged[Thiscall]<nint, void>)d;
        _hc_ready = _ld_hooked;
        _hc_status = _hc_ready ? "ready" : "off: a load hook was not queued (see the load diagnostics status)";
    }

    // -- the loading thread (from hl_pcl) -------------------------------------------------------------------------------------
    /// <summary>A new outermost load: the previous window ends (its GPU create never came), the accumulators restart.</summary>
    private void hc_win_open() {
        hc_win_close(false);
        t_hc_win = 1; t_hc_miss = false; t_hc_open = t_hc_read = t_hc_readb = t_hc_alloc = t_hc_alloc_load = 0; t_hc_slot = -1; t_hc_c = 0;
        t_hc_res_open = FhUtil.get_at<uint>(0x153CD44);   // HC-T-1: live, on the loading thread (a game global, always mapped)
        t_hc_hdr_off = 0; t_hc_hdr_got = 0; t_hc_hdr_mark = t_hc_hdr_ib = t_hc_hdr_vb = t_hc_hdr_tex = 0;
    }
    /// <summary>The window ends: at the cluster's GPU create (bytes through it) or at the next load (bytes of the load only).
    /// Two sizes per held cluster: the GAME-POOL bytes it pins (the pool hooks' net bytes, only with diag_cluster_bytes; else
    /// unmeasured) and its VRAM bytes (the Phyre header's texture + vertex + index buffer sizes, else the file bytes read).</summary>
    private void hc_win_close(bool gpu_done) {
        if (t_hc_win == 0) return;
        bool alloc_ok = _ld_bytes_q && (seen_mask & 4096) != 0;
        long pool = alloc_ok ? Math.Max(0L, gpu_done ? t_hc_alloc : t_hc_alloc_load) : -1;
        bool hdr = t_hc_hdr_got == 15 && t_hc_hdr_mark == 0x50485952;
        long hsum = (long)t_hc_hdr_tex + t_hc_hdr_vb + t_hc_hdr_ib;
        // HC-T-6: a misattributed field (a seek, another stream) must not become one cluster's VRAM: each <= 512 MB, and the sum
        // at most 4x the file bytes read (a DDS file is about its texture's size); else the file bytes stand in
        if (hdr && (t_hc_hdr_tex > (512u << 20) || t_hc_hdr_vb > (512u << 20) || t_hc_hdr_ib > (512u << 20) || (t_hc_readb > 0 && hsum > 4 * t_hc_readb))) { hdr = false; if (t_hc_was_miss) lt_add(LT.hdr_bad, 1); }
        long vram = hdr ? hsum : t_hc_was_miss && t_hc_readb > 0 ? t_hc_readb : -1;
        if (t_hc_was_miss) { if (alloc_ok) lt_add(LT.alloc_b, pool); if (hdr) { lt_add(LT.hdr_ok, 1); lt_add(LT.hdr_vram_b, vram); } }
        int s = t_hc_slot; nint c = t_hc_c;
        if (s >= 0 && t_hc_was_miss && System.Threading.Volatile.Read(ref _hc_state[s]) == 2 && _hc_ptr[s] == c) {
            _hc_bytes[s] = pool < 0 ? -1 : (int)Math.Min(pool, int.MaxValue);
            _hc_vram[s] = vram < 0 ? -1 : (int)Math.Min(vram, int.MaxValue);
        }
        t_hc_win = 0; t_hc_slot = -1;
    }
    /// <summary>The Phyre cluster header fields by stream offset (+0 marker 0x50485952, +0x48 index, +0x4C vertex, +0x50 max
    /// texture buffer size; Fahrenheit petypes.g.cs PClusterHeaderD3D11), whatever the read sizes: `n` bytes were just read
    /// into `buf` at stream offset t_hc_hdr_off. A field split across two reads is missed (the file bytes stand in).</summary>
    private void hc_header_bytes(nint buf, uint n) {
        if (t_hc_win != 1 || buf == 0 || n == 0) return;
        long off = t_hc_hdr_off; t_hc_hdr_off = off + n;
        if (off >= 0x54) return;
        uint v;
        if (hc_hdr_at(buf, off, n, 0x00, out v)) { t_hc_hdr_mark = v; t_hc_hdr_got |= 1; }
        if (hc_hdr_at(buf, off, n, 0x48, out v)) { t_hc_hdr_ib = v;   t_hc_hdr_got |= 2; }
        if (hc_hdr_at(buf, off, n, 0x4C, out v)) { t_hc_hdr_vb = v;   t_hc_hdr_got |= 4; }
        if (hc_hdr_at(buf, off, n, 0x50, out v)) { t_hc_hdr_tex = v;  t_hc_hdr_got |= 8; }
    }
    private bool hc_hdr_at(nint buf, long off, uint n, int field, out uint v) {
        v = 0;
        if (field < off || field + 4 > off + n) return false;
        return read((byte*)buf + (field - off), out v);
    }
    /// <summary>After an outermost load returned r: hit or miss, the segments, then the cache's acquire (or a re-hit).</summary>
    private void hc_after_load(dl_pcl o, nint self, nint path, nint r, long dt) {
        bool miss = _ld_open_q && (seen_mask & 512) != 0 ? t_hc_miss : dt * 10000 >= System.Diagnostics.Stopwatch.Frequency;   // without the open hook: the 0.1 ms rule
        t_hc_was_miss = miss; t_hc_alloc_load = t_hc_alloc;
        if (miss) { lt_add(LT.pcl_miss_n, 1); lt_add(LT.pcl_miss_t, dt); lt_add(LT.open_t, t_hc_open); lt_add(LT.read_t, t_hc_read); lt_add(LT.read_b, t_hc_readb); }
        t_hc_c = r; t_hc_win = r != 0 ? 2 : 0;
        if (r == 0) return;
        int s = hc_find(r);
        if (s >= 0) {   // already held: refresh its stamp; a hit only our reference made possible is one the cache saved
            _hc_stamp[s] = System.Threading.Volatile.Read(ref _hc_stamp_now); lt_add(LT.hc_held_hits, 1);
            if (_hc_orphan[s] != 0) { _hc_orphan[s] = 0; lt_add(LT.hc_saved, 1); }
            return;
        }
        byte* p = stackalloc byte[k_hc_path];
        if (!hc_copy_path(path, p)) return;
        int el = hc_eligible(p);
        if (el != 0) lt_add(LT.elig_n, 1);   // HC-13: counted before the admission gates, for clusters not yet held (the held hit returned above)
        hc_sample(p, el);                     // HC-7: a handful of sample paths with their verdict, spread across the session
        if (el == 0) return;
        int mode = _hc_mode;
        if (!_hd_cluster_cache || !_hc_ready || _hc_disabled || mode == 0 || (mode == 2 && el != 2) || !_hc_admit_ok) return;
        // HC-T-1: only while the pool's transient growth block is down for this whole load: its reserved size at the window's
        // open and now must both be the session base. Residue placed in the transient block would keep the game from freeing it.
        long rb = System.Threading.Volatile.Read(ref _hc_res_base), rnow = FhUtil.get_at<uint>(0x153CD44);
        if (rb < 0 || t_hc_res_open != rb || rnow != rb) { lt_add(LT.hc_skip_up, 1); return; }
        if (System.Threading.Interlocked.CompareExchange(ref _hc_adding, 1, 0) != 0) return;   // another thread is acquiring: skip, never wait
        try {
            if (hc_find(r) >= 0) return;   // another thread took it meanwhile: one held reference per cluster
            long held = hc_held(out int n, out _), vheld = hc_held_vram(out _, out _);
            if (n >= k_hc_max || held >= (long)_hc_cap_kb << 10 || vheld >= (long)_hc_vcap_mb << 20) return;   // both budgets
            int slot = hc_reserve();
            if (slot < 0) return;
            nint c2;
            t_hc_extra = 1;
            try { c2 = o(self, path); }   // the same chain once more: a hit (refcount + 1), no I/O
            finally { t_hc_extra = 0; }
            if (c2 == 0) { System.Threading.Volatile.Write(ref _hc_state[slot], 0); return; }   // cannot happen for a hit; nothing taken
            int at = slot * k_hc_path;
            for (int i = 0; i < k_hc_path; i++) _hc_path[at + i] = p[i];
            _hc_ptr[slot] = c2; _hc_bytes[slot] = -1; _hc_vram[slot] = -1; _hc_orphan[slot] = 0; _hc_stamp[slot] = System.Threading.Volatile.Read(ref _hc_stamp_now);
            System.Threading.Volatile.Write(ref _hc_state[slot], 2);   // published: the path and pointer are visible before the state
            lt_add(LT.hc_added, 1);
            t_hc_slot = c2 == r ? slot : -1;   // its bytes come at the window's end (the miss's own allocations)
        }
        finally { System.Threading.Volatile.Write(ref _hc_adding, 0); }
    }
    /// <summary>HC-7: a spread of sample paths with their verdict, for the render thread to log with their fios form. Every
    /// 64th path, plus the first few .dds.phyre the filter rejects (so a wrong directory filter is visible). Loading thread.</summary>
    private void hc_sample(byte* p, int el) {
        int tick = System.Threading.Interlocked.Increment(ref _hc_samp_tick) - 1;
        bool reject_dds = el == 0 && hc_ends_dds(p);
        if ((tick & 63) != 0 && !(reject_dds && System.Threading.Volatile.Read(ref _hc_samp_w) < 4)) return;
        int i = System.Threading.Interlocked.Increment(ref _hc_samp_w) - 1;   // the render thread drains up to k_hc_samp behind
        int at = (i % k_hc_samp) * k_hc_path;
        for (int k = 0; k < k_hc_path; k++) _hc_samp[at + k] = p[k];
        _hc_samp_verdict[i % k_hc_samp] = el;
    }
    private static bool hc_ends_dds(byte* p) { int n = 0; while (n < k_hc_path - 1 && p[n] != 0) n++; return hc_ends(p, n, ".dds.phyre"u8); }
    private int hc_find(nint c) {
        int hi = System.Threading.Volatile.Read(ref _hc_hi);
        for (int s = 0; s < hi; s++) if (System.Threading.Volatile.Read(ref _hc_state[s]) == 2 && _hc_ptr[s] == c) return s;
        return -1;
    }
    /// <summary>A free slot, marked "being filled" (only the acquire, under _hc_adding, raises _hc_hi).</summary>
    private int hc_reserve() {
        for (int s = 0; s < k_hc_max; s++) {
            if (System.Threading.Volatile.Read(ref _hc_state[s]) != 0 || System.Threading.Interlocked.CompareExchange(ref _hc_state[s], 1, 0) != 0) continue;
            if (s >= _hc_hi) System.Threading.Volatile.Write(ref _hc_hi, s + 1);
            return s;
        }
        return -1;
    }
    /// <summary>The held clusters' GAME-POOL bytes (an unmeasured one counts as k_hc_pool_est).</summary>
    private long hc_held(out int n, out int unknown) {
        n = 0; unknown = 0; long sum = 0;
        int hi = System.Threading.Volatile.Read(ref _hc_hi);
        for (int s = 0; s < hi; s++) {
            if (System.Threading.Volatile.Read(ref _hc_state[s]) != 2) continue;
            n++; int b = _hc_bytes[s];
            if (b >= 0) sum += Math.Max(b, k_hc_pool_est); else { sum += k_hc_pool_est; unknown++; }   // HC-T-5: a window's net bytes can cancel to 0
        }
        return sum;
    }
    /// <summary>The held clusters' VRAM bytes (an unmeasured one counts as the average of the measured, or k_hc_vram_est).</summary>
    private long hc_held_vram(out int unknown, out long avg) {
        unknown = 0; long known = 0; int kn = 0;
        int hi = System.Threading.Volatile.Read(ref _hc_hi);
        for (int s = 0; s < hi; s++) {
            if (System.Threading.Volatile.Read(ref _hc_state[s]) != 2) continue;
            int b = _hc_vram[s];
            if (b >= 0) { known += b; kn++; } else unknown++;
        }
        avg = kn > 0 ? known / kn : k_hc_vram_est;
        return known + unknown * avg;
    }
    private long hc_slot_bytes(int s) { int b = _hc_bytes[s]; return Math.Max(b, k_hc_pool_est); }   // HC-T-5
    private long hc_slot_vram(int s, long avg) { int b = _hc_vram[s]; return b >= 0 ? b : avg; }
    /// <summary>The path as the detour received it (guarded read, NUL-terminated within 255 bytes).</summary>
    private bool hc_copy_path(nint path, byte* dst) {
        if (path == 0 || _fn_safe_read == null) return false;
        if (_fn_safe_read((void*)path, dst, k_hc_path) == 0) {   // the string may end a region: byte by byte up to its NUL
            int i = 0;
            for (; i < k_hc_path - 1; i++) { if (_fn_safe_read((byte*)path + i, dst + i, 1) == 0) return false; if (dst[i] == 0) break; }
            dst[i] = 0;
        }
        dst[k_hc_path - 1] = 0;
        return dst[0] != 0;
    }
    /// <summary>The phase-1 filter on the path (ASCII case-insensitive): 0 no, 1 an eligible texture, 2 an eligible /magic/ texture.</summary>
    private static int hc_eligible(byte* p) {
        int n = 0; while (n < k_hc_path - 1 && p[n] != 0) n++;
        if (!hc_ends(p, n, ".dds.phyre"u8)) return 0;
        if (hc_has(p, n, "/menu/"u8) || hc_has(p, n, "/shaders/"u8) || hc_has(p, n, "/event/"u8)) return 0;
        if (hc_has(p, n, "/magic/"u8)) return 2;
        return hc_has(p, n, "/map/"u8) || hc_has(p, n, "/chr/"u8) ? 1 : 0;   // HC-7: /btlmap/ does not exist; battle maps live under /map/
    }
    private static byte lc(byte b) => b >= (byte)'A' && b <= (byte)'Z' ? (byte)(b + 32) : b;
    private static bool hc_at(byte* p, int at, ReadOnlySpan<byte> pat) { for (int i = 0; i < pat.Length; i++) if (lc(p[at + i]) != pat[i]) return false; return true; }
    private static bool hc_ends(byte* p, int n, ReadOnlySpan<byte> pat) => n >= pat.Length && hc_at(p, n - pat.Length, pat);
    private static bool hc_has(byte* p, int n, ReadOnlySpan<byte> pat) { for (int at = 0; at + pat.Length <= n; at++) if (hc_at(p, at, pat)) return true; return false; }

    // -- the main thread ------------------------------------------------------------------------------------------------------
    /// <summary>At the map load request 0x909A20 (ld_map_request): what its nested map free will mean for the cache.</summary>
    private void hc_map_request(int kind, int map, sbyte bst, int field, int btlmap) {
        _hc_p_valid = true;
        if (bst == 9 && (kind == 0x13 || kind == 0x1A) && map == btlmap) {   // the battle's own map load (LD-1: state 9 at this call)
            bool random = _ld_kind == 1 && _ld_kind_tear == _ld_tear_seq;     // LD-3: a random encounter started since the last teardown
            _hc_p_release = _hc_zone >= 0 && field != _hc_zone; _hc_p_zone = field; _hc_p_mode = random ? 1 : 2;
            _hc_p_label = random ? (_hc_p_release ? "ENTRY (random battle; the field map changed: release all)" : "ENTRY (random battle)")
                                 : (_hc_p_release ? "ENTRY (script battle; the field map changed: release all)" : "ENTRY (script battle: keep, admit /magic/ only)");
        }
        else if (bst == 0) {   // 0x909A20 at battle_state 0 is MAPREG, the way back (its only other caller)
            bool same = _hc_zone >= 0 && map == _hc_zone;
            _hc_p_release = !same; _hc_p_zone = map; _hc_p_mode = 1;
            _hc_p_label = same ? "EXIT (back to the same field map)" : "ZONE (the way back loads another map)";
        }
        else { _hc_p_release = true; _hc_p_zone = -1; _hc_p_mode = 1; _hc_p_label = I($"ZONE (a map load at battle_state {bst})"); }
    }

    /// <summary>Right after the game's map free returned (hl_mapfree, main thread): the releases, the budget, the next admission
    /// mode, the orphan scan and the HDC: line. mine = inside our 0x909A20 detour (else an area change, save load, title, event).</summary>
    private void hc_after_free(bool mine) {
        bool rel; int zone, mode; string label;
        if (mine && _hc_p_valid) { rel = _hc_p_release; zone = _hc_p_zone; mode = _hc_p_mode; label = _hc_p_label; }
        else { rel = true; zone = -1; mode = 1; label = "ZONE (area change, save load, title or event map)"; }
        _hc_p_valid = false;
        int tid = Environment.CurrentManagedThreadId;
        bool on_main = tid != _ld_render_mtid && (_ld_main_tid == 0 || tid == _ld_main_tid);   // HC-5 / HC-R-5: never the render thread; the main thread as captured once at 0x909A20
        int stamp = _hc_stamp_now + 1; System.Threading.Volatile.Write(ref _hc_stamp_now, stamp);
        uint* v = stackalloc uint[4]; v[0] = v[1] = v[2] = v[3] = 0;
        int low = -1; var vs = _fn_va_stats; if (vs != null) { vs(v); low = (int)v[2]; }   // MB: the largest free block below 2 GB
        var vrf = _fn_va_regions; if (vrf != null) { uint* vr2 = stackalloc uint[2]; vr2[0] = vr2[1] = 0; vrf(null, 0, null, vr2); _hc_vram_mb = (int)vr2[0]; _hc_vram_budget_mb = (int)vr2[1]; }   // HC-R-6: current
        long res = FhUtil.get_at<uint>(0x153CD44), com = FhUtil.get_at<uint>(0x153CD48);   // the game pool's reserved / committed (Fahrenheit's FhMallocModule)
        long held = hc_held(out int n, out int pool_unk);                 // game-pool bytes the held set pins
        long vheld = hc_held_vram(out int vram_unk, out long vavg);       // VRAM bytes the held set keeps
        // the manager's entry pool, copied once before any release: HC-1's pointer-in-pool fallback reads it, and it gives the orphan scan
        nint mgr = FhUtil.get_at<nint>(0x8CCA4C);   // the ClusterManager singleton (the this of every loadPCluster)
        int in_use = hc_scan(mgr, out int orphans, out long orphan_bytes);

        // The pool's reserved size. base = the lowest seen (the pool without its transient growth block); hw = the highest seen
        // while the cache held nothing (the transient 256 -> 384 MB block at battle-entry frees is normal, cache or not).
        if (_hc_res_base < 0 || res < _hc_res_base) System.Threading.Volatile.Write(ref _hc_res_base, res);
        if (_hc_res_hw < 0 || (n == 0 && res > _hc_res_hw)) _hc_res_hw = res;
        bool transient_up = res > _hc_res_base;
        _hc_transient_up = transient_up; _hc_low_last = low;
        if (n == 0 && !transient_up && low >= 0) _hc_low_empty = low;   // the reference: nothing held, transient block down

        // HC-R-9 brake, retuned (HC-T-3): only a reserved size above base + one 128 MB block at a map free, or the armed settled
        // check (reserved still above base after the way back settled, while holding; HC-T-2), is a growth we may have caused.
        // The first: release all and brake until the next zone change; a second one in the session: disabled.
        bool is_zone = label.StartsWith("ZONE", StringComparison.Ordinal), braked_before = _hc_brake;
        bool req = _hc_brake_req; _hc_brake_req = false;
        long grow_at = _hc_res_base + k_hc_growth_step;   // HC-T-3: one known transient block above the base is normal, cache or not
        bool grew = !_hc_disabled && n > 0 && (res > grow_at || req);
        string grew_why = !grew ? "" : res > grow_at ? I($"the game pool's reserved {res >> 20} MB is above base + 128 MB ({grow_at >> 20} MB; empty-cache high {_hc_res_hw >> 20} MB) while the cache held {n} clusters") : _hc_brake_req_why;
        if (grew) {
            _hc_growths++;
            if (_hc_growths >= 2) { hc_disable(grew_why + " (the second time this session)"); _hc_brake = false; }
            else { _hc_brake = true; _hc_brake_reason = grew_why; }
            hc_msg(I($"HDC: BRAKE: {grew_why}: releasing everything now; {(_hc_disabled ? "a second growth: the cache is DISABLED for the session" : "admission paused until the next zone change")}"), false);
        }
        if (grew && is_zone && _hc_brake) { _hc_brake = false; hc_msg("HDC: the brake was applied at a zone change, which releases everything anyway: admission re-armed for the new zone (HC-T-7)", false); }
        else if (braked_before && is_zone) { _hc_brake = false; hc_msg(I($"HDC: brake released at a zone change (after: {_hc_brake_reason}); admission re-armed (one more growth while holding disables the cache)"), false); }

        // Gates and caps.
        bool low_ok = low >= k_hc_low_need_mb;   // admission: the game has its 128 MB room (HC-T-1: no "transient block up" exception; the acquire also checks the live counter)
        bool low_drop = !transient_up && low >= 0 && low < k_hc_low_need_mb;              // drop all: the room is gone with the block down
        bool vram_ok = _hc_vram_budget_mb <= 0 || _hc_vram_mb < (long)_hc_vram_budget_mb * k_hc_vram_stop_pct / 100;
        // HC-T-8: the pool cap from the base pool only (computed when the transient block is down, held over block-up frees),
        // and eviction only past cap + 1/8 (admission stops at the cap): no admit-at-one-free, evict-at-the-next swing
        if (!transient_up || _hc_pool_cap_hold < 0) _hc_pool_cap_hold = Math.Min(k_hc_pool_cap, Math.Max(0L, (res - com + held) / 2));
        long pool_cap = _hc_pool_cap_hold, pool_evict = pool_cap + pool_cap / 8;
        long vram_cap = _hc_vram_budget_mb > 0 ? Math.Min(k_hc_vram_cap, ((long)_hc_vram_budget_mb << 20) * k_hc_vram_cap_pct / 100) : k_hc_vram_cap;
        bool on = _hd_cluster_cache && _hc_ready && !_hc_disabled && !_hc_brake;
        string why = ""; bool all = false;
        if (n > 0) {
            if (!_hd_cluster_cache || !_hc_ready) { all = true; why = "the cache is off"; }
            else if (_hc_disabled) { all = true; why = "disabled for the session: " + _hc_off_reason; }
            else if (grew || _hc_brake) { all = true; why = "brake: " + (grew ? grew_why : _hc_brake_reason); }
            else if (rel) { all = true; why = label; }
            else if (low < 0) { all = true; why = "the address space was not measured"; }
            else if (low_drop) { all = true; why = I($"the largest free block below 2 GB is {low} MB (< {k_hc_low_need_mb}) with the pool's transient block down"); }
        }
        int* pick = stackalloc int[k_hc_max]; byte* taken = stackalloc byte[k_hc_max]; int np = 0;
        for (int s = 0; s < k_hc_max; s++) taken[s] = 0;
        int hi = System.Threading.Volatile.Read(ref _hc_hi);
        if (all) { for (int s = 0; s < hi; s++) if (System.Threading.Volatile.Read(ref _hc_state[s]) == 2) pick[np++] = s; }
        else if (on && (held > pool_evict || vheld > vram_cap)) {   // rule 4: oldest stamp first, until under both caps
            long left = held, vleft = vheld;
            why = I($"over a cap (pool {held >> 10} KB / {pool_cap >> 10} KB, vram {vheld >> 20} / {vram_cap >> 20} MB): oldest first");
            while ((left > pool_cap || vleft > vram_cap) && np < k_hc_max) {
                int best = -1;
                for (int s = 0; s < hi; s++) {
                    if (taken[s] != 0 || System.Threading.Volatile.Read(ref _hc_state[s]) != 2) continue;
                    if (best < 0 || _hc_stamp[s] - _hc_stamp[best] < 0) best = s;
                }
                if (best < 0) break;
                taken[best] = 1; pick[np++] = best; left -= hc_slot_bytes(best); vleft -= hc_slot_vram(best, vavg);
            }
        }
        int released = 0, mism0 = _hc_mism, byp0 = _hc_by_ptr; long rel_bytes = 0, rel_vram = 0; bool flush_ok = true;
        if (np > 0) {
            if (on_main && hc_render_flush()) {
                byte* norm = stackalloc byte[k_hc_path];
                for (int k = 0; k < np; k++) { long b = hc_slot_bytes(pick[k]), vb = hc_slot_vram(pick[k], vavg); if (hc_release_slot(pick[k], mgr, norm)) { released++; rel_bytes += b; rel_vram += vb; } }
                _hc_reason = I($"{why}: released {released} (pool {rel_bytes >> 10} KB, vram {rel_vram >> 20} MB) at map free #{stamp}{(_hc_by_ptr != byp0 ? I($"; {_hc_by_ptr - byp0} by pointer (name check failed)") : "")}{(_hc_mism != mism0 ? I($"; {_hc_mism - mism0} forgotten (pointer gone)") : "")}");
            }
            else { flush_ok = false; _hc_flush_fail++; _hc_reason = I($"{why}: NOT released at map free #{stamp} ({(on_main ? "the render context could not be read for the flush" : "not on the main thread (HC-5)")}; kept until the next map free)"); }
        }
        // HC-14: with a release outstanding (the flush could not run) the release-all rules can't act, so freeze admission until it does
        bool open = on && flush_ok && vram_ok && low_ok;
        _hc_cap_kb = open ? (int)(pool_cap >> 10) : 0;
        _hc_vcap_mb = open ? (int)(vram_cap >> 20) : 0;
        _hc_admit_ok = low_ok;
        _hc_mode = mode; _hc_zone = zone;
        long held2 = hc_held(out int n2, out _), vheld2 = hc_held_vram(out _, out _);
        _hc_held_n = n2; _hc_held_kb = (int)(held2 >> 10); _hc_held_vram_mb = (int)(vheld2 >> 20); _hc_orphan_n = orphans;
        string adm = _hc_disabled ? "disabled for the session" : _hc_brake ? "braked until the next zone change" : !on ? "off" : !flush_ok ? "closed (a release is outstanding)"
                   : !vram_ok ? I($"closed (vram use {_hc_vram_mb} MB > {k_hc_vram_stop_pct} % of {_hc_vram_budget_mb} MB)")
                   : !low_ok ? I($"closed (largest free block below 2 GB {low} MB < {k_hc_low_need_mb})")
                   : mode == 2 ? "/magic/ only" : "open";
        // the held set is reported as it was at this map free BEFORE the releases (session 1's "0 measured" was the post-release count), then after
        hc_msg(I($"HDC: map free #{stamp}: {label} | hd_cluster_cache {(_hc_disabled ? "DISABLED" : _hc_brake ? "BRAKED" : on ? "on" : _hd_cluster_cache ? "on but not ready" : "off")}: held before {n} (pool {held >> 10} KB, {n - pool_unk} measured; vram {vheld >> 20} MB, {n - vram_unk} measured; {orphans} held only by the cache), released {released}{(np > 0 ? " (" + why + ")" : "")}, held after {n2} (pool {held2 >> 10} KB, vram {vheld2 >> 20} MB)")
             + I($" | caps pool {pool_cap >> 10} KB, vram {vram_cap >> 20} MB; admission {adm} | low 2 GB largest free {low} MB (need {k_hc_low_need_mb}; with nothing held {_hc_low_empty} MB), pool reserved {res >> 20} MB (base {_hc_res_base >> 20}, empty-cache high {_hc_res_hw >> 20}, transient block {(transient_up ? "UP" : "down")}) committed {com >> 20} MB | vram {_hc_vram_mb} / {_hc_vram_budget_mb} MB | ClusterManager entries {(in_use < 0 ? "?" : in_use.ToString(System.Globalization.CultureInfo.InvariantCulture))} of 1024"),
               is_zone);
        hc_regions(label);   // HC-6: the patch-free region diff (main thread)
    }
    /// <summary>Render thread, when the way back has settled (the loader idle on the field, or ld_present's 60 s timeout).
    /// HC-T-2: logged on every way back, cache on or off. The pool's reserved size should be back at its base (the transient
    /// growth block freed). A way back with nothing held that settles at the base arms the test; once armed, a way back that
    /// stays above the base while holding asks the next map free (main thread) to brake and closes admission at once.
    /// HC-T-4: a way back with nothing held re-bases to its reserved size (a permanent growth by the game itself).</summary>
    private void hc_settled(bool timeout) {
        long b = System.Threading.Volatile.Read(ref _hc_res_base); if (b < 0) return;
        hc_held(out int n, out _);
        long res = FhUtil.get_at<uint>(0x153CD44);
        bool pass = res == b;
        string act = "";
        if (n == 0) {
            if (pass) { if (!_hc_settled_armed) { _hc_settled_armed = true; act = "; baseline taken: the test is armed"; } }
            else { System.Threading.Volatile.Write(ref _hc_res_base, res); act = I($"; nothing held: re-based to {res >> 20} MB (HC-T-4)"); }
        }
        else if (!pass && _hc_settled_armed && !_hc_brake_req) {
            _hc_brake_req_why = I($"the game pool stayed at {res >> 20} MB reserved (base {b >> 20} MB) after the way back settled, with {n} clusters held");
            _hc_brake_req = true; _hc_cap_kb = 0; _hc_vcap_mb = 0;   // close admission now; the next map free releases and brakes
            act = "; BRAKE requested (admission closed now, release at the next map free)";
        }
        else if (!pass) act = "; not armed yet (no cache-off baseline): no brake";
        d7_log(I($"HDC: settled after the way back{(timeout ? " (60 s timeout)" : "")}: reserved {res >> 20} MB (base {b >> 20} MB) {(pass ? "PASS" : "above base")}, held {n}{act}"));
    }
    /// <summary>HC-1 / HC-3: stop admitting for the rest of the session, once, with a reason.</summary>
    private void hc_disable(string reason) { if (_hc_disabled) return; _hc_disabled = true; _hc_off_reason = reason; }
    /// <summary>HC-6: the native region diff into a line and the vram readout, on the main thread (it already walks this range).</summary>
    private void hc_regions(string tag) {
        var f = _fn_va_regions; if (f == null) return;
        byte* buf = stackalloc byte[3600]; uint* vr2 = stackalloc uint[2]; vr2[0] = vr2[1] = 0;
        int tn = Math.Min(tag.Length, 80); byte* tb = stackalloc byte[tn + 1];
        for (int i = 0; i < tn; i++) { char ch = tag[i]; tb[i] = ch >= 32 && ch < 127 ? (byte)ch : (byte)'?'; } tb[tn] = 0;
        int len = f(buf, 3600, tb, vr2);
        _hc_vram_mb = (int)vr2[0]; _hc_vram_budget_mb = (int)vr2[1];   // for the NEXT map free's vram gate
        if (len > 0) { var sb = new System.Text.StringBuilder(len); for (int i = 0; i < len && buf[i] != 0; i++) sb.Append((char)buf[i]); hc_msg(sb.ToString(), false); }
    }
    /// <summary>What 0x671300 does before its releases: submit the context's partly recorded list (ctx->vtbl[2], 0x671347),
    /// then the full render flush 0x5BC3F0 (0x671350). Main thread only (never the render thread: it would wait for itself).</summary>
    private bool hc_render_flush() {
        if (_hc_flush == null) return false;
        nint r = FhUtil.get_at<nint>(0x8CB9D8);   // the renderer (0x670FE0 / 0x671300's ECX)
        if (r == 0 || !read((byte*)r + 0xD0, out nint ctx) || ctx == 0 || !read((byte*)ctx, out nint vt) || vt == 0 || !read((byte*)vt + 8, out nint kick)) return false;
        nint exe = (nint)FhUtil.ptr_at<byte>(0);
        if (kick < exe || kick >= exe + 0x237D000) return false;   // only game code (SizeOfImage)
        ((delegate* unmanaged[Thiscall]<nint, void>)kick)(ctx);
        _hc_flush(ctx);
        return true;
    }
    /// <summary>HC-1: release our one reference by pointer. The name check (getPClusterByName on the fios form of the stored
    /// path, as loadPCluster hashed it) is kept only as a diagnostic. When it fails but the pointer is still in the entry pool
    /// with refcount >= 1 (the pre-release copy in _hc_scan), release by pointer anyway: 0x69BDB0 is a no-op for an absent
    /// pointer and refcount-aware, and only the menu force-unload ignores refcounts (excluded by the filter). The slot is freed
    /// either way; a pointer truly gone is "forgotten". The first mismatch is logged in full.</summary>
    private bool hc_release_slot(int s, nint mgr, byte* norm) {
        if (System.Threading.Interlocked.CompareExchange(ref _hc_state[s], 3, 2) != 2) return false;
        nint c = _hc_ptr[s]; bool ok = false;
        if (c == 0 || mgr == 0) { _hc_no_mgr++; }
        else {
            for (int i = 0; i < k_hc_path; i++) norm[i] = 0;
            fixed (byte* p = &_hc_path[s * k_hc_path]) _hc_fios(p, norm, k_hc_path);
            norm[k_hc_path - 1] = 0;
            nint by_name = _hc_getbyname(mgr, norm);
            if (by_name == c) { _hc_release(mgr, c); ok = true; }
            else {
                int rc = _hc_pool_valid ? hc_pool_rc(c) : -2;   // the pre-release pool copy (hc_scan)
                if (rc >= 1) { _hc_release(mgr, c); ok = true; _hc_by_ptr++; if (!_hc_mism_logged) hc_log_mismatch(s, c, by_name, rc, true); }
                else { _hc_mism++; if (!_hc_mism_logged) hc_log_mismatch(s, c, by_name, rc, false); hc_disable("a held cluster's pointer was gone from the manager pool at release (possible name/hash mismatch)"); }
            }
        }
        _hc_ptr[s] = 0; _hc_bytes[s] = -1; _hc_vram[s] = -1; _hc_orphan[s] = 0;
        System.Threading.Volatile.Write(ref _hc_state[s], 0);
        return ok;
    }
    /// <summary>HC-1: the refcount of pointer c in the pre-release pool copy, or -1 if absent.</summary>
    private int hc_pool_rc(nint c) {
        fixed (byte* b = _hc_scan) for (int i = 0; i < 1024; i++) { if (b[0x6000 + i] == 0) continue; if (*(nint*)(b + i * 0x18) == c) return *(int*)(b + i * 0x18 + 4); }
        return -1;
    }
    /// <summary>HC-1: the first release name-check mismatch, in detail: the raw and fios forms, their hashes, and what getByName returned.</summary>
    private void hc_log_mismatch(int s, nint c, nint by_name, int rc, bool recovered) {
        _hc_mism_logged = true;
        var raw = new System.Text.StringBuilder(); var nm = new System.Text.StringBuilder();
        fixed (byte* b = _hc_path) { int at = s * k_hc_path; for (int i = 0; i < k_hc_path - 1 && b[at + i] != 0; i++) { byte ch = b[at + i]; raw.Append(ch >= 32 && ch < 127 ? (char)ch : '?'); } }
        byte* norm = stackalloc byte[k_hc_path]; for (int i = 0; i < k_hc_path; i++) norm[i] = 0;
        fixed (byte* p = &_hc_path[s * k_hc_path]) _hc_fios(p, norm, k_hc_path); norm[k_hc_path - 1] = 0;
        for (int i = 0; i < k_hc_path - 1 && norm[i] != 0; i++) nm.Append(norm[i] >= 32 && norm[i] < 127 ? (char)norm[i] : '?');
        hc_msg(I($"HDC: release name check failed ({(recovered ? "released by pointer" : "FORGOTTEN; admission disabled")}): ptr 0x{(long)c:X}, getByName returned 0x{(long)by_name:X}, pool refcount {rc} | stored path '{raw}' | fios form '{nm}'"), false);
    }
    /// <summary>The manager's 1024-entry pool (+0x1C, 0x18 each: +0 PCluster*, +4 refcount; +0x601C the used flags), one guarded
    /// copy (kept in _hc_scan for HC-1's release fallback): marks the held clusters only we hold (refcount 1). Entries beyond
    /// 1024 (heap fallback) are not seen. Returns the used count.</summary>
    private int hc_scan(nint mgr, out int orphans, out long orphan_bytes) {
        orphans = 0; orphan_bytes = 0; _hc_pool_valid = false;
        if (mgr == 0 || _fn_safe_read == null) return -1;
        int hi = System.Threading.Volatile.Read(ref _hc_hi);
        for (int s = 0; s < hi; s++) _hc_orphan[s] = 0;
        fixed (byte* b = _hc_scan) {
            if (_fn_safe_read((byte*)mgr + 0x1C, b, 0x6000) == 0 || _fn_safe_read((byte*)mgr + 0x601C, b + 0x6000, 0x400) == 0) return -1;
            _hc_pool_valid = true;
            int used = 0;
            for (int i = 0; i < 1024; i++) {
                if (b[0x6000 + i] == 0) continue;
                used++;
                nint c = *(nint*)(b + i * 0x18); int rc = *(int*)(b + i * 0x18 + 4);
                if (rc != 1 || c == 0) continue;
                int s = hc_find(c);
                if (s >= 0) { _hc_orphan[s] = 1; orphans++; int bb = _hc_bytes[s]; if (bb > 0) orphan_bytes += bb; }
            }
            return used;
        }
    }
    private void hc_msg(string m, bool zone) {
        int i = _hc_msg_w;
        _hc_msg_zone[i & 7] = zone; System.Threading.Volatile.Write(ref _hc_msg[i & 7], m);
        _hc_msg_w = i + 1;   // volatile: published after the line
    }

    // -- the render thread ----------------------------------------------------------------------------------------------------
    /// <summary>From ld_present: the HDC: and region lines of the map frees since the last Present (and the ntdll reservation
    /// report at a zone change when diag_va_log is on), plus the sample paths (HC-7).</summary>
    private void hc_present() {
        int w = _hc_msg_w;
        if (w - _hc_msg_r > 8) _hc_msg_r = w - 8;
        for (; _hc_msg_r < w; _hc_msg_r++) {
            string? m = System.Threading.Volatile.Read(ref _hc_msg[_hc_msg_r & 7]);
            if (m == null) continue;
            d7_log(m);
            if (_hc_msg_zone[_hc_msg_r & 7] && _diag_va_log) va_report("zone change");   // the ntdll log (opt-in); the patch-free region diff is already in the HDC messages
        }
        // HC-7: the sample loadPCluster paths, with their verdict and fios form
        int sw = System.Threading.Volatile.Read(ref _hc_samp_w);
        if (sw - _hc_samp_r > k_hc_samp) _hc_samp_r = sw - k_hc_samp;
        for (; _hc_samp_r < sw; _hc_samp_r++) {
            int slot = _hc_samp_r % k_hc_samp, at = slot * k_hc_path, el = _hc_samp_verdict[slot];
            var raw = new System.Text.StringBuilder(); for (int i = 0; i < k_hc_path - 1 && _hc_samp[at + i] != 0; i++) { byte ch = _hc_samp[at + i]; raw.Append(ch >= 32 && ch < 127 ? (char)ch : '?'); }
            if (raw.Length == 0) continue;
            byte* norm = stackalloc byte[k_hc_path]; for (int i = 0; i < k_hc_path; i++) norm[i] = 0;
            if (_hc_fios != null) { fixed (byte* p = &_hc_samp[at]) _hc_fios(p, norm, k_hc_path); norm[k_hc_path - 1] = 0; }
            var nm = new System.Text.StringBuilder(); for (int i = 0; i < k_hc_path - 1 && norm[i] != 0; i++) nm.Append(norm[i] >= 32 && norm[i] < 127 ? (char)norm[i] : '?');
            string vv = el == 1 ? "eligible" : el == 2 ? "eligible (/magic/)" : "rejected";
            d7_log(I($"HDC: sample loadPCluster path [{vv}] '{raw}' | fios '{nm}'"));
        }
    }
    /// <summary>The native ntdll low-2 GB reservation report (opt-in diag_va_log), with a tag. A no-op when diag_va_log is off.</summary>
    private void va_report(string tag) {
        var f = _fn_va_report; if (f == null) return;
        int n = Math.Min(tag.Length, 120); byte* b = stackalloc byte[n + 1];
        for (int i = 0; i < n; i++) { char ch = tag[i]; b[i] = ch >= 32 && ch < 127 ? (byte)ch : (byte)'?'; }
        b[n] = 0; f(b);
    }
    /// <summary>Debug > Battle loading: the cache's checkbox and status line.</summary>
    private void hc_ui() {
        if (!_hc_ready) ImGui.BeginDisabled(true);
        ImGui.Checkbox("hd_cluster_cache: keep the HD textures of the field and battle maps loaded across random battles in one zone (experimental)", ref _hd_cluster_cache);
        if (!_hc_ready) ImGui.EndDisabled();
        string state = !_hc_ready ? _hc_status : _hc_disabled ? "DISABLED for the session: " + _hc_off_reason
                     : _hc_brake ? "BRAKED until the next zone change: " + _hc_brake_reason : _hd_cluster_cache ? (_hc_growths > 0 ? "on (re-armed after a pool-growth brake; one more disables it)" : "on") : "off";
        string adm = _hc_cap_kb > 0 && _hc_vcap_mb > 0 ? "admission open" : "admission closed";
        ImGui.TextWrapped($"{state}, {adm}: held {_hc_held_n} clusters (VRAM {_hc_held_vram_mb} MB of cap {_hc_vcap_mb} MB; game pool {_hc_held_kb} KB of cap {_hc_cap_kb} KB; {_hc_orphan_n} held only by the cache), vram use {_hc_vram_mb} / {_hc_vram_budget_mb} MB, largest free block below 2 GB {_hc_low_last} MB (transient pool block {(_hc_transient_up ? "up" : "down")}), hits saved (session) {lt(LT.hc_saved)}, skipped while the pool's transient block was up {lt(LT.hc_skip_up)}, settled test {(_hc_settled_armed ? "armed" : "not armed yet")}; last release: {_hc_reason}"
            + ((_hc_mism + _hc_by_ptr + _hc_flush_fail + _hc_no_mgr) > 0 ? $" | released by pointer (name check failed) {_hc_by_ptr}, forgotten {_hc_mism}, no manager {_hc_no_mgr}, releases put off {_hc_flush_fail}" : ""));
        ImGui.TextDisabled($"Held: textures ending .dds.phyre under /map/, /chr/ or /magic/, one extra reference each. Two budgets: VRAM (min 1 GB, {k_hc_vram_cap_pct} % of the budget; no admission above {k_hc_vram_stop_pct} % use) and the game pool (min 8 MB, half its free part). Admission needs a free block of {k_hc_low_need_mb} MB below 2 GB (the game's 128 MB pool growth) and, per texture, the pool's transient growth block down for its whole load. Released by pointer right after a map free: all at a zone change, all when the block is gone with the transient block down, oldest first over a cap; a pool growth beyond base + 128 MB, or the pool not back at its base once the way back settled, brakes it until the next zone change (twice: off for the session). One HDC: line per map free in ffxvr_diag.log.");
    }
}
