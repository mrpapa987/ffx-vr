// SPDX-License-Identifier: MIT

namespace FFXVR;

// Battle tag (5 Nov 2026; native ffxvr_tag.h, ABI 46). A swap-in ("Tidus joined the field", "N left the field") used to keep the
// eyes on the outgoing character until the incoming one had run in and stood still: about 2 s in the owner's 5 Nov log, watching
// him run off. Now push_player watches the two while the swap is pending and hands the eyes on where they cross (tag::step:
// closer than bt_tag_dist, or past their closest; the first frames and any frame before both move are ignored):
//   bt_tag 0 "wait until he arrives": as before (the crossing is still measured, for the log);
//          1 "tag and wait on the spot" (default): at the tag the follow goes to the incoming character; in first person the
//            eye stays on the outgoing one's standing spot (his position when the swap began: the slot the incoming one runs
//            to; a swap during a hold keeps that spot), at the incoming one's eye height (his head by the shared rule,
//            FphWalk.HeadPick), until he arrives and stands still (the arrival rule below) or the 300 frames since the swap
//            began are up; then it follows him. While the eye is held native centres the body hide on the spot: he shows
//            running in and hides once he is near it (tag::hide_centre);
//          2 "tag and ride in with him": at the tag the eyes go to the incoming character.
//   A tag blinks (the eye-jump blink: eye_jump_blink frames) and faces the enemies (ej_follow: battle_face, from the spot).
//   No tag (the wait as before) when the followed character stays on the field (Tidus joining for someone else), when either
//   character cannot be read or the outgoing one is already gone, or when the two have not crossed by the arrival / 300 frames.
//   ini bt_tag (0 / 1 / 2; 1), bt_tag_dist (1-20 units; 6). One "battle tag:" line per swap (100 a session).
public sealed unsafe partial class FFXVRModule {
    private int   _bt_tag = 1;          // ini bt_tag
    private float _bt_tag_dist = 6f;    // ini bt_tag_dist (horizontal units; party models are about 15-22 tall)
    private delegate* unmanaged[Cdecl]<int, float, float, float, float, float, float*, int, int> _fn_bt_tag_step;   // ABI 46
    private delegate* unmanaged[Cdecl]<int, float, float, float, int> _fn_bt_tag_hold;                              // ABI 46

    private bool    _btg_open;          // a swap whose battle tag: line is not written yet
    private int     _btg_state;         // 0 not watching, 1 watching the pending swap, 2 the eye held on the spot (the follow is his)
    private bool    _btg_begun;         // native's watch has begun (both readable)
    private int     _btg_mode;          // bt_tag when the swap began
    private int     _btg_out_ply = -1, _btg_out_id = -1;   // the outgoing character: entity index, chr id
    private Vector3 _btg_spot;          // game space: the outgoing character's position when the swap began
    private int     _btg_frames;        // frames since the swap began
    private int     _btg_tag_at = -1, _btg_verdict;      // the frame of the tag (bt_tag 0: of the crossing), tag::Verdict
    private float   _btg_min = -1f, _btg_d;              // the closest since both moved (-1 none yet), the distance at the tag
    private string  _btg_why = "", _btg_note = "";       // the swap; why there was no tag
    private bool    _btg_fed;           // the hold was sent this frame
    private int     _btg_hold_n, _btg_still;             // frames held; the still rule's count while held
    private Vector3 _btg_prev;          // his position last frame (the still rule while held)
    private int     _btg_lines;         // battle tag: lines this session
    private static readonly string[] k_btg_label = { "wait until he arrives", "tag and wait on the spot", "tag and ride in with him" };
    private bool    _btg_carry;         // a hold ended this frame: a swap beginning now takes its spot (the slot), not a runner's position
    private bool    _btg_carried;       // this swap's spot is the one of the swap before (review N1)

    // The arrival ("still") rule, one set for the pending swap (push_player) and the hold (btg_hold_tick) (review N3): he has
    // arrived when more than k_btl_arrive_min frames have passed since the swap began and he has moved under k_btl_still_d units
    // (horizontal, per sample) for more than k_btl_still_n frames in a row; k_btl_swap_max frames end a swap whatever happens.
    // 15 render frames is about a quarter second at 60 fps; at 60 fps render / 30 Hz logic every other frame shows no movement,
    // so the count only grows once he stops on logic steps too: 0.3 per sample works at 30 and at 60 fps. Frames, not time, on
    // purpose: FFX logic is frame-locked (R33, won't fix).
    private const int k_btl_arrive_min = 30, k_btl_still_n = 15, k_btl_swap_max = 300; private const float k_btl_still_d = 0.3f;
    private static int btl_still_next(int still, float dx, float dz) => dx * dx + dz * dz < k_btl_still_d * k_btl_still_d ? still + 1 : 0;
    private static bool btl_arrived(int frames, int still) => frames > k_btl_arrive_min && still > k_btl_still_n;

    private bool btg_parse(string k, string v) {
        switch (k) {
            case "bt_tag":      pe(k, v, ref _bt_tag, 0, 1, 2); return true;
            case "bt_tag_dist": pf(k, v, ref _bt_tag_dist, 1f, 20f); return true;
            default: return false;
        }
    }
    private string btg_ini_text() => $"bt_tag={_bt_tag.ToString(CI)}\nbt_tag_dist={_bt_tag_dist.ToString("F1", CI)}\n";
    private const string k_btg_tip = "How a swap hands the eyes on: wait until he arrives, tag and wait on the spot, or tag and ride in with him.";
    /// <summary>F8 > Battle > Battle view, under the battle follow.</summary>
    private void btg_ui() {
        ImGui.Text("When characters swap:"); ImGui.SameLine();
        ui_radio("bt_tag", "wait for him##btag", ref _bt_tag, 0, UiTag.None, k_btg_tip); ImGui.SameLine();
        ui_radio("bt_tag", "tag on the spot##btag", ref _bt_tag, 1, UiTag.None, k_btg_tip); ImGui.SameLine();
        ui_radio("bt_tag", "ride in with him##btag", ref _bt_tag, 2, UiTag.None, k_btg_tip);
    }

    /// <summary>A party member's position (game space), when the entity is that chr id and its actor is live and finite.</summary>
    private bool btg_pos(Chr* plys, int ply, int id, out Vector3 p) {
        p = default;
        if (plys == null || ply < 0 || ply >= 18 || !read(plys + ply, out Chr c) || c.chr_id != id || c.actor == null || !read(c.actor, out Actor a) || !live(a)) return false;
        p = new Vector3(a.chr_pos_vec.X, a.chr_pos_vec.Y, a.chr_pos_vec.Z);
        return finite(p);
    }

    /// <summary>push_player: a swap-in begins (a new pending switch to `want`, entity wi), before _btl_chr / _btl_follow_id
    /// change: they are the outgoing character. out_on_field: he is still on the roster (not his swap).</summary>
    private void btg_begin(Chr* plys, string why, int want, int wi, bool out_on_field) {
        // review N1: a swap that begins during a hold (the first incoming one swapped out again mid-run; the hold was ended a
        // moment ago by the follow change) keeps the held spot: the new incoming one runs to the same slot, not to where the
        // first one is now
        bool carry = _btg_state == 2 || _btg_carry;
        btg_end("another swap began");
        btg_log("another swap began");
        _btg_open = true; _btg_state = 0; _btg_begun = false; _btg_mode = _bt_tag; _btg_why = why ?? ""; _btg_note = ""; _btg_carried = false;
        _btg_frames = 0; _btg_tag_at = -1; _btg_verdict = 0; _btg_min = -1f; _btg_d = 0f;
        _btg_out_ply = _btl_chr; _btg_out_id = _btl_follow_id;
        if (_fn_bt_tag_step == null || _fn_bt_tag_hold == null) { _btg_note = "no native tag"; return; }
        if (out_on_field) { _btg_note = "the followed character stays on the field"; return; }
        if (!btg_pos(plys, _btg_out_ply, _btg_out_id, out Vector3 o)) { _btg_note = "the outgoing character is already off the field (not readable)"; return; }
        if (carry) _btg_carried = true; else _btg_spot = o;
        _btg_state = 1;
    }
    /// <summary>push_player: the pending switch changed to one that is not a swap-in (a summon, a manual pick) while a swap was
    /// watched: its line is written now, with that reason (review N2).</summary>
    private void btg_interrupt(string why) {
        if (!_btg_open) return;
        _btg_state = 0;
        btg_log(I($"interrupted before the switch: {why}"));
    }
    private void btg_drop(string why) { _btg_state = 0; if (_btg_note.Length == 0) _btg_note = why; }

    /// <summary>push_player, each frame of the pending swap-in (frames = frames since it began), before the switch. True: tag now
    /// (bt_tag 1 / 2). Watches from the first frame both are readable; a character lost after that gives up the tag.</summary>
    private bool btg_watch(Chr* plys, int want, int wi, int frames) {
        _btg_frames = frames;
        if (_btg_state != 1) return false;
        if (!btg_pos(plys, _btg_out_ply, _btg_out_id, out Vector3 o)) { btg_drop($"the outgoing character went off the field after {frames} frames"); return false; }
        bool ok_i = btg_pos(plys, wi, want, out Vector3 i);
        if (!_btg_begun) {
            if (!ok_i) return false;   // not set up yet: the watch starts at his first readable frame
            _btg_begun = _fn_bt_tag_step(0, o.X, o.Z, i.X, i.Z, _bt_tag_dist, null, 0) == 0;
            if (!_btg_begun) btg_drop("a position is not a number");
            return false;
        }
        if (!ok_i) { btg_drop($"the incoming character could not be read after {frames} frames"); return false; }
        float* r = stackalloc float[4];
        int v = _fn_bt_tag_step(1, o.X, o.Z, i.X, i.Z, _bt_tag_dist, r, 4);
        _btg_d = r[0]; _btg_min = r[1];
        if (v != 1 && v != 2) { if (v != 0) btg_drop("a position is not a number"); return false; }
        _btg_tag_at = frames; _btg_verdict = v; _btg_state = 0;   // one crossing a swap (bt_tag 0: recorded for the line only)
        return _btg_mode != 0;
    }

    /// <summary>push_player, right after the follow switched to the incoming character (_btl_chr / _btl_follow_id are his) on a
    /// swap-in: at the tag (bt_tag 1 in first person: the eye held on the spot; otherwise a blink to him), or at his arrival /
    /// the 300 frames without a tag (end says which).</summary>
    private void btg_switched(Chr* plys, bool tagged, string end) {
        if (!tagged) { btg_log(end); return; }
        if (_btg_mode == 1 && _fp_mode == 1 && btg_pos(plys, _btl_chr, _btl_follow_id, out Vector3 p)) {
            _btg_state = 2; _btg_hold_n = 0; _btg_still = 0; _btg_prev = p;
            btg_feed();   // the first frame of the hold: native blinks
            return;
        }
        _fn_bt_tag_hold(2, 0f, 0f, 0f);   // a tag without a hold: the blink
        btg_log(_btg_mode == 2 ? "the eyes rode in with him from the tag" : $"the eyes went to him at the tag (no hold: view {_fp_mode})");
    }
    private void btg_feed() { _fn_bt_tag_hold(1, -_btg_spot.X, -_btg_spot.Y, _btg_spot.Z); _btg_fed = true; _btg_hold_n++; }   // render space negates X and Y

    /// <summary>push_player, every battle-follow frame after the switch logic: keeps the hold until he arrives and stands still (the
    /// still rule: past 30 frames since the swap began, 15 frames without moving 0.3 units) or 300 frames since the swap began.</summary>
    private void btg_hold_tick(Chr* plys) {
        if (_btg_state != 2 || _btg_fed) return;
        _btg_frames++;
        if (_fp_mode != 1) { btg_end($"the view changed (view {_fp_mode})"); return; }
        if (!btg_pos(plys, _btl_chr, _btl_follow_id, out Vector3 p)) { btg_end("he could not be read"); return; }
        _btg_still = btl_still_next(_btg_still, p.X - _btg_prev.X, p.Z - _btg_prev.Z);
        _btg_prev = p;
        if (btl_arrived(_btg_frames, _btg_still)) { btg_end("he arrived and stood still"); return; }
        if (_btg_frames > k_btl_swap_max) { btg_end(I($"the {k_btl_swap_max}-frame limit")); return; }
        btg_feed();
    }
    private int _btg_miss;   // consecutive frames the follow logic did not run during a hold
    /// <summary>push_player, after the battle follow: a hold the follow logic did not send this frame (no roster, a stale roster
    /// read: one frame of [0,0,0]) is kept for up to 10 frames in a battle with the follow on, so a glitch does not end it (and blink
    /// again when it comes back); then, or out of battle / follow off / another view, it ends.</summary>
    private void btg_guard(bool in_battle) {
        if (_btg_state != 2) { _btg_miss = 0; return; }
        if (_btg_fed) { _btg_miss = 0; return; }
        if (in_battle && _battle_follow && _fp_mode == 1 && ++_btg_miss <= 10) { _btg_frames++; btg_feed(); return; }
        btg_end(!in_battle ? "the battle ended" : !_battle_follow ? "the battle follow was turned off" : _fp_mode != 1 ? $"the view changed (view {_fp_mode})" : "the battle follow had no roster for 10 frames");
    }
    /// <summary>Ends a hold (no-op without one): native releases the eye, and the swap's line is written.</summary>
    private void btg_end(string why) {
        if (_btg_state != 2) return;
        _btg_state = 0; _btg_carry = true;   // (cleared at the next push_player)
        if (_fn_bt_tag_hold != null) _fn_bt_tag_hold(0, 0f, 0f, 0f);
        btg_log(I($"the eye held {_btg_hold_n} frames on the spot, until {why} ({_btg_frames} frames after the swap began)"));
    }
    /// <summary>The swap's one line: the mode, the frames until the tag, the closest distance, why it tagged, how it ended.</summary>
    private void btg_log(string end) {
        if (!_btg_open) return;
        _btg_open = false;
        if (_btg_lines >= 100) return;
        _btg_lines++;
        string tag = _btg_tag_at < 0 ? "no tag" + (_btg_note.Length > 0 ? $" ({_btg_note})" : " (they did not cross before that)")
                   : I($"{(_btg_mode == 0 ? "they crossed (no tag in this mode)" : "tagged")} after {_btg_tag_at} frames, why: {(_btg_verdict == 1 ? "distance" : "passed")} ({_btg_d:F1} units apart)");
        string closest = _btg_min >= 0f ? I($", closest {_btg_min:F1} units") : "";
        string spot = _btg_carried ? " (the spot of the swap before)" : "";
        _logger.Info(I($"battle tag: {_btg_why}: bt_tag {_btg_mode} ({k_btg_label[_btg_mode]}), bt_tag_dist {_bt_tag_dist:F1}{spot}: {tag}{closest}; {end}") + (_btg_lines == 100 ? " (no more of these lines this session)" : ""));
    }
}
