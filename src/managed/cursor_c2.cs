// SPDX-License-Identifier: MIT

namespace FFXVR;

// Cursor stage C2 (design-notes "Cursor stage C2"; research SYNTHESIS.md sections 2.4-2.7 and 3, stage C2).
// cursor_source=game (default): the game's target hand and turn triangle are skipped while the headset shows our markers
// (the detours in diag7.cs ask native, ffxvr_cursor_skip), and our 3D markers are built from build 7's snapshots: exactly
// the candidates the game draws (its own mask, recorded only when the game itself calls its draw, under its own gates), at
// the game's own anchors (optpos[Chr+0xDF9]; the turn pointer's optpos[1] - 2), with the 30 Hz correction (expected ~0).
// cursor_source=legacy: the old memory-based markers (BtlWindow sel_chrs / cur_chr_id), hide list and pointer rule, unchanged.
// Without the hooks (diag_build7=0 at launch) the game source cannot work: it falls back to legacy and says so.
public sealed unsafe partial class FFXVRModule {

    private bool  _cursor_game_cfg = true;   // ini cursor_source: game (true) / legacy (false)
    private bool  _c2_active;                // decided once per frame (c2_decide): the game source is in use this frame
    private bool  _c2_dead;                  // a hook was queued but never called while the game showed its cursor
    private string _c2_dead_why = "";
    private int   _c2_gate_frames, _c2_win_frames;   // C2-4: Presents (cumulative, not reset when the gates close) with the turn-pointer gates open / a target window up
    private string _c2_status = "";
    private float _marker_lift = 2f;         // game source: the marker tip this many units above the game's anchor (2 = the game's own turn-pointer lift); ini marker_lift
    private int   _c2_prev_t, _c2_prev_u, _c2_since_t = 999, _c2_since_u = 999;   // Presents since a snapshot last arrived (backstop only)
    // C2-2/C2-3: a snapshot counts only if its stamp is within k_c2_live game frames of the counter and not older than the
    // floor: the frame the source / markers were last (re)enabled, the current battle's first frame, the current turn's first frame
    private uint  _c2_on_frame, _c2_btl_frame, _c2_turn_frame; private bool _c2_was_on, _c2_in_btl; private int _c2_turn_chr = -1;
    private uint  _c2_skip_frame = uint.MaxValue; private bool _c2_skip_val;   // C2-8 (iv): the skip latched per game frame (main thread)
    private readonly int[] _hide_push_src = new int[16]; private int _hide_push_n;   // C2-1: pushed hide-list slot -> _hide_list index
    private const int k_c2_live = 3,   /* C2R-2: one late frame never blinks */ k_c2_backstop = 180;   // game frames (the measured lag is a steady 1) / Presents (only if the counter itself stops)
    private delegate* unmanaged[Cdecl]<int, void> _fn_cursor_game;
    private delegate* unmanaged[Cdecl]<int> _fn_cursor_skip;

    private const uint k_c2_target_col = 0xFF4040FF, k_c2_turn_col = 0xFF40E0FF;   // the colours the markers always had
    private static float[][] hide_list_cursor_defaults() => hide_list_defaults();   // the two entries the game source makes redundant
    private static bool c2_is_default(float[] e) { foreach (float[] x in hide_list_cursor_defaults()) if (x[0] == e[0] && x[1] == e[1] && x[2] == e[2] && x[3] == e[3] && x[4] == e[4] && x[5] == e[5]) return true; return false; }

    /// <summary>Once per frame, before the markers are built: is the game source in use? It needs the hooks (queued at launch,
    /// which diag_build7=0 prevents) and must not have been found dead.</summary>
    private void c2_decide() {
        // C2-5: both hooks must have been queued (one() said "queued"). With only one of them the game source would leave that
        // cursor unmarked and unhidden, so both cursors fall back to legacy (the safer choice: one consistent source).
        bool hooks = _h7_tgt_ok && _h7_turn_ok && _h7_tgt != null && _h7_turn != null && _fn_cursor_skip != null;
        sbyte bst = FhUtil.get_at<sbyte>(0xD2A8E0); uint frame = FhUtil.get_at<uint>(0x1FCBBF0);
        if (bst != 0 && !_c2_in_btl) { _c2_btl_frame = frame; _c2_turn_chr = -1; } _c2_in_btl = bst != 0;   // a battle starts: nothing recorded before it counts
        // Dead hooks (C2-4/C2-5): the game shows its cursor (its own gates, read here) for 60 Presents in all, and that detour was
        // never called. Counted cumulatively: the gates close on every enemy turn, and a working hook is called on the first open frame.
        if (hooks && !_c2_dead && bst is 1 or 2 && FhUtil.get_at<uint>(0xF3C948) != 0 && FhUtil.get_at<uint>(0xF3D6E0) == 0) {
            if (_d7_turn_calls_total == 0 && FhUtil.get_at<uint>(0x1FCC08C) != 0 && FhUtil.get_at<uint>(0x858EF8) == 1 && ++_c2_gate_frames > 60) _c2_dead_why = "turn pointer (0x492590)";
            if (_d7_tgt_calls_total == 0 && FhUtil.get_at<uint>(0x1FCC08C) != 0 && FhUtil.get_at<uint>(0x858EF8) == 1) {   // C2R-1: the game's own gate; a window whose draw callback is the target-pointer stub, open (state >= 3): the game is drawing its hand
                uint stub = (uint)((nint)FhUtil.ptr_at<byte>(0) + k7_stub); bool win = false;
                for (int w = 0; w < 8; w++) if (FhUtil.get_at<uint>(0xF3C950 + w * 0xF0 + 0x90) == stub && (FhUtil.get_at<byte>(0xF3C950 + w * 0xF0 + 0x01) & 0xF) >= 3) win = true;
                if (win && ++_c2_win_frames > 60) _c2_dead_why = "target pointer (0x49C870)";
            }
            if (_c2_dead_why.Length != 0) { _c2_dead = true; _logger.Warning($"cursor stage C2: the {_c2_dead_why} hook is not being called (core.log may show a MinHook failure); both cursors back to the legacy markers"); }
        }
        bool was = _c2_active;
        _c2_active = _cursor_game_cfg && hooks && !_c2_dead;
        _c2_status = !_cursor_game_cfg ? "legacy (cursor_source=legacy)"
                   : _c2_active ? "game (the game's own cursor hooks)"
                   : _fn_cursor_skip == null || (!_h7_tgt_ok && !_h7_turn_ok) ? "legacy: the cursor hooks are not installed (diag_build7=0 at launch, or the native helper is missing)"
                   : !hooks ? $"legacy for both cursors: only the {(_h7_tgt_ok ? "target" : "turn")} pointer hook is installed (see core.log)"
                   : $"legacy for both cursors: the {_c2_dead_why} hook is installed but never called (see core.log)";
        if (was != _c2_active) _hide_list_dirty = true;   // the two default cursor entries come and go with the source
        bool on = _c2_active && _markers_on;
        if (on && !_c2_was_on) { _c2_on_frame = frame; _c2_since_t = _c2_since_u = 999; }   // C2-2: (re)enabled: older snapshots never count
        _c2_was_on = on;
    }

    /// <summary>Main thread (the cursor detours): native's skip, latched per game frame so the target and turn detours of one
    /// frame agree even if a Present falls between them (C2-8 window iv).</summary>
    private bool c2_skip_now() {
        var sk = _fn_cursor_skip; if (sk == null) return false;
        uint f = FhUtil.get_at<uint>(0x1FCBBF0);
        if (f != _c2_skip_frame) { _c2_skip_frame = f; _c2_skip_val = sk() != 0; }
        return _c2_skip_val;
    }

    /// <summary>Is a snapshot with this stamp live (C2-2/C2-3)? Within k_c2_live game frames of the counter (a negative lag, the
    /// counter behind the stamp, is not live), not older than the floor, and not stuck for k_c2_backstop Presents.</summary>
    private bool c2_live(uint stamp, uint frame, int since) {
        int lag = (int)(frame - stamp);
        return lag >= 0 && lag <= k_c2_live && since <= k_c2_backstop
            && (int)(stamp - _c2_on_frame) >= 0 && (int)(stamp - _c2_btl_frame) >= 0;
    }

    /// <summary>The game's anchor with the 30 Hz correction (SYNTHESIS 2.3): optpos - logic root + drawn root, or the raw
    /// optpos for attached actors / override matrices. Game (PS2) space.</summary>
    private static Vector3 c2_anchor(in D7Anchor t) => (t.flags & 3) != 0 ? t.opt : t.opt - t.tl + t.td;

    /// <summary>Our markers from the snapshots (render space). A snapshot counts while its game-frame stamp is live (c2_live):
    /// the game draws its cursors every frame it shows them, so the markers go within two game frames of the game's (confirm,
    /// enemy turn, auto-battle), and a main-thread hitch (counter stopped) does not blink them.</summary>
    private void c2_markers() {
        d7_read_tgt(); d7_read(_d7u, ref _d7u_seq, ref _r7u, ref _r7u_seq);   // read and bookkept even with the markers off (C2-2)
        _c2_since_t = _r7_seq != 0 && _r7_seq != _c2_prev_t ? 0 : Math.Min(_c2_since_t + 1, 999); _c2_prev_t = _r7_seq;
        _c2_since_u = _r7u_seq != 0 && _r7u_seq != _c2_prev_u ? 0 : Math.Min(_c2_since_u + 1, 999); _c2_prev_u = _r7u_seq;
        uint frame = FhUtil.get_at<uint>(0x1FCBBF0);
        bool live_u = _r7u_seq != 0 && _r7u.ok != 0 && c2_live(_r7u.stamp, frame, _c2_since_u);
        if (live_u && _r7u.chr != _c2_turn_chr) { _c2_turn_chr = _r7u.chr; _c2_turn_frame = _r7u.stamp; }   // a new turn: target snapshots before it are the last turn's
        bool live_t = _r7_seq != 0 && c2_live(_r7h.stamp, frame, _c2_since_t) && (_c2_turn_chr < 0 || (int)(_r7h.stamp - _c2_turn_frame) >= 0);
        if (!_markers_on) return;
        Vector3 ua = live_u ? c2_anchor(_r7u.a) : default; ua.Y -= 2.3f;   // the game's triangle: A = optpos[1] - 2, apex A - 0.3 (0x897860's table)
        if (live_t)
            for (int i = 0; i < _r7h.n && _mk_n < 32; i++) {
                Vector3 a = c2_anchor(_r7t[i]); a.Y -= _marker_lift;   // game y points down
                // C2-7: the acting member targeted (self, all allies): stack the red marker above the turn marker (each is 5 * scale
                // tall, tip down; the turn marker is drawn last and would cover it)
                if (live_u && _r7t[i].id == _r7u.a.id) a.Y = MathF.Min(a.Y, ua.Y - (2f * 2.5f * _marker_scale + 0.5f));
                _mk_pos[_mk_n * 3] = -a.X; _mk_pos[_mk_n * 3 + 1] = -a.Y; _mk_pos[_mk_n * 3 + 2] = a.Z; _mk_col[_mk_n] = k_c2_target_col; _mk_n++; _markers_drawn++;
            }
        if (live_u && _mk_n < 32) {
            _mk_pos[_mk_n * 3] = -ua.X; _mk_pos[_mk_n * 3 + 1] = -ua.Y; _mk_pos[_mk_n * 3 + 2] = ua.Z; _mk_col[_mk_n] = k_c2_turn_col; _mk_n++; _markers_drawn++;
        }
    }

    /// <summary>The hide list as pushed to native: with the game source, without the two default cursor entries (the game's
    /// cursors are skipped at the source, so those entries would only produce false positives).</summary>
    private int c2_hide_list(float* buf) {
        int cnt = 0;
        for (int li = 0; li < _hide_list.Count && cnt < 16; li++) {
            float[] e = _hide_list[li];
            if (_c2_active && c2_is_default(e)) continue;
            for (int k = 0; k < 6; k++) buf[cnt * 6 + k] = e[k];
            _hide_push_src[cnt] = li; cnt++;   // C2-1: native's hit counters are indexed by this slot
        }
        _hide_push_n = cnt;
        return cnt;
    }
}
