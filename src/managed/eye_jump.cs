// SPDX-License-Identifier: MIT

namespace FFXVR;

// Eye jumps (roadmap 4.1; native ffxvr_eyejump.h, ABI 44).
// The native side blinks when the first-person eye base moves more than eye_jump_units in one frame (battle start, a swap or
// a summon handing the eyes on, the manual cycle, a scripted teleport), at most one blink start per 30 frames, and the
// blink now also blanks frames with a UI pass (all of battle, field frames with a text box) while the UI panel stays.
// This side asks native to face the enemies when the battle follow changes (battle_face): the centre of the live, visible
// monsters, sent once some are found (within 120 frames: they can appear a few frames after the start), and keeps the field
// heading over the battle (ffxvr_eye_event 2 at the start, 3 at the end: restored at the first frame in the same view).
//   ini eye_jump_blink (0-15 frames, 6; 0 = no blink), eye_jump_units (4-40, 8), battle_face (0 off, 1 body forward (default),
//   2 where you look). F8 > camera cuts, the "eye jumps:" and "battle: face the enemies:" lines.
public sealed unsafe partial class FFXVRModule {

    private int   _eye_jump_blink = 6;    // ini eye_jump_blink: black frames when the eye jumps (0 = off)
    private float _eye_jump_units = 8f;   // ini eye_jump_units: a jump is a move of more than this in one frame (about 0.7 m)
    private int   _battle_face = 1;       // ini battle_face: 0 off, 1 body forward (the enemies ahead of the recentred forward), 2 where you look
    private delegate* unmanaged[Cdecl]<int, float, int, int*, int, int> _fn_eye_jump_set;   // ABI 44
    private delegate* unmanaged[Cdecl]<int, float, float, float, void> _fn_eye_event;       // ABI 44

    private int    _ej_face_left;          // frames the face request still looks for monsters (0 = none pending)
    private string _ej_face_why = "";
    private int    _ej_face_logs;          // face: lines this session (60 at most)
    private readonly int[] _ej_stats = new int[6];   // native: jumps, blinks, extended, held back, headings turned, kept

    private bool ej_parse(string k, string v) {
        switch (k) {
            case "eye_jump_blink": pi(k, v, ref _eye_jump_blink, 0, 15); return true;
            case "eye_jump_units": pf(k, v, ref _eye_jump_units, 4f, 40f); return true;
            case "battle_face":    pe(k, v, ref _battle_face, 0, 1, 2); return true;
            default: return false;
        }
    }
    private string ej_ini_text() {
        var ci = CI;
        return
            $"eye_jump_blink={_eye_jump_blink.ToString(ci)}\n" +
            $"eye_jump_units={_eye_jump_units.ToString("F1", ci)}\n" +
            $"battle_face={_battle_face.ToString(ci)}\n";
    }
    /// <summary>Every frame (push_all): the settings, and the native counts for the overlay.</summary>
    private void ej_push() {
        if (_fn_eye_jump_set == null) return;
        fixed (int* o = _ej_stats) _fn_eye_jump_set(_eye_jump_blink, _eye_jump_units, _battle_face, o, 6);
    }
    /// <summary>push_player, at the in_battle edge (before the follow state is reset for the new battle).</summary>
    private void ej_battle_edge(bool in_battle) {
        _ej_face_left = 0;
        if (_fn_eye_event != null) _fn_eye_event(in_battle ? 2 : 3, 0f, 0f, 0f);
    }
    /// <summary>push_player: the battle follow moved to another party member (start, swap, summon, dismissal, manual).</summary>
    private void ej_follow(string why) {
        if (_battle_face == 0 || (_fp_mode != 1 && _fp_mode != 3)) return;
        _ej_face_left = 120; _ej_face_why = why ?? "";
    }
    /// <summary>push_player, every battle frame: while a face request is pending, the centre of the live, visible monsters
    /// (as push_battle_anchor finds them), sent to native in render space (-x, -y, z) once there is at least one.</summary>
    private void ej_face_tick() {
        if (_ej_face_left <= 0 || _fn_eye_event == null) return;
        _ej_face_left--;
        Chr* mons = Globals.Battle.monster_characters;
        float sx = 0, sy = 0, sz = 0; int n = 0;
        for (int i = 0; i < k_mon_n; i++)
            if (mons != null && read(mons + i, out Chr mc) && mc.actor != null && read(mc.actor, out Actor ma) && live(ma) && mc.stat_visible != 0) { sx += ma.chr_pos_vec.X; sy += ma.chr_pos_vec.Y; sz += ma.chr_pos_vec.Z; n++; }
        if (n == 0) {
            if (_ej_face_left == 0 && _ej_face_logs < 60) { _ej_face_logs++; _logger.Info($"face: battle follow ({_ej_face_why}): no live, visible monster within 120 frames, the heading is kept"); }
            return;
        }
        _ej_face_left = 0;
        _fn_eye_event(1, -sx / n, -sy / n, sz / n);
        if (_ej_face_logs < 60) { _ej_face_logs++; _logger.Info($"face: battle follow ({_ej_face_why}) -> the centre of {n} monster(s) at ({sx / n:F0} {sy / n:F0} {sz / n:F0}), battle_face {_battle_face}{(_ej_face_logs == 60 ? " (no more of these lines this session)" : "")}"); }
    }
    /// <summary>F8, under "camera cuts:" (next to "blink").</summary>
    private void ej_ui() {
        ImGui.Text("eye jumps:"); ImGui.SameLine();
        ImGui.SetNextItemWidth(140); ImGui.SliderInt("blink frames##ej", ref _eye_jump_blink, 0, 15, _eye_jump_blink == 0 ? "off" : "%d"); ImGui.SameLine();
        ImGui.SetNextItemWidth(140); ImGui.SliderFloat("jump over (units)##ej", ref _eye_jump_units, 4f, 40f, "%.0f");
        if (_fn_eye_jump_set != null) { ImGui.SameLine(); ImGui.TextDisabled($"seen {_ej_stats[0]}, blinked {_ej_stats[1]} (extended {_ej_stats[2]}, held back {_ej_stats[3]})"); }
        ImGui.Text("battle: face the enemies:"); ImGui.SameLine();
        ImGui.RadioButton("off##bf", ref _battle_face, 0); ImGui.SameLine();
        ImGui.RadioButton("body forward##bf", ref _battle_face, 1); ImGui.SameLine();
        ImGui.RadioButton("where you look##bf", ref _battle_face, 2);
        if (_fn_eye_jump_set != null) { ImGui.SameLine(); ImGui.TextDisabled($"turned {_ej_stats[4]}, kept {_ej_stats[5]} (under 15°)"); }
    }
}
