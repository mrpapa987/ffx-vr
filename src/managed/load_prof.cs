// SPDX-License-Identifier: MIT

namespace FFXVR;

// Load lock build (design-notes "Load lock: profiler and lock release"; research parse-time.md sections 4 and 5).
// The battle-entry "parse" time of the HD texture clusters is most likely the loader thread waiting for Phyre's global
// render-interface lock (VA 0xC94EE4), which the render thread holds for whole command lists, Present and so our
// xrWaitFrame included. The native helper does the work (ffxvr_native.cpp, "Load lock build"); this side owns the settings,
// the phase and window marks, the two exact probes and the prof block's managed line.
//
//   diag_load_prof (ini, default 2 in this test build; the probes hook at the next launch, the sampler follows the ini at
//     once): 0 off, 1 the native sampler on the loader thread, 2 also the render thread while the loader waits for the
//     lock. One "prof:" block after each "load:" line. Needs diag_load=1 (its hooks give the phases and windows).
//   load_lock_release (ini and overlay, live, never saved: every launch starts at 0): 0 off, 1 the render thread gives the
//     lock up around our xrWaitFrame. Only while a battle load phase is open, only when the render thread holds the lock
//     exactly once, and it is taken back before the game runs again. (LL-1: there is no mode 2: releasing over the game's
//     Present would leave the immediate context unserialised against the main thread's lock holders.)
//
// Probes (FhUtil offsets; queued after every other load hook, as Fahrenheit's commit stops at the first failing one):
//   0x065AB0 the texture section     thiscall int(this, stream, a, b), ret 0xC  (loader; only caller 0x57C07E via the
//                                    thunk 0x465AA0): takes the Phyre lock (0x465AFA) before the mip reads, releases it
//                                    after the creates. Timed inside a loadPCluster window; "before the first read" is the
//                                    lock wait + the 2x temporary buffer's allocation and 0xCD fill.
//   0x18FC20 PTexture2D create       thiscall int(this, data, a, b, c), ret 0x10 (7 callers; only timed inside a window):
//                                    CreateTexture2D with every mip's data, then the SRV; the mip bytes from the PTexture.
// Prologues 55 8B EC 6A FF 68 (SEH frames), no branch into the first 5 bytes, no FFX-60fps signature over them (pt/sig2.py,
// 188 patterns), not hooked by Fahrenheit (only its generated handles name them). Sg_MainLoop and FFX-60fps's 0x781020 /
// 0x861160 are not touched.
public sealed unsafe partial class FFXVRModule {

    private int  _diag_load_prof = 2;          // ini diag_load_prof: 0 off, 1 loader, 2 loader + render thread (on for this test build)
    private int  _load_lock_release = 0;       // ini load_lock_release: 0 off, 1 around our xrWaitFrame (live; LL-2: not saved, 0 at every launch)
    private bool _pr_q;                        // the two probes were queued at launch (diag_load_prof != 0 then)
    private string _pr_status = "not set up";
    private delegate* unmanaged[Cdecl]<int, void> _fn_prof_set, _fn_prof_mark, _fn_prof_phase;
    private delegate* unmanaged[Cdecl]<int, byte*, byte*, void> _fn_prof_report;
    private delegate* unmanaged[Cdecl]<int, int> _fn_lock_release_set;
    private delegate* unmanaged[Cdecl]<byte*, int, int> _fn_lock_release_status;

    private const int kl_plat = 0x65AB0, kl_texc = 0x18FC20;
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] private delegate int dl_plat(nint self, nint a, nint b, nint c);
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] private delegate int dl_texc(nint self, nint data, int a, int b, int c);
    private dl_plat? _hl_plat, _ol_plat; private dl_texc? _hl_texc, _ol_texc;
    private nint _al_plat, _al_texc;

    // per thread (the loading thread): the texture section of the window's cluster
    [ThreadStatic] private static int  t_pr_in_plat;                         // 1 inside 0x465AB0 before its first read, 2 after it
    [ThreadStatic] private static long t_pr_plat0, t_pr_pre_w, t_pr_texc_w;  // its start; this window's "before the first read" and create times

    /// <summary>ini diag_load_prof: 0 / 1 / 2, or off / on (on = 2).</summary>
    private void lp_parse_prof(string k, string v) {
        string t = v.Trim().ToLowerInvariant();
        if (t == "0" || t == "1" || t == "2") _diag_load_prof = t[0] - '0';
        else if (try_bool(t, out bool b)) _diag_load_prof = b ? 2 : 0;
        else _ini_bad.Add($"{k}='{v}'");
    }

    // -- the probes (loading thread; nothing outside a loadPCluster window of this thread) -----------------------------------
    private int hl_plat(nint self, nint a, nint b, nint c) {
        dl_plat o = System.Threading.Volatile.Read(ref _ol_plat) ?? d7_resolve_main(ref _ol_plat, _al_plat, _hl_plat!);
        seen(16384);
        if (t_hc_win != 1 || t_pr_in_plat != 0) return o(self, a, b, c);
        long t0 = now(); t_pr_plat0 = t0; t_pr_in_plat = 1;
        int r;
        try { r = o(self, a, b, c); } finally { t_pr_in_plat = 0; }
        lt_add(LT.pr_plat_n, 1); lt_add(LT.pr_plat_t, now() - t0);
        return r;
    }
    private int hl_texc(nint self, nint data, int a, int b, int c) {
        dl_texc o = System.Threading.Volatile.Read(ref _ol_texc) ?? d7_resolve_main(ref _ol_texc, _al_texc, _hl_texc!);
        seen(32768);
        if (t_hc_win != 1) return o(self, data, a, b, c);   // render targets (main thread), the mip-skip reload, other windows: straight through
        long t0 = now(); int r = o(self, data, a, b, c); long dt = now() - t0;
        lt_add(LT.pr_texc_n, 1); lt_add(LT.pr_texc_t, dt); t_pr_texc_w += dt;
        try { lt_add(LT.pr_texc_b, lp_tex_bytes(self)); } catch { _ld_faults++; }
        return r;
    }
    /// <summary>hl_read, inside a window: the first read inside the texture section ends its "before the first read" part.</summary>
    private void lp_first_read() {
        if (t_pr_in_plat != 1) return;
        long d = now() - t_pr_plat0; t_pr_in_plat = 2; t_pr_pre_w += d;
        lt_add(LT.pr_pre_n, 1); lt_add(LT.pr_pre_t, d);
    }
    /// <summary>The PTexture2D's mip bytes as 0x58FC20 passes them to CreateTexture2D: w +0x1C, h +0x20, mips +0xC (+1; one
    /// when +0x10 is 0 and +0x14 bit 1 clear), the format at [this]: bits per pixel +0xC, block format +0x10 bit 0 (4x4).
    /// Guarded reads; 0 when anything is out of range.</summary>
    private long lp_tex_bytes(nint self) {
        if (!read((byte*)self, out nint fmt) || fmt == 0 || !read((byte*)self + 0xC, out uint mips) || !read((byte*)self + 0x10, out uint f10) || !read((byte*)self + 0x14, out uint f14)
            || !read((byte*)self + 0x1C, out uint w) || !read((byte*)self + 0x20, out uint h) || !read((byte*)fmt + 0xC, out uint bpp) || !read((byte*)fmt + 0x10, out uint ff)) return 0;
        if (w == 0 || h == 0 || w > 16384 || h > 16384 || bpp == 0 || bpp > 128) return 0;
        uint levels = mips + 1; if (f10 == 0 && ((f14 >> 1) & 1) == 0) levels = 1; if (levels > 16) levels = 16;
        bool blk = (ff & 1) != 0; long sum = 0;
        for (int m = 0; m < levels; m++) {
            uint ww = Math.Max(1u, w >> m), hh = Math.Max(1u, h >> m);
            if (blk) { ww = (ww + 3) & ~3u; hh = (hh + 3) & ~3u; }
            sum += (long)bpp * ww * hh / 8;
        }
        return sum;
    }
    /// <summary>hc_after_load, for a miss: this window's split into its file-size bucket (< 1, 1-3, 3-6, >= 6 MB read).</summary>
    private void lp_after_miss(long dt) {
        if (!_pr_q) return;
        long rb = t_hc_readb; int sb = rb < (1L << 20) ? 0 : rb < (3L << 20) ? 1 : rb < (6L << 20) ? 2 : 3;
        int k = (int)LT.pr_s0_n + sb * 4;
        lt_add((LT)k, 1); lt_add((LT)(k + 1), Math.Max(0, dt - t_hc_open - t_hc_read)); lt_add((LT)(k + 2), t_pr_pre_w); lt_add((LT)(k + 3), t_pr_texc_w);
    }

    // -- phases, windows and the report ---------------------------------------------------------------------------------------
    private void lp_phase(int v) { var f = _fn_prof_phase; if (f != null) f(v); }
    private void lp_mark(bool main, int inside) { var f = _fn_prof_mark; if (f != null) f(inside | (main ? 2 : 0)); }
    /// <summary>Render thread, right after a "load:" line: the native prof block (sampler, lock, the lock release) with the
    /// probe line, then the phase ends.</summary>
    private void lp_report(int phase, string label, long[] b) {
        var f = _fn_prof_report;
        if (f != null) {
            string extra = lp_probe_line(b);
            int nl = Math.Min(label.Length, 150), ne = Math.Min(extra.Length, 3000);
            byte* lb = stackalloc byte[nl + 1]; byte* eb = stackalloc byte[ne + 1];
            for (int i = 0; i < nl; i++) { char ch = label[i]; lb[i] = ch >= 32 && ch < 127 ? (byte)ch : (byte)'?'; } lb[nl] = 0;
            for (int i = 0; i < ne; i++) { char ch = extra[i]; eb[i] = (ch >= 32 && ch < 127) || ch == '\n' ? (byte)ch : (byte)'?'; } eb[ne] = 0;
            f(phase, lb, eb);
        }
        lp_phase(-phase);
    }
    private string lp_probe_line(long[] b) {
        long d(LT k) => lt(k) - b[(int)k];
        if (!_pr_q) return "prof:  probes: off (diag_load_prof=0 at launch)";
        if ((seen_mask & (16384 | 32768)) == 0) return "prof:  probes: queued but not called yet (not installed? core.log has Fahrenheit's hook results)";
        long pn = d(LT.pr_plat_n), qn = d(LT.pr_pre_n), cn = d(LT.pr_texc_n), bn = d(LT.pr_big_n);
        string big = _ld_bytes_q && (seen_mask & 4096) != 0 ? I($", of it allocations of 1 MB+ {ms(d(LT.pr_big_t)):F0} ms in {bn}") : ", allocations not timed (diag_cluster_bytes=0)";
        var sb = new System.Text.StringBuilder();
        sb.Append(I($"prof:  probes: texture section 0x465AB0 {pn} calls {ms(d(LT.pr_plat_t)):F0} ms (before its first mip read, i.e. the lock wait + the temporary buffer: {ms(d(LT.pr_pre_t)):F0} ms in {qn}{big}) | texture create 0x58FC20 {cn} calls {ms(d(LT.pr_texc_t)):F0} ms, {d(LT.pr_texc_b) >> 20} MB of mips{(cn > 0 ? I($" ({ms(d(LT.pr_texc_t)) / cn:F1} ms each)") : "")}"));
        sb.Append("\nprof:  misses by file size (mean per miss):");
        string[] lab = { "< 1 MB", "1-3 MB", "3-6 MB", ">= 6 MB" }; bool any = false;
        for (int s = 0; s < 4; s++) {
            int k = (int)LT.pr_s0_n + s * 4; long n = d((LT)k); if (n <= 0) continue;
            sb.Append(I($"{(any ? " |" : "")} {lab[s]}: n={n} parse {ms(d((LT)(k + 1))) / n:F1} ms (texture section before the first read {ms(d((LT)(k + 2))) / n:F1}, create {ms(d((LT)(k + 3))) / n:F1})")); any = true;
        }
        if (!any) sb.Append(" no miss");
        return sb.ToString();
    }

    /// <summary>From push_all, every frame (render thread): both settings to the native side.</summary>
    private void lp_push() {
        var ps = _fn_prof_set; if (ps != null) ps(_diag_load ? _diag_load_prof : 0);   // without diag_load's hooks there is no phase or window to sample
        var ls = _fn_lock_release_set; if (ls != null) ls(_load_lock_release);
    }
    private string lp_status_text() {
        var f = _fn_lock_release_status; if (f == null) return "";
        byte* buf = stackalloc byte[512]; buf[0] = 0; f(buf, 512);
        return Marshal.PtrToStringAnsi((nint)buf) ?? "";
    }
    /// <summary>The main window's row (after the battle transition's).</summary>
    private void lp_ui() {
        ImGui.Text("battle loads, engine lock (load_lock_release):"); ImGui.SameLine();
        ImGui.RadioButton("off##llr", ref _load_lock_release, 0); ImGui.SameLine();
        ImGui.RadioButton("released while waiting for the headset (test)##llr", ref _load_lock_release, 1);
        ImGui.TextDisabled("  not saved: every launch starts off" + (_diag_load ? "" : " | needs diag_load=1 (its hooks open the battle load phases)") + (_fn_lock_release_status != null ? " | " + lp_status_text() : ""));
    }
    /// <summary>Debug > Battle loading: the profiler's switch.</summary>
    private void lp_debug_ui() {
        ImGui.Text("load profiler (diag_load_prof): one \"prof:\" block after each \"load:\" line"); ImGui.SameLine();
        ImGui.RadioButton("off##dlp", ref _diag_load_prof, 0); ImGui.SameLine();
        ImGui.RadioButton("loader thread##dlp", ref _diag_load_prof, 1); ImGui.SameLine();
        ImGui.RadioButton("+ render thread##dlp", ref _diag_load_prof, 2);
        ImGui.TextDisabled($"  the sampler follows this at once; the two exact probes (0x465AB0, 0x58FC20) hook at launch: {(_diag_load ? _pr_status : "off (diag_load=0: no phase, no window, no probe)")}");
    }
}
