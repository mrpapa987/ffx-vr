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
}
