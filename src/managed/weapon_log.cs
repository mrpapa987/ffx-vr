// SPDX-License-Identifier: MIT

namespace FFXVR;

// The weapon-hands capture (research/weapon-hands-design.md; the feature: in a first-person battle the followed character's
// weapon is drawn in the VR hand while he stands idle). A LOG-ONLY research channel: every read guarded (native safe_read) or of
// the exe's own statics, nothing written to the game, no hook. ffxvr_diag.log, lines "wpn: ...".
//
// When: in a battle (Btl.battle_state != 0) with F8 > Developer > Inspect > "Weapon research log" (ini diag_weapon, default 0).
// What:
//   - a full snapshot at the battle's start, on each change of the followed character or of the view, and every 10 s: the
//     battle's turn / command state, both VR hands' frames, every party member (chr 0..7, and the ball / doll records 0x1C /
//     0x1D) with its motion bytes and flags, every weapon record of theirs (both hands) with its actor, attachment and root
//     matrix, any other 'w' model or actor attached to a party member, each weapon model's index-buffer keys (once per battle),
//     the followed character's hand bones (once per battle) and weapon bone chain;
//   - an event block on each change of a party member's motion (Chr+0x3F0 / +0x3F1, the actor's motion id, death, a thrown
//     weapon): that member's line and weapons; for the followed one also the hands and the bone chain;
//   - after a snapshot or an event of the followed character, the next frame's draws of the weapon models (native's probe,
//     armed for that frame only): how many, each one's constant-buffer size, world matrix and first palette slot, and how far
//     that is from the actor's drawn root;
//   - a compact line every 0.5 s (unchanged: one every 5 s): the followed character's motion, its class and the candidate
//     "weapon in the hand" rule, the weapon's root, each hand's distance from it.
//   Bounded: 8000 lines, 300 snapshots and 2000 event blocks a session; at most 8 event blocks a second.
//
// Game facts used (installed build, module offsets = VA - 0x400000; checked in our exe's disassembly, dis_full.txt and the
// Ghidra export; names from Fahrenheit where it has them, no code taken):
//   [0xD334D4] the Chr array, 0xF90 each (MsGetChr 0x794020): +0x00 the actor, +0x04 the model id, +0x0E chr_id, +0x10 in this
//              battle, +0x13 the chr record of its weapon when the weapon has one (0x1C Wakka's ball, 0x1D Lulu's doll, 0x1E
//              Yojimbo's; 0xFF none; 0x795BB0), +0x15..+0x1D visibility bytes (+0x1D: the weapon shows, 0x797080), +0x20 the
//              motion set, +0x3F0 the standard battle motion playing and +0x3F1 the idle in effect (ffxvr_weapon.h), +0x3F3
//              a motion lock, +0x408 / +0x409 attack / return motion type, +0x40A, +0x412 move flag, +0x434 the motion has
//              ended, +0x448 wait-motion flag, +0x4B5 the weapon rides the hand (0x7938E0), +0x4B6 / +0x4B7 hand 0 / 1's
//              weapon is off the hand (thrown: 0x793F10 detaches, 0x793330 attaches again), +0x592 / +0x593 the weapon /
//              armour inventory index (the model of hand 0 / hand 1), +0x6E4 hp, +0xDCC stat_death, +0xDCE stone, +0xDD6.
//   [0xD3446C] the weapon records, 0x390 each, index chr + 0x1F * hand (0x795B10; MsGetChr ids 0x1F..0x5C): +0x00 the weapon's
//              actor (Ch_Allocate 0x824F90 in the same actor table), +0x04 its model id (0x4xxx), +0x0D the attach point on
//              the owner (14 for hand 0, 15 for hand 1: the Chr initialiser 0x79C110), +0x0E 0x5000 / 0x5100 + chr.
//   actor:     +0x19C the parent, +0x1A0 the parent's bone, +0x1A4 the offset, +0x1B4 the scale (the attach 0x832630 looks the
//              point up in the parent model's table, 0x833A70), +0x1C0 -> +0x0A u16 the bone count, +0x1D0 the root matrix
//              (4 x 4 for row vectors, translation +0x200; 0x832760: attached = scale x the parent's bone x the parent's
//              root, the override pointer +0x10C is used only when not attached), +0x328 the bone records (0x160 each: +0x00
//              the parent record, +0x88 its matrix in model space, translation in 1 / 1000), +0x524 the sub-positions (the
//              attach points as world positions, 0x8350F0), +0x72C the motion id playing (Ch_SetMotion 0x837AC0), +0x830
//              the model object (its index-buffer keys: IbChain).
//   battle:    0xD2A8E0 battle_state, 0x1FCC088 the chr choosing a command, 0x1FCC08C its mask, 0xD2BDE0 / 0xD2BDE1 the input /
//              attack cue counts, 0xD2AC70 the first attack cue (+0 the attacker), 0xD2C895 the roster.
public sealed unsafe partial class FFXVRModule {

    private bool _diag_weapon;   // ini diag_weapon (default 0; Developer): the weapon research log in battles
    private delegate* unmanaged[Cdecl]<uint*, int, int> _fn_wpn_probe;                   // ABI 65: the weapon models' keys, armed for a frame
    private delegate* unmanaged[Cdecl]<float*, int, uint*, uint*, int> _fn_wpn_report;   // ABI 65: that frame's matched draws
    private delegate* unmanaged[Cdecl]<float*, int, int> _fn_wpn_hands;                  // ABI 65: both hands' frames (render space)
    private delegate* unmanaged[Cdecl]<int, int, int> _fn_wpn_phase;                     // ABI 65: a battle motion's class | 0x100 the hold rule

    private const int k_wpn_chr_n = 8, k_wpn_chr_size = 0xF90, k_wpn_rec_size = 0x390, k_wpn_bone_size = 0x160, k_wpn_bones_max = 256;
    private const int k_wpn_lines_max = 8000, k_wpn_full_max = 300, k_wpn_ev_max = 2000, k_wpn_keys_max = 64, k_wpn_recs = 40;
    private static readonly string[] k_wpn_phase = { "none", "idle", "weak", "guard", "attack", "magic", "skill", "item", "move", "hurt", "dead", "win", "sleep", "special", "other" };
    private bool _wpn_active; private int _wpn_battle, _wpn_lines, _wpn_full, _wpn_ev, _wpn_sec_n, _wpn_follow_prev = -2, _wpn_mode_prev = -1;
    private double _wpn_sec_t, _wpn_compact_t, _wpn_compact_sig_t, _wpn_full_t; private long _wpn_compact_sig;
    private readonly long[] _wpn_sig_prev = new long[k_wpn_chr_n];
    private byte* _wpn_chr, _wpn_rec, _wpn_bones, _wpn_ibseg, _wpn_tmp; private uint* _wpn_q, _wpn_pq; private float* _wpn_rep;
    private readonly Dictionary<long, uint[]> _wpn_keys = new();     // this battle: (slot, model object) -> its segment keys (buffer, offset, count each)
    private readonly List<long> _wpn_key_ids = new();                // this event: the key sets to arm
    private readonly HashSet<int> _wpn_bones_logged = new();         // this battle: the chrs whose hand bones were dumped
    private bool _wpn_probe_due; private ulong _wpn_probe_at; private int _wpn_probe_keys;
    private int _wpn_fid = -1;   // the followed chr for the log: the battle follow's, else the party member whose actor the eyes are on

    /// <summary>settings_parse's default branch: diag_weapon. False: not ours.</summary>
    private bool wpn_parse(string k, string v) {
        if (k != "diag_weapon") return false;
        pb(k, v, ref _diag_weapon); return true;
    }
    private void wpn_bind(Func<string, nint> req) {
        _fn_wpn_probe = (delegate* unmanaged[Cdecl]<uint*, int, int>)req("ffxvr_wpn_probe");
        _fn_wpn_report = (delegate* unmanaged[Cdecl]<float*, int, uint*, uint*, int>)req("ffxvr_wpn_report");
        _fn_wpn_hands = (delegate* unmanaged[Cdecl]<float*, int, int>)req("ffxvr_wpn_hands");
        _fn_wpn_phase = (delegate* unmanaged[Cdecl]<int, int, int>)req("ffxvr_wpn_phase");
    }
    private void wpn_unbind() { _fn_wpn_probe = null; _fn_wpn_report = null; _fn_wpn_hands = null; _fn_wpn_phase = null; }

    private void wpn_line(string s) {
        if (_wpn_lines >= k_wpn_lines_max) return;
        _wpn_lines++;
        d7_log("wpn: " + s + (_wpn_lines == k_wpn_lines_max ? " (no more wpn: lines this session)" : ""));
    }
    private void wpn_reset() {
        _wpn_active = false; _wpn_fid = -1; _wpn_follow_prev = -2; _wpn_mode_prev = -1; _wpn_compact_sig = 0; _wpn_probe_due = false;
        Array.Clear(_wpn_sig_prev); _wpn_keys.Clear(); _wpn_bones_logged.Clear();
        if (_fn_wpn_probe != null) _fn_wpn_probe(null, 0);
    }

    /// <summary>From push_all, every frame (after push_player: the follow is this frame's). Off: one test. On, out of battle:
    /// one static read. In battle: one guarded read of the 8 party Chr records (32 KB) and the frame's actor-table snapshot.</summary>
    private void wpn_tick() {
        if (!_diag_weapon) { if (_wpn_active) wpn_reset(); return; }
        if (_fn_safe_read == null || _fn_diag_line == null) return;
        sbyte bst = FhUtil.get_at<sbyte>(0xD2A8E0);
        if (bst == 0) {
            if (_wpn_active) { wpn_line(I($"=== battle {_wpn_battle} ended (frame {_frames}, t {_now:F2})")); wpn_reset(); }
            return;
        }
        if (_wpn_chr == null) {   // kept for the session
            _wpn_chr = (byte*)Marshal.AllocHGlobal(k_wpn_chr_n * k_wpn_chr_size); _wpn_rec = (byte*)Marshal.AllocHGlobal(2 * k_wpn_chr_n * k_wpn_rec_size);
            _wpn_bones = (byte*)Marshal.AllocHGlobal(k_wpn_bones_max * k_wpn_bone_size); _wpn_ibseg = (byte*)Marshal.AllocHGlobal(32 * 0x6C);
            _wpn_tmp = (byte*)Marshal.AllocHGlobal(2 * k_wpn_chr_size); _wpn_q = (uint*)Marshal.AllocHGlobal(k_wpn_keys_max * 16); _wpn_pq = (uint*)Marshal.AllocHGlobal(k_wpn_keys_max * 16);
            _wpn_rep = (float*)Marshal.AllocHGlobal(k_wpn_recs * 32 * sizeof(float));
        }
        string? why = null; bool full = false;
        if (!_wpn_active) { _wpn_active = true; _wpn_battle++; why = I($"battle {_wpn_battle} start"); full = true; _wpn_full_t = _now; }
        if (_wpn_probe_due && _frames > _wpn_probe_at) wpn_draws();
        nint chrs = FhUtil.get_at<nint>(0xD334D4);
        bool chr_ok = chrs != 0 && _fn_safe_read((void*)chrs, _wpn_chr, (uint)(k_wpn_chr_n * k_wpn_chr_size)) != 0;
        Actor* actors = Globals.actors; Actor* snap = tbl_snap(actors);
        _wpn_fid = _btl_follow_id >= 0 && _btl_follow_id < k_wpn_chr_n ? _btl_follow_id : -1;
        if (_wpn_fid < 0 && chr_ok && _fp_actor_idx >= 0)   // the battle follow off: the party member the field pick is on
            for (int id = 0; id < k_wpn_chr_n; id++) if (wpn_slot(actors, (nint)(*(uint*)(_wpn_chr + id * k_wpn_chr_size))) == _fp_actor_idx) { _wpn_fid = id; break; }
        if (_wpn_fid != _wpn_follow_prev) { why = wpn_why(why, I($"followed chr {_wpn_follow_prev} -> {_wpn_fid}")); full = true; _wpn_follow_prev = _wpn_fid; }
        if (_fp_mode != _wpn_mode_prev) { why = wpn_why(why, I($"view {_wpn_mode_prev} -> {_fp_mode}")); full = true; _wpn_mode_prev = _fp_mode; }
        if (_now - _wpn_full_t >= 10.0 || _now < _wpn_full_t) { why = wpn_why(why, "10 s"); full = true; }
        int changed = 0;
        if (chr_ok) for (int id = 0; id < k_wpn_chr_n; id++) {
            byte* c = _wpn_chr + id * k_wpn_chr_size;
            long sig = 0;
            if (*(uint*)c != 0) {
                byte* a = wpn_actor((nint)(*(uint*)c), actors, snap, null, out _);
                uint am = a != null ? *(uint*)(a + 0x72C) : 0u;
                sig = 1L | (long)c[0x3F0] << 8 | (long)c[0x3F1] << 16 | (long)(c[0xDCC] & 0xF) << 24 | (long)(c[0x4B6] & 1) << 28 | (long)(c[0x4B7] & 1) << 29 | (long)am << 32;
            }
            if (sig != _wpn_sig_prev[id]) { if (_wpn_sig_prev[id] != 0 && sig != 0) changed |= 1 << id; _wpn_sig_prev[id] = sig; }
        }
        if (_now - _wpn_sec_t >= 1.0 || _now < _wpn_sec_t) { _wpn_sec_t = _now; _wpn_sec_n = 0; }
        if (full && _wpn_full < k_wpn_full_max) { _wpn_full++; _wpn_full_t = _now; wpn_snapshot(why!, chr_ok, chrs, actors, snap, bst, 0xFF, true); }
        else if (changed != 0 && _wpn_ev < k_wpn_ev_max && _wpn_sec_n < 8) { _wpn_ev++; _wpn_sec_n++; wpn_snapshot(wpn_changed_text(changed), chr_ok, chrs, actors, snap, bst, changed, false); }
        else if (_now - _wpn_compact_t >= 0.5 || _now < _wpn_compact_t) { _wpn_compact_t = _now; wpn_compact(chr_ok, actors, snap, bst); }
    }
    private static string wpn_why(string? a, string b) => a == null ? b : a + "; " + b;
    private string wpn_changed_text(int mask) {
        var sb = new System.Text.StringBuilder("motion change:");
        for (int id = 0; id < k_wpn_chr_n; id++) if ((mask & (1 << id)) != 0) { byte* c = _wpn_chr + id * k_wpn_chr_size; sb.Append(I($" chr {id} -> 0x{c[0x3F0]:X2} ({wpn_phase_text(c[0x3F0], c[0x3F1])})")); }
        return sb.ToString();
    }
    private string wpn_phase_text(int motion, int idle) {
        if (_fn_wpn_phase == null) return "?";
        int p = _fn_wpn_phase(motion, idle), k = p & 0xFF;
        return (k < k_wpn_phase.Length ? k_wpn_phase[k] : "?") + ((p & 0x100) != 0 ? ", HOLD" : "");
    }
    private bool wpn_hold(int motion, int idle) => _fn_wpn_phase != null && (_fn_wpn_phase(motion, idle) & 0x100) != 0;

    // -- reads ----------------------------------------------------------------------------------------------------------
    private static int wpn_slot(Actor* actors, nint p) {
        if (actors == null || p == 0) return -1;
        long d = (long)p - (long)(nint)actors;
        return d >= 0 && d % sizeof(Actor) == 0 && d / sizeof(Actor) < 48 ? (int)(d / sizeof(Actor)) : -1;
    }
    /// <summary>An actor's bytes: the frame's table snapshot when it is one of the 48 slots, else a guarded read into buf (0x880
    /// bytes; null: no read). Null when it does not read. slot = its table slot or -1.</summary>
    private byte* wpn_actor(nint p, Actor* actors, Actor* snap, byte* buf, out int slot) {
        slot = wpn_slot(actors, p);
        if (slot >= 0 && snap != null) return (byte*)(snap + slot);
        if (p == 0 || buf == null || _fn_safe_read((void*)p, buf, (uint)sizeof(Actor)) == 0) return null;
        return buf;
    }
    private static Vector3 wpn_v3(byte* p) => new Vector3(*(float*)p, *(float*)(p + 4), *(float*)(p + 8));
    private static string wpn_p(Vector3 v) => I($"({v.X:F2},{v.Y:F2},{v.Z:F2})");
    /// <summary>A 4 x 4 for row vectors (the game's actor and bone matrices): its three axis rows and the translation row.</summary>
    private static string wpn_m(byte* m, float t_scale = 1f) {
        float* f = (float*)m;
        return I($"[x ({f[0]:F3},{f[1]:F3},{f[2]:F3}) y ({f[4]:F3},{f[5]:F3},{f[6]:F3}) z ({f[8]:F3},{f[9]:F3},{f[10]:F3}) t ({f[12] * t_scale:F2},{f[13] * t_scale:F2},{f[14] * t_scale:F2})]");
    }
    /// <summary>v M for a row vector with w = 1 (the game's convention).</summary>
    private static Vector3 wpn_rowmul(Vector3 v, byte* m) {
        float* f = (float*)m;
        return new Vector3(v.X * f[0] + v.Y * f[4] + v.Z * f[8] + f[12], v.X * f[1] + v.Y * f[5] + v.Z * f[9] + f[13], v.X * f[2] + v.Y * f[6] + v.Z * f[10] + f[14]);
    }
    private struct WpnHands { public bool ok0, ok1, fp, latched; public Vector3 p0, p1, g0, g1, f0, f1, u0, u1, r0, r1; public float s, sq0, sq1, tr0, tr1; public int mode, drawn, why; }
    /// <summary>Both hands' frames in game space (render space with x and y negated): the palm's origin (p), the grip pose's (g)
    /// and the palm frame's axes: forward (its -Z, toward the fingers), up (+Y) and right (+X).</summary>
    private WpnHands wpn_hands() {
        var h = new WpnHands { mode = -1 };
        if (_fn_wpn_hands == null) return h;
        float* v = stackalloc float[64];
        for (int i = 0; i < 64; i++) v[i] = 0f;
        if (_fn_wpn_hands(v, 64) < 61) return h;
        static Vector3 g(float x, float y, float z) => new Vector3(-x, -y, z);
        for (int k = 0; k < 2; k++) {
            float* o = v + k * 28; bool ok = o[0] > 0.5f;
            Vector3 p = g(o[10], o[11], o[12]), gp = g(o[23], o[24], o[25]);
            Vector3 r = g(o[1], o[4], o[7]), u = g(o[2], o[5], o[8]), f = g(-o[3], -o[6], -o[9]);   // the frame's columns: X, Y and -Z
            if (k == 0) { h.ok0 = ok; h.p0 = p; h.g0 = gp; h.r0 = r; h.u0 = u; h.f0 = f; h.sq0 = o[26]; h.tr0 = o[27]; }
            else { h.ok1 = ok; h.p1 = p; h.g1 = gp; h.r1 = r; h.u1 = u; h.f1 = f; h.sq1 = o[26]; h.tr1 = o[27]; }
            if (ok) h.s = o[13];
        }
        h.fp = v[56] > 0.5f; h.mode = (int)v[57]; h.latched = v[58] > 0.5f; h.drawn = (int)v[59]; h.why = (int)v[60];
        return h;
    }
    private static string wpn_hand_text(bool ok, Vector3 p, Vector3 g, Vector3 f, Vector3 u, Vector3 r, float sq, float tr) =>
        ok ? I($"palm {wpn_p(p)} grip point {wpn_p(g)} fwd ({f.X:F3},{f.Y:F3},{f.Z:F3}) up ({u.X:F3},{u.Y:F3},{u.Z:F3}) right ({r.X:F3},{r.Y:F3},{r.Z:F3}) squeeze {sq:F2} trigger {tr:F2}") : "not tracked";
    private static string wpn_d(bool ok, Vector3 a, Vector3 b) => ok ? I($"{Vector3.Distance(a, b):F1}") : "-";

    // -- the snapshot / the event block ---------------------------------------------------------------------------------
    /// <summary>mask: the chrs to write (bit per chr 0..7; 0xFF with full = everything). full adds the battle line, the other
    /// weapon-like actors, the ball / doll records and the periodic parts.</summary>
    private void wpn_snapshot(string why, bool chr_ok, nint chrs, Actor* actors, Actor* snap, sbyte bst, int mask, bool full) {
        int no = full ? _wpn_full : _wpn_ev; string tag = full ? I($"[S{no}] ") : I($"[E{no}] ");
        void L(string s) => wpn_line(tag + s);
        WpnHands hd = wpn_hands();
        bool follow_in = _wpn_fid >= 0 && (mask & (1 << _wpn_fid)) != 0;
        L(I($"=== {why} | map {map_name()} frame {_frames} t {_now:F2} | battle_state 0x{bst:X2} roster [{_btl_roster[0]},{_btl_roster[1]},{_btl_roster[2]}] followed chr {_wpn_fid} (battle follow {_btl_follow_id}, actor #{_fp_actor_idx}) pending {_btl_pending_id} tag state {_btg_state} | view {_fp_mode}{(full && _wpn_full == k_wpn_full_max ? " (the last full snapshot this session)" : "")}"));
        if (full) wpn_battle_line(L);
        if (full || follow_in) {
            L(I($"hands (game space, {hd.s:F2} units = 1 m; base view {hd.mode}{(hd.fp ? " first person" : "")}; poses {(hd.latched ? "the tick's latch" : "live (VR hands off or not latched)")}; drawn bits {hd.drawn}, last hide reason {hd.why}): R {wpn_hand_text(hd.ok1, hd.p1, hd.g1, hd.f1, hd.u1, hd.r1, hd.sq1, hd.tr1)} | L {wpn_hand_text(hd.ok0, hd.p0, hd.g0, hd.f0, hd.u0, hd.r0, hd.sq0, hd.tr0)}"));
        }
        if (!chr_ok) { L(I($"the Chr array did not read (0x{(ulong)chrs:X})")); return; }
        nint wtab = FhUtil.get_at<nint>(0xD3446C);
        bool rec_ok = wtab != 0 && _fn_safe_read((void*)wtab, _wpn_rec, (uint)(k_wpn_chr_n * k_wpn_rec_size)) != 0
                                && _fn_safe_read((byte*)wtab + 0x1F * k_wpn_rec_size, _wpn_rec + k_wpn_chr_n * k_wpn_rec_size, (uint)(k_wpn_chr_n * k_wpn_rec_size)) != 0;
        if (!rec_ok && full) L(I($"the weapon records did not read ([0xD3446C] = 0x{(ulong)wtab:X})"));
        _wpn_key_ids.Clear();
        byte* abuf = stackalloc byte[sizeof(Actor)]; byte* obuf = stackalloc byte[sizeof(Actor)];
        ulong listed = 0;   // the table slots written as a member or a weapon
        for (int id = 0; id < k_wpn_chr_n; id++) {
            if ((mask & (1 << id)) == 0) continue;
            byte* c = _wpn_chr + id * k_wpn_chr_size;
            if (*(uint*)c == 0 && c[0x10] == 0) continue;
            byte* a = wpn_actor((nint)(*(uint*)c), actors, snap, abuf, out int slot);
            if (slot >= 0) listed |= 1UL << slot;
            wpn_member_line(L, id, c, a, slot, hd);
            if (!rec_ok) continue;
            for (int hand = 0; hand < 2; hand++) {
                byte* r = _wpn_rec + (hand * k_wpn_chr_n + id) * k_wpn_rec_size;
                if (*(uint*)r == 0 && *(uint*)(r + 4) == 0) continue;
                byte* w = wpn_actor((nint)(*(uint*)r), actors, snap, obuf, out int ws);
                if (ws >= 0) listed |= 1UL << ws;
                wpn_weapon_line(L, I($"weapon of chr {id} hand {hand}: record +0x04 model 0x{*(uint*)(r + 4):X} +0x08 0x{*(uint*)(r + 8):X} point +0x0D {r[0x0D]} id +0x0E 0x{*(ushort*)(r + 0x0E):X4} +0x15 {r[0x15]} +0x34 {r[0x34]} +0x3A {r[0x3A]} | "),
                                (nint)(*(uint*)r), w, ws, r[0x0D], actors, snap, hd, id == _wpn_fid);
            }
            if (id == _wpn_fid && a != null) wpn_bones(L, id, a, rec_ok ? _wpn_rec + id * k_wpn_rec_size : null, actors, snap, obuf, full);
        }
        if (full) {
            // the ball's and the doll's own Chr records (ids 0x1C, 0x1D)
            if (_fn_safe_read((byte*)chrs + 0x1C * k_wpn_chr_size, _wpn_tmp, (uint)(2 * k_wpn_chr_size)) != 0)
                for (int k = 0; k < 2; k++) {
                    byte* c = _wpn_tmp + k * k_wpn_chr_size;
                    if (*(uint*)c == 0) continue;
                    byte* a = wpn_actor((nint)(*(uint*)c), actors, snap, abuf, out int slot);
                    wpn_member_line(L, 0x1C + k, c, a, slot, hd);
                }
            // any other 'w' model, or actor riding a party member's bone, in the table
            if (snap != null) for (int i = 0; i < 48; i++) {
                if ((listed & (1UL << i)) != 0 || !live(snap[i])) continue;
                byte* w = (byte*)(snap + i);
                int ps = wpn_slot(actors, (nint)(*(uint*)(w + 0x19C)));
                bool on_member = ps >= 0 && (listed & (1UL << ps)) != 0;
                if (((snap[i].chr_id >> 12) & 0xF) != 4 && !on_member) continue;
                wpn_weapon_line(L, I($"other weapon-like actor: "), (nint)(actors + i), w, i, -1, actors, snap, hd, false);
            }
        }
        if (follow_in) wpn_arm();
    }

    private void wpn_battle_line(Action<string> L) {
        short turn = FhUtil.get_at<short>(0x1FCC088); uint cmd = FhUtil.get_at<uint>(0x1FCC08C);
        byte n_in = FhUtil.get_at<byte>(0xD2BDE0), n_att = FhUtil.get_at<byte>(0xD2BDE1), att0 = FhUtil.get_at<byte>(0xD2AC70), att0_n = FhUtil.get_at<byte>(0xD2AC73);
        string mod = _r7u_seq != 0 ? I($"turn pointer snapshot chr {_r7u.chr} ({_r7_since_u} Presents old); target mask 0x{_r7h.mask:X7} ({_r7_since_t} Presents old)") : "no turn / target snapshot (those hooks are not installed)";
        L(I($"battle: chr choosing [0x1FCC088] {turn} mask [0x1FCC08C] 0x{cmd:X} ui gate [0x858EF8] {FhUtil.get_at<uint>(0x858EF8)} auto-battle {FhUtil.get_at<uint>(0xF3D6E0)} | input cues {n_in} attack cues {n_att} (the first: attacker {att0}, commands {att0_n}) | mod: {mod}"));
    }

    /// <summary>One party member (or weapon chr record): the Chr bytes c, its actor a (null: not readable).</summary>
    private void wpn_member_line(Action<string> L, int id, byte* c, byte* a, int slot, in WpnHands hd) {
        var sb = new System.Text.StringBuilder();
        int on = -1; for (int i = 0; i < 3; i++) if (_btl_roster[i] == id) on = i;
        sb.Append(I($"chr 0x{id:X2}{(id == _wpn_fid ? " (FOLLOWED)" : "")} {(on >= 0 ? I($"field slot {on}") : "not on the roster")} model +0x04 0x{*(uint*)(c + 4):X} in battle +0x10 {c[0x10]} +0xDC8 {c[0xDC8]} | "));
        if (a == null) sb.Append(I($"actor 0x{*(uint*)c:X8} {(*(uint*)c == 0 ? "none" : "not readable")}"));
        else {
            Vector3 pos = wpn_v3(a + 0x0C), root = wpn_v3(a + 0x200);
            sb.Append(I($"actor #{slot} '{actor_name(*(char**)(a + 4))}' id 0x{*(ushort*)a:X4} pos {wpn_p(pos)} drawn root {wpn_p(root)} rot {*(float*)(a + 0x158):F2} hide +0x180 0x{a[0x180]:X2} alpha +0x354 {*(float*)(a + 0x354):F2} | actor motion +0x72C 0x{*(uint*)(a + 0x72C):X8} +0x730 {*(int*)(a + 0x730)} flags +0x194 0x{*(uint*)(a + 0x194):X8}"));
            if (hd.ok1) sb.Append(I($" | R hand {Vector3.Distance(hd.p1, root):F1} from the root"));
        }
        sb.Append(I($" | motion +0x3F0 0x{c[0x3F0]:X2} ({wpn_phase_text(c[0x3F0], c[0x3F1])}) idle +0x3F1 0x{c[0x3F1]:X2} set +0x20 {c[0x20]} num +0x4BB {c[0x4BB]} attack type +0x408 {c[0x408]} return +0x409 {c[0x409]}"));
        sb.Append(I($" | lock +0x3F3 {c[0x3F3]} +0x40A {c[0x40A]} move +0x412 {c[0x412]} +0x432 {c[0x432]} +0x433 {c[0x433]} ended +0x434 {c[0x434]} wait +0x448 {c[0x448]} action +0xDD6 {c[0xDD6]} cmd type +0x446 {c[0x446]} win +0x43B {c[0x43B]}"));
        sb.Append(I($" | death +0xDCC {c[0xDCC]} stone +0xDCE {c[0xDCE]} hp +0x6E4 {*(int*)(c + 0x6E4)} | vis +0x15..+0x1D {c[0x15]:X2} {c[0x16]:X2} {c[0x17]:X2} {c[0x18]:X2} {c[0x19]:X2} {c[0x1A]:X2} {c[0x1B]:X2} {c[0x1C]:X2} {c[0x1D]:X2}"));
        sb.Append(I($" | weapon: rides the hand +0x4B5 {c[0x4B5]} off the hand +0x4B6 {c[0x4B6]} / {c[0x4B7]} own chr +0x13 0x{c[0x13]:X2} owner +0x14 0x{c[0x14]:X2} inventory +0x592 {c[0x592]} / {c[0x593]}"));
        L(sb.ToString());
    }

    /// <summary>One weapon actor w (null: not readable): attachment, flags, root matrix, the checks against the owner's bone and
    /// sub-position, the hands' distances; its model's keys once per battle. point: the record's attach point (-1 none).</summary>
    private void wpn_weapon_line(Action<string> L, string head, nint ptr, byte* w, int slot, int point, Actor* actors, Actor* snap, in WpnHands hd, bool arm) {
        var sb = new System.Text.StringBuilder(head);
        if (w == null) { sb.Append(I($"actor 0x{(ulong)ptr:X8} {(ptr == 0 ? "none (not created)" : "not readable")}")); L(sb.ToString()); return; }
        nint par = (nint)(*(uint*)(w + 0x19C)); int ps = wpn_slot(actors, par), bone = *(int*)(w + 0x1A0);
        nint model = (nint)(*(uint*)(w + 0x830)); byte mv = 0xFF; if (model > 0x10000 && model < 0x7FFF0000) read((byte*)model + 0x14, out mv);
        Vector3 root = wpn_v3(w + 0x200);
        sb.Append(I($"actor {(slot >= 0 ? I($"#{slot}") : I($"0x{(ulong)ptr:X8} (outside the 48 slots)"))} '{actor_name(*(char**)(w + 4))}' id 0x{*(ushort*)w:X4} | parent +0x19C {(par == 0 ? "-" : ps >= 0 ? I($"#{ps}") : "ext")} bone +0x1A0 {bone} offset +0x1A4 ({*(float*)(w + 0x1A4):F2},{*(float*)(w + 0x1A8):F2},{*(float*)(w + 0x1AC):F2}) scale +0x1B4 {*(float*)(w + 0x1B4):F3} +0x1B8 {*(int*)(w + 0x1B8)}"));
        sb.Append(I($" | hide +0x180 0x{w[0x180]:X2} alpha +0x354 {*(float*)(w + 0x354):F2} override +0x10C 0x{*(uint*)(w + 0x10C):X} flags +0x194 0x{*(uint*)(w + 0x194):X8} +0x198 0x{*(uint*)(w + 0x198):X8} | model +0x830 0x{(ulong)model:X} vis {mv:X2} res +0x1C4 0x{*(uint*)(w + 0x1C4):X8} motion +0x72C 0x{*(uint*)(w + 0x72C):X8}"));
        sb.Append(I($" | pos +0x0C {wpn_p(wpn_v3(w + 0x0C))} scale +0x5C {*(float*)(w + 0x5C):F3} root +0x1D0 {wpn_m(w + 0x1D0)}"));
        // the owner's bone and sub-position against the weapon's root (the attach rule 0x832760 / 0x8350F0, read back)
        if (ps >= 0 && snap != null) {
            byte* o = (byte*)(snap + ps);
            nint bt = (nint)(*(uint*)(o + 0x328)); byte* bm = stackalloc byte[64];
            if (bt != 0 && bone >= 0 && bone < k_wpn_bones_max && _fn_safe_read((byte*)bt + bone * k_wpn_bone_size + 0x88, bm, 64) != 0) {
                Vector3 bw = wpn_rowmul(wpn_v3(bm + 48) * 1000f, o + 0x1D0);
                sb.Append(I($" | owner's bone {bone} model space {wpn_m(bm, 1000f)} -> world {wpn_p(bw)}, {Vector3.Distance(bw, root):F2} from the weapon's root"));
            }
            else sb.Append(I($" | owner's bone {bone}: not readable (table 0x{(ulong)bt:X})"));
            if (point >= 0 && point < 26) { Vector3 sp = wpn_v3(o + 0x524 + 16 * point); sb.Append(I($"; owner's sub-position {point} {wpn_p(sp)}, {Vector3.Distance(sp, root):F2} from it")); }
            sb.Append(I($"; owner's root +0x1D0 {wpn_m(o + 0x1D0)}"));
        }
        sb.Append(I($" | from the hands' palms: R {wpn_d(hd.ok1, hd.p1, root)} L {wpn_d(hd.ok0, hd.p0, root)}"));
        L(sb.ToString());
        if (slot >= 0) wpn_keys(L, slot, model, *(char**)(w + 4), arm);
    }

    /// <summary>A weapon model's index-buffer keys (IbChain: what the body hide matches draws by): read and logged once per battle,
    /// slot and model object; arm adds them to the next probe.</summary>
    private void wpn_keys(Action<string> L, int slot, nint model, char* name, bool arm) {
        if (model == 0) return;
        long id = ((long)slot << 40) ^ (long)model;
        if (!_wpn_keys.TryGetValue(id, out uint[]? keys)) {
            if (_wpn_keys.Count >= 64) return;
            int n = 0; int segs = IbChain.Walk(model, (uint)slot, _wpn_q, ref n, 16, _wpn_ibseg, 32, _fn_safe_read, out bool fail);
            keys = new uint[n * 3];
            var sb = new System.Text.StringBuilder(I($"  keys of #{slot} '{actor_name(name)}': {segs} segments{(fail ? " (a read failed)" : "")}:"));
            for (int e = 0; e < n; e++) { keys[e * 3] = _wpn_q[e * 4]; keys[e * 3 + 1] = _wpn_q[e * 4 + 1]; keys[e * 3 + 2] = _wpn_q[e * 4 + 2]; sb.Append(I($" {_wpn_q[e * 4]:X8}+{_wpn_q[e * 4 + 1]}/{_wpn_q[e * 4 + 2]}")); }
            if (!fail || n > 0) _wpn_keys[id] = keys;   // (a chain that did not read at all is tried again at the next snapshot)
            L(sb.ToString());
        }
        if (arm && keys.Length > 0 && !_wpn_key_ids.Contains(id)) _wpn_key_ids.Add(id);
    }

    /// <summary>Arms native's draw probe for the next frame with the followed character's weapon keys (tag = the actor's slot).</summary>
    private void wpn_arm() {
        if (_fn_wpn_probe == null || _wpn_key_ids.Count == 0) return;
        int n = 0;
        foreach (long id in _wpn_key_ids) {
            if (!_wpn_keys.TryGetValue(id, out uint[]? keys)) continue;
            for (int e = 0; e + 2 < keys.Length && n < k_wpn_keys_max; e += 3) { _wpn_pq[n * 4] = keys[e]; _wpn_pq[n * 4 + 1] = keys[e + 1]; _wpn_pq[n * 4 + 2] = keys[e + 2]; _wpn_pq[n * 4 + 3] = (uint)(id >> 40); n++; }
        }
        if (n == 0) return;
        _wpn_probe_keys = _fn_wpn_probe(_wpn_pq, n); _wpn_probe_due = true; _wpn_probe_at = _frames;
    }

    /// <summary>The frame after an armed one: the weapon models' draws as native saw them, each against its actor's drawn root
    /// as it reads now (game space).</summary>
    private void wpn_draws() {
        _wpn_probe_due = false;
        if (_fn_wpn_report == null) return;
        uint frame = 0, total = 0; int n = _fn_wpn_report(_wpn_rep, k_wpn_recs, &frame, &total);
        if (_fn_wpn_probe != null) _fn_wpn_probe(null, 0);
        Actor* actors = Globals.actors; Actor* snap = tbl_snap(actors);
        wpn_line(I($"draws: armed at frame {_wpn_probe_at} with {_wpn_probe_keys} keys; native's frame {frame}: {total} matched draws{(total == 0 ? " (none: the model was not drawn, was drawn another way than DrawIndexed from index 0, or its buffers changed)" : "")}"));
        for (int i = 0; i < n && i < 16; i++) {
            float* o = _wpn_rep + i * 32; int tag = (int)o[0], fl = (int)o[5];
            Vector3 root = default; bool have = false; string nm = "?";
            if (snap != null && tag >= 0 && tag < 48) { root = *(Vector3*)((byte*)(snap + tag) + 0x200); have = true; nm = actor_name(snap[tag].chr_name); }
            var sb = new System.Text.StringBuilder(I($"  draw {(int)o[2]} of #{tag} '{nm}': key {(int)o[1]} count {(int)o[3]}{((fl & 4) != 0 ? " (NOT the key's count)" : "")}{((fl & 8) != 0 ? "" : " no depth target")}{((fl & 16) != 0 ? " LIGHT CAMERA (a shadow caster)" : "")}"));
            if ((fl & 1) == 0) sb.Append(" | no eye image for its slot-0 buffer (not a camera buffer the mod saw this frame)");
            else {
                Vector3 wt = new Vector3(-o[8 + 3], -o[8 + 7], o[8 + 11]);
                sb.Append(I($" | buffer {(int)o[4]} B{((fl & 2) != 0 ? " (skinned layout)" : " (NOT the skinned layout: the offset is a guess)")} world t {wpn_p(wt)}{(have ? I($" {Vector3.Distance(wt, root):F2} from the drawn root") : "")} rows (render space) [({o[8]:F3},{o[9]:F3},{o[10]:F3}) ({o[12]:F3},{o[13]:F3},{o[14]:F3}) ({o[16]:F3},{o[17]:F3},{o[18]:F3})]"));
                if ((fl & 32) != 0) {
                    Vector3 pt = new Vector3(-o[20 + 3], -o[20 + 7], o[20 + 11]);
                    sb.Append(I($" | palette {(int)o[6]} slots at +{(int)o[7]}, slot 0 t {wpn_p(pt)}{(have ? I($" {Vector3.Distance(pt, root):F2} from the drawn root") : "")} rows [({o[20]:F3},{o[21]:F3},{o[22]:F3}) ({o[24]:F3},{o[25]:F3},{o[26]:F3}) ({o[28]:F3},{o[29]:F3},{o[30]:F3})]"));
                }
                else sb.Append(I($" | palette: {(int)o[6]} slots"));
            }
            wpn_line(sb.ToString());
        }
        if (n > 16) wpn_line(I($"  ({n - 16} more draws not written)"));
    }

    /// <summary>The followed character's bones: the weapon bone's chain to the root on every block; once per battle the hand
    /// bones (the chain's first four and every bone that hangs off them: the fingers) with their matrices, in model space.</summary>
    private void wpn_bones(Action<string> L, int id, byte* a, byte* rec0, Actor* actors, Actor* snap, byte* wbuf, bool full) {
        nint bt = (nint)(*(uint*)(a + 0x328)); nint info = (nint)(*(uint*)(a + 0x1C0));
        if (bt == 0 || info == 0 || !read((byte*)info + 0x0A, out ushort nb) || nb == 0) { L(I($"bones of chr {id}: no bone table (+0x328 0x{(ulong)bt:X}, +0x1C0 0x{(ulong)info:X})")); return; }
        int n = Math.Min((int)nb, k_wpn_bones_max);
        if (_fn_safe_read((void*)bt, _wpn_bones, (uint)(n * k_wpn_bone_size)) == 0) { L(I($"bones of chr {id}: the table did not read (0x{(ulong)bt:X}, {nb} bones)")); return; }
        int bone = -1;
        if (rec0 != null && *(uint*)rec0 != 0) { byte* w = wpn_actor((nint)(*(uint*)rec0), actors, snap, wbuf, out _); if (w != null && *(uint*)(w + 0x19C) == *(uint*)(_wpn_chr + id * k_wpn_chr_size)) bone = *(int*)(w + 0x1A0); }
        int parent(int b) { long d = (long)(*(uint*)(_wpn_bones + b * k_wpn_bone_size)) - (long)bt; return d >= 0 && d % k_wpn_bone_size == 0 && d / k_wpn_bone_size < n ? (int)(d / k_wpn_bone_size) : -1; }
        if (bone < 0 || bone >= n) { L(I($"bones of chr {id}: {nb} bones; hand 0's weapon is not on this actor (no chain)")); return; }
        Span<int> chain = stackalloc int[8]; int cn = 0;
        for (int b = bone; b >= 0 && cn < 8; b = parent(b)) chain[cn++] = b;
        var sb = new System.Text.StringBuilder(I($"bones of chr {id} ({nb} bones, model space, t x 1000): the weapon bone's chain:"));
        for (int k = 0; k < cn; k++) sb.Append(I($" b{chain[k]} {wpn_m(_wpn_bones + chain[k] * k_wpn_bone_size + 0x88, 1000f)}{(k + 1 < cn ? " <-" : "")}"));
        L(sb.ToString());
        if (!full || _wpn_bones_logged.Contains(id)) return;
        _wpn_bones_logged.Add(id);
        // the bones that hang off the chain's second to fourth (the weapon's chain root, the hand, the forearm): fingers and helpers
        var sc = new System.Text.StringBuilder(I($"hand bones of chr {id} (children of"));
        int top = Math.Min(cn, 4);
        for (int k = 1; k < top; k++) sc.Append(I($" b{chain[k]}"));
        sc.Append("):"); int shown = 0;
        for (int b = 0; b < n && shown < 24; b++) {
            int p = parent(b); bool hit = false;
            for (int k = 1; k < top; k++) if (p == chain[k]) hit = true;
            for (int k = 0; k < cn; k++) if (b == chain[k]) hit = false;
            if (!hit) continue;
            shown++; sc.Append(I($" b{b}(of b{p}) {wpn_m(_wpn_bones + b * k_wpn_bone_size + 0x88, 1000f)};"));
        }
        L(sc.ToString());
        // every bone's parent, 60 a line (the skeleton's shape, to name the joints offline)
        for (int b0 = 0; b0 < n; b0 += 60) {
            var sp = new System.Text.StringBuilder(I($"bone parents of chr {id} from b{b0}:"));
            for (int b = b0; b < n && b < b0 + 60; b++) sp.Append(I($" {parent(b)}"));
            L(sp.ToString());
        }
    }

    // -- the compact line -----------------------------------------------------------------------------------------------
    private void wpn_compact(bool chr_ok, Actor* actors, Actor* snap, sbyte bst) {
        if (_wpn_lines >= k_wpn_lines_max) return;
        int id = _wpn_fid;
        var sb = new System.Text.StringBuilder(I($"t {_now:F2} f {_frames} state 0x{bst:X2} view {_fp_mode} | "));
        long sig = bst * 31L + _fp_mode * 7L + id * 131L;
        WpnHands hd = wpn_hands();
        if (!chr_ok || id < 0 || id >= k_wpn_chr_n) sb.Append(I($"followed chr {id}: not a party member read here"));
        else {
            byte* c = _wpn_chr + id * k_wpn_chr_size;
            byte* a = wpn_actor((nint)(*(uint*)c), actors, snap, null, out int slot);
            bool hold = wpn_hold(c[0x3F0], c[0x3F1]);
            sb.Append(I($"chr {id} #{slot} motion 0x{c[0x3F0]:X2} ({wpn_phase_text(c[0x3F0], c[0x3F1])}) actor motion 0x{(a != null ? *(uint*)(a + 0x72C) : 0u):X8} death {c[0xDCC]} off the hand {c[0x4B6]}/{c[0x4B7]}"));
            sig = sig * 31 + c[0x3F0] + (hold ? 977 : 0) + c[0xDCC] * 13 + (a != null ? *(uint*)(a + 0x72C) : 0u);
            nint wtab = FhUtil.get_at<nint>(0xD3446C); uint wp = 0;
            if (wtab != 0 && read((byte*)wtab + id * k_wpn_rec_size, out wp) && wp != 0) {
                int ws = wpn_slot(actors, (nint)wp);
                if (ws >= 0 && snap != null) {
                    byte* w = (byte*)(snap + ws); Vector3 root = wpn_v3(w + 0x200);
                    sb.Append(I($" | weapon #{ws} root {wpn_p(root)} hide 0x{w[0x180]:X2} parent {wpn_slot(actors, (nint)(*(uint*)(w + 0x19C)))} | palms from it: R {wpn_d(hd.ok1, hd.p1, root)} L {wpn_d(hd.ok0, hd.p0, root)}"));
                    sig = sig * 31 + ws + w[0x180] * 17 + (long)(root.X * 2f) + (long)(root.Y * 2f) * 3 + (long)(root.Z * 2f) * 5;
                }
                else sb.Append(I($" | weapon actor 0x{wp:X8} outside the 48 slots"));
            }
            else sb.Append(" | no weapon actor (hand 0)");
        }
        sb.Append(I($" | R palm {(hd.ok1 ? wpn_p(hd.p1) : "-")} L palm {(hd.ok0 ? wpn_p(hd.p0) : "-")} | choosing {FhUtil.get_at<short>(0x1FCC088)} mask 0x{FhUtil.get_at<uint>(0x1FCC08C):X} cues {FhUtil.get_at<byte>(0xD2BDE0)}/{FhUtil.get_at<byte>(0xD2BDE1)} attacker {FhUtil.get_at<byte>(0xD2AC70)}"));
        sig = sig * 31 + FhUtil.get_at<uint>(0x1FCC08C) + FhUtil.get_at<byte>(0xD2BDE1) * 7 + (hd.ok1 ? (long)(hd.p1.X * 2f) + (long)(hd.p1.Y * 2f) * 3 + (long)(hd.p1.Z * 2f) * 5 : 0);
        if (sig == _wpn_compact_sig && _now - _wpn_compact_sig_t < 5.0 && _now >= _wpn_compact_sig_t) return;   // unchanged: one every 5 s
        _wpn_compact_sig = sig; _wpn_compact_sig_t = _now;
        wpn_line(sb.ToString());
    }

    /// <summary>F8 > Developer > Inspect: the checkbox and the state.</summary>
    private void wpn_ui() {
        ui_check("diag_weapon", "Weapon research log (diag_weapon): in ffxvr_diag.log, in battles: the party's motion state, their weapon actors, the weapon models' draws and the VR hands (\"wpn:\" lines)", ref _diag_weapon);
        ImGui.TextDisabled(esc(I($"wpn: {(_wpn_active ? I($"logging battle {_wpn_battle}") : _diag_weapon ? "waiting for a battle" : "off")} | lines {_wpn_lines} of {k_wpn_lines_max}, snapshots {_wpn_full} of {k_wpn_full_max}, motion events {_wpn_ev} of {k_wpn_ev_max}")));
    }
}
