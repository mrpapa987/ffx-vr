// SPDX-License-Identifier: MIT

namespace FFXVR;

// The sphere-hands capture (research/sphere-hands-design.md; the feature: take and place the Cloister of Trials spheres with a
// grip of the VR hand). A LOG-ONLY research channel: every read guarded (native safe_read), nothing written to the game, no
// hook. ffxvr_diag.log, lines "sph: ...".
//
// When: in a Cloister of Trials map (SphTrial: bsyt0100, klyt1000, djyt0600, mcyt0500, bvyt1100, dome0700 and dome0200) while
// any diagnostic is on (diag_any: the developer profile), or anywhere with F8 > Developer > Inspect > "Sphere research log"
// (ini diag_sphere, default 0).
// What:
//   - a compact line every 0.5 s (only when something in it changed, else one every 5 s): Tidus's root and heading, the game's
//     talk state (the confirm request, the chosen worker), the trial's carried-sphere variable and its socket variables, the text
//     and choice windows, the nearest interaction workers and object actors with their distance from Tidus and each VR hand;
//   - a full snapshot (every talk-enabled worker within 120 units with its talk block, every object actor and every actor
//     within 40 units with its flags, attachment, model and index-buffer keys, the skinned draws near Tidus, the text of each
//     open window, Tidus's sub-positions while an object shares his root) on: entering the map, player control off and on,
//     a new button press, the game's confirm request, a change of its talk target, a choice box opening or closing, a text
//     window opening, and a change of the carried / socket variables.
//   Bounded: 4000 compact lines and 400 full snapshots a session (150 before sphere hands S1+2), at most 4 snapshots a second.
//   Since S1+2 a snapshot lists only the talk-enabled workers (and the target), 40 at most, and the grab feature (sphere_hands.cs)
//   shares these reads.
//
// Game facts used (installed build, module offsets = VA - 0x400000; checked in our exe's disassembly, dis_full.txt and the
// Ghidra export; facts from Fahrenheit, the FFX screen reader and FFXDataParser re-derived there, no code taken):
//   [0xF26B28] the Atel controller. +0x00 flags (bits 0x22: the player may act, 0x86C4C0), +0x0A u16 the player's worker,
//              +0x0C u16 the worker count, +0x14 / +0x16 / +0x18 u16 the counts of the 0x5B8 / 0x558 / 0x2E8 workers, +0x1C the
//              worker array (0x86A830: first the 0xB58 workers, then 0x558, 0x5B8, 0x2E8); +0x1E4 u8 1 = a confirm (channel 0)
//              or secondary (1) press this frame, +0x1E6 its channel, +0x1E8 s16 the worker chosen for it (-1 none), +0x1EC
//              its squared distance, +0x1F0 the player's own talk radius, +0x218 the player's position (0x867950, 0x8655B0,
//              0x866980, 0x8684B0, 0x869E40).
//   worker: +0x00 the script header (its byte 0 = the worker type: 1 object with an actor, 2 a line zone, 3 an area zone,
//           5 / 6 others), +0x04 the script chunk, +0x2E u16 its global index, +0x34 / +0x36 u16 flags, +0x9C the actor,
//           +0xAA u8, +0xAB u8 the interaction enables (bit 0 talk = the confirm channel; 0x8764F0 tests 1 << channel);
//           the talk block at +0x558 (+0x284 for types 5 / 6): +0 position (a type 1 worker copies its actor's +0x0C there
//           every frame, 0x869E40), +0x2C height tolerance, +0x30 the second point / +0x34 the heading (the player's facing),
//           +0x3C the angle tolerance, +0x40 the talk range, +0x48 the touch radius (0x866980).
//   chunk: +0x0C the script name's offset, +0x20 the event data's offset (declaration location 6, 0x86C2E0), +0x34 u16 the
//           worker count, +0x38 the workers' header offsets.
//   The trials' state variables (the screen reader's cloister contracts, declaration location 6): see SphTrial.
//   Text: the render-side windows (game_gates.cs block; +0x94 the page text pointer) and the script's TOMesWinWork table
//         ([0xF26B82] bank, 0xF26D70: +0x08 the text pointer, +0x14 the state); the cursor +0x01 mode, +0x02 row, +0x05 items.
//   Input: 0xF270C0 raw held, 0xF270D0 raw new presses (before the game's button mapping, 0x888C40).
public sealed unsafe partial class FFXVRModule {

    private bool _diag_sphere;   // ini diag_sphere (default 0; Developer): the sphere research log anywhere (it runs in the cloisters with any diagnostic on)
    private delegate* unmanaged[Cdecl]<float*, int, int> _fn_sph_hands;   // ABI 63: the drawn hands' positions (render space)

    private const int k_sph_lines_max = 4000, k_sph_full_max = 400;   // sphere hands S1+2: 400 snapshots (the Djose session spent 150 by its last third)
    private const float k_sph_upm = 11.4f;   // game units per metre (the uidepth readback's measure)
    private string _sph_map = ""; private int _sph_trial = -1; private double _sph_map_t = -1;
    private bool _sph_active, _sph_entered;
    private int _sph_lines, _sph_full; private double _sph_compact_t, _sph_compact_sig_t; private int _sph_full_sec; private double _sph_sec_t;
    private long _sph_compact_sig;
    private uint _sph_pl_prev = 1; private uint _sph_pressed_prev; private byte _sph_req_prev; private short _sph_tgt_prev = -1;
    private int _sph_choice_prev = -1, _sph_open_prev; private long _sph_vars_prev;
    private nint _sph_chunk; private int _sph_chunk_wk = -1; private double _sph_chunk_t = -1;
    private byte* _sph_ctl; private byte* _sph_vars; private byte[]? _sph_blk;
    private uint* _sph_ibq; private byte* _sph_ibseg;
    private readonly Dictionary<nint, string> _sph_chunk_names = new();
    private readonly Dictionary<nint, int[]> _sph_chunk_tabs = new();
    private readonly HashSet<long> _sph_keys_logged = new();

    /// <summary>settings_parse's default branch: diag_sphere. False: not ours.</summary>
    private bool sph_parse(string k, string v) {
        if (k != "diag_sphere") return false;
        pb(k, v, ref _diag_sphere); return true;
    }
    private void sph_bind(Func<string, nint> req) { _fn_sph_hands = (delegate* unmanaged[Cdecl]<float*, int, int>)req("ffxvr_sph_hands"); }

    /// <summary>From push_all, every frame. Cheap when inactive: the map name is read 4 times a second.</summary>
    private void sph_tick() {
        if (_fn_safe_read == null) return;
        if (_now - _sph_map_t >= 0.25 || _now < _sph_map_t) {
            _sph_map_t = _now;
            string m = map_name();
            if (m != _sph_map) { _sph_map = m; _sph_trial = SphTrial.Find(map_base(m)); _sph_entered = false; _sph_chunk = 0; _sph_chunk_t = -1; _sph_keys_logged.Clear(); }
        }
        bool active = _diag_sphere || (_sph_trial >= 0 && diag_any());
        bool feat = sphh_wanted();   // sphere hands (sphere_hands.cs): the grab feature needs the same reads
        if (!active && !feat) { if (_sph_active) sph_reset(); sphh_push_off(); return; }
        _sph_active = true;
        if (_sph_ctl == null) { _sph_ctl = (byte*)Marshal.AllocHGlobal(0x240); _sph_vars = (byte*)Marshal.AllocHGlobal(0x100); }   // kept for the session
        // this frame's cheap reads: the controller, the player, the presses, the windows, the trial's variables
        uint ctl = 0; bool ctl_ok = read(FhUtil.ptr_at<uint>(0xF26B28), out ctl) && ctl != 0 && _fn_safe_read((void*)(nint)ctl, _sph_ctl, 0x238) != 0;
        bool pl_ok = read(FhUtil.ptr_at<uint>(GameGates.k_player), out uint pl);
        read(FhUtil.ptr_at<uint>(0xF270D0), out uint pressed);
        _sph_blk ??= new byte[GameGates.k_blk_len];
        GameGates.Text tx = GameGates.Text.None;
        fixed (byte* b = _sph_blk) {
            if (_fn_safe_read(FhUtil.ptr_at<byte>(GameGates.k_blk), b, (uint)GameGates.k_blk_len) != 0)
                tx = GameGates.Scan(new ReadOnlySpan<byte>(b, GameGates.k_blk_len), (uint)(nint)FhUtil.ptr_at<byte>(GameGates.k_blk));
        }
        if (ctl_ok && (_sph_chunk == 0 || _now - _sph_chunk_t >= 2.0 || _now < _sph_chunk_t)) sph_find_chunk(ctl);
        long vsig = sph_vars_read(out int carried);
        if (feat) sphh_push(ctl_ok ? ctl : 0, pl_ok, pl, tx, carried); else sphh_push_off();
        if (!active) return;   // the log below: only with the log on
        // the events that take a full snapshot
        string? why = null;
        if (!_sph_entered) { _sph_entered = true; why = I($"entered map {_sph_map}{(_sph_trial >= 0 ? " (" + SphTrial.T[_sph_trial].name + "'s cloister)" : " (not a cloister: the Developer checkbox)")}"); }
        if (pl_ok && (pl == 0) != (_sph_pl_prev == 0)) why = sph_why(why, pl == 0 ? "player control OFF" : "player control back");
        if (pl_ok) _sph_pl_prev = pl;
        if (pressed != 0 && pressed != _sph_pressed_prev) why = sph_why(why, I($"button press (raw new 0x{pressed:X})"));
        _sph_pressed_prev = pressed;
        if (ctl_ok) {
            byte req = _sph_ctl[0x1E4]; short tgt = *(short*)(_sph_ctl + 0x1E8);
            if (req == 1 && _sph_req_prev != 1) why = sph_why(why, I($"the game's confirm request (channel {_sph_ctl[0x1E6]}, target worker {tgt})"));
            if (tgt != _sph_tgt_prev) why = sph_why(why, I($"talk target {_sph_tgt_prev} -> {tgt}"));
            _sph_req_prev = req; _sph_tgt_prev = tgt;
        }
        if (tx.choice_slot != _sph_choice_prev) why = sph_why(why, tx.choice_slot >= 0 ? I($"choice box OPEN (slot {tx.choice_slot}, row {tx.choice_row})") : "choice box closed");
        _sph_choice_prev = tx.choice_slot;
        if ((tx.open & ~_sph_open_prev) != 0) why = sph_why(why, I($"text window opened (open 0x{tx.open:X2})"));
        _sph_open_prev = tx.open;
        if (vsig != _sph_vars_prev) { if (_sph_vars_prev != 0 && vsig != 0) why = sph_why(why, I($"trial variables changed (carried {carried})")); _sph_vars_prev = vsig; }
        if (why != null) sph_full_snapshot(why, ctl_ok ? ctl : 0, pl, tx);
        if (_now - _sph_compact_t >= 0.5 || _now < _sph_compact_t) { _sph_compact_t = _now; sph_compact(ctl_ok ? ctl : 0, pl, tx, carried); }
    }
    private static string sph_why(string? a, string b) => a == null ? b : a + "; " + b;
    private void sph_reset() {
        _sph_active = false; _sph_entered = false; _sph_pl_prev = 1; _sph_pressed_prev = 0; _sph_req_prev = 0; _sph_tgt_prev = -1;
        _sph_choice_prev = -1; _sph_open_prev = 0; _sph_vars_prev = 0; _sph_compact_sig = 0; _sph_chunk = 0;
    }

    // -- the Atel workers (pure reads; 0x86A830's arithmetic) ---------------------------------------------------------
    /// <summary>Worker i's address from the controller copy (0 outside), and its size class (0xB58, 0x558, 0x5B8, 0x2E8, 0x30).</summary>
    private nint sph_worker(int i, out int cls) {
        cls = 0;
        int count = *(ushort*)(_sph_ctl + 0x0C), n5b8 = *(ushort*)(_sph_ctl + 0x14), n558 = *(ushort*)(_sph_ctl + 0x16), n2e8 = *(ushort*)(_sph_ctl + 0x18);
        nint b = (nint)(*(uint*)(_sph_ctl + 0x1C));
        int nb58 = count - n5b8 - n558 - n2e8;
        if (b == 0 || i < 0 || i >= count || count > 1024 || nb58 < 0) return 0;
        if (i < nb58) { cls = 0xB58; return b + i * 0xB58; }
        b += nb58 * 0xB58; i -= nb58;
        if (i < n558) { cls = 0x558; return b + i * 0x558; }
        b += n558 * 0x558; i -= n558;
        if (i < n5b8) { cls = 0x5B8; return b + i * 0x5B8; }
        b += n5b8 * 0x5B8; i -= n5b8;
        if (i < n2e8) { cls = 0x2E8; return b + i * 0x2E8; }
        b += n2e8 * 0x2E8; i -= n2e8;
        cls = 0x30; return b + i * 0x30;
    }
    private struct SphW {
        public int gi, local, type, cls, actor_slot; public nint w, chunk, actor; public ushort f34, f36, idx2e; public byte aa, ab;
        public bool block; public Vector3 pos, p2; public float heading, htol, atol, range, touch, contact_h;
    }
    /// <summary>One worker: its header fields and talk block (guarded reads). False when it does not read.</summary>
    private bool sph_worker_read(int gi, Actor* actors, out SphW o) {
        o = default; o.gi = gi; o.local = -1; o.actor_slot = -1;
        nint w = sph_worker(gi, out int cls); if (w == 0) return false;
        o.w = w; o.cls = cls;
        byte* h = stackalloc byte[0xB0];
        if (_fn_safe_read((void*)w, h, (uint)(cls >= 0xB0 ? 0xB0 : cls)) == 0) return false;
        nint hdr = (nint)(*(uint*)h); o.chunk = (nint)(*(uint*)(h + 4));
        if (hdr == 0 || !read((void*)hdr, out byte type)) return false;
        o.type = type;
        if (cls < 0xB0) return true;
        o.idx2e = *(ushort*)(h + 0x2E); o.f34 = *(ushort*)(h + 0x34); o.f36 = *(ushort*)(h + 0x36); o.aa = h[0xAA]; o.ab = h[0xAB];
        o.actor = (nint)(*(uint*)(h + 0x9C));
        if (actors != null && o.actor != 0) { long d = (long)o.actor - (long)(nint)actors; if (d >= 0 && d % sizeof(Actor) == 0 && d / sizeof(Actor) < 48) o.actor_slot = (int)(d / sizeof(Actor)); }
        int boff = type == 4 ? -1 : (type == 5 || type == 6) ? 0x284 : 0x558;
        if (boff > 0 && boff + 0x50 <= cls) {
            float* f = stackalloc float[20];
            if (_fn_safe_read((byte*)w + boff, f, 0x50) != 0) {
                o.block = true; o.pos = new Vector3(f[0], f[1], f[2]); o.contact_h = f[7]; o.htol = f[11]; o.p2 = new Vector3(f[12], f[13], f[14]);
                o.heading = f[13]; o.atol = f[15]; o.range = f[16]; o.touch = f[18];
            }
        }
        o.local = sph_local(o.chunk, hdr);
        return true;
    }
    /// <summary>The worker's index inside its own script (the chunk's header-offset table, read once per chunk), -1 unknown.</summary>
    private int sph_local(nint chunk, nint hdr) {
        if (chunk == 0) return -1;
        if (!_sph_chunk_tabs.TryGetValue(chunk, out int[]? tab)) {
            tab = Array.Empty<int>();
            if (read((byte*)chunk + 0x34, out ushort n) && n > 0 && n <= 1024) {
                var t = new int[n];
                fixed (int* tp = t) if (_fn_safe_read((byte*)chunk + 0x38, tp, (uint)(n * 4)) != 0) tab = t;
            }
            if (_sph_chunk_tabs.Count > 64) _sph_chunk_tabs.Clear();
            _sph_chunk_tabs[chunk] = tab;
        }
        long off = (long)hdr - (long)chunk;
        for (int k = 0; k < tab.Length; k++) if (tab[k] == off) return k;
        return -1;
    }
    private string sph_chunk_name(nint chunk) {
        if (chunk == 0) return "-";
        if (_sph_chunk_names.TryGetValue(chunk, out string? s)) return s;
        s = "?";
        byte* nm = stackalloc byte[9]; nm[8] = 0;
        if (read((byte*)chunk + 0x0C, out uint no) && no < 0x800000 && _fn_safe_read((byte*)chunk + no, nm, 8) != 0) {
            var sb = new System.Text.StringBuilder();
            for (int i = 0; i < 8 && nm[i] != 0; i++) sb.Append(nm[i] >= 32 && nm[i] < 127 ? (char)nm[i] : '?');
            s = sb.ToString();
        }
        if (_sph_chunk_names.Count > 64) _sph_chunk_names.Clear();
        _sph_chunk_names[chunk] = s;
        return s;
    }
    /// <summary>The trial's script chunk: the first worker whose chunk carries the trial's script name (the player's first).</summary>
    private void sph_find_chunk(uint ctl) {
        _sph_chunk_t = _now; _sph_chunk = 0; _sph_chunk_wk = -1;
        if (_sph_trial < 0) return;
        string want = SphTrial.T[_sph_trial].script;
        int count = *(ushort*)(_sph_ctl + 0x0C), pw = *(ushort*)(_sph_ctl + 0x0A);
        for (int k = -1; k < count && k < 1024; k++) {
            int gi = k < 0 ? pw : k;
            nint w = sph_worker(gi, out _); if (w == 0) continue;
            if (!read((byte*)w + 4, out uint ch) || ch == 0) continue;
            if (sph_chunk_name((nint)ch) == want) { _sph_chunk = (nint)ch; _sph_chunk_wk = gi; return; }
        }
    }
    /// <summary>The trial's state variables (event data, declaration location 6) into _sph_vars; a signature of them (0: not read)
    /// and the carried-sphere value (-1 unknown).</summary>
    private long sph_vars_read(out int carried) {
        carried = -1;
        if (_sph_trial < 0 || _sph_chunk == 0) return 0;
        ref readonly SphTrial t = ref SphTrial.T[_sph_trial];
        if (!read((byte*)_sph_chunk + 0x20, out uint ev) || ev == 0 || ev > 0x800000) return 0;
        int len = t.n * t.size; if (len > 0x100) return 0;
        if (_fn_safe_read((byte*)_sph_chunk + ev + t.lo, _sph_vars, (uint)len) == 0) return 0;
        int ci = (t.carried - t.lo) / t.size;
        carried = sph_var(ci);
        long sig = 1469598103934665603L;
        for (int i = 0; i < len; i++) sig = (sig ^ _sph_vars[i]) * 1099511628211L;
        return sig == 0 ? 1 : sig;
    }
    private int sph_var(int i) {
        ref readonly SphTrial t = ref SphTrial.T[_sph_trial];
        if (i < 0 || i >= t.n) return -1;
        return t.size == 4 ? *(int*)(_sph_vars + i * 4) : _sph_vars[i];
    }
    private string sph_vars_text(int carried) {
        if (_sph_trial < 0) return "not a cloister map";
        ref readonly SphTrial t = ref SphTrial.T[_sph_trial];
        if (_sph_chunk == 0) return I($"{t.script}'s script not found among the workers");
        var sb = new System.Text.StringBuilder();
        sb.Append(I($"carried {carried} ({SphTrial.Name(_sph_trial, carried)}) | vars @+0x{t.lo:X} ({(t.size == 4 ? "i32" : "u8")}):"));
        for (int i = 0; i < t.n; i++) { int off = t.lo + i * t.size; sb.Append(I($" {off:X}={sph_var(i)}")); }
        return sb.ToString();
    }

    // -- the hands ----------------------------------------------------------------------------------------------------
    /// <summary>The drawn hands in game space (render space with x and y negated); ok[h] false when not drawn on this frame or the last.</summary>
    private void sph_hands(Vector3* palm, bool* ok, float* sq, out Vector3 eye, out bool eye_ok) {
        float* v = stackalloc float[24];
        for (int i = 0; i < 24; i++) v[i] = 0f;
        int n = _fn_sph_hands != null ? _fn_sph_hands(v, 24) : 0;
        for (int h = 0; h < 2; h++) { float* o = v + h * 10; ok[h] = n >= 24 && o[0] > 0.5f; palm[h] = new Vector3(-o[1], -o[2], o[3]); sq[h] = o[7]; }
        eye = new Vector3(-v[20], -v[21], v[22]); eye_ok = n >= 24 && v[23] > 0.5f;
    }
    private static string sph_d(bool ok, Vector3 a, Vector3 b) => ok ? I($"{Vector3.Distance(a, b):F1}") : "-";

    // -- the compact line ---------------------------------------------------------------------------------------------
    private void sph_compact(uint ctl, uint pl, in GameGates.Text tx, int carried) {
        if (_sph_lines >= k_sph_lines_max) return;
        Actor* actors = Globals.actors; Actor* snap = tbl_snap(actors);
        int ti = sph_player_slot(actors, pl);
        Vector3 tp = default; float th = 0f;
        if (snap != null && ti >= 0) { tp = new Vector3(snap[ti].chr_pos_vec.X, snap[ti].chr_pos_vec.Y, snap[ti].chr_pos_vec.Z); th = snap[ti].chr_rotation_rad; }
        Vector3* palm = stackalloc Vector3[2]; bool* hok = stackalloc bool[2]; float* sq = stackalloc float[2];
        sph_hands(palm, hok, sq, out _, out _);
        var sb = new System.Text.StringBuilder();
        long sig = (long)carried * 31 + tx.open * 7 + tx.choice_slot * 131 + tx.choice_row * 1031 + (pl == 0 ? 1 : 2) * 7919;
        sb.Append(I($"sph: {_sph_map} | Tidus {(ti >= 0 ? I($"#{ti} ({tp.X:F1},{tp.Y:F1},{tp.Z:F1}) h {th:F2}") : "?")} control {(pl != 0 ? "yes" : "NO")}"));
        if (ctl != 0) {
            short tgt = *(short*)(_sph_ctl + 0x1E8);
            sb.Append(I($" | ctl b0 0x{_sph_ctl[0]:X2} req {_sph_ctl[0x1E4]} tgt {tgt}"));
            sig = sig * 31 + tgt + _sph_ctl[0] * 65537L;
        }
        sb.Append(I($" | hands L {(hok[0] ? I($"({palm[0].X:F1},{palm[0].Y:F1},{palm[0].Z:F1}) sq {sq[0]:F2}") : "-")} R {(hok[1] ? I($"({palm[1].X:F1},{palm[1].Y:F1},{palm[1].Z:F1}) sq {sq[1]:F2}") : "-")}"));
        sb.Append(" | ").Append(sph_vars_text(carried));
        sb.Append(I($" | text open 0x{tx.open:X2} shown 0x{tx.shown:X2} choice {(tx.choice_slot >= 0 ? I($"slot {tx.choice_slot} row {tx.choice_row}") : "-")}"));
        // the nearest interaction workers (talk enabled) and object actors, three each
        if (ctl != 0 && ti >= 0) {
            int count = Math.Min((int)*(ushort*)(_sph_ctl + 0x0C), 1024);
            Span<int> bi = stackalloc int[3]; Span<float> bd = stackalloc float[3]; bi.Fill(-1); bd.Fill(float.MaxValue);
            Span<SphW> bw = stackalloc SphW[3];
            for (int i = 0; i < count; i++) {
                if (!sph_worker_read(i, actors, out SphW w) || !w.block || (w.ab & 1) == 0 || w.type < 1 || w.type > 3) continue;
                float d = Vector3.Distance(w.pos, tp); if (!(d < 120f)) continue;
                for (int k = 0; k < 3; k++) if (d < bd[k]) { for (int m = 2; m > k; m--) { bd[m] = bd[m - 1]; bi[m] = bi[m - 1]; bw[m] = bw[m - 1]; } bd[k] = d; bi[k] = i; bw[k] = w; break; }
            }
            sb.Append(" | near workers:");
            for (int k = 0; k < 3; k++) if (bi[k] >= 0) {
                SphW w = bw[k];
                sb.Append(I($" w{w.gi}/L{w.local} t{w.type} ab{w.ab:X} a#{w.actor_slot} d {bd[k]:F1} (r {w.range:F1}) dL {sph_d(hok[0], palm[0], w.pos)} dR {sph_d(hok[1], palm[1], w.pos)};"));
                sig = sig * 31 + w.gi * 7 + (long)(bd[k] * 2f);
            }
        }
        if (snap != null && ti >= 0) {
            sb.Append(" | objects:");
            int shown = 0;
            for (int i = 0; i < 48 && shown < 4; i++) {
                if (i == ti || !live(snap[i]) || ((snap[i].chr_id >> 12) & 0xF) != 5) continue;
                Vector3 p = new Vector3(snap[i].chr_pos_vec.X, snap[i].chr_pos_vec.Y, snap[i].chr_pos_vec.Z);
                float d = Vector3.Distance(p, tp); if (!(d < 80f)) continue;
                shown++;
                byte hide = ((byte*)(snap + i))[0x180];
                sb.Append(I($" #{i} {actor_name(snap[i].chr_name)} d {d:F1} dL {sph_d(hok[0], palm[0], p)} dR {sph_d(hok[1], palm[1], p)} hide {hide:X2};"));
                sig = sig * 31 + i * 13 + (long)(d * 2f) + hide;
            }
        }
        sb.Append(sphh_brief(out long gsig)); sig = sig * 31 + gsig;   // sphere hands (sphere_hands.cs): the grab state
        // dedupe: an unchanged line goes out once every 5 s
        if (sig == _sph_compact_sig && _now - _sph_compact_sig_t < 5.0 && _now >= _sph_compact_sig_t) return;
        _sph_compact_sig = sig; _sph_compact_sig_t = _now;
        _sph_lines++;
        if (_sph_lines == k_sph_lines_max) sb.Append(" (no more compact sph: lines this session)");
        d7_log(sb.ToString());
    }
    /// <summary>Tidus's slot: the game's player actor ([0x1300788]) when it is in the table, else the followed actor.</summary>
    private int sph_player_slot(Actor* actors, uint pl) {
        if (actors != null && pl != 0) { long d = (long)pl - (long)(nint)actors; if (d >= 0 && d % sizeof(Actor) == 0 && d / sizeof(Actor) < 48) return (int)(d / sizeof(Actor)); }
        return _fp_actor_idx;
    }

    // -- the full snapshot --------------------------------------------------------------------------------------------
    private void sph_full_snapshot(string why, uint ctl, uint pl, in GameGates.Text tx) {
        if (_sph_full >= k_sph_full_max) return;
        if (_now - _sph_sec_t >= 1.0 || _now < _sph_sec_t) { _sph_sec_t = _now; _sph_full_sec = 0; }
        if (_sph_full_sec >= 4) return;
        _sph_full_sec++; _sph_full++;
        int no = _sph_full;
        void L(string s) => d7_log(I($"sph: [{no}] ") + s);
        Actor* actors = Globals.actors; Actor* snap = tbl_snap(actors);
        int ti = sph_player_slot(actors, pl);
        Vector3 tp = default;
        Vector3* palm = stackalloc Vector3[2]; bool* hok = stackalloc bool[2]; float* sq = stackalloc float[2];
        sph_hands(palm, hok, sq, out Vector3 eye, out bool eye_ok);
        read(FhUtil.ptr_at<int>(0xEFBC40), out int ev_id); read(FhUtil.ptr_at<ushort>(0xD2CA90), out ushort room);
        read(FhUtil.ptr_at<uint>(0xF270C0), out uint held); read(FhUtil.ptr_at<uint>(0xF270D0), out uint pressed);
        L(I($"=== {why} | map {_sph_map} event {ev_id} room {room} frame {_frames} t {_now:F2} | raw held 0x{held:X} new 0x{pressed:X} | fp_mode {_fp_mode}{(_sph_full == k_sph_full_max ? " (the last full snapshot this session)" : "")}"));
        if (snap != null && ti >= 0) {
            Actor a = snap[ti]; byte* r = (byte*)(snap + ti);
            tp = new Vector3(a.chr_pos_vec.X, a.chr_pos_vec.Y, a.chr_pos_vec.Z);
            L(I($"Tidus: actor #{ti} '{actor_name(a.chr_name)}' (player actor 0x{pl:X8}) root ({tp.X:F2},{tp.Y:F2},{tp.Z:F2}) rot +0x158 {a.chr_rotation_rad:F3} dir +0x168 {a.chr_direction:F3} drawn root +0x200 ({*(float*)(r + 0x200):F2},{*(float*)(r + 0x204):F2},{*(float*)(r + 0x208):F2}) worker +0x190 {*(ushort*)(r + 0x190)}"));
        }
        else L(I($"Tidus: not found (player actor 0x{pl:X8}, followed slot {_fp_actor_idx})"));
        L(I($"hands (game space; {k_sph_upm} units = 1 m): L {(hok[0] ? I($"palm ({palm[0].X:F2},{palm[0].Y:F2},{palm[0].Z:F2}) sq {sq[0]:F2} d Tidus {Vector3.Distance(palm[0], tp):F1}") : "not drawn")} | R {(hok[1] ? I($"palm ({palm[1].X:F2},{palm[1].Y:F2},{palm[1].Z:F2}) sq {sq[1]:F2} d Tidus {Vector3.Distance(palm[1], tp):F1}") : "not drawn")} | eye base {(eye_ok ? I($"({eye.X:F2},{eye.Y:F2},{eye.Z:F2})") : "-")}"));
        int count = 0;
        if (ctl != 0) {
            byte* c = _sph_ctl;
            count = Math.Min((int)*(ushort*)(c + 0x0C), 1024);
            L(I($"controller 0x{ctl:X8}: b0 0x{c[0]:X2} b1 0x{c[1]:X2} b2 0x{c[2]:X2} b3 0x{c[3]:X2} | player worker {*(ushort*)(c + 0x0A)} | workers {count} (0x558 {*(ushort*)(c + 0x16)}, 0x5B8 {*(ushort*)(c + 0x14)}, 0x2E8 {*(ushort*)(c + 0x18)}) | confirm req {c[0x1E4]} channel {c[0x1E6]} target {*(short*)(c + 0x1E8)} d2 {*(float*)(c + 0x1EC):F1} | player radius +0x1F0 {*(float*)(c + 0x1F0):F2} pos +0x218 ({*(float*)(c + 0x218):F2},{*(float*)(c + 0x21C):F2},{*(float*)(c + 0x220):F2}) | npc_last +0x1E8 as Fahrenheit names it"));
            if (sph_worker_read(*(ushort*)(c + 0x0A), actors, out SphW pw))
                L(I($"player worker {pw.gi} (local {pw.local}, {sph_chunk_name(pw.chunk)}) type {pw.type} block pos ({pw.pos.X:F2},{pw.pos.Y:F2},{pw.pos.Z:F2}) heading +0x34 {pw.heading:F3} actor #{pw.actor_slot}"));
        }
        L(I($"trial: {(_sph_trial >= 0 ? SphTrial.T[_sph_trial].name + " " + SphTrial.T[_sph_trial].script : "-")} chunk 0x{(ulong)_sph_chunk:X} (found via worker {_sph_chunk_wk}) | ") + sph_vars_text(sph_var(_sph_trial >= 0 ? (SphTrial.T[_sph_trial].carried - SphTrial.T[_sph_trial].lo) / SphTrial.T[_sph_trial].size : -1)));
        sph_text_lines(L, tx);
        // the interaction workers: any interaction enabled, within 120 units of Tidus (24 at most), and the chosen target
        if (ctl != 0) {
            short tgt = *(short*)(_sph_ctl + 0x1E8); int n = 0;
            for (int i = 0; i < count && n < 40; i++) {
                if (!sph_worker_read(i, actors, out SphW w)) continue;
                bool is_t = i == tgt;
                bool watch = _sph_trial >= 0 && w.chunk == _sph_chunk && SphSockets.Watch(SphTrial.T[_sph_trial].script, w.local);   // always listed (review S3: Djose's 62 / 64, not yet captured)
                if (!is_t && !watch && (!w.block || (w.ab & 1) == 0 || w.type < 1 || w.type > 3)) continue;   // S1+2: only the talk-enabled ones (the spheres' own workers filled the list)
                float d = Vector3.Distance(w.pos, tp);
                if (!is_t && !watch && !(d < 120f)) continue;
                n++;
                float ang = MathF.Atan2(w.pos.Z - tp.Z, w.pos.X - tp.X);
                L(I($"worker {w.gi}{(is_t ? " (THE TALK TARGET)" : "")} local {w.local} {sph_chunk_name(w.chunk)} type {w.type} +0xAB 0x{w.ab:X2} +0xAA {w.aa} +0x34 0x{w.f34:X4} +0x36 0x{w.f36:X4} | pos ({w.pos.X:F2},{w.pos.Y:F2},{w.pos.Z:F2}) d Tidus {d:F1} dL {sph_d(hok[0], palm[0], w.pos)} dR {sph_d(hok[1], palm[1], w.pos)} bearing {ang:F2} | range +0x40 {w.range:F2} angle tol +0x3C {w.atol:F3} height tol +0x2C {w.htol:F2} touch +0x48 {w.touch:F2} contact h +0x1C {w.contact_h:F2} p2 ({w.p2.X:F1},{w.p2.Y:F1},{w.p2.Z:F1}) | actor #{w.actor_slot}{(w.actor_slot >= 0 && snap != null ? " '" + actor_name(snap[w.actor_slot].chr_name) + "'" : "")}"));
            }
        }
        // the actors: every object (chr_id type 5) and any actor within 40 units of Tidus or a hand
        if (snap != null) {
            float* wp = stackalloc float[128 * 3]; int nwp = _fn_world_positions != null ? _fn_world_positions(wp, 128) : 0;
            bool shared = false;
            for (int i = 0; i < 48; i++) {
                if (i == ti || !live(snap[i])) continue;
                Actor a = snap[i]; byte* r = (byte*)(snap + i);
                Vector3 p = new Vector3(a.chr_pos_vec.X, a.chr_pos_vec.Y, a.chr_pos_vec.Z);
                bool obj = ((a.chr_id >> 12) & 0xF) == 5;
                float d = Vector3.Distance(p, tp);
                bool near = d < 40f || (hok[0] && Vector3.Distance(p, palm[0]) < 40f) || (hok[1] && Vector3.Distance(p, palm[1]) < 40f);
                if (!obj && !near) continue;
                if (d < 0.5f) shared = true;
                nint par = (nint)(*(uint*)(r + 0x19C)); nint model = (nint)(*(uint*)(r + 0x830)); nint ovr = (nint)(*(uint*)(r + 0x10C));
                byte mv = 0xFF; if (model > 0x10000 && model < 0x7FFF0000) read((byte*)model + 0x14, out mv);
                Vector3 dr = *(Vector3*)(r + 0x200);
                // the nearest skinned draw (render space -> game space) to its drawn root
                float bd = float.MaxValue; Vector3 bp = default;
                for (int k = 0; k < nwp; k++) { Vector3 q = new Vector3(-wp[k * 3], -wp[k * 3 + 1], wp[k * 3 + 2]); float e = Vector3.DistanceSquared(q, dr); if (e < bd) { bd = e; bp = q; } }
                Vector4 o0 = a.chr_optpos[0];
                L(I($"actor #{i} id 0x{a.chr_id:X4} '{actor_name(a.chr_name)}'{(obj ? " OBJECT" : "")} root ({p.X:F2},{p.Y:F2},{p.Z:F2}) d Tidus {d:F2} dL {sph_d(hok[0], palm[0], p)} dR {sph_d(hok[1], palm[1], p)} | hide +0x180 0x{r[0x180]:X2} f194 0x{*(uint*)(r + 0x194):X8} f198 0x{*(uint*)(r + 0x198):X8} worker +0x190 {*(ushort*)(r + 0x190)} | parent +0x19C {(par == 0 ? "-" : RideLogic.ParentIndex((long)actors, (long)par, sizeof(Actor), 48, i) is int pix && pix >= 0 ? pix.ToString(CI) : "ext")} bone +0x1A0 {*(int*)(r + 0x1A0)} +0x1B8 {*(int*)(r + 0x1B8)} override +0x10C 0x{(ulong)ovr:X} | drawn root ({dr.X:F2},{dr.Y:F2},{dr.Z:F2}) nearest draw {(nwp > 0 ? I($"({bp.X:F1},{bp.Y:F1},{bp.Z:F1}) {MathF.Sqrt(bd):F2} off") : "-")} | model 0x{(ulong)model:X} vis {mv:X2} res +0x1C4 0x{*(uint*)(r + 0x1C4):X8} | optpos0 ({o0.X:F1},{o0.Y:F1},{o0.Z:F1}) scale ({a.chr_scale_vec.X:F2}) rot {a.chr_rotation_rad:F2}"));
                if (obj) sph_keys_line(L, i, model, a.chr_name);
            }
            // a shared root (the carry seen 7 Nov): Tidus's sub-positions, to find the hand points; and the skinned draws near him
            if (shared && ti >= 0) {
                var sb = new System.Text.StringBuilder("Tidus sub-positions (an object shares his root; index (x,y,z) up from the root):");
                for (int k = 0; k < 26; k++) { Vector4 v = snap[ti].chr_optpos[k]; if (v.X == 0f && v.Y == 0f && v.Z == 0f) continue; sb.Append(I($" {k} ({v.X:F1},{v.Y:F1},{v.Z:F1}) {tp.Y - v.Y:F1};")); }
                L(sb.ToString());
            }
            var sw = new System.Text.StringBuilder("skinned draws within 30 units of Tidus (game space):");
            int nd = 0;
            for (int k = 0; k < nwp && nd < 16; k++) { Vector3 q = new Vector3(-wp[k * 3], -wp[k * 3 + 1], wp[k * 3 + 2]); float e = Vector3.Distance(q, tp); if (e < 30f) { nd++; sw.Append(I($" ({q.X:F1},{q.Y:F1},{q.Z:F1}) {e:F1};")); } }
            L(nd > 0 ? sw.ToString() : "skinned draws within 30 units of Tidus: none");
        }
    }
    /// <summary>An object actor's model index-buffer keys (what body_hide matches draws by): once per map, slot and model object.</summary>
    private void sph_keys_line(Action<string> L, int slot, nint model, char* name) {
        if (model == 0 || _fn_safe_read == null) return;
        long key = ((long)slot << 40) ^ (long)model;
        if (_sph_keys_logged.Contains(key) || _sph_keys_logged.Count > 200) return;
        _sph_keys_logged.Add(key);
        if (_sph_ibq == null) { _sph_ibq = (uint*)Marshal.AllocHGlobal(64 * 16); _sph_ibseg = (byte*)Marshal.AllocHGlobal(32 * 0x6C); }   // kept for the session
        int n = 0; int segs = IbChain.Walk(model, 1, _sph_ibq, ref n, 64, _sph_ibseg, 32, _fn_safe_read, out bool fail);
        var sb = new System.Text.StringBuilder(I($"  keys of #{slot} '{actor_name(name)}': {segs} segments{(fail ? " (a read failed)" : "")}:"));
        for (int e = 0; e < n && e < 16; e++) sb.Append(I($" {_sph_ibq[e * 4]:X8}+{_sph_ibq[e * 4 + 1]}/{_sph_ibq[e * 4 + 2]}"));
        L(sb.ToString());
    }

    // -- the text windows ---------------------------------------------------------------------------------------------
    private void sph_text_lines(Action<string> L, in GameGates.Text tx) {
        L(I($"text: render windows open 0x{tx.open:X2} shown 0x{tx.shown:X2} | choice {(tx.choice_slot >= 0 ? I($"slot {tx.choice_slot} row {tx.choice_row} (mask 0x{tx.choice:X2})") : "none")}"));
        if (_sph_blk == null) return;
        uint blk_va = (uint)(nint)FhUtil.ptr_at<byte>(GameGates.k_blk);
        byte* tb = stackalloc byte[160];
        fixed (byte* b = _sph_blk) {
            for (int i = 0; i < 8; i++) {
                if ((tx.open & (1 << i)) == 0) continue;
                uint p = *(uint*)(b + GameGates.k_win_tab + i * 4); long o = (long)p - blk_va;
                if (o < 0 || o + GameGates.k_win_len > GameGates.k_win_tab) continue;
                byte* w = b + o;
                uint tp = *(uint*)(w + 0x94);
                string cur = "";
                uint cp = *(uint*)(b + GameGates.k_cur_tab + i * 4); long co = (long)cp - blk_va;
                if (co >= GameGates.k_cur_lo && co + GameGates.k_cur_len <= GameGates.k_cur_tab) { byte* c = b + co; cur = I($" | cursor mode {c[1]} row {(sbyte)c[2]} items {c[5]}"); }
                string txt = "-";
                if (tp != 0) { for (int k = 0; k < 160; k++) tb[k] = 0; if (_fn_safe_read((void*)(nint)tp, tb, 160) != 0) txt = sph_decode(tb, 160); }
                L(I($"render window {i}: id {*(ushort*)(w + 0x04)} state {*(ushort*)(w + 0x2C)} x {*(float*)(w + 0x48):F0} y {*(float*)(w + 0x4C):F0} w {*(ushort*)(w + 0x44)} h {*(ushort*)(w + 0x46)}{cur} | page text 0x{tp:X8}: \"{txt}\""));
            }
        }
        // the script side (TOMesWinWork): its own text pointer per window of the current bank
        if (!read(FhUtil.ptr_at<sbyte>(0xF26B82), out sbyte bank) || bank < 0 || bank > 15) return;
        byte* mw = stackalloc byte[8 * 0x2C];
        if (_fn_safe_read(FhUtil.ptr_at<byte>(0xF26D70) + bank * 8 * 0x2C, mw, 8 * 0x2C) == 0) return;
        for (int i = 0; i < 8; i++) {
            byte* m = mw + i * 0x2C; ushort st = *(ushort*)(m + 0x14); if (st == 0) continue;
            uint tp = *(uint*)(m + 0x08); string txt = "-";
            if (tp != 0) { for (int k = 0; k < 160; k++) tb[k] = 0; if (_fn_safe_read((void*)(nint)tp, tb, 160) != 0) txt = sph_decode(tb, 160); }
            L(I($"script window {i} (bank {bank}): state {st} flags 0x{m[0x1D]:X2} +0x20 {m[0x20]} | text 0x{tp:X8}: \"{txt}\""));
        }
    }
    /// <summary>The game's text encoding, its ASCII part (index = byte - 0x30): enough to read a sphere's name. Control codes are
    /// shown in braces ({03} a new line, {10 3n} choice row n, {0B g} a button glyph, {12..23 x} a substitution); 0 ends it.</summary>
    private const string k_sph_charset = "0123456789 !\"#$%&'()*+,-./:;<=>?ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
    private static string sph_decode(byte* p, int n) {
        var sb = new System.Text.StringBuilder();
        for (int i = 0; i < n && sb.Length < 200; i++) {
            byte b = p[i];
            if (b == 0) break;
            if (b >= 0x30) { int k = b - 0x30; sb.Append(k < k_sph_charset.Length ? k_sph_charset[k] : '~'); continue; }
            if (b >= 0x2B) { sb.Append(I($"{{{b:X2} {(i + 1 < n ? p[i + 1] : 0):X2}}}")); i++; continue; }   // a two-byte glyph outside ASCII
            bool two = b == 0x04 || b == 0x0A || b == 0x0B || (b >= 6 && b <= 14) || b == 16 || (b >= 18 && b <= 42);
            if (two && i + 1 < n) { sb.Append(I($"{{{b:X2} {p[i + 1]:X2}}}")); i++; }
            else sb.Append(I($"{{{b:X2}}}"));
        }
        return sb.ToString();
    }

    /// <summary>F8 > Developer > Inspect: the checkbox and the state.</summary>
    private void sph_ui() {
        ui_check("diag_sphere", "Sphere research log (diag_sphere): in ffxvr_diag.log, the sphere / socket / talk / text state (\"sph:\" lines), anywhere; in a Cloister of Trials it runs with any diagnostic on", ref _diag_sphere);
        ImGui.TextDisabled(esc(I($"sph: map {_sph_map} {(_sph_trial >= 0 ? "(" + SphTrial.T[_sph_trial].name + "'s cloister)" : "(not a cloister)")} | {(_diag_sphere || (_sph_trial >= 0 && diag_any()) ? "logging" : _sph_active ? "no log (read for sphere hands only)" : "off")} | compact lines {_sph_lines} of {k_sph_lines_max}, snapshots {_sph_full} of {k_sph_full_max}")));
        sphh_dev_ui();   // sphere hands: the target, the prediction, the sequence (sphere_hands.cs)
    }
}

/// <summary>The Cloisters of Trials and their state variables (declaration location 6: chunk + [chunk+0x20] + offset). Scripts,
/// rooms, offsets, element types and the sphere values are the FFX screen reader's cloister contracts (facts; their trial
/// designs and solver data), the names of values only where it states them; the capture fills the rest.</summary>
internal readonly struct SphTrial {
    public readonly string script, name; public readonly int room, lo, n, size, carried; public readonly int[] vals; public readonly string[] names;
    SphTrial(string script, string name, int room, int lo, int n, int size, int carried, int[] vals, string[] names) {
        this.script = script; this.name = name; this.room = room; this.lo = lo; this.n = n; this.size = size; this.carried = carried; this.vals = vals; this.names = names;
    }
    public static readonly SphTrial[] T = {
        new("bsyt0100", "Besaid",    122, 0xAC, 17, 4, 0xAC, new[] { 1, 2, 3 }, new[] { "Besaid Sphere", "Glyph Sphere", "Destruction Sphere" }),
        new("klyt1000", "Kilika",    108, 0x58, 11, 1, 0x5B, new[] { 6 }, new[] { "Destruction Sphere" }),
        new("djyt0600", "Djose",     214, 0x94, 13, 1, 0x94, new[] { 1, 3, 10 }, new[] { "Djose Sphere", "Charged Djose Sphere", "Destruction Sphere" }),   // logs9; 3's name a guess until the game's text shows it
        new("mcyt0500", "Macalania", 239, 0x94, 10, 1, 0x94, new[] { 4, 10, 11 }, new[] { "Macalania Sphere", "Destruction Sphere", "Glyph Sphere" }),
        new("bvyt1100", "Bevelle",   306, 0x32, 72, 1, 0x32 + 61, new[] { 1 }, new[] { "Destruction Sphere" }),
        new("dome0700", "Zanarkand", 320, 0x08, 6, 4, 0x08, new[] { 1, 2, 3 }, new[] { "Besaid Sphere", "Kilika Sphere", "Destruction Sphere" }),
        new("dome0200", "Zanarkand (dome0200)", -1, 0, 0, 1, 0, Array.Empty<int>(), Array.Empty<string>()),
    };
    /// <summary>The trial of a map's base name ("djyt0600"), -1 none.</summary>
    public static int Find(string map_base) {
        for (int i = 0; i < T.Length; i++) if (string.Equals(T[i].script, map_base, StringComparison.Ordinal)) return i;
        return -1;
    }
    public static string Name(int trial, int v) {
        if (v == 0) return "none";
        if (v < 0 || trial < 0) return "?";
        var t = T[trial];
        for (int i = 0; i < t.vals.Length; i++) if (t.vals[i] == v) return t.names[i];
        return "unknown: the capture names it";
    }
}
