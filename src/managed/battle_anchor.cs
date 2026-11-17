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
//   Outliers (review S2): the party defines the arena. A candidate with a coordinate that is not a finite number is
//   dropped; a party member farther than k_ba_radius (400) from the party's median (a newcomer still at the bench); an
//   enemy only when farther than k_ba_far (1500) from it, so the enemies can never drop the party and a far enemy is
//   still an enemy. With no party candidate the monsters' own median and k_ba_radius. Review round 2: when every
//   monster candidate is over k_ba_far from the party, the party is not trusted (an aeon still parked at the bench)
//   and nothing is counted; and the table's centre is the average of the counted brought to within k_ba_reach (150)
//   of the party's own centre, so far enemies do not pull it into empty space (they still give the start heading).
//   When: only while the battle is drawn (battle_state 0x16, 1, 2: the load chain has the fielded party at the far
//   spot too, the end states are not the fight any more); outside that, and when nothing is left to count, the table
//   stays where it is (none yet: the diorama stays on the player, as before). A battle begins when battle_state leaves
//   0, and also when it enters 4..7 from a later state (review S4: battles that follow each other with no frame at 0).
//   The table holds still (review S3): what is pushed is not the centre of each frame but a held place. It moves only
//   after the counted set changed (a kill, a swap, a summon) once the centre has stood still for half a second, or when
//   the centre has been over 40 units away for a second; then it glides there in 0.6 s. A run-in to attack and back
//   moves nothing. battle_anchor_smooth_ms still works on top (native smooths what is pushed; 0, the default, adds
//   nothing).
//   The view at the start (ini battle_view_reset, default 1): the first time the table is used in a battle, native
//   keeps the field's view, puts the diorama's offset, tilt and zoom back to their defaults and turns the heading to
//   look from the party toward the enemies (ffxvr_eye_event 4: the direction, sent with every centre that has both
//   groups); what is moved during the battle stays for that battle; when the battle ends the field's view is put back
//   (ffxvr_dioanchor.h). The request rides in ffxvr_anchor_override's "valid" argument (bit 0 valid, bit 1 reset, the
//   battle's serial from bit 2 up) and in a new kind of ffxvr_eye_event: no export changes shape, ABI 70 kept.
//   The log (diag_verbose or diag_battle): one "diorama: battle anchor" line at the first centre of a battle and whenever
//   the set changes (40 a battle at most).
// The rule between the two "pure rule" marks is written in the subset of C# that is also C++ after a handful of textual
// replacements: the checks (dio_chk: gen.py, dio_test.cpp, check.py) build exactly these lines with g++ and test them.
public sealed unsafe partial class FFXVRModule {

    // --- pure rule begin ---
    internal const float k_ba_radius = 400.0f;   // units: a party member this far from the party's median (no party: a monster from the monsters') is an outlier
    internal const float k_ba_far = 1500.0f;     // units: an enemy this far from the party's median is not an enemy of this fight (the bench stands about 3143 away)
    internal const int k_ba_max = 32;            // candidates: 18 party slots + 11 monsters fit
    internal const int k_ba_mon = 0x100;         // a candidate's tag: a party entity's chr id (under this), a monster's slot + this
    internal const int k_ba_ok = 0;              // counted
    internal const int k_ba_benched = 1;         // a party member (or an aeon) not on the field roster
    internal const int k_ba_no_actor = 2;        // the actor is not live
    internal const int k_ba_not_in = 3;          // a monster slot that is not in this battle (Chr+0x10 == 0)
    internal const int k_ba_hidden = 4;          // a monster the game hides (the main vote +0x16 or the appear vote +0x17 is 0)
    internal const int k_ba_dead = 5;            // a monster that is dead (stat_death Chr+0xDCC) or has no HP
    internal const int k_ba_nonfinite = 6;       // a coordinate is NaN or infinite
    internal const int k_ba_outlier = 7;         // too far (k_ba_radius / k_ba_far)
    internal const int k_ba_untrusted = 8;       // a party candidate, when every enemy is over k_ba_far from the party: it is the party that is elsewhere
    internal const float k_ba_reach = 150.0f;    // units: the table's centre is never farther than this from the party's centre
    internal const int k_ba_log_max = 40;        // "diorama: battle anchor" lines a battle
    // the table holds still (ba_hold_step)
    internal const float k_ba_band = 40.0f;      // units: the fight's centre may wander this far from the table's (a run-in, a kill) without moving it
    internal const float k_ba_away_s = 1.0f;     // s: ... farther than that for this long moves it
    internal const float k_ba_settle_s = 0.5f;   // s: after the counted set changed, the centre must have stood still this long before the table follows
    internal const float k_ba_wait_max_s = 3.0f; // s: ... or this long has passed since the change
    internal const float k_ba_glide_s = 0.6f;    // s: the table's move to a new centre (eased)
    internal const float k_ba_still = 0.05f;     // units a frame: the centre "stands still" under this
    internal const int k_ba_hold_n = 16;         // floats of the hold's state: 0..2 the table, 3..5 / 6..8 a glide's start / end, 9 its time (-1: none),
                                                 // 10 the time since a set change still waited on (-1: none), 11 how long the centre has stood still,
                                                 // 12..14 the centre last frame, 15 how long it has been outside the band

    // The battle is drawn and fought: battle_state 0x16 (the intro, drawn in 3D), 1 and 2 (ej_bst's class 2).
    internal static bool ba_live(int bst) {
        return bst == 0x16 || bst == 1 || bst == 2;
    }
    // A battle begins: prev = the battle_state of the frame before (0: no battle). From no battle, or (chained battles that
    // never pass 0) into the start of the load chain (4..7: btl_roster_stale) from any later state.
    internal static bool ba_new_battle(int prev, int bst) {
        if (bst == 0) return false;
        if (prev == 0) return true;
        return btl_roster_stale(bst) && !btl_roster_stale(prev);
    }
    // The field roster's three bytes can be used: not the battle before's (btl_roster_stale), no chr id twice, not empty.
    internal static bool ba_roster_ok(byte r0, byte r1, byte r2, int bst) {
        if (btl_roster_stale(bst)) return false;
        if (r0 == 0xFF && r1 == 0xFF && r2 == 0xFF) return false;
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
    // How many candidates are counted (why == k_ba_ok) with lo <= tag < hi.
    internal static int ba_counted(int[] why, int[] tag, int n, int lo, int hi) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            if (why[i] == k_ba_ok && tag[i] >= lo && tag[i] < hi) m++;
        }
        return m;
    }
    // The median of v over the counted candidates with lo <= tag < hi; tmp holds n values at least. None: 0.
    internal static float ba_median(float[] v, int[] why, int[] tag, int n, int lo, int hi, float[] tmp) {
        int m = 0;
        for (int i = 0; i < n; i++) {
            if (why[i] == k_ba_ok && tag[i] >= lo && tag[i] < hi) { tmp[m] = v[i]; m++; }
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
    // The centre of the fight. In: n candidates (positions, tag, why = the verdict so far). A counted candidate with a
    // coordinate that is not finite becomes k_ba_nonfinite. The party defines the arena: with a party candidate left, the
    // reference is the party's median (per axis); a party member farther than k_ba_radius from it is k_ba_outlier (a
    // newcomer still at the bench), a monster only when farther than k_ba_far (the enemies never drop the party, and a
    // far enemy is still part of the fight). With no party candidate: the monsters' median and k_ba_radius.
    // Review round 2, S1: a party of one or two is no outlier test of itself (an aeon still parked at the bench). When
    // there are monster candidates and every one of them is too far from the party, it is the party that is not in the
    // fight yet: the party candidates become k_ba_untrusted and nothing is counted (the caller's table stays).
    // Returns how many are counted; c (9 floats): 0..2 the table's centre, 3..5 the party's average, 6..8 the monsters'
    // (each group untouched when it has nobody). The centre is the average of all counted, brought to within k_ba_reach
    // of the party's average when there is a party (round 2, S2: far enemies would put it in empty space; they still
    // give the heading through 6..8).
    internal static int ba_select(float[] x, float[] y, float[] z, int[] why, int[] tag, int n, float[] tmp, float[] c) {
        for (int i = 0; i < n; i++) {
            if (why[i] != k_ba_ok) continue;
            if (!(float.IsFinite(x[i]) && float.IsFinite(y[i]) && float.IsFinite(z[i]))) why[i] = k_ba_nonfinite;
        }
        bool party = ba_counted(why, tag, n, 0, k_ba_mon) > 0;
        int qc = ba_counted(why, tag, n, k_ba_mon, 0x7FFFFFFF);
        int lo = party ? 0 : k_ba_mon;
        int hi = party ? k_ba_mon : 0x7FFFFFFF;
        float mx = ba_median(x, why, tag, n, lo, hi, tmp);
        float my = ba_median(y, why, tag, n, lo, hi, tmp);
        float mz = ba_median(z, why, tag, n, lo, hi, tmp);
        float px = 0.0f;
        float py = 0.0f;
        float pz = 0.0f;
        float qx = 0.0f;
        float qy = 0.0f;
        float qz = 0.0f;
        int pn = 0;
        int qn = 0;
        for (int i = 0; i < n; i++) {
            if (why[i] != k_ba_ok) continue;
            float r = (party && tag[i] >= k_ba_mon) ? k_ba_far : k_ba_radius;
            float dx = x[i] - mx;
            float dy = y[i] - my;
            float dz = z[i] - mz;
            if (!(dx * dx + dy * dy + dz * dz <= r * r)) { why[i] = k_ba_outlier; continue; }
            if (tag[i] < k_ba_mon) { px += x[i]; py += y[i]; pz += z[i]; pn++; }
            else { qx += x[i]; qy += y[i]; qz += z[i]; qn++; }
        }
        if (qc > 0 && qn == 0) {   // (with no party candidate nobody is counted here anyway)
            for (int i = 0; i < n; i++) {
                if (why[i] == k_ba_ok) why[i] = k_ba_untrusted;
            }
            return 0;
        }
        int m = pn + qn;
        if (m == 0) return 0;
        c[0] = (px + qx) / m; c[1] = (py + qy) / m; c[2] = (pz + qz) / m;
        if (pn > 0) { c[3] = px / pn; c[4] = py / pn; c[5] = pz / pn; }
        if (qn > 0) { c[6] = qx / qn; c[7] = qy / qn; c[8] = qz / qn; }
        if (pn > 0) {
            float ox = c[0] - c[3];
            float oy = c[1] - c[4];
            float oz = c[2] - c[5];
            float o2 = ox * ox + oy * oy + oz * oz;
            if (o2 > k_ba_reach * k_ba_reach) {
                float k = k_ba_reach / MathF.Sqrt(o2);
                c[0] = c[3] + ox * k; c[1] = c[4] + oy * k; c[2] = c[5] + oz * k;
            }
        }
        return m;
    }
    // The table (s: k_ba_hold_n floats): a battle's first centre. The table is put on it, and follows once it has stood
    // still (the first frames of the intro may have the fighters on the move).
    internal static void ba_hold_start(float[] s, float tx, float ty, float tz) {
        s[0] = tx; s[1] = ty; s[2] = tz;
        s[3] = tx; s[4] = ty; s[5] = tz;
        s[6] = tx; s[7] = ty; s[8] = tz;
        s[9] = -1.0f; s[10] = 0.0f; s[11] = 0.0f;
        s[12] = tx; s[13] = ty; s[14] = tz;
        s[15] = 0.0f;
    }
    // The table, one frame: t = this frame's centre of the fight, changed = the counted set is not last frame's, dt in s.
    // The table moves only (a) after a set change, once the centre has stood still k_ba_settle_s (or k_ba_wait_max_s after
    // the change): so a kill during a run-in is not taken where the attacker happened to be; (b) when the centre has been
    // more than k_ba_band away for k_ba_away_s. It then glides to the centre of that moment in k_ba_glide_s, eased; a move
    // of more than k_ba_radius is another place altogether (the fighters were not placed yet): put there at once, but
    // only when the centre has stood still k_ba_settle_s (round 2, S1); while it has not, the table stays and waits.
    internal static void ba_hold_step(float[] s, float tx, float ty, float tz, bool changed, float dt) {
        if (!(dt > 0.0f)) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        float ex = tx - s[12];
        float ey = ty - s[13];
        float ez = tz - s[14];
        if (ex * ex + ey * ey + ez * ez <= k_ba_still * k_ba_still) s[11] += dt;
        else s[11] = 0.0f;
        s[12] = tx; s[13] = ty; s[14] = tz;
        if (changed) { s[10] = 0.0f; s[15] = 0.0f; }
        float dx = tx - s[0];
        float dy = ty - s[1];
        float dz = tz - s[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        bool go = false;
        if (s[10] >= 0.0f) {
            s[10] += dt;
            if (s[11] >= k_ba_settle_s || s[10] >= k_ba_wait_max_s) { s[10] = -1.0f; go = true; }
        }
        else if (s[9] < 0.0f) {
            if (d2 > k_ba_band * k_ba_band) {
                s[15] += dt;
                if (s[15] >= k_ba_away_s) go = true;
            }
            else s[15] = 0.0f;
        }
        if (go) {
            s[15] = 0.0f;
            if (d2 > k_ba_radius * k_ba_radius) {
                if (s[11] >= k_ba_settle_s) { s[0] = tx; s[1] = ty; s[2] = tz; s[9] = -1.0f; }
                else s[10] = k_ba_wait_max_s;
            }
            else if (d2 > 0.0001f) {
                s[3] = s[0]; s[4] = s[1]; s[5] = s[2];
                s[6] = tx; s[7] = ty; s[8] = tz;
                s[9] = 0.0f;
            }
        }
        if (s[9] >= 0.0f) {
            s[9] += dt;
            float u = s[9] / k_ba_glide_s;
            if (u >= 1.0f) { s[0] = s[6]; s[1] = s[7]; s[2] = s[8]; s[9] = -1.0f; }
            else {
                float e = u * u * (3.0f - 2.0f * u);
                s[0] = s[3] + (s[6] - s[3]) * e;
                s[1] = s[4] + (s[7] - s[4]) * e;
                s[2] = s[5] + (s[8] - s[5]) * e;
            }
        }
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
    private readonly float[] _ba_x = new float[k_ba_max], _ba_y = new float[k_ba_max], _ba_z = new float[k_ba_max], _ba_tmp = new float[k_ba_max];
    private readonly float[] _ba_c = new float[9], _ba_h = new float[k_ba_hold_n];   // ba_select's centres (game space); the table (ba_hold_step)
    private readonly int[] _ba_why = new int[k_ba_max], _ba_tag = new int[k_ba_max];
    private int _ba_n;                        // candidates of the last collection
    private int _ba_bst;                      // battle_state last frame (0: no battle)
    private int _ba_serial;                   // this battle's serial (ba_next_serial)
    private bool _ba_have;                    // the table has a place in this battle (_ba_h[0..2], game space)
    private bool _ba_held;                    // no centre was computed this frame: the table stays
    private bool _ba_no_reset;                // battle_anchor was off during this battle: ticking it on does not reset the view
    private int _ba_party, _ba_mons;          // counted at the last collection
    private string _ba_note = "";             // why no centre was computed ("" = computed this frame)
    private int _ba_set; private bool _ba_set_ok;                          // the counted set of the last centre (the table's "changed")
    private int _ba_sig; private bool _ba_sig_set; private int _ba_logs;   // the logged set, the lines this battle

    private static string ba_why_name(int w) =>
        w == k_ba_ok ? "counted" : w == k_ba_benched ? "benched" : w == k_ba_no_actor ? "no live actor" : w == k_ba_not_in ? "not in the battle"
        : w == k_ba_hidden ? "hidden" : w == k_ba_dead ? "dead" : w == k_ba_nonfinite ? "not a finite position" : w == k_ba_outlier ? "outlier" : w == k_ba_untrusted ? "away from every enemy" : "?";
    private int ba_count(int w) { int k = 0; for (int i = 0; i < _ba_n; i++) if (_ba_why[i] == w) k++; return k; }
    private string ba_table() => _ba_h[9] >= 0f ? "gliding to a new centre" : _ba_h[10] >= 0f ? "waiting for the fight to stand still" : "still";

    /// <summary>In battle the diorama anchors to the table: the held centre of the fight (the party members on the field
    /// roster and the monsters in the battle; the file's head has the rule). From push_view_rules, every frame.</summary>
    private void push_battle_anchor() {
        if (_fn_anchor_override == null) return;
        _battle_anchor_n = 0; _ba_held = false;
        Btl* btl = Globals.Battle.btl; bool in_battle = read(btl, out Btl btl_v) && btl_v.battle_state != 0;
        int bst = in_battle ? btl_v.battle_state : 0;
        bool begins = ba_new_battle(_ba_bst, bst);
        if (begins || (bst == 0 && _ba_bst != 0)) {   // a battle begins (also right after another, with no frame outside one) or ends
            _ba_have = false; _ba_n = 0; _ba_party = 0; _ba_mons = 0; _ba_set_ok = false; _ba_sig_set = false; _ba_logs = 0; _ba_no_reset = false;
            if (begins) {
                _ba_serial = ba_next_serial(_ba_serial);
                if (_fn_eye_event != null) _fn_eye_event(4, 0f, 0f, 0f);   // no heading toward the enemies yet
            }
        }
        _ba_bst = bst;
        if (bst == 0) { _ba_note = ""; _fn_anchor_override(0, 0, 0, 0); return; }
        if (!_battle_anchor) { _ba_no_reset = true; _ba_note = "off (battle_anchor 0)"; _fn_anchor_override(0, 0, 0, 0); return; }
        if (!ba_live(bst)) _ba_note = "not the fight now (the battle is loading, or it has ended)";
        else if (!ba_collect(bst)) _ba_note = "the field roster cannot be used now";
        else {
            int m = ba_select(_ba_x, _ba_y, _ba_z, _ba_why, _ba_tag, _ba_n, _ba_tmp, _ba_c);
            _ba_party = ba_counted(_ba_why, _ba_tag, _ba_n, 0, k_ba_mon); _ba_mons = m - _ba_party;
            if (m > 0) {
                int set = _ba_n;   // who is counted
                for (int i = 0; i < _ba_n; i++) if (_ba_why[i] == k_ba_ok) set = unchecked(set * 31 + _ba_tag[i] + 1);
                bool changed = !_ba_set_ok || set != _ba_set;
                _ba_set = set; _ba_set_ok = true;
                if (!_ba_have) { ba_hold_start(_ba_h, _ba_c[0], _ba_c[1], _ba_c[2]); _ba_have = true; }
                else ba_hold_step(_ba_h, _ba_c[0], _ba_c[1], _ba_c[2], changed, ImGui.GetIO().DeltaTime);
                // the heading the view takes at this battle's reset: from the party toward the enemies (render space negates X; native: dio::face_yaw)
                if (_fn_eye_event != null && _ba_party > 0 && _ba_mons > 0) _fn_eye_event(4, -(_ba_c[6] - _ba_c[3]), 0f, _ba_c[8] - _ba_c[5]);
                _ba_note = "";
            }
            else _ba_note = ba_count(k_ba_untrusted) > 0 ? "the party is away from every enemy (not placed yet)" : "no combatant left to count";
            ba_log(bst, m);
        }
        _ba_held = _ba_note.Length > 0;
        if (!_ba_have) { _fn_anchor_override(0, 0, 0, 0); return; }
        _battle_anchor_n = _ba_party + _ba_mons;
        _fn_anchor_override(-_ba_h[0], -_ba_h[1], _ba_h[2], ba_valid_arg(_battle_view_reset && !_ba_no_reset, _ba_serial));   // render space negates X and Y
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
                _ba_tag[n] = k_ba_mon + i; _ba_why[n] = ba_mon_why(live(ma), mb[k_ej_chr_in], mb[0x16], mb[k_ej_chr_show], mb[k_ej_chr_death], hp); n++;
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
        if (m > 0) sb.Append(I($"the centre ({_ba_c[0]:F1} {_ba_c[1]:F1} {_ba_c[2]:F1}) (game space) of {_ba_party} party + {_ba_mons} monster(s), the table at ({_ba_h[0]:F1} {_ba_h[1]:F1} {_ba_h[2]:F1}) {ba_table()}"));
        else if (_ba_have) sb.Append(I($"nobody left to count: the table stays at ({_ba_h[0]:F1} {_ba_h[1]:F1} {_ba_h[2]:F1})"));
        else sb.Append("nobody to count yet: the diorama stays on the player");
        if (_ba_party > 0 && _ba_mons > 0) sb.Append(I($"; the party at ({_ba_c[3]:F1} {_ba_c[4]:F1} {_ba_c[5]:F1}), the enemies at ({_ba_c[6]:F1} {_ba_c[7]:F1} {_ba_c[8]:F1})"));
        sb.Append(I($"; dropped {_ba_n - _ba_party - _ba_mons} of {_ba_n} (outlier: a party member over {k_ba_radius:F0} units from the party's median, an enemy over {k_ba_far:F0}) |"));
        for (int i = 0; i < _ba_n; i++) {
            if (i > 0) sb.Append(';');
            if (_ba_tag[i] < k_ba_mon) sb.Append(I($" chr {_ba_tag[i]}")); else sb.Append(I($" monster {_ba_tag[i] - k_ba_mon}"));
            sb.Append(I($" ({_ba_x[i]:F1} {_ba_y[i]:F1} {_ba_z[i]:F1}) {ba_why_name(_ba_why[i])}"));
        }
        if (_ba_logs == k_ba_log_max) sb.Append(" (no more of these lines this battle)");
        d7_log(sb.ToString());
    }
    /// <summary>F8, Developer tab, "Battle, overdrives and rendering": the "battle diorama:" line.</summary>
    private string ba_status() {
        if (_ba_bst == 0) return "battle diorama: not in battle";
        if (!_ba_have) return "battle diorama: no centre yet" + (_ba_note.Length > 0 ? " (" + _ba_note + ")" : "");
        int drop = _ba_n - _ba_party - _ba_mons;
        string s = I($"battle diorama: combatants {_battle_anchor_n} counted ({_ba_party} party + {_ba_mons} monsters), {drop} dropped");
        if (drop > 0) s += I($" (benched {ba_count(k_ba_benched)}, hidden {ba_count(k_ba_hidden)}, dead {ba_count(k_ba_dead)}, outlier {ba_count(k_ba_outlier)}, other {ba_count(k_ba_no_actor) + ba_count(k_ba_not_in) + ba_count(k_ba_nonfinite) + ba_count(k_ba_untrusted)})");
        s += _ba_held ? "; the table stays: " + _ba_note : "; the table: " + ba_table();
        return s;
    }
}
