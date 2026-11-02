// SPDX-License-Identifier: MIT

namespace FFXVR;

// Dream overlay (design-notes "Dream overlay (branch `dream`)"; research dream-overlay.md). The HD port's own dream filter
// (0x63DF70: film grain, a sepia tint, a soft vignette and dust specks, drawn as full-screen 2D quads after the UI pass opens)
// landed on the head-locked UI panel: a flat-screen-sized grainy sepia rectangle in the headset. The native helper decides
// (ffxvr_native.cpp: dream_tick reads the game's filter flag at Present, dream_rule matches the draws, ffxvr_eye2_begin draws
// mode 2's layers into the eye images); this side owns the setting and the overlay row. Outside the seven dream maps, with the
// game's flag 0, the rule does nothing at all (no reads), so normal play is unchanged.
//
//   dream_overlay (ini and overlay, applies at once; default 1):
//     0 = on the UI panel, as before (the old rectangle);
//     1 = hidden in the headset: drawn into the game's own target only, so the monitor is unchanged;
//     2 = tint both eyes: the three full-screen layers drawn into both eye images with the game's blend; the specks hidden.
public sealed unsafe partial class FFXVRModule {

    private int _dream_overlay = 1;            // ini dream_overlay: 0 panel (old), 1 hidden in the headset, 2 per-eye layers
    private delegate* unmanaged[Cdecl]<int, void> _fn_dream_set;            // ABI 18
    private delegate* unmanaged[Cdecl]<int*, int, int> _fn_dream_status;    // ABI 18: (out, n) -> count of values

    /// <summary>From push_all, every frame (a change in the overlay or the ini applies at the next draw).</summary>
    private void dream_push() {
        if (_fn_dream_set != null) _fn_dream_set(_dream_overlay);
    }

    /// <summary>"game filter: on/off, draws handled last frame N" (the native status; render thread).</summary>
    private string dream_status_text() {
        var f = _fn_dream_status; if (f == null) return "";
        int* v = stackalloc int[9];
        for (int i = 0; i < 9; i++) v[i] = 0;
        int n = f(v, 9);
        if (n < 9) return "";
        string map = v[4] >= 0 ? $"field map 0x{v[4]:X}" : "field map ?";
        return $"game filter: {(v[0] != 0 ? "on" : "off")}, draws handled last frame {v[2]} | {map} | windows this session {v[6]}, last {v[3]} frames"
             + $" | filter-shaped quads with the wrong colour (left alone): {v[5]}" + (v[8] != 0 ? $" | flag reads failed {v[8]}" : "");
    }

    /// <summary>The main window's row (after the UI panel's checkbox).</summary>
    private void dream_ui() {
        ImGui.Text("dream filter (sepia flashback look) in the headset:"); ImGui.SameLine();
        ImGui.RadioButton("hide (default)##dream", ref _dream_overlay, 1); ImGui.SameLine();
        ImGui.RadioButton("tint both eyes##dream", ref _dream_overlay, 2); ImGui.SameLine();
        ImGui.RadioButton("show on the panel (old)##dream", ref _dream_overlay, 0);
        if (_fn_dream_status != null) ImGui.TextDisabled("  " + esc(dream_status_text()));
    }
}
