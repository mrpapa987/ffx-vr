// SPDX-License-Identifier: MIT

namespace FFXVR;

// 3D damage numbers (roadmap 4.2, 6 Nov 2026; native ffxvr_dmg3d.h, ABI 47). Off by default for this first build (ini dmg3d=0;
// F8 > View: "3D damage numbers", live). The battle's damage / heal / MP digits are drawn at each target in the world, in both
// eyes, with the game's own hop, fade and colour; the four word sprites ("Miss" and the others, texture 600) and the Doom
// countdown stay on the panel (v1).
//   Source: the game's damage-number nodes (pool [0x11360B8], list [0x11360BC]), read on the MAIN thread right where the game
//   has just drawn them: the first job join after the frame counter moved (h7_join, always installed: Sg_MainLoop runs the
//   battle ticks, then the UI with its queue push and draw, then the counter, then that join), with the push's own gates
//   (battle_state != 0 and not 0x1F-0x21, [0x133C948] != 0). One guarded read of the pool (0xA00 B) when the list is not empty,
//   native walks it (ffxvr_dmg_walk), then per target: the anchor as the game computes it (optpos[Chr+0x442] through d7_anchor,
//   lifted by Chr+0x4E8; Chr+0x442 = 0xFF: optpos[1], counted: the game uses 0x794AB0 there) and its screen anchors. Published
//   in a two-slot seqlock with the game-frame stamp; the render thread hands the latest to native once per Present
//   (ffxvr_dmg_set), which keeps it while it is live (C2's rule) and draws it in the marker pass.
//   The flat numbers on the panel stay (the default). dmg3d_hide2d=1 (ini, read at launch for its hook; off by default) hooks the
//   queue draw 0x893290 (its single call site 0x89B3CB, main thread); the detour records there instead, and while native draws
//   the digits (ffxvr_dmg_skip) takes exactly those entries off the queue, only when every entry matches the game's own
//   placement predicted from the nodes (ffxvr_dmg_queue); on any doubt the queue is left as it is.
//   Check of the mapping without the hook: at the join the queue's entries of this frame are still in memory (the draw zeroes
//   only the count), and the same prediction is compared with them (the 10 s line's "queue check").
// Logging: native writes one dmg3d: line per new number (the first 40); here a dmg3d: line every 10 s in battle while on.
public sealed unsafe partial class FFXVRModule {
    private bool  _dmg3d = false;        // ini dmg3d (live)
    private float _dmg_size = 1f;        // ini dmg3d_scale (0.3-3)
    private float _dmg_lift = 0f;        // ini dmg3d_lift (units above the game's anchor, -10..20)
    private bool  _dmg_hide2d = false;   // ini dmg3d_hide2d: hook the queue draw at launch; live once hooked
    private bool  _dmg_hide_at_launch;   // what it was at launch (the hook)

    [StructLayout(LayoutKind.Sequential, Pack = 1)] private struct DmgNode { public byte flags, kind, chr, idx, glyph, delay, counter, len; public short ox, oy; public byte ex, ey, slot, pad; }   // dmg::Node, 16 B
    [StructLayout(LayoutKind.Sequential, Pack = 1)] private struct DmgChr {   // dmg::Chr, 72 B
        public Vector3 opt, tl, td; public float h4e8; public int sx, sy, f44, f48, f4c, f50;
        public byte chr, idx442, src, aflags, ok, p0, p1, p2;
    }
    private delegate* unmanaged[Cdecl]<byte*, uint, uint, DmgNode*, int, uint*, int> _fn_dmg_walk;                       // ABI 47
    private delegate* unmanaged[Cdecl]<byte*, int, int, DmgNode*, int, DmgChr*, int, byte*, uint*, int> _fn_dmg_queue;   // ABI 47
    private delegate* unmanaged[Cdecl]<DmgNode*, int, DmgChr*, int, uint, uint, uint, int, void> _fn_dmg_set;          // ABI 47
    private delegate* unmanaged[Cdecl]<int, float, float, byte*, int> _fn_dmg_cfg;                                   // ABI 47
    private delegate* unmanaged[Cdecl]<int> _fn_dmg_skip;                                                             // ABI 47
    private delegate* unmanaged[Cdecl]<uint*, int, void> _fn_dmg_status;                                              // ABI 47

    private const int k_dmg_pool = 0xD360B8, k_dmg_head = 0xD360BC;    // VA 0x11360B8 / 0x11360BC
    private const int k_dmg_qcount = 0xF3F108, k_dmg_qent = 0xF3F110;  // VA 0x133F108 (s16) / 0x133F110 (16 B x 63)
    private const int k_dmg_table = 0x8424B0;                           // VA 0xC424B0: the hop table, 96 B
    private const int k_dmg_draw = 0x493290;                            // VA 0x893290: the queue draw, cdecl void(); prologue 57 E8
    private readonly byte[] _dmg_table = new byte[96]; private bool _dmg_table_ok;

    // the hide hook (main thread)
    private d7_void? _h_dmg_draw, _o_dmg_draw; private nint _a_dmg_draw; private bool _dmg_hook_ok; private string _dmg_hook_status = "not hooked (dmg3d_hide2d=0 at launch)";
    private int _dmg_draw_calls; private uint _dmg_draw_stamp = uint.MaxValue; private uint _dmg_skip_frame = uint.MaxValue; private bool _dmg_skip_val;

    // snapshots: written on the main thread, read at Present
    private struct DmgHdr { public uint stamp, why, qwhy; public int nn, nc, qmatched, qexpected, qdropped, qat, from_draw, gate; }
    private readonly DmgHdr[] _dmgh = new DmgHdr[2]; private readonly DmgNode[] _dmgn = new DmgNode[2 * 128]; private readonly DmgChr[] _dmgc = new DmgChr[2 * 16]; private int _dmg_seq;
    private uint _dmg_join_stamp = uint.MaxValue; private bool _dmg_published_empty = true;
    // main-thread counters (one writer; read by the render thread for the log)
    private int _dmg_snaps_join, _dmg_snaps_draw, _dmg_reads_failed, _dmg_faults, _dmg_nodes_seen, _dmg_chr_exact, _dmg_chr_ff, _dmg_chr_none;
    private int _dmg_cut_loop, _dmg_cut_outside, _dmg_cut_misaligned, _dmg_cut_free, _dmg_cut_cap, _dmg_cut_full;
    private int _dmg_q_ok, _dmg_q_mismatch, _dmg_q_missing, _dmg_q_other, _dmg_hidden_frames, _dmg_hidden_digits, _dmg_hide_doubt;
    private struct DmgMis { public uint stamp, why; public int at, from_draw; public long game0, game1, ours0, ours1; }   // a mismatch (16 B entries as two longs)
    private readonly DmgMis[] _dmg_mis = new DmgMis[3]; private int _dmg_mis_n, _dmg_mis_logged;

    // render thread
    private DmgHdr _r_dmgh; private readonly DmgNode[] _r_dmgn = new DmgNode[128]; private readonly DmgChr[] _r_dmgc = new DmgChr[16]; private int _r_dmg_seq, _r_dmg_prev, _r_dmg_since = 999;
    private bool _dmg_was_on, _dmg_atlas; private uint _dmg_on_frame; private double _dmg_line_t; private int _dmg_lines; private bool _dmg_hello;
    private readonly uint[] _dmg_st = new uint[18];

    private bool dmg_parse(string k, string v) {
        switch (k) {
            case "dmg3d":        pb(k, v, ref _dmg3d); return true;
            case "dmg3d_scale":  pf(k, v, ref _dmg_size, 0.3f, 3f); return true;
            case "dmg3d_lift":   pf(k, v, ref _dmg_lift, -10f, 20f); return true;
            case "dmg3d_hide2d": pb(k, v, ref _dmg_hide2d); return true;
            default: return false;
        }
    }
    private string dmg_ini_text() => $"dmg3d={(_dmg3d ? 1 : 0)}\ndmg3d_scale={_dmg_size.ToString("0.00", CI)}\ndmg3d_lift={_dmg_lift.ToString("0.0", CI)}\ndmg3d_hide2d={(_dmg_hide2d ? 1 : 0)}\n";

    /// <summary>At module init, after every other hook of ours (Fahrenheit's commit stops at the first failing insertion, so a
    /// refusal here takes nothing else with it): the hop table, and with dmg3d_hide2d=1 the queue draw's hook.</summary>
    private void dmg3d_init() {
        _dmg_hide_at_launch = _dmg_hide2d;
        try { byte* t = FhUtil.ptr_at<byte>(k_dmg_table); for (int i = 0; i < 96; i++) _dmg_table[i] = t[i]; _dmg_table_ok = true; }
        catch (Exception e) { _logger.Warning($"dmg3d: the hop table could not be read ({e.GetType().Name}); the checked copy is used"); }
        if (!_dmg_hide2d) return;
        if (!_native_ok || _fn_dmg_skip == null || _fn_dmg_queue == null) { _dmg_hook_status = "not hooked: the native helper did not load"; return; }
        // the bytes the design checked (push edi; call rel32): anything else (another mod's patch, another exe) is left alone
        byte b0 = 0, b1 = 0;
        try { b0 = FhUtil.get_at<byte>(k_dmg_draw); b1 = FhUtil.get_at<byte>(k_dmg_draw + 1); } catch { }
        if (b0 != 0x57 || b1 != 0xE8) { _dmg_hook_status = I($"not hooked: 0x893290 starts {b0:X2} {b1:X2}, not 57 E8 (patched by another mod, or another exe): the flat numbers stay"); _logger.Warning("dmg3d: " + _dmg_hook_status); return; }
        _h_dmg_draw = h_dmg_draw; new FhCallLocation("FFX.exe", k_dmg_draw).try_resolve(out _a_dmg_draw);
        if (_a_dmg_draw == 0) { _dmg_hook_status = "not hooked: FFX.exe not found"; return; }
        try {
            _dmg_hook_ok = new FhCallHandle<d7_void>(new FhCallLocation((void*)_a_dmg_draw)).hook(this, _h_dmg_draw!);
            _dmg_hook_status = _dmg_hook_ok ? "queue draw 0x893290 queued (\"queued\" is not \"installed\": the readout's call count proves it in a battle)" : "the queue draw hook was REFUSED (another mod hooks 0x893290?): the flat numbers stay";
        }
        catch (Exception e) { _dmg_hook_status = $"the queue draw hook FAILED ({e.GetType().Name}): the flat numbers stay"; }
        _logger.Info("dmg3d: " + _dmg_hook_status);
    }
    /// <summary>Render thread, with diag7_resolve: the detour's original, once (B7-1).</summary>
    private void dmg3d_resolve() {
        if (_h_dmg_draw == null || _o_dmg_draw != null || !_dmg_hook_ok) return;
        try { System.Threading.Volatile.Write(ref _o_dmg_draw, d7_chain(_a_dmg_draw, _h_dmg_draw)); } catch (Exception e) { _logger.Error($"dmg3d: resolving the original: {e}"); }
    }

    // -- main thread ---------------------------------------------------------------------------------------------------
    /// <summary>The queue draw (0x893290, main thread; dmg3d_hide2d=1 at launch). The original always runs, after.</summary>
    private void h_dmg_draw() {
        d7_void o = System.Threading.Volatile.Read(ref _o_dmg_draw) ?? d7_resolve_main(ref _o_dmg_draw, _a_dmg_draw, _h_dmg_draw!);
        _dmg_draw_calls++;
        if (_dmg3d) { try { dmg_record(true); } catch { _dmg_faults++; } }
        o();
    }
    /// <summary>h7_join, after the original: the first join after the frame counter moved, on the main thread.</summary>
    private void dmg3d_join() {
        if (!_dmg3d) return;
        if (_fn_main_note == null || _fn_main_note() == 0) return;
        uint stamp = FhUtil.get_at<uint>(0x1FCBBF0);
        if (stamp == _dmg_join_stamp) return;
        _dmg_join_stamp = stamp;
        if (_dmg_draw_stamp == stamp - 1) return;   // the queue draw's detour recorded this frame, at the draw itself
        try { dmg_record(false); } catch { _dmg_faults++; }
    }
    /// <summary>Native's skip, latched per game frame (main thread).</summary>
    private bool dmg_skip_now() {
        var sk = _fn_dmg_skip; if (sk == null) return false;
        uint f = FhUtil.get_at<uint>(0x1FCBBF0);
        if (f != _dmg_skip_frame) { _dmg_skip_frame = f; _dmg_skip_val = sk() != 0; }
        return _dmg_skip_val;
    }

    /// <summary>One snapshot of the nodes and their targets (main thread). draw: from the queue draw's detour (before the game
    /// draws the queue: the hide may compact it), else from the join (after: the queue's stale entries are checked only).</summary>
    private void dmg_record(bool draw) {
        if (_fn_dmg_walk == null || _fn_safe_read == null) return;
        uint stamp = FhUtil.get_at<uint>(0x1FCBBF0);
        if (draw) _dmg_draw_stamp = stamp;
        sbyte bst = FhUtil.get_at<sbyte>(0xD2A8E0);
        bool gate = bst != 0 && bst != 0x1F && bst != 0x20 && bst != 0x21 && FhUtil.get_at<uint>(0xF3C948) != 0;
        uint pbase = FhUtil.get_at<uint>(k_dmg_pool), head = FhUtil.get_at<uint>(k_dmg_head);
        bool any = gate && pbase != 0 && head != 0;
        if (!any && _dmg_published_empty) return;   // nothing up, and the last snapshot said so already
        int s = (_dmg_seq + 1) & 1;
        ref DmgHdr h = ref _dmgh[s];
        h = default; h.stamp = stamp; h.from_draw = draw ? 1 : 0; h.gate = gate ? 1 : 0;
        if (any) {
            byte* pool = stackalloc byte[0xA00];
            if (_fn_safe_read((void*)(nint)pbase, pool, 0xA00) == 0) { _dmg_reads_failed++; }
            else {
                uint why = 0;
                fixed (DmgNode* nodes = &_dmgn[s * 128]) h.nn = _fn_dmg_walk(pool, pbase, head, nodes, 128, &why);
                h.why = why;
                if ((why & 1) != 0) _dmg_cut_loop++; if ((why & 2) != 0) _dmg_cut_outside++; if ((why & 4) != 0) _dmg_cut_misaligned++;
                if ((why & 8) != 0) _dmg_cut_cap++;  if ((why & 16) != 0) _dmg_cut_free++;   if ((why & 32) != 0) _dmg_cut_full++;
                _dmg_nodes_seen += h.nn;
                nint chrs = FhUtil.get_at<nint>(0xD334D4);
                for (int i = 0; i < h.nn; i++) {
                    int id = _dmgn[s * 128 + i].chr; bool have = false;
                    for (int k = 0; k < h.nc; k++) if (_dmgc[s * 16 + k].chr == id) { have = true; break; }
                    if (have || h.nc >= 16) continue;
                    dmg_chr(chrs, id, ref _dmgc[s * 16 + h.nc]); h.nc++;
                }
                dmg_queue(ref h, s, draw);
            }
        }
        _dmg_published_empty = h.nn == 0;
        if (draw) _dmg_snaps_draw++; else _dmg_snaps_join++;
        System.Threading.Volatile.Write(ref _dmg_seq, _dmg_seq + 1);
    }

    /// <summary>One target: the anchor as 0x79F380 builds it, the roots for the 30 Hz correction, the screen anchors.</summary>
    private void dmg_chr(nint chrs, int id, ref DmgChr c) {
        c = default; c.chr = (byte)id; c.src = 2;
        if (chrs == 0 || id >= 0x1F) { _dmg_chr_none++; return; }   // 0x794020: ids from 0x1F are another table (none seen with numbers)
        byte* cb = (byte*)chrs + id * 0xF90;
        if (!read(cb + 0x442, out byte i442) || !read(cb + 0x4E8, out float h)) { _dmg_chr_none++; return; }
        int* scr = stackalloc int[8];   // +0xF34 .. +0xF50: sx sy, F3C F40 (the cursor's PS2 px), F44 F48, F4C F50
        if (_fn_safe_read(cb + 0xF34, scr, 32) == 0) { _dmg_chr_none++; return; }
        c.idx442 = i442; c.h4e8 = h; c.sx = scr[0]; c.sy = scr[1]; c.f44 = scr[4]; c.f48 = scr[5]; c.f4c = scr[6]; c.f50 = scr[7];
        D7Anchor a = default;
        if (!d7_anchor(chrs, id, i442 != 0xFF ? i442 : 1, ref a)) { _dmg_chr_none++; return; }   // no actor: the game uses 0x794AB0; the number stays on the panel
        c.opt = a.opt; c.tl = a.tl; c.td = a.td; c.aflags = (byte)a.flags; c.ok = 1;
        c.src = (byte)(i442 == 0xFF ? 1 : 0);
        if (c.src == 0) _dmg_chr_exact++; else _dmg_chr_ff++;
    }

    /// <summary>The queue against the nodes: from the draw detour, the live queue (and the hide when native draws the digits);
    /// from the join, the stale entries of this frame (a check only).</summary>
    private void dmg_queue(ref DmgHdr h, int s, bool draw) {
        if (_fn_dmg_queue == null || h.nn == 0) return;
        bool hide = draw && _dmg_hide2d && dmg_skip_now();
        DmgEntry16* q = (DmgEntry16*)FhUtil.ptr_at<byte>(k_dmg_qent);
        int count = draw ? FhUtil.get_at<short>(k_dmg_qcount) : 63;
        if (count < 0 || count > 63) { _dmg_q_other++; return; }
        uint* info = stackalloc uint[12];
        int r;
        fixed (DmgNode* n = &_dmgn[s * 128]) fixed (DmgChr* c = &_dmgc[s * 16]) fixed (byte* t = _dmg_table)
            r = _fn_dmg_queue((byte*)q, count, hide ? 1 : 0, n, h.nn, c, h.nc, _dmg_table_ok ? t : null, info);
        h.qwhy = info[5]; h.qmatched = (int)info[1]; h.qexpected = (int)info[3]; h.qdropped = (int)info[2]; h.qat = (int)info[4];
        if (info[5] == 0) _dmg_q_ok++; else if (info[5] == 1) _dmg_q_mismatch++; else if (info[5] == 2) _dmg_q_missing++; else _dmg_q_other++;
        if (info[5] == 1 && _dmg_mis_n < 3 && h.qat >= 0 && h.qat < 63) {   // the first mismatches, for the log
            ref DmgMis m = ref _dmg_mis[_dmg_mis_n];
            m.stamp = h.stamp; m.why = info[5]; m.at = h.qat; m.from_draw = draw ? 1 : 0;
            m.game0 = q[h.qat].a; m.game1 = q[h.qat].b; m.ours0 = (long)(info[6] | ((ulong)info[7] << 32)); m.ours1 = (long)(info[8] | ((ulong)info[9] << 32));
            System.Threading.Volatile.Write(ref _dmg_mis_n, _dmg_mis_n + 1);
        }
        if (!hide) return;
        if (r >= 0 && r <= count) {
            if (info[2] != 0) { *(short*)FhUtil.ptr_at<byte>(k_dmg_qcount) = (short)r; _dmg_hidden_frames++; _dmg_hidden_digits += (int)info[2]; }
        }
        else _dmg_hide_doubt++;   // nothing changed: the game draws its numbers this frame
    }
    [StructLayout(LayoutKind.Sequential, Pack = 1)] private struct DmgEntry16 { public long a, b; }

    // -- render thread -------------------------------------------------------------------------------------------------
    private bool dmg_read() {
        for (int attempt = 0; attempt < 2; attempt++) {
            int s1 = System.Threading.Volatile.Read(ref _dmg_seq); if (s1 == 0) return false;
            int sl = s1 & 1; DmgHdr h = _dmgh[sl]; int nn = Math.Clamp(h.nn, 0, 128), nc = Math.Clamp(h.nc, 0, 16);
            Array.Copy(_dmgn, sl * 128, _r_dmgn, 0, nn); Array.Copy(_dmgc, sl * 16, _r_dmgc, 0, nc);
            System.Threading.Interlocked.MemoryBarrier();
            if (System.Threading.Volatile.Read(ref _dmg_seq) != s1) continue;
            _r_dmgh = h; _r_dmgh.nn = nn; _r_dmgh.nc = nc; _r_dmg_seq = s1; return true;
        }
        return false;
    }

    /// <summary>push_markers (render thread, every Present): the latest snapshot to native. Off: one clearing call, then nothing.</summary>
    private void dmg3d_push() {
        if (_fn_dmg_set == null || _fn_dmg_cfg == null) return;
        if (!_dmg3d) {
            if (_dmg_was_on) { _fn_dmg_cfg(0, _dmg_size, _dmg_lift, null); _fn_dmg_set(null, 0, null, 0, 0, 0, 0, 999); _dmg_was_on = false; }
            return;
        }
        uint frame = FhUtil.get_at<uint>(0x1FCBBF0);
        if (!_dmg_was_on) { _dmg_was_on = true; _dmg_on_frame = frame; _r_dmg_since = 999; }   // switched on: older snapshots never count
        fixed (byte* t = _dmg_table) _dmg_atlas = _fn_dmg_cfg(1, _dmg_size, _dmg_lift, _dmg_table_ok ? t : null) != 0;
        dmg_read();
        _r_dmg_since = _r_dmg_seq != 0 && _r_dmg_seq != _r_dmg_prev ? 0 : Math.Min(_r_dmg_since + 1, 999); _r_dmg_prev = _r_dmg_seq;
        uint floor = (int)(_dmg_on_frame - _c2_btl_frame) > 0 ? _dmg_on_frame : _c2_btl_frame;
        if (_r_dmg_seq == 0) _fn_dmg_set(null, 0, null, 0, 0, frame, floor, 999);
        else fixed (DmgNode* n = _r_dmgn) fixed (DmgChr* c = _r_dmgc) _fn_dmg_set(n, _r_dmgh.nn, c, _r_dmgh.nc, _r_dmgh.stamp, frame, floor, _r_dmg_since);
        try { dmg_log_tick(); } catch (Exception e) { if (_dmg_lines < 1000) { _dmg_lines = 1000; _logger.Error($"dmg3d log: {e.Message}"); } }
    }

    private static string dmg_entry(long a, long b) {   // a queue entry's 16 B as the game's fields
        short x = (short)(a & 0xFFFF), y = (short)((a >> 16) & 0xFFFF); int kind = (int)(a >> 32), glyph = (int)(b & 0xFFFFFFFF); uint rgba = (uint)(b >> 32);
        return FormattableString.Invariant($"(x {x} y {y} kind {kind} glyph 0x{glyph:X2} rgba {rgba & 0xFF:X2} {(rgba >> 8) & 0xFF:X2} {(rgba >> 16) & 0xFF:X2} {rgba >> 24:X2})");
    }

    /// <summary>The 10 s dmg3d: line (in battle, while on; at most 500 a session) and the first queue mismatches.</summary>
    private void dmg_log_tick() {
        if (_fn_diag_line == null) return;
        if (!_dmg_hello) { _dmg_hello = true; d7_log(I($"dmg3d: on (size {_dmg_size:F2}, lift {_dmg_lift:F1}); the flat numbers {(_dmg_hide2d ? "hidden while the headset shows ours" : "stay on the panel")}; hide hook: {_dmg_hook_status}; hop table {(_dmg_table_ok ? "read" : "NOT read (the checked copy)")}")); }
        int mn = System.Threading.Volatile.Read(ref _dmg_mis_n);
        while (_dmg_mis_logged < mn) {
            DmgMis m = _dmg_mis[_dmg_mis_logged++];
            d7_log(I($"dmg3d: queue check mismatch at game frame {m.stamp} ({(m.from_draw != 0 ? "at the draw" : "after the draw, stale entries")}), entry {m.at}: game {dmg_entry(m.game0, m.game1)} vs predicted {dmg_entry(m.ours0, m.ours1)}"));
        }
        if (!_c2_in_btl) return;
        double now = ImGui.GetTime();
        if (now - _dmg_line_t < 10.0 || _dmg_lines >= 500) return;
        _dmg_line_t = now; _dmg_lines++;
        if (_fn_dmg_status != null) fixed (uint* u = _dmg_st) _fn_dmg_status(u, 18);
        d7_log(I($"dmg3d: 10 s: snapshots at the join {_dmg_snaps_join}, at the queue draw {_dmg_snaps_draw} (hook calls {_dmg_draw_calls}), nodes seen {_dmg_nodes_seen}, pool reads failed {_dmg_reads_failed}, main-thread faults {_dmg_faults} | targets: the game's anchor {_dmg_chr_exact}, Chr+0x442=0xFF (optpos[1]) {_dmg_chr_ff}, no anchor {_dmg_chr_none} | walks cut: loop {_dmg_cut_loop} outside {_dmg_cut_outside} misaligned {_dmg_cut_misaligned} free {_dmg_cut_free} cap {_dmg_cut_cap} full {_dmg_cut_full} | queue check (our placement vs the game's entries): matched {_dmg_q_ok}, mismatch {_dmg_q_mismatch}, missing {_dmg_q_missing} (the UI was not drawn that frame, or the queue full), other {_dmg_q_other} | hide: {(_dmg_hide2d ? (_dmg_hook_ok ? "on" : "on, NOT HOOKED") : "off")}, frames {_dmg_hidden_frames}, digits taken off {_dmg_hidden_digits}, left for a doubt {_dmg_hide_doubt} | native: snapshots {_dmg_st[4]}, digits drawn {_dmg_st[6]} ({_dmg_st[5]} now), numbers {_dmg_st[11]} (lines {_dmg_st[12]}, dropped {_dmg_st[13]}), without battle.dds {_dmg_st[7]} Presents, no anchor {_dmg_st[8]}, over the cap {_dmg_st[9]}, skip frames {_dmg_st[10]}; battle.dds {(_dmg_atlas ? "held" : "not held")}, hop table {(_dmg_st[14] != 0 ? "as checked" : "DIFFERENT")}"));
    }

    /// <summary>F8 > View, after the UI panel line.</summary>
    private void dmg3d_ui() {
        ImGui.Checkbox("3D damage numbers", ref _dmg3d); ImGui.SameLine();
        ImGui.TextDisabled("battle: the damage / heal numbers at each target in the world (experimental; \"Miss\" and the other words stay on the panel)");
        if (!_dmg3d) return;
        ImGui.SetNextItemWidth(160); ImGui.SliderFloat("number size##dmg3d", ref _dmg_size, 0.3f, 3f, "%.2f"); ImGui.SameLine();
        ImGui.SetNextItemWidth(160); ImGui.SliderFloat("number lift##dmg3d", ref _dmg_lift, -10f, 20f, "%.1f units"); ImGui.SameLine();
        ImGui.Checkbox("hide the flat numbers on the panel##dmg3d", ref _dmg_hide2d);
        if (_dmg_hide2d && !_dmg_hide_at_launch) { ImGui.SameLine(); ImGui.TextDisabled("(takes effect at the next launch: the setting is saved now, the hook it needs is installed at launch)"); }
        if (_fn_dmg_status != null) fixed (uint* u = _dmg_st) _fn_dmg_status(u, 18);
        ImGui.TextDisabled(I($"  battle.dds {(_dmg_atlas ? "held" : "not found yet (found at the first target menu or number of a battle)")}; digits now {_dmg_st[5]}, numbers seen {_dmg_st[11]}; targets without the game's anchor {_dmg_chr_ff + _dmg_chr_none}; queue check matched {_dmg_q_ok} / mismatch {_dmg_q_mismatch}{(_dmg_hide_at_launch ? I($"; hide: {esc(_dmg_hook_status)}, calls {_dmg_draw_calls}, digits taken off {_dmg_hidden_digits}") : "")}"));
    }
}
