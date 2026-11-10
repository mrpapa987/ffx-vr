// SPDX-License-Identifier: MIT

namespace FFXVR;

// vr-menu S2 (research vr-menu-design.md section 3, in the session scratchpad): the F8 window's own panel in the headset. While the
// headset is on (and the ImGui layer and vr_menu_panel are on, and native has the panel ready) the F8 window is drawn as a separate
// ImGui window "FFX VR###ffxvr_vr" at the right edge of the screen, 30 x 34 em, opaque, with bigger padding; every frame its rectangle
// (and that of a combo list or tooltip it opened, the window's own beside it) goes to native (ffxvr_panel_set), which sends every
// ImGui draw clipped inside it into the panel's own capture and shows that 1:1 as a world-locked quad fixed in the room
// (ffxvr_native.cpp vpanel_*; ffxvr_panel.h). The head-locked ImGui layer keeps everything else (the save screen, F7). ImGui draws a
// top-level window's decorations with the whole display as the clip, so this window and its popups draw none: their backgrounds
// are drawn here, clipped to themselves (review S1). The flat window keeps its own place and size in imgui.ini.
//
//   vr_menu_panel     1 (default) / 0: 0 keeps the F8 window on the head-locked ImGui layer, as before S2 (a way back)
//   vr_menu_distance  0.4..2.5 m (1.0): where the panel opens, ahead of the eyes (level)
//   vr_menu_height    -0.6..0.3 m (-0.15): its centre against eye height
//   vr_menu_scale     0.6..2.0 (1.0): one em is 0.9 degrees at 1 m times this
//   The grab writes the three: let go of the panel and they become where it was left (and its size); the next open uses them.
//
// Opening: F8 (keyboard only, the owner's decision). Where it was if that is within 50 degrees of the gaze and 3 m, else in front.
// Home, the headset's recentre and "Bring here" put it in front again. A Bushido targets round (Practice targets or a real one)
// slides it aside until it clears the targets (about 53 degrees at the defaults), or hides it for the round when that is too far.
//
// vr-menu S3 (design section 4): the laser pointer. Native follows each controller's aim ray onto the panel (ffxvr_pointer.h) and
// gives us ImGui's mouse every frame (ffxvr_panel_pointer); vrp_pointer feeds it, after the window, in render_imgui (after
// Fahrenheit's NewFrame: the events apply at the next one). The owner's trigger is the left button, its stick the wheel.
//   vr_menu_hand     0 (default) either hand, 1 the right only, 2 the left only
//   vr_menu_haptics  1 (default) / 0: a tick on each new item under the pointer, a buzz on a click, a faint tick on arriving
// The real cursor: the Win32 backend polls GetCursorPos every NewFrame while the physical mouse has not moved over the window
// (ImGui 1.92.9b, imgui_impl_win32.cpp ImGui_ImplWin32_UpdateMouseData), which would override us. While the pointer feeds ImGui we
// write io.MousePos and set io.WantSetMousePos: the backend then moves the OS cursor to our point (only with the game in front) and
// skips its poll, and ImGui itself drops every queued mouse-position event while that flag is set (imgui.cpp UpdateInputEvents),
// so our direct position stands whatever the backend or WM_MOUSEMOVE queue. The OS cursor follows the beam on the TV.
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
    private Vector4 _vrp_popup_bg;                               // the popup background colour while vrp_popup_push hides ImGui's own
    private readonly float[] _vrp_state = new float[12];
    private readonly float[] _vrp_set = new float[14];
    private delegate* unmanaged[Cdecl]<int, float*, int, void> _fn_panel_set;     // ABI 58
    private delegate* unmanaged[Cdecl]<float*, int, int> _fn_panel_state;         // ABI 58
    private delegate* unmanaged[Cdecl]<int, void> _fn_panel_place;               // ABI 58
    private delegate* unmanaged[Cdecl]<int, float*, int, float*, int, int> _fn_panel_pointer;  // ABI 59: vr-menu S3, the laser pointer; 60: + in (the hand-back)
    private delegate* unmanaged[Cdecl]<int, int> _fn_menu_chord;                   // ABI 62: vr-menu S4, the left grip + the left menu button
    private bool  _vr_menu_chord = true;        // ini vr_menu_chord: the left grip + the left menu button opens / closes this window (headset + controllers)
    private int   _vmc_seen = -1;               // native's toggle count last seen (-1: not read yet)
    private double _vmc_pulse2 = -1;            // the open pattern's second pulse, at this ImGui time (-1: none)
    private int   _vr_menu_hand = 0;            // ini vr_menu_hand: 0 either, 1 right, 2 left
    private bool  _vr_menu_haptics = true;      // ini vr_menu_haptics
    private readonly float[] _vrp_ptr = new float[11];
    private bool  _vrp_ptr_active, _vrp_ptr_btn, _vrp_ptr_logged, _vrp_focused;   // feeding ImGui; the button sent; the method logged; F8 focused
    private int   _vrp_ptr_owner = -1;          // the hand that owns the pointer (for the haptics)
    private uint  _vrp_hover_id;                // the last item that ticked (a new one ticks; review N1)
    private int   _vrp_off_press = -1;          // native's count of trigger presses off the panel, last seen

    private void vrp_bind(Func<string, nint> req) {
        _fn_panel_set = (delegate* unmanaged[Cdecl]<int, float*, int, void>)req("ffxvr_panel_set");
        _fn_panel_state = (delegate* unmanaged[Cdecl]<float*, int, int>)req("ffxvr_panel_state");
        _fn_panel_place = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_panel_place");
        _fn_panel_pointer = (delegate* unmanaged[Cdecl]<int, float*, int, float*, int, int>)req("ffxvr_panel_pointer");
        _fn_menu_chord = (delegate* unmanaged[Cdecl]<int, int>)req("ffxvr_menu_chord");
    }
    private void vrp_clear() { _fn_panel_set = null; _fn_panel_state = null; _fn_panel_place = null; _fn_panel_pointer = null; _vrp_ptr_active = false; _vrp_ptr_btn = false; _fn_menu_chord = null; _vmc_seen = -1; }

    private bool vrp_parse(string k, string v) {
        switch (k) {
            case "vr_menu_panel":    pb(k, v, ref _vr_menu_panel); return true;
            case "vr_menu_distance": pf(k, v, ref _vr_menu_distance, 0.4f, 2.5f); return true;
            case "vr_menu_height":   pf(k, v, ref _vr_menu_height, -0.6f, 0.3f); return true;
            case "vr_menu_scale":    pf(k, v, ref _vr_menu_scale, 0.6f, 2f); return true;
            case "vr_menu_hand":     pi(k, v, ref _vr_menu_hand, 0, 2); return true;
            case "vr_menu_haptics":  pb(k, v, ref _vr_menu_haptics); return true;
            case "vr_menu_chord":    pb(k, v, ref _vr_menu_chord); return true;
            case "vr_menu_hold": case "vr_menu_hold_ms": return true;   // S4's first try (a long press; Virtual Desktop takes it): retired, ignored, no longer written
            default: return false;
        }
    }
    private string vrp_ini_text() =>
        $"vr_menu_panel={(_vr_menu_panel ? 1 : 0)}\n" +
        $"vr_menu_distance={_vr_menu_distance.ToString("F2", CI)}\n" +
        $"vr_menu_height={_vr_menu_height.ToString("F2", CI)}\n" +
        $"vr_menu_scale={_vr_menu_scale.ToString("F2", CI)}\n" +
        $"vr_menu_hand={_vr_menu_hand}\n" +
        $"vr_menu_haptics={(_vr_menu_haptics ? 1 : 0)}\n" +
        $"vr_menu_chord={(_vr_menu_chord ? 1 : 0)}\n";

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
        ImGui.PushStyleVar(ImGuiStyleVar.FramePadding, st.FramePadding * 1.5f);
        ImGui.PushStyleVar(ImGuiStyleVar.ItemSpacing, st.ItemSpacing * 1.25f);
        ImGui.PushStyleVar(ImGuiStyleVar.WindowBorderSize, 0f);   // (the border is a decoration, clipped to the display)
        _vrp_drawing = true;
        try {
            // no title bar, background, border or scrollbar of ImGui's own: they would be clipped to the whole display and stay on the
            // head-locked layer (review S1); F8 closes it, the header says what it is
            if (ImGui.Begin("FFX VR###ffxvr_vr", ref _window_open, ImGuiWindowFlags.NoMove | ImGuiWindowFlags.NoResize | ImGuiWindowFlags.NoCollapse | ImGuiWindowFlags.NoSavedSettings
                            | ImGuiWindowFlags.NoDocking | ImGuiWindowFlags.NoTitleBar | ImGuiWindowFlags.NoBackground | ImGuiWindowFlags.NoScrollbar)) {
                Vector2 p = ImGui.GetWindowPos(), s = ImGui.GetWindowSize();
                vrp_fill(p, s, bg);
                _vrp_x0 = p.X; _vrp_y0 = p.Y; _vrp_x1 = p.X + s.X; _vrp_y1 = p.Y + s.Y; _vrp_em = ImGui.GetFontSize(); _vrp_drawn = true;
                _vrp_wx0 = _vrp_x0; _vrp_wy0 = _vrp_y0; _vrp_wx1 = _vrp_x1; _vrp_wy1 = _vrp_y1;
                draw_window_body();
                _vrp_focused = ImGui.IsWindowFocused(ImGuiFocusedFlags.RootAndChildWindows);   // (popups it opened count: the popup hierarchy)
            }
        }
        finally { ImGui.End(); _vrp_drawing = false; ImGui.PopStyleVar(3); }
    }

    /// <summary>The current window's background, drawn first in its draw list and clipped to its own rectangle (so native routes it to
    /// the panel with the rest of the window).</summary>
    private static void vrp_fill(Vector2 p, Vector2 s, Vector4 bg) {
        ImDrawListPtr dl = ImGui.GetWindowDrawList();
        dl.PushClipRect(p, p + s, false);
        dl.AddRectFilled(p, p + s, ImGui.GetColorU32(bg));
        dl.PopClipRect();
    }

    /// <summary>Before a popup of the headset's window begins (BeginCombo, BeginTooltip): ImGui's own background and border hidden (both
    /// are clipped to the display); vrp_union_popup draws the background inside it. Returns whether it pushed (pass it to the pop).</summary>
    private bool vrp_popup_push() {
        if (!_vrp_drawing) return false;
        _vrp_popup_bg = *ImGui.GetStyleColorVec4(ImGuiCol.PopupBg); _vrp_popup_bg.W = 1f;
        ImGui.PushStyleColor(ImGuiCol.PopupBg, new Vector4(0f, 0f, 0f, 0f));   // alpha 0: ImGui adds no rectangle at all
        ImGui.PushStyleVar(ImGuiStyleVar.PopupBorderSize, 0f);
        return true;
    }
    private static void vrp_popup_pop(bool pushed) { if (pushed) { ImGui.PopStyleVar(1); ImGui.PopStyleColor(1); } }

    /// <summary>A combo in the headset's window: BeginCombo with vrp_popup_push / _pop around it (the list then calls vrp_union_popup).</summary>
    private bool vrp_begin_combo(string label, string preview) {
        bool pushed = vrp_popup_push();
        bool open = ImGui.BeginCombo(label, preview);
        vrp_popup_pop(pushed);
        return open;
    }

    /// <summary>A tooltip: in the headset's window it sits under the item, inside the window's width, wrapped, with its own background,
    /// and joins the panel's rectangle (review N3: with stick navigation every helper's tooltip appears beside the nav item, and the
    /// mouse is not where the panel is); elsewhere ImGui's SetTooltip.</summary>
    private void vrp_tooltip(string s) {
        if (!_vrp_drawing || !_vrp_drawn) { ImGui.SetTooltip(esc(s)); return; }
        float em = ImGui.GetFontSize(), wrap = 24f * em, pad = ImGui.GetStyle().WindowPadding.X;
        Vector2 a = ImGui.GetItemRectMin(), b = ImGui.GetItemRectMax();
        float x = MathF.Max(_vrp_wx0, MathF.Min(a.X, _vrp_wx1 - wrap - 2f * pad));
        ImGui.SetNextWindowPos(new Vector2(x, b.Y + 0.25f * em), ImGuiCond.Always);
        bool pushed = vrp_popup_push();
        bool open = ImGui.BeginTooltip();
        vrp_popup_pop(pushed);
        if (!open) return;
        vrp_union_popup();
        ImGui.PushTextWrapPos(ImGui.GetCursorPosX() + wrap);
        ImGui.TextUnformatted(s);
        ImGui.PopTextWrapPos();
        ImGui.EndTooltip();
    }

    /// <summary>First thing inside a popup the headset's window opened (a combo list, a tooltip): its background (vrp_popup_push hid
    /// ImGui's), and its rectangle joins the panel's, so it shows on the panel too (design 3.6). Not on the frame it appears (review
    /// N2): ImGui hides it then and it still has its last place, possibly a flat-window or F7 combo's.</summary>
    private void vrp_union_popup() {
        if (!_vrp_drawing || !_vrp_drawn || ImGui.IsWindowAppearing()) return;
        Vector2 p = ImGui.GetWindowPos(), s = ImGui.GetWindowSize();
        vrp_fill(p, s, _vrp_popup_bg);
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
        _vrp_drawn_last = _vrp_drawn;   // (vrp_pointer, next: the window's rectangle keeps the park point off it)
        _vrp_drawn = false;
    }

    /// <summary>vr-menu S3: every frame after vrp_push (render_imgui, render thread): native's pointer into ImGui's mouse, the haptic
    /// ticks, and a trigger pulled off the panel giving the game its input back. Native decides the mouse (vptr::Feed): while pointing,
    /// and for 3 frames after (at the park point, review B1), io.MousePos is written with io.WantSetMousePos set, then one "no mouse" is
    /// queued. No position event is queued while feeding (ImGui drops them under the flag anyway; review S1: fewer writes to the input
    /// queue, which Fahrenheit's WndProc also writes from the message-pump thread): only the button, the wheel and that one "no mouse".</summary>
    private void vrp_pointer() {
        if (_fn_panel_pointer == null) return;
        ImGuiIOPtr io = ImGui.GetIO();
        Vector2 mp = io.MousePos, ds = io.DisplaySize;
        _vrp_ptr_in[0] = mp.X; _vrp_ptr_in[1] = mp.Y; _vrp_ptr_in[2] = ds.X; _vrp_ptr_in[3] = ds.Y;
        bool win = _vrp_drawn_last;
        _vrp_ptr_in[4] = win ? _vrp_wx0 : 0f; _vrp_ptr_in[5] = win ? _vrp_wy0 : 0f; _vrp_ptr_in[6] = win ? _vrp_wx1 : 0f; _vrp_ptr_in[7] = win ? _vrp_wy1 : 0f;
        int n; fixed (float* i = _vrp_ptr_in) fixed (float* o = _vrp_ptr) n = _fn_panel_pointer(_vr_menu_hand, i, _vrp_ptr_in.Length, o, _vrp_ptr.Length);
        if (n < 11) return;
        int owner = (int)_vrp_ptr[0];
        bool active = _vrp_ptr[1] != 0f;
        bool btn = active && _vrp_ptr[5] != 0f;
        if (_vrp_ptr[2] != 0f && float.IsFinite(_vrp_ptr[3]) && float.IsFinite(_vrp_ptr[4])) {
            io.MousePos = new Vector2(_vrp_ptr[3], _vrp_ptr[4]); io.WantSetMousePos = true;   // stands this frame; the backend moves the OS cursor and skips its poll
            if (!_vrp_ptr_logged && active) {
                _vrp_ptr_logged = true;
                bool can_set = (io.BackendFlags & ImGuiBackendFlags.HasSetMousePos) != 0;
                d7_log($"menu pointer: ImGui's mouse from the {(owner == 1 ? "right" : "left")} controller via io.MousePos + io.WantSetMousePos, no queued positions "
                     + (can_set ? "(the backend moves the OS cursor and skips its GetCursorPos poll; queued positions are dropped meanwhile)"
                                : "(the backend does not move the OS cursor: queued positions are still dropped while the flag is set)")
                     + "; on leaving: 3 frames at the park point, then one no-mouse event");
            }
        }
        if (btn != _vrp_ptr_btn) { io.AddMouseButtonEvent(0, btn); _vrp_ptr_btn = btn; }   // (up when the pointer leaves: a drag ends cleanly)
        float wheel = _vrp_ptr[6];
        if (active && wheel != 0f && float.IsFinite(wheel)) io.AddMouseWheelEvent(0f, wheel);
        if (_vrp_ptr[7] != 0f) io.AddMousePosEvent(-float.MaxValue, -float.MaxValue);   // the hand-back's end: no mouse (once)
        if (active && !_vrp_ptr_active && _vr_menu_haptics && _fn_haptic != null && owner >= 0) _fn_haptic(owner, 0.08f, 0.006f);   // arriving on the panel
        if (!active) { _vrp_hover_prev = 0; }
        _vrp_ptr_active = active; _vrp_ptr_owner = owner;
        // haptics (review N1): a tick when the item under the pointer changes to a new one and stays for a frame (the gap between two
        // list items, A -> 0 -> A, does not tick again); a buzz on a press over an item
        if (active && _vr_menu_haptics && _fn_haptic != null && owner >= 0) {
            uint id = ImGuiP.GetHoveredID();
            if (id != 0 && id == _vrp_hover_prev && id != _vrp_hover_id) { _fn_haptic(owner, 0.12f, 0.008f); _vrp_hover_id = id; }
            _vrp_hover_prev = id;
            if (_vrp_ptr[5] != 0f && !_vrp_click_prev && id != 0) _fn_haptic(owner, 0.25f, 0.012f);
        }
        _vrp_click_prev = active && _vrp_ptr[5] != 0f;
        // a trigger pulled off the panel while F8 has the focus: the focus goes, so Fahrenheit hands the game its input again
        // (design 4.5: a focused ImGui window holds WantCaptureKeyboard). Not while the controllers drive ImGui's navigation in every
        // mod menu (imgui_vrpad=2, review N4): there the focus is the navigation's.
        int off = (int)_vrp_ptr[8];
        if (_vrp_off_press >= 0 && off != _vrp_off_press && !active && _vrp_focused && _vrp_shown && _imgui_vrpad != 2) ImGui.SetWindowFocus((byte*)null);
        _vrp_off_press = off;
    }
    private bool _vrp_click_prev;

    /// <summary>vr-menu S4: every frame (render_imgui, open or closed): the setting to native, and each chord native counted (the left
    /// grip held, then the left menu button) toggles the window exactly as F8 does (toggle_window), with a buzz on the left hand: two
    /// short pulses opening, one longer closing. Native gates it (the headset on, the controllers the gamepad), keeps the press and the
    /// button from the game and ImGui until it is let go, and the grip from the quick squeeze and the world grab (ffxvr_menuchord.h).
    /// (S4's first try, a long press of the button alone, is gone: Virtual Desktop reads that button on the headset and takes long
    /// presses and double taps for its own menu.)</summary>
    private void vrp_chord() {
        if (_vmc_pulse2 >= 0 && _now >= _vmc_pulse2) { _vmc_pulse2 = -1; if (_fn_haptic != null) _fn_haptic(0, 0.55f, 0.03f); }
        if (_fn_menu_chord == null) return;
        int n = _fn_menu_chord(_vr_menu_chord ? 1 : 0);
        if (_vmc_seen >= 0 && n != _vmc_seen) {
            toggle_window();
            if (_fn_haptic != null) {
                if (_window_open) { _fn_haptic(0, 0.55f, 0.03f); _vmc_pulse2 = _now + 0.09; }   // opened: tap-tap
                else { _fn_haptic(0, 0.4f, 0.08f); _vmc_pulse2 = -1; }                          // closed: one longer
            }
        }
        _vmc_seen = n;
    }
    private uint _vrp_hover_prev;               // the hovered item last frame (a tick needs it held for a frame)
    private readonly float[] _vrp_ptr_in = new float[8];
    private bool _vrp_drawn_last;               // the headset's window was drawn this frame (its rectangle for the park point)

    /// <summary>F8 > Controls > Menu panel (in the headset).</summary>
    private void vrp_ui() {
        ImGui.SeparatorText("Menu panel (in the headset)");
        ui_check("vr_menu_panel", "This menu on its own panel in the room##vrp", ref _vr_menu_panel, UiTag.None,
                 "In the headset this menu is a panel fixed in the room, about 1 m ahead and a little below your eyes. Point at it: the trigger clicks, the stick scrolls. "
               + "Grip it (pointing at it or touching it) to move it, with both grips to resize it. Off: the menu is on the head-locked layer with Fahrenheit's menus, as before.");
        ui_check("vr_menu_chord", "Left grip + left menu button opens or closes this menu##vrp", ref _vr_menu_chord, UiTag.None,
                 "In the headset with the controllers: hold the left grip, then press the left controller's menu button, to open or close this menu, as F8 does. "
               + "That press does not pause the game, and the grip makes no L1 and grabs nothing. The menu button alone is still Start. "
               + "Virtual Desktop keeps long presses and double taps of that button for its own menu. Two short buzzes: opened; one longer: closed.");
        if (!_vr_menu_panel) return;
        ui_slider("vr_menu_distance", "distance (m)##vrp", ref _vr_menu_distance, 0.4f, 2.5f, "%.2f", UiTag.None, "Where it opens: this far ahead of your eyes. Letting go of the panel sets it.", 140f); ImGui.SameLine();
        ui_slider("vr_menu_height", "height (m)##vrp", ref _vr_menu_height, -0.6f, 0.3f, "%.2f", UiTag.None, "Where it opens: its centre against your eye height. Letting go of the panel sets it.", 140f); ImGui.SameLine();
        ui_slider("vr_menu_scale", "size##vrp", ref _vr_menu_scale, 0.6f, 2f, "%.2fx", UiTag.None, "1 = one line of text about 0.9 degrees tall at 1 m. Resizing it with both grips sets it.", 140f);
        if (_fn_panel_place != null && ImGui.SmallButton("Bring it here##vrp")) _fn_panel_place(1);
        ui_tip("The panel in front of you again (Home and the headset's recentre do the same).");
        ImGui.TextUnformatted("pointer:"); ImGui.SameLine();
        const string hand_tip = "Which controller points at this panel. Either: the one pointing at it (the last to arrive, or the one that pulls its trigger on it).";
        ui_radio("vr_menu_hand", "either hand##vrph", ref _vr_menu_hand, 0, UiTag.None, hand_tip); ImGui.SameLine();
        ui_radio("vr_menu_hand", "right##vrph", ref _vr_menu_hand, 1, UiTag.None, hand_tip); ImGui.SameLine();
        ui_radio("vr_menu_hand", "left##vrph", ref _vr_menu_hand, 2, UiTag.None, hand_tip); ImGui.SameLine();
        ui_check("vr_menu_haptics", "ticks##vrp", ref _vr_menu_haptics, UiTag.None, "A light tick on each new item under the pointer, a buzz on a click.");
        if (_vr_menu_panel && _xr_on && _imgui_layer && !_vrp_available) { ImGui.SameLine(); ImGui.TextDisabled("(not available: the panel shaders or swapchain failed, see ffxvr_diag.log)"); }
        if (!_imgui_layer) { ImGui.SameLine(); ImGui.TextDisabled("(needs \"Show mod menus in the headset\", View > Panels)"); }
    }
}
