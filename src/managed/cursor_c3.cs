// SPDX-License-Identifier: MIT

namespace FFXVR;

// Cursor stage C3 + first-person markers (design-notes "Cursor stage C3 + first-person markers"; SYNTHESIS 2.4 / stage C3,
// exe-battle-ui Q3/Q4, exe-verify 3/6). cursor_look=game (default): with the game cursor source, our markers take the game's
// look: the target hand cut from battle.dds (native finds the atlas among the UI pass's textures by the exe's own UV rects)
// placed beside the anchor as 0x8C0670 places it, and the turn pointer as the game's own world-space triangle (0x892590) with
// its theta and colours from the turn snapshot. The game's hand is always skipped in the headset: native finds battle.dds from
// the CTB panel's copy of the pointer (drawn for every targeted row, not through our detour), normally at the first targeting
// frame; until then a short wait without a target marker (k_c3_wait), then our red triangle. The search runs only in battle
// with the game source and the markers on. cursor_look=mod: our red / yellow triangles. The legacy source always draws ours.
// fp_self_marker=low (default) | hide | normal: in first person in the headset, the turn marker over the actor we look out of
// (the battle-follow pick: Tidus by default, a manual pick, the summoner during summons; else the field pick) is not drawn
// (low, hide), and a target marker on that actor (self-target) is drawn small, at chest height ahead (low) or not at all
// (hide). Third person and the diorama are unchanged.
public sealed unsafe partial class FFXVRModule {

    private bool  _cursor_look_game = true;   // ini cursor_look: game (true) / mod
    private float _cursor_hand_size = 1f;     // ini cursor_hand_size: the hand's size against the game's at its default battle view
    private int   _fp_self_marker = 0;        // ini fp_self_marker: 0 low, 1 hide, 2 normal
    private bool  _c3_vflip_inv;              // overlay only: sample battle.dds the other way up (if the hand shows the wrong art)
    private bool  _c3_atlas_ok;               // native holds battle.dds (asked this frame, before the markers are built)
    private int   _c3_wait_frames;            // Presents spent waiting for the find this session (C3-1)
    private readonly int[] _mk_kind = new int[32];
    private delegate* unmanaged[Cdecl]<int, float, float, uint, uint, int, int> _fn_cursor_look;
    private delegate* unmanaged[Cdecl]<int*, int, void> _fn_markers_kinds;
    private delegate* unmanaged[Cdecl]<void> _fn_cursor_atlas_dump;

    private const int k_c3_wait = 6;          // Presents of a live target without a marker while battle.dds is searched for, at most, per session
    private const int k_mk_hand_party = 2, k_mk_hand_monster = 3, k_mk_game_turn = 4, k_mk_self = 16;   // native Marker kinds

    /// <summary>The marker on the actor we look out of, in first person in the headset? (its turn marker would sit in our face)</summary>
    private bool c3_is_self(in D7Anchor a) => _fp_mode == 1 && _xr_on && a.actor != 0 && a.actor == System.Threading.Volatile.Read(ref _d7_follow_actor);

    /// <summary>Every frame before the markers are built: the look parameters (the turn triangle's from the snapshot just read)
    /// and native's answer, battle.dds held or not. The search is on only in battle with the game source and the markers on
    /// (C3-5): game_source = the caller is the game-source path.</summary>
    private void c3_look(bool game_source) {
        bool search = (game_source && _cursor_look_game && _markers_on && _c2_in_btl) || (_dmg3d && _c2_in_btl);   // 3D damage numbers need the digits' art too
        if (_fn_cursor_look != null) _c3_atlas_ok = _fn_cursor_look(search ? 1 : 0, _cursor_hand_size, _r7u.theta, _r7u.apex, _r7u.bas, _c3_vflip_inv ? 1 : 0) != 0;
    }

    /// <summary>After the markers are set (which resets the kinds): the kinds.</summary>
    private void c3_push() {
        if (_fn_markers_kinds != null) fixed (int* k = _mk_kind) _fn_markers_kinds(k, _mk_n);
    }
}
