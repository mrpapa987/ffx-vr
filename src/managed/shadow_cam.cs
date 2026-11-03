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

    // The ShadowManager's grad-mesh pool (design-notes "Headset round 1 of chocobo": the Calm Lands crash). Native
    // (ffxvr_shadow_guard_install, ABI 34) installs two things once, each only over the bytes it expects:
    // - the guard: GetASpareGradMeshInstance (0x6CC980) returning null (all 16 instances of a buffer half in use; the game then
    //   wrote through the null at 0x6D1736) now skips that shadow for the frame instead of crashing;
    // - the distance budget: Ch_DrawMain's per-actor shadow request (0x830240) is skipped for characters farther than
    //   shadow_near from the player once the pool holds shadow_budget instances (they stay drawn, without a shadow).
    // The pool is never released while the game runs (the flags are cleared only by the pool's setup, 0x6D1B40, from the
    // renderer's resource build), so the budget is what keeps the near characters' shadows for the session.
    // Install: from the job-join hook on the main thread (between frames) when it runs, else from here after 600 frames.
    // Watch: every 60 frames the native status; a "shadow pool:" line when the counts change (one every 2 s, 200 a session).
    private float _shadow_near = 300f;          // ini shadow_near: game units (11.4 a metre: 300 is about 26 m); 0 = no budget
    private int   _shadow_budget = 12;          // ini shadow_budget: of the 16 instances per half; far characters get none past it
    private delegate* unmanaged[Cdecl]<int> _fn_sg_install;                     // ABI 34
    private delegate* unmanaged[Cdecl]<float, int, void> _fn_sg_budget;        // ABI 34
    private delegate* unmanaged[Cdecl]<int*, int, int> _fn_sg_status;          // ABI 34
    private volatile bool _sg_tried; private volatile int _sg_state; private volatile bool _sg_on_main;
    private int  _sp_frame, _sp_used0 = -1, _sp_used1 = -1, _sp_hits = -1, _sp_skips = -1, _sp_lines, _sp_frames_up; private long _sp_log_t; private bool _sp_warned, _sp_inst_logged;
    private string _sp_status = "";
    private void shadow_pool_watch() {
        if (_fn_sg_budget != null) _fn_sg_budget(_shadow_near, _shadow_budget);
        if (!_sg_tried && _native_ready && _fn_sg_install != null && ++_sp_frames_up > 600) { _sg_tried = true; _sg_state = _fn_sg_install(); }   // no job-join hook: from the render thread
        if (_sg_tried && !_sp_inst_logged) {
            _sp_inst_logged = true; int st = _sg_state;
            _logger.Info($"shadow pool: guard {((st & 3) == 3 ? "on (both tile lists)" : (st & 3) != 0 ? "on for one tile list only" : "NOT installed (the bytes were not as expected)")}, distance budget {((st & 4) != 0 ? "on" : "NOT installed")} ({(_sg_on_main ? "the main thread" : "the render thread")}) | shadow_near {_shadow_near:F0} units, budget {_shadow_budget} of 16");
        }
        if (_fn_sg_status == null || ++_sp_frame % 60 != 0) return;
        int* v = stackalloc int[6]; for (int i = 0; i < 6; i++) v[i] = 0;
        if (_fn_sg_status(v, 6) < 6) return;
        int u0 = v[4], u1 = v[5], hits = v[1], skips = v[2];
        bool changed = u0 != _sp_used0 || u1 != _sp_used1 || hits != _sp_hits || (skips > 0) != (_sp_skips > 0);
        _sp_used0 = u0; _sp_used1 = u1; _sp_hits = hits; _sp_skips = skips;
        if (changed || _sp_status.Length == 0)
            _sp_status = $"grad-mesh instances in use: half 0 {u0}/16, half 1 {u1}/16 | far characters left without a shadow {skips} times (budget {_shadow_budget} past {_shadow_near:F0} units) | the guard skipped {hits} (would have crashed)";
        long now = System.Diagnostics.Stopwatch.GetTimestamp();
        if (changed && _sp_lines < 200 && now - _sp_log_t >= 2 * System.Diagnostics.Stopwatch.Frequency) {
            _sp_log_t = now; _sp_lines++;
            _logger.Info($"shadow pool: {_sp_status} | map {map_name()}{(_ride_att ? " | riding" : "")}{(_sp_lines == 200 ? " (no more of these lines this session)" : "")}");
        }
        if (!_sp_warned && hits > 0) { _sp_warned = true; _logger.Warning($"shadow pool: full: the guard skipped a shadow that would have crashed the game (the Calm Lands crash of 3 Nov). Nothing to do; shadows may be missing until the game is restarted."); }
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
        ImGui.SetNextItemWidth(160); ImGui.SliderFloat("shadows only within this distance once the pool fills (units, 0 = off)##sgnear", ref _shadow_near, 0f, 1000f, "%.0f"); ImGui.SameLine();
        ImGui.SetNextItemWidth(120); ImGui.SliderInt("of 16 instances##sgbudget", ref _shadow_budget, 4, 16); ImGui.SameLine();
        ImGui.TextDisabled("11.4 units a metre; far characters stay drawn, without a shadow");
    }
}
