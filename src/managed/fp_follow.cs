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
    private int _ib_bad_slot = -1, _ib_bad_id; private nint _ib_bad_h; private int _ib_bad_lines;   // finding 1: the followed actor's chain failed to read

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

    /// <summary>finding 1: ib_build saw the followed actor's (k == 0) model chain fail to read whole: that slot, with this +0x830 and
    /// chr_id, ranks after every healthy listed model until its model changes, the slot is reused, or the chain reads whole again.</summary>
    private void fp_chain_mark(int slot, nint h, int chr_id, bool failed, char* name) {
        if (failed) {
            if (slot == _ib_bad_slot && h == _ib_bad_h && chr_id == _ib_bad_id) return;
            _ib_bad_slot = slot; _ib_bad_h = h; _ib_bad_id = chr_id;
            if (_ib_bad_lines < 20) { _ib_bad_lines++; _logger.Info(I($"first person: actor {slot} '{actor_name(name)}''s model chain (+0x830 0x{(ulong)h:X}) did not read whole: it ranks after every listed model whose chain reads (a stale or rebuilt model; finding 1) on map {map_name()}") + (_ib_bad_lines == 20 ? " (no more of these lines this session)" : "")); }
        }
        else if (slot == _ib_bad_slot) _ib_bad_slot = -1;
    }
    // -- finding 4: no NPC heads, no hopping ---------------------------------------------------------------------------------
    // s1 (6 Nov): first person sat in an NPC ('n130', actor 0) for 23 s on ki/kino0800 and hopped c102 -> c003 -> c108 -> c002
    // in 34 s on ki/kino0300, each a new body (hidden) and a new head. In first person on foot the pick is now FpPick.Allowed: the
    // first pick of a scene as before but never an NPC, then no other body until a listed model (Tidus) is live; when the
    // followed actor goes mid-scene the eye stays where it was (its last root, head and eye height), with nothing hidden, until a
    // listed model is live (a re-pick: the eye-jump blink) or the scene ends (a map change, a new event, another view). The
    // game camera was the fallback for "nothing at all" and stays so for a scene that starts with nothing to follow: holding
    // the last place is the least jarring for a body that leaves mid-scene (the game camera would cut to wherever it is).
    private bool _fpk_on, _fp_strict_had; private ulong _fpk_scene, _fp_none_scene; private Vector3 _fpk_pos, _fpk_head; private int _fpk_hv;
    private int _fpk_lines; private ulong _fpk_from; private string _fpk_who = "";
    /// <summary>A normal followed frame: in strict mode, where the eye is (game space), for a later hold.</summary>
    private void fpk_note(bool strict, Vector3 pos, Vector3 head, int hv) {
        _fp_strict_had = strict;
        if (!strict) return;
        _fpk_pos = pos; _fpk_head = head; _fpk_hv = hv & 1;   // no displaced-body bit while held
        _fpk_who = _fp_how;   // "actor N 'c001' (model c001)", set at the pick (no allocation here)
    }
    private void fpk_start(Actor* actors, int skip_i) {
        _fpk_on = true; _fpk_from = _frames;
        if (_fpk_lines >= 40) return;
        _fpk_lines++;
        string skip = skip_i >= 0 && read(actors + skip_i, out Actor sa) ? I($"; not followed: actor {skip_i} '{actor_name(sa.chr_name)}' ({(FpPick.IsNpc((int)sa.chr_id) ? "an NPC" : "another body")})") : "";
        _logger.Info(I($"first person: the followed {_fpk_who} is gone and no listed model is live on map {map_name()}: the eye stays where it was ({_fpk_pos.X:F1}, {_fpk_pos.Y:F1}, {_fpk_pos.Z:F1}) until one is (finding 4: no hop to another body mid-scene){skip}") + (_fpk_lines == 40 ? " (no more of these lines this session)" : ""));
    }
    private void fpk_release(string why) {
        _fpk_on = false;
        if (_fpk_lines >= 40) return;
        _fpk_lines++;
        _logger.Info(I($"first person: the held eye is released after {(_frames - _fpk_from) / 60.0:F1} s: {why}") + (_fpk_lines == 40 ? " (no more of these lines this session)" : ""));
    }
    /// <summary>A held frame: the last place again (player_set first: the head is an offset from it), no body hide (fp_set gets hide 0).</summary>
    private void fpk_frame() {
        _fn_player_set(-_fpk_pos.X, -_fpk_pos.Y, _fpk_pos.Z, 1);
        if (_fn_player_head_set != null) _fn_player_head_set(-_fpk_head.X, -_fpk_head.Y, _fpk_head.Z, _fpk_hv);
        body_hide_push(null, -1, -1, false, default, 0, default);
        _sa_used = false; _sa_status = "the eye held where the followed actor was (nothing listed to follow)";
        _fp_how = "held: " + _fpk_who + " left; waiting for a listed model";
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
    private bool _fp_event_hold = true; private float _fp_event_leash = 12f;   // ini fp_event_hold (default 1), fp_event_leash (units, 2..40)
    private delegate* unmanaged[Cdecl]<int, float, float*, int, int> _fn_fp_event;   // ABI 50
    private long _fpev_key = -1; private readonly float[] _fpev_st = new float[4];   // the last eligible frame's (slot, scene generation); native's state for F8
    /// <summary>push_player, every frame after the eye is sent: eligible = the field, first person, on foot (not riding), not
    /// blitzball, a followed actor; with control off ([0x1300788] 0) native holds the eye. release = the followed slot or the
    /// scene generation (map, event, the model instance a load re-creates) changed since the last eligible frame.</summary>
    private void fpev_push(bool eligible, Actor* actors, int pick) {
        if (_fn_fp_event == null) return;
        bool off = eligible && gs_player(out uint pl) && pl == 0;
        long key = eligible ? ((long)pick << 32) | body_hide_scene_gen(actors, pick) : -1;
        bool rel = eligible && _fpev_key >= 0 && key != _fpev_key;
        if (eligible) _fpev_key = key;
        fixed (float* o = _fpev_st) _fn_fp_event((off ? 1 : 0) | (_fp_event_hold ? 2 : 0) | (rel ? 4 : 0), _fp_event_leash, o, 4);
    }
    /// <summary>F8, under the eye jumps.</summary>
    private void fpev_ui() {
        ImGui.Checkbox("hold the eye while the game moves you (control off)##fpev", ref _fp_event_hold); ImGui.SameLine();
        ImGui.SetNextItemWidth(140); ImGui.SliderFloat("leash (units)##fpev", ref _fp_event_leash, 2f, 40f, "%.0f");
        if (_fn_fp_event != null) { ImGui.SameLine(); ImGui.TextDisabled(I($"{(_fpev_st[0] == 1f ? $"holding {_fpev_st[1]:F1} s, the eye {_fpev_st[2]:F1} units off" : _fpev_st[0] == 2f ? "gliding back" : "not holding")}; holds {_fpev_st[3]:F0}")); }
    }

    /// <summary>finding 1: is slot i the marked one, with the same model and chr_id (tbl: this frame's snapshot or the live table)?</summary>
    private bool fp_chain_bad(int i, Actor* tbl, int chr_id) {
        if (i != _ib_bad_slot || tbl == null) return false;
        if (chr_id != _ib_bad_id || !read((byte*)(tbl + i) + 0x830, out nint h) || h != _ib_bad_h) { _ib_bad_slot = -1; return false; }
        return true;
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
