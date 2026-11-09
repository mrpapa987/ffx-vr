// SPDX-License-Identifier: MIT

namespace FFXVR;

// The F8 menu, reorganised (vr-menu S1; design: research/vr-menu-design.md sections 2 and 6, in the session scratchpad).
//
// A compact header that is always visible (Headset, Recentre, one status line, the version, warnings only when present, and
// on the right the Developer checkbox), then one tab bar: View, Controls, Battle, Overdrives, Movies, Hands, and Developer
// (only in developer mode, which IS diag_verbose: design 2.5, the owner's decision of 8 Nov). The selected tab's content
// scrolls in its own child window, so the header stays in view.
//
// Nothing here sends a setting to the native side: push_all (module.cs) sends every setting every frame whether the window,
// a tab or a section is drawn or not. Hiding a widget never changes behaviour; the key keeps its ini value.
//
// The widget helpers (ui_check, ui_radio, ui_slider, ui_slider_int) take the ini key first. They draw the widget, a tooltip
// that ends with "ini: <key>", and the tags: UiTag.Experimental (a small amber "experimental"), UiTag.NextLaunch (a small amber
// "next launch" while the value differs from the one the hooks were installed with), UiTag.Dev (shown in developer mode
// only). UiTag.Common marks the common settings (design 2.3's C column) for the later simple settings page (design section 8):
// it draws nothing; that page will draw the same helpers with a filter.
public sealed unsafe partial class FFXVRModule {

    private const int k_tab_view = 0, k_tab_controls = 1, k_tab_battle = 2, k_tab_overdrives = 3, k_tab_movies = 4, k_tab_hands = 5, k_tab_dev = 6;
    private static readonly string[] k_tab_key   = { "view", "controls", "battle", "overdrives", "movies", "hands", "developer" };
    private static readonly string[] k_tab_label = { "View", "Controls", "Battle", "Overdrives", "Movies", "Hands", "Developer" };
    private static readonly string[] k_tab_child = { "##tab_view", "##tab_controls", "##tab_battle", "##tab_overdrives", "##tab_movies", "##tab_hands", "##tab_developer" };
    private string _menu_tab = "view";          // ini menu_tab (default view): the selected tab, by name (ImGui does not keep tab state)
    private bool _menu_tab_restore = true;      // select _menu_tab at the next draw (the first one, and after a hand edit of the key)

    /// <summary>The keys only the Developer tab draws, for the header's "not at their defaults" note while developer mode is off
    /// (design 2.4: hidden is not reset). diag_verbose is the toggle itself. vrmenu_chk/check.py keeps this list in step.</summary>
    private static readonly string[] k_dev_keys = {
        "cursor_source", "diag_actors", "diag_build7", "diag_cluster_bytes", "diag_load", "diag_load_lines", "diag_load_prof", "diag_transition",
        "diag_va_log", "eye2_preview", "fast_reads", "fh_dpi_fix", "fmv_depth_edge_band", "fmv_depth_edge_fill", "fmv_depth_median",
        "fmv_depth_tight_fov", "fp_actor", "fp_head_fwd", "fp_head_parts", "fp_head_reach", "fp_head_smooth", "fp_head_up", "hands",
        "hands_head", "hands_mirror", "hands_off_back", "hands_off_right", "hands_off_up", "hands_pitch", "hands_roll", "hands_show",
        "hands_stretch", "hands_swap", "hands_yaw", "hd_cluster_cache", "hide_cursor", "hide_pointers", "keepalive", "limiter_probe",
        "load_lock_release", "noimg_mode", "shadow_budget", "shadow_near", "slow_guard", "slow_guard_restart", "smooth_anchor",
        "stick_overscan", "stick_reproject_margin_ms", "stick_reproject_predict", "timer_fix", "ui_adaptive_margin", "ui_adaptive_min",
        "ui_hide", "vr_hands_debug",
    };

    private LaunchKeys _nl_at;                  // the launch-only keys as the hooks saw them (init, before the hooks are queued)
    private bool _verbose_at_launch;            // diag_verbose at init: the camera trace's fresh file and header follow it (ffxvr_init)
    private List<string> _nl_pending = new();   // NextLaunch.Pending, refreshed with the menu (menu_refresh)
    private Dictionary<string, string>? _ini_defaults;   // ini_text() at init before the file is read: every key's code default
    private int _dev_off_n; private string _dev_off_list = "";
    private string _diag_on_list = "";
    private string _perf_full = "", _perf_short = "";
    private double _menu_next;                  // menu_refresh's next time (_now)
    private int _msaa_targets_seen = -1, _msaa_at_targets;   // msaa's "next scene" tag: the value the game's last scene targets were made with

    private static readonly Vector4 k_amber = new(1f, 0.8f, 0.3f, 1f), k_red = new(1f, 0.35f, 0.35f, 1f), k_tag_amber = new(0.95f, 0.68f, 0.28f, 1f);

    // -- init and the ini -----------------------------------------------------------------------------------------------

    /// <summary>init, before settings_load: every key's code default (for the "not at their defaults" note). A failure leaves
    /// the note off.</summary>
    private void menu_defaults_capture() {
        try { _ini_defaults = ini_kv(ini_text()); }
        catch (Exception e) { _ini_defaults = null; _logger.Info($"menu: the code defaults could not be read ({e.GetType().Name}); the developer-defaults note is off"); }
    }

    /// <summary>init, after settings_load and before the hooks are queued: the values the launch-only hooks follow.</summary>
    private void menu_launch_snapshot() { _nl_at = launch_keys(); _verbose_at_launch = _diag_verbose; _msaa_at_targets = _msaa; }

    private LaunchKeys launch_keys() => new LaunchKeys {
        dmg_hide2d = _dmg_hide2d, odt_hide2d = _od_tgt_hide2d, cluster_bytes = _diag_cluster_bytes, va_log = _diag_va_log,
        load_prof = _diag_load_prof, cursor_game = _cursor_game_cfg, diag7 = _diag7, load_lines = _diag_load_lines,
        lock_release = _load_lock_release, hd_cache = _hd_cluster_cache, diag_load = _diag_load,
    };

    /// <summary>settings_parse: menu_tab=view|controls|battle|overdrives|movies|hands|developer.</summary>
    private void menu_tab_parse(string k, string v) {
        string t = v.Trim().ToLowerInvariant();
        if (Array.IndexOf(k_tab_key, t) >= 0) { _menu_tab = t; _menu_tab_restore = true; }
        else _ini_bad.Add($"{k}='{v}'");
    }

    /// <summary>Four times a second while the window is drawn: the next-launch keys, the developer defaults, the diagnostics
    /// list and the perf line (cheap enough per frame, but none of them changes faster).</summary>
    private void menu_refresh() {
        if (_now < _menu_next) return;
        _menu_next = _now + 0.25;
        _nl_pending = NextLaunch.Pending(_nl_at, launch_keys());
        var on = diag_on(); _diag_on_n = on.Count; _diag_on_list = string.Join(", ", on);
        if (!_diag_verbose && _ini_defaults != null) {
            var now = ini_kv(ini_text()); var off = new List<string>();
            foreach (string k in k_dev_keys)
                if (now.TryGetValue(k, out string? v) && _ini_defaults.TryGetValue(k, out string? d) && v != d) off.Add(k);
            _dev_off_n = off.Count; _dev_off_list = string.Join(", ", off);
        }
        _perf_full = ""; _perf_short = "";
        if (_fn_perf_status != null) {
            byte* pb = stackalloc byte[768]; pb[0] = 0; _fn_perf_status(pb, 768);
            _perf_full = Marshal.PtrToStringAnsi((nint)pb) ?? "";
            _perf_short = perf_brief(_perf_full);
        }
    }

    private static readonly System.Text.RegularExpressions.Regex k_perf_frame = new(@"frame ([0-9.]+) ms");
    private static readonly System.Text.RegularExpressions.Regex k_perf_gpu = new(@"span ([0-9.]+) ms \(max [0-9.]+\) against the ~([0-9.]+) ms knee(: OVER)?");
    private static readonly System.Text.RegularExpressions.Regex k_perf_vram = new(@"vram ([0-9]+) / ([0-9]+) MB");
    /// <summary>ffxvr_perf_status cut to frame / GPU / VRAM (design 2.2); the first line when the format is not recognised.</summary>
    private static string perf_brief(string p) {
        var parts = new List<string>(3);
        var m = k_perf_frame.Match(p); if (m.Success) parts.Add($"frame {m.Groups[1].Value} ms");
        m = k_perf_gpu.Match(p); if (m.Success) parts.Add($"GPU {m.Groups[1].Value} of ~{m.Groups[2].Value} ms knee{(m.Groups[3].Success ? " OVER" : "")}");
        m = k_perf_vram.Match(p); if (m.Success) parts.Add($"vram {m.Groups[1].Value} / {m.Groups[2].Value} MB");
        if (parts.Count > 0) return string.Join(", ", parts);
        int nl = p.IndexOf('\n'); return nl >= 0 ? p[..nl] : p;
    }

    // -- the widget helpers -----------------------------------------------------------------------------------------------

    private bool ui_check(string key, string label, ref bool v, UiTag tags = UiTag.None, string? tip = null) {
        bool ch = ImGui.Checkbox(label, ref v); ui_after(key, tags, tip); return ch;
    }
    private bool ui_radio(string key, string label, ref int v, int value, UiTag tags = UiTag.None, string? tip = null) {
        bool ch = ImGui.RadioButton(label, ref v, value);
        ui_after(key, v == value ? tags : tags & ~UiTag.NextLaunch, tip);   // review N5: one "next launch" per row, on the selected choice
        return ch;
    }
    private bool ui_slider(string key, string label, ref float v, float lo, float hi, string fmt, UiTag tags = UiTag.None, string? tip = null, float width = 0f) {
        if (width > 0f) ImGui.SetNextItemWidth(width);
        bool ch = ImGui.SliderFloat(label, ref v, lo, hi, fmt); ui_after(key, tags, tip); return ch;
    }
    private bool ui_slider_int(string key, string label, ref int v, int lo, int hi, string fmt, UiTag tags = UiTag.None, string? tip = null, float width = 0f) {
        if (width > 0f) ImGui.SetNextItemWidth(width);
        bool ch = ImGui.SliderInt(label, ref v, lo, hi, fmt); ui_after(key, tags, tip); return ch;
    }
    /// <summary>The last item is hovered for a tooltip: build and marshal tooltip text only then (review N1: nothing per frame).</summary>
    private static bool tip_hovered() => ImGui.IsItemHovered(ImGuiHoveredFlags.ForTooltip);
    /// <summary>The last item's tooltip, only while it is hovered (s goes through esc: a '%' is text).</summary>
    private static void ui_tip(string s) { if (tip_hovered()) ImGui.SetTooltip(esc(s)); }
    /// <summary>After the widget: its tooltip (the help, then the ini key; "" = a session-only control) and its tags.</summary>
    private void ui_after(string key, UiTag tags, string? tip) {
        if (tip_hovered()) {
            string k = key.Length > 0 ? "ini: " + key : "not saved (this session only)";
            ImGui.SetTooltip(esc(string.IsNullOrEmpty(tip) ? k : tip + "\n" + k));
        }
        if ((tags & UiTag.Experimental) != 0) ui_tag("experimental", "Experimental: it may change or misbehave. Tell the developer what you see.");
        if ((tags & UiTag.NextLaunch) != 0 && _nl_pending.Contains(key)) ui_tag("next launch", "Saved now; it takes effect at the next launch of the game.");
        if ((tags & UiTag.Dev) != 0) ui_tag("dev", "Shown in developer mode only.", false);
    }
    /// <summary>A small tag on the same line (80 % of our text size), amber or grey, with its own tooltip.</summary>
    private void ui_tag(string text, string tip, bool amber = true) {
        ImGui.SameLine();
        ImGui.PushFont(null, _our_px * 0.8f);
        try { if (amber) ImGui.TextColored(k_tag_amber, text); else ImGui.TextDisabled(text); }
        finally { ImGui.PopFont(); }
        ui_tip(tip);
    }
    /// <summary>Above a tab's experimental items, which come last (design 2.4).</summary>
    private static void ui_exp_separator() { ImGui.Spacing(); ImGui.Separator(); }
    private static void ui_warn(string s, Vector4 c) { ImGui.PushStyleColor(ImGuiCol.Text, c); ImGui.TextWrapped(esc(s)); ImGui.PopStyleColor(1); }

    /// <summary>s cut to w pixels with "..." (the header's one-line status).</summary>
    private static string fit(string s, float w) {
        if (w <= 0f) return "";
        if (ImGui.CalcTextSize(s).X <= w) return s;
        int lo = 0, hi = s.Length;
        while (lo < hi) { int mid = (lo + hi + 1) / 2; if (ImGui.CalcTextSize(s[..mid] + "...").X <= w) lo = mid; else hi = mid - 1; }
        return s[..lo] + "...";
    }

    // -- the window -------------------------------------------------------------------------------------------------------

    /// <summary>draw_window, native ready: the header, the tab bar, then the selected tab in its own scrolling child.</summary>
    private void draw_menu() {
        if (_fn_xr_set == null) { ImGui.TextWrapped($"Native helper: {esc(_native_status)}"); return; }
        menu_refresh();
        draw_header();
        int sel = -1;
        // Mixed (review N4): shrink to the style's minimum tab width, then scroll, so a narrow window keeps the labels readable
        if (ImGui.BeginTabBar("##ffxvr_tabs", ImGuiTabBarFlags.FittingPolicyMixed)) {
            try {
                for (int i = 0; i < k_tab_key.Length; i++) {
                    if (i == k_tab_dev && !_diag_verbose) continue;   // developer mode = diag_verbose: the tab comes and goes the same frame
                    // review S4: the Developer tab is never restored (menu_tab=developer opens View): the native diagnostics are
                    // armed only after a click on the tab this session, as the old Debug header (collapsed at every launch) needed
                    var fl = _menu_tab_restore && i != k_tab_dev && k_tab_key[i] == _menu_tab ? ImGuiTabItemFlags.SetSelected : ImGuiTabItemFlags.None;
                    bool open = ImGui.BeginTabItem(k_tab_label[i], fl);
                    if (i == k_tab_hands) ui_tip("VR hands at the controllers (experimental)");
                    if (open) { sel = i; ImGui.EndTabItem(); }
                }
            }
            finally { ImGui.EndTabBar(); }
        }
        _menu_tab_restore = false;
        if (sel < 0) return;
        _menu_tab = k_tab_key[sel];
        // As opening the old Debug header did: while the Developer tab is the selected one, keep the native diagnostics armed
        // (1 s heartbeat, slot-0 readback); another tab, or the window closed, disarms them. It is never restored at launch (above),
        // so being selected means it was clicked this session.
        if (sel == k_tab_dev && _fn_diag_keepalive != null) _fn_diag_keepalive(2);
        try {
            if (ImGui.BeginChild(k_tab_child[sel], Vector2.Zero, ImGuiChildFlags.NavFlattened)) {
                switch (sel) {
                    case k_tab_view: tab_view(); break;
                    case k_tab_controls: tab_controls(); break;
                    case k_tab_battle: tab_battle(); break;
                    case k_tab_overdrives: tab_overdrives(); break;
                    case k_tab_movies: tab_movies(); break;
                    case k_tab_hands: tab_hands(); break;
                    case k_tab_dev: draw_dev_tab(); break;
                }
            }
        }
        finally { ImGui.EndChild(); }
    }

    private const string k_dev_tip = "Developer mode: shows the Developer tab and turns on the verbose log: periodic lines in ffxvr_diag.log (up to 64 MB a session "
                                   + "instead of 4), the camera trace, Ctrl+F9 dumps. It also turns on the developer's automatic captures: one frame census at the "
                                   + "first battle frame in the headset (a one-frame hitch), up to 3 slow-GPU dumps instead of 1, the overdrive log and the per-movie "
                                   + "FMV lines. Applies now; the camera trace file starts fresh at the next launch. Once raised, the log cap stays at 64 MB until "
                                   + "the next start (unticking does not close the log).";

    /// <summary>Always visible (design 2.2): Headset, Recentre, the status in one line (the full text in its tooltip), Developer
    /// on the right; the perf line and the version; warnings and notes only when present.</summary>
    private void draw_header() {
        var style = ImGui.GetStyle();
        float sp = style.ItemSpacing.X;
        float right = ImGui.GetCursorPosX() + ImGui.GetContentRegionAvail().X;
        float devw = ImGui.GetFrameHeight() + style.ItemInnerSpacing.X + ImGui.CalcTextSize("Developer").X;
        ImGui.Checkbox("Headset", ref _xr_want);   // session only; pushed every frame by push_all, until the native side accepts (it waits for the first 3D frame)
        ui_tip("The headset view, this session (View > Starting: start in the headset)\nnot saved (this session only)");
        ImGui.SameLine(); if (ImGui.Button("Recentre") && _fn_xr_recenter != null) _fn_xr_recenter();
        ui_tip("Home: the view and the big screen in front of you again");
        string st = xr_status_text();
        int cut = st.IndexOf("  yaw ", StringComparison.Ordinal);
        string brief = cut > 0 ? st[..cut] : st;
        if (_fn_xr_screen_now != null && _xr_on) {
            int sn = _fn_xr_screen_now();
            string sh = sn == 1 ? "showing: virtual screen" : sn == 2 ? "showing: stereo screen" : "showing: 3D scene";
            brief += " - " + sh; st += "\n" + sh;
        }
        ImGui.SameLine();
        ImGui.TextDisabled(esc(fit(brief, right - devw - sp - ImGui.GetCursorPosX())));
        if (st.Length > 0) ui_tip(st);
        ImGui.SameLine(Math.Max(right - devw, ImGui.GetItemRectMax().X - ImGui.GetWindowPos().X + sp));
        ImGui.Checkbox("Developer", ref _diag_verbose);   // developer mode = diag_verbose (design 2.5); its tooltip is built only on hover
        if (tip_hovered()) ImGui.SetTooltip(esc(k_dev_tip + (_diag_on_n > 0 ? I($"\ndiagnostics on: {_diag_on_n} ({_diag_on_list})") : "\ndiagnostics: all off") + "\nini: diag_verbose"));

        if (_perf_short.Length > 0) { ImGui.TextDisabled(esc(_perf_short)); ui_tip(_perf_full); ImGui.SameLine(); }
        ImGui.TextDisabled(esc($"FFX VR {s_mod_version}"));
        if (tip_hovered()) ImGui.SetTooltip(esc($"FFX VR {s_mod_version} | native {_native_version} | Fahrenheit {s_fh_version}"));
        if (_diag_on_n > 0) {
            ImGui.SameLine(); ImGui.TextColored(k_tag_amber, I($"log: {_diag_on_n} on"));
            if (tip_hovered()) ImGui.SetTooltip(esc($"diagnostics on: {_diag_on_list}. A player's install has them all off: tools\\set-logging.ps1 -Profile Release"));
        }
        if (_diag_verbose && !_verbose_at_launch) ImGui.TextDisabled("Verbose log on for this session. The camera trace starts fresh at the next launch.");

        // warnings, only when present
        if (_detour_status != "installed") ui_warn($"Draw detours: {_detour_status}. Stereo, cursor hiding and the UI panel may not work; core.log has the details.", k_red);
        if (_ini_note.Length > 0) ui_warn(_ini_note, k_amber);   // R30 / B8-12, for the session
        if (_xr_drop_image)   // a test toggle that blacks out the headset after 30 frames: always visible while it is on
            ui_warn("TEST ON: the eye image is being dropped (Developer > Camera & stereo > Headset tests). The headset holds the last frame, then goes black. Untick it to resume.", k_red);
        if (_fn_lowres_status != null) {   // the game renders below the headset's per-eye size (monitor off at launch, a lower desktop mode)
            byte* lr = stackalloc byte[320];
            if (_fn_lowres_status(lr, 320) != 0) ui_warn(Marshal.PtrToStringAnsi((nint)lr) ?? "", k_amber);
        }
        int over = _perf_full.IndexOf("\nOver the knee:", StringComparison.Ordinal);
        if (over >= 0) { int e = _perf_full.IndexOf('\n', over + 1); ui_warn(e > 0 ? _perf_full[(over + 1)..e] : _perf_full[(over + 1)..], k_amber); }
        if (_nl_pending.Count > 0) {
            ImGui.TextColored(k_amber, I($"{_nl_pending.Count} change{(_nl_pending.Count == 1 ? " applies" : "s apply")} at the next launch"));
            if (tip_hovered()) ImGui.SetTooltip(esc("Saved now, applied when the game next starts (the hooks they need are installed at launch): " + string.Join(", ", _nl_pending)));
        }
        if (!_diag_verbose && _dev_off_n > 0) {
            ImGui.TextDisabled(I($"{_dev_off_n} developer setting{(_dev_off_n == 1 ? " is" : "s are")} not at the default (turn on Developer to see them)"));
            ui_tip("Changed in ffxvr.ini, drawn only in the Developer tab: " + _dev_off_list);
        }
        dev_live_ui();   // review S3: session-only developer states still running with Developer off
    }

    /// <summary>Review S3: the session-only developer states that keep running when Developer is off (F10 / F11 work for every
    /// player): one amber line naming them, with [clear]. Nothing is cleared by itself: a tester may want them kept.</summary>
    private void dev_live_ui() {
        bool eye2 = _stereo_on && !_xr_want, lift = _hands_dev && _wig_on;   // eye 2 is off in flat play by default (and forced on in the headset)
        if (_diag_verbose || !(_slowmo_on || _hover_mode != 0 || _bisect_mode != 0 || _ovr_freeze || eye2 || lift)) return;
        string s = "Developer tools still active:" + (_slowmo_on ? " slow motion (F10)," : "") + (_hover_mode != 0 ? " the UI highlight (F11)," : "")
                 + (_bisect_mode != 0 ? (_bisect_mode == 1 ? " a hidden draw range," : " only a draw range shown,") : "") + (_ovr_freeze ? " the game camera frozen," : "")
                 + (eye2 ? " eye 2 rendered without the headset," : "") + (lift ? " a bone lifted (bone finder)," : "");
        ImGui.TextColored(k_amber, esc(s.TrimEnd(',')));
        ImGui.SameLine(); if (ImGui.SmallButton("clear##devlive")) dev_live_clear();
        ui_tip("Turns them all off (they are session-only and not saved). Developer shows their controls again.");
    }
    private void dev_live_clear() {
        _slowmo_on = false; _hover_mode = 0; _bisect_mode = 0; _ovr_freeze = false;   // push_all / render_imgui_inner send them every frame
        if (!_xr_want) _stereo_on = false;
        if (_wig_on) { _wig_on = false; if (_fn_bone_wiggle != null) _fn_bone_wiggle(_wig_rec, _wig_slot, 0f); }   // the lift is sent only while its section draws
        _logger.Info("menu: the developer states were cleared from the header (slow motion, UI highlight, draw range, frozen camera, eye 2, bone lift)");
    }

    // -- the tabs ---------------------------------------------------------------------------------------------------------

    private const string k_start_tip = "The view the headset starts in, when it starts by itself.";
    private const string k_view_tip = "End, or right grip + right stick click, cycles first person -> third person -> diorama. Left grip + left stick click resets the view.";
    private const string k_backdrop_tip = "Hut interiors are a painted backdrop with 3D props: the whole room on the big screen, or the props in 3D over the flat painting.";
    private const string k_msaa_tip = "Multisampling of the game's scene targets. Applies when the game next creates them (a scene change).";
    private const string k_hist_tip = "The game blends frames in water areas (trails in the headset): off, as the game does it, or the pass skipped.";

    /// <summary>View & comfort (design 2.3, tab "View").</summary>
    private void tab_view() {
        ImGui.SeparatorText("Starting");
        ui_check("headset_autostart", "Start in the headset", ref _headset_autostart, UiTag.Common, "At launch the headset view starts by itself at the first 3D frame.");
        if (_headset_autostart) {
            ImGui.Text("Start in:"); ImGui.SameLine();
            ui_radio("start_view", "seat##sv", ref _start_view, 0, UiTag.Common, k_start_tip); ImGui.SameLine();
            ui_radio("start_view", "first person##sv", ref _start_view, 1, UiTag.Common, k_start_tip); ImGui.SameLine();
            ui_radio("start_view", "third person##sv", ref _start_view, 3, UiTag.Common, k_start_tip); ImGui.SameLine();
            ui_radio("start_view", "diorama##sv", ref _start_view, 2, UiTag.Common, k_start_tip);
            ui_check("overlay_at_start", "Open this menu at start", ref _overlay_at_start, UiTag.None, "Off: no mod window until F8 when the headset starts by itself.");
        }

        ImGui.SeparatorText("View");
        int fp_before = _fp_mode;
        ImGui.Text("View:"); ImGui.SameLine();
        ui_radio("", "seat##vm", ref _fp_mode, 0, UiTag.Common, k_view_tip); ImGui.SameLine();
        ui_radio("", "first person##vm", ref _fp_mode, 1, UiTag.Common, k_view_tip); ImGui.SameLine();
        ui_radio("", "third person##vm", ref _fp_mode, 3, UiTag.Common, k_view_tip); ImGui.SameLine();
        ui_radio("", "diorama##vm", ref _fp_mode, 2, UiTag.Common, k_view_tip + " The diorama follows the player.");
        if (_fp_mode != fp_before) apply_mode_scale(fp_before, _fp_mode);
        ui_slider("world_scale", "World size##ws", ref _xr_scale, 10f, 300f, "%.0f units/m", UiTag.Common, "The seat view's and the diorama's scale (first and third person are always life-size).");
        ImGui.SameLine(); ImGui.TextDisabled(_xr_scale < 20f ? "life-size" : _xr_scale < 60f ? "large model" : "tabletop diorama");
        ui_check("head_position", "Lean with your head", ref _xr_translate, UiTag.None, "The view follows your head's position, not only its turn."); ImGui.SameLine();
        ui_check("seat", "Seat view: look around freely", ref _xr_seat, UiTag.None, "In the seat view your head owns the orientation.");
        ui_check("headset_fov", "Use the headset's full field of view", ref _xr_fov);
        ui_slider("pitch_follow", "Follow the camera's tilt##pf", ref _xr_pitch_follow, 0f, 1f, "%.2f", UiTag.None, "0 keeps the horizon level; 1 follows the game camera's tilt (the original view).");
        float sr = _diag_verbose ? 2000f : 300f, sf = _diag_verbose ? 2000f : 600f;   // developer mode: the old camera override's range
        ui_slider("seat_height", "Seat height##seat", ref _ovr_up, -sr, sr, "%.0f units", UiTag.None, "Raise or lower the seat view (Numpad 7 / 9).");
        ui_slider("seat_forward", "Seat forward##seat", ref _ovr_forward, -sf, sf, "%.0f units", UiTag.None, "Move the seat view forward or back (Numpad + / -).");
        ImGui.SameLine(); if (ImGui.Button("Reset##seat")) { _ovr_yaw = _ovr_pitch = 0; _ovr_right = _ovr_up = _ovr_forward = 0; }
        ui_tip("Seat height and forward back to 0 (the camera override's yaw, pitch and right too; Numpad 5 does the same)");

        ImGui.SeparatorText("Third person & diorama");
        ImGui.Text("Third person:"); ImGui.SameLine();
        ui_slider("tp_distance", "distance##tp", ref _tp_dist, 4f, 120f, "%.0f", UiTag.None, "In third person the grips zoom this distance, not the world.", 120f); ImGui.SameLine();
        ui_slider("tp_height", "height##tp", ref _tp_h, 0f, 60f, "%.0f", UiTag.None, null, 120f); ImGui.SameLine();
        ui_slider("tp_pitch", "pitch##tp", ref _tp_pitch, -60f, 30f, "%.0f deg", UiTag.None, null, 120f); ImGui.SameLine();
        ui_slider("tp_follow_rate", "heading follows the run##tp", ref _tp_follow_rate, 0f, 180f, "%.0f deg/s", UiTag.None, "0 = a fixed heading, like first person (recommended).", 140f);
        ImGui.Text("Diorama:"); ImGui.SameLine();
        ui_slider("follow_distance", "follow distance##dio", ref _fp_follow_dist, 10f, 300f, "%.0f", UiTag.None, null, 160f); ImGui.SameLine();
        ui_slider("follow_height", "follow height##dio", ref _fp_follow_h, 0f, 150f, "%.0f", UiTag.None, null, 160f);
        ui_slider("diorama_floor", "Diorama floor##dio", ref _diorama_floor, 0f, 40f, "%.0f", UiTag.None, "Never below this many units above the player's feet (0 = off; suspended while a grip is held).", 160f); ImGui.SameLine();
        ui_slider("anchor_smooth_ms", "Follow smoothing (ms)##asm", ref _anchor_smooth_ms, 0f, 150f, "%.0f", UiTag.None, "Third person and the diorama follow the player with this lag (0 = snap; 24 to try).", 160f);

        ImGui.SeparatorText("First person");
        ImGui.Text("Your body:"); ImGui.SameLine();
        ui_radio("fp_body_mode", "hide all##fpb", ref _fp_body_mode, 0); ImGui.SameLine();
        ui_radio("fp_body_mode", "show body, hide head##fpb", ref _fp_body_mode, 1); ImGui.SameLine();
        ui_radio("fp_body_mode", "show everything##fpb", ref _fp_body_mode, 2);
        ImGui.Text("Eye height:"); ImGui.SameLine();
        ui_radio("fp_head_anchor", "fixed##fha", ref _fp_head_anchor, 0); ImGui.SameLine();
        ui_radio("fp_head_anchor", "follow the head (swim, crouch)##fha", ref _fp_head_anchor, 1, UiTag.None, "The eye follows the head bone's height."); ImGui.SameLine();
        ui_radio("fp_head_anchor", "full head bone##fha", ref _fp_head_anchor, 2, UiTag.None, "The eye follows the head bone, leaning too.");
        if (_fp_head_anchor == 0) ui_slider("fp_eye_height", "Eye height (units)##feh", ref _fp_eye_h, 0f, 40f, "%.0f", UiTag.None, null, 160f);
        shadow_self_ui();   // your own shadow (shadow_cam.cs)
        fpev_ui();          // a steady view during scripted moves (fp_follow.cs)
        ride_ui();          // riding (ride.cs)

        ImGui.SeparatorText("Comfort");
        ImGui.Text("On a camera cut:"); ImGui.SameLine();
        ui_check("cut_recentre", "face the new view##cut", ref _cut_recentre, UiTag.None, "Turn to face the game camera's new direction at a cut."); ImGui.SameLine();
        ui_check("cut_blink", "blink##cut", ref _cut_blink, UiTag.None, "A short fade at a camera cut.");
        ui_check("follow_pans", "Follow camera pans##pans", ref _follow_pans, UiTag.None, "Turn with the game camera when it pans.");
        if (_follow_pans) { ImGui.SameLine(); ui_slider("follow_rate", "speed (deg/s)##pans", ref _follow_rate, 5f, 360f, "%.0f", UiTag.None, null, 160f); }
        ej_ui();   // blink when the view jumps (eye_jump.cs)

        ImGui.SeparatorText("Panels");
        ui_check("ui_panel", "Game menus and subtitles on a floating panel", ref _ui_on, UiTag.None, "The game's menus, subtitles and HUD on their own panel in front of you.");
        ui_slider("ui_distance", "Panel distance (m)##uip", ref _ui_dist, 0.5f, 4f, "%.2f", UiTag.Common, null, 160f); ImGui.SameLine();
        ui_slider("ui_width", "size (m)##uip", ref _ui_width, 0.5f, 4f, "%.2f", UiTag.Common, "The panel's width.", 160f);
        if (_ui_on) uidepth_ui();   // in front of nearby objects, the dark backing (uidepth.cs)
        dream_ui();                 // the flashback filter (dream_overlay.cs)
        ImGui.Text("Painted rooms (huts):"); ImGui.SameLine();
        ui_radio("backdrop_mode", "on the big screen##bd", ref _backdrop_mode, 1, UiTag.None, k_backdrop_tip); ImGui.SameLine();
        ui_radio("backdrop_mode", "3D props over the painting##bd", ref _backdrop_mode, 0, UiTag.None, k_backdrop_tip);
        ui_check("imgui_layer", "Show mod menus in the headset (this menu, Fahrenheit's menus)", ref _imgui_layer);
        if (_imgui_layer) fh_menu_ui();   // the save screen on the big screen (module.cs)
        // FH-2: overlay_text_scale, driven in whole 0.05 steps (10..40): one keyboard / gamepad press moves one step (a float
        // slider's 1 % nav step, 0.015, would round back to the same value), and a drag bakes a few font sizes, not hundreds.
        // The format has no '%': ImGui shows it as it is (a Ctrl+click entry takes the step count, clamped below).
        int ots = (int)MathF.Round(text_scale_step(_overlay_text_scale) * 20f);
        if (ui_slider_int("overlay_text_scale", "Menu text size##ots", ref ots, 10, 40, I($"{ots / 20f:F2}"), UiTag.Common,
                          "This menu, Eye 2 and the labels (the size now: Developer > Logging & status).", 160f)) _overlay_text_scale = text_scale_step(ots / 20f);
        ImGui.SameLine(); if (ImGui.SmallButton("default##ots")) _overlay_text_scale = 1f;

        if (ImGui.CollapsingHeader("Graphics & performance")) {
            shadow_ui();   // character shadows (shadow_cam.cs)
            ui_check("cull_dist", "Never hide distant characters (no pop-in)", ref _cull_dist, UiTag.None, "No distance or frustum cull of characters (a code patch; unticking restores it).");
            if (_fn_cull_dist_get != null && _fn_cull_dist_get() == 0 && _cull_dist_rc != 0) { ImGui.SameLine(); ImGui.TextColored(k_amber, I($"not applied (rc {_cull_dist_rc}, see ffxvr_diag.log)")); }
            ui_check("cull_scene", "Draw the whole scene (fixes edge pop-in; costs GPU)", ref _cull_scene, UiTag.None, "No frustum cull of the scene: what the game camera does not see stays drawn when you look around.");
            if (_fn_cull_scene_get != null && _fn_cull_scene_get() == 0 && _cull_scene_rc != 0) { ImGui.SameLine(); ImGui.TextColored(k_amber, I($"not applied (rc {_cull_scene_rc}, see ffxvr_diag.log)")); }
            ImGui.Text("Anti-aliasing:"); ImGui.SameLine();
            ui_radio("msaa", "game (8x)##msaa", ref _msaa, 0, UiTag.None, k_msaa_tip); ImGui.SameLine();
            ui_radio("msaa", "4x##msaa", ref _msaa, 4, UiTag.None, k_msaa_tip); ImGui.SameLine();
            ui_radio("msaa", "2x##msaa", ref _msaa, 2, UiTag.None, k_msaa_tip);
            if (msaa_next_scene()) ui_tag("next scene", "Saved now; it takes effect when the game next creates its scene targets (a scene change).");
            ImGui.Text("Water trails (frame blending):"); ImGui.SameLine();
            ui_radio("hist_mode", "off##hist", ref _hist_mode, 1, UiTag.None, k_hist_tip); ImGui.SameLine();
            ui_radio("hist_mode", "game##hist", ref _hist_mode, 0, UiTag.None, k_hist_tip); ImGui.SameLine();
            ui_radio("hist_mode", "skip pass##hist", ref _hist_mode, 2, UiTag.None, k_hist_tip);
            haze_ui();   // heat shimmer, the experimental per-eye choice last (haze.cs)
        }
    }

    /// <summary>msaa's "next scene" tag: the value differs from the one the game's last scene targets were made with (counted
    /// by the native targets counter; at launch, the launch value).</summary>
    private bool msaa_next_scene() {
        if (_fn_msaa_status == null) return false;
        uint seen, changed, lw, lh, ln; _fn_msaa_status(&seen, &changed, &lw, &lh, &ln);
        if ((int)seen != _msaa_targets_seen) { if (_msaa_targets_seen >= 0) _msaa_at_targets = _msaa; _msaa_targets_seen = (int)seen; }
        return _msaa != _msaa_at_targets;
    }

    private const string k_pad_map = "A / B / X / Y, sticks, triggers = LT / RT, grips = LB / RB (a quick squeeze in battle and the seat / diorama), left menu = Start, left stick click = Select.";
    private const string k_help_text =
        "Keyboard: Home recentres; End cycles first person -> third person -> diorama; F8 shows or hides this menu (F7 is Fahrenheit's own); "
      + "Numpad 1 / 3 turn left / right; Insert: the next party member's eyes in battle.\n"
      + "Controllers: " + k_pad_map + "\n"
      + "Chords (never reach the game): right grip + right stick click = the next view; left grip + right stick click = the next party member's view "
      + "(battle); left grip + left stick click = reset the view; both stick clicks = Virtual Desktop's overlay.";

    /// <summary>Controls (design 2.3, tab "Controls"). The S2/S3 "Menu panel" section comes with the VR panel.</summary>
    private void tab_controls() {
        ImGui.SeparatorText("Controllers");
        ui_check("vrpad", "Use the Quest controllers as the gamepad", ref _vrpad, UiTag.Common, k_pad_map);
        ui_check("haptics", "Controller rumble", ref _haptics, UiTag.Common, "The game's vibration on the controllers (the in-game Vibration option still decides)."); ImGui.SameLine();
        ui_slider("haptics_gain", "strength##hap", ref _haptics_gain, 0f, 1f, "%.2f", UiTag.Common, null, 160f);
        if (_fn_haptic != null) {
            ImGui.SameLine(); if (ImGui.SmallButton("test left")) _fn_haptic(0, 0.5f, 0.1f);
            ImGui.SameLine(); if (ImGui.SmallButton("test right")) _fn_haptic(1, 0.5f, 0.1f);
        }
        qc_ui();   // the quick squeeze, the Virtual Desktop chord, the save screen (quest_ctl.cs)

        ImGui.SeparatorText("Turning");
        ui_check("turn_stick", "Turn with the right stick", ref _turn_stick, UiTag.None, "Numpad 1 / 3 turn too."); ImGui.SameLine();
        ui_slider("turn_rate", "speed##turn", ref _turn_rate, 30f, 240f, "%.0f deg/s", UiTag.Common, null, 160f);
        sr_ui();   // extra-smooth stick turning (stickrep.cs)

        ImGui.SeparatorText("Moving");
        ui_check("move_remap", "Walk where you look (first person)", ref _move_remap, UiTag.Common, "The left stick walks relative to where you look."); ImGui.SameLine();
        ui_check("move_invert", "invert##move", ref _move_invert, UiTag.Common);
        qc_walk_ui();   // ... only while walking (quest_ctl.cs)

        ImGui.SeparatorText("Grab the world");
        ui_check("gestures", "Grab the world with the grips (seat and diorama)", ref _gestures, UiTag.None, "One hand pans; two hands pan, turn, tilt and zoom."); ImGui.SameLine();
        ui_slider("gest_tilt", "wrist-roll tilt##gest", ref _gest_tilt, -2f, 2f, "%.1f", UiTag.None, "The tilt's gain (wrist roll).", 120f); ImGui.SameLine();
        if (ImGui.SmallButton("reset grab")) { if (_fn_gesture_reset != null) _fn_gesture_reset(); }
        ui_tip("The grab's move, turn, tilt and zoom back to none (Numpad 5 too)");

        ImGui.SeparatorText("Help");
        ImGui.TextDisabled("Home: recentre.  End: next view.  F8: this menu.  (?)");
        ui_tip(k_help_text);
    }

    private const string k_bv_tip = "The view a battle starts in; the view from before returns when the battle ends.";

    /// <summary>Battle (design 2.3, tab "Battle"): the 3D damage numbers (experimental) last.</summary>
    private void tab_battle() {
        ImGui.SeparatorText("Battle view");
        ImGui.Text("Battle view:"); ImGui.SameLine();
        ui_radio("battle_view", "as now##bv", ref _battle_view, -1, UiTag.Common, k_bv_tip); ImGui.SameLine();
        ui_radio("battle_view", "first person##bv", ref _battle_view, 1, UiTag.Common, k_bv_tip); ImGui.SameLine();
        ui_radio("battle_view", "third person##bv", ref _battle_view, 3, UiTag.Common, k_bv_tip); ImGui.SameLine();
        ui_radio("battle_view", "diorama##bv", ref _battle_view, 2, UiTag.Common, k_bv_tip);
        ui_check("battle_anchor", "Battle diorama: centre on the fight", ref _battle_anchor, UiTag.None, "In battle the diorama centres on the fight, not on the player.");
        ui_check("battle_follow", "First person in battle: ride along with the active character", ref _battle_follow, UiTag.None,
                 "First person follows the field: Tidus when he is on it, swaps move the eyes, a summon gives them to Yuna. Tidus joining the field takes the eyes; "
               + "left grip + right stick click (or Insert) moves them to the next party member.");
        btg_ui();   // when characters swap (bt_tag.cs)
        ui_slider("battle_anchor_smooth_ms", "Battle follow smoothing (ms)##basm", ref _battle_anchor_smooth_ms, 0f, 1000f, "%.0f", UiTag.None, "The battle diorama follows its centre with this lag (0 = snap; 325 to try).", 160f);
        ej_face_ui();   // face the enemies (eye_jump.cs)

        ImGui.SeparatorText("Battle start");
        bt_ui();   // the battle start effect (battle_transition.cs)

        ImGui.SeparatorText("Target cursor");
        ui_check("cursor_look", "Use the game's cursor art", ref _cursor_look_game, UiTag.None, "The hand from battle.dds and the game's spinning triangle (needs the game cursor source).");
        if (_cursor_look_game) {
            if (!_c3_atlas_ok) { ImGui.SameLine(); ImGui.TextDisabled("battle.dds not found yet (found at the first target menu of a battle; red triangles until then)"); }
            ui_slider("cursor_hand_size", "Hand size##cur", ref _cursor_hand_size, 0.3f, 3f, "%.2f", UiTag.None, null, 160f);
        }
        ui_slider("marker_scale", "Cursor size##cur", ref _marker_scale, 0.3f, 3f, "%.2f", UiTag.None, null, 160f);
        if (_c2_active) { ImGui.SameLine(); ui_slider("marker_lift", "lift##cur", ref _marker_lift, -5f, 20f, "%.1f", UiTag.None, "Above the game's own anchor.", 160f); }
        ImGui.Text("Your own marker in first person:"); ImGui.SameLine();
        ui_radio("fp_self_marker", "low##fps", ref _fp_self_marker, 0, UiTag.None, "No turn marker; a self-target small at chest height."); ImGui.SameLine();
        ui_radio("fp_self_marker", "hidden##fps", ref _fp_self_marker, 1); ImGui.SameLine();
        ui_radio("fp_self_marker", "normal##fps", ref _fp_self_marker, 2);

        ImGui.SeparatorText("Blitzball");
        ui_check("blitz_overlay", "Scoreboard and radar on the panel", ref _blitz_overlay, UiTag.None, "They are drawn into the scene without a camera (double vision); this puts them on the UI panel.");

        ui_exp_separator();
        dmg3d_ui();   // 3D damage numbers (dmg3d.cs)
    }

    /// <summary>Overdrives (design 2.3, tab "Overdrives"): od_gest.cs draws it.</summary>
    private void tab_overdrives() {
        odg_ui();
    }

    /// <summary>Movies (design 2.3, tab "Movies"): FMVs in 3D, the big screen, the 3D cinema screen.</summary>
    private void tab_movies() {
        ImGui.SeparatorText("3D movies");
        fmv3d_ui();   // fmv_relief.cs

        ImGui.SeparatorText("Big screen (2D scenes, menus, movies)");
        ImGui.Text("Big screen for 2D scenes:"); ImGui.SameLine();
        ui_radio("screen_mode", "auto##scr", ref _xr_screen_mode, 0); ImGui.SameLine();
        ui_radio("screen_mode", "always##scr", ref _xr_screen_mode, 1); ImGui.SameLine();
        ui_radio("screen_mode", "never##scr", ref _xr_screen_mode, 2);
        ui_slider("screen_width", "Screen size (m)##scr", ref _xr_screen_width, 1f, 8f, "%.1f", UiTag.Common, "The big screen's width; applies at once.", 160f); ImGui.SameLine();
        ui_slider("screen_distance", "distance (m)##scr", ref _xr_screen_dist, 1f, 6f, "%.1f", UiTag.Common, "Applies at once; Home re-places the screen in front of you.", 160f);

        ImGui.SeparatorText("3D cinema screen");
        ImGui.Text("3D cinema screen (the game's camera, in 3D):"); ImGui.SameLine();
        ui_check("stereo_screen_blitz", "in blitzball##s3", ref _s3_blitz); ImGui.SameLine();
        ui_check("stereo_screen_all", "everywhere##s3", ref _s3_all, UiTag.None, "3D scenes on the big screen, through the game's own camera, in 3D.");
        if (_s3_blitz || _s3_all) {
            ui_slider("stereo_screen_depth", "Depth##s3", ref _s3_depth, 0f, 3f, "%.2f", UiTag.None, "1 = standard; 0 = flat.");
            ui_check("stereo_screen_auto", "Auto depth##s3", ref _s3_auto, UiTag.None, "The screen plane follows the subject, and nothing pops out further than the given part of the screen."); ImGui.SameLine();
            ui_slider("stereo_screen_max_pop", "max pop-out (% of the screen)##s3pop", ref _s3_max_pop, 0f, 6f, "%.1f", UiTag.None, "0 = nothing in front of the screen.", 120f);
            if (!_s3_auto) ui_slider("stereo_screen_plane", "Screen plane##s3", ref _s3_plane, 50f, 2000f, "%.0f", UiTag.None, "Game units: nearer pops out, farther goes in.");
        }
    }

    /// <summary>Hands (design 2.3, tab "Hands"): today's "VR hands (experimental)" header, hands.cs.</summary>
    private void tab_hands() {
        ImGui.TextColored(k_tag_amber, "experimental");
        ui_tip("VR hands are experimental: they may change or misbehave. Tell the developer what you see.");
        hands2_ui();
    }

    // -- Developer > Logging & status: the readouts that sat beside the player settings -----------------------------------

    /// <summary>Design 2.4: a player tab shows status only when it is actionable; the live readouts are here.</summary>
    private void dev_status_ui() {
        if (ImGui.TreeNode("Headset and controllers")) {
            ImGui.TextWrapped(esc(xr_status_text()));   // the OpenXR runtime's name is in it
            if (_fn_xr_screen_now != null && _xr_on) { int sn = _fn_xr_screen_now(); ImGui.Text(sn == 1 ? "showing: virtual screen" : sn == 2 ? "showing: stereo screen" : "showing: 3D scene"); }
            if (_fn_vrpad_status != null) {
                float* st = stackalloc float[24]; byte* pl = stackalloc byte[64]; byte* pr = stackalloc byte[64]; uint frames;
                int ready = _fn_vrpad_status(st, pl, pr, 64, &frames);
                if (ready == 0) ImGui.TextDisabled(_xr_on ? "controllers: no input this frame (headset not focused, or no bindings)" : "controllers: headset off");
                else {
                    ImGui.Text($"controllers: syncing ({frames} frames)   left: {esc(Marshal.PtrToStringAnsi((nint)pl))}   right: {esc(Marshal.PtrToStringAnsi((nint)pr))}");
                    for (int h = 0; h < 2; h++) {
                        float* o = st + h * 12;
                        ImGui.Text($"  {(h == 0 ? "L" : "R")}: {(o[0] > 0 ? "on " : "off")} stick ({o[1],5:F2},{o[2],5:F2}){(o[3] > 0 ? "*" : " ")}  {(h == 0 ? "X" : "A")}{(o[4] > 0 ? "#" : "-")} {(h == 0 ? "Y" : "B")}{(o[5] > 0 ? "#" : "-")}  trig {o[6]:F2} grip {o[7]:F2}  {(h == 0 ? (o[8] > 0 ? "menu#" : "menu-") : "     ")}  pose {(o[9] > 0 ? "grip" : "----")}/{(o[10] > 0 ? "aim" : "---")}");
                    }
                }
            }
            {
                int hs = _haptics_status, src = (hs >> 16) & 0xF;
                ImGui.TextDisabled($"rumble: {(((hs >> 20) & 1) != 0 ? "hooked" : "XInputSetState not hooked")}, source {(src == 1 ? "the game's XInputSetState" : src == 2 ? "the game's pad block" : src == 3 ? "in-game Vibration off" : "none (headset off or not focused, haptics off, or a pad in use with the VR pad off)")}, motors left {hs & 0xFF} right {(hs >> 8) & 0xFF} (of 254)");
            }
            ImGui.TextDisabled($"grab: {(_gest_active != 0 ? "GRABBING   " : "")}world scale now {_xr_scale * _gest_scale_mul:F0} units/m (base {_xr_scale:F0} x {_gest_scale_mul:F2}), grab offset ({_gest_move_x:F0}, {_gest_move_y:F0}, {_gest_move_z:F0})");
            if (_fn_move_status != null) {
                float yv, yc, dd; uint calls; int hooked;
                _fn_move_status(&yv, &yc, &dd, &calls, &hooked);
                ImGui.TextDisabled(hooked != 0 ? $"pad reads: {calls}  view {yv:F0}° camera {yc:F0}° stick turned {dd:F0}°" : "pad hook not installed");
            }
            ImGui.TextDisabled("walk gate: " + esc(qc_text(0)));
            ImGui.TextDisabled("grips: " + esc(qc_text(1)));
            ImGui.TextDisabled(esc(imgui_pad_status()));
            ImGui.TreePop();
        }
        if (ImGui.TreeNode("View, comfort and panels")) {
            if (_fn_cut_count != null) ImGui.TextDisabled($"cuts seen: {_fn_cut_count()}");
            if (_fn_eye_jump_set != null) {
                ImGui.TextDisabled($"eye jumps: seen {_ej_stats[0]}, blinked {_ej_stats[1]} (extended {_ej_stats[2]}, held back {_ej_stats[3]}){(_cut_blink ? $"; cut blinks held back {_ej_stats[6]}" : "")}");
                ImGui.TextDisabled($"battle face: turned {_ej_stats[4]}, kept {_ej_stats[5]} (under 15 deg)");
            }
            if (_fn_fp_event != null) ImGui.TextDisabled(I($"event hold: {(_fpev_st[0] == 1f ? $"holding {_fpev_st[1]:F1} s, the eye {_fpev_st[2]:F1} units off" : _fpev_st[0] == 2f ? "gliding back" : "not holding")}; holds {_fpev_st[3]:F0}"));
            if (_xr_want) { string s = sr_status_text(out _); if (s.Length > 0) ImGui.TextDisabled("stick turning: " + esc(s)); }
            if (_fn_imgui_draws != null) ImGui.TextDisabled($"mod menu layer draws {_fn_imgui_draws()} (shown when this overlay or the save screen is open, or at {_imgui_min_draws}+ draws)");
            if (_fn_imgui_status != null) {
                int* v = stackalloc int[6];
                if (_fn_imgui_status(v, 6) >= 6) {
                    string where = v[1] == 2 ? "on the virtual screen" : v[1] == 1 ? "head-locked" : "not shown";
                    string head = v[0] != 0 ? (v[2] != 0 ? "save screen open: on the screen, placed where you faced; " : "save screen open: on the screen; ") : "";
                    string size = v[4] > 0 ? $", {v[4]}x{v[5]}" : "";
                    string refused = v[3] > 0 ? $", {v[3]} draws refused (no blend state)" : "";
                    ImGui.TextDisabled(esc($"{head}layer {where}{size}{refused}"));
                }
            }
            if (_xr_want && _fn_uidepth_status != null) ImGui.TextDisabled("UI panel: " + esc(uidepth_live_text()));
            if (_fn_dream_status != null) ImGui.TextDisabled("flashback filter: " + esc(dream_status_text()));
            if (_fn_backdrop_draws != null) ImGui.TextDisabled($"backdrop draws seen: {_fn_backdrop_draws()}");
            if (_xr_want && _fn_haze_status != null) ImGui.TextDisabled("heat haze: " + esc(haze_live_text()));
            if ((_s3_blitz || _s3_all) && _s3_auto && _fn_screen3d_status != null) {
                int vis; float nd, sd, pl, sp; _fn_screen3d_status(&vis, &nd, &sd, &pl, &sp);
                ImGui.TextDisabled($"3D cinema screen: {vis} characters in view, nearest {nd:F0}, subject {sd:F0}; screen plane {pl:F0}, eye separation {sp:F1} (background limited by the {_xr_screen_width:F1} m screen)");
            }
            ImGui.TreePop();
        }
        if (ImGui.TreeNode("Battle, overdrives and rendering")) {
            ImGui.TextDisabled(_battle_anchor_n > 0 ? $"battle diorama: combatants {_battle_anchor_n}" : "battle diorama: not in battle");
            if (_btl_who.Length > 0) ImGui.TextDisabled($"battle first person now: {esc(_btl_who)}");   // an actor name read from the game
            ImGui.TextDisabled(_in_blitz ? $"blitzball overlay: routed last frame {_blitz_overlay_n}" : "blitzball overlay: not in blitzball");
            if (_fn_bt_status != null) ImGui.TextDisabled("battle start: " + esc(bt_status_text()));
            if (_dmg3d) {
                if (_fn_dmg_status != null) fixed (uint* u = _dmg_st) _fn_dmg_status(u, 18);
                ImGui.TextDisabled(I($"3D damage numbers: battle.dds {(_dmg_atlas ? "held" : "not found yet")}; digits now {_dmg_st[5]}, numbers seen {_dmg_st[11]}; targets without the game's anchor {_dmg_chr_ff + _dmg_chr_none}; queue check matched {_dmg_q_ok} / mismatch {_dmg_q_mismatch}{(_dmg_hide_at_launch ? I($"; hide: {esc(_dmg_hook_status)}, calls {_dmg_draw_calls}, digits taken off {_dmg_hidden_digits}") : "")}"));
            }
            ImGui.TextDisabled("overdrives: " + esc(odg_text(0)));
            ImGui.TextDisabled("overdrives, last: " + esc(odg_text(1)));
            ImGui.TextDisabled(esc(odt_row_text()));
            if (_fn_cull_dist_get != null) ImGui.TextDisabled("characters cull patch: " + (_fn_cull_dist_get() != 0 ? "patched" : (_cull_dist_rc != 0 ? I($"not applied (rc {_cull_dist_rc}, see diag log)") : "off")));
            if (_fn_cull_scene_get != null) ImGui.TextDisabled("scene cull patch: " + (_fn_cull_scene_get() != 0 ? "patched" : (_cull_scene_rc != 0 ? I($"not applied (rc {_cull_scene_rc}, see diag log)") : "off")));
            if (_fn_msaa_status != null) {
                uint seen, changed, lw, lh, ln; _fn_msaa_status(&seen, &changed, &lw, &lh, &ln);
                ImGui.TextDisabled(seen == 0 ? "MSAA: takes effect when the game next creates its scene targets" : $"MSAA: targets created {seen} (overridden {changed}), last {lw}x{lh} @ {ln} samples");
            }
            if (_fn_hist_draws != null) ImGui.TextDisabled($"frame blending draws seen: {_fn_hist_draws()}");
            ImGui.TreePop();
        }
    }
}

/// <summary>vr-menu S1: the UI tags (design 2.4). Common = the later simple settings page's controls (design section 8).</summary>
[Flags]
internal enum UiTag { None = 0, Common = 1, Experimental = 2, NextLaunch = 4, Dev = 8 }

/// <summary>vr-menu S1: the launch-only keys, as the values the hooks follow (taken at init) or as they are now. Kept free of
/// game types for the scratch test.</summary>
internal struct LaunchKeys {
    public bool dmg_hide2d, odt_hide2d, cluster_bytes, va_log, cursor_game, diag7, load_lines, hd_cache, diag_load;
    public int load_prof, lock_release;
}

/// <summary>vr-menu S1 (design 2.4, "next launch"): which keys differ from what the launch installed, so that a change waits for
/// the next launch. dmg3d_hide2d turned on without its hook (dmg3d_init; once hooked it is live both ways), od_targets_hide2d either way (its patch and hook are made only at init, odt_row_init), diag_cluster_bytes / diag_va_log
/// (ld_init), diag_load_prof's two probes (on / off), cursor_source=game without the cursor hooks (diag7_init: queued with
/// cursor_source=game or diag_build7 at launch; going back to legacy is live), and the load hooks (ld_init: queued only when
/// one of diag_load_lines, load_lock_release, hd_cluster_cache, diag_load was on at launch). smooth_anchor is not here: the job
/// join it needs is always hooked now (diag7_init, chocobo re-review MF-1). Pure: (launch, now) -> keys.</summary>
internal static class NextLaunch {
    public static bool LoadHooks(in LaunchKeys k) => k.load_lines || k.lock_release != 0 || k.hd_cache || k.diag_load;
    public static List<string> Pending(in LaunchKeys at, in LaunchKeys now) {
        var p = new List<string>();
        // review S1: only on without the hook; hooked at launch, the switch is live both ways
        if (now.dmg_hide2d && !at.dmg_hide2d) p.Add("dmg3d_hide2d");
        if (now.odt_hide2d != at.odt_hide2d) p.Add("od_targets_hide2d");
        if (now.cluster_bytes != at.cluster_bytes) p.Add("diag_cluster_bytes");
        if (now.va_log != at.va_log) p.Add("diag_va_log");
        if ((now.load_prof > 0) != (at.load_prof > 0)) p.Add("diag_load_prof");
        if (now.cursor_game && !(at.cursor_game || at.diag7)) p.Add("cursor_source");
        if (!LoadHooks(at)) {
            if (now.load_lines) p.Add("diag_load_lines");
            if (now.lock_release != 0) p.Add("load_lock_release");
            if (now.hd_cache) p.Add("hd_cluster_cache");
            if (now.diag_load) p.Add("diag_load");
        }
        return p;
    }
}
