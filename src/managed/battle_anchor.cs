// SPDX-License-Identifier: MIT

namespace FFXVR;

// The battle diorama's anchor (17 Nov 2026, branch dio-anchor; design-notes "Battle: only the combatants in the fight count").
// In battle the diorama (fp_mode 2) is anchored to the centre of the fight (ffxvr_anchor_override). Until this file the
// centre was the average of every live actor in the battle's player list, and that list also holds the party's bench:
// with a seven-member party the four members off the field stand at one far spot (the 16 / 17 Nov logs: (-7.85, 0,
// 3143.56) while the fight is within about 100 units of (0, 0, 0)), so the table sat over a thousand units off the fight.
//   Who counts: the party members on the game's field roster (three chr ids at k_roster_off, the roster the battle follow
//   uses: an aeon is on it during a summon, the party that walked off is not), whatever their visibility; the monsters
//   that are in the battle, shown by the game (the main vote +0x16 and the appear vote +0x17) and alive.
//   Outliers: a candidate farther than k_ba_radius from the candidates' median (per axis), or with a coordinate that is
//   not a finite number, is dropped; the centre is the average of the rest.
//   When: only while the battle is drawn (battle_state 0x16, 1, 2: the load chain has the fielded party at the far
//   spot too, the end states are not the fight any more); outside that, and when nothing is left to count, the last
//   centre of this battle is kept (none yet: the diorama stays on the player, as before).
//   The view at the start (ini battle_view_reset, default 1): the first time the centre is used in a battle, native puts
//   the diorama's offset, tilt and zoom back to their defaults (behind and above the fight), so nothing is left over
//   from the field or from the battle before; what is moved during the battle stays for that battle. The request rides
//   in ffxvr_anchor_override's "valid" argument (bit 0 valid, bit 1 reset, the battle's serial from bit 2 up): no new
//   export, ABI 70 kept.
//   The log (diag_verbose or diag_battle): one "diorama: battle anchor" line at the first centre of a battle and whenever
//   the set changes (40 a battle at most).
// The rule between the two "pure rule" marks is written in the subset of C# that is also C++ after a handful of textual
// replacements: the checks (dio_chk: gen.py, dio_test.cpp, check.py) build exactly these lines with g++ and test them.
public sealed unsafe partial class FFXVRModule {

    // --- pure rule begin ---
    internal const float k_ba_radius = 400.0f;   // units: the fight spans about 60 to 200, the bench stands about 3000 away
    internal const int k_ba_max = 32;            // candidates: 18 party slots + 11 monsters fit
    internal const int k_ba_ok = 0;              // counted
    internal const int k_ba_benched = 1;         // a party member (or an aeon) not on the field roster
    internal const int k_ba_no_actor = 2;        // the actor is not live
    internal const int k_ba_not_in = 3;          // a monster slot that is not in this battle (Chr+0x10 == 0)
    internal const int k_ba_hidden = 4;          // a monster the game hides (the main vote +0x16 or the appear vote +0x17 is 0)
    internal const int k_ba_dead = 5;            // a monster that is dead (stat_death Chr+0xDCC) or has no HP
    internal const int k_ba_nonfinite = 6;       // a coordinate is NaN or infinite
    internal const int k_ba_outlier = 7;         // farther than k_ba_radius from the candidates' median
    internal const int k_ba_log_max = 40;        // "diorama: battle anchor" lines a battle

    // The battle is drawn and fought: battle_state 0x16 (the intro, drawn in 3D), 1 and 2 (ej_bst's class 2).
    internal static bool ba_live(int bst) {
        return bst == 0x16 || bst == 1 || bst == 2;
    }
    // The field roster's three bytes can be used: not the battle before's (btl_roster_stale), no chr id twice.
    internal static bool ba_roster_ok(byte r0, byte r1, byte r2, int bst) {
        if (btl_roster_stale(bst)) return false;
        if (r0 == r1 && r0 != 0xFF) return false;
        if (r1 == r2 && r1 != 0xFF) return false;
        if (r0 == r2 && r0 != 0xFF) return false;
        return true;
    }
    // The field slot (0..2) a chr id stands in, -1: not on the field (0xFF is an empty slot, never an id).
    internal static int ba_roster_slot(int id, byte r0, byte r1, byte r2) {
        if (id < 0 || id >= 0xFF) return -1;
        if (id == r0) return 0;
        if (id == r1) return 1;
        if (id == r2) return 2;
        return -1;
    }
    // A party entity's verdict: on the roster and live, whatever its visibility (an effect may hide a front-liner for a moment).
    internal static int ba_ply_why(bool live, bool on_roster) {
        if (!on_roster) return k_ba_benched;
        if (!live) return k_ba_no_actor;
        return k_ba_ok;
    }
    // A monster slot's verdict: inb = Chr+0x10, vis = Chr+0x16, show = Chr+0x17, death = Chr+0xDCC, hp = the current HP.
    internal static int ba_mon_why(bool live, byte inb, byte vis, byte show, byte death, int hp) {
        if (!live) return k_ba_no_actor;
        if (inb == 0) return k_ba_not_in;
        if (vis == 0 || show == 0) return k_ba_hidden;
        if (death != 0 || hp <= 0) return k_ba_dead;
        return k_ba_ok;
    }
    // The median of v over the candidates still counted (why == k_ba_ok); tmp holds n values at least. None: 0.
    internal static float ba_median(float[] v, int[] why, int n, float[] tmp) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            if (why[i] == k_ba_ok) { tmp[m] = v[i]; m++; }
        }
        if (m == 0) return 0.0f;
        for (int i = 1; i < m; i++) {
            float t = tmp[i];
            int j = i - 1;
            while (j >= 0 && tmp[j] > t) { tmp[j + 1] = tmp[j]; j--; }
            tmp[j + 1] = t;
        }
        if ((m & 1) != 0) return tmp[m / 2];
        return (tmp[m / 2 - 1] + tmp[m / 2]) * 0.5f;
    }
    // The centre of the fight. In: n candidates (positions, why = the verdict so far). A counted candidate with a
    // coordinate that is not finite becomes k_ba_nonfinite, one farther than radius from the median of the rest
    // k_ba_outlier. Returns how many are counted and their average in c (c[0..2]; untouched when the result is 0).
    internal static int ba_select(float[] x, float[] y, float[] z, int[] why, int n, float radius, float[] tmp, float[] c) {
        for (int i = 0; i < n; i++) {
            if (why[i] != k_ba_ok) continue;
            if (!(float.IsFinite(x[i]) && float.IsFinite(y[i]) && float.IsFinite(z[i]))) why[i] = k_ba_nonfinite;
        }
        float mx = ba_median(x, why, n, tmp);
        float my = ba_median(y, why, n, tmp);
        float mz = ba_median(z, why, n, tmp);
        float sx = 0.0f;
        float sy = 0.0f;
        float sz = 0.0f;
        int m = 0;
        for (int i = 0; i < n; i++) {
            if (why[i] != k_ba_ok) continue;
            float dx = x[i] - mx;
            float dy = y[i] - my;
            float dz = z[i] - mz;
            if (!(dx * dx + dy * dy + dz * dz <= radius * radius)) { why[i] = k_ba_outlier; continue; }
            sx += x[i]; sy += y[i]; sz += z[i]; m++;
        }
        if (m == 0) return 0;
        c[0] = sx / m; c[1] = sy / m; c[2] = sz / m;
        return m;
    }
    // A battle's serial: 1 .. 0xFFFFF, never 0 (native starts at 0: no battle seen).
    internal static int ba_next_serial(int s) {
        if (s < 0) return 1;
        return (s % 0xFFFFF) + 1;
    }
    // ffxvr_anchor_override's "valid" for a centre: bit 0 valid, bit 1 the view starts from its defaults, the serial from bit 2.
    internal static int ba_valid_arg(bool reset, int serial) {
        return 1 | (reset ? 2 : 0) | (serial << 2);
    }
    // --- pure rule end ---

    private bool _battle_view_reset = true;   // ini battle_view_reset: a battle in the diorama starts from the default view
    private readonly float[] _ba_x = new float[k_ba_max], _ba_y = new float[k_ba_max], _ba_z = new float[k_ba_max], _ba_tmp = new float[k_ba_max], _ba_c = new float[3];
    private readonly int[] _ba_why = new int[k_ba_max], _ba_tag = new int[k_ba_max];   // tag: a party entity's chr id, a monster's 0x100 + slot
    private int _ba_n;                        // candidates of the last collection
    private bool _ba_was;                     // in battle last frame
    private int _ba_serial;                   // this battle's serial (ba_next_serial)
    private bool _ba_have;                    // a centre was found in this battle (_ba_c, game space)
    private bool _ba_held;                    // this frame's centre is the kept one
    private int _ba_party, _ba_mons;          // counted at the last collection
    private string _ba_note = "";             // why the centre is kept or missing ("" = computed this frame)
    private int _ba_sig; private bool _ba_sig_set; private int _ba_logs;   // the logged set, the lines this battle

    private static string ba_why_name(int w) =>
        w == k_ba_ok ? "counted" : w == k_ba_benched ? "benched" : w == k_ba_no_actor ? "no live actor" : w == k_ba_not_in ? "not in the battle"
        : w == k_ba_hidden ? "hidden" : w == k_ba_dead ? "dead" : w == k_ba_nonfinite ? "not a finite position" : w == k_ba_outlier ? "outlier" : "?";
    private int ba_count(int w) { int k = 0; for (int i = 0; i < _ba_n; i++) if (_ba_why[i] == w) k++; return k; }

    /// <summary>In battle the diorama anchors to the centre of the fight: the party members on the field roster and the
    /// monsters in the battle (the file's head has the rule). From push_view_rules, every frame.</summary>
    private void push_battle_anchor() {
        if (_fn_anchor_override == null) return;
        _battle_anchor_n = 0; _ba_held = false;
        Btl* btl = Globals.Battle.btl; bool in_battle = read(btl, out Btl btl_v) && btl_v.battle_state != 0;
        if (in_battle != _ba_was) {
            _ba_was = in_battle; _ba_have = false; _ba_n = 0; _ba_party = 0; _ba_mons = 0; _ba_sig_set = false; _ba_logs = 0;
            if (in_battle) _ba_serial = ba_next_serial(_ba_serial);
        }
        if (!in_battle) { _ba_note = ""; _fn_anchor_override(0, 0, 0, 0); return; }
        if (!_battle_anchor) { _ba_note = "off (battle_anchor 0)"; _fn_anchor_override(0, 0, 0, 0); return; }
        int bst = btl_v.battle_state;
        if (!ba_live(bst)) _ba_note = "not the fight now (the battle is loading, or it has ended)";
        else if (!ba_collect(bst)) _ba_note = "the field roster cannot be used now";
        else {
            int m = ba_select(_ba_x, _ba_y, _ba_z, _ba_why, _ba_n, k_ba_radius, _ba_tmp, _ba_c);
            _ba_party = 0; _ba_mons = 0;
            for (int i = 0; i < _ba_n; i++) if (_ba_why[i] == k_ba_ok) { if (_ba_tag[i] < 0x100) _ba_party++; else _ba_mons++; }
            if (m > 0) { _ba_have = true; _ba_note = ""; }
            else _ba_note = "no combatant left to count";
            ba_log(bst, m);
        }
        _ba_held = _ba_note.Length > 0;
        if (!_ba_have) { _fn_anchor_override(0, 0, 0, 0); return; }
        _battle_anchor_n = _ba_party + _ba_mons;
        _fn_anchor_override(-_ba_c[0], -_ba_c[1], _ba_c[2], ba_valid_arg(_battle_view_reset, _ba_serial));   // render space negates X and Y
    }
    /// <summary>The candidates of this frame into _ba_x / _ba_y / _ba_z / _ba_why / _ba_tag (game space): every party entity
    /// with an actor (on the roster or benched), every monster slot with an actor. False: the roster cannot be used.</summary>
    private bool ba_collect(int bst) {
        byte r0 = 0xFF, r1 = 0xFF, r2 = 0xFF; bool roster_ok = true;
        try { r0 = FhUtil.get_at<byte>(k_roster_off); r1 = FhUtil.get_at<byte>(k_roster_off + 1); r2 = FhUtil.get_at<byte>(k_roster_off + 2); } catch { roster_ok = false; }
        if (!roster_ok || !ba_roster_ok(r0, r1, r2, bst)) return false;
        Chr* mons = Globals.Battle.monster_characters; Chr* plys = Globals.Battle.player_characters;
        int n = 0; bool t0 = false, t1 = false, t2 = false;   // a roster slot is taken by the first entity with its chr id (as the battle follow's ply_of)
        if (plys != null) {
            for (int i = 0; i < k_ply_n && n < k_ba_max; i++) {
                if (!read(plys + i, out Chr pc) || pc.actor == null || !read(pc.actor, out Actor pa)) continue;
                int id = pc.chr_id;
                int slot = ba_roster_slot(id, r0, r1, r2);
                bool on = (slot == 0 && !t0) || (slot == 1 && !t1) || (slot == 2 && !t2);
                bool lv = live(pa);
                if (on && lv) { if (slot == 0) t0 = true; else if (slot == 1) t1 = true; else t2 = true; }
                _ba_x[n] = pa.chr_pos_vec.X; _ba_y[n] = pa.chr_pos_vec.Y; _ba_z[n] = pa.chr_pos_vec.Z;
                _ba_tag[n] = id & 0xFF; _ba_why[n] = ba_ply_why(lv, on); n++;
            }
        }
        if (mons != null) {
            for (int i = 0; i < k_ej_mon_n && n < k_ba_max; i++) {   // 11 monster slots (eye_jump.cs: the allocator's block is 20 party + 11 monsters)
                if (!read(mons + i, out Chr mc) || mc.actor == null || !read(mc.actor, out Actor ma)) continue;
                byte* mb = (byte*)&mc;   // the copy: Fahrenheit's Chr is the game's 0xF90 bytes
                int hp = mc.ram.current_hp;
                _ba_x[n] = ma.chr_pos_vec.X; _ba_y[n] = ma.chr_pos_vec.Y; _ba_z[n] = ma.chr_pos_vec.Z;
                _ba_tag[n] = 0x100 + i; _ba_why[n] = ba_mon_why(live(ma), mb[k_ej_chr_in], mb[0x16], mb[k_ej_chr_show], mb[k_ej_chr_death], hp); n++;
            }
        }
        _ba_n = n;
        return true;
    }
    /// <summary>The "diorama: battle anchor" line (ffxvr_diag.log; diag_verbose or diag_battle): at the first collection of
    /// a battle and whenever the set (who is listed, and each one's verdict) changes; k_ba_log_max a battle.</summary>
    private void ba_log(int bst, int m) {
        if (!(_diag_verbose || _diag_battle) || _fn_diag_line == null) return;
        int sig = _ba_n;
        for (int i = 0; i < _ba_n; i++) sig = unchecked(sig * 31 + ((_ba_tag[i] << 4) | _ba_why[i]));
        if (_ba_sig_set && sig == _ba_sig) return;
        _ba_sig = sig; _ba_sig_set = true;
        if (_ba_logs >= k_ba_log_max) return;
        _ba_logs++;
        var sb = new System.Text.StringBuilder();
        sb.Append(I($"diorama: battle anchor: battle {_ba_serial}, battle_state 0x{bst:X2}, view {_fp_mode} | "));
        if (m > 0) sb.Append(I($"the centre ({_ba_c[0]:F1} {_ba_c[1]:F1} {_ba_c[2]:F1}) (game space) of {_ba_party} party + {_ba_mons} monster(s)"));
        else if (_ba_have) sb.Append(I($"nobody left to count: the last centre ({_ba_c[0]:F1} {_ba_c[1]:F1} {_ba_c[2]:F1}) is kept"));
        else sb.Append("nobody to count yet: the diorama stays on the player");
        sb.Append(I($"; dropped {_ba_n - _ba_party - _ba_mons} of {_ba_n} (a candidate over {k_ba_radius:F0} units from the median is an outlier) |"));
        for (int i = 0; i < _ba_n; i++) {
            if (i > 0) sb.Append(';');
            if (_ba_tag[i] < 0x100) sb.Append(I($" chr {_ba_tag[i]}")); else sb.Append(I($" monster {_ba_tag[i] - 0x100}"));
            sb.Append(I($" ({_ba_x[i]:F1} {_ba_y[i]:F1} {_ba_z[i]:F1}) {ba_why_name(_ba_why[i])}"));
        }
        if (_ba_logs == k_ba_log_max) sb.Append(" (no more of these lines this battle)");
        d7_log(sb.ToString());
    }
    /// <summary>F8, Developer tab, "Battle, overdrives and rendering": the "battle diorama:" line.</summary>
    private string ba_status() {
        if (!_ba_was) return "battle diorama: not in battle";
        if (!_ba_have) return "battle diorama: no centre yet" + (_ba_note.Length > 0 ? " (" + _ba_note + ")" : "");
        int drop = _ba_n - _ba_party - _ba_mons;
        string s = I($"battle diorama: combatants {_battle_anchor_n} counted ({_ba_party} party + {_ba_mons} monsters), {drop} dropped");
        if (drop > 0) s += I($" (benched {ba_count(k_ba_benched)}, hidden {ba_count(k_ba_hidden)}, dead {ba_count(k_ba_dead)}, outlier {ba_count(k_ba_outlier)}, other {ba_count(k_ba_no_actor) + ba_count(k_ba_not_in) + ba_count(k_ba_nonfinite)})");
        if (_ba_held) s += "; the centre is kept: " + _ba_note;
        return s;
    }
}
