// SPDX-License-Identifier: MIT

namespace FFXVR;

// vr-menu S2 (research vr-menu-design.md section 3, in the session scratchpad): the F8 window's own panel in the headset. While the
// headset is on (and the ImGui layer and vr_menu_panel are on, and native has the panel ready) the F8 window is drawn as a separate
// ImGui window "FFX VR###ffxvr_vr" at the right edge of the screen, 30 x 34 em, opaque, with bigger padding; every frame its rectangle
// (and that of a combo list it opened, the window's own beside it) goes to native (ffxvr_panel_set), which crops it out of the ImGui
// capture 1:1 into a world-locked quad fixed in the room (ffxvr_native.cpp vpanel_*; ffxvr_panel.h). The flat window keeps its own place and size in imgui.ini.
//
//   vr_menu_panel     1 (default) / 0: 0 keeps the F8 window on the head-locked ImGui layer, as before S2 (a way back)
//   vr_menu_distance  0.4..2.5 m (1.0): where the panel opens, ahead of the eyes (level)
//   vr_menu_height    -0.6..0.3 m (-0.15): its centre against eye height
//   vr_menu_scale     0.6..2.0 (1.0): one em is 0.9 degrees at 1 m times this
//   The grab writes the three: let go of the panel and they become where it was left (and its size); the next open uses them.
//
// Opening: F8 (keyboard only, the owner's decision). Where it was if that is within 50 degrees of the gaze and 3 m, else in front.
// Home, the headset's recentre and "Bring here" put it in front again. Practice targets slides it 30 degrees aside for the round.
public sealed unsafe partial class FFXVRModule {

    private bool  _vr_menu_panel = true;        // ini vr_menu_panel
    private float _vr_menu_distance = 1.0f;     // ini vr_menu_distance (m)
    private float _vr_menu_height = -0.15f;     // ini vr_menu_height (m)
    private float _vr_menu_scale = 1.0f;        // ini vr_menu_scale
    private bool  _vrp_available, _vrp_shown;   // native: the panel can be used (shaders, the session's swapchain) / it showed this frame
    private int   _vrp_latched;                 // native: the grips latched to it (1 left, 2 right)
    private int   _vrp_ends = -1;               // native's grab-end count last seen (-1: not read yet)
    private bool  _vrp_drawing, _vrp_drawn;     // the headset's window is being drawn / was drawn this frame (its rectangle is valid)
    private float _vrp_x0, _vrp_y0, _vrp_x1, _vrp_y1, _vrp_em;   // the window and the popups it opened this frame (ImGui coordinates)
    private float _vrp_wx0, _vrp_wy0, _vrp_wx1, _vrp_wy1;        // the window alone (the panel's pose is its centre: it stays put when a list opens)
    private readonly float[] _vrp_state = new float[12];
    private readonly float[] _vrp_set = new float[14];
    private delegate* unmanaged[Cdecl]<int, float*, int, void> _fn_panel_set;     // ABI 58
    private delegate* unmanaged[Cdecl]<float*, int, int> _fn_panel_state;         // ABI 58
    private delegate* unmanaged[Cdecl]<int, void> _fn_panel_place;               // ABI 58

    private void vrp_bind(Func<string, nint> req) {
        _fn_panel_set = (delegate* unmanaged[Cdecl]<int, float*, int, void>)req("ffxvr_panel_set");
        _fn_panel_state = (delegate* unmanaged[Cdecl]<float*, int, int>)req("ffxvr_panel_state");
        _fn_panel_place = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_panel_place");
    }
    private void vrp_clear() { _fn_panel_set = null; _fn_panel_state = null; _fn_panel_place = null; }

    private bool vrp_parse(string k, string v) {
        switch (k) {
            case "vr_menu_panel":    pb(k, v, ref _vr_menu_panel); return true;
            case "vr_menu_distance": pf(k, v, ref _vr_menu_distance, 0.4f, 2.5f); return true;
            case "vr_menu_height":   pf(k, v, ref _vr_menu_height, -0.6f, 0.3f); return true;
            case "vr_menu_scale":    pf(k, v, ref _vr_menu_scale, 0.6f, 2f); return true;
            default: return false;
        }
    }
    private string vrp_ini_text() =>
        $"vr_menu_panel={(_vr_menu_panel ? 1 : 0)}\n" +
        $"vr_menu_distance={_vr_menu_distance.ToString("F2", CI)}\n" +
        $"vr_menu_height={_vr_menu_height.ToString("F2", CI)}\n" +
        $"vr_menu_scale={_vr_menu_scale.ToString("F2", CI)}\n";

    /// <summary>The F8 window is the headset's panel window this frame (decided from last frame's native state).</summary>
    private bool vrp_use() => _vr_menu_panel && _xr_on && _imgui_layer && _vrp_available && _fn_panel_set != null;

    /// <summary>our_font_px's VR rule (design 3.2): in the headset's window the text is at least 28 px (the panel image never far below
    /// the display's density) and at most 45 px (a 30 x 34 em window must fit the 1536 x 1536 panel). final: the size after FontScaleDpi.</summary>
    private float vrp_font(float final) => vrp_use() ? Math.Clamp(final, 28f, 45f) : final;

    /// <summary>draw_window in the headset: the F8 window as its own window at the right edge, 30 x 34 em, opaque, bigger targets.</summary>
    private void draw_window_vr() {
        float em = ImGui.GetFontSize();
        Vector2 ds = ImGui.GetIO().DisplaySize;
        var size = new Vector2(30f * em, 34f * em);
        ImGui.SetNextWindowPos(new Vector2(MathF.Max(0f, ds.X - size.X - em), em), ImGuiCond.Always);
        ImGui.SetNextWindowSize(size, ImGuiCond.Always);
        Vector4 bg = *ImGui.GetStyleColorVec4(ImGuiCol.WindowBg); bg.W = 1f;   // opaque: compresses cleanly in Virtual Desktop's video
        var st = ImGui.GetStyle();
        ImGui.PushStyleColor(ImGuiCol.WindowBg, bg);
        ImGui.PushStyleVar(ImGuiStyleVar.FramePadding, st.FramePadding * 1.5f);
        ImGui.PushStyleVar(ImGuiStyleVar.ItemSpacing, st.ItemSpacing * 1.25f);
        _vrp_drawing = true;
        try {
            if (ImGui.Begin("FFX VR###ffxvr_vr", ref _window_open, ImGuiWindowFlags.NoMove | ImGuiWindowFlags.NoResize | ImGuiWindowFlags.NoCollapse | ImGuiWindowFlags.NoSavedSettings | ImGuiWindowFlags.NoDocking)) {
                Vector2 p = ImGui.GetWindowPos(), s = ImGui.GetWindowSize();
                _vrp_x0 = p.X; _vrp_y0 = p.Y; _vrp_x1 = p.X + s.X; _vrp_y1 = p.Y + s.Y; _vrp_em = ImGui.GetFontSize(); _vrp_drawn = true;
                _vrp_wx0 = _vrp_x0; _vrp_wy0 = _vrp_y0; _vrp_wx1 = _vrp_x1; _vrp_wy1 = _vrp_y1;
                draw_window_body();
            }
        }
        finally { ImGui.End(); _vrp_drawing = false; ImGui.PopStyleVar(2); ImGui.PopStyleColor(1); }
    }

    /// <summary>Inside a popup the headset's window opened (a combo list): its rectangle joins the crop, so the list shows on the
    /// panel too (design 3.6; tooltips follow the mouse and wait for S3's pointer).</summary>
    private void vrp_union_popup() {
        if (!_vrp_drawing || !_vrp_drawn) return;
        Vector2 p = ImGui.GetWindowPos(), s = ImGui.GetWindowSize();
        _vrp_x0 = MathF.Min(_vrp_x0, p.X); _vrp_y0 = MathF.Min(_vrp_y0, p.Y); _vrp_x1 = MathF.Max(_vrp_x1, p.X + s.X); _vrp_y1 = MathF.Max(_vrp_y1, p.Y + s.Y);
    }

    /// <summary>Every frame after the window (render_imgui, render thread, after ffxvr_begin_frame): native's state first (a finished
    /// grab into the three settings), then this frame's rectangle to native, which pairs it with the capture of these draws at the next
    /// begin_frame (stamped g_frames, used at g_frames + 1).</summary>
    private void vrp_push() {
        if (_fn_panel_set == null) return;
        // native's state first: a grab that ended in this frame's xr_tick must reach the settings before they are sent back (else the
        // next frame would show the old size or place for one frame)
        if (_fn_panel_state != null) {
            int n; fixed (float* o = _vrp_state) n = _fn_panel_state(o, _vrp_state.Length);
            if (n >= 12) {
                _vrp_available = _vrp_state[0] != 0f; _vrp_shown = _vrp_state[1] != 0f; _vrp_latched = (int)_vrp_state[2];
                int ends = (int)_vrp_state[6];
                if (_vrp_ends >= 0 && ends != _vrp_ends && float.IsFinite(_vrp_state[3]) && float.IsFinite(_vrp_state[4]) && float.IsFinite(_vrp_state[5])) {
                    // a grab ended: the panel opens where it was left, at its size (saved by the debounced ini save)
                    _vr_menu_distance = Math.Clamp(_vrp_state[3], 0.4f, 2.5f); _vr_menu_height = Math.Clamp(_vrp_state[4], -0.6f, 0.3f); _vr_menu_scale = Math.Clamp(_vrp_state[5], 0.6f, 2f);
                }
                _vrp_ends = ends;
            }
        }
        int flags = (_vrp_drawn && _window_open ? 1 : 0) | (_vr_menu_panel ? 2 : 0);
        Vector2 ds = ImGui.GetIO().DisplaySize;
        _vrp_set[0] = _vrp_x0; _vrp_set[1] = _vrp_y0; _vrp_set[2] = _vrp_x1; _vrp_set[3] = _vrp_y1; _vrp_set[4] = _vrp_em;
        _vrp_set[5] = ds.X; _vrp_set[6] = ds.Y; _vrp_set[7] = _vr_menu_distance; _vrp_set[8] = _vr_menu_height; _vrp_set[9] = _vr_menu_scale;
        _vrp_set[10] = _vrp_wx0; _vrp_set[11] = _vrp_wy0; _vrp_set[12] = _vrp_wx1; _vrp_set[13] = _vrp_wy1;
        fixed (float* v = _vrp_set) _fn_panel_set(flags, v, _vrp_set.Length);
        _vrp_drawn = false;
    }

    /// <summary>F8 > Controls > Menu panel (in the headset).</summary>
    private void vrp_ui() {
        ImGui.SeparatorText("Menu panel (in the headset)");
        ui_check("vr_menu_panel", "This menu on its own panel in the room##vrp", ref _vr_menu_panel, UiTag.None,
                 "In the headset this menu is a panel fixed in the room, about 1 m ahead and a little below your eyes. Grip it with either hand (pointing at it or reaching it) "
               + "to move it, with both grips to resize it. Off: the menu is on the head-locked layer with Fahrenheit's menus, as before.");
        if (!_vr_menu_panel) return;
        ui_slider("vr_menu_distance", "distance (m)##vrp", ref _vr_menu_distance, 0.4f, 2.5f, "%.2f", UiTag.None, "Where it opens: this far ahead of your eyes. Letting go of the panel sets it.", 140f); ImGui.SameLine();
        ui_slider("vr_menu_height", "height (m)##vrp", ref _vr_menu_height, -0.6f, 0.3f, "%.2f", UiTag.None, "Where it opens: its centre against your eye height. Letting go of the panel sets it.", 140f); ImGui.SameLine();
        ui_slider("vr_menu_scale", "size##vrp", ref _vr_menu_scale, 0.6f, 2f, "%.2fx", UiTag.None, "1 = one line of text about 0.9 degrees tall at 1 m. Resizing it with both grips sets it.", 140f);
        if (_fn_panel_place != null && ImGui.SmallButton("Bring it here##vrp")) _fn_panel_place(1);
        ui_tip("The panel in front of you again (Home and the headset's recentre do the same).");
        if (_vr_menu_panel && _xr_on && _imgui_layer && !_vrp_available) { ImGui.SameLine(); ImGui.TextDisabled("(not available: the panel shaders or swapchain failed, see ffxvr_diag.log)"); }
        if (!_imgui_layer) { ImGui.SameLine(); ImGui.TextDisabled("(needs \"Show mod menus in the headset\", View > Panels)"); }
    }
}
