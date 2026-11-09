// SPDX-License-Identifier: MIT

namespace FFXVR;

// Heat haze (design-notes "Heat haze in fire (branch `haze`)"; research fire-haze.md). The shimmer over torches and fire spells is
// a PS2-style effect that samples a copy of the frame at a screen position its pixel shader computes from its own constant buffer.
// Eye 2 got its own copy but eye 1's screen position, so the flame showed the scene from about a quarter screen away (a grey-blue
// "reflection") in one eye. The native helper now gives those draws eye 2's pixel-shader data (ffxvr_native.cpp: haze_match,
// haze_ps_upload); this side owns the setting and the overlay rows.
//
//   heat_haze (ini and overlay, applies at the next frame; default 0 since the loads build):
//     0 = off in the headset: the haze draws are skipped while the headset shows the 3D view (the monitor, which shows eye 1,
//         loses them too then); the virtual screen, the stereo screen and flat mode keep them;
//     1 = fixed per eye (experimental: in testing it did not fix the Luca torches): the haze draws get eye 2's pixel-shader
//         camera data;
//     2 = as before the haze build (for an A/B comparison).
//   The 10 s "haze:" line is written in modes 1 and 2 only (the test modes).
public sealed unsafe partial class FFXVRModule {

    private int _heat_haze = 0;                                            // ini heat_haze: 0 off in the headset (default since the loads build), 1 fixed per eye (experimental), 2 as before
    private delegate* unmanaged[Cdecl]<int, void> _fn_haze_set;            // ABI 22
    private delegate* unmanaged[Cdecl]<int*, int, int> _fn_haze_status;    // ABI 22: (out, n) -> count of values

    /// <summary>From push_all, every frame (applies at the next Present).</summary>
    private void haze_push() {
        if (_fn_haze_set != null) _fn_haze_set(_heat_haze);
    }

    /// <summary>The native status: 12 values (see ffxvr_haze_status), or false.</summary>
    private bool haze_status(int* v) {
        var f = _fn_haze_status; if (f == null) return false;
        for (int i = 0; i < 12; i++) v[i] = 0;
        return f(v, 12) >= 12;
    }

    /// <summary>"heat haze: 4 draws this frame, fixed" (the main window's live text).</summary>
    private string haze_live_text() {
        int* v = stackalloc int[12];
        if (!haze_status(v)) return "";
        if (v[1] == 0) return "heat haze: none in view";
        // HZ-5: "fixed" only when eye 2 really got pixel-shader images this frame
        string what = v[0] == 1 ? (v[2] > 0 ? $"fixed ({v[2]} eye-2 uploads)" : "matched, nothing uploaded (see Developer > Rendering > Heat haze)")
                    : v[0] == 0 ? (v[3] > 0 ? "skipped" : "drawn (not the headset's 3D view)") : "as before";
        return $"heat haze: {v[1]} draws this frame, {what}";
    }

    /// <summary>The Developer tab status line: the frame's counts and the session's.</summary>
    private string haze_status_text() {
        int* v = stackalloc int[12];
        if (!haze_status(v)) return "";
        string s = haze_live_text() + $" | this session: matched {v[4]}, eye-2 uploads {v[5]}";
        if (v[6] != 0) s += $", no pixel-shader image {v[6]}";
        if (v[7] != 0) s += $", buffer shared with the vertex shader {v[7]}";
        if (v[11] != 0) s += $", size mismatch {v[11]}";
        s += $" | other draws with pixel-shader-only camera data: {v[8]} of {v[9]} sampled";
        if (v[8] != 0) s += $" (eyes differ {v[10]})";
        return s;
    }

    private const string k_haze_tip = "Torches and fire spells draw a shimmer that samples a copy of the frame; in the headset one eye showed a grey patch beside the flame. "
                                    + "Off in the headset hides the shimmer while the headset shows the 3D view (the fire itself stays); as the game: unchanged; "
                                    + "per eye is experimental (in testing it did not fix the Luca torches).";
    /// <summary>F8 > View > Graphics & performance, the experimental choice last (the status: Developer > Logging & status).</summary>
    private void haze_ui() {
        ImGui.Text("Heat shimmer over fire:"); ImGui.SameLine();
        ui_radio("heat_haze", "off in the headset##haze", ref _heat_haze, 0, UiTag.None, k_haze_tip); ImGui.SameLine();
        ui_radio("heat_haze", "as the game##haze", ref _heat_haze, 2, UiTag.None, k_haze_tip); ImGui.SameLine();
        ui_radio("heat_haze", "per eye##haze", ref _heat_haze, 1, UiTag.Experimental, k_haze_tip);
    }

    /// <summary>Developer > Rendering > Heat haze: the explanation and the status line (the setting: View > Graphics & performance).</summary>
    private void haze_debug_ui() {
        ImGui.TextWrapped("Torches and fire spells draw a shimmer that samples a copy of the frame. Eye 2 used eye 1's screen position for it, "
                        + "so the flame showed a grey patch of the scene beside it in one eye. Off in the headset hides the shimmer while the headset "
                        + "shows the 3D view, on the monitor too; the fire itself stays. Fixed per eye is experimental: in testing it did not fix "
                        + "the Luca torches.");
        ImGui.TextDisabled(I($"heat_haze={_heat_haze} (View > Graphics & performance > Heat shimmer over fire)"));
        if (_fn_haze_status != null) ImGui.TextDisabled(esc(haze_status_text()));
        ImGui.TextDisabled("ffxvr_diag.log: 'haze:' every 10 s while haze draws are seen (modes 1 and 2 only); Frame census: HAZE lines");
    }
}
