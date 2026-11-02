// SPDX-License-Identifier: MIT

namespace FFXVR;

// Shadow camera (design-notes "Character shadows: the light camera (branch `shadows`)"; research shadows-one-eye.md). The
// game's ShadowManager draws each character from straight above into its own tile of a 4096x4096 shadow atlas. Those
// uploads carry the light's orthographic projection next to the scene's perspective one, so the camera override took them
// for the scene camera and put the head pose and each eye's FOV into the light: a character's shadow showed in one eye only,
// depending on where it stood relative to the gaze, with a sheared shape. The native helper now recognises the light camera
// (ffxvr_native.cpp: light_proj, maybe_override) and leaves it alone; this side owns the two settings and the overlay rows.
//
//   shadow_cam (ini and overlay, applies at the next frame; default 1):
//     1 = light cameras left alone: both eyes draw the game's shadow tiles;
//     0 = overridden as the scene camera, as before (for an A/B comparison only).
//   fp_self_shadow (ini and overlay, applies at once; default 1):
//     1 = the player's own shadow is drawn with the game's light, stable, in every view and body mode (in first person with
//         fp_body_mode 0 the body is hidden, its shadow on the ground is not);
//     0 = hidden in first person's whole-body modes only: fp_body_mode 0 (as before this build), or the VR hands with floating
//         hands, where the hand parts' shadow is hidden too. With fp_body_mode 1 or 2 it changes nothing.
public sealed unsafe partial class FFXVRModule {

    private int _shadow_cam = 1;               // ini shadow_cam: 1 the game's light cameras (default), 0 overridden (old)
    private int _fp_self_shadow = 1;           // ini fp_self_shadow: 1 own shadow always drawn (default), 0 hidden with a wholly hidden body
    private delegate* unmanaged[Cdecl]<int, int, void> _fn_shadow_set;      // ABI 19
    private delegate* unmanaged[Cdecl]<int*, int, int> _fn_shadow_status;  // ABI 19: (out, n) -> count of values

    /// <summary>From push_all, every frame (shadow_cam applies at the next Present, fp_self_shadow at once).</summary>
    private void shadow_push() {
        if (_fn_shadow_set != null) _fn_shadow_set(_shadow_cam, _fp_self_shadow);
    }

    /// <summary>"light cameras last frame N | this session M ..." (the native status; render thread).</summary>
    private string shadow_status_text() {
        var f = _fn_shadow_status; if (f == null) return "";
        int* v = stackalloc int[8];
        for (int i = 0; i < 8; i++) v[i] = 0;
        int n = f(v, 8);
        if (n < 8) return "";
        string s = $"light cameras last frame {v[0]} ({(v[6] != 0 ? "the game's" : "overridden")}) | this session {v[1]}";
        if (v[2] != 0) s += $", not looking straight down {v[2]}";
        if (v[3] != 0) s += $", overridden with shadow_cam 0 {v[3]}";
        if (v[7] == 0) s += $" | own shadow draws hidden last frame {v[4]}";
        return s + $" | map lines logged {v[5]}";
    }

    /// <summary>The main window's row (in the View block, after the headset field of view).</summary>
    private void shadow_ui() {
        bool on = _shadow_cam != 0;
        if (ImGui.Checkbox("character shadows from the game's light (both eyes)", ref on)) _shadow_cam = on ? 1 : 0;
        ImGui.SameLine();
        bool self = _fp_self_shadow != 0;
        if (ImGui.Checkbox("my own shadow in first person", ref self)) _fp_self_shadow = self ? 1 : 0;
        if (_shadow_cam == 0) { ImGui.SameLine(); ImGui.TextDisabled("(off: the old one-eye shadows, for comparison)"); }
    }

    /// <summary>Debug > Character shadows: the same two settings and the status line.</summary>
    private void shadow_debug_ui() {
        ImGui.TextWrapped("The game draws each character from straight above into a tile of its shadow atlas (the ShadowManager's light camera). "
                        + "Left alone, both eyes get the game's tiles. The old behaviour moved that camera with the head, so a shadow showed in one eye only.");
        ImGui.RadioButton("leave the light camera alone (default)##shadowcam", ref _shadow_cam, 1); ImGui.SameLine();
        ImGui.RadioButton("override it as the scene camera (old, A/B)##shadowcam", ref _shadow_cam, 0);
        ImGui.RadioButton("first person: draw my own shadow (default)##selfshadow", ref _fp_self_shadow, 1); ImGui.SameLine();
        ImGui.RadioButton("hide it with the body (whole-body mode only)##selfshadow", ref _fp_self_shadow, 0);
        if (_fn_shadow_status != null) ImGui.TextDisabled(esc(shadow_status_text()));
    }
}
