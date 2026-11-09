// SPDX-License-Identifier: MIT

namespace FFXVR;

// fix-eye (the 6 / 7 Nov 2026 logs, research/logs-1107-behaviour.md): the field follow pick's rules on top of RideLogic.FpScore.
// Finding 1 (s2 07:24:50-07:26:09, ki/kino0400): first person followed a second 'c001' (actor 36) whose model chain behind
// +0x830 was being rewritten (a stale model pointer) for 79 s, while the game's player was actor 0: a returning actor 0 only
// tied with it, and a tie kept the incumbent. Now a tie goes to the game's player actor ([0x1300788]; with control off, the
// last actor it named on this map while that slot holds the same actor), and a pick whose model chain stopped reading whole
// (the IB table build's "a read failed", body_hide.cs) ranks after every healthy listed model.
public sealed unsafe partial class FFXVRModule {

    private int _fp_gp_idx = -1, _fp_gp_id; private ulong _fp_gp_scene;   // the last slot [0x1300788] named, its chr_id, on which scene
    private FpMarks _fp_marks; private int _ib_bad_lines;   // finding 1: the model objects (+0x830) whose chain failed to read (FpMark)
    private uint* _fpm_q; private byte* _fpm_seg;              // review R2-S2: scratch for the marks' own re-reads (kept for the session)
    private double _fp_how_t = -1e9; private int _fp_how_skip;   // review B1: the "following" line, one a second

    /// <summary>The map words and the event id (body_hide_scene_gen's signature without the followed actor's model): changes with
    /// a map change or a new event. No allocation.</summary>
    private static ulong fp_scene_sig() {
        ulong sig = 1469598103934665603UL;
        try { sig = (sig ^ FhUtil.get_at<ulong>(0x1FCBC60)) * 1099511628211UL; sig = (sig ^ FhUtil.get_at<ulong>(0x1FCBC68)) * 1099511628211UL; } catch { }
        return (sig ^ (ulong)(Globals.event_id != null ? *Globals.event_id : -1)) * 1099511628211UL;
    }

    /// <summary>finding 1: the game's player actor's table slot ([0x1300788], gs_player: one read a frame), or with control off the
    /// last slot it named on this scene while that slot is live with the same chr_id; -1 none.</summary>
    private int fp_game_player(Actor* actors, ulong scene) {
        if (actors == null || !gs_player(out uint pl)) return -1;
        if (pl != 0) {
            long d = (long)pl - (long)(nint)actors;
            if (d < 0 || d % sizeof(Actor) != 0 || d / sizeof(Actor) >= 48) return -1;   // not in the table (never seen): no tie-break
            int i = (int)(d / sizeof(Actor));
            if (!read(actors + i, out Actor a) || !live(a)) return -1;
            _fp_gp_idx = i; _fp_gp_id = (int)a.chr_id; _fp_gp_scene = scene;
            return i;
        }
        if (_fp_gp_idx < 0 || _fp_gp_scene != scene || !read(actors + _fp_gp_idx, out Actor b) || !live(b) || (int)b.chr_id != _fp_gp_id) return -1;
        return _fp_gp_idx;
    }

    /// <summary>finding 1: ib_build saw the followed actor's (k == 0) model chain fail to read whole: every actor on that model object
    /// (+0x830) ranks after every healthy listed model until a whole read of the object (any slot) or a new scene. Review B1: the
    /// object, not the slot: in A1 actors 0 and 36 shared one object, and a per-slot mark moved with the pick, flipping it every frame;
    /// keyed on the object both rank the same and the tie goes to the game's player actor.</summary>
    /// Review round 2: a set of up to FpMark.k_n objects (R2-S1: two failing objects no longer take turns in one mark), each re-read
    /// by itself every 30 frames wherever its actors stand and cleared on a whole read, and expired after k_expire_s (R2-S2: a
    /// transient failure of the real Tidus no longer sticks while the eye sits on a model parked away). The rule is FpMark (pure).
    private void fp_chain_mark(int slot, nint h, bool failed, bool followed, char* name) {
        if (FpMark.Walked(ref _fp_marks, (long)h, failed, followed, _now) && _ib_bad_lines < 20) {
            _ib_bad_lines++;
            _logger.Info(I($"first person: actor {slot} '{actor_name(name)}''s model chain (+0x830 0x{(ulong)h:X}) did not read whole: every actor on that object ranks after every listed model whose chain reads (a stale or rebuilt model; finding 1; re-read every 30 frames) on map {map_name()}") + (_ib_bad_lines == 20 ? " (no more of these lines this session)" : ""));
        }
    }
    /// <summary>Review R2-S2: push_player, at the 30-frame rescan tick: each marked object walked by itself (one IbChain.Walk, wherever
    /// its actors are); a whole read clears it, a failed one keeps it fresh.</summary>
    private void fp_marks_reread() {
        if (_fn_safe_read == null) return;
        for (int i = 0; i < FpMark.k_n; i++) {
            long h = FpMark.At(ref _fp_marks, i, _now); if (h == 0) continue;
            if (_fpm_q == null) { _fpm_q = (uint*)Marshal.AllocHGlobal(k_ib_max * 16); _fpm_seg = (byte*)Marshal.AllocHGlobal(k_ib_segs * 0x6C); }
            int n = 0; IbChain.Walk((nint)h, 0, _fpm_q, ref n, k_ib_max, _fpm_seg, k_ib_segs, _fn_safe_read, out bool fail);
            FpMark.Walked(ref _fp_marks, h, fail, true, _now);
        }
    }
    // -- finding 4: no NPC heads, no hopping ---------------------------------------------------------------------------------
    // s1 (6 Nov): first person sat in an NPC ('n130', actor 0) for 23 s on ki/kino0800 and hopped c102 -> c003 -> c108 -> c002
    // in 34 s on ki/kino0300, each a new body (hidden) and a new head. In first person on foot the pick is now FpPick.Allowed: the
    // first pick of a scene as before but never an NPC, then no other body until a listed model (Tidus) is live; when the
    // followed actor goes mid-scene the eye stays where it was (its last root, head and eye height), with nothing hidden, until a
    // listed model is live (a re-pick: the eye-jump blink) or the scene ends (a map change, a new event, another view). The
    // game camera was the fallback for "nothing at all" and stays so for a scene that starts with nothing to follow: holding
    // the last place is the least jarring for a body that leaves mid-scene (the game camera would cut to wherever it is).
    // Review S2: but not for long: after FpPick.k_hold_s (3 s) or at a camera cut the held eye goes to the game camera (with a
    // blink: ffxvr_fp_event bit 8) and stays there until a listed model is live or the scene ends (s1 22:41:46: the cutscene after
    // battle 1 ran 61 s on Yuna with no listed model; the eye would have stood all that time at Tidus's last battle spot).
    private bool _fpk_on, _fp_strict_had; private ulong _fpk_scene, _fp_none_scene; private Vector3 _fpk_pos, _fpk_head; private int _fpk_hv;
    private int _fpk_lines; private string _fpk_who = "", _fpk_how = "";
    private bool _fpk_done, _fpk_blink; private double _fpk_t0; private int _fpk_cuts0;   // review S2: the hold handed to the game camera; a blink to send; its start, the cut count then
    /// <summary>A normal followed frame: in strict mode, where the eye is (game space), for a later hold.</summary>
    private void fpk_note(bool strict, Vector3 pos, Vector3 head, int hv) {
        _fp_strict_had = strict;
        if (!strict) return;
        _fpk_pos = pos; _fpk_head = head; _fpk_hv = hv & 1;   // no displaced-body bit while held
        _fpk_who = _fp_how;   // "actor N 'c001' (model c001)", set at the pick (no allocation here)
    }
    private void fpk_start(Actor* actors, int skip_i) {
        _fpk_on = true; _fpk_t0 = _now; _fpk_cuts0 = (int)_fpev_st[16];
        _fpk_how = "held: " + _fpk_who + " left; waiting for a listed model";   // review S6: built once
        if (_fpk_lines >= 40) return;
        _fpk_lines++;
        string skip = skip_i >= 0 && read(actors + skip_i, out Actor sa) ? I($"; not followed: actor {skip_i} '{actor_name(sa.chr_name)}' ({(FpPick.IsNpc((int)sa.chr_id) ? "an NPC" : "another body")})") : "";
        _logger.Info(I($"first person: the followed {_fpk_who} is gone and no listed model is live on map {map_name()}: the eye stays where it was ({_fpk_pos.X:F1}, {_fpk_pos.Y:F1}, {_fpk_pos.Z:F1}) until one is (finding 4: no hop to another body mid-scene){skip}") + (_fpk_lines == 40 ? " (no more of these lines this session)" : ""));
    }
    private void fpk_release(string why) {
        _fpk_on = false;
        if (_fpk_lines >= 40) return;
        _fpk_lines++;
        _logger.Info(I($"first person: the held eye is released after {_now - _fpk_t0:F1} s: {why}") + (_fpk_lines == 40 ? " (no more of these lines this session)" : ""));
    }
    /// <summary>A held frame: the last place again (player_set first: the head is an offset from it), no body hide (fp_set gets hide 0).</summary>
    private void fpk_frame() {
        _fn_player_set(-_fpk_pos.X, -_fpk_pos.Y, _fpk_pos.Z, 1);
        if (_fn_player_head_set != null) _fn_player_head_set(-_fpk_head.X, -_fpk_head.Y, _fpk_head.Z, _fpk_hv);
        body_hide_push(null, -1, -1, false, default, 0, default);
        _sa_used = false; _sa_status = "the eye held where the followed actor was (nothing listed to follow)";
        _fp_how = _fpk_how;
    }
    /// <summary>Strict, nothing allowed at the scene's first pick (only an NPC actor 0, or a hidden party model): the game's camera,
    /// as before for "nothing"; one line per scene.</summary>
    private void fp_none_log(Actor* actors, int skip_i, ulong scene) {
        if (skip_i < 0 || scene == _fp_none_scene || _fpk_lines >= 40) return;
        _fp_none_scene = scene; _fpk_lines++;
        string who = read(actors + skip_i, out Actor sa) ? I($"actor {skip_i} '{actor_name(sa.chr_name)}' ({(FpPick.IsNpc((int)sa.chr_id) ? "an NPC" : "a party model the game hides")})") : I($"actor {skip_i}");
        _logger.Info(I($"first person: nothing to follow on map {map_name()} ({who} is not followed: finding 4); the game's camera until a listed model is live") + (_fpk_lines == 40 ? " (no more of these lines this session)" : ""));
    }

    // -- the event hold (research/sphere-grab-eye.md): native ffxvr_eyejump.h ej::EvHold, ffxvr_fp_event (ABI 50) ----------------
    // In the Djose cloister (7 Nov) the game moved Tidus's root to the interaction mark when control went off (a sphere taken from a
    // wall socket) and the eye went with it into the wall. While control is off in first person on foot, native holds the eye's
    // horizontal position where it was, on a leash of fp_event_leash units (a single move over twice that is a staging: the hold
    // starts again there, with the blink); at control's return it glides back over 0.3 s under half the leash, else blinks and
    // snaps. A map change, a load or a re-pick (a new scene generation or followed slot: the release bit), a camera cut, or 45 s end it.
    // Cutscenes (control off for minutes) end it at their first camera cut, and the 45 s cap ends one without cuts (a scripted
    // walk under a following camera): an interaction rarely lasts longer (the djyt0000 grab: 27 s with control off). An "object or
    // actor within 20 units" gate was not taken: a cutscene's cast stands that near too, so it would not tell the two apart.
    // Review S1: the cut that starts a cutscene can reach native a frame before the control flag, or on the same frame: a cut on
    // the edge frame or the 2 before blocks the start (EV_BLOCKED). Review S3: no hold while the eye is on the drawn head.
    // review S1: off by default until the owner's capture confirms the cause (the game moving the root); F8 ticks it for a test
    private bool _fp_event_hold = false; private float _fp_event_leash = 12f;   // ini fp_event_hold (default 0), fp_event_leash (units, 2..40)
    private delegate* unmanaged[Cdecl]<int, float, float*, int, int> _fn_fp_event;   // ABI 50
    private long _fpev_key = -1; private readonly float[] _fpev_st = new float[17];   // the last eligible frame's (slot, scene generation); native's state for F8
    /// <summary>push_player, every frame after the eye is sent: eligible = the field, first person, on foot (not riding), not
    /// blitzball, a followed actor; with control off ([0x1300788] 0) native holds the eye. release = the followed slot or the
    /// scene generation (map, event, the model instance a load re-creates) changed since the last eligible frame.</summary>
    private void fpev_push(bool eligible, Actor* actors, int pick) {
        if (_fn_fp_event == null) return;
        bool off = eligible && gs_player(out uint pl) && pl == 0;
        long key = eligible ? ((long)pick << 32) | body_hide_scene_gen(actors, pick) : -1;
        bool rel = eligible && _fpev_key >= 0 && key != _fpev_key;
        if (eligible) _fpev_key = key;
        // the automatic capture: the motion log for 20-40 s from the frame control goes off (a diagnostic profile only)
        bool can = false;
        if (off && !_fpcap.off_prev) {
            can = diag_any() && _fn_motion_log != null && !(read(FhUtil.ptr_at<uint>(GameGates.k_menu), out uint menu) && menu != 0);   // not for a field menu
            _fpcap_share_slot = -1; _fpcap_share = ""; _fpcap_share_dmin = 0f;
        }
        int ml = FpCap.Step(ref _fpcap, off, can, _now);
        if (ml > 0) {
            _fn_motion_log(ml);
            if (ml == FpCap.k_frames) { string m = I($"eye capture: player control off in first person on map {map_name()}: the motion log runs {FpCap.k_frames / 60} s ({FpCap.k_frames_max / 60} s while control stays off), with the eye columns; run {_fpcap.runs} of {FpCap.k_runs} this session"); _logger.Info(m); d7_log(m); }
        }
        if (_fpcap.episode && off && _eye_share_frame == _frames && _eye_share_slot >= 0) {   // the actor on his root during the episode (the nearest seen)
            if (_eye_share_slot != _fpcap_share_slot) { _fpcap_share_slot = _eye_share_slot; _fpcap_share = I($"actor {_eye_share_slot} '{actor_name((char*)_eye_share_name)}' (chr_id 0x{_eye_share_id:X}, {chr_kind(_eye_share_id)})"); _fpcap_share_dmin = _eye_share_d; }
            else if (_eye_share_d < _fpcap_share_dmin) _fpcap_share_dmin = _eye_share_d;
        }
        fixed (float* o = _fpev_st) {
            _fn_fp_event((off ? 1 : 0) | (_fp_event_hold ? 2 : 0) | (rel ? 4 : 0) | (_fpk_blink ? 8 : 0), _fp_event_leash, o, 17); _fpk_blink = false;   // 8: review S2's blink
            if (o[4] != _fpcap_seq) { _fpcap_seq = o[4]; if (_fpcap.episode && !off) fpcap_summary(o); }
        }
    }
    private FpCapState _fpcap; private float _fpcap_seq; private int _fpcap_share_slot = -1; private string _fpcap_share = ""; private float _fpcap_share_dmin;
    private int _eye_share_slot = -1, _eye_share_id; private nint _eye_share_name; private float _eye_share_d; private ulong _eye_share_frame = ulong.MaxValue;   // eye_disp_step's shared-root actor this frame
    private static string chr_kind(int chr_id) { uint t = (uint)chr_id >> 12; return t == 0 ? "a party / event character model" : t == 2 ? "an NPC" : t == 4 ? "a 'w' model" : t == 5 ? "an object (chr/obj)" : I($"type {t}"); }
    /// <summary>The capture's one summary line when control is back (native's episode, ej::Episode): the diag log, with the motion lines.</summary>
    private void fpcap_summary(float* o) {
        int dir = (int)o[7];
        string d = dir == 1 ? "forward" : dir == 2 ? "sideways" : dir == 3 ? "back" : "nowhere (under half a unit)";
        string m = I($"eye capture: control back (or first person left) on map {map_name()} after {o[5]:F0} frames (about {o[5] / 60f:F1} s): the root moved {o[6]:F1} units, {d} of the view at the start ({o[8]:F1} along, {o[9]:F1} across); the largest single-frame step {o[10]:F1} at +{o[11]:F0} frames; the eye at most {o[12]:F1} units from where it was when control went off; black frames {o[13]:F0}, camera cuts {o[14]:F0}, hold restarts {o[15]:F0} (fp_event_hold {(_fp_event_hold ? 1 : 0)}, leash {_fp_event_leash:F0}); on his root: {(_fpcap_share_slot >= 0 ? I($"{_fpcap_share}, nearest {_fpcap_share_dmin:F2}") : "nobody")}; the nearest scene depth: the uidepth: lines");
        _logger.Info(m); d7_log(m);
    }
    /// <summary>F8 > View > First person (the hold's state: Developer > Logging & status).</summary>
    private void fpev_ui() {
        ui_check("fp_event_hold", "Steady view during scripted moves##fpev", ref _fp_event_hold, UiTag.None, "Hold the eye while the game moves you (control off) in first person."); ImGui.SameLine();
        ui_slider("fp_event_leash", "slack (units)##fpev", ref _fp_event_leash, 2f, 40f, "%.0f", UiTag.None, "How far the game may move you before the eye trails.", 140f);
    }

    /// <summary>finding 1: is h (an actor's +0x830) the marked model object?</summary>
    private bool fp_chain_bad(nint h) => FpMark.Has(ref _fp_marks, (long)h, _now);
}

/// <summary>fix-eye review round 2: the marked model objects (FpMark); 0 = an empty entry.</summary>
internal struct FpMarks { public long h0, h1, h2, h3; public double t0, t1, t2, t3; public ulong scene; }
/// <summary>fix-eye finding 1 / review B1, R2-S1, R2-S2: the model objects (+0x830) whose chain did not read whole. Set by a failed
/// walk of the followed actor's object (or of a marked one: its re-read keeps it fresh), cleared by any whole walk of it; up to k_n at
/// once (the oldest gives way), each expiring after k_expire_s; a new scene clears them all. Pure: the scratch test transpiles it.</summary>
internal static class FpMark {
    public const int k_n = 4;
    public const double k_expire_s = 5.0;
    public static bool Live(long h, double t, double now) => h != 0 && now - t < k_expire_s;
    /// <summary>Entry i's object if it is set and not expired, else 0.</summary>
    public static long At(ref FpMarks m, int i, double now) {
        if (i == 0) return Live(m.h0, m.t0, now) ? m.h0 : 0;
        if (i == 1) return Live(m.h1, m.t1, now) ? m.h1 : 0;
        if (i == 2) return Live(m.h2, m.t2, now) ? m.h2 : 0;
        return Live(m.h3, m.t3, now) ? m.h3 : 0;
    }
    public static bool Has(ref FpMarks m, long h, double now) {
        if (h == 0) return false;
        return At(ref m, 0, now) == h || At(ref m, 1, now) == h || At(ref m, 2, now) == h || At(ref m, 3, now) == h;
    }
    /// <summary>A walk of object h: failed and (the followed actor's or a mark's own re-read): set or refresh; whole: clear. Returns
    /// true when h was newly set (the caller's log line).</summary>
    public static bool Walked(ref FpMarks m, long h, bool failed, bool followed, double now) {
        if (h == 0) return false;
        if (!failed) {
            if (m.h0 == h) m.h0 = 0;
            if (m.h1 == h) m.h1 = 0;
            if (m.h2 == h) m.h2 = 0;
            if (m.h3 == h) m.h3 = 0;
            return false;
        }
        if (!followed) return false;
        bool was = Has(ref m, h, now);
        if (m.h0 == h) { m.t0 = now; return !was; }
        if (m.h1 == h) { m.t1 = now; return !was; }
        if (m.h2 == h) { m.t2 = now; return !was; }
        if (m.h3 == h) { m.t3 = now; return !was; }
        // an empty or expired entry, else the oldest
        int k = 0; double oldest = m.t0;
        if (!Live(m.h0, m.t0, now)) k = 0;
        else if (!Live(m.h1, m.t1, now)) k = 1;
        else if (!Live(m.h2, m.t2, now)) k = 2;
        else if (!Live(m.h3, m.t3, now)) k = 3;
        else { if (m.t1 < oldest) { k = 1; oldest = m.t1; } if (m.t2 < oldest) { k = 2; oldest = m.t2; } if (m.t3 < oldest) { k = 3; } }
        if (k == 0) { m.h0 = h; m.t0 = now; } else if (k == 1) { m.h1 = h; m.t1 = now; } else if (k == 2) { m.h2 = h; m.t2 = now; } else { m.h3 = h; m.t3 = now; }
        return true;
    }
    /// <summary>A new scene (map words, event id) clears every mark.</summary>
    public static void Scene(ref FpMarks m, ulong scene) {
        if (scene == m.scene) return;
        m.h0 = 0; m.h1 = 0; m.h2 = 0; m.h3 = 0; m.scene = scene;
    }
}

/// <summary>fix-eye: the automatic capture's state (FpCap.Step).</summary>
internal struct FpCapState { public int runs, left; public bool ext, off_prev, episode; public double last; }
/// <summary>fix-eye: the automatic capture (research/sphere-grab-eye.md section 4): when control goes off in first person on foot (the
/// caller's can: a diagnostic profile, no field menu), the motion log runs k_frames (20 s), extended once to k_frames_max (40 s)
/// in all while control stays off; k_runs a session, k_gap seconds apart. Step returns the frames to (re)start the log with this
/// frame (0: nothing to do). Pure: the scratch test transpiles it.</summary>
internal static class FpCap {
    public const int k_runs = 6, k_frames = 1200, k_frames_max = 2400, k_ext_at = 60;
    public const double k_gap = 30.0;
    public static int Step(ref FpCapState s, bool off, bool can, double now) {
        int r = 0;
        if (off && !s.off_prev) {
            s.episode = false;
            if (can && s.runs < k_runs && (s.runs == 0 || now - s.last >= k_gap)) { s.runs++; s.last = now; s.left = k_frames; s.ext = false; s.episode = true; r = k_frames; }
        }
        if (s.left > 0) {
            s.left--;
            if (off && s.episode && !s.ext && s.left == k_ext_at) { s.ext = true; s.left += k_frames_max - k_frames; r = s.left; }
        }
        s.off_prev = off;
        return r;
    }
}

/// <summary>fix-eye: the follow pick's pure rules (the scratch test transpiles this class; keep it to plain arithmetic).</summary>
internal static class FpPick {
    public const int k_listed = 1000;    // FpScore below this: a listed model (visible: rank; hidden by the game: 500 + rank)
    public const int k_party = 100000;   // ... below this: a visible party model (1000 + chr_id); 100000 actor 0; 200000+ a hidden party model
    public const int k_bad = 900;        // finding 1: a listed model whose chain does not read whole: 900 + rank
    /// <summary>finding 1: a candidate's score with its model chain marked bad: a listed model after every healthy listed one (visible or
    /// hidden; 900 + rank) and before the party fallback; a visible party model among the hidden ones. Negative stays negative.</summary>
    public static int Bad(int s) {
        if (s < 0) return s;
        if (s < k_listed) { int r = s % 500; return k_bad + (r < 99 ? r : 99); }
        if (s < k_party) return s + 199000;
        return s;
    }
    /// <summary>finding 1: does score s at slot i beat the best so far (best at best_i)? Lower wins; a tie goes to the game's player
    /// actor gp (-1: none known), never away from it.</summary>
    public static bool Better(int s, int i, int best, int best_i, int gp) {
        if (s < 0) return false;
        if (s < best) return true;
        return s == best && gp >= 0 && i == gp && best_i != gp;
    }
    /// <summary>Is the current pick kept this frame without a rescan? cur = its score (-1: dead, or its slot holds another actor);
    /// scan = the 30-frame rescan tick; gp_other = the game's player actor is known and is another slot. As before (FP-1, FP-2,
    /// FP-R3: a listed or party pick rescans at the tick, a hidden listed model, actor 0 and a hidden party model every frame), but
    /// a pick at score 0 also rescans at the tick while the game's player is another slot (finding 1: a returning player only ties).</summary>
    public static bool Keep(int cur, bool scan, bool gp_other) {
        if (cur == 0) return !(scan && gp_other);
        return cur > 0 && cur < k_party && (cur < 500 || cur >= k_listed) && !scan;
    }
    /// <summary>finding 4: an NPC model ('n', chr_id type 2: chr_id >> 12).</summary>
    public static bool IsNpc(int chr_id) => ((chr_id >> 12) & 0xF) == 2;
    /// <summary>review S2: the held eye (the followed body gone mid-scene) goes back to the game camera after k_hold_s seconds or at a
    /// camera cut: holding beats a hop into another head, but a scene that moves on elsewhere must still be seen.</summary>
    public const double k_hold_s = 3.0;
    public static bool HoldEnds(double held_s, bool cut) => cut || held_s >= k_hold_s;
    /// <summary>finding 4: may a candidate at score s (chr_id) be followed? strict = first person on foot in the field (fp_mode 1,
    /// not battle, not blitzball); first = nothing is followed and nothing held on this scene (the first pick after a map change, a
    /// load, a battle or another view). Outside strict: anything FpScore accepts, as before. In strict: a listed model always (Tidus,
    /// visible or hidden by the game, or with a bad chain); the first pick of a scene may also be the party fallback or actor 0,
    /// but never an NPC actor 0 ('n130' on ki/kino0800 for 23 s) nor a party model the game hides (parked at (0, 0, 0)); after
    /// that, no hop to another body mid-scene (c102 -> c003 -> c108 -> c002 on ki/kino0300): the caller keeps the current pick
    /// while it is live and otherwise holds the eye where it was. The current pick is tested with first = true.</summary>
    public static bool Allowed(int s, bool strict, bool first, int chr_id) {
        if (s < 0) return false;
        if (!strict || s < k_listed) return true;
        if (!first || s >= 200000) return false;
        return !(s >= k_party && IsNpc(chr_id));
    }
}
