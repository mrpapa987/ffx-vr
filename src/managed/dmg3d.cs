// SPDX-License-Identifier: MIT

namespace FFXVR;

// 3D damage numbers (roadmap 4.2, 6 Nov 2026; native ffxvr_dmg3d.h, ABI 47). Off by default again since 8 Nov (ini dmg3d_show=0): with the
// panel's own numbers still shown and the words (Immune, Miss, ...) left on the panel, they added little (the owner). The older keys
// dmg3d and dmg3d_on are ignored, so the new default reaches an existing ini;
// F8 main page, under "battle start (glass break)": "3D damage numbers", live). The battle's damage / heal / MP digits are drawn at each target in the world, in both
// eyes, with the game's own hop, fade and colour; the four word sprites ("Miss" and the others, texture 600) and the Doom
// countdown stay on the panel (v1).
//   Source: the game's damage-number nodes (pool [0x11360B8], list [0x11360BC]), read on the MAIN thread right where the game
//   has just drawn them: the first job join after the frame counter moved (h7_join, always installed: Sg_MainLoop runs the
//   battle ticks, then the UI with its queue push and draw, then the counter, then that join), with the push's own gates
//   (battle_state != 0, [0x133C948] != 0: 0x780CC0 / 0x89B360; 0x79F660's battle_state 0x1F-0x21 test skips only the Doom
//   countdown's loop, the digits go on being drawn) and the main loop's three UI-draw gates ([0x12FB8FB] == 0, [0x12FBBB4] != 0,
//   [0x12FB7D0] == 0, 0x8214F4..0x821533: a frame without the UI publishes nothing, the last snapshot holds as the game's panel
//   does, and the live rule clears it after 3 such frames). One guarded read of the pool (0xA00 B) when the list is not empty,
//   native walks it (ffxvr_dmg_walk), then per target: the anchor as the game computes it (optpos[Chr+0x442] through d7_anchor;
//   Chr+0x442 = 0xFF: the game's 0x794AB0, ported (dmg_ab0, native ffxvr_dmg_anchor: the override matrix, the parent's optpos, or
//   the actor's root on the ground); lifted by Chr+0x4E8; an index above 25, a d7_anchor fallback or a failed port read: a
//   stand-in, counted and never hidden from the panel)
//   and its screen anchors. First person (C3's rule, c3_is_self): the digits of the actor we look out of go to C3's self spot,
//   0.5 m ahead at chest height, instead of our own eyes; with the hide they leave the panel like the others (they are drawn,
//   in front of you: one rule for every target, and the panel never shows half of the numbers). Published
//   in a two-slot seqlock with the game-frame stamp; the render thread hands the latest to native once per Present
//   (ffxvr_dmg_set), which keeps it while it is live (C2's rule) and draws it in the marker pass.
//   The flat numbers on the panel stay (the default). dmg3d_hide2d=1 (ini, read at launch for its hook; off by default) hooks the
//   queue draw 0x893290 (its single call site 0x89B3CB, main thread); the detour records there instead, and while native draws
//   the digits (ffxvr_dmg_skip) takes exactly those entries off the queue, only when every entry matches the game's own
//   placement predicted from the nodes (ffxvr_dmg_queue); on any doubt the queue is left as it is.
//   Check of the mapping without the hook: at the join the queue's entries of this frame are still in memory (the draw zeroes
//   only the count), and the same prediction is compared with them (the 10 s line's "queue check").
// Logging: native writes one dmg3d: line per new number (the first 40); here a dmg3d: line every 10 s in battle while on. Both only
// with a diagnostic profile (diag_any(): dmg3d is on by default, so they would reach every Release log); the hello line always.
public sealed unsafe partial class FFXVRModule {
    private bool  _dmg3d = false;        // ini dmg3d_show (live; default 0 since 8 Nov). The older keys dmg3d and dmg3d_on (saved as 1 by the
                                         // play2-to-fix-all builds) are ignored, as fmv_depth_edges was, so the new default reaches an existing ini
    private float _dmg_size = 1f;        // ini dmg3d_scale (0.3-3)
    private float _dmg_lift = 0f;        // ini dmg3d_lift (units above the game's anchor, -10..20)
    private bool  _dmg_hide2d = false;   // ini dmg3d_hide2d: hook the queue draw at launch; live once hooked
    private bool  _dmg_hide_at_launch;   // what it was at launch (the hook)

    [StructLayout(LayoutKind.Sequential, Pack = 1)] private struct DmgNode { public byte flags, kind, chr, idx, glyph, delay, counter, len; public short ox, oy; public byte ex, ey, slot, pad; }   // dmg::Node, 16 B
    [StructLayout(LayoutKind.Sequential, Pack = 1)] private struct DmgChr {   // dmg::Chr, 72 B
        public Vector3 opt, tl, td; public float h4e8; public int sx, sy, f44, f48, f4c, f50;
        public byte chr, idx442, src, aflags, ok, how, p1, p2;   // how: dmg::AnchorHow, the 0x794AB0 port's branch (log only)
    }
    [StructLayout(LayoutKind.Sequential, Pack = 1)] private struct DmgAnchorIn {   // dmg::AnchorIn, 56 B: what 0x794AB0 reads
        public Vector3 ovr_t, parent_opt, pos; public float f70, c3a4, a4f0;
        public byte ovr, ovr_ok, parent, parent_ok, stat_death, flying, btl_9dd, btl_9ce;
    }
    private delegate* unmanaged[Cdecl]<byte*, uint, uint, DmgNode*, int, uint*, int> _fn_dmg_walk;                       // ABI 47
    private delegate* unmanaged[Cdecl]<byte*, int, int, DmgNode*, int, DmgChr*, int, byte*, uint*, int> _fn_dmg_queue;   // ABI 47
    private delegate* unmanaged[Cdecl]<DmgNode*, int, DmgChr*, int, uint, uint, uint, int, void> _fn_dmg_set;          // ABI 47
    private delegate* unmanaged[Cdecl]<int, float, float, byte*, int> _fn_dmg_cfg;                                   // ABI 47
    private delegate* unmanaged[Cdecl]<int> _fn_dmg_skip;                                                             // ABI 47
    private delegate* unmanaged[Cdecl]<uint*, int, void> _fn_dmg_status;                                              // ABI 47
    private delegate* unmanaged[Cdecl]<DmgAnchorIn*, float*, int> _fn_dmg_anchor;                                     // ABI 50

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
    private int _dmg_snaps_join, _dmg_snaps_draw, _dmg_reads_failed, _dmg_faults, _dmg_nodes_seen, _dmg_chr_exact, _dmg_chr_ff, _dmg_chr_none, _dmg_chr_ab0;
    private int _dmg_cut_loop, _dmg_cut_outside, _dmg_cut_misaligned, _dmg_cut_free, _dmg_cut_cap, _dmg_cut_full;
    private int _dmg_ui_skipped, _dmg_chr_self;   // join frames without the game's UI draw; first-person self targets
    private int _dmg_q_ok, _dmg_q_mismatch, _dmg_q_missing, _dmg_q_other, _dmg_hidden_frames, _dmg_hidden_digits, _dmg_hide_doubt;
    private struct DmgMis { public uint stamp, why; public int at, from_draw; public long game0, game1, ours0, ours1; }   // a mismatch (16 B entries as two longs)
    private readonly DmgMis[] _dmg_mis = new DmgMis[3]; private int _dmg_mis_n, _dmg_mis_logged;

    // render thread
    private DmgHdr _r_dmgh; private readonly DmgNode[] _r_dmgn = new DmgNode[128]; private readonly DmgChr[] _r_dmgc = new DmgChr[16]; private int _r_dmg_seq, _r_dmg_prev, _r_dmg_since = 999;
    private bool _dmg_was_on, _dmg_atlas; private uint _dmg_on_frame; private double _dmg_line_t; private int _dmg_lines; private bool _dmg_hello;
    private readonly uint[] _dmg_st = new uint[18];

    private bool dmg_parse(string k, string v) {
        switch (k) {
            case "dmg3d": case "dmg3d_on": return true;   // older keys: ignored, the numbers start off (dmg3d_show)
            case "dmg3d_show":   pb(k, v, ref _dmg3d); return true;
            case "dmg3d_scale":  pf(k, v, ref _dmg_size, 0.3f, 3f); return true;
            case "dmg3d_lift":   pf(k, v, ref _dmg_lift, -10f, 20f); return true;
            case "dmg3d_hide2d": pb(k, v, ref _dmg_hide2d); return true;
            default: return false;
        }
    }
    private string dmg_ini_text() => $"dmg3d_show={(_dmg3d ? 1 : 0)}\ndmg3d_scale={_dmg_size.ToString("0.00", CI)}\ndmg3d_lift={_dmg_lift.ToString("0.0", CI)}\ndmg3d_hide2d={(_dmg_hide2d ? 1 : 0)}\n";

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
        // the main loop drew no UI this iteration (0x8214F4: [0x12FB8FB] == 0, 0x82152A: [0x12FBBB4] != 0, 0x821533: [0x12FB7D0]
        // == 0, else 0x8AB580, which never reaches 0x89B360): publish nothing, the last snapshot holds (the detour is its own proof)
        if (!draw && (FhUtil.get_at<byte>(0xEFB8FB) != 0 || FhUtil.get_at<byte>(0xEFBBB4) == 0 || FhUtil.get_at<uint>(0xEFB7D0) != 0)) { _dmg_ui_skipped++; return; }
        sbyte bst = FhUtil.get_at<sbyte>(0xD2A8E0);
        bool gate = bst != 0 && FhUtil.get_at<uint>(0xF3C948) != 0;   // 0x780CC0 / 0x89B360 (0x79F660's 0x1F-0x21 test skips only the Doom loop)
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
        if (!d7_anchor(chrs, id, i442 != 0xFF ? i442 : 1, ref a)) { _dmg_chr_none++; return; }   // no actor: the game uses 0x794AB0's no-actor branch; the number stays on the panel
        c.opt = a.opt; c.tl = a.tl; c.td = a.td; c.aflags = (byte)(a.flags & 7); c.ok = 1;
        if (c3_is_self(a)) { c.aflags |= 8; _dmg_chr_self++; }   // dmg::k_self: first person looks out of this actor (C3's rule): the self spot
        // 0xFF: the game anchors at 0x794AB0(chr) (7 Nov logs, finding 3: optpos[1] put the numbers 30-490 PS2 px above the game's)
        if (i442 == 0xFF) c.how = dmg_ab0(chrs, cb, a, ref c);
        // a stand-in (drawn, never taken off the panel): 0xFF when the port's reads failed (optpos[1]), an index above 25 (0x8354F0
        // reads past optpos[25]), or d7_anchor's fallback to optpos[0] / the root for an empty sub-position (review N2)
        c.src = (byte)(c.how != 0 ? 0 : i442 == 0xFF || (a.flags & 4) != 0 || a.fallback != 0 ? 1 : 0);
        if (c.src == 0) { _dmg_chr_exact++; if (c.how != 0) _dmg_chr_ab0++; } else _dmg_chr_ff++;
    }

    /// <summary>The game's 0x794AB0 for a target whose Chr+0x442 is 0xFF (main thread): the reads here, the branch logic in native
    /// (ffxvr_dmg3d.h anchor_794ab0, through ffxvr_dmg_anchor). On success c.opt is the game's point (game space; native lifts it
    /// by Chr+0x4E8 as 0x79F380 does) and c.tl / c.td the roots of its 30 Hz correction: the actor's own (d7_anchor's: opt - tl + td
    /// moves the root to where it is drawn), the parent's for the parent's optpos, none for the override matrix (aflags 2).
    /// Returns the AB_* bits, 0 = keep the optpos[1] stand-in.</summary>
    private byte dmg_ab0(nint chrs, byte* cb, in D7Anchor a, ref DmgChr c) {
        if (_fn_dmg_anchor == null || a.actor == 0) return 0;
        byte* ac = (byte*)a.actor;
        if (!read(ac + 0x10C, out uint ovr) || !read(ac + 0x0C, out Vector3 pos) || !read(ac + 0x4F0, out float a4f0)) return 0;
        if (!read(cb + 0x43F, out byte par) || !read(cb + 0x441, out byte pidx) || !read(cb + 0xDCC, out byte death) || !read(cb + 0x505, out byte fly)
            || !read(cb + 0xF70, out float f70) || !read(cb + 0x3A4, out float c3a4)) return 0;
        DmgAnchorIn x = default;
        x.pos = pos; x.a4f0 = a4f0; x.f70 = f70; x.c3a4 = c3a4; x.parent = par; x.stat_death = death; x.flying = fly;
        x.btl_9dd = FhUtil.get_at<byte>(0xD2C9DD); x.btl_9ce = FhUtil.get_at<byte>(0xD2C9CE);   // VA 0x112C9DD / 0x112C9CE
        x.ovr = (byte)(ovr != 0 ? 1 : 0);
        if (ovr != 0 && read((byte*)(nint)ovr + 0x30, out Vector3 t)) { x.ovr_t = t; x.ovr_ok = 1; }
        D7Anchor pa = default;
        if (ovr == 0 && par != 0xFF && par < 0x1F) {   // 0x795080: the parent chr's actor (none: the actor's own root)
            if (!read((byte*)chrs + par * 0xF90, out nint pact)) return 0;   // unreadable: keep the stand-in (review N2)
            if (pact != 0) {
                x.parent_ok = 2;   // it has one: its optpos[Chr+0x441] must read (0x8354F0), or the port fails
                if (d7_anchor(chrs, par, pidx, ref pa) && (pa.flags & 4) == 0) { x.parent_opt = pa.opt; x.parent_ok = 1; }
            }
        }
        float* o = stackalloc float[3];
        int how = _fn_dmg_anchor(&x, o);
        if (how == 0) return 0;
        c.opt = new Vector3(o[0], o[1], o[2]);
        if ((how & 3) == 2) { c.tl = pa.tl; c.td = pa.td; c.aflags = (byte)((c.aflags & 8) | (pa.flags & 3)); }
        return (byte)how;
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
        fixed (byte* t = _dmg_table) _dmg_atlas = _fn_dmg_cfg(1 | (diag_any() ? 2 : 0), _dmg_size, _dmg_lift, _dmg_table_ok ? t : null) != 0;   // 2: native's per-number lines (a diagnostic profile only)
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

    /// <summary>The 10 s dmg3d: line (in battle, while on, with a diagnostic profile only: dmg3d is on for every player now, od-visual
    /// review N2; at most 500 a session) and the first queue mismatches.</summary>
    private void dmg_log_tick() {
        if (_fn_diag_line == null) return;
        if (!_dmg_hello) { _dmg_hello = true; d7_log(I($"dmg3d: on (size {_dmg_size:F2}, lift {_dmg_lift:F1}); the flat numbers {(_dmg_hide2d ? "hidden while the headset shows ours" : "stay on the panel")}; hide hook: {_dmg_hook_status}; hop table {(_dmg_table_ok ? "read" : "NOT read (the checked copy)")}")); }
        int mn = System.Threading.Volatile.Read(ref _dmg_mis_n);
        while (_dmg_mis_logged < mn) {
            DmgMis m = _dmg_mis[_dmg_mis_logged++];
            d7_log(I($"dmg3d: queue check mismatch at game frame {m.stamp} ({(m.from_draw != 0 ? "at the draw" : "after the draw, stale entries")}), entry {m.at}: game {dmg_entry(m.game0, m.game1)} vs predicted {dmg_entry(m.ours0, m.ours1)}"));
        }
        if (!_c2_in_btl || !diag_any()) return;
        double now = ImGui.GetTime();
        if (now - _dmg_line_t < 10.0 || _dmg_lines >= 500) return;
        _dmg_line_t = now; _dmg_lines++;
        if (_fn_dmg_status != null) fixed (uint* u = _dmg_st) _fn_dmg_status(u, 18);
        d7_log(I($"dmg3d: 10 s: snapshots at the join {_dmg_snaps_join} (join frames without the game's UI draw {_dmg_ui_skipped}), at the queue draw {_dmg_snaps_draw} (hook calls {_dmg_draw_calls}), nodes seen {_dmg_nodes_seen}, pool reads failed {_dmg_reads_failed}, main-thread faults {_dmg_faults} | targets: the game's anchor {_dmg_chr_exact} (of them through the 0x794AB0 port {_dmg_chr_ab0}), stand-in (0xFF port failed / above 25 / fallback) {_dmg_chr_ff}, no anchor {_dmg_chr_none}, first person's own (self spot) {_dmg_chr_self} | walks cut: loop {_dmg_cut_loop} outside {_dmg_cut_outside} misaligned {_dmg_cut_misaligned} free {_dmg_cut_free} cap {_dmg_cut_cap} full {_dmg_cut_full} | queue check (our placement vs the game's entries): matched {_dmg_q_ok}, mismatch {_dmg_q_mismatch}, missing {_dmg_q_missing} (the UI was not drawn that frame, or the queue full), other {_dmg_q_other} | hide: {(_dmg_hide2d ? (_dmg_hook_ok ? "on" : "on, NOT HOOKED") : "off")}, frames {_dmg_hidden_frames}, digits taken off {_dmg_hidden_digits}, left for a doubt {_dmg_hide_doubt} | native: snapshots {_dmg_st[4]}, digits drawn {_dmg_st[6]} ({_dmg_st[5]} now), numbers {_dmg_st[11]} (lines {_dmg_st[12]}, dropped {_dmg_st[13]}), without battle.dds {_dmg_st[7]} Presents, no anchor {_dmg_st[8]}, over the cap {_dmg_st[9]}, skip frames {_dmg_st[10]}; battle.dds {(_dmg_atlas ? "held" : "not held")}, hop table {(_dmg_st[14] != 0 ? "as checked" : "DIFFERENT")}"));
    }

    /// <summary>F8 > Battle, last (experimental). The readout is in Developer > Logging & status.</summary>
    private void dmg3d_ui() {
        ui_check("dmg3d_show", "3D damage numbers", ref _dmg3d, UiTag.Experimental, "Battle: the damage / heal numbers at each target in the world (\"Miss\" and the other words stay on the panel).");
        if (!_dmg3d) return;
        ui_slider("dmg3d_scale", "size##dmg3d", ref _dmg_size, 0.3f, 3f, "%.2f", UiTag.None, null, 160f); ImGui.SameLine();
        ui_slider("dmg3d_lift", "lift##dmg3d", ref _dmg_lift, -10f, 20f, "%.1f units", UiTag.None, null, 160f); ImGui.SameLine();
        ui_check("dmg3d_hide2d", "hide the flat numbers##dmg3d", ref _dmg_hide2d, UiTag.NextLaunch, _dmg_hide_at_launch
                 ? "The game's flat numbers are left off the panel. Applies at once: the hook it needs was installed at launch."
                 : "The game's flat numbers are left off the panel. Turned on, it takes effect at the next launch: the setting is saved now, the hook it needs is installed at launch.");
        if (!_dmg_atlas) ImGui.TextDisabled("battle.dds not found yet (found at the first target menu or number of a battle)");
    }
}
