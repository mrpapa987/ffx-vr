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
        shadow_pool_watch();
    }

    // Crash watch (chocobo session 2, 3 Nov 2026: design-notes "The Calm Lands crash"). The game crashed in the ShadowManager's
    // render (0x6D1530, called from the scene render 0x653600 via 0x6CA2D3): GetASpareGradMeshInstance (0x6CC980) returned
    // null ("Shouldn't get here, contact cg please") and the caller wrote through it. The pool is 16 instances per buffer
    // half: in-use flags at ShadowManager+0xDC4 + half * 16, the half at gva 0xCDEC24, the manager at gva 0xCDEC28; no code
    // in .text clears a flag. The two tile lists' counts are at +0x37A8 / +0x37AC. Read-only: every 60 frames, logged when the
    // counts change (at most one line every 2 s, 200 a session), so the next session shows whether the pool fills over time.
    private int  _sp_frame, _sp_used0 = -1, _sp_used1 = -1, _sp_l0, _sp_l1, _sp_lines; private long _sp_log_t; private bool _sp_warned;
    private string _sp_status = "";
    private void shadow_pool_watch() {
        if (_fn_safe_read == null || ++_sp_frame % 60 != 0) return;
        nint mgr = FhUtil.get_at<nint>(0x8DEC28);   // gva 0xCDEC28 (0x6D2390 returns it)
        if (mgr == 0) { _sp_status = "no ShadowManager"; return; }
        byte* f = stackalloc byte[32]; int* c = stackalloc int[2];
        if (_fn_safe_read((byte*)mgr + 0xDC4, f, 32) == 0 || _fn_safe_read((byte*)mgr + 0x37A8, c, 8) == 0) { _sp_status = "not readable"; return; }
        int u0 = 0, u1 = 0; for (int i = 0; i < 16; i++) { if (f[i] != 0) u0++; if (f[16 + i] != 0) u1++; }
        int half = FhUtil.get_at<int>(0x8DEC24);
        bool changed = u0 != _sp_used0 || u1 != _sp_used1;
        _sp_used0 = u0; _sp_used1 = u1; _sp_l0 = c[0]; _sp_l1 = c[1];
        if (changed) _sp_status = $"grad-mesh instances in use: half 0 {u0}/16, half 1 {u1}/16 (now {half}; the game crashes when one is full) | tile lists {c[0]}, {c[1]}";
        long now = System.Diagnostics.Stopwatch.GetTimestamp();
        if (changed && _sp_lines < 200 && now - _sp_log_t >= 2 * System.Diagnostics.Stopwatch.Frequency) {
            _sp_log_t = now; _sp_lines++;
            _logger.Info($"shadow pool: {_sp_status} | map {map_name()}{(_ride_att ? " | riding" : "")}{(_sp_lines == 200 ? " (no more of these lines this session)" : "")}");
        }
        if (!_sp_warned && Math.Max(u0, u1) >= 14) { _sp_warned = true; _logger.Warning($"shadow pool: {Math.Max(u0, u1)} of 16 grad-mesh instances in use - the game crashes at 16 (the Calm Lands crash of 3 Nov). Save soon; F8: try unticking 'keep characters drawn'."); }
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
        ImGui.TextDisabled("ShadowManager pool: " + esc(_sp_status));
    }
}
