// SPDX-License-Identifier: MIT

namespace FFXVR;

// Adaptive UI panel (design-notes "Adaptive UI panel: depth and backing plate (branch `uidepth`)"). The game's UI pass is shown
// on a head-locked quad composited over the scene with no depth, so a 3D object nearer than the panel is covered by text that
// is stereoscopically behind it. The native helper (ffxvr_native.cpp: uid_depth_pass, uid_present, uib_copy) measures the
// nearest scene depth behind the visible UI content on the GPU and moves the panel in front of it, and can draw a soft dark
// halo under the UI. This side owns the settings, the overlay rows and the status line.
//
//   ui_adaptive (ini and overlay, default 1): the panel comes in front of near geometry; the width scales with the distance,
//     so the text keeps its size in view.
//   ui_adaptive_min (m, default 0.6): the panel never comes closer than this.
//   ui_adaptive_margin (factor, default 0.85): the panel goes to the nearest depth times this.
//   ui_backing (0..1, default 0.35; 0 = off): the opacity of the dark halo behind the UI content. The halo grows from near-opaque
//     UI only (text, borders, icons), so a translucent window's fill is not darkened behind (review UD-9).
//   The projection slot the depth is linearised with (Debug only, not saved): 0 auto (default: the slot the scene's VP block is
//   built from, detected each frame), 1 the near variant, 2 the main slot (it reads about 2x the near variant in this game).
public sealed unsafe partial class FFXVRModule {

    private bool  _ui_adaptive = true;            // ini ui_adaptive
    private float _ui_adaptive_min = 0.6f;        // ini ui_adaptive_min (m)
    private float _ui_adaptive_margin = 0.85f;    // ini ui_adaptive_margin (factor on the nearest depth)
    private float _ui_backing = 0.35f;            // ini ui_backing (0 = off)
    private int   _ui_adaptive_proj = 0;          // Debug: 0 auto (default), 1 near-variant projection, 2 main (not saved)
    private delegate* unmanaged[Cdecl]<int, float, float, float, int, void> _fn_uidepth_set;   // ABI 20
    private delegate* unmanaged[Cdecl]<float*, int, int> _fn_uidepth_status;                 // ABI 20: (out, n) -> count of values

    private static readonly string[] k_uid_states = {
        "no 3D panel", "adaptive depth off", "dream filter (left alone)", "waiting for the first depth readback", "no UI pixels",
        "a full-screen menu (left at the set distance)", "no scene depth this frame", "nothing nearer than the panel", "following the scene depth",
        "no depth readback lately (back to the set distance)", "camera cut (holding)" };

    /// <summary>From push_all, every frame (applies at once).</summary>
    private void uidepth_push() {
        if (_fn_uidepth_set != null) _fn_uidepth_set(_ui_adaptive ? 1 : 0, _ui_adaptive_min, _ui_adaptive_margin, _ui_backing, _ui_adaptive_proj);
    }

    /// <summary>The native status: 15 values (see ffxvr_uidepth_status), or false.</summary>
    private bool uidepth_status(float* v) {
        var f = _fn_uidepth_status; if (f == null) return false;
        for (int i = 0; i < 15; i++) v[i] = 0f;
        return f(v, 15) >= 15;
    }

    /// <summary>"panel at 0.82 m (nearest 0.96 m): following the scene depth" (the main window's live text).</summary>
    private string uidepth_live_text() {
        float* v = stackalloc float[15];
        if (!uidepth_status(v)) return "";
        int st = (int)v[4]; string state = st >= 0 && st < k_uid_states.Length ? k_uid_states[st] : $"state {st}";
        string near = v[2] > 0f ? $" (nearest {v[2]:F2} m)" : "";
        return $"panel at {v[0]:F2} m{near}: {state}";
    }

    /// <summary>The Debug status line: the live values, the passes' GPU cost and the depth row in use.</summary>
    private string uidepth_status_text() {
        float* v = stackalloc float[15];
        if (!uidepth_status(v)) return "";
        if (v[12] < 0f) return "the passes could not be built (no shader compiler?): see ffxvr_diag.log";
        string age = v[9] < 0f ? "none yet" : $"{v[9]:F0} frames old";
        return uidepth_live_text() + $" | target {v[1]:F2} m, UI on {v[3] * 100f:F0}% of the panel, readback {age}"
             + $" | GPU: depth pass {v[5]:F3} ms, UI tiles {v[6]:F3} ms, backing {v[7]:F3} ms{(v[8] != 0f ? " (drawn)" : "")}"
             + $" | depth row {(v[13] != 0f ? "main" : "near variant")} {v[10]:F4} {v[11]:F4}"
             + $" (the scene's VP block is built from: {(v[14] < 0f ? "not decided yet" : v[14] != 0f ? "main" : "near variant")})";
    }

    /// <summary>The main window's row (View block, under "UI on its own panel").</summary>
    private void uidepth_ui() {
        ImGui.Checkbox("UI panel comes in front of near objects (adaptive depth)", ref _ui_adaptive); ImGui.SameLine();
        ImGui.SetNextItemWidth(140); ImGui.SliderFloat("dark backing behind the UI (0 = off)##uidb", ref _ui_backing, 0f, 1f, "%.2f");
        if (_xr_want && _fn_uidepth_status != null) { ImGui.SameLine(); ImGui.TextDisabled(uidepth_live_text()); }
    }

    /// <summary>Debug > UI panel, next to the distance and width sliders: every setting and the status.</summary>
    private void uidepth_debug_ui() {
        ImGui.Checkbox("adaptive depth##uid", ref _ui_adaptive); ImGui.SameLine();
        ImGui.SetNextItemWidth(140); ImGui.SliderFloat("nearest (m)##uidmin", ref _ui_adaptive_min, 0.3f, 2f, "%.2f"); ImGui.SameLine();
        ImGui.SetNextItemWidth(140); ImGui.SliderFloat("margin (x the nearest depth)##uidmargin", ref _ui_adaptive_margin, 0.5f, 1f, "%.2f");
        ImGui.SliderFloat("backing plate opacity (0 = off)##uidback", ref _ui_backing, 0f, 1f, "%.2f");
        ImGui.Text("depth linearised with:"); ImGui.SameLine();
        ImGui.RadioButton("auto (default)##uidproj", ref _ui_adaptive_proj, 0); ImGui.SameLine();
        ImGui.RadioButton("near-variant projection##uidproj", ref _ui_adaptive_proj, 1); ImGui.SameLine();
        ImGui.RadioButton("main projection (reads ~2x; test only, not saved)##uidproj", ref _ui_adaptive_proj, 2);
        if (_fn_uidepth_status != null) ImGui.TextDisabled(uidepth_status_text());
    }
}
