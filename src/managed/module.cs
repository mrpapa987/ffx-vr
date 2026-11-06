// SPDX-License-Identifier: MIT

namespace FFXVR;

/// <summary>
///     Scaffold module for the FFX VR mod.
/// </summary>
/// <remarks>
///     Proves our code runs in-process and can draw through Fahrenheit's ImGui
///     overlay - which lives inside the hooked <c>IDXGISwapChain::Present</c>,
///     where the VR renderer will eventually go - and bootstraps the native
///     helper that captures D3D11 constant buffer uploads.
/// </remarks>
[FhLoad(FhGameId.FFX)]
public sealed unsafe partial class FFXVRModule : FhModule {   // partial: the build-7 diagnostics live in diag7.cs

    private const string NativeDll = "ffxvr_native.dll";

    private ulong _frames;
    private bool  _window_open = true;

    private nint   _native_handle;
    private string _native_status = "not loaded";
    private bool   _native_init_done;
    private const int k_abi = 48;   // must equal FFXVR_ABI in ffxvr_native.cpp (bumped with any export change; 37: ffxvr_fp_self_set, ffxvr_fp_others_set; 38: ffxvr_cpu_report, ffxvr_hands2_set's and ffxvr_fp_self_set's returns, tap game bits 0x200 / 0x400; 39: ffxvr_fp_others_set(xyz, heads, n), npc-anchor; 40: ffxvr_fp_ib_set, npc-anchor round 2; 41: ffxvr_fmv3d_set, ffxvr_fmv3d_text, fmv-relief S3; 42: ffxvr_fmv3d_set(on, amount, flags, offset, range, pop, smooth), the "3D movie" mapping; 43: + edge, the window rule; 44: ffxvr_eye_jump_set, ffxvr_eye_event, eye jumps; 45: ffxvr_move_gate_set, ffxvr_grip_set, ffxvr_imgui_pad_find / _set, ffxvr_imgui_xstate / _xcaps, ffxvr_ctl_text, Quest-only controls, merged on top of 44; 46: ffxvr_bt_tag_step, ffxvr_bt_tag_hold, battle tag; 47: ffxvr_dmg_walk / _queue / _set / _cfg / _skip / _status, 3D damage numbers; 48: ffxvr_odg_set, ffxvr_odg_text, overdrive gestures, merged on top of 47)
    private bool   _native_ok;      // every export bound and the ABI matches: begin_frame may be called. Stays true after an init
                                    // failure: some vtable patches may be live, and begin_frame's heartbeat (vtable audit) is the diagnostic
    private bool   _native_ready;   // ffxvr_init returned 0: the hooks are in, the full UI and the per-frame pushes run
    private bool   _native_failed;  // a hard failure (missing DLL, mismatch, init failed): the window shows why
    private bool   _native_mismatch; // of those, the ones a reinstall fixes: the DLL missing, failing to load, or not matching
    private string _ui_fault_last = "";   // draw_window's last exception message (logged once per distinct message)
    private bool   _window_open_forced, _window_open_pre_force;   // a failure opened the window; the ini keeps the previous overlay_open until F8
    private ulong  _push_frame = ulong.MaxValue;                  // push_all runs once per frame
    private bool   _xr_want_prev, _ovr_before_xr, _stereo_before_xr;   // what the headset forced on, restored when it goes off

    private nint _game_device;
    private nint _game_context;

    private delegate* unmanaged[Cdecl]<nint, nint, int> _fn_init;
    private delegate* unmanaged[Cdecl]<int>       _fn_buffer_count;
    private delegate* unmanaged[Cdecl]<int>       _fn_dump_count;
    private delegate* unmanaged[Cdecl]<int>       _fn_hook_calls;
    private delegate* unmanaged[Cdecl]<void>      _fn_begin_frame;
    private delegate* unmanaged[Cdecl]<int>       _fn_frame_writes;
    private delegate* unmanaged[Cdecl]<float*, int> _fn_get_camera;
    private delegate* unmanaged[Cdecl]<int>       _fn_camera_reads;
    private delegate* unmanaged[Cdecl]<int>       _fn_slot0_count;
    private delegate* unmanaged[Cdecl]<int>       _fn_camera_changes;
    private delegate* unmanaged[Cdecl]<int, int>  _fn_counter;
    private delegate* unmanaged[Cdecl]<int, int> _fn_on_draw;
    private delegate* unmanaged[Cdecl]<nint, void> _fn_draw_ctx;
    private delegate* unmanaged[Cdecl]<int, uint, uint, int, void> _fn_draw_args;
    private delegate* unmanaged[Cdecl]<int, int> _fn_eye2_begin;
    private delegate* unmanaged[Cdecl]<void> _fn_eye2_end;
    private delegate* unmanaged[Cdecl]<int, float, int, int, int, void> _fn_stereo_set;
    private delegate* unmanaged[Cdecl]<int, float, int, int, int, int, int> _fn_xr_set;
    private bool _xr_fov = true, _xr_seat = true;
    private bool _xr_want = false;
    private delegate* unmanaged[Cdecl]<int, float, float, void> _fn_xr_screen_set;
    private delegate* unmanaged[Cdecl]<int, float, float, void> _fn_ui_set;
    private delegate* unmanaged[Cdecl]<int> _fn_ui_draws;
    private delegate* unmanaged[Cdecl]<float, void> _fn_xr_pitch_follow;
    private float _xr_pitch_follow = 0.85f;
    private delegate* unmanaged[Cdecl]<int, int, int, float, float, void> _fn_cuts_set;
    private delegate* unmanaged[Cdecl]<int, int, float, float, int, void> _fn_turn_set;
    private delegate* unmanaged[Cdecl]<uint> _fn_turn_count;
    private delegate* unmanaged[Cdecl]<byte*, int, void> _fn_perf_status;
    private delegate* unmanaged[Cdecl]<int, void> _fn_noimg_set;
    private delegate* unmanaged[Cdecl]<uint> _fn_noimg_draws;
    private int _noimg_mode = 0;
    private delegate* unmanaged[Cdecl]<int, int, int, void> _fn_bisect_set;
    private delegate* unmanaged[Cdecl]<uint> _fn_scene_draws;
    private int _bisect_mode = 0, _bisect_a = 0, _bisect_b = 0;
    private delegate* unmanaged[Cdecl]<void> _fn_blend_off, _fn_blend_restore;
    private delegate* unmanaged[Cdecl]<int, void> _fn_hist_set;
    private delegate* unmanaged[Cdecl]<uint> _fn_hist_draws;
    private int _hist_mode = 1;
    private delegate* unmanaged[Cdecl]<int, float, void> _fn_move_set;
    private delegate* unmanaged[Cdecl]<float*, float*, float*, uint*, int*, void> _fn_move_status;
    private bool _move_remap = true, _move_invert = false;
    private bool _turn_stick = true, _turn_blink = false; private int _turn_mode = 1; private float _turn_snap = 30f, _turn_rate = 90f;
    private delegate* unmanaged[Cdecl]<int> _fn_cut_count;
    private bool _cut_recentre = true, _cut_blink = false, _follow_pans = false; private float _follow_rate = 20f;
    private delegate* unmanaged[Cdecl]<int, float, float, float, int, float, void> _fn_fp_set;
    private delegate* unmanaged[Cdecl]<float, float, float, int, void> _fn_player_set;
    private delegate* unmanaged[Cdecl]<float, float, float, int, void> _fn_player_head_set;
    private delegate* unmanaged[Cdecl]<float, float, float, int, void> _fn_anchor_override;
    private bool _battle_anchor = true; private int _battle_anchor_n = 0;
    // UI draw hiding experiment (cursor): 0 = off, 1 = hide draws from the 1024x1024 atlas, 2 = also the textureless draws, 3 = only in battle (1024x1024), 4 = textureless draws only, in battle, 5 = textureless except the 18-index window backgrounds, in battle
    private int _ui_hide = 0; private uint _ui_hidden_n = 0;
    private bool _hide_cursor = true;   // battle cursors (the hide list below), in battle, headset only
    // Hide list: per-quad sprite signatures "tex_w,tex_h,w0,w1,h0,h1" (texture size 0,0 = textureless; quad width /
    // height ranges in UI units; the game batches sprites, so a draw's index count is not part of it - a matching quad
    // inside a batch is cut out of the draw). Defaults are the two cursors picked on 21 Oct: the target pointer
    // (1024 atlas, 68x45) and the pointer over the acting character (textureless quad, width pulses 15..28, height 30).
    private static float[][] hide_list_defaults() => new[] { new float[] { 1024, 1024, 66, 69, 43, 46 }, new float[] { 0, 0, 8, 36, 24, 34 } };   // the CTB portraits are 70x46: keep them out
    private readonly List<float[]> _hide_list = new(hide_list_defaults());
    private bool _hide_list_dirty = true; private readonly uint[] _hide_hits = new uint[16];
    private bool _hide_pointers = true; private uint _pointer_stats = 0;   // the pointer rule (small quads above where an actor projects through the game camera)
    private delegate* unmanaged[Cdecl]<int, void> _fn_ui_hide_pointers; private delegate* unmanaged[Cdecl]<uint> _fn_ui_pointer_stats;
    private delegate* unmanaged[Cdecl]<float*, int, void> _fn_ui_hide_list; private delegate* unmanaged[Cdecl]<uint*, int, void> _fn_ui_hide_hits; private delegate* unmanaged[Cdecl]<uint*, int, int> _fn_draw_skip_ranges;
    // Battle: first person follows whoever stands in the player's slot (Tidus until he is swapped out, then
    // the character that took his place), and the view can switch to a chosen mode for the battle's duration.
    private bool _battle_follow = true; private int _battle_view = -1;   // -1 keep the current view; else the fp_mode for battles
    // Battle follow state. The game's own field roster (exe-analysis.md section 15) says who is on the
    // field; the eyes follow a chr id (0 Tidus, 1 Yuna, 2 Auron, 3 Kimahri, 4 Wakka, 5 Lulu, 6 Rikku,
    // 8..0x11 Aeons), and only the actor position is read from the entities.
    private int _btl_chr = -1; private int _btl_follow_id = -1, _btl_follow_slot = -1, _btl_pre_summon_id = -1, _btl_pending_id = -1, _btl_pending_frames = 0, _btl_pending_still = 0; private Vector4 _btl_pending_pos; private string _btl_pending_why = "";
    private bool _btl_was = false; private int _btl_mode_before = -1; private string _btl_who = ""; private bool _btl_aeon_out = false; private int _btl_cycle_req = 0;
    private readonly byte[] _btl_roster = new byte[3], _btl_roster_prev = new byte[3];
    private const int k_roster_off = 0xD2C895, k_roster_saved_off = 0xD2C89C, k_aeon_summoner_off = 0x6D1;
    private const int k_ply_n = 18, k_mon_n = 16;   // battle entity slots: players 18 (party, Aeons, the Magus Sisters), monsters 16
    private delegate* unmanaged[Cdecl]<float, void> _fn_floor_set; private float _diorama_floor = 8f;
    private delegate* unmanaged[Cdecl]<int, float, float, float, void> _fn_fp_head_set;
    private int _fp_head_anchor = 1; private float _fp_head_reach = 8f; private float _fp_head_up = 1.5f, _fp_head_smooth = 0.35f, _fp_head_fwd = 1.5f;   // 0 fixed, 1 head-bone height (default: swimming, crouching), 2 full head bone
    private delegate* unmanaged[Cdecl]<int> _fn_fp_hidden;
    private int _fp_mode = 0; private float _fp_eye_h = 18f, _fp_follow_dist = 60f, _fp_follow_h = 25f, _fp_hide_radius = 0.75f;
    private float _tp_dist = 24f, _tp_h = 22f, _tp_pitch = -8f, _tp_follow_rate = 0f;   // third person: units, units, degrees, deg/s (0 = heading never follows)
    private delegate* unmanaged[Cdecl]<float, float, float, float, void> _fn_tp_set;
    // Anchor smoothing (diorama / third person, and the battle centre): time constants in ms, applied once per frame
    // natively; 0 = snap (the closest to the behaviour before Oct 2026). A/B candidates: 24 and 325.
    private float _anchor_smooth_ms = 0f, _battle_anchor_smooth_ms = 0f;
    private delegate* unmanaged[Cdecl]<float, float, void> _fn_view_smooth_set;
    private delegate* unmanaged[Cdecl]<byte*, int, int> _fn_lowres_status;   // the "game renders below the headset" line
    private delegate* unmanaged[Cdecl]<int, int, void> _fn_xr_debug;          // headset test hooks: drop the eye image, simulate swapchain timeouts
    private bool _xr_drop_image = false; private int _xr_sim_timeouts_req = 0;  // session only, never saved
    private delegate* unmanaged[Cdecl]<float*, int, int> _fn_bones_capture; private int _bones_take = 0;
    private delegate* unmanaged[Cdecl]<int, int, float, void> _fn_bone_wiggle; private delegate* unmanaged[Cdecl]<int*, int*, int> _fn_bone_status;
    private delegate* unmanaged[Cdecl]<int, int, void> _fn_bones_seq; private delegate* unmanaged[Cdecl]<int> _fn_bones_seq_left; private int _seq_frames = 240;
    private delegate* unmanaged[Cdecl]<int, int, float, float, float, float, float, float, int, int, int, void> _fn_hands_set; private bool _hands_swap = false, _hands_mirror = false; private int _hands_only = 0; private delegate* unmanaged[Cdecl]<uint*, nint> _fn_hands_status;
    private delegate* unmanaged[Cdecl]<int, float, int> _fn_hands_side; private int _hands_px_left = -1; private float _hands_stretch = 1.6f; private bool _hands_side_pushed = false;
    private delegate* unmanaged[Cdecl]<void> _fn_hands_calibrate, _fn_hands_dump; private delegate* unmanaged[Cdecl]<int, int, void> _fn_hands_method; private bool _hands_nan = true, _hands_split = false; private delegate* unmanaged[Cdecl]<float*, int, uint> _fn_hands_cal_data;
    private double _hands_cal_at = -1; private uint _hands_cal_seen = 0; private string _hands_cal_ini = ""; private bool _hands_cal_loaded = false;
    private bool _hands_dev = false; private bool _hands_on = false, _hands_head = true; private int _hands_show = 3; private float _hands_yaw = 0f, _hands_pitch = 0f, _hands_roll = 0f, _hands_off_r = 0f, _hands_off_u = 0f, _hands_off_b = 0f;
    private bool _wig_on = false; private int _wig_rec = 10, _wig_slot = 0; private float _wig_amt = 3f;
    // Bone labels: (record, slot) -> name, kept in bone_labels.txt next to the mod.
    private readonly Dictionary<(int, int), string> _bone_labels = new();
    private static readonly string[] k_bone_names = { "L upper arm", "L forearm", "L hand", "L fingers", "R upper arm", "R forearm", "R hand", "R fingers", "head", "neck", "torso", "hip", "L leg", "R leg", "nothing visible", "other" };
    private string _bone_labels_path => Path.Combine(Path.GetDirectoryName(_ini_path) ?? ".", "bone_labels.txt");
    private void bone_labels_load() {
        try { if (!File.Exists(_bone_labels_path)) return;
            foreach (string l in File.ReadAllLines(_bone_labels_path)) { var t = l.Split('\t'); if (t.Length >= 3 && int.TryParse(t[0], System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out int r) && int.TryParse(t[1], System.Globalization.NumberStyles.Integer, System.Globalization.CultureInfo.InvariantCulture, out int s)) _bone_labels[(r, s)] = t[2]; } }
        catch { }
    }
    private void bone_labels_save() {
        try { var lines = new List<string>(); foreach (var kv in _bone_labels) lines.Add(kv.Key.Item1.ToString(System.Globalization.CultureInfo.InvariantCulture) + "\t" + kv.Key.Item2.ToString(System.Globalization.CultureInfo.InvariantCulture) + "\t" + kv.Value); lines.Sort(); File.WriteAllLines(_bone_labels_path, lines); }
        catch { }
    }
    private bool _fp_hide_body = true; private float _scale_before_fp = 94f;
    private int _fp_body_mode = 0; private string _fp_head_parts = "0,1,2,3,9"; private int _fp_solo = -1;   // 0 = whole body hidden (default). Tidus (c101): parts 0-3 hair, 9 face/head, kept for later   // 0 hide all, 1 hide head parts only, 2 show all
    private delegate* unmanaged[Cdecl]<int, uint, int, void> _fn_fp_body_set;
    private string? _head_mask_src; private uint _head_mask;   // R22: parse_mask(_fp_head_parts), cached
    private delegate* unmanaged[Cdecl]<uint> _fn_fp_parts;
    private static uint parse_mask(string list) {
        uint m = 0;
        foreach (var tok in list.Split(new[] { ',', ' ', ';' }, StringSplitOptions.RemoveEmptyEntries))
            if (int.TryParse(tok.Trim(), out int i) && i >= 0 && i < 32) m |= 1u << i;
        return m;
    }
    private const float k_true_scale = 11.4f;   // 20-unit characters = 1.75 m
    private delegate* unmanaged[Cdecl]<float*, uint*, int, void> _fn_markers_set;
    private delegate* unmanaged[Cdecl]<int, void> _fn_markers_3d;
    private float[] _mk_pos = new float[32 * 3]; private uint[] _mk_col = new uint[32]; private int _mk_n;
    private bool _markers_3d = true;
    private float _marker_scale = 1f; private delegate* unmanaged[Cdecl]<float, void> _fn_markers_scale; private delegate* unmanaged[Cdecl]<void> _fn_ui_atlas_dump; private delegate* unmanaged[Cdecl]<int, void> _fn_motion_log;
    private bool _markers_game_art = false; private uint _markers_game_art_n = 0; private delegate* unmanaged[Cdecl]<int, uint> _fn_markers_game_art;   // the game's own cursor sprites at the markers
    private bool _ui_on = true; private float _ui_dist = 1.5f, _ui_width = 1.6f;
    private delegate* unmanaged[Cdecl]<int> _fn_xr_screen_now;
    private int _xr_screen_mode = 0; private float _xr_screen_width = 2.5f, _xr_screen_dist = 2.5f;
    // Stereo screen: 3D frames on the big screen, through the game's own camera, in stereo. Default for blitzball;
    // optional everywhere. The screen plane is the distance (game units) that sits exactly on the screen (nearer
    // pops out, farther recedes); depth 1 = the stereographers' 1/30 rule (eye separation = that distance / 30).
    private bool _s3_blitz = true, _s3_all = false; private float _s3_depth = 1f, _s3_plane = 300f;
    private bool _s3_auto = true; private float _s3_max_pop = 2.5f;   // auto depth: the screen plane follows the subject, pop-out capped (% of screen width)
    private delegate* unmanaged[Cdecl]<int, float, float, int, float, void> _fn_screen3d;
    private delegate* unmanaged[Cdecl]<int*, float*, float*, float*, float*, void> _fn_screen3d_status;
    private int _backdrop_mode = 1;
    private delegate* unmanaged[Cdecl]<int, int> _fn_cull_dist_set;
    private delegate* unmanaged[Cdecl]<int> _fn_cull_dist_get;
    private bool _cull_dist = true; private int _cull_dist_rc = 0;
    private delegate* unmanaged[Cdecl]<int, int> _fn_cull_scene_set;
    private delegate* unmanaged[Cdecl]<int> _fn_cull_scene_get;
    private bool _cull_scene = true; private int _cull_scene_rc = 0;
    private delegate* unmanaged[Cdecl]<int, void> _fn_vrpad_set;
    private delegate* unmanaged[Cdecl]<float*, byte*, byte*, int, uint*, int> _fn_vrpad_status;
    private delegate* unmanaged[Cdecl]<int, float, float, void> _fn_haptic;
    private bool _vrpad = true;
    private bool _haptics = true; private float _haptics_gain = 0.8f; private int _haptics_status;   // rumble to haptics (ini haptics, haptics_gain)
    private delegate* unmanaged[Cdecl]<int, float, int> _fn_haptics_set;
    private delegate* unmanaged[Cdecl]<int, int, int, int, void> _fn_imgui_layer_set; private delegate* unmanaged[Cdecl]<uint> _fn_imgui_draws; private bool _imgui_layer = true; private int _imgui_min_draws = 12;
    private delegate* unmanaged[Cdecl]<int*, int, int> _fn_imgui_status;   // ABI 21 (saveui): where the ImGui layer goes, the forced screen
    /// <summary>saveui: ini fh_menu_on_screen (default 1, applies at once). While Fahrenheit's save screen is open the frame goes to
    /// the virtual screen and its menu lies exactly over it, world-locked, as on the monitor (native xr_tick). 0: the old
    /// head-locked layer over whatever view was showing.</summary>
    private bool _fh_menu_on_screen = true;
    /// <summary>FH-1 (Fahrenheit port): ini fh_dpi_fix (default 1, applies at once). Fahrenheit alpha12 turns on ImGui's
    /// ConfigDpiScaleFonts / ConfigDpiScaleViewports (runtime/imgui.cs:90-91, once, in platform_bind), so every ImGui font is
    /// multiplied by the Windows display scale although the game is DPI-unaware (its save screen outgrows its frames); main
    /// turned both off (#376, 5173f28). 1: we do the same once Fahrenheit has set them, and put FontScaleDpi back to 1 at that
    /// edge (ImGui leaves it as it was); 0: Fahrenheit's own values are put back. Nothing is written where Fahrenheit already
    /// has them off (alpha13). Our only write to Fahrenheit's ImGui state (design-notes, Fahrenheit port).</summary>
    private bool _fh_dpi_fix = true;
    private int  _dpi_fh_fonts = -1; private bool _dpi_fh_vps;   // Fahrenheit's values as first seen (-1: not yet)
    private bool _dpi_ours; private int _dpi_offs;                // the flags are off because of us; how many times we turned them off
    private const float k_fh_font_px = 20f;                       // Fahrenheit's base font (core/gui/gui.cs: AddFontFromFileTTF(..., 20f))
    /// <summary>FH-2: ini overlay_text_scale (default 1, 0.5..2 in steps of 0.05, applies at once): our text (F8 window, Eye 2,
    /// markers, hover labels) relative to its default size, 20 px x the Windows display scale.</summary>
    private float _overlay_text_scale = 1f;
    private float _our_px = k_fh_font_px;                         // FH-2: this frame's pushed size, computed before the push
    private uint _save_st;        // cpu-lean: this frame's save_screen_state(), read once in push_all
    private bool _fh_menu_open;   // latched: set at OPEN (2: Fahrenheit draws the menu, from a full-screen black), cleared at CLOSED (0)
    /// <summary>Fahrenheit's save-screen state word (FhSavePal is internal; FhSavePal.pal_addr_screen_state, FFX: 0x8CB994, a DWORD in
    /// FFX.exe's .data, VA 0xCCB994): 0 closed, 1 opening, 2 open, 3 / 4 unnamed. A guarded read (native, VirtualQuery first):
    /// an access violation is fatal under Fahrenheit's stage-0 debugger even inside a try.</summary>
    private uint save_screen_state() {
        if (_fn_safe_read == null) return 0;
        uint v = 0;
        return _fn_safe_read(FhUtil.ptr_at<uint>(0x8CB994), &v, 4) != 0 ? v : 0u;
    }
    private delegate* unmanaged[Cdecl]<int, float, void> _fn_gesture_set;
    private delegate* unmanaged[Cdecl]<void> _fn_gesture_reset;
    private delegate* unmanaged[Cdecl]<float*, float*, int*, uint> _fn_gesture_poll;
    private bool _gestures = true; private float _gest_tilt = 1f; private float _gest_scale_mul = 1f; private int _gest_active = 0; private float _gest_move_x, _gest_move_y, _gest_move_z;
    private delegate* unmanaged[Cdecl]<int, void> _fn_msaa_set;
    private delegate* unmanaged[Cdecl]<uint*, uint*, uint*, uint*, uint*, void> _fn_msaa_status;
    private int _msaa = 4;   // 0 = game default (8x), 2, 4
    private delegate* unmanaged[Cdecl]<int, int, int> _fn_patch_set;
    private delegate* unmanaged[Cdecl]<int> _fn_patch_count;
    private delegate* unmanaged[Cdecl]<int, byte*> _fn_patch_name;
    private bool[] _patch_on = new bool[8];
    private delegate* unmanaged[Cdecl]<int, void> _fn_backdrop_set;
    private delegate* unmanaged[Cdecl]<uint> _fn_backdrop_draws;
    private delegate* unmanaged[Cdecl]<void> _fn_xr_recenter;
    private delegate* unmanaged[Cdecl]<int, void> _fn_dump_seq;
    private delegate* unmanaged[Cdecl]<byte*, int, void> _fn_xr_status;
    private bool  _xr_on = false, _xr_rotate = true, _xr_translate = true;
    private float _xr_scale = 94f;
    private delegate* unmanaged[Cdecl]<int, nint> _fn_eye2_srv;
    private int _eye2_source = 0;
    private delegate* unmanaged[Cdecl]<int> _fn_eye2_draws;
    private delegate* unmanaged[Cdecl]<int> _fn_eye2_same_inputs;      // eye-2 draws last frame with no swapped input and no eye-2 constants (an upper bound)
    private delegate* unmanaged[Cdecl]<int, void> _fn_diag_keepalive;  // keeps the native per-frame diagnostics armed; only while Debug is expanded
    private delegate* unmanaged[Cdecl]<int*, int*, int, int> _fn_eye2_size;
    private bool  _stereo_on = false;
    private float _ipd = 6f;
    private bool  _eye2_preview = false;   // the flat-screen debug window for eye 2; never while the headset runs (it would land in the ImGui layer)
    private delegate* unmanaged[Cdecl]<nint, nint, nint, nint> _fn_on_updatesub;
    private delegate* unmanaged[Cdecl]<uint, nint*, nint, void> _fn_on_rendertargets;
    private delegate* unmanaged[Cdecl]<uint, nint*, nint, void> _fn_on_rendertargets_uav;
    private delegate* unmanaged[Cdecl]<nint, uint, nint, uint, uint, void> _fn_on_resolve;
    private delegate* unmanaged[Cdecl]<float, float, float, float, void> _fn_on_viewport;
    private delegate* unmanaged[Cdecl]<nint, int, void> _fn_on_clear;
    private delegate* unmanaged[Cdecl]<void> _fn_frame_arm;
    private delegate* unmanaged[Cdecl]<uint, uint, int, void> _fn_ui_hide; private delegate* unmanaged[Cdecl]<uint> _fn_ui_hidden_draws; private delegate* unmanaged[Cdecl]<int, void> _fn_ui_hide_cursor; private delegate* unmanaged[Cdecl]<uint> _fn_ui_rect_stats; private uint _rect_stats = 0;
    private delegate* unmanaged[Cdecl]<float, float, void> _fn_ui_pick; private delegate* unmanaged[Cdecl]<nint> _fn_ui_pick_result; private delegate* unmanaged[Cdecl]<int, void> _fn_slowmo;
    private string _pick_text = ""; private int _pick_n = 0; private double _pick_poll_until = -1; private int _pick_poll_frames = 0; private int _slowmo_ms = 150; private bool _slowmo_on = false;
    private delegate* unmanaged[Cdecl]<float, float, int, void> _fn_ui_hover; private delegate* unmanaged[Cdecl]<float*, int, int> _fn_ui_hover_rects;
    private int _hover_mode = 0;   // 0 off, 1 outline the quads under the mouse, 2 outline every UI quad (F11 cycles)
    private delegate* unmanaged[Cdecl]<uint, int, void> _fn_dump_cb; private int _dump_cb_size = 1456;
    private delegate* unmanaged[Cdecl]<uint, uint, void> _fn_census_arm;
    private delegate* unmanaged[Cdecl]<uint, uint, nint*, void> _fn_on_ps_srvs;
    private delegate* unmanaged[Cdecl]<nint, int, float*, uint, float, byte, void> _fn_on_clear_twin;
    private delegate* unmanaged[Cdecl]<nint, void> _fn_set_backbuffer;
    private delegate* unmanaged[Cdecl]<nint, uint, uint, uint, uint, nint, uint, nint, int, int> _fn_on_copy;
    private int _frame_census_n = 0;
    private delegate* unmanaged[Cdecl]<int> _fn_measure;
    private int _measure_n = 0;
    private delegate* unmanaged[Cdecl]<int, int, float, float, float, float, float, void> _fn_override_set;
    private delegate* unmanaged[Cdecl]<int> _fn_override_applied;
    private delegate* unmanaged[Cdecl]<int> _fn_override_mask;
    private delegate* unmanaged[Cdecl]<void*, void*, uint, int> _fn_safe_read;
    private delegate* unmanaged[Cdecl]<float, float, float, float*, int> _fn_project;
    private bool  _markers_on    = true;
    private float _marker_height = 6f;   // clearance of the pointer's tip above the actor's head (render units)
    private int   _markers_drawn;
    private delegate* unmanaged[Cdecl]<float*, int, int> _fn_world_positions;
    private double _last_pos_log;

    // -- camera override state (owned here, pushed to native every frame) ----
    private bool  _ovr_enabled, _ovr_freeze;
    private float _ovr_yaw, _ovr_pitch;
    private float _ovr_right, _ovr_up = -78f, _ovr_forward;   // seat height default from testing

    // -- inline detours on d3d11.dll's draw functions ------------------------
    //
    // Patching the context's vtable for these does not stick: the D3D11
    // runtime restores the draw slots itself, as the vtable audit showed. So
    // the functions are detoured in place through Fahrenheit's MinHook, the
    // same mechanism it uses for Present. Addresses come from the vtable at
    // install time, which by then holds the runtime's own entries.

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_DrawIndexed(nint ctx, uint index_count, uint start_index, int base_vertex);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_Draw(nint ctx, uint vertex_count, uint start_vertex);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_DrawIndexedInstanced(nint ctx, uint a, uint b, uint c, int d, uint e);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_DrawInstanced(nint ctx, uint a, uint b, uint c, uint d);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_UpdateSubresource(nint ctx, nint resource, uint sub, nint box, nint src, uint row_pitch, uint depth_pitch);

    private d_DrawIndexed?          _h_drawindexed,  _o_drawindexed;
    private d_Draw?                 _h_draw,         _o_draw;
    private d_DrawIndexedInstanced? _h_drawidxinst,  _o_drawidxinst;
    private d_DrawInstanced?        _h_drawinst,     _o_drawinst;
    private d_UpdateSubresource?    _h_updatesub,    _o_updatesub;
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_OMSetRenderTargets(nint ctx, uint n, nint* rtvs, nint dsv);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_RSSetViewports(nint ctx, uint n, float* viewports);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_ClearRenderTargetView(nint ctx, nint rtv, float* color);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_ClearDepthStencilView(nint ctx, nint dsv, uint flags, float depth, byte stencil);
    private d_OMSetRenderTargets?     _h_omrt,  _o_omrt;
    private d_RSSetViewports?         _h_vp,    _o_vp;
    private d_ClearRenderTargetView?  _h_clr,   _o_clr;
    private d_ClearDepthStencilView?  _h_clrd,  _o_clrd;
    private nint _a_omrt, _a_vp, _a_clr, _a_clrd, _a_pssrv, _a_copy, _a_copyreg;
    private nint _a_resolve, _a_omrtuav;
    private nint _a_exitprocess;
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_ExitProcess(uint code);
    private d_ExitProcess? _h_exitprocess, _o_exitprocess;
    private delegate* unmanaged[Cdecl]<void> _fn_shutdown;
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_CopyResource(nint ctx, nint dst, nint src);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_ResolveSubresource(nint ctx, nint dst, uint dst_sub, nint src, uint src_sub, uint fmt);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_OMSetRenderTargetsAndUAVs(nint ctx, uint n, nint* rtvs, nint dsv, uint uav_start, uint uav_n, nint* uavs, uint* counts);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_CopySubresourceRegion(nint ctx, nint dst, uint dst_sub, uint x, uint y, uint z, nint src, uint src_sub, nint box);
    private d_CopyResource? _h_copy, _o_copy;
    private d_ResolveSubresource? _h_resolve, _o_resolve;
    private d_OMSetRenderTargetsAndUAVs? _h_omrtuav, _o_omrtuav;
    private d_CopySubresourceRegion? _h_copyreg, _o_copyreg;
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void d_PSSetShaderResources(nint ctx, uint start, uint n, nint* views);
    private d_PSSetShaderResources? _h_pssrv, _o_pssrv;
    private nint _a_drawindexed, _a_draw, _a_drawidxinst, _a_drawinst, _a_updatesub;
    private string _detour_status = "not installed";

    public FFXVRModule() { }

    public override bool init(FhModContext mod_context, FileStream global_state_file) {
        _logger.Info($"FFX VR {s_mod_version} module initialized (Fahrenheit {s_fh_version}).");
        settings_load();
        load_native(mod_context);
        if (_native_ok) cpu_settings_push();   // vq-lean (review SF1): fast_reads before any frame reads (cpu_report.cs)
        if (_fn_haptics_set != null) _fn_haptics_set(_haptics ? 1 : 0, _haptics_gain);   // before ffxvr_init, which installs the XInputSetState hook only if haptics or build 7 are on
        diag7_init();   // build 7 (log only): game-function hooks must be queued here, at init (diag7.cs)
        ld_init();      // load diagnostics (load_diag.cs): queued after build 7's hooks (the job join included), so a failure here cannot take them with it (LD-7)
        sr_init();      // stickrep (stickrep.cs): the flush gate's two hooks, queued last of all ours (Fahrenheit's commit stops at the first failing insertion)
        dmg3d_init();   // 3D damage numbers (dmg3d.cs): the hop table; the queue draw's hook only with dmg3d_hide2d=1, after all the others
        return true;
    }

    /// <summary>
    ///     Loads the native helper by absolute path, rather than relying on the
    ///     default DLL search order finding it inside the mod folder. Hooks are
    ///     not installed yet: that needs the game's device context, which only
    ///     exists once ImGui's D3D11 backend has been initialized.
    /// </summary>
    private void load_native(FhModContext mod_context) {
        string path = Path.Join(mod_context.Paths.ModDir.FullName, NativeDll);
        _h2_mod_dir = mod_context.Paths.ModDir.FullName;   // VR hands v2: the packs are in <mod>\hands\ (hands.cs)

        if (!File.Exists(path)) {
            _native_mismatch = true; native_hard_fail("missing: " + NativeDll);
            _logger.Warning($"Native helper not found at {path}. Constant buffer capture is off.");
            return;
        }

        try {
            _native_handle = NativeLibrary.Load(path);
            // Every export is required: both DLLs ship together and nothing is compiled conditionally, so a missing
            // name means a mismatched pair. Nothing is called until the whole set (and ffxvr_abi) checks out.
            var missing = new List<string>();
            nint req(string name) { if (NativeLibrary.TryGetExport(_native_handle, name, out nint a)) return a; missing.Add(name); return 0; }
            var fn_abi = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_abi");

            _fn_init         = (delegate* unmanaged[Cdecl]<nint, nint, int>)req("ffxvr_init");
            _fn_buffer_count = (delegate* unmanaged[Cdecl]<int>)      req("ffxvr_buffer_count");
            _fn_dump_count   = (delegate* unmanaged[Cdecl]<int>)      req("ffxvr_dump_count");
            _fn_hook_calls   = (delegate* unmanaged[Cdecl]<int>)      req("ffxvr_hook_calls");
            _fn_begin_frame  = (delegate* unmanaged[Cdecl]<void>)     req("ffxvr_begin_frame");
            _fn_frame_writes = (delegate* unmanaged[Cdecl]<int>)      req("ffxvr_frame_writes");
            _fn_get_camera   = (delegate* unmanaged[Cdecl]<float*, int>)req("ffxvr_get_camera");
            _fn_camera_reads = (delegate* unmanaged[Cdecl]<int>)      req("ffxvr_camera_reads");
            _fn_slot0_count  = (delegate* unmanaged[Cdecl]<int>)      req("ffxvr_slot0_count");
            _fn_camera_changes = (delegate* unmanaged[Cdecl]<int>)    req("ffxvr_camera_changes");
            _fn_counter      = (delegate* unmanaged[Cdecl]<int, int>) req("ffxvr_counter");
            _fn_on_draw      = (delegate* unmanaged[Cdecl]<int, int>)req("ffxvr_on_draw");
            _fn_draw_ctx     = (delegate* unmanaged[Cdecl]<nint, void>)req("ffxvr_draw_ctx");
            _fn_draw_args = (delegate* unmanaged[Cdecl]<int, uint, uint, int, void>)req("ffxvr_draw_args");
            _fn_eye2_begin   = (delegate* unmanaged[Cdecl]<int, int>)req("ffxvr_eye2_begin");
            _fn_eye2_end     = (delegate* unmanaged[Cdecl]<void>)req("ffxvr_eye2_end");
            _fn_stereo_set   = (delegate* unmanaged[Cdecl]<int, float, int, int, int, void>)req("ffxvr_stereo_set");
            _fn_xr_set       = (delegate* unmanaged[Cdecl]<int, float, int, int, int, int, int>)req("ffxvr_xr_set");
            _fn_xr_recenter  = (delegate* unmanaged[Cdecl]<void>)req("ffxvr_xr_recenter");
            _fn_xr_screen_set = (delegate* unmanaged[Cdecl]<int, float, float, void>)req("ffxvr_xr_screen_set");
            _fn_ui_set       = (delegate* unmanaged[Cdecl]<int, float, float, void>)req("ffxvr_ui_set");
            _fn_ui_draws     = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_ui_draws");
            _fn_xr_pitch_follow = (delegate* unmanaged[Cdecl]<float, void>)req("ffxvr_xr_pitch_follow");
            _fn_cuts_set     = (delegate* unmanaged[Cdecl]<int, int, int, float, float, void>)req("ffxvr_cuts_set");
            _fn_eye_jump_set = (delegate* unmanaged[Cdecl]<int, float, int, int, int*, int, int>)req("ffxvr_eye_jump_set"); _fn_eye_event = (delegate* unmanaged[Cdecl]<int, float, float, float, void>)req("ffxvr_eye_event");   // ABI 44: eye jumps (eye_jump.cs)
            _fn_bt_tag_step = (delegate* unmanaged[Cdecl]<int, float, float, float, float, float, float*, int, int>)req("ffxvr_bt_tag_step"); _fn_bt_tag_hold = (delegate* unmanaged[Cdecl]<int, float, float, float, int>)req("ffxvr_bt_tag_hold");   // ABI 46: battle tag (bt_tag.cs)
            _fn_dmg_walk = (delegate* unmanaged[Cdecl]<byte*, uint, uint, DmgNode*, int, uint*, int>)req("ffxvr_dmg_walk"); _fn_dmg_queue = (delegate* unmanaged[Cdecl]<byte*, int, int, DmgNode*, int, DmgChr*, int, byte*, uint*, int>)req("ffxvr_dmg_queue");   // ABI 47: 3D damage numbers (dmg3d.cs)
            _fn_dmg_set = (delegate* unmanaged[Cdecl]<DmgNode*, int, DmgChr*, int, uint, uint, uint, int, void>)req("ffxvr_dmg_set"); _fn_dmg_cfg = (delegate* unmanaged[Cdecl]<int, float, float, byte*, int>)req("ffxvr_dmg_cfg");
            _fn_dmg_skip = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_dmg_skip"); _fn_dmg_status = (delegate* unmanaged[Cdecl]<uint*, int, void>)req("ffxvr_dmg_status");
            _fn_turn_set     = (delegate* unmanaged[Cdecl]<int, int, float, float, int, void>)req("ffxvr_turn_set");
            _fn_turn_count   = (delegate* unmanaged[Cdecl]<uint>)req("ffxvr_turn_count");
            _fn_perf_status  = (delegate* unmanaged[Cdecl]<byte*, int, void>)req("ffxvr_perf_status");
            _fn_noimg_set    = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_noimg_set");
            _fn_noimg_draws  = (delegate* unmanaged[Cdecl]<uint>)req("ffxvr_noimg_draws");
            _fn_bisect_set   = (delegate* unmanaged[Cdecl]<int, int, int, void>)req("ffxvr_bisect_set");
            _fn_scene_draws  = (delegate* unmanaged[Cdecl]<uint>)req("ffxvr_scene_draws");
            _fn_blend_off    = (delegate* unmanaged[Cdecl]<void>)req("ffxvr_blend_off");
            _fn_blend_restore= (delegate* unmanaged[Cdecl]<void>)req("ffxvr_blend_restore");
            _fn_hist_set     = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_hist_set");
            _fn_hist_draws   = (delegate* unmanaged[Cdecl]<uint>)req("ffxvr_hist_draws");
            _fn_move_set     = (delegate* unmanaged[Cdecl]<int, float, void>)req("ffxvr_move_set");
            _fn_move_status  = (delegate* unmanaged[Cdecl]<float*, float*, float*, uint*, int*, void>)req("ffxvr_move_status");
            _fn_cut_count    = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_cut_count");
            _fn_fp_set       = (delegate* unmanaged[Cdecl]<int, float, float, float, int, float, void>)req("ffxvr_fp_set");
            _fn_player_set   = (delegate* unmanaged[Cdecl]<float, float, float, int, void>)req("ffxvr_player_set");
            _fn_player_head_set = (delegate* unmanaged[Cdecl]<float, float, float, int, void>)req("ffxvr_player_head_set");
            _fn_ride_set     = (delegate* unmanaged[Cdecl]<int, float, float, float, float, float, float, float, uint>)req("ffxvr_ride_set");   // ABI 32: riding (ride.cs)
            _fn_ride_stats   = (delegate* unmanaged[Cdecl]<float*, int, int>)req("ffxvr_ride_stats");   // ABI 33
            _fn_fp_self_set  = (delegate* unmanaged[Cdecl]<float, float, float, int, int, float*, int>)req("ffxvr_fp_self_set");   // ABI 37: the body hide's ownership (body_hide.cs); ABI 38: + its return
            _fn_fp_others_set = (delegate* unmanaged[Cdecl]<float*, float*, int, int>)req("ffxvr_fp_others_set");   // ABI 39: + the heads (npc-anchor)
            _fn_fp_ib_set    = (delegate* unmanaged[Cdecl]<uint*, int, uint, int>)req("ffxvr_fp_ib_set");   // ABI 40: the exact owner by index buffer (npc-anchor round 2)
            _fn_fmv3d_set    = (delegate* unmanaged[Cdecl]<int, float, int, int, float, float, float, float, void>)req("ffxvr_fmv3d_set"); _fn_fmv3d_text = (delegate* unmanaged[Cdecl]<int, byte*, int, int>)req("ffxvr_fmv3d_text");   // ABI 41: FMVs in 3D (fmv_relief.cs); 42: the "3D movie" mapping; 43: the edge band
            _fn_cpu_report   = (delegate* unmanaged[Cdecl]<float*, int, void>)req("ffxvr_cpu_report");   // ABI 38: the cpu: line's managed sections (cpu_report.cs)
            _fn_sg_install   = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_shadow_guard_install");    // ABI 34: the ShadowManager pool guard (shadow_cam.cs)
            _fn_sg_budget    = (delegate* unmanaged[Cdecl]<float, int, void>)req("ffxvr_shadow_budget_set");
            _fn_sg_status    = (delegate* unmanaged[Cdecl]<int*, int, int>)req("ffxvr_shadow_pool_status");
            _fn_anchor_override = (delegate* unmanaged[Cdecl]<float, float, float, int, void>)req("ffxvr_anchor_override");
            _fn_floor_set    = (delegate* unmanaged[Cdecl]<float, void>)req("ffxvr_floor_set");
            _fn_fp_head_set  = (delegate* unmanaged[Cdecl]<int, float, float, float, void>)req("ffxvr_fp_head_set");
            _fn_fp_hidden    = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_fp_hidden");
            _fn_markers_set  = (delegate* unmanaged[Cdecl]<float*, uint*, int, void>)req("ffxvr_markers_set");
            _fn_markers_3d   = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_markers_3d");
            _fn_markers_game_art = (delegate* unmanaged[Cdecl]<int, uint>)req("ffxvr_markers_game_art"); _fn_markers_scale = (delegate* unmanaged[Cdecl]<float, void>)req("ffxvr_markers_scale"); _fn_ui_atlas_dump = (delegate* unmanaged[Cdecl]<void>)req("ffxvr_ui_atlas_dump"); _fn_motion_log = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_motion_log");
            _fn_xr_screen_now = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_xr_screen_now");
            _fn_screen3d = (delegate* unmanaged[Cdecl]<int, float, float, int, float, void>)req("ffxvr_screen3d"); _fn_screen3d_status = (delegate* unmanaged[Cdecl]<int*, float*, float*, float*, float*, void>)req("ffxvr_screen3d_status");
            _fn_dump_seq     = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_dump_seq");
            _fn_shutdown     = (delegate* unmanaged[Cdecl]<void>)req("ffxvr_shutdown");
            _fn_xr_status    = (delegate* unmanaged[Cdecl]<byte*, int, void>)req("ffxvr_xr_status");
            _fn_eye2_srv     = (delegate* unmanaged[Cdecl]<int, nint>)req("ffxvr_eye2_srv");
            _fn_eye2_draws   = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_eye2_draws");
            _fn_eye2_same_inputs = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_eye2_same_inputs");
            _fn_diag_keepalive = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_diag_keepalive");
            _fn_eye2_size    = (delegate* unmanaged[Cdecl]<int*, int*, int, int>)req("ffxvr_eye2_size");
            _fn_on_updatesub = (delegate* unmanaged[Cdecl]<nint, nint, nint, nint>)req("ffxvr_on_updatesub");
            _fn_on_rendertargets = (delegate* unmanaged[Cdecl]<uint, nint*, nint, void>)req("ffxvr_on_rendertargets");
            _fn_on_rendertargets_uav = (delegate* unmanaged[Cdecl]<uint, nint*, nint, void>)req("ffxvr_on_rendertargets_uav");
            _fn_on_resolve = (delegate* unmanaged[Cdecl]<nint, uint, nint, uint, uint, void>)req("ffxvr_on_resolve");
            _fn_on_viewport  = (delegate* unmanaged[Cdecl]<float, float, float, float, void>)req("ffxvr_on_viewport");
            _fn_on_clear     = (delegate* unmanaged[Cdecl]<nint, int, void>)req("ffxvr_on_clear");
            _fn_frame_arm    = (delegate* unmanaged[Cdecl]<void>)req("ffxvr_frame_arm");
            _fn_ui_hide = (delegate* unmanaged[Cdecl]<uint, uint, int, void>)req("ffxvr_ui_hide"); _fn_ui_hidden_draws = (delegate* unmanaged[Cdecl]<uint>)req("ffxvr_ui_hidden_draws"); _fn_ui_hide_cursor = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_ui_hide_cursor"); _fn_ui_rect_stats = (delegate* unmanaged[Cdecl]<uint>)req("ffxvr_ui_rect_stats");
            _fn_ui_hide_list = (delegate* unmanaged[Cdecl]<float*, int, void>)req("ffxvr_ui_hide_list"); _fn_ui_hide_hits = (delegate* unmanaged[Cdecl]<uint*, int, void>)req("ffxvr_ui_hide_hits"); _fn_draw_skip_ranges = (delegate* unmanaged[Cdecl]<uint*, int, int>)req("ffxvr_draw_skip_ranges");
            _fn_ui_hide_pointers = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_ui_hide_pointers"); _fn_ui_pointer_stats = (delegate* unmanaged[Cdecl]<uint>)req("ffxvr_ui_pointer_stats");
            _fn_blitz_overlay = (delegate* unmanaged[Cdecl]<int, uint>)req("ffxvr_blitz_overlay");
            _fn_ui_pick = (delegate* unmanaged[Cdecl]<float, float, void>)req("ffxvr_ui_pick"); _fn_ui_pick_result = (delegate* unmanaged[Cdecl]<nint>)req("ffxvr_ui_pick_result");
            _fn_slowmo = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_slowmo");
            _fn_ui_hover = (delegate* unmanaged[Cdecl]<float, float, int, void>)req("ffxvr_ui_hover"); _fn_ui_hover_rects = (delegate* unmanaged[Cdecl]<float*, int, int>)req("ffxvr_ui_hover_rects");
            _fn_dump_cb      = (delegate* unmanaged[Cdecl]<uint, int, void>)req("ffxvr_dump_cb");
            _fn_census_arm   = (delegate* unmanaged[Cdecl]<uint, uint, void>)req("ffxvr_census_arm");
            _fn_backdrop_set = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_backdrop_set");
            _fn_cull_dist_set = (delegate* unmanaged[Cdecl]<int, int>)req("ffxvr_cull_dist_set");
            _fn_cull_dist_get = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_cull_dist_get");
            _fn_cull_scene_set = (delegate* unmanaged[Cdecl]<int, int>)req("ffxvr_cull_scene_set");
            _fn_cull_scene_get = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_cull_scene_get");
            _fn_msaa_set     = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_msaa_set");
            _fn_vrpad_set    = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_vrpad_set");
            _fn_imgui_layer_set = (delegate* unmanaged[Cdecl]<int, int, int, int, void>)req("ffxvr_imgui_layer_set");   // ABI 21: + menu
            _fn_imgui_status = (delegate* unmanaged[Cdecl]<int*, int, int>)req("ffxvr_imgui_status");   // ABI 21 (saveui)
            _fn_imgui_draws  = (delegate* unmanaged[Cdecl]<uint>)req("ffxvr_imgui_draws");
            _fn_vrpad_status = (delegate* unmanaged[Cdecl]<float*, byte*, byte*, int, uint*, int>)req("ffxvr_vrpad_status");
            _fn_haptic       = (delegate* unmanaged[Cdecl]<int, float, float, void>)req("ffxvr_haptic");
            _fn_gesture_set  = (delegate* unmanaged[Cdecl]<int, float, void>)req("ffxvr_gesture_set");
            _fn_gesture_reset = (delegate* unmanaged[Cdecl]<void>)req("ffxvr_gesture_reset");
            _fn_tp_set       = (delegate* unmanaged[Cdecl]<float, float, float, float, void>)req("ffxvr_tp_set");
            _fn_view_smooth_set = (delegate* unmanaged[Cdecl]<float, float, void>)req("ffxvr_view_smooth_set");
            _fn_lowres_status = (delegate* unmanaged[Cdecl]<byte*, int, int>)req("ffxvr_lowres_status");
            _fn_xr_debug = (delegate* unmanaged[Cdecl]<int, int, void>)req("ffxvr_xr_debug");
            _fn_bones_capture = (delegate* unmanaged[Cdecl]<float*, int, int>)req("ffxvr_bones_capture");
            _fn_bone_wiggle  = (delegate* unmanaged[Cdecl]<int, int, float, void>)req("ffxvr_bone_wiggle");
            _fn_bone_status  = (delegate* unmanaged[Cdecl]<int*, int*, int>)req("ffxvr_bone_status");
            _fn_bones_seq    = (delegate* unmanaged[Cdecl]<int, int, void>)req("ffxvr_bones_seq");
            _fn_bones_seq_left = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_bones_seq_left");
            _fn_hands_set    = (delegate* unmanaged[Cdecl]<int, int, float, float, float, float, float, float, int, int, int, void>)req("ffxvr_hands_set");
            _fn_hands_status = (delegate* unmanaged[Cdecl]<uint*, nint>)req("ffxvr_hands_status");
            _fn_hands_calibrate = (delegate* unmanaged[Cdecl]<void>)req("ffxvr_hands_calibrate");
            _fn_hands_dump   = (delegate* unmanaged[Cdecl]<void>)req("ffxvr_hands_dump");
            _fn_hands_method = (delegate* unmanaged[Cdecl]<int, int, void>)req("ffxvr_hands_method");
            _fn_hands_side   = (delegate* unmanaged[Cdecl]<int, float, int>)req("ffxvr_hands_side");
            _fn_hands_cal_data  = (delegate* unmanaged[Cdecl]<float*, int, uint>)req("ffxvr_hands_cal_data");
            _fn_gesture_poll = (delegate* unmanaged[Cdecl]<float*, float*, int*, uint>)req("ffxvr_gesture_poll");
            _fn_fp_body_set  = (delegate* unmanaged[Cdecl]<int, uint, int, void>)req("ffxvr_fp_body_set");
            _fn_fp_parts     = (delegate* unmanaged[Cdecl]<uint>)req("ffxvr_fp_parts");
            _fn_msaa_status  = (delegate* unmanaged[Cdecl]<uint*, uint*, uint*, uint*, uint*, void>)req("ffxvr_msaa_status");
            _fn_patch_set    = (delegate* unmanaged[Cdecl]<int, int, int>)req("ffxvr_patch_set");
            _fn_patch_count  = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_patch_count");
            _fn_patch_name   = (delegate* unmanaged[Cdecl]<int, byte*>)req("ffxvr_patch_name");
            _fn_backdrop_draws = (delegate* unmanaged[Cdecl]<uint>)req("ffxvr_backdrop_draws");
            _fn_measure      = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_measure");
            _fn_on_ps_srvs   = (delegate* unmanaged[Cdecl]<uint, uint, nint*, void>)req("ffxvr_on_ps_srvs");
            _fn_on_clear_twin = (delegate* unmanaged[Cdecl]<nint, int, float*, uint, float, byte, void>)req("ffxvr_on_clear_twin");
            _fn_set_backbuffer = (delegate* unmanaged[Cdecl]<nint, void>)req("ffxvr_set_backbuffer");
            _fn_on_copy      = (delegate* unmanaged[Cdecl]<nint, uint, uint, uint, uint, nint, uint, nint, int, int>)req("ffxvr_on_copy");
            _fn_override_set = (delegate* unmanaged[Cdecl]<int, int, float, float, float, float, float, void>)req("ffxvr_override_set");
            _fn_override_applied = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_override_applied");
            _fn_override_mask    = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_override_mask");
            _fn_safe_read        = (delegate* unmanaged[Cdecl]<void*, void*, uint, int>)req("ffxvr_safe_read");
            _fn_project          = (delegate* unmanaged[Cdecl]<float, float, float, float*, int>)req("ffxvr_project");
            _fn_world_positions  = (delegate* unmanaged[Cdecl]<float*, int, int>)req("ffxvr_world_positions");
            _fn_diag7_set = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_diag7_set"); _fn_diag_line = (delegate* unmanaged[Cdecl]<byte*, void>)req("ffxvr_diag_line");
            _fn_main_note = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_main_note"); _fn_motion_cols = (delegate* unmanaged[Cdecl]<float*, int, void>)req("ffxvr_motion_cols");
            _fn_bb_resized = (delegate* unmanaged[Cdecl]<int, int, int, void>)req("ffxvr_bb_resized");
            _fn_cursor_game = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_cursor_game"); _fn_cursor_skip = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_cursor_skip");
            _fn_haptics_set = (delegate* unmanaged[Cdecl]<int, float, int>)req("ffxvr_haptics_set");
            _fn_game_callers = (delegate* unmanaged[Cdecl]<uint*, int, int>)req("ffxvr_game_callers"); _fn_va_stats = (delegate* unmanaged[Cdecl]<uint*, void>)req("ffxvr_va_stats");
            _fn_va_watch = (delegate* unmanaged[Cdecl]<int, int>)req("ffxvr_va_watch"); _fn_va_report = (delegate* unmanaged[Cdecl]<byte*, void>)req("ffxvr_va_report");
            _fn_va_regions = (delegate* unmanaged[Cdecl]<byte*, int, byte*, uint*, int>)req("ffxvr_va_regions");
            _fn_bt_set = (delegate* unmanaged[Cdecl]<int, int, void>)req("ffxvr_bt_set"); _fn_bt_status = (delegate* unmanaged[Cdecl]<byte*, int, int>)req("ffxvr_bt_status");   // ABI 15: battle transition
            _fn_prof_set = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_prof_set"); _fn_prof_mark = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_prof_mark"); _fn_prof_phase = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_prof_phase");   // ABI 17: load lock build
            _fn_prof_report = (delegate* unmanaged[Cdecl]<int, byte*, byte*, void>)req("ffxvr_prof_report"); _fn_lock_release_set = (delegate* unmanaged[Cdecl]<int, int>)req("ffxvr_lock_release_set"); _fn_lock_release_status = (delegate* unmanaged[Cdecl]<byte*, int, int>)req("ffxvr_lock_release_status");
            _fn_dream_set = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_dream_set"); _fn_dream_status = (delegate* unmanaged[Cdecl]<int*, int, int>)req("ffxvr_dream_status");   // ABI 18: dream overlay
            _fn_shadow_set = (delegate* unmanaged[Cdecl]<int, int, void>)req("ffxvr_shadow_set"); _fn_shadow_status = (delegate* unmanaged[Cdecl]<int*, int, int>)req("ffxvr_shadow_status");   // ABI 19: shadow camera
            _fn_uidepth_set = (delegate* unmanaged[Cdecl]<int, float, float, float, int, void>)req("ffxvr_uidepth_set"); _fn_uidepth_status = (delegate* unmanaged[Cdecl]<float*, int, int>)req("ffxvr_uidepth_status");   // ABI 20: adaptive UI panel
            _fn_haze_set = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_haze_set"); _fn_haze_status = (delegate* unmanaged[Cdecl]<int*, int, int>)req("ffxvr_haze_status");   // ABI 22: heat haze
            _fn_slow_set = (delegate* unmanaged[Cdecl]<int, int, void>)req("ffxvr_slow_set"); _fn_slow_status = (delegate* unmanaged[Cdecl]<int*, int, byte*>)req("ffxvr_slow_status");   // ABI 36: the slow-GPU guard (slow_guard.cs)
            _fn_load_info = (delegate* unmanaged[Cdecl]<int*, int, int>)req("ffxvr_load_info");   // ABI 23: loads build (the load: lines, the lock release per load kind); 24: 60 values
            _fn_sr_set = (delegate* unmanaged[Cdecl]<int, float, int, float, int, int, int, float, void>)req("ffxvr_sr_set"); _fn_sr_gate = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_sr_gate");   // ABI 25: stickrep
            _fn_sr_status = (delegate* unmanaged[Cdecl]<byte*, int, int>)req("ffxvr_sr_status"); _fn_turn_keys = (delegate* unmanaged[Cdecl]<int, void>)req("ffxvr_turn_keys");
            _fn_log_config = (delegate* unmanaged[Cdecl]<int, int, void>)req("ffxvr_log_config"); _fn_build_info = (delegate* unmanaged[Cdecl]<byte*>)req("ffxvr_build_info");   // ABI 26: release prep (logging.cs)
            _fn_pre_resolve = (delegate* unmanaged[Cdecl]<nint, uint, nint, uint, uint, void>)req("ffxvr_pre_resolve"); _fn_hands2_set = (delegate* unmanaged[Cdecl]<VrHandsCfg*, int, int>)req("ffxvr_hands2_set");   // ABI 27: VR hands v2 (hands.cs); ABI 38: returns whether the coming frame can draw them
            _fn_hands2_status = (delegate* unmanaged[Cdecl]<int*, int, byte*>)req("ffxvr_hands2_status"); _fn_hands2_cal = (delegate* unmanaged[Cdecl]<float*, int, uint>)req("ffxvr_hands2_cal");
            _fn_hands2_pack = (delegate* unmanaged[Cdecl]<VrHandsPackDesc*, int, void>)req("ffxvr_hands2_pack");   // ABI 28: VR hands v2 S2, the hand pack
            _fn_hands2_pack_error = (delegate* unmanaged[Cdecl]<byte*>)req("ffxvr_hands2_pack_error");   // ABI 29: VR hands v2 S3, user packs
            qc_bind(req);   // ABI 44: Quest-only controls (quest_ctl.cs, imgui_pad.cs)
            odg_bind(req);  // ABI 47: overdrive gestures (od_gest.cs)
            _fn_hands2_tap_set = (delegate* unmanaged[Cdecl]<VrTapCfg*, int, void>)req("ffxvr_hands2_tap_set"); _fn_hands2_tap_status = (delegate* unmanaged[Cdecl]<float*, int, byte*>)req("ffxvr_hands2_tap_status");   // ABI 30: the tap gesture (hands.cs)
            _fn_cursor_look = (delegate* unmanaged[Cdecl]<int, float, float, uint, uint, int, int>)req("ffxvr_cursor_look"); _fn_markers_kinds = (delegate* unmanaged[Cdecl]<int*, int, void>)req("ffxvr_markers_kinds"); _fn_cursor_atlas_dump = (delegate* unmanaged[Cdecl]<void>)req("ffxvr_cursor_atlas_dump");

            int abi = missing.Count == 0 && fn_abi != null ? fn_abi() : -1;
            if (missing.Count > 0 || abi != k_abi) {
                string why = missing.Count > 0 ? "missing " + string.Join(", ", missing.Count > 6 ? missing.GetRange(0, 6) : missing) + (missing.Count > 6 ? $" and {missing.Count - 6} more" : "") : $"abi {abi} vs {k_abi}";
                clear_native_fns();   // nothing may reach a mismatched DLL, even a future call site gated only on its pointer
                _native_mismatch = true; native_hard_fail($"ffxvr_native.dll does not match ffxvr.dll ({why}); reinstall the mod");
                _logger.Error($"Native helper mismatch: {why}. Nothing native will be called.");
                return;
            }
            _native_ok = true;
            log_native_loaded();   // release prep: the native version, and diag_verbose / the log cap before ffxvr_init (logging.cs)
            _native_status = "loaded, waiting for the render backend";
            _logger.Info("Native helper loaded. Waiting for ImGui's D3D11 backend to come up.");
        }
        catch (Exception ex) {
            _native_ok = false; clear_native_fns();
            _native_mismatch = true; native_hard_fail("load threw: " + ex.GetType().Name);
            _logger.Error($"Failed to load the native helper: {ex}");
        }
    }

    /// <summary>Every bound export back to null (after a mismatch or a failed load).</summary>
    private void clear_native_fns() {
        _fn_init = null; _fn_buffer_count = null; _fn_dump_count = null; _fn_hook_calls = null; _fn_begin_frame = null; _fn_frame_writes = null;
        _fn_get_camera = null; _fn_camera_reads = null; _fn_slot0_count = null; _fn_camera_changes = null; _fn_counter = null; _fn_on_draw = null;
        _fn_draw_ctx = null; _fn_draw_args = null; _fn_eye2_begin = null; _fn_eye2_end = null; _fn_stereo_set = null; _fn_xr_set = null;
        _fn_xr_recenter = null; _fn_xr_screen_set = null; _fn_ui_set = null; _fn_ui_draws = null; _fn_xr_pitch_follow = null; _fn_cuts_set = null;
        _fn_eye_jump_set = null; _fn_eye_event = null;   // ABI 44 (eye jumps)
        _fn_bt_tag_step = null; _fn_bt_tag_hold = null;   // ABI 46 (battle tag)
        _fn_dmg_walk = null; _fn_dmg_queue = null; _fn_dmg_set = null; _fn_dmg_cfg = null; _fn_dmg_skip = null; _fn_dmg_status = null;   // ABI 47 (3D damage numbers)
        _fn_turn_set = null; _fn_turn_count = null; _fn_perf_status = null; _fn_noimg_set = null; _fn_noimg_draws = null;
        _fn_bisect_set = null; _fn_scene_draws = null; _fn_blend_off = null; _fn_blend_restore = null; _fn_hist_set = null; _fn_hist_draws = null;
        _fn_move_set = null; _fn_move_status = null; _fn_cut_count = null; _fn_fp_set = null; _fn_player_set = null; _fn_player_head_set = null; _fn_ride_set = null; _fn_ride_stats = null; _fn_fp_self_set = null; _fn_fp_others_set = null; _fn_fp_ib_set = null; _fn_cpu_report = null; _fn_sg_install = null; _fn_sg_budget = null; _fn_sg_status = null;
        _fn_anchor_override = null; _fn_floor_set = null; _fn_fp_head_set = null; _fn_fp_hidden = null; _fn_markers_set = null; _fn_markers_3d = null;
        _fn_markers_game_art = null; _fn_markers_scale = null; _fn_ui_atlas_dump = null; _fn_motion_log = null; _fn_xr_screen_now = null; _fn_screen3d = null;
        _fn_screen3d_status = null; _fn_dump_seq = null; _fn_shutdown = null; _fn_xr_status = null; _fn_eye2_srv = null; _fn_eye2_draws = null; _fn_eye2_same_inputs = null; _fn_diag_keepalive = null; _fn_log_config = null; _fn_build_info = null;
        _fn_eye2_size = null; _fn_on_updatesub = null; _fn_on_rendertargets = null; _fn_on_rendertargets_uav = null; _fn_on_resolve = null; _fn_on_viewport = null;
        _fn_on_clear = null; _fn_frame_arm = null; _fn_ui_hide = null; _fn_ui_hidden_draws = null; _fn_ui_hide_cursor = null; _fn_ui_rect_stats = null;
        _fn_ui_hide_list = null; _fn_ui_hide_hits = null; _fn_draw_skip_ranges = null; _fn_ui_hide_pointers = null; _fn_ui_pointer_stats = null; _fn_blitz_overlay = null;
        _fn_ui_pick = null; _fn_ui_pick_result = null; _fn_slowmo = null; _fn_ui_hover = null; _fn_ui_hover_rects = null; _fn_dump_cb = null;
        _fn_census_arm = null; _fn_backdrop_set = null; _fn_cull_dist_set = null; _fn_cull_dist_get = null; _fn_cull_scene_set = null; _fn_cull_scene_get = null;
        _fn_msaa_set = null; _fn_vrpad_set = null; _fn_imgui_layer_set = null; _fn_imgui_draws = null; _fn_vrpad_status = null; _fn_haptic = null;
        _fn_gesture_set = null; _fn_gesture_reset = null; _fn_tp_set = null; _fn_view_smooth_set = null; _fn_lowres_status = null; _fn_xr_debug = null; _fn_bones_capture = null; _fn_bone_wiggle = null; _fn_bone_status = null;
        _fn_bones_seq = null; _fn_bones_seq_left = null; _fn_hands_set = null; _fn_hands_status = null; _fn_hands_calibrate = null; _fn_hands_dump = null;
        _fn_hands_method = null; _fn_hands_side = null; _fn_hands_cal_data = null; _fn_gesture_poll = null; _fn_fp_body_set = null; _fn_fp_parts = null;
        _fn_msaa_status = null; _fn_patch_set = null; _fn_patch_count = null; _fn_patch_name = null; _fn_backdrop_draws = null; _fn_measure = null;
        _fn_on_ps_srvs = null; _fn_on_clear_twin = null; _fn_set_backbuffer = null; _fn_on_copy = null; _fn_override_set = null; _fn_override_applied = null;
        _fn_override_mask = null; _fn_safe_read = null; _fn_project = null; _fn_world_positions = null;
        _fn_diag7_set = null; _fn_diag_line = null; _fn_main_note = null; _fn_motion_cols = null; _fn_bb_resized = null;
        _fn_cursor_game = null; _fn_cursor_skip = null; _fn_cursor_look = null; _fn_markers_kinds = null; _fn_cursor_atlas_dump = null;
        _fn_game_callers = null; _fn_va_stats = null; _fn_haptics_set = null; _fn_va_watch = null; _fn_va_report = null; _fn_va_regions = null;
        _fn_bt_set = null; _fn_bt_status = null;
        _fn_prof_set = null; _fn_prof_mark = null; _fn_prof_phase = null; _fn_prof_report = null; _fn_lock_release_set = null; _fn_lock_release_status = null;
        _fn_dream_set = null; _fn_dream_status = null;
        _fn_shadow_set = null; _fn_shadow_status = null;
        _fn_uidepth_set = null; _fn_uidepth_status = null;
        _fn_imgui_status = null;   // ABI 21 (saveui; _fn_imgui_layer_set is cleared above)
        _fn_haze_set = null; _fn_haze_status = null;   // ABI 22 (heat haze)
        _fn_slow_set = null; _fn_slow_status = null;   // ABI 36 (the slow-GPU guard)
        _fn_fmv3d_set = null; _fn_fmv3d_text = null;   // ABI 41 (FMVs in 3D)
        qc_clear();   // ABI 44 (Quest-only controls)
        odg_clear();  // ABI 47 (overdrive gestures)
        _fn_load_info = null;   // ABI 23 (loads)
        _fn_sr_set = null; _fn_sr_gate = null; _fn_sr_status = null; _fn_turn_keys = null;   // ABI 25 (stickrep)
        _fn_pre_resolve = null; _fn_hands2_set = null; _fn_hands2_status = null; _fn_hands2_cal = null; _fn_hands2_pack = null; _fn_hands2_pack_error = null;   // ABI 27, 28, 29 (VR hands v2)
        _fn_hands2_tap_set = null; _fn_hands2_tap_status = null;   // ABI 30 (the tap gesture)
    }
    /// <summary>A hard failure (no DLL, a mismatched pair, init failed): the window opens with the status. The forced
    /// open is not saved: the ini keeps the overlay_open from before until the player presses F8.</summary>
    private void native_hard_fail(string status) {
        _native_status = status; _native_failed = true;
        force_window_open();
    }
    private void force_window_open() {
        if (!_window_open_forced) { _window_open_pre_force = _window_open; _window_open_forced = true; }
        _window_open = true;
    }

    /// <summary>
    ///     Recovers the game's own D3D11 device and immediate context from ImGui's
    ///     backend data, then hands the context to the native helper.
    /// </summary>
    /// <remarks>
    ///     <c>BackendRendererUserData</c> points at <c>ImGui_ImplDX11_Data</c>, whose
    ///     first two fields are <c>ID3D11Device*</c> and <c>ID3D11DeviceContext*</c>.
    ///     Fahrenheit initialized that backend with the game's own objects, so this
    ///     is the context the game actually renders with - unlike a separately
    ///     created device, whose vtable shares nothing with it.
    /// </remarks>
    private void try_install_hooks() {
        if (_native_init_done || !_native_ok || _fn_init == null) return;

        nint backend = (nint)ImGui.GetIO().BackendRendererUserData;
        if (backend == 0) return;   // backend not up yet; try again next frame

        _native_init_done = true;

        nint* fields = (nint*)backend;
        _game_device  = fields[0];
        _game_context = fields[1];

        if (_game_context == 0) {
            native_hard_fail("ImGui backend gave no device context");
            _logger.Error("BackendRendererUserData held a null device context.");
            return;
        }

        int rc = _fn_init(_game_device, _game_context);

        _native_status = rc switch {
             0 => "hooked",
            -1 => "failed: null context passed",
            -2 => "failed: could not patch the vtable",
            -3 => "failed: context vtable unreadable",
             _ => $"failed: code {rc}",
        };

        if (rc == 0) _logger.Info($"Hooks installed on the game's context 0x{_game_context:X8} (device 0x{_game_device:X8}).");
        else         { _logger.Error($"Native init failed with code {rc} for context 0x{_game_context:X8}."); native_hard_fail(_native_status); return; }

        _native_ready = true;   // the vtable hooks are in and cannot be rolled back: run on, even if a detour below fails
        try { install_draw_detours(); }
        catch (Exception e) { _detour_status = "FAILED: " + e.GetType().Name + " (see core.log)"; _logger.Error($"Draw detours: {e}"); }
        if (_detour_status != "installed") force_window_open();   // shown in red at the top of the window
    }

    private void install_draw_detours() {
        nint* vt = *(nint**)_game_context;
        _a_drawindexed = vt[12];
        _a_draw        = vt[13];
        _a_drawidxinst = vt[20];
        _a_drawinst    = vt[21];
        _a_updatesub   = vt[48];
        _a_omrt = vt[33]; _a_vp = vt[44]; _a_clr = vt[50]; _a_clrd = vt[53]; _a_pssrv = vt[8]; _a_copyreg = vt[46]; _a_copy = vt[47]; _a_resolve = vt[57]; _a_omrtuav = vt[34];

        _h_drawindexed = h_drawindexed;
        _h_draw        = h_draw;
        _h_drawidxinst = h_drawidxinst;
        _h_drawinst    = h_drawinst;
        _h_updatesub   = h_updatesub;
        _h_omrt = h_omrt; _h_vp = h_vp; _h_clr = h_clr; _h_clrd = h_clrd; _h_pssrv = h_pssrv; _h_copy = h_copy; _h_copyreg = h_copyreg; _h_resolve = h_resolve; _h_omrtuav = h_omrtuav;

        bool ok = true;
        diag7_insert_begin();   // build 7 (B7-1): no main-thread detour resolves its original while these insertions write Fahrenheit's table
        try {
        ok &= new FhCallHandle<d_DrawIndexed>         (new FhCallLocation((void*)_a_drawindexed)).hook(this, _h_drawindexed);
        ok &= new FhCallHandle<d_Draw>                (new FhCallLocation((void*)_a_draw))       .hook(this, _h_draw);
        ok &= new FhCallHandle<d_DrawIndexedInstanced>(new FhCallLocation((void*)_a_drawidxinst)).hook(this, _h_drawidxinst);
        ok &= new FhCallHandle<d_DrawInstanced>       (new FhCallLocation((void*)_a_drawinst))   .hook(this, _h_drawinst);
        ok &= new FhCallHandle<d_UpdateSubresource>   (new FhCallLocation((void*)_a_updatesub))  .hook(this, _h_updatesub);
        ok &= new FhCallHandle<d_OMSetRenderTargets>  (new FhCallLocation((void*)_a_omrt)).hook(this, _h_omrt);
        ok &= new FhCallHandle<d_RSSetViewports>      (new FhCallLocation((void*)_a_vp))  .hook(this, _h_vp);
        ok &= new FhCallHandle<d_ClearRenderTargetView>(new FhCallLocation((void*)_a_clr)).hook(this, _h_clr);
        ok &= new FhCallHandle<d_ClearDepthStencilView>(new FhCallLocation((void*)_a_clrd)).hook(this, _h_clrd);
        ok &= new FhCallHandle<d_PSSetShaderResources>(new FhCallLocation((void*)_a_pssrv)).hook(this, _h_pssrv);
        ok &= new FhCallHandle<d_CopyResource>(new FhCallLocation((void*)_a_copy)).hook(this, _h_copy);
        ok &= new FhCallHandle<d_CopySubresourceRegion>(new FhCallLocation((void*)_a_copyreg)).hook(this, _h_copyreg);
        ok &= new FhCallHandle<d_ResolveSubresource>(new FhCallLocation((void*)_a_resolve)).hook(this, _h_resolve);
        ok &= new FhCallHandle<d_OMSetRenderTargetsAndUAVs>(new FhCallLocation((void*)_a_omrtuav)).hook(this, _h_omrtuav);

        // The headset session must be torn down while the D3D device is still
        // alive; the runtime crashes if it is left to DLL unload at exit.
        nint k32 = NativeLibrary.Load("kernel32.dll");
        _a_exitprocess = NativeLibrary.GetExport(k32, "ExitProcess");
        _h_exitprocess = h_exitprocess;
        ok &= new FhCallHandle<d_ExitProcess>(new FhCallLocation((void*)_a_exitprocess)).hook(this, _h_exitprocess);
        }
        finally { diag7_insert_end(); }

        _detour_status = ok ? "installed" : "FAILED (see core.log)";
        _logger.Info($"Draw detours {_detour_status}: DrawIndexed 0x{_a_drawindexed:X8}, Draw 0x{_a_draw:X8}, "
                   + $"DrawIndexedInstanced 0x{_a_drawidxinst:X8}, DrawInstanced 0x{_a_drawinst:X8}, UpdateSubresource 0x{_a_updatesub:X8}");
    }

    // Each detour reports to the native side, then continues down the chain.
    // The chain pointer is resolved once and cached; resolving it per call
    // would cost a delegate marshal on every draw.

    // Inline detours fire for every D3D11 context in the process (the OpenXR
    // runtime has its own). Only the game's immediate context is ours.
    private bool ours(nint ctx) => ctx == _game_context;

    private void h_drawindexed(nint ctx, uint n, uint start, int base_vertex) {
        if (_fn_draw_ctx != null) _fn_draw_ctx(ctx);
        if (_fn_draw_args != null) _fn_draw_args(1, n, start, base_vertex);
        int r = (ours(ctx) && _fn_on_draw != null) ? _fn_on_draw(1) : 0;
        _o_drawindexed ??= new FhCallHandle<d_DrawIndexed>(new FhCallLocation((void*)_a_drawindexed)).chain_from(_h_drawindexed!).fnptr;
        if (r < 0) return;                          // hidden (the player's own body in first person)
        int eye2 = r & 15; bool noblend = (r & 16) != 0 && _fn_blend_off != null;
        // partial hide: index ranges (batched 2D sprites: hide list, pointer rule) are left out, the rest drawn in pieces between them (R39)
        uint* rg = stackalloc uint[16]; int nr = 0;
        if ((r & 0x20000) != 0 && _fn_draw_skip_ranges != null) {
            nr = _fn_draw_skip_ranges(rg, 8);
            if (nr < 0 || nr > 8) nr = 0;
            uint at = 0;   // sorted, non-overlapping, inside the draw; anything else -> drawn whole
            for (int i = 0; i < nr; i++) { uint f = rg[i*2], c = rg[i*2+1]; if (c == 0 || f < at || f > n || c > n - f) { nr = 0; break; } at = f + c; }
        }
        if (noblend) _fn_blend_off();
        draw_pieces(ctx, n, start, base_vertex, nr, rg);
        for (int k = 0; k < eye2 && _fn_eye2_begin != null; k++) if (_fn_eye2_begin(k) != 0) { draw_pieces(ctx, n, start, base_vertex, nr, rg); _fn_eye2_end(); }
        if (noblend) _fn_blend_restore();
    }
    // Outlines, on the monitor, the UI quads the native side collected this frame (hover highlight), and
    // hands it the mouse position for the next frame. UI units -> pixels: centred 1920x1080 layout.
    private void draw_hover_highlight() {
        if (_fn_ui_hover == null || _fn_ui_hover_rects == null) return;
        Vector2 ds = ImGui.GetIO().DisplaySize; Vector2 m = ImGui.GetMousePos();
        _fn_ui_hover(ds.X > 0 ? m.X / ds.X : 0f, ds.Y > 0 ? m.Y / ds.Y : 0f, _hover_mode);
        if (_hover_mode == 0 || ds.X <= 0 || ds.Y <= 0) return;
        float* r = stackalloc float[512 * 7]; int n = _fn_ui_hover_rects(r, 512);
        ImDrawListPtr dl = ImGui.GetForegroundDrawList();
        float sx = ds.X / 1920f, sy = ds.Y / 1080f;
        for (int i = 0; i < n; i++) {
            float x0 = (r[i*7] + 960f) * sx, y0 = (r[i*7+1] + 540f) * sy, x1 = (r[i*7+2] + 960f) * sx, y1 = (r[i*7+3] + 540f) * sy;
            int flags = (int)r[i*7+6]; bool hit = (flags & 1) != 0, ptr = (flags & 2) != 0, actor = (flags & 4) != 0;
            if (actor) {   // where a character's root projects through the game camera
                float cx = 0.5f * (x0 + x1), cy = 0.5f * (y0 + y1);
                dl.AddLine(new Vector2(cx - 12, cy), new Vector2(cx + 12, cy), 0xFF00FF00u, 2f); dl.AddLine(new Vector2(cx, cy - 12), new Vector2(cx, cy + 12), 0xFF00FF00u, 2f);
                continue;
            }
            uint col = hit ? 0xFF00E5FFu : ptr ? 0xFF0000FFu : 0x80FFFFFFu;   // ABGR: yellow under the mouse, red = pointer rule, white other
            dl.AddRect(new Vector2(x0, y0), new Vector2(x1, y1), col, 0f, (hit || ptr) ? 3f : 1f, ImDrawFlags.None);
            if (hit || ptr) {
                dl.AddRectFilled(new Vector2(x0, y0), new Vector2(x1, y1), hit ? 0x3000E5FFu : 0x300000FFu);
                string tex = r[i*7+4] == 0 ? "textureless" : $"{r[i*7+4]:F0}x{r[i*7+5]:F0}";
                dl.AddText(new Vector2(x0, y1 + 2), col, $"{r[i*7+2] - r[i*7]:F0}x{r[i*7+3] - r[i*7+1]:F0} {tex}{(ptr ? " [pointer]" : "")}");
            }
        }
    }
    // The draw minus nr skipped (first, count) ranges (validated by the caller): the gaps between them, in order.
    private void draw_pieces(nint ctx, uint n, uint start, int base_vertex, int nr, uint* rg) {
        if (nr <= 0) { _o_drawindexed!(ctx, n, start, base_vertex); return; }
        uint at = 0;
        for (int i = 0; i < nr; i++) { uint f = rg[i*2]; if (f > at) _o_drawindexed!(ctx, f - at, start + at, base_vertex); at = f + rg[i*2+1]; }
        if (at < n) _o_drawindexed!(ctx, n - at, start + at, base_vertex);
    }

    private void h_draw(nint ctx, uint n, uint start) {
        if (_fn_draw_ctx != null) _fn_draw_ctx(ctx);
        if (_fn_draw_args != null) _fn_draw_args(2, n, start, 0);
        int r = (ours(ctx) && _fn_on_draw != null) ? _fn_on_draw(2) : 0;
        _o_draw ??= new FhCallHandle<d_Draw>(new FhCallLocation((void*)_a_draw)).chain_from(_h_draw!).fnptr;
        if (r < 0) return;
        int eye2 = r & 15; bool noblend = (r & 16) != 0 && _fn_blend_off != null;
        if (noblend) _fn_blend_off();
        _o_draw!(ctx, n, start);
        for (int k = 0; k < eye2 && _fn_eye2_begin != null; k++) if (_fn_eye2_begin(k) != 0) { _o_draw!(ctx, n, start); _fn_eye2_end(); }
        if (noblend) _fn_blend_restore();
    }

    private void h_drawidxinst(nint ctx, uint a, uint b, uint c, int d, uint e) {
        if (ours(ctx) && _fn_on_draw != null) _fn_on_draw(3);   // instanced paths unused by this game
        _o_drawidxinst ??= new FhCallHandle<d_DrawIndexedInstanced>(new FhCallLocation((void*)_a_drawidxinst)).chain_from(_h_drawidxinst!).fnptr;
        _o_drawidxinst!(ctx, a, b, c, d, e);
    }

    private void h_drawinst(nint ctx, uint a, uint b, uint c, uint d) {
        if (ours(ctx) && _fn_on_draw != null) _fn_on_draw(4);
        _o_drawinst ??= new FhCallHandle<d_DrawInstanced>(new FhCallLocation((void*)_a_drawinst)).chain_from(_h_drawinst!).fnptr;
        _o_drawinst!(ctx, a, b, c, d);
    }

    private void h_omrt(nint ctx, uint n, nint* rtvs, nint dsv) {
        if (ours(ctx) && _fn_on_rendertargets != null) _fn_on_rendertargets(n, rtvs, dsv);
        _o_omrt ??= new FhCallHandle<d_OMSetRenderTargets>(new FhCallLocation((void*)_a_omrt)).chain_from(_h_omrt!).fnptr;
        _o_omrt!(ctx, n, rtvs, dsv);
    }

    private void h_vp(nint ctx, uint n, float* vps) {
        if (ours(ctx) && _fn_on_viewport != null && n > 0 && vps != null) _fn_on_viewport(vps[2], vps[3], vps[0], vps[1]);
        _o_vp ??= new FhCallHandle<d_RSSetViewports>(new FhCallLocation((void*)_a_vp)).chain_from(_h_vp!).fnptr;
        _o_vp!(ctx, n, vps);
    }

    private void h_copy(nint ctx, nint dst, nint src) {
        _o_copy ??= new FhCallHandle<d_CopyResource>(new FhCallLocation((void*)_a_copy)).chain_from(_h_copy!).fnptr;
        _o_copy!(ctx, dst, src);
        if (ours(ctx) && _fn_on_copy != null) _fn_on_copy(dst, 0, 0, 0, 0, src, 0, 0, 1);
    }

    private void h_exitprocess(uint code) {
        settings_flush();   // R21: an unsaved change (or one still being debounced) reaches the file; a hand edit still wins
        try { if (_fn_shutdown != null) _fn_shutdown(); } catch { }
        _o_exitprocess ??= new FhCallHandle<d_ExitProcess>(new FhCallLocation((void*)_a_exitprocess)).chain_from(_h_exitprocess!).fnptr;
        _o_exitprocess!(code);
    }

    private void h_resolve(nint ctx, nint dst, uint dst_sub, nint src, uint src_sub, uint fmt) {
        _o_resolve ??= new FhCallHandle<d_ResolveSubresource>(new FhCallLocation((void*)_a_resolve)).chain_from(_h_resolve!).fnptr;
        if (ours(ctx) && _fn_pre_resolve != null) _fn_pre_resolve(dst, dst_sub, src, src_sub, fmt);   // VR hands v2: drawn into the scene target before the game resolves it
        _o_resolve!(ctx, dst, dst_sub, src, src_sub, fmt);
        if (ours(ctx) && _fn_on_resolve != null) _fn_on_resolve(dst, dst_sub, src, src_sub, fmt);
    }

    private void h_omrtuav(nint ctx, uint n, nint* rtvs, nint dsv, uint uav_start, uint uav_n, nint* uavs, uint* counts) {
        if (ours(ctx) && _fn_on_rendertargets_uav != null) _fn_on_rendertargets_uav(n, rtvs, dsv);
        _o_omrtuav ??= new FhCallHandle<d_OMSetRenderTargetsAndUAVs>(new FhCallLocation((void*)_a_omrtuav)).chain_from(_h_omrtuav!).fnptr;
        _o_omrtuav!(ctx, n, rtvs, dsv, uav_start, uav_n, uavs, counts);
    }

    private void h_copyreg(nint ctx, nint dst, uint dst_sub, uint x, uint y, uint z, nint src, uint src_sub, nint box) {
        _o_copyreg ??= new FhCallHandle<d_CopySubresourceRegion>(new FhCallLocation((void*)_a_copyreg)).chain_from(_h_copyreg!).fnptr;
        _o_copyreg!(ctx, dst, dst_sub, x, y, z, src, src_sub, box);
        if (ours(ctx) && _fn_on_copy != null) _fn_on_copy(dst, dst_sub, x, y, z, src, src_sub, box, 0);
    }

    private void h_pssrv(nint ctx, uint start, uint n, nint* views) {
        if (ours(ctx) && _fn_on_ps_srvs != null) _fn_on_ps_srvs(start, n, views);
        _o_pssrv ??= new FhCallHandle<d_PSSetShaderResources>(new FhCallLocation((void*)_a_pssrv)).chain_from(_h_pssrv!).fnptr;
        _o_pssrv!(ctx, start, n, views);
    }

    private void h_clr(nint ctx, nint rtv, float* color) {
        if (ours(ctx) && _fn_on_clear != null) _fn_on_clear(rtv, 0);
        if (ours(ctx) && _fn_on_clear_twin != null) _fn_on_clear_twin(rtv, 0, color, 0, 0f, 0);
        _o_clr ??= new FhCallHandle<d_ClearRenderTargetView>(new FhCallLocation((void*)_a_clr)).chain_from(_h_clr!).fnptr;
        _o_clr!(ctx, rtv, color);
    }

    private void h_clrd(nint ctx, nint dsv, uint flags, float depth, byte stencil) {
        if (ours(ctx) && _fn_on_clear != null) _fn_on_clear(dsv, 1);
        if (ours(ctx) && _fn_on_clear_twin != null) _fn_on_clear_twin(dsv, 1, null, flags, depth, stencil);
        _o_clrd ??= new FhCallHandle<d_ClearDepthStencilView>(new FhCallLocation((void*)_a_clrd)).chain_from(_h_clrd!).fnptr;
        _o_clrd!(ctx, dsv, flags, depth, stencil);
    }

    private void h_updatesub(nint ctx, nint resource, uint sub, nint box, nint src, uint row_pitch, uint depth_pitch) {
        // The native side records the game's camera and, when the override is
        // on, hands back a patched private copy to upload in its place.
        if (ours(ctx) && _fn_on_updatesub != null) src = _fn_on_updatesub(resource, box, src);
        _o_updatesub ??= new FhCallHandle<d_UpdateSubresource>(new FhCallLocation((void*)_a_updatesub)).chain_from(_h_updatesub!).fnptr;
        _o_updatesub!(ctx, resource, sub, box, src, row_pitch, depth_pitch);
    }

    /// <summary>
    ///     Shows the camera read live out of the game's own constant buffer.
    ///     Offsets came from a RenderDoc capture; see docs/camera-found.md.
    ///     Translation sits in the last column, so the world position of the
    ///     camera is -R^T * t.
    /// </summary>
    // -- settings file ---------------------------------------------------------
    //
    // ffxvr.ini next to the mod: what a player sets in the main window
    // persists, so the overlay is not needed once things are configured.

    private string _ini_path = "";
    private string _ini_last = "";
    private bool _headset_autostart = false; private int _start_view = 1; private bool _overlay_at_start = true, _overlay_at_start_loaded = true;   // start_view: 0 seat, 1 first person, 3 third person, 2 diorama

    private string hide_list_str() {
        var ci = System.Globalization.CultureInfo.InvariantCulture; var parts = new List<string>();
        foreach (float[] e in _hide_list) parts.Add(string.Join(",", Array.ConvertAll(e, x => x.ToString("0.#", ci))));
        return string.Join(";", parts);
    }
    private void hide_list_parse(string v) {
        var ci = System.Globalization.CultureInfo.InvariantCulture; var list = new List<float[]>();
        foreach (string part in v.Split(';', StringSplitOptions.RemoveEmptyEntries)) {
            string[] f = part.Split(','); if (f.Length != 6) continue;   // (7-field entries from the first build are dropped)
            float[] e = new float[6]; bool ok = true; for (int k = 0; k < 6; k++) if (!float.TryParse(f[k], System.Globalization.NumberStyles.Float, ci, out e[k])) ok = false;
            if (ok && !list.Exists(x => x[0] == e[0] && x[1] == e[1] && x[2] == e[2] && x[3] == e[3] && x[4] == e[4] && x[5] == e[5])) list.Add(e);
        }
        _hide_list.Clear(); _hide_list.AddRange(list.Count > 0 ? list : hide_list_defaults()); _hide_list_dirty = true;
    }

    // Settings lifecycle (build 8, G1-final R21/R27-R33): settings_load (init: read with retries, validated parse, launch
    // effects), settings_reload_check (every 1 s of _now: a hand edit), settings_save_tick (every 0.5 s: write once the text
    // has been stable for 1 s), settings_write (temp file + flush + move, never over a hand edit), settings_flush (exit).
    private static readonly System.Globalization.CultureInfo CI = System.Globalization.CultureInfo.InvariantCulture;
    private bool _settings_ok = false;   // a load succeeded (or there was no file): saving is allowed
    private double _now;                 // ImGui.GetTime(), once per frame: the clock of every settings timer (R33)
    private readonly object _ini_lock = new();   // settings_write runs on the render thread (save tick) and on ExitProcess's thread (flush)
    private DateTime _ini_mtime_seen = DateTime.MinValue; private long _ini_len_seen = -1;   // the file as we last read or wrote it (-1: absent)
    private string? _ini_pending; private double _ini_pending_at, _ini_next_snap, _ini_retry_at, _ini_next_check;
    private int _ini_fail_n, _ini_read_fail_n, _ini_reload_fault_n, _ini_bak_fail_n, _ini_loaded_keys; private volatile bool _ini_exiting; private bool _ini_backed_up; private string _ini_note = "";
    // LD2-5: the ini's revision. An ini without ini_rev (or older) was written before the loads build: its load_lock_release
    // (always 0, LL-2's test rule) and heat_haze (the haze build's 1) carry no choice of the player's, so the launch read
    // replaces them with this build's defaults once; the next save writes ini_rev. The diag_* keys are left as they are.
    // Rev 2 (release prep, logging.cs): diag_verbose is new and defaults to 0; an older ini without it reads 1, the periodic
    // lines every build before it wrote, so an existing (developer's) setup logs as before.
    private const int k_ini_rev = 2;           // 1: the loads build; 2: release prep (diag_verbose)
    private int _ini_rev_read;                 // what the file said (0: absent)
    private string? _ini_diag_note;            // the migration / diag_* note, written to ffxvr_diag.log at the first ready frame
    private bool _diag_actors = false;         // ini diag_actors (default 0): the "actors:" line every 10 s in Fahrenheit's log (LD2-8)
    private readonly List<string> _ini_bad = new();   // the last parse's ignored values, for its one log line
    private readonly List<string> _ini_clamped = new();   // ... and the values pulled into range (B8-13)
    private string _ini_disk = "";   // the file's text as we last read or wrote it: a reload applies only the keys the hand edit changed
    private static Dictionary<string, string> ini_kv(string text) {
        var d = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (string raw in text.Split('\n')) { string line = raw.Trim(); int eq = line.IndexOf('='); if (line.Length == 0 || line[0] == '#' || eq < 0) continue; d[line[..eq].Trim()] = line[(eq + 1)..].Trim(); }
        return d;
    }

    // -- validated parsing (R27, R32): a bad value keeps the current one and is listed; numbers are invariant culture,
    // clamped; U+2212 (the minus sign some regional formats write) reads as '-'.
    private static string num(string v) => v.Replace('\u2212', '-').Trim();
    private void pf(string k, string v, ref float f, float lo, float hi) {
        if (float.TryParse(num(v), System.Globalization.NumberStyles.Float, CI, out float t) && float.IsFinite(t)) { f = Math.Clamp(t, lo, hi); if (f != t) _ini_clamped.Add($"{k}={v}->{f.ToString(CI)}"); }
        else _ini_bad.Add($"{k}='{v}'");
    }
    private void pi(string k, string v, ref int i, int lo, int hi) {
        if (int.TryParse(num(v), System.Globalization.NumberStyles.Integer, CI, out int t)) { i = Math.Clamp(t, lo, hi); if (i != t) _ini_clamped.Add($"{k}={v}->{i.ToString(CI)}"); }
        else _ini_bad.Add($"{k}='{v}'");
    }
    private void pe(string k, string v, ref int i, params int[] ok) { if (int.TryParse(num(v), System.Globalization.NumberStyles.Integer, CI, out int t) && Array.IndexOf(ok, t) >= 0) i = t; else _ini_bad.Add($"{k}='{v}'"); }
    private static bool try_bool(string v, out bool b) {
        switch (v.Trim().ToLowerInvariant()) {
            case "1": case "true": case "yes": case "on": b = true; return true;
            case "0": case "false": case "no": case "off": b = false; return true;
            default: b = false; return false;
        }
    }
    private void pb(string k, string v, ref bool b) { if (try_bool(v, out bool t)) b = t; else _ini_bad.Add($"{k}='{v}'"); }
    /// <summary>A two-word setting (cursor_source=game|legacy, cursor_look=game|mod): b = the first word.</summary>
    private void pw(string k, string v, ref bool b, string yes, string no) {
        string t = v.Trim();
        if (t.Equals(yes, StringComparison.OrdinalIgnoreCase)) b = true; else if (t.Equals(no, StringComparison.OrdinalIgnoreCase)) b = false; else _ini_bad.Add($"{k}='{v}'");
    }
    private string bad_suffix() => (_ini_bad.Count > 0 ? "; ignored bad values (kept the previous value): " + string.Join(", ", _ini_bad) : "")
                                 + (_ini_clamped.Count > 0 ? "; clamped to the allowed range: " + string.Join(", ", _ini_clamped) : "");

    /// <summary>The settings in the file, validated (R27). No launch effects (R28): world_scale goes to the scale of the
    /// non-life-size views, whatever the current view. Returns the number of known keys read.</summary>
    private int settings_parse(string[] lines) {
        _ini_bad.Clear(); _ini_clamped.Clear(); int known = 0; float ws = float.NaN;
        foreach (string raw in lines) {
            string line = raw.Trim();
            int eq = line.IndexOf('=');
            if (line.Length == 0 || line[0] == '#' || eq < 0) continue;
            string k = line[..eq].Trim(), v = line[(eq + 1)..].Trim();
            known++;
            switch (k) {
                case "headset_autostart": pb(k, v, ref _headset_autostart); break;
                case "start_view":        pe(k, v, ref _start_view, 0, 1, 2, 3); break;
                case "overlay_at_start":  pb(k, v, ref _overlay_at_start); break;
                case "world_scale":       { float t = float.NaN; pf(k, v, ref t, 10f, 300f); if (!float.IsNaN(t)) ws = t; } break;
                case "head_position":     pb(k, v, ref _xr_translate); break;
                case "seat":              pb(k, v, ref _xr_seat); break;
                case "headset_fov":       pb(k, v, ref _xr_fov); break;
                case "screen_mode":       pe(k, v, ref _xr_screen_mode, 0, 1, 2); break;
                case "stereo_screen_blitz": pb(k, v, ref _s3_blitz); break;
                case "stereo_screen_all": pb(k, v, ref _s3_all); break;
                case "stereo_screen_depth": pf(k, v, ref _s3_depth, 0f, 4f); break;
                case "stereo_screen_plane": pf(k, v, ref _s3_plane, 10f, 5000f); break;
                case "stereo_screen_auto": pb(k, v, ref _s3_auto); break;
                case "stereo_screen_max_pop": pf(k, v, ref _s3_max_pop, 0f, 6f); break;
                case "backdrop_mode":     pe(k, v, ref _backdrop_mode, 0, 1); break;
                case "cull_dist":         pb(k, v, ref _cull_dist); break;
                case "cull_scene":        pb(k, v, ref _cull_scene); break;
                case "msaa":              pe(k, v, ref _msaa, 0, 2, 4); break;
                case "vrpad":             pb(k, v, ref _vrpad); break;
                case "haptics":           pb(k, v, ref _haptics); break;                  // the game's rumble on the Quest controllers
                case "haptics_gain":      pf(k, v, ref _haptics_gain, 0f, 1f); break;
                case "slow_guard":        pb(k, v, ref _slow_guard); break;               // the slow-GPU guard (default 1; slow_guard.cs)
                case "slow_guard_restart": pb(k, v, ref _slow_guard_restart); break;      // ... its last step, the VR session restart (default 1)
                case "timer_fix":         pb(k, v, ref _timer_fix); break;                // grid-timer: 1 ms timer + no timer throttling (default 0; cpu_report.cs)
                case "limiter_probe":     pb(k, v, ref _limiter_probe); break;            // grid-timer: time the game's frame limiter for the cpu: line (default 1)
                case "fast_reads":        pb(k, v, ref _fast_reads); break;               // vq-lean: the guarded reads' per-frame run cache (default 1; cpu_report.cs)
                case "imgui_layer":       pb(k, v, ref _imgui_layer); break;
                case "fh_menu_on_screen": pb(k, v, ref _fh_menu_on_screen); break;            // Fahrenheit's save screen on the virtual screen (default 1; applies at once)
                case "fh_dpi_fix":        pb(k, v, ref _fh_dpi_fix); break;                   // FH-1: Fahrenheit's ImGui text ignores the Windows display scale, as Fahrenheit #376 (default 1; applies at once)
                case "overlay_text_scale": pf(k, v, ref _overlay_text_scale, 0.5f, 2f); _overlay_text_scale = text_scale_step(_overlay_text_scale); break; // FH-2: our overlay's text size, x the default 20 px x Windows scale (default 1, steps of 0.05; applies at once)
                case "gestures":          pb(k, v, ref _gestures); break;
                case "gest_tilt":         pf(k, v, ref _gest_tilt, -2f, 2f); break;
                case "fp_body_mode":      pe(k, v, ref _fp_body_mode, 0, 1, 2); break;
                case "battle_anchor":     pb(k, v, ref _battle_anchor); break;
                case "ui_hide":           pi(k, v, ref _ui_hide, 0, 5); break;
                case "hide_cursor":       pb(k, v, ref _hide_cursor); break;
                case "hide_pointers":     pb(k, v, ref _hide_pointers); break;
                case "blitz_overlay":     pb(k, v, ref _blitz_overlay); break;
                case "markers_game_art":  pb(k, v, ref _markers_game_art); break;
                case "marker_scale":      pf(k, v, ref _marker_scale, 0.2f, 5f); break;
                case "hide_list":         hide_list_parse(v); break;
                case "battle_follow":     pb(k, v, ref _battle_follow); break;
                case "battle_view":       pi(k, v, ref _battle_view, -1, 3); break;
                case "bt_tag": case "bt_tag_dist": btg_parse(k, v); break;   // battle tag (bt_tag.cs)
                case "dmg3d": case "dmg3d_scale": case "dmg3d_lift": case "dmg3d_hide2d": dmg_parse(k, v); break;   // 3D damage numbers (dmg3d.cs)
                case "diorama_floor":     pf(k, v, ref _diorama_floor, 0f, 40f); break;
                case "fp_head_anchor": {  // 0/1/2; an old bool-style value still means 2 (on) or 0 (off)
                    if (int.TryParse(num(v), System.Globalization.NumberStyles.Integer, CI, out int fha)) { if (fha >= 0 && fha <= 2) _fp_head_anchor = fha; else _ini_bad.Add($"{k}='{v}'"); }
                    else if (try_bool(v, out bool fhb)) _fp_head_anchor = fhb ? 2 : 0;
                    else _ini_bad.Add($"{k}='{v}'");
                } break;
                case "fp_head_up":        pf(k, v, ref _fp_head_up, -5f, 10f); break;
                case "fp_head_reach":     pf(k, v, ref _fp_head_reach, 0f, 40f); break;
                case "fp_head_smooth":    pf(k, v, ref _fp_head_smooth, 0.05f, 1f); break;
                case "fp_head_fwd":       pf(k, v, ref _fp_head_fwd, 0f, 6f); break;
                case "fp_head_parts":     _fp_head_parts = v; break;
                case "screen_width":      pf(k, v, ref _xr_screen_width, 1f, 8f); break;
                case "screen_distance":   pf(k, v, ref _xr_screen_dist, 1f, 6f); break;
                case "overlay_open":      pb(k, v, ref _window_open); break;
                case "eye2_preview":      pb(k, v, ref _eye2_preview); break;
                case "ui_panel":          pb(k, v, ref _ui_on); break;
                case "ui_distance":       pf(k, v, ref _ui_dist, 0.5f, 4f); break;
                case "ui_width":          pf(k, v, ref _ui_width, 0.5f, 4f); break;
                case "ui_adaptive":       pb(k, v, ref _ui_adaptive); break;                          // the UI panel comes in front of near objects (default 1; applies at once)
                case "ui_adaptive_min":   pf(k, v, ref _ui_adaptive_min, 0.3f, 2f); break;            // its nearest distance (m)
                case "ui_adaptive_margin": pf(k, v, ref _ui_adaptive_margin, 0.5f, 1f); break;        // the panel goes to the nearest depth times this
                case "ui_backing":        pf(k, v, ref _ui_backing, 0f, 1f); break;                   // the dark halo behind the UI: opacity, 0 = off
                case "dream_overlay":     pi(k, v, ref _dream_overlay, 0, 2); break;   // the game's dream filter in the headset: 1 hidden (default), 2 both eyes, 0 on the panel (applies at once)
                case "pitch_follow":      pf(k, v, ref _xr_pitch_follow, 0f, 1f); break;
                case "seat_height":       pf(k, v, ref _ovr_up, -5000f, 5000f); break;
                case "seat_forward":      pf(k, v, ref _ovr_forward, -5000f, 5000f); break;
                case "cut_recentre":      pb(k, v, ref _cut_recentre); break;
                case "cut_blink":         pb(k, v, ref _cut_blink); break;
                case "eye_jump_blink": case "eye_jump_units": case "battle_face": ej_parse(k, v); break;   // eye jumps (eye_jump.cs)
                case "turn_stick":        pb(k, v, ref _turn_stick); break;
                case "turn_mode":         pi(k, v, ref _turn_mode, 0, 1); break;
                case "turn_snap":         pf(k, v, ref _turn_snap, 5f, 90f); break;
                case "turn_rate":         pf(k, v, ref _turn_rate, 10f, 360f); break;
                case "turn_blink":        pb(k, v, ref _turn_blink); break;
                case "stick_reproject":   pe(k, v, ref _stick_reproject, 0, 1, 2); break;          // stickrep: 0 off, 1 first person + seat (default), 2 also third person + diorama (at once)
                case "stick_reproject_margin_ms": pf(k, v, ref _sr_margin_ms, 0.5f, 6f); break;    // stickrep: the extra frame's lead on the slot's deadline
                case "stick_reproject_predict": pb(k, v, ref _sr_predict); break;                  // stickrep: an extra first when a game frame cannot make the open slot
                case "stick_overscan":    pf(k, v, ref _stick_overscan, 0f, 4f); break;            // stickrep: degrees per side the rendered view is widened (default 0)
                case "keepalive":         pb(k, v, ref _keepalive); break;                         // stickrep: the loading keepalive alone, unturned (with stick_reproject=0)
                case "keepalive_mt_protect": { bool ignored = false; pb(k, v, ref ignored); } break;   // SR-X2: accepted, ignored (never on: see sr_mt_init)
                case "move_remap":        pb(k, v, ref _move_remap); break;
                case "move_invert":       pb(k, v, ref _move_invert); break;
                case "fp_actor": {   // FP-1: a comma list of model names, best first; the old single default "c101" becomes the new list
                    string fa = v.Trim();
                    bool ok = fa.Length > 0 && fa.Length <= 63; int nn = 0;
                    if (ok) foreach (string t in fa.Split(',')) { string n = t.Trim(); if (n.Length == 0 || n.Length > 15) continue; nn++; }   // FP-R3: empty and over-long tokens skipped, as fp_names() does
                    if (!ok || nn == 0) _ini_bad.Add($"{k}='{v}'");
                    else if (fa == "c101") { _logger.Info($"settings: fp_actor=c101 (the old default) read as '{k_fp_actor_default}'"); _fp_actor = k_fp_actor_default; }
                    else _fp_actor = fa;
                } break;
                case "noimg_mode":        pi(k, v, ref _noimg_mode, 0, 2); break;
                case "shadow_near":       pf(k, v, ref _shadow_near, 0f, 2000f); break;   // the shadow budget (shadow_cam.cs): 0 = off
                case "shadow_budget":     pi(k, v, ref _shadow_budget, 2, 8); break;      // shadowed characters a pass (f654547's 4..16 instances clamp into range)
                case "shadow_cam":        pi(k, v, ref _shadow_cam, 0, 1); break;       // character shadows: 1 the game's light camera left alone (default), 0 overridden (old, A/B)
                case "fp_self_shadow":    pi(k, v, ref _fp_self_shadow, 0, 1); break;   // 1 own shadow always drawn (default), 0 hidden in first person's whole-body mode
                case "heat_haze":         pi(k, v, ref _heat_haze, 0, 2); break;        // fire shimmer: 0 off in the headset's 3D view (default), 1 fixed per eye (experimental), 2 as before (A/B)
                case "fmv_depth":         pb(k, v, ref _fmv_depth); break;              // FMVs in 3D with a converted depth file (fmv_relief.cs; default 0)
                case "fmv_depth_strength": { float t = float.NaN; pf(k, v, ref t, 0f, 0.25f); fmv3d_migrate_strength(t); } break;   // the old linear relief: migrated to fmv_depth_amount (strength / 0.08), no longer written
                case "fmv_depth_amount":  pf(k, v, ref _fmv_depth_amount, 0f, 1f); _fmv_depth_amount_seen = true; break;   // the "3D movie" mapping's multiplier (default 1)
                case "fmv_depth_range":   pf(k, v, ref _fmv_depth_range, 0.5f, 10f); break;   // m behind the screen for the farthest content (default 3)
                case "fmv_depth_pop":     pf(k, v, ref _fmv_depth_pop, 0f, 0.5f); break;      // m in front of the screen for the nearest content (default 0.15)
                case "fmv_depth_smooth":  pf(k, v, ref _fmv_depth_smooth, 0f, 0.95f); break;  // the temporal filter within a shot (default 0.6)
                case "fmv_depth_median":  pb(k, v, ref _fmv_depth_median); break;            // Debug: the 3x3 median (default 1)
                case "fmv_depth_edges":   break;                                             // round 6: the old edge fill key (saved as 1 by every earlier build) is ignored: the fill starts off
                case "fmv_depth_edge_fill": pb(k, v, ref _fmv_depth_edges); break;           // Debug: the edge fill (default 0)
                case "fmv_depth_edge_band": pf(k, v, ref _fmv_depth_edge_band, 0.01f, 0.15f); break;   // Debug: where pop-out ramps to the screen plane at the border (default 0.04)
                case "fmv_depth_flat_subs": pb(k, v, ref _fmv_depth_flat_subs); break;  // the bottom 15 % flat (subtitles; default 0)
                case "fmv_depth_tight_fov": pb(k, v, ref _fmv_depth_tight_fov); break;  // Debug: the relief's projection narrowed to the screen (default 1; review SF-1)
                case "fmv_depth_offset":  break;                                         // session-only since review N6: an old line is ignored (the offset starts at 0)
                case "hist_mode":         pi(k, v, ref _hist_mode, 0, 2); break;
                case "follow_pans":       pb(k, v, ref _follow_pans); break;
                case "follow_rate":       pf(k, v, ref _follow_rate, 1f, 360f); break;
                case "fp_eye_height":     pf(k, v, ref _fp_eye_h, 0f, 40f); break;
                case "follow_distance":   pf(k, v, ref _fp_follow_dist, 10f, 300f); break;
                case "follow_height":     pf(k, v, ref _fp_follow_h, 0f, 150f); break;
                case "tp_distance":       pf(k, v, ref _tp_dist, 4f, 120f); break;
                case "tp_height":         pf(k, v, ref _tp_h, 0f, 60f); break;
                case "tp_pitch":          pf(k, v, ref _tp_pitch, -60f, 30f); break;
                case "tp_follow_rate":    pf(k, v, ref _tp_follow_rate, 0f, 180f); break;
                case "anchor_smooth_ms":  pf(k, v, ref _anchor_smooth_ms, 0f, 500f); break;
                case "battle_anchor_smooth_ms": pf(k, v, ref _battle_anchor_smooth_ms, 0f, 3000f); break;
                case "hands_dev":         pb(k, v, ref _hands_dev); break;
                case "hands":             pb(k, v, ref _hands_on); break;
                case "hands_head":        pb(k, v, ref _hands_head); break;
                case "hands_show":        pi(k, v, ref _hands_show, 0, 3); break;
                case "hands_yaw":         pf(k, v, ref _hands_yaw, -180f, 180f); break;
                case "hands_pitch":       pf(k, v, ref _hands_pitch, -180f, 180f); break;
                case "hands_roll":        pf(k, v, ref _hands_roll, -180f, 180f); break;
                case "hands_off_right":   pf(k, v, ref _hands_off_r, -0.5f, 0.5f); break;
                case "hands_off_up":      pf(k, v, ref _hands_off_u, -0.5f, 0.5f); break;
                case "hands_off_back":    pf(k, v, ref _hands_off_b, -0.5f, 0.5f); break;
                case "hands_swap":        pb(k, v, ref _hands_swap); break;
                case "hands_mirror":      pb(k, v, ref _hands_mirror); break;
                case "hands_cal":         _hands_cal_ini = v; break;
                case "hands_px_left":     pi(k, v, ref _hands_px_left, -1, 1); break;
                case "hands_stretch":     pf(k, v, ref _hands_stretch, 1f, 2f); break;
                case "diag_build7":       pb(k, v, ref _diag7); break;   // build 7 diagnostics (default 0; logging at once; the hooks follow cursor_source / smooth_anchor at launch)
                case "smooth_anchor":     pb(k, v, ref _smooth_anchor); break;   // the first / third person / diorama anchor from the drawn (interpolated) root
                case "diag_load":         pb(k, v, ref _diag_load); break;             // the load lines' expensive parts (default 0; at once)
                case "diag_load_lines":   pb(k, v, ref _diag_load_lines); break;       // loads build: one load: line per load (default 0 since the release prep; at once; the hooks at launch)
                case "battle_cache_models": break;                                     // retired (the HD cluster cache build): read and ignored, dropped at the next save
                case "hd_cluster_cache":  pb(k, v, ref _hd_cluster_cache); break;      // keep the HD map textures across random battles in one zone (applies at once)
                case "diag_cluster_bytes": pb(k, v, ref _diag_cluster_bytes); break;   // HC-2: the game pool's alloc / free hooks, for the bytes per HD cluster (hooks at the next launch; default 0: the header read stands in)
                case "diag_va_log":       pb(k, v, ref _diag_va_log); break;           // HC-6: the ntdll reservation patch (hooks at the next launch; default 0: the region diff is patch-free)
                case "battle_transition": bt_parse_mode(k, v); break;                  // the glass break at a battle start: 2 frozen world, 1 head-locked, 0 old (applies at once)
                case "diag_transition":   pb(k, v, ref _diag_transition); break;       // the per-frame bt: lines in a battle transition window
                case "diag_load_prof":    lp_parse_prof(k, v); break;                  // load lock build: the load profiler 0/1/2 (the probes hook at the next launch; the sampler at once)
                case "load_lock_release": pe(k, v, ref _load_lock_release, 0, 1, 2); break;   // 0 off, 1 battle loads (default), 2 all loads (test: a hand edit applies at once; a launch read and the saved file make it 1)
                case "ini_rev":           pi(k, v, ref _ini_rev_read, 0, 1000000); break;     // LD2-5: the file's revision (a launch read below k_ini_rev migrates)
                case "diag_actors":       pb(k, v, ref _diag_actors); break;                 // LD2-8: the actors: line every 10 s (default 0; at once)
                case "diag_verbose":      pb(k, v, ref _diag_verbose); _diag_verbose_read = true; break;   // release prep: the periodic log lines, the camera trace, Ctrl+F9 (default 0; at once; logging.cs)
                case "cursor_source":     pw(k, v, ref _cursor_game_cfg, "game", "legacy"); break;   // cursor stage C2
                case "marker_lift":       pf(k, v, ref _marker_lift, -5f, 20f); break;              // C2-10
                case "cursor_look":       pw(k, v, ref _cursor_look_game, "game", "mod"); break;    // cursor stage C3
                case "cursor_hand_size":  pf(k, v, ref _cursor_hand_size, 0.3f, 3f); break;
                case "fp_self_marker": {  // low (default) / hide / normal
                    string fs = v.Trim().ToLowerInvariant();
                    if (fs == "low") _fp_self_marker = 0; else if (fs == "hide") _fp_self_marker = 1; else if (fs == "normal") _fp_self_marker = 2; else _ini_bad.Add($"{k}='{v}'");
                } break;
                default:                  if (!hands2_parse(k, v) && !ride_parse(k, v) && !qc_parse(k, v) && !odg_parse(k, v)) known--; break;   // VR hands v2: the vr_hands_* keys (hands.cs); riding: ride_* (ride.cs); Quest-only controls (quest_ctl.cs); od_gest* (od_gest.cs)
            }
        }
        if (!float.IsNaN(ws)) { if (life_size(_fp_mode)) _scale_before_fp = ws; else _xr_scale = ws; }   // R28
        return known;
    }

    /// <summary>LD2-5, the launch read only (also the late one): an ini older than k_ini_rev has its OLD DEFAULTS replaced, logged
    /// once; real choices are kept (LD2-R-2). load_lock_release=0 there is always LL-2's forced value (every build before 76ce742
    /// wrote 0 whatever the overlay said, and 76ce742 itself never reached a player), so it becomes 1; heat_haze=1 is the haze
    /// build's default and becomes 0, while 0 or 2 were picked and stay. The diag_* keys are never changed, but the ones that are
    /// on are listed (every launch), so the owner sees what an old ini keeps on. Also every launch: a saved or hand-written
    /// load_lock_release=2 (the test mode) starts as 1 (LD2-R-3). The next save writes ini_rev (ini_text).</summary>
    private void settings_migrate() {
        string mig = "";
        if (_ini_rev_read < 1) {
            var ch = new List<string>();
            if (_load_lock_release == 0) { _load_lock_release = 1; ch.Add("load_lock_release 0 -> 1 (battle loads; the 0 was the old builds' forced value)"); }
            if (_heat_haze == 1) { _heat_haze = 0; ch.Add("heat_haze 1 -> 0 (off in the headset; 1 was the haze build's default)"); }
            mig = I($"settings: ffxvr.ini rev {_ini_rev_read} (< 1, written before the loads build): migrated once: {(ch.Count > 0 ? string.Join(", ", ch) : "nothing to change")}; other values (heat_haze 0 / 2, every diag_* key) are kept as chosen");
            _logger.Info(mig);
        }
        if (_ini_rev_read < 2 && !_diag_verbose_read) {   // release prep: an existing ini keeps the periodic lines it always had
            _diag_verbose = true;
            string m2 = I($"settings: ffxvr.ini rev {_ini_rev_read} (< 2) has no diag_verbose: 1, the periodic log lines as before (a fresh install starts with 0; tools\\set-logging.ps1 -Profile Release turns every diagnostic off)");
            _logger.Info(m2); mig = mig.Length > 0 ? mig + "\n" + m2 : m2;
        }
        if (_load_lock_release == 2) { _load_lock_release = 1; _logger.Info("settings: load_lock_release=2 (all loads, the test mode) starts as 1 (battle loads): the test mode is never kept across launches"); }
        var on = diag_on();
        _logger.Info("settings: diag_* keys on: " + (on.Count > 0 ? string.Join(", ", on) : "none"));
        _ini_diag_note = mig.Length > 0 ? mig : null;   // the list itself opens every session's ffxvr_diag.log (log_hello)
    }

    /// <summary>The launch-only effects of the file (R28): init only, after the first parse.</summary>
    private void settings_apply_launch() {
        if (!_headset_autostart) return;
        _xr_want = true;
        if (_start_view >= 0 && _start_view <= 3 && _start_view != _fp_mode) { apply_mode_scale(_fp_mode, _start_view); _fp_mode = _start_view; }
        _window_open = _overlay_at_start;   // straight into the headset: no overlay until F8
    }

    /// <summary>The startup file has been read (at init, or late by the reload when the init read failed): remember it, and
    /// check for a truncated file. A truncated one (fewer than 10 known keys, with a .bak present) is not canonicalised at
    /// launch (B8-1): it stays short until a real change, so the note repeats next session, and the .bak is kept once as
    /// ffxvr.ini.bak-keep, which nothing overwrites or loads.</summary>
    private void settings_first_read_done(string raw, int keys) {
        _ini_loaded_keys = keys; _ini_last = raw; _ini_disk = raw;   // no spurious first reload; the first write still canonicalises the file
        string bak = _ini_path + ".bak", keep = _ini_path + ".bak-keep";
        if (keys >= 10 || !File.Exists(bak)) return;
        _ini_last = ini_text();
        bool kept = File.Exists(keep);
        if (!kept) { try { File.Copy(bak, keep, false); File.SetAttributes(keep, File.GetAttributes(keep) & ~FileAttributes.ReadOnly); kept = true; } catch (Exception e) { _logger.Warning($"settings: could not copy ffxvr.ini.bak to ffxvr.ini.bak-keep ({e.Message})"); } }
        _ini_note = $"ffxvr.ini looks truncated ({keys} keys); your previous settings are in ffxvr.ini.bak{(kept ? " (also kept as ffxvr.ini.bak-keep)" : "")}";
        _logger.Warning("settings: " + _ini_note + ". Nothing is loaded from it automatically, and the short file is left as it is until a setting changes");
    }

    /// <summary>Init: the path, the bone labels, the startup read (retried: the previous game process may still hold the
    /// file; the only place that sleeps), the parse and the launch effects.</summary>
    private void settings_load() {
        try {
            string dir = Path.GetDirectoryName(typeof(FFXVRModule).Assembly.Location) ?? ".";
            _ini_path = Path.Combine(dir, "ffxvr.ini");
            bone_labels_load();
            string raw = ""; Exception? last = null; DateTime mt = DateTime.MinValue; long len = -1; bool gone = !File.Exists(_ini_path);
            for (int attempt = 0; attempt < 10 && !gone; attempt++) {
                try { var fi = new FileInfo(_ini_path); mt = fi.LastWriteTimeUtc; len = fi.Length; raw = File.ReadAllText(_ini_path); last = null; break; }   // stat first: a change in between is reloaded later
                catch (FileNotFoundException) { gone = true; }   // deleted meanwhile: the "no file yet" case (B8-6e)
                catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) { last = e; System.Threading.Thread.Sleep(100); }
            }
            if (gone) { _settings_ok = true; _ini_len_seen = -1; _logger.Info($"settings: no {_ini_path} yet; defaults"); return; }   // _ini_last "": the defaults are written once
            if (last != null) { _settings_ok = false; _logger.Info($"settings: could not read {_ini_path} ({last.Message}); keeping defaults in memory and not saving until it can be read (retried every second)"); return; }
            _ini_rev_read = 0; _diag_verbose_read = false;
            int keys = settings_parse(raw.Split('\n'));
            settings_migrate();   // LD2-5
            settings_apply_launch();
            _ini_mtime_seen = mt; _ini_len_seen = len;
            settings_first_read_done(raw, keys);
            _logger.Info($"settings: loaded {_ini_path} ({keys} keys): headset_autostart={_headset_autostart} start_view={_start_view} overlay_at_start={_overlay_at_start} overlay_open={_window_open} eye2_preview={_eye2_preview}{bad_suffix()}");
            _overlay_at_start_loaded = _overlay_at_start; _settings_ok = true;
        }
        catch (Exception e) { _settings_ok = false; _logger.Info($"settings: load failed ({e.GetType().Name}: {e.Message}); not saving this session"); }
    }

    /// <summary>R31/R33: every second of _now, a hand edit of the ini is read back. Seen (mtime, length) are only marked
    /// after a successful read, so a locked file is retried; no sleep; file values win (no launch effects, R28); the
    /// overlay's open state stays per session; the file stays as typed until a setting really changes (unless a value had
    /// to be ignored or clamped: then the corrected text is written, B8-4). A deleted ini is not recreated here: it is
    /// written again at the next settings change. Under _ini_lock (B8-5): the exit flush cannot write between the
    /// "seen" marking and the parse.</summary>
    private void settings_reload_check() {
        if (_ini_path.Length == 0 || _now < _ini_next_check || _ini_exiting) return;
        _ini_next_check = _now + 1.0;
        lock (_ini_lock) {
            try {
                var fi = new FileInfo(_ini_path);
                if (!fi.Exists) {
                    if (_ini_len_seen != -1) { _ini_len_seen = -1; _ini_mtime_seen = DateTime.MinValue; _ini_last = ini_text(); _ini_disk = ""; _ini_pending = null; _logger.Info("settings: ffxvr.ini was deleted; it is written again at the next settings change"); }
                    if (!_settings_ok && _ini_disk.Length == 0) { _settings_ok = true; _logger.Info("settings: ffxvr.ini is gone (the start-up read had failed); defaults, saved at the next settings change"); }   // B8-6e
                    return;
                }
                DateTime mt = fi.LastWriteTimeUtc; long len = fi.Length;
                if (mt == _ini_mtime_seen && len == _ini_len_seen) return;
                string on_disk;
                try { on_disk = File.ReadAllText(_ini_path); }
                catch (Exception e) when (e is IOException || e is UnauthorizedAccessException) {   // R29: logged once, retried quietly every second
                    if (_ini_read_fail_n++ == 0) _logger.Info($"settings: could not read ffxvr.ini ({e.Message}); retrying every second");
                    return;
                }
                if (_ini_read_fail_n > 0) { _logger.Info($"settings: ffxvr.ini can be read again (after {_ini_read_fail_n} tries)"); _ini_read_fail_n = 0; }
                _ini_mtime_seen = mt; _ini_len_seen = len;
                if (!_settings_ok && _ini_disk.Length == 0) {   // B8-6: the start-up read had failed; this is that read, late
                    _ini_rev_read = 0; _diag_verbose_read = false;
                    int all = settings_parse(on_disk.Split('\n'));
                    settings_migrate();   // LD2-5: this is the launch read, late
                    settings_first_read_done(on_disk, all);
                    _settings_ok = true; _overlay_at_start_loaded = _overlay_at_start; _ini_pending = null;
                    _logger.Info($"settings: ffxvr.ini read at last ({all} keys); launch options (autostart, start view) apply from the next start{bad_suffix()}");
                    return;
                }
                string disk_before = _ini_disk; _ini_disk = on_disk;   // B8-8: kept current on every successful read
                if (on_disk == _ini_last) return;
                // Only the keys the edit changed (vs the file as we last read or wrote it) are applied: with nothing unsaved that is the
                // same as parsing the whole file, and an overlay change not yet written (a slider moved < 1.5 s before the edit) is
                // kept and then written together with the edit. A key changed both ways takes the file's value.
                var prev = ini_kv(disk_before); var changed = new List<string>();
                foreach (var kv in ini_kv(on_disk)) if (!prev.TryGetValue(kv.Key, out string? pv) || pv != kv.Value) changed.Add(kv.Key + "=" + kv.Value);
                bool unsaved = ini_text() != _ini_last;
                bool win = _window_open;
                int keys = settings_parse(changed.ToArray());
                _window_open = win;
                _ini_pending = null; _settings_ok = true; _overlay_at_start_loaded = _overlay_at_start;
                bool corrected = _ini_bad.Count > 0 || _ini_clamped.Count > 0;
                if (!unsaved && !corrected) _ini_last = ini_text();   // the file stays as typed until a setting really changes (else: the merge / the correction is written)
                if (keys == 0 && !corrected) _logger.Info("settings: ffxvr.ini re-read; no setting changed");
                else _logger.Info($"settings: ffxvr.ini was edited on disk; reloaded ({keys} changed key{(keys == 1 ? "" : "s")}){(unsaved ? "; an unsaved overlay change is kept and written with it" : "")}{(corrected ? "; the corrected text is written back" : "")}{bad_suffix()}");
            }
            catch (Exception e) { if (_ini_reload_fault_n++ == 0) _logger.Info($"settings: reload failed ({e.GetType().Name}: {e.Message}); retrying every second"); }   // B8-7: its own counter
        }
    }

    /// <summary>R21: every 0.5 s a snapshot of the settings text; written once it has been the same for 1 s (a slider drag
    /// writes once, after the release), and not before a failed write's 5 s retry time.</summary>
    private void settings_save_tick() {
        if (_ini_path.Length == 0 || !_settings_ok || _now < _ini_next_snap || _ini_exiting) return;
        _ini_next_snap = _now + 0.5;
        string text = ini_text();
        if (text == _ini_last) { _ini_pending = null; return; }
        if (text != _ini_pending) { _ini_pending = text; _ini_pending_at = _now; return; }   // still changing
        if (_now - _ini_pending_at < 1.0 || _now < _ini_retry_at) return;
        settings_write(text, false);
    }
    /// <summary>R21: the exit flush (first thing in the ExitProcess detour): anything not yet written. A hand edit still wins.
    /// From here on no other save runs (B8-5: a render-thread write queued behind this one would put an older text back).</summary>
    private void settings_flush() {
        _ini_exiting = true;
        try { if (_settings_ok && _ini_path.Length > 0) { string t = ini_text(); if (t != _ini_last) settings_write(t, true); } }
        catch (Exception e) { try { _logger.Info($"settings: exit flush skipped ({e.GetType().Name}: {e.Message})"); } catch { } }   // B8-11
    }

    /// <summary>R21/R29/R30: under _ini_lock (the exit flush can run on another thread). Never over a hand edit: if the
    /// file changed since our last read or write (mtime, length, and then its content, B8-3) it is left alone and reloaded
    /// next frame. Temp file, flushed to disk, then moved over the ini (atomic on one volume). The .bak is made once a
    /// session, only from a startup file that parsed fully (10+ keys), in its own try, through its own temp file, with any
    /// read-only flag cleared (B8-2, B8-10). Failures are logged once and retried every 5 s.</summary>
    private bool settings_write(string text, bool from_exit) {
        lock (_ini_lock) {
            if (_ini_exiting && !from_exit) return false;   // B8-5
            string tmp = _ini_path + ".tmp";
            try {
                var fi = new FileInfo(_ini_path);
                if (fi.Exists && (fi.LastWriteTimeUtc != _ini_mtime_seen || fi.Length != _ini_len_seen)) { _ini_next_check = 0; return false; }   // hand edit: it wins
                if (fi.Exists && fi.IsReadOnly) throw new UnauthorizedAccessException("ffxvr.ini is read-only");   // B8-9: fail before the temp file and the fsync
                if (fi.Exists && File.ReadAllText(_ini_path) != _ini_disk) { _ini_next_check = 0; return false; }   // B8-3: same stamp and length, other content
                if (!_ini_backed_up) {
                    _ini_backed_up = true;   // either way: a bad .bak never blocks saving
                    if (fi.Exists && _ini_loaded_keys >= 10) {
                        string bak = _ini_path + ".bak", btmp = bak + ".tmp";
                        try {
                            File.Copy(_ini_path, btmp, true);
                            File.SetAttributes(btmp, File.GetAttributes(btmp) & ~FileAttributes.ReadOnly);   // File.Copy carries ReadOnly over
                            if (File.Exists(bak)) File.SetAttributes(bak, File.GetAttributes(bak) & ~FileAttributes.ReadOnly);
                            File.Move(btmp, bak, true);
                        }
                        catch (Exception e) {
                            if (_ini_bak_fail_n++ == 0) _logger.Warning($"settings: could not refresh ffxvr.ini.bak ({e.GetType().Name}: {e.Message}); saving goes on");
                            try { File.Delete(btmp); } catch { }
                        }
                    }
                }
                using (var fs = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None))
                using (var sw = new StreamWriter(fs, new System.Text.UTF8Encoding(false))) { sw.Write(text); sw.Flush(); fs.Flush(true); }
                File.Move(tmp, _ini_path, true);
                var nf = new FileInfo(_ini_path); _ini_mtime_seen = nf.LastWriteTimeUtc; _ini_len_seen = nf.Length;
                _ini_last = text; _ini_disk = text; _ini_pending = null;
                if (_overlay_at_start != _overlay_at_start_loaded) { _logger.Info($"settings: overlay_at_start changed to {_overlay_at_start} (window open {_window_open}, frame {_frames})"); _overlay_at_start_loaded = _overlay_at_start; }
                if (_ini_fail_n > 0) { _logger.Info($"settings: saving works again (after {_ini_fail_n} failed tries)"); _ini_fail_n = 0; }
                return true;
            }
            catch (Exception e) {
                if (_ini_fail_n++ == 0) _logger.Warning($"settings: could not save {_ini_path} ({e.GetType().Name}: {e.Message}); retrying every 5 s");
                _ini_retry_at = _now + 5.0;
                try { File.Delete(tmp); } catch { }
                return false;
            }
        }
    }

    /// <summary>The ini as the overlay would write it (canonical; every number in the invariant culture, R32).</summary>
    private string ini_text() {
        var ci = CI;
        return
            "# FFX VR settings. Edited by the overlay; hand edits are fine.\n" +
            $"ini_rev={k_ini_rev.ToString(ci)}\n" +   // LD2-5: an ini without it (or older) is migrated once at launch
            $"headset_autostart={(_headset_autostart ? 1 : 0)}\n" +
            $"start_view={_start_view.ToString(ci)}\n" +
            $"overlay_at_start={(_overlay_at_start ? 1 : 0)}\n" +
            $"world_scale={(life_size(_fp_mode) ? _scale_before_fp : _xr_scale).ToString("F1", ci)}\n" +   // the seat / diorama scale; first and third person are always true scale
            $"head_position={(_xr_translate ? 1 : 0)}\n" +
            $"seat={(_xr_seat ? 1 : 0)}\n" +
            $"headset_fov={(_xr_fov ? 1 : 0)}\n" +
            $"screen_mode={_xr_screen_mode.ToString(ci)}\n" +
            $"stereo_screen_blitz={(_s3_blitz ? 1 : 0)}\n" +
            $"stereo_screen_all={(_s3_all ? 1 : 0)}\n" +
            $"stereo_screen_depth={_s3_depth.ToString("F2", ci)}\n" +
            $"stereo_screen_plane={_s3_plane.ToString("F0", ci)}\n" +
            $"stereo_screen_auto={(_s3_auto ? 1 : 0)}\n" +
            $"stereo_screen_max_pop={_s3_max_pop.ToString("F1", ci)}\n" +
            $"backdrop_mode={_backdrop_mode.ToString(ci)}\n" +
            $"cull_dist={(_cull_dist ? 1 : 0)}\n" +
            $"cull_scene={(_cull_scene ? 1 : 0)}\n" +
            $"msaa={_msaa.ToString(ci)}\n" +
            $"vrpad={(_vrpad ? 1 : 0)}\n" +
            $"haptics={(_haptics ? 1 : 0)}\n" +
            $"haptics_gain={_haptics_gain.ToString("0.##", ci)}\n" +
            $"slow_guard={(_slow_guard ? 1 : 0)}\n" +
            $"slow_guard_restart={(_slow_guard_restart ? 1 : 0)}\n" +
            $"timer_fix={(_timer_fix ? 1 : 0)}\n" +
            $"limiter_probe={(_limiter_probe ? 1 : 0)}\n" +
            $"fast_reads={(_fast_reads ? 1 : 0)}\n" +
            $"imgui_layer={(_imgui_layer ? 1 : 0)}\n" +
            $"fh_menu_on_screen={(_fh_menu_on_screen ? 1 : 0)}\n" +
            $"fh_dpi_fix={(_fh_dpi_fix ? 1 : 0)}\n" +
            $"overlay_text_scale={_overlay_text_scale.ToString("0.##", ci)}\n" +
            $"gestures={(_gestures ? 1 : 0)}\n" +
            $"gest_tilt={_gest_tilt.ToString("F2", ci)}\n" +
            $"fp_body_mode={_fp_body_mode.ToString(ci)}\n" +
            $"fp_head_anchor={_fp_head_anchor.ToString(ci)}\n" +
            $"battle_anchor={(_battle_anchor ? 1 : 0)}\n" +
            $"ui_hide={_ui_hide.ToString(ci)}\n" +
            $"hide_cursor={(_hide_cursor ? 1 : 0)}\n" +
            $"hide_pointers={(_hide_pointers ? 1 : 0)}\n" +
            $"blitz_overlay={(_blitz_overlay ? 1 : 0)}\n" +
            $"markers_game_art={(_markers_game_art ? 1 : 0)}\n" +
            $"marker_scale={_marker_scale.ToString("0.##", ci)}\n" +
            $"hide_list={hide_list_str()}\n" +
            $"battle_follow={(_battle_follow ? 1 : 0)}\n" +
            $"battle_view={_battle_view.ToString(ci)}\n" +
            btg_ini_text() +   // battle tag (bt_tag.cs)
            dmg_ini_text() +   // 3D damage numbers (dmg3d.cs)
            $"diorama_floor={_diorama_floor.ToString("F1", ci)}\n" +
            $"fp_head_up={_fp_head_up.ToString("F2", ci)}\n" +
            $"fp_head_reach={_fp_head_reach.ToString("F1", ci)}\n" +
            $"fp_head_smooth={_fp_head_smooth.ToString("F2", ci)}\n" +
            $"fp_head_fwd={_fp_head_fwd.ToString("F2", ci)}\n" +
            $"fp_head_parts={_fp_head_parts}\n" +
            $"screen_width={_xr_screen_width.ToString("F2", ci)}\n" +
            $"screen_distance={_xr_screen_dist.ToString("F2", ci)}\n" +
            $"overlay_open={((_window_open_forced ? _window_open_pre_force : _window_open) ? 1 : 0)}\n" +   // a failure's forced open is not saved
            $"eye2_preview={(_eye2_preview ? 1 : 0)}\n" +
            $"ui_panel={(_ui_on ? 1 : 0)}\n" +
            $"ui_distance={_ui_dist.ToString("F2", ci)}\n" +
            $"ui_width={_ui_width.ToString("F2", ci)}\n" +
            $"ui_adaptive={(_ui_adaptive ? 1 : 0)}\n" +
            $"ui_adaptive_min={_ui_adaptive_min.ToString("F2", ci)}\n" +
            $"ui_adaptive_margin={_ui_adaptive_margin.ToString("F2", ci)}\n" +
            $"ui_backing={_ui_backing.ToString("F2", ci)}\n" +
            $"dream_overlay={_dream_overlay.ToString(ci)}\n" +
            $"pitch_follow={_xr_pitch_follow.ToString("F2", ci)}\n" +
            $"seat_height={_ovr_up.ToString("F0", ci)}\n" +
            $"seat_forward={_ovr_forward.ToString("F0", ci)}\n" +
            $"cut_recentre={(_cut_recentre ? 1 : 0)}\n" +
            $"cut_blink={(_cut_blink ? 1 : 0)}\n" +
            ej_ini_text() +   // eye jumps (eye_jump.cs)
            $"turn_stick={(_turn_stick ? 1 : 0)}\n" +
            $"turn_mode={_turn_mode.ToString(ci)}\n" +
            $"turn_snap={_turn_snap.ToString("F0", ci)}\n" +
            $"turn_rate={_turn_rate.ToString("F0", ci)}\n" +
            $"turn_blink={(_turn_blink ? 1 : 0)}\n" +
            $"stick_reproject={_stick_reproject.ToString(ci)}\n" +
            $"stick_reproject_margin_ms={_sr_margin_ms.ToString("0.0", ci)}\n" +
            $"stick_reproject_predict={(_sr_predict ? 1 : 0)}\n" +
            $"stick_overscan={_stick_overscan.ToString("0.0", ci)}\n" +
            $"keepalive={(_keepalive ? 1 : 0)}\n" +
            $"keepalive_mt_protect=0\n" +   // SR-X2: never on
            $"move_remap={(_move_remap ? 1 : 0)}\n" +
            $"move_invert={(_move_invert ? 1 : 0)}\n" +
            $"fp_actor={_fp_actor}\n" +
            $"noimg_mode={_noimg_mode.ToString(ci)}\n" +
            $"shadow_cam={_shadow_cam.ToString(ci)}\n" +
            $"fp_self_shadow={_fp_self_shadow.ToString(ci)}\n" +
            $"shadow_near={_shadow_near.ToString("F0", ci)}\n" +
            $"shadow_budget={_shadow_budget.ToString(ci)}\n" +
            $"heat_haze={_heat_haze.ToString(ci)}\n" +
            $"fmv_depth={(_fmv_depth ? 1 : 0)}\n" +
            $"fmv_depth_amount={_fmv_depth_amount.ToString("F2", ci)}\n" +
            $"fmv_depth_range={_fmv_depth_range.ToString("F1", ci)}\n" +
            $"fmv_depth_pop={_fmv_depth_pop.ToString("F2", ci)}\n" +
            $"fmv_depth_smooth={_fmv_depth_smooth.ToString("F2", ci)}\n" +
            $"fmv_depth_median={(_fmv_depth_median ? 1 : 0)}\n" +
            $"fmv_depth_edge_fill={(_fmv_depth_edges ? 1 : 0)}\n" +
            $"fmv_depth_edge_band={_fmv_depth_edge_band.ToString("F3", ci)}\n" +
            $"fmv_depth_flat_subs={(_fmv_depth_flat_subs ? 1 : 0)}\n" +
            $"fmv_depth_tight_fov={(_fmv_depth_tight_fov ? 1 : 0)}\n" +
            $"hist_mode={_hist_mode.ToString(ci)}\n" +
            $"follow_pans={(_follow_pans ? 1 : 0)}\n" +
            $"follow_rate={_follow_rate.ToString("F0", ci)}\n" +
            $"fp_eye_height={_fp_eye_h.ToString("F0", ci)}\n" +
            $"follow_distance={_fp_follow_dist.ToString("F0", ci)}\n" +
            $"follow_height={_fp_follow_h.ToString("F0", ci)}\n" +
            $"tp_distance={_tp_dist.ToString("F0", ci)}\n" +
            $"tp_height={_tp_h.ToString("F0", ci)}\n" +
            $"tp_pitch={_tp_pitch.ToString("F0", ci)}\n" +
            $"tp_follow_rate={_tp_follow_rate.ToString("F0", ci)}\n" +
            $"anchor_smooth_ms={_anchor_smooth_ms.ToString("F0", ci)}\n" +
            $"battle_anchor_smooth_ms={_battle_anchor_smooth_ms.ToString("F0", ci)}\n" +
            $"hands_dev={(_hands_dev ? 1 : 0)}\n" +
            $"hands={(_hands_on ? 1 : 0)}\n" +
            $"hands_head={(_hands_head ? 1 : 0)}\n" +
            $"hands_show={_hands_show.ToString(ci)}\n" +
            $"hands_yaw={_hands_yaw.ToString("F0", ci)}\n" +
            $"hands_pitch={_hands_pitch.ToString("F0", ci)}\n" +
            $"hands_roll={_hands_roll.ToString("F0", ci)}\n" +
            $"hands_off_right={_hands_off_r.ToString("F3", ci)}\n" +
            $"hands_off_up={_hands_off_u.ToString("F3", ci)}\n" +
            $"hands_off_back={_hands_off_b.ToString("F3", ci)}\n" +
            $"hands_swap={(_hands_swap ? 1 : 0)}\n" +
            $"hands_mirror={(_hands_mirror ? 1 : 0)}\n" +
            $"hands_cal={_hands_cal_ini}\n" +
            $"hands_px_left={_hands_px_left.ToString(ci)}\n" +
            $"hands_stretch={_hands_stretch.ToString("F2", ci)}\n" +
            hands2_ini_text() +   // VR hands v2 (hands.cs)
            ride_ini_text() +     // riding (ride.cs)
            qc_ini_text() +       // Quest-only controls (quest_ctl.cs)
            odg_ini_text() +      // overdrive gestures (od_gest.cs)
            $"diag_verbose={(_diag_verbose ? 1 : 0)}\n" +
            $"diag_build7={(_diag7 ? 1 : 0)}\n" +
            $"cursor_source={(_cursor_game_cfg ? "game" : "legacy")}\n" +
            $"smooth_anchor={(_smooth_anchor ? 1 : 0)}\n" +
            $"diag_load={(_diag_load ? 1 : 0)}\n" +
            $"diag_load_lines={(_diag_load_lines ? 1 : 0)}\n" +
            $"diag_actors={(_diag_actors ? 1 : 0)}\n" +
            $"hd_cluster_cache={(_hd_cluster_cache ? 1 : 0)}\n" +
            $"diag_cluster_bytes={(_diag_cluster_bytes ? 1 : 0)}\n" +
            $"diag_va_log={(_diag_va_log ? 1 : 0)}\n" +
            $"battle_transition={_battle_transition.ToString(ci)}\n" +
            $"diag_transition={(_diag_transition ? 1 : 0)}\n" +
            $"diag_load_prof={_diag_load_prof.ToString(ci)}\n" +
            $"load_lock_release={(_load_lock_release == 2 ? 1 : _load_lock_release).ToString(ci)}\n" +   // 0 off, 1 battle loads (default); the test mode 2 is written as 1 (LD2-R-3: never persisted)
            $"marker_lift={_marker_lift.ToString("F1", ci)}\n" +
            $"cursor_look={(_cursor_look_game ? "game" : "mod")}\n" +
            $"cursor_hand_size={_cursor_hand_size.ToString("0.##", ci)}\n" +
            $"fp_self_marker={(_fp_self_marker == 1 ? "hide" : _fp_self_marker == 2 ? "normal" : "low")}\n";
    }

    // -- main window --------------------------------------------------------------

    private string xr_status_text() {
        if (_fn_xr_status == null) return "";
        byte* buf = stackalloc byte[256];
        _fn_xr_status(buf, 256);
        return System.Runtime.InteropServices.Marshal.PtrToStringAnsi((nint)buf) ?? "";
    }

    /// <summary>What a player touches: headset, comfort, view. Everything else is under Debug.</summary>
    private void draw_main() {
        if (_fn_xr_set == null) { ImGui.Text($"Native helper: {esc(_native_status)}"); return; }

        // Headset
        ImGui.Checkbox("Headset", ref _xr_want);   // pushed every frame by push_all, until the native side accepts (it waits for the first 3D frame)
        ImGui.SameLine(); if (ImGui.Button("Recentre (Home)") && _fn_xr_recenter != null) _fn_xr_recenter();
        ImGui.SameLine(); ImGui.Checkbox("start automatically", ref _headset_autostart);
        if (_headset_autostart) {
            ImGui.SameLine(); ImGui.Checkbox("overlay open at start", ref _overlay_at_start); ImGui.SameLine(); ImGui.Text("start in:"); ImGui.SameLine();
            ImGui.RadioButton("seat##sv", ref _start_view, 0); ImGui.SameLine(); ImGui.RadioButton("first person##sv", ref _start_view, 1); ImGui.SameLine();
            ImGui.RadioButton("third person##sv", ref _start_view, 3); ImGui.SameLine(); ImGui.RadioButton("diorama##sv", ref _start_view, 2);
        }
        ImGui.TextWrapped(esc(xr_status_text()));   // the OpenXR runtime's name is in it
        if (_xr_drop_image) {   // a test toggle that blacks out the headset after 30 frames: always visible while it is on
            ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.35f, 0.35f, 1f));
            ImGui.TextWrapped("TEST ON: the eye image is being dropped (Debug > Headset tests). The headset holds the last frame, then goes black. Untick it to resume.");
            ImGui.PopStyleColor(1);
        }
        if (_fn_lowres_status != null) {   // the game renders below the headset's per-eye size (monitor off at launch, a lower desktop mode)
            byte* lr = stackalloc byte[320];
            if (_fn_lowres_status(lr, 320) != 0) {
                ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.8f, 0.3f, 1f));
                ImGui.TextWrapped(esc(System.Runtime.InteropServices.Marshal.PtrToStringAnsi((nint)lr)));
                ImGui.PopStyleColor(1);
            }
        }
        if (_fn_xr_screen_now != null && _xr_on) { int sn = _fn_xr_screen_now(); ImGui.Text(sn == 1 ? "showing: virtual screen" : sn == 2 ? "showing: stereo screen" : "showing: 3D scene"); }

        // Controllers
        ImGui.Separator();
        ImGui.Checkbox("Quest controllers as the gamepad", ref _vrpad); ImGui.SameLine();
        ImGui.TextDisabled("A/B/X/Y, sticks, triggers = LT/RT, grips = LB/RB (a quick squeeze in battle and the seat / diorama), left menu = Start, left stick click = Select");
        if (_fn_vrpad_status != null) {
            float* st = stackalloc float[24]; byte* pl = stackalloc byte[64]; byte* pr = stackalloc byte[64]; uint frames;
            int ready = _fn_vrpad_status(st, pl, pr, 64, &frames);
            if (ready == 0) ImGui.TextDisabled(_xr_on ? "controllers: no input this frame (headset not focused, or no bindings)" : "controllers: headset off");
            else {
                ImGui.Text($"controllers: syncing ({frames} frames)   left: {esc(System.Runtime.InteropServices.Marshal.PtrToStringAnsi((nint)pl))}   right: {esc(System.Runtime.InteropServices.Marshal.PtrToStringAnsi((nint)pr))}");
                for (int h = 0; h < 2; h++) {
                    float* o = st + h * 12;
                    ImGui.Text($"  {(h == 0 ? "L" : "R")}: {(o[0] > 0 ? "on " : "off")} stick ({o[1],5:F2},{o[2],5:F2}){(o[3] > 0 ? "*" : " ")}  {(h == 0 ? "X" : "A")}{(o[4] > 0 ? "#" : "-")} {(h == 0 ? "Y" : "B")}{(o[5] > 0 ? "#" : "-")}  trig {o[6]:F2} grip {o[7]:F2}  {(h == 0 ? (o[8] > 0 ? "menu#" : "menu-") : "     ")}  pose {(o[9] > 0 ? "grip" : "----")}/{(o[10] > 0 ? "aim" : "---")}");
                }
                if (_fn_haptic != null) { if (ImGui.SmallButton("buzz left")) _fn_haptic(0, 0.5f, 0.1f); ImGui.SameLine(); if (ImGui.SmallButton("buzz right")) _fn_haptic(1, 0.5f, 0.1f); }
            }
        }
        ImGui.Checkbox("rumble on the controllers (the game's vibration; the in-game Vibration option still decides)", ref _haptics); ImGui.SameLine();
        ImGui.SetNextItemWidth(160f); ImGui.SliderFloat("strength##hap", ref _haptics_gain, 0f, 1f);
        {
            int hs = _haptics_status, src = (hs >> 16) & 0xF;
            ImGui.TextDisabled($"  rumble: {(((hs >> 20) & 1) != 0 ? "hooked" : "XInputSetState not hooked")}, source {(src == 1 ? "the game's XInputSetState" : src == 2 ? "the game's pad block" : src == 3 ? "in-game Vibration off" : "none (headset off or not focused, haptics off, or a pad in use with the VR pad off)")}, motors left {hs & 0xFF} right {(hs >> 8) & 0xFF} (of 254)");
        }
        ImGui.Checkbox("grips grab the world in the seat view (one hand pans; two hands pan, turn, tilt and zoom)", ref _gestures);
        ImGui.SameLine(); ImGui.SetNextItemWidth(120); ImGui.SliderFloat("tilt (wrist roll) gain", ref _gest_tilt, -2f, 2f, "%.1f");
        ImGui.TextDisabled($"right grip + right stick click: first person on/off.   {(_gest_active != 0 ? "GRABBING   " : "")}world scale now {_xr_scale * _gest_scale_mul:F0} units/m (base {_xr_scale:F0} x {_gest_scale_mul:F2}), grab offset ({_gest_move_x:F0}, {_gest_move_y:F0}, {_gest_move_z:F0})");
        ImGui.SameLine(); if (ImGui.SmallButton("reset grab")) { if (_fn_gesture_reset != null) _fn_gesture_reset(); }
        qc_ui();   // Quest-only controls: quick-squeeze grips, the VD overlay mask, the save screen (quest_ctl.cs)
        odg_ui();  // overdrive gestures: Lulu's circles, Tidus's swing, Auron's slashes (od_gest.cs)

        // Comfort
        ImGui.Separator();
        ImGui.SliderFloat("world scale", ref _xr_scale, 10f, 300f, "%.0f units/m");
        ImGui.SameLine(); ImGui.TextDisabled(_xr_scale < 20f ? "life-size" : _xr_scale < 60f ? "large model" : "tabletop diorama");
        ImGui.Checkbox("head position (lean)", ref _xr_translate); ImGui.SameLine();
        ImGui.Checkbox("seat (head owns orientation)", ref _xr_seat);
        ImGui.SliderFloat("pitch follow", ref _xr_pitch_follow, 0f, 1f, "%.2f");
        ImGui.SameLine(); ImGui.TextDisabled("0 level horizon .. 1 the original view");
        ImGui.Text("camera cuts:"); ImGui.SameLine();
        ImGui.Checkbox("recentre on cut", ref _cut_recentre); ImGui.SameLine();
        ImGui.Checkbox("blink", ref _cut_blink); ImGui.SameLine();
        ImGui.Checkbox("follow pans", ref _follow_pans);
        if (_follow_pans) ImGui.SliderFloat("pan follow rate (deg/s)", ref _follow_rate, 5f, 360f);
        if (_fn_cut_count != null) { ImGui.SameLine(); ImGui.TextDisabled($"cuts seen: {_fn_cut_count()}"); }
        ej_ui();   // eye jumps: eye_jump_blink, eye_jump_units, battle_face (eye_jump.cs)
        ImGui.SliderFloat("seat height", ref _ovr_up, -300f, 300f, "%.0f units");
        ImGui.SliderFloat("seat forward", ref _ovr_forward, -600f, 600f, "%.0f units");
        ImGui.SameLine(); if (ImGui.Button("reset offsets")) { _ovr_yaw = _ovr_pitch = 0; _ovr_right = _ovr_up = _ovr_forward = 0; }

        // View mode
        ImGui.Separator();
        ImGui.Text("camera:"); ImGui.SameLine();
        int fp_before = _fp_mode;
        ImGui.RadioButton("authored (seat)", ref _fp_mode, 0); ImGui.SameLine();
        ImGui.RadioButton("first person", ref _fp_mode, 1); ImGui.SameLine();
        ImGui.RadioButton("third person", ref _fp_mode, 3); ImGui.SameLine();
        ImGui.RadioButton("diorama (follows the player)", ref _fp_mode, 2);
        ImGui.SameLine(); ImGui.TextDisabled("End / right grip + right stick click: first person -> third person -> diorama.   left grip + left stick click: reset the view");

        // Turning (first person and seat alike)
        ImGui.Text("turning:"); ImGui.SameLine();
        ImGui.Checkbox("right stick", ref _turn_stick); ImGui.SameLine();
        ImGui.SliderFloat("turn speed", ref _turn_rate, 30f, 240f, "%.0f deg/s");
        ImGui.SameLine(); ImGui.TextDisabled("Numpad 1/3: turn left/right");
        sr_ui();       // stickrep: stick_reproject (the turn drawn at the headset's rate)
        ImGui.Checkbox("left stick follows where I look (first person)", ref _move_remap); ImGui.SameLine();
        ImGui.Checkbox("invert", ref _move_invert);
        if (_fn_move_status != null) {
            float yv, yc, dd; uint calls; int hooked;
            _fn_move_status(&yv, &yc, &dd, &calls, &hooked);
            ImGui.SameLine(); ImGui.TextDisabled(hooked != 0 ? $"pad reads: {calls}  view {yv:F0}\u00b0 camera {yc:F0}\u00b0 stick turned {dd:F0}\u00b0" : "pad hook not installed");
        }
        qc_walk_ui();   // the walk gate: move_remap_walk_only (quest_ctl.cs)
        if (_fp_mode != fp_before) apply_mode_scale(fp_before, _fp_mode);
        hands2_ui();   // VR hands v2: the "VR hands (experimental)" header (hands.cs)

        // View
        ImGui.Separator();
        ImGui.Checkbox("headset field of view", ref _xr_fov); ImGui.SameLine();
        shadow_ui();   // shadow camera: shadow_cam, fp_self_shadow
        haze_ui();     // heat haze: heat_haze
        ImGui.Text("2D screens:"); ImGui.SameLine();
        ImGui.RadioButton("auto", ref _xr_screen_mode, 0); ImGui.SameLine();
        ImGui.RadioButton("always", ref _xr_screen_mode, 1); ImGui.SameLine();
        ImGui.RadioButton("never", ref _xr_screen_mode, 2);
        fmv3d_ui();    // fmv-relief (S3): fmv_depth, fmv_depth_amount / _range / _pop / _smooth, fmv_depth_flat_subs (fmv_relief.cs)
        ImGui.Text("stereo screen (3D scenes on the big screen, through the game's own camera, in 3D):"); ImGui.SameLine();
        ImGui.Checkbox("in blitzball##s3", ref _s3_blitz); ImGui.SameLine(); ImGui.Checkbox("everywhere##s3", ref _s3_all);
        if (_s3_blitz || _s3_all) {
            ImGui.SliderFloat("3D depth (1 = standard; 0 = flat)##s3", ref _s3_depth, 0f, 3f, "%.2f");
            ImGui.Checkbox("auto depth: the screen plane follows the subject, and nothing pops out further than##s3", ref _s3_auto); ImGui.SameLine();
            ImGui.SetNextItemWidth(120); ImGui.SliderFloat("% of the screen (0 = nothing in front of it)##s3pop", ref _s3_max_pop, 0f, 6f, "%.1f");
            if (_s3_auto && _fn_screen3d_status != null) {
                int vis; float nd, sd, pl, sp; _fn_screen3d_status(&vis, &nd, &sd, &pl, &sp);
                ImGui.TextDisabled($"  now: {vis} characters in view, nearest {nd:F0}, subject {sd:F0}; screen plane {pl:F0}, eye separation {sp:F1} (background limited by the {_xr_screen_width:F1} m screen)");
            }
            if (!_s3_auto) ImGui.SliderFloat("screen plane (game units: nearer pops out, farther goes in)##s3", ref _s3_plane, 50f, 2000f, "%.0f");
            ImGui.SliderFloat("screen width (m)##s3", ref _xr_screen_width, 1f, 8f, "%.1f");
            ImGui.SliderFloat("screen distance (m)##s3", ref _xr_screen_dist, 1f, 6f, "%.1f"); ImGui.SameLine(); ImGui.TextDisabled("Home re-places the screen in front of you");
        }
        ImGui.Text("painted-backdrop rooms (hut interiors):"); ImGui.SameLine();
        ImGui.RadioButton("whole room on the screen", ref _backdrop_mode, 1); ImGui.SameLine();
        ImGui.RadioButton("3D props over the flat backdrop", ref _backdrop_mode, 0);
        if (_fn_backdrop_draws != null) { ImGui.SameLine(); ImGui.TextDisabled($"backdrop draws seen: {_fn_backdrop_draws()}"); }
        ImGui.Checkbox("UI on its own panel (menus, subtitles, HUD)", ref _ui_on); ImGui.SameLine();
        ImGui.Checkbox("Fahrenheit menus (save screen) and this overlay in the headset", ref _imgui_layer);
        if (_fn_imgui_draws != null) { ImGui.SameLine(); ImGui.TextDisabled($"layer draws {_fn_imgui_draws()} (shown when this overlay or the save screen is open, or at {_imgui_min_draws}+ draws)"); }
        if (_imgui_layer) fh_menu_ui();   // saveui: fh_menu_on_screen
        if (_ui_on) uidepth_ui();   // adaptive UI panel: ui_adaptive, ui_backing (the rest in Debug > UI panel)
        dream_ui();   // dream overlay: dream_overlay
        bt_ui();
        dmg3d_ui();    // 3D damage numbers: dmg3d, dmg3d_scale, dmg3d_lift, dmg3d_hide2d (dmg3d.cs)
        lp_ui();   // load lock build: load_lock_release
        ImGui.Checkbox("keep characters drawn (no distance or frustum cull of characters)", ref _cull_dist);
        if (_fn_cull_dist_get != null) { ImGui.SameLine(); ImGui.TextDisabled(_fn_cull_dist_get() != 0 ? "patched" : (_cull_dist_rc != 0 ? $"not applied (rc {_cull_dist_rc}, see diag log)" : "off")); }
        ImGui.Checkbox("keep all geometry drawn (no frustum cull of the scene; costs GPU time)", ref _cull_scene);
        if (_fn_cull_scene_get != null) { ImGui.SameLine(); ImGui.TextDisabled(_fn_cull_scene_get() != 0 ? "patched" : (_cull_scene_rc != 0 ? $"not applied (rc {_cull_scene_rc}, see diag log)" : "off")); }
        ImGui.Text("MSAA for the scene targets:"); ImGui.SameLine();
        ImGui.RadioButton("game default (8x)", ref _msaa, 0); ImGui.SameLine();
        ImGui.RadioButton("4x", ref _msaa, 4); ImGui.SameLine();
        ImGui.RadioButton("2x", ref _msaa, 2);
        if (_fn_msaa_status != null) {
            uint seen, changed, lw, lh, ln; _fn_msaa_status(&seen, &changed, &lw, &lh, &ln);
            ImGui.SameLine();
            ImGui.TextDisabled(seen == 0 ? "takes effect when the game next creates its scene targets" : $"targets created: {seen} (overridden {changed}), last {lw}x{lh} @ {ln} samples");
        }
        ImGui.Text("frame blending (water areas):"); ImGui.SameLine();
        ImGui.RadioButton("off (no trails)", ref _hist_mode, 1); ImGui.SameLine();
        ImGui.RadioButton("game default", ref _hist_mode, 0); ImGui.SameLine();
        ImGui.RadioButton("skip pass", ref _hist_mode, 2);
        if (_fn_hist_draws != null) { ImGui.SameLine(); ImGui.TextDisabled($"blend draws seen: {_fn_hist_draws()}"); }

        ImGui.Separator();
        // FH-2: overlay_text_scale, driven in whole 0.05 steps (10..40): one keyboard / gamepad press moves one step (a float
        // slider's 1 % nav step, 0.015, would round back to the same value), and a drag bakes a few font sizes, not hundreds.
        // The format has no '%': ImGui shows it as it is (a Ctrl+click entry takes the step count, clamped below).
        int ots = (int)MathF.Round(text_scale_step(_overlay_text_scale) * 20f);
        ImGui.SetNextItemWidth(160f);
        if (ImGui.SliderInt("text size of this overlay, Eye 2 and the labels##ots", ref ots, 10, 40, I($"{ots / 20f:F2}"))) _overlay_text_scale = text_scale_step(ots / 20f);
        ImGui.SameLine(); if (ImGui.SmallButton("default##ots")) _overlay_text_scale = 1f;
        ImGui.SameLine(); ImGui.TextDisabled(I($"now {ImGui.GetFontSize():F0} px"));
        if (_fn_perf_status != null) {
            byte* pb = stackalloc byte[256]; _fn_perf_status(pb, 256);
            ImGui.TextDisabled(esc(System.Runtime.InteropServices.Marshal.PtrToStringAnsi((nint)pb)));
        }
        ImGui.TextDisabled("Home: recentre   F8: hide/show this overlay (F7 is Fahrenheit's own mod-settings menu)");
        log_main_ui();   // release prep: the version, and a reminder while diagnostics are on (logging.cs)
    }

    /// <summary>Camera numbers, override controls, second eye, captures. Collapsed by default.</summary>
    private void draw_debug() {
        if (!ImGui.CollapsingHeader("Debug")) return;   // settings reach the native side through push_all, whatever is drawn
        // Expanded: keep the native diagnostics armed (1 s heartbeat, slot-0 readback). Closing the header disarms them.
        if (_fn_diag_keepalive != null) _fn_diag_keepalive(2);
        log_debug_ui();   // release prep: the version, "diagnostics: N on", diag_verbose, the log's size (logging.cs)

        ImGui.Text($"{ImGui.GetIO().Framerate:F1} fps   native: {esc(_native_status)}   detours: {esc(_detour_status)}");
        ImGui.Checkbox("Fahrenheit's text ignores the Windows display scale (fh_dpi_fix, as Fahrenheit #376)", ref _fh_dpi_fix);
        ImGui.SameLine(); ImGui.TextDisabled(I($"Windows scale {ImGui.GetMainViewport().DpiScale:F2}x; Fahrenheit's own: {(_dpi_fh_fonts == 1 ? "scaled" : _dpi_fh_fonts == 0 ? "not scaled" : "?")}; turned off {_dpi_offs}x; our text {ImGui.GetFontSize():F1} px (pushed {_our_px:F1}, overlay_text_scale {_overlay_text_scale:F2})"));
        diag7_ui();   // build 7: status line, "show cursor anchors"
        if (_fn_counter != null)
            ImGui.Text($"draws: indexed {_fn_counter(1)}, plain {_fn_counter(2)}   UpdateSubresource {_fn_counter(7)}   buffers {(_fn_buffer_count != null ? _fn_buffer_count() : -1)}");
        slow_debug_ui();   // the slow-GPU guard: its two settings and its state line (slow_guard.cs)
        odg_debug_ui();    // overdrive gestures: the test aid (overdrives always ready), Auron's raw sequence (od_gest.cs)
        timer_debug_ui();  // grid-timer: timer_fix, limiter_probe (cpu_report.cs)
        fmv_debug_ui();    // fmv-sync (S2): the movie playing, its frame, the view, the depth file (fmv_sync.cs)
        fmv3d_debug_ui();  // fmv-relief (S3): FMV 3D: the relief's state, decode times, ring fill; fmv_depth_tight_fov, the session-only depth offset (fmv_relief.cs)

        if (ImGui.TreeNode("Camera (from the game's constant buffers)")) {
            float* c = stackalloc float[48];
            if (_fn_get_camera == null || _fn_get_camera(c) == 0) ImGui.Text("not read yet");
            else {
                float t0 = c[3], t1 = c[7], t2 = c[11];
                float px = -(c[0] * t0 + c[4] * t1 + c[8]  * t2);
                float py = -(c[1] * t0 + c[5] * t1 + c[9]  * t2);
                float pz = -(c[2] * t0 + c[6] * t1 + c[10] * t2);
                float yaw   = MathF.Atan2(c[8], c[10]) * 180f / MathF.PI;
                float pitch = MathF.Asin(Math.Clamp(-c[9], -1f, 1f)) * 180f / MathF.PI;
                ImGui.Text($"yaw {yaw,7:F1}  pitch {pitch,6:F1}   position {px,9:F2} {py,9:F2} {pz,9:F2}");
                float m0 = c[16 + 0], m5 = c[16 + 5];
                if (MathF.Abs(m0) > 1e-6f && MathF.Abs(m5) > 1e-6f)
                    ImGui.Text($"game FOV: {2f * MathF.Atan(1f / m5) * 180f / MathF.PI:F1} vertical, {2f * MathF.Atan(1f / m0) * 180f / MathF.PI:F1} horizontal");
                if (ImGui.TreeNode("matrices")) {
                    for (int r = 0; r < 4; r++) ImGui.Text($"{c[r*4+0],10:F4} {c[r*4+1],10:F4} {c[r*4+2],10:F4} {c[r*4+3],10:F4}");
                    ImGui.Spacing();
                    for (int r = 0; r < 4; r++) ImGui.Text($"{c[16+r*4+0],10:F4} {c[16+r*4+1],10:F4} {c[16+r*4+2],10:F4} {c[16+r*4+3],10:F4}");
                    ImGui.TreePop();
                }
            }
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Camera override")) {
            ImGui.Checkbox("enabled", ref _ovr_enabled); ImGui.SameLine();
            ImGui.Checkbox("freeze game camera", ref _ovr_freeze); ImGui.SameLine();
            if (ImGui.Button("reset")) { _ovr_yaw = _ovr_pitch = 0; _ovr_right = _ovr_up = _ovr_forward = 0; }
            ImGui.SliderFloat("yaw",     ref _ovr_yaw,     -180f, 180f);
            ImGui.SliderFloat("pitch",   ref _ovr_pitch,    -89f,  89f);
            ImGui.SliderFloat("right",   ref _ovr_right,  -2000f, 2000f);
            ImGui.SliderFloat("up",      ref _ovr_up,     -2000f, 2000f);
            ImGui.SliderFloat("forward", ref _ovr_forward, -2000f, 2000f);
            int mask = _fn_override_mask != null ? _fn_override_mask() : 0;
            ImGui.Text($"buffers patched: {(_fn_override_applied != null ? _fn_override_applied() : -1)}   blocks:"
                     + ((mask & 1)  != 0 ? " view" : "") + ((mask & 2)  != 0 ? " inv" : "") + ((mask & 4) != 0 ? " pos" : "")
                     + ((mask & 8)  != 0 ? " VP" : "")   + ((mask & 16) != 0 ? " VW" : "")  + ((mask & 32) != 0 ? " PVW" : "")
                     + ((mask & 64) != 0 ? " IVW" : "") + ((mask & 128) != 0 ? " shifted" : "") + ((mask & 256) != 0 ? " product-only" : ""));
            ImGui.TextDisabled("Numpad: 4/6 yaw, 8/2 pitch, +/- forward/back, 7/9 up/down, 5 reset (numpad 0 is the fps mod's turbo)");
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Second eye")) {
            ImGui.Checkbox("render eye 2", ref _stereo_on);
            ImGui.SliderFloat("eye separation without headset", ref _ipd, 0f, 30f);
            ImGui.Checkbox("preview window", ref _eye2_preview); ImGui.SameLine();
            ImGui.RadioButton("final", ref _eye2_source, 0); ImGui.SameLine();
            ImGui.RadioButton("scene", ref _eye2_source, 1); ImGui.SameLine();
            ImGui.RadioButton("scene eye 1", ref _eye2_source, 2);
            if (_fn_eye2_draws != null) ImGui.Text($"eye-2 draws last frame: {_fn_eye2_draws()}" + (_fn_eye2_same_inputs != null ? $"   (with the same inputs as eye 1, at most: {_fn_eye2_same_inputs()})" : ""));
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Headset tests (frame hold, swapchain timeouts)")) {
            ImGui.Checkbox("drop the eye image every frame (the headset holds the last frame for 30 frames, then goes black; untick to resume)", ref _xr_drop_image);
            if (ImGui.SmallButton("simulate 20 swapchain wait timeouts")) _xr_sim_timeouts_req = 20; ImGui.SameLine();
            if (ImGui.SmallButton("simulate 60")) _xr_sim_timeouts_req = 60; ImGui.SameLine();
            ImGui.TextDisabled("ffxvr_diag.log: 'swapchain N wait ... (try N)' then 'recovered after N tries'");
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("UI panel")) {
            ImGui.SliderFloat("distance (m)", ref _ui_dist, 0.5f, 4f);
            ImGui.SliderFloat("width (m)", ref _ui_width, 0.5f, 4f);
            uidepth_debug_ui();   // adaptive UI panel: ui_adaptive, ui_adaptive_min, ui_adaptive_margin, ui_backing, the projection slot, the status
            if (_fn_ui_draws != null) ImGui.Text($"UI draws last frame: {_fn_ui_draws()}");
            if (_fn_dream_status != null) ImGui.TextDisabled("dream filter: " + esc(dream_status_text()));
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Virtual screen panel")) {
            ImGui.SliderFloat("width (m)", ref _xr_screen_width, 1f, 6f);
            ImGui.SliderFloat("distance (m)", ref _xr_screen_dist, 1f, 6f);
            ImGui.TextDisabled("both apply at once; Home re-places the screen in front of you");
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Character shadows (the ShadowManager's light camera)")) {
            shadow_debug_ui();
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Heat haze (the shimmer over fire)")) {
            haze_debug_ui();
            ImGui.TreePop();
        }
        if (ImGui.TreeNode("Stick reprojection and the loading keepalive")) {
            sr_debug_ui();
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Effects without a camera (fog, water: ghosting experiment)")) {
            ImGui.TextWrapped("Scene draws whose constant buffers carry no camera we recognise are drawn identically in both eyes, which shows as ghosting. Try each setting in a foggy or watery spot, then press Frame census so the log lists those draws.");
            ImGui.RadioButton("draw in both eyes (default)", ref _noimg_mode, 0); ImGui.SameLine();
            ImGui.RadioButton("eye 1 only", ref _noimg_mode, 1); ImGui.SameLine();
            ImGui.RadioButton("hide", ref _noimg_mode, 2);
            if (_fn_noimg_draws != null) { ImGui.SameLine(); ImGui.TextDisabled($"such draws so far: {_fn_noimg_draws()}"); }
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Culling (code patches)")) {
            ImGui.TextWrapped("Patches to the game's visibility tests, verified against the expected bytes before writing; untick restores. The two checkboxes above (characters, scene) are the normal way to drive these.");
            if (_fn_patch_set != null && _fn_patch_count != null) {
                int np = Math.Min(_fn_patch_count(), _patch_on.Length);
                for (int i = 0; i < np; i++) {
                    string name = (_fn_patch_name != null ? System.Runtime.InteropServices.Marshal.PtrToStringAnsi((nint)_fn_patch_name(i)) : null) ?? $"patch {i}";
                    bool on = _patch_on[i];
                    if (ImGui.Checkbox(name, ref on)) { if (_fn_patch_set(i, on ? 1 : 0) == 0) _patch_on[i] = on; }
                }
            }
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Find a draw (bisect scene draws)")) {
            int total = _fn_scene_draws != null ? (int)_fn_scene_draws() : 0;
            ImGui.TextWrapped($"Scene draws last frame: {total}. Hide a range of them (or show only that range) and narrow it down until the thing you want to identify disappears; then Frame census lists the draw's shaders. Draw order is stable while you stand still.");
            ImGui.RadioButton("off", ref _bisect_mode, 0); ImGui.SameLine();
            ImGui.RadioButton("hide range", ref _bisect_mode, 1); ImGui.SameLine();
            ImGui.RadioButton("show only range", ref _bisect_mode, 2);
            int hi = Math.Max(total, 1);
            ImGui.SliderInt("from", ref _bisect_a, 0, hi); ImGui.SliderInt("to", ref _bisect_b, 0, hi);
            if (ImGui.Button("first half")) { _bisect_a = 0; _bisect_b = hi / 2; } ImGui.SameLine();
            if (ImGui.Button("second half")) { _bisect_a = hi / 2 + 1; _bisect_b = hi; } ImGui.SameLine();
            if (ImGui.Button("split: lower")) { _bisect_b = (_bisect_a + _bisect_b) / 2; } ImGui.SameLine();
            if (ImGui.Button("split: upper")) { _bisect_a = (_bisect_a + _bisect_b) / 2 + 1; }
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("First person / follow")) {
            ImGui.Checkbox("in battle the diorama centres on the fight, not the player", ref _battle_anchor); ImGui.SameLine(); ImGui.TextDisabled(_battle_anchor_n > 0 ? $"combatants {_battle_anchor_n}" : "not in battle");
            ImGui.Checkbox("in battle, first person follows the field: Tidus when he is on it, swaps move the eyes, a summon gives them to Yuna", ref _battle_follow); ImGui.SameLine(); ImGui.TextDisabled(_btl_who.Length > 0 ? $"now: {esc(_btl_who)}" : "");   // an actor name read from the game
            ImGui.TextDisabled("Tidus joining the field takes the eyes; left grip + right stick click (or Insert) moves them to the next party member");
            btg_ui();   // battle tag: when a swap hands the eyes on (bt_tag.cs)
            ImGui.Checkbox("smooth follow anchor: the eyes follow the drawn (60 fps) body, not the 30 Hz logic position", ref _smooth_anchor); ImGui.SameLine();
            ImGui.TextDisabled($"{esc(_sa_status)} (snapshot {(_sa_age >= 999 ? "none" : $"{_sa_age} frames old")}{(_sa_rejects > 0 ? $", {_sa_rejects} refused as implausible" : "")}){(_sa_flat > 60 ? " - the drawn root equals the logic position: is FFX-60fps smoothing on (F7)?" : "")}");
            ride_ui();   // chocobo: the riding status and its options (ride.cs)
            if (_c2_active) ImGui.TextWrapped("Game cursor source: in the headset the game's target hand and turn triangle are skipped where the game draws them, and our markers replace them. The pointer rule and the two default cursor rows below are off; the hide list still hides what picks added. The \"legacy cursor source\" checkbox under Battle markers brings the old behaviour back (unticking the sprite-signature checkbox does not).");
            if (_c2_active) ImGui.BeginDisabled(true);
            ImGui.Checkbox("hide the game's battle pointers by position, headset only: any small plain or 1024-atlas quad in the column above where a character projects through the game's camera", ref _hide_pointers); ImGui.SameLine();
            if (_c2_active) { ImGui.EndDisabled(); ImGui.TextDisabled("(off: game cursor source)"); }
            else ImGui.TextDisabled($"hidden last frame: {_pointer_stats & 0xFFFF}, characters projected: {_pointer_stats >> 16}");
            ImGui.Checkbox("hide the game's battle cursors by sprite signature, headset only (the list is filled by picking, below)", ref _hide_cursor); ImGui.SameLine();
            ImGui.TextDisabled($"candidates {_rect_stats >> 16}, rect unknown {_rect_stats & 0xFFFF}");
            for (int i = 0; i < _hide_list.Count; i++) {
                float[] e = _hide_list[i];
                int slot = -1; for (int k = 0; k < _hide_push_n; k++) if (_hide_push_src[k] == i) slot = k;   // C2-1: the pushed slot of this row (the game source drops the two defaults)
                string hits = slot >= 0 ? $"hidden last frame: {_hide_hits[slot]}" : _c2_active && c2_is_default(e) ? "(off: game cursor source)" : "(not pushed: native takes 16)";
                string row = $"  {(e[0] == 0 ? "textureless" : $"{e[0]:F0}x{e[1]:F0} atlas")} quad {e[2]:F0}-{e[3]:F0} x {e[4]:F0}-{e[5]:F0} units   {hits}";
                if (slot >= 0) ImGui.Text(esc(row)); else ImGui.TextDisabled(esc(row)); ImGui.SameLine();
                if (ImGui.SmallButton($"remove##hl{i}")) { _hide_list.RemoveAt(i); _hide_list_dirty = true; break; }
            }
            if (ImGui.SmallButton("reset the list to the two known cursors")) { _hide_list.Clear(); _hide_list.AddRange(hide_list_defaults()); _hide_list_dirty = true; }
            ImGui.Checkbox("blitzball: put the scoreboard and radar on the UI panel (they are drawn into the scene with no camera: double vision)", ref _blitz_overlay); ImGui.SameLine();
            ImGui.TextDisabled(_in_blitz ? $"routed last frame: {_blitz_overlay_n}" : "not in blitzball");
            ImGui.Text("cursor experiment - hide 2D draws:"); ImGui.SameLine();
            ImGui.RadioButton("off##uh", ref _ui_hide, 0); ImGui.SameLine(); ImGui.RadioButton("1024x1024 atlas, in battle##uh", ref _ui_hide, 3); ImGui.SameLine();
            ImGui.RadioButton("1024x1024 atlas, always##uh", ref _ui_hide, 1); ImGui.SameLine(); ImGui.RadioButton("+ textureless draws##uh", ref _ui_hide, 2); ImGui.SameLine();
            ImGui.RadioButton("textureless only, in battle##uh", ref _ui_hide, 4); ImGui.SameLine();
            ImGui.RadioButton("textureless except window backgrounds, in battle##uh", ref _ui_hide, 5); ImGui.SameLine(); ImGui.TextDisabled($"hidden last frame: {_ui_hidden_n}");
            ImGui.Text("battles start in:"); ImGui.SameLine();
            ImGui.RadioButton("the current view##bv", ref _battle_view, -1); ImGui.SameLine(); ImGui.RadioButton("first person##bv", ref _battle_view, 1); ImGui.SameLine();
            ImGui.RadioButton("third person##bv", ref _battle_view, 3); ImGui.SameLine(); ImGui.RadioButton("diorama##bv", ref _battle_view, 2); ImGui.SameLine(); ImGui.TextDisabled("(the view from before returns when the battle ends)");
            ImGui.SliderFloat("diorama floor: never below this many units above the player's feet (0 = off; suspended while a grip is held)", ref _diorama_floor, 0f, 40f, "%.0f");
            ImGui.Text("eye height:"); ImGui.SameLine();
            ImGui.RadioButton("fixed", ref _fp_head_anchor, 0); ImGui.SameLine();
            ImGui.RadioButton("head bone height (swimming, crouching)", ref _fp_head_anchor, 1); ImGui.SameLine();
            ImGui.RadioButton("full head bone (leans too)", ref _fp_head_anchor, 2);
            ImGui.SliderFloat("head, in battle: ignore points further out than (units; 0 = off)", ref _fp_head_reach, 0f, 40f, "%.1f");   // every anchor mode: the pick also feeds the displaced-body eye in mode 0
            if (_fp_head_anchor != 0) {
                ImGui.SliderFloat("eye offset above the head bone", ref _fp_head_up, -5f, 10f);
                ImGui.SliderFloat("head smoothing (1 = none, 0.1 = heavy)", ref _fp_head_smooth, 0.05f, 1f);
                if (_fp_head_anchor == 2) ImGui.SliderFloat("eye offset forward of the head bone (the bone is the neck joint)", ref _fp_head_fwd, 0f, 6f);
            }
            else ImGui.SliderFloat("eye height (units)", ref _fp_eye_h, 0f, 40f);
            ImGui.SliderFloat("follow distance", ref _fp_follow_dist, 10f, 300f);
            ImGui.SliderFloat("follow height", ref _fp_follow_h, 0f, 150f);
            ImGui.Text("third person:"); ImGui.SameLine(); ImGui.SetNextItemWidth(120); ImGui.SliderFloat("distance", ref _tp_dist, 4f, 120f, "%.0f"); ImGui.SameLine();
            ImGui.SetNextItemWidth(120); ImGui.SliderFloat("height", ref _tp_h, 0f, 60f, "%.0f"); ImGui.SameLine();
            ImGui.SetNextItemWidth(120); ImGui.SliderFloat("pitch", ref _tp_pitch, -60f, 30f, "%.0f deg"); ImGui.SameLine();
            ImGui.SetNextItemWidth(140); ImGui.SliderFloat("heading follows the run", ref _tp_follow_rate, 0f, 180f, "%.0f deg/s");
            ImGui.SameLine(); ImGui.TextDisabled("0 = fixed heading like first person (recommended); grips zoom the distance, not the world");
            ImGui.SetNextItemWidth(160); ImGui.SliderFloat("anchor smoothing (ms, 0 = snap)", ref _anchor_smooth_ms, 0f, 150f, "%.0f"); ImGui.SameLine();
            ImGui.SetNextItemWidth(160); ImGui.SliderFloat("battle anchor smoothing (ms, 0 = snap)", ref _battle_anchor_smooth_ms, 0f, 1000f, "%.0f"); ImGui.SameLine();
            ImGui.TextDisabled("third person / diorama follow the player, and the battle diorama its centre, with this lag (24 / 325 to try)");
            ImGui.Checkbox("hide the player's body", ref _fp_hide_body); ImGui.SameLine();
            ImGui.SliderFloat("hide radius", ref _fp_hide_radius, 0.1f, 12f); ImGui.SameLine(); ImGui.TextDisabled("the player's parts sit exactly on the actor position; an NPC you talk to is 1+ units away, so keep this small");
            ImGui.Text("in first person:"); ImGui.SameLine();
            ImGui.RadioButton("hide my whole body", ref _fp_body_mode, 0); ImGui.SameLine();
            ImGui.RadioButton("show my body, hide the head parts", ref _fp_body_mode, 1); ImGui.SameLine();
            ImGui.RadioButton("show everything", ref _fp_body_mode, 2);
            ImGui.SetNextItemWidth(160); ImGui.InputText("head parts (indices, e.g. 2,3)", ref _fp_head_parts, 64); ImGui.SameLine();
            ImGui.TextDisabled($"player parts drawn into the scene last frame: {(_fn_fp_parts != null ? _fn_fp_parts() : 0)}");
            hands2_debug_ui();   // VR hands v2: the debug bits, the pose dump (hands.cs)
            if (!_hands_dev) { _hands_on = false; }   // VR hands are tabled: hidden unless hands_dev=1 in the ini (docs/design-notes.md, "VR hands: state of play")
            if (_hands_dev && _fn_bones_capture != null && ImGui.Button($"capture bone palettes (one frame -> ffxvr_bones_{_bones_take + 1}.bin)")) {
                float* op = stackalloc float[78]; Actor* actors = Globals.actors;
                if (actors != null && _fp_actor_idx >= 0 && read(actors + _fp_actor_idx, out Actor pa)) { for (int k = 0; k < 26; k++) { op[k*3] = pa.chr_optpos[k].X; op[k*3+1] = pa.chr_optpos[k].Y; op[k*3+2] = pa.chr_optpos[k].Z; } }
                _bones_take = _fn_bones_capture(op, 64);
                _logger.Info($"bone palette capture armed: take {_bones_take} -> ffxvr_bones_{_bones_take}.bin + 'bones:' lines in ffxvr_diag.log");
            }
            if (_hands_dev && _fn_hands_set != null) {
                ImGui.Separator();
                ImGui.Checkbox("VR hands (first person): Tidus's arms follow the controllers", ref _hands_on); ImGui.SameLine();
                ImGui.Checkbox("collapse the head bones", ref _hands_head);
                ImGui.Text("draw:"); ImGui.SameLine();
                ImGui.RadioButton("whole body", ref _hands_show, 0); ImGui.SameLine(); ImGui.RadioButton("arms", ref _hands_show, 1); ImGui.SameLine();
                ImGui.RadioButton("forearms + hands", ref _hands_show, 2); ImGui.SameLine(); ImGui.RadioButton("hands only", ref _hands_show, 3);
                if (_fn_hands_status != null) { uint hf; nint why = _fn_hands_status(&hf); ImGui.SameLine(); ImGui.TextDisabled($"{esc(System.Runtime.InteropServices.Marshal.PtrToStringAnsi(why))}  frames {hf}"); }
                ImGui.SetNextItemWidth(90); ImGui.SliderFloat("hand yaw", ref _hands_yaw, -180f, 180f, "%.0f"); ImGui.SameLine();
                ImGui.SetNextItemWidth(90); ImGui.SliderFloat("pitch", ref _hands_pitch, -180f, 180f, "%.0f"); ImGui.SameLine();
                ImGui.SetNextItemWidth(90); ImGui.SliderFloat("roll", ref _hands_roll, -180f, 180f, "%.0f"); ImGui.SameLine();
                ImGui.SetNextItemWidth(70); ImGui.SliderFloat("out", ref _hands_off_r, -0.15f, 0.15f, "%.2f m"); ImGui.SameLine();
                ImGui.SetNextItemWidth(70); ImGui.SliderFloat("up", ref _hands_off_u, -0.15f, 0.15f, "%.2f m"); ImGui.SameLine();
                ImGui.SetNextItemWidth(70); ImGui.SliderFloat("back", ref _hands_off_b, -0.15f, 0.15f, "%.2f m");
                ImGui.TextDisabled("calibration: turn the hand about the grip until the fingers point along the controller; offsets move the wrist off the grip");
                if (_fn_hands_calibrate != null && _fn_hands_cal_data != null) {
                    double now = ImGui.GetTime();
                    if (ImGui.Button("calibrate: 10 s countdown")) _hands_cal_at = now + 10.0;
                    ImGui.SameLine();
                    if (_hands_cal_at > 0) {
                        if (now >= _hands_cal_at) { _fn_hands_calibrate(); _hands_cal_at = -1; }
                        else ImGui.Text($"{(int)Math.Ceiling(_hands_cal_at - now)} ... hold both arms STRAIGHT OUT IN FRONT, shoulder width, PALMS DOWN, fingers forward");
                    }
                    else ImGui.TextDisabled("pose: arms straight out in front, palms down; the buzz = captured");
                    ImGui.SameLine(); if (ImGui.SmallButton("reset calibration")) { float* id = stackalloc float[18]; for (int i = 0; i < 18; i++) id[i] = (i % 9) % 4 == 0 ? 1f : 0f; _fn_hands_cal_data(id, 1); _hands_cal_ini = ""; if (_fn_hands_side != null) _fn_hands_side(-2, _hands_stretch); _hands_px_left = -1; }
                    float* rc = stackalloc float[18]; uint seen = _fn_hands_cal_data(rc, 0);
                    if (seen != _hands_cal_seen) {   // a capture happened: persist it
                        _hands_cal_seen = seen; var sb2 = new System.Text.StringBuilder(); for (int i = 0; i < 18; i++) { if (i > 0) sb2.Append(','); sb2.Append(rc[i].ToString("F4", System.Globalization.CultureInfo.InvariantCulture)); }
                        _hands_cal_ini = sb2.ToString();
                    }
                }
                ImGui.SetNextItemWidth(120); ImGui.SliderFloat("arm stretch (to reach a real arm's length)", ref _hands_stretch, 1f, 2f, "%.2fx");
                if (_fn_hands_dump != null) { if (ImGui.SmallButton("dump one frame of bone groups to ffxvr_diag.log")) _fn_hands_dump(); }
                ImGui.SameLine(); ImGui.Checkbox("NaN cut at the seam", ref _hands_nan); ImGui.SameLine(); ImGui.Checkbox("split draw per hand", ref _hands_split);
                if (_fn_hands_method != null) _fn_hands_method(_hands_nan ? 1 : 0, _hands_split ? 1 : 0);
                ImGui.Text("sorting out the sides:"); ImGui.SameLine();
                if (ImGui.SmallButton("the arm models are on the wrong sides: flip")) { if (_fn_hands_side != null) { int r2 = _fn_hands_side(-3, _hands_stretch); _hands_px_left = r2 & 1; } } ImGui.SameLine();
                ImGui.Checkbox("swap arms", ref _hands_swap); ImGui.SameLine(); ImGui.Checkbox("mirror targets", ref _hands_mirror); ImGui.SameLine();
                ImGui.RadioButton("both", ref _hands_only, 0); ImGui.SameLine(); ImGui.RadioButton("left controller only", ref _hands_only, 1); ImGui.SameLine(); ImGui.RadioButton("right only", ref _hands_only, 2);
            }
            if (_hands_dev && _fn_bone_wiggle != null && _fn_bone_status != null) {
                int* slots = stackalloc int[32]; int matches; int nrec = _fn_bone_status(slots, &matches);
                ImGui.Text("bone finder:"); ImGui.SameLine(); ImGui.Checkbox("lift one bone", ref _wig_on); ImGui.SameLine();
                ImGui.SetNextItemWidth(100); ImGui.SliderInt("record", ref _wig_rec, 0, Math.Max(0, nrec - 1)); ImGui.SameLine();
                int nsl = (_wig_rec >= 0 && _wig_rec < nrec) ? slots[_wig_rec] : 0;
                ImGui.SetNextItemWidth(100); ImGui.SliderInt("slot", ref _wig_slot, 0, Math.Max(0, nsl - 1)); ImGui.SameLine();
                ImGui.SetNextItemWidth(80); ImGui.SliderFloat("units up", ref _wig_amt, 0.5f, 10f, "%.1f");
                if (ImGui.IsKeyPressed(ImGuiKey.PageUp)) _wig_slot = Math.Min(Math.Max(0, nsl - 1), _wig_slot + 1);
                if (ImGui.IsKeyPressed(ImGuiKey.PageDown)) _wig_slot = Math.Max(0, _wig_slot - 1);
                var sb = new System.Text.StringBuilder(); for (int r = 0; r < nrec; r++) { if (r > 0) sb.Append(' '); sb.Append(slots[r]); }
                ImGui.TextDisabled($"PgUp/PgDn: next/previous slot.   player records last frame: {nrec} (slots: {sb})   this bone also in {matches} other slots");
                _fn_bone_wiggle(_wig_rec, _wig_slot, _wig_on ? _wig_amt : 0f);
                // Labelling: a button per body part names the current slot, saves, and steps to the next slot.
                string cur = _bone_labels.TryGetValue((_wig_rec, _wig_slot), out string? cl) ? cl : "(unlabelled)";
                ImGui.Text($"record {_wig_rec} slot {_wig_slot}: {esc(cur)}"); ImGui.SameLine();
                if (ImGui.SmallButton("clear")) { _bone_labels.Remove((_wig_rec, _wig_slot)); bone_labels_save(); }
                for (int i = 0; i < k_bone_names.Length; i++) {
                    if (i % 8 != 0) ImGui.SameLine();
                    if (ImGui.SmallButton(k_bone_names[i])) { _bone_labels[(_wig_rec, _wig_slot)] = k_bone_names[i]; bone_labels_save(); _wig_slot = Math.Min(Math.Max(0, nsl - 1), _wig_slot + 1); }
                }
                int labelled = 0; foreach (var kv in _bone_labels) if (kv.Key.Item1 == _wig_rec) labelled++;
                ImGui.TextDisabled($"labelled {labelled} of {nsl} slots in record {_wig_rec}; saved to bone_labels.txt");
                if (_fn_bones_seq != null && _fn_bones_seq_left != null) {
                    int left = _fn_bones_seq_left();
                    if (ImGui.Button($"record {_seq_frames} frames of record {_wig_rec}'s palette (then move around)")) _fn_bones_seq(_wig_rec, _seq_frames);
                    ImGui.SameLine(); ImGui.SetNextItemWidth(100); ImGui.SliderInt("frames", ref _seq_frames, 60, 1200);
                    ImGui.SameLine(); ImGui.TextDisabled(left > 0 ? $"recording: {left} frames left" : "-> ffxvr_bones_seq.bin");
                }
                if (ImGui.TreeNode("labels so far")) {
                    foreach (string name in k_bone_names) {
                        var sl = new List<string>(); foreach (var kv in _bone_labels) if (kv.Value == name) sl.Add(kv.Key.Item1 == _wig_rec ? kv.Key.Item2.ToString() : $"{kv.Key.Item1}:{kv.Key.Item2}");
                        if (sl.Count > 0) ImGui.Text($"{name,-16} {string.Join(" ", sl)}");
                    }
                    ImGui.TreePop();
                }
            }
            ImGui.SetNextItemWidth(200); ImGui.SliderInt("show only part # (-1 = off; use in third person to find the head)", ref _fp_solo, -1, 31);
            if (_fn_fp_hidden != null) ImGui.Text($"draws hidden: {_fn_fp_hidden()}");
            body_hide_ui();   // the on-foot ownership: radius, NPC parts kept visible (body_hide.cs)
            ImGui.InputText("player model names (comma list, best first)", ref _fp_actor, 64); ImGui.SameLine();
            ImGui.TextDisabled((_fp_actor_idx < 0 ? "no live actor" : _fp_how.Length > 0 ? esc(_fp_how) : $"actor {_fp_actor_idx}") + (_ride_att ? " | " + esc(_ride_status) : ""));
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Actors (field actor table; the one that moves when you walk is the player)")) {
            draw_actors();
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Pick a UI element (flat mode: hover it with the mouse, press F9)")) {
            ImGui.TextWrapped("F9 logs every 2D UI draw under the mouse to ffxvr_diag.log (texture, index count, rectangle, constant buffer) and lists them here. F10 toggles slow motion so things that flash by (damage numbers) can be hovered.");
            ImGui.SliderInt("slow motion: ms of sleep per frame", ref _slowmo_ms, 20, 1000); ImGui.SameLine(); ImGui.Checkbox("on (F10)", ref _slowmo_on);
            ImGui.Text("highlight (F11 cycles):"); ImGui.SameLine();
            ImGui.RadioButton("off##hv", ref _hover_mode, 0); ImGui.SameLine(); ImGui.RadioButton("the quads under the mouse##hv", ref _hover_mode, 1); ImGui.SameLine(); ImGui.RadioButton("every UI quad the mod sees##hv", ref _hover_mode, 2);
            ImGui.TextDisabled("yellow = under the mouse (what F9 would pick), red = the pointer rule would hide it, green cross = a character's position through the game camera, white = other sprites");
            ImGui.Text($"picks so far: {_pick_n}");
            if (_pick_text.Length == 0) ImGui.TextDisabled("(nothing picked yet)");
            else {
                string[] hits = _pick_text.Split('\n'); int hi = 0;
                foreach (string h in hits) {
                    int bar = h.IndexOf("|sig="); string text = bar >= 0 ? h[..bar] : h;
                    ImGui.TextWrapped(esc(text));
                    if (bar >= 0) {
                        string[] f = h[(bar + 5)..].Split(','); ImGui.SameLine();
                        if (f.Length == 4 && ImGui.SmallButton($"hide this##pk{hi}")) {
                            var ci = System.Globalization.CultureInfo.InvariantCulture;
                            float tw = float.Parse(f[0], ci), th = float.Parse(f[1], ci), w = float.Parse(f[2], ci), hh = float.Parse(f[3], ci);
                            // width gets room for pulsing (about half to 1.3x), height a few units; no duplicates
                            float[] e = { tw, th, MathF.Floor(w * 0.5f), MathF.Ceiling(w * 1.3f), MathF.Floor(hh - 3), MathF.Ceiling(hh + 3) };
                            bool dup = _hide_list.Exists(x => x[0] == e[0] && x[1] == e[1] && x[2] == e[2] && x[3] == e[3] && x[4] == e[4] && x[5] == e[5]);
                            if (!dup && _hide_list.Count < 16) { _hide_list.Add(e); _hide_list_dirty = true; _logger.Info($"hide list: added {tw}x{th} quad {w}x{hh} from a pick"); }
                        }
                    }
                    hi++;
                }
            }
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Battle loading (diagnostics, HD cluster cache)")) {
            ImGui.TextWrapped($"load diagnostics: {esc(_ld_status)}");
            ImGui.Checkbox("load lines (diag_load_lines): one \"load:\" line per battle entry, way back, area / event map change, save load and the title's load in ffxvr_diag.log", ref _diag_load_lines);
            ImGui.Checkbox("load diagnostics (diag_load): add the expensive parts to those lines (address-space walks, the chr cache table, the wait callers' stack walks, the HDC: line per map free)", ref _diag_load);
            ImGui.Checkbox("actors line (diag_actors): every 10 s, the live actors that are not drawn, in Fahrenheit's log", ref _diag_actors);
            ImGui.Checkbox("cluster bytes (diag_cluster_bytes; hooks at the next launch): the game pool's allocations inside each HD cluster load, for its resident size. Off: the Phyre header size stands in. Measurement only (hot path)", ref _diag_cluster_bytes);
            ImGui.Checkbox("ntdll reservation log (diag_va_log; hooks at the next launch): name the caller of every low-2 GB reservation. Off: a patch-free region diff at each map free", ref _diag_va_log);
            bt_debug_ui();
            lp_debug_ui();
            hc_ui();
            ImGui.TextDisabled($"hooks seen working 0x{seen_mask:X} (0x1FF the core nine; 0x200 open, 0x400 read, 0x800 post fixups, 0x1000 / 0x2000 pool alloc / free, 0x4000 / 0x8000 texture section / create)");
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Battle markers")) {
            ImGui.Checkbox("draw target / turn markers", ref _markers_on); ImGui.SameLine();
            ImGui.Checkbox("as 3D geometry in the headset", ref _markers_3d);
            ImGui.TextWrapped($"cursor source: {esc(_c2_status)}");
            bool legacy_src = !_cursor_game_cfg; if (ImGui.Checkbox("legacy cursor source (the markers from battle-window memory, cursor hiding by shape)", ref legacy_src)) _cursor_game_cfg = !legacy_src;
            if (_c2_active) ImGui.SliderFloat("pointer lift above the game's anchor", ref _marker_lift, -5f, 20f);
            else ImGui.SliderFloat("pointer clearance above the head", ref _marker_height, -20f, 60f);
            ImGui.SliderFloat("pointer size", ref _marker_scale, 0.3f, 3f);
            ImGui.Checkbox("the game's look (the hand from battle.dds, the game's spinning triangle; needs the game cursor source)", ref _cursor_look_game);
            if (_cursor_look_game) {
                ImGui.SameLine(); ImGui.TextDisabled(_c3_atlas_ok ? "battle.dds: found" : "battle.dds: not found yet (it is found at the first target menu of a battle; red triangles until then)");
                ImGui.SliderFloat("hand size", ref _cursor_hand_size, 0.3f, 3f);
                ImGui.Checkbox("sample the atlas the other way up (only if the hand shows the wrong art)", ref _c3_vflip_inv); ImGui.SameLine();
                if (_fn_cursor_atlas_dump != null && ImGui.SmallButton("dump the battle atlas")) _fn_cursor_atlas_dump();
                ImGui.SameLine(); ImGui.TextDisabled("-> battle_atlas.dds next to the mod, details in ffxvr_diag.log");
            }
            ImGui.Text("first person, on the actor we look out of:"); ImGui.SameLine();
            ImGui.RadioButton("low (no turn marker; a self-target small at chest height)##fps", ref _fp_self_marker, 0); ImGui.SameLine();
            ImGui.RadioButton("hide##fps", ref _fp_self_marker, 1); ImGui.SameLine(); ImGui.RadioButton("normal##fps", ref _fp_self_marker, 2);
            ImGui.Text($"drawn this frame: {_markers_drawn}");
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Captures (files land next to the mod, collected by run-ffx.ps1)")) {
            if (_fn_frame_arm != null) { if (ImGui.Button("Frame census")) { _fn_frame_arm(); _frame_census_n++; } ImGui.SameLine(); ImGui.Text($"{_frame_census_n}  ffxvr_frame.log: one frame's targets, draws, resolves (the automatic census at the first glass break goes to ffxvr_frame_auto.log)"); }
            if (_fn_motion_log != null) { if (ImGui.Button("Motion log (3 s)")) _fn_motion_log(180); ImGui.SameLine(); ImGui.Text("per drawn frame: actor position vs rendered position, view yaw, head yaw -> ffxvr_diag.log (walk and turn with the stick while it runs; Numpad 5 also starts it)"); }
            if (_fn_ui_atlas_dump != null) { if (ImGui.Button("Dump UI atlases")) _fn_ui_atlas_dump(); ImGui.SameLine(); ImGui.Text("the 1024x1024 sprite sheets the UI samples this frame -> ui_atlas_*.dds next to the mod (do it in a battle with a target selected)"); }
            if (_fn_census_arm != null) { if (ImGui.Button("Stack census (character draws)")) _fn_census_arm(1872, 16384); ImGui.SameLine(); ImGui.Text("ffxvr_stacks.log: the game's call chains behind one frame's skinned-character uploads (where the draw/cull loop lives)"); }
            if (_fn_dump_cb != null) {
                ImGui.SetNextItemWidth(90); ImGui.InputInt("##cbsize", ref _dump_cb_size, 0); ImGui.SameLine();
                if (ImGui.Button("Dump buffer layout")) _fn_dump_cb((uint)Math.Max(_dump_cb_size, 64), 2);
                ImGui.SameLine(); ImGui.Text("ffxvr_diag.log: the next 2 uploads of that byte size, block by block (hut walls: 1456)");
            }
            if (_fn_measure != null)   { if (ImGui.Button("Measure + images")) { _fn_measure(); _measure_n++; } ImGui.SameLine(); ImGui.Text($"{_measure_n}  ffxvr_diag.log + img_*.bmp: eye 1 vs eye 2"); }
            if (_fn_dump_seq != null)  { if (ImGui.Button("8-frame sequence")) _fn_dump_seq(8); ImGui.SameLine(); ImGui.Text("seq_*.bmp: both eyes, 8 consecutive frames"); }
            ImGui.TreePop();
        }
    }

    /// First person -> third person -> diorama (the chord and the End key).
    private static int next_view_mode(int m) => m == 1 ? 3 : m == 3 ? 2 : 1;
    private static bool life_size(int m) => m == 1 || m == 3;
    /// First and third person are true scale; leaving them for a seat or the diorama restores the scale from before.
    private void apply_mode_scale(int before, int after) {
        if (!life_size(before) && life_size(after)) { _scale_before_fp = _xr_scale; _xr_scale = k_true_scale; }
        else if (life_size(before) && !life_size(after)) _xr_scale = _scale_before_fp;
    }
    private void toggle_first_person() {
        int before = _fp_mode; _fp_mode = next_view_mode(_fp_mode);
        apply_mode_scale(before, _fp_mode);
    }
    /// <summary>Every key, once per frame, whether the window is open or not. F8, F9-F11 always work; the rest are
    /// ignored while a text box has the keyboard (typing "2,3" with the numpad must not move the camera).</summary>
    private void handle_hotkeys() {
        _turn_key_dir = 0;   // stickrep: set below unless a text field has the keyboard
        if (ImGui.IsKeyPressed(ImGuiKey.F8)) { _window_open = !_window_open; _window_open_forced = false; }   // the player's own choice is saved again
        if (ImGui.IsKeyPressed(ImGuiKey.F10)) _slowmo_on = !_slowmo_on;
        if (ImGui.IsKeyPressed(ImGuiKey.F11)) _hover_mode = (_hover_mode + 1) % 3;
        if (_native_ready && ImGui.IsKeyPressed(ImGuiKey.F9) && _fn_ui_pick != null) {
            Vector2 m = ImGui.GetMousePos(), ds = ImGui.GetIO().DisplaySize;
            if (ds.X > 0 && ds.Y > 0) { _fn_ui_pick(m.X / ds.X, m.Y / ds.Y); _pick_n++; _pick_poll_until = _now + 3.0; _pick_poll_frames = 3; _logger.Info($"pick at ({m.X:F0},{m.Y:F0}) of {ds.X:F0}x{ds.Y:F0}"); }
        }
        if (Convert.ToBoolean(ImGui.GetIO().WantTextInput) || !_native_ready) return;   // Convert: builds whether Hexa exposes bool or byte
        bt_user_keys();   // a view hotkey ends an open battle transition window
        if (ImGui.IsKeyPressed(ImGuiKey.Home) && _fn_xr_recenter != null) _fn_xr_recenter();
        if (ImGui.IsKeyPressed(ImGuiKey.End)) toggle_first_person();
        if (ImGui.IsKeyPressed(ImGuiKey.Insert)) _btl_cycle_req++;   // battle: the eyes to the next party member (consumed by push_player this frame)
        float dt   = ImGui.GetIO().DeltaTime;
        float rot  = 60f  * dt;     // deg/s
        float move = 400f * dt;     // units/s
        if (ImGui.IsKeyDown(ImGuiKey.Keypad4))        _ovr_yaw     -= rot;
        if (ImGui.IsKeyDown(ImGuiKey.Keypad6))        _ovr_yaw     += rot;
        if (ImGui.IsKeyDown(ImGuiKey.Keypad8))        _ovr_pitch   += rot;
        if (ImGui.IsKeyDown(ImGuiKey.Keypad2))        _ovr_pitch   -= rot;
        if (ImGui.IsKeyDown(ImGuiKey.KeypadAdd))      _ovr_forward += move;
        if (ImGui.IsKeyDown(ImGuiKey.KeypadSubtract)) _ovr_forward -= move;
        if (ImGui.IsKeyDown(ImGuiKey.Keypad9))        _ovr_up      += move;
        if (ImGui.IsKeyDown(ImGuiKey.Keypad7))        _ovr_up      -= move;
        if (ImGui.IsKeyPressed(ImGuiKey.Keypad5))     { _ovr_yaw = _ovr_pitch = 0; _ovr_right = _ovr_up = _ovr_forward = 0; if (_fn_gesture_reset != null) _fn_gesture_reset(); if (_fn_motion_log != null) _fn_motion_log(180); }
        _ovr_pitch = Math.Clamp(_ovr_pitch, -89f, 89f);
        // Numpad 1 / 3: native integrates them like the stick (time-based; on the display clock with stick_reproject, so the
        // extra frames turn with them too); sent every frame by sr_push (ffxvr_turn_keys). The native ffxvr_turn export stays (unused here).
        _turn_key_dir = (ImGui.IsKeyDown(ImGuiKey.Keypad3) ? 1 : 0) - (ImGui.IsKeyDown(ImGuiKey.Keypad1) ? 1 : 0);
    }

    /// <summary>The only call of ffxvr_gesture_poll (it reads and clears): grab state for the overlay, and the chords.</summary>
    private void poll_gestures() {
        if (_fn_gesture_poll == null) return;
        float* mv = stackalloc float[3]; float mul; int act;
        uint toggles = _fn_gesture_poll(mv, &mul, &act);
        _gest_move_x = mv[0]; _gest_move_y = mv[1]; _gest_move_z = mv[2]; _gest_scale_mul = mul; _gest_active = act;
        if ((toggles & 0xFF) != 0) toggle_first_person();
        if (((toggles >> 8) & 0xFF) != 0) _btl_cycle_req++;
        if (toggles != 0 || act != 0) _bt_user_ev = true;   // a gesture chord or a world grab ends an open battle transition window
    }

    /// <summary>saveui: the main window's row under the ImGui layer checkbox (fh_menu_on_screen) and its live state.</summary>
    private void fh_menu_ui() {
        ImGui.Checkbox("Fahrenheit's save screen on the virtual screen (as on the monitor)", ref _fh_menu_on_screen);
        if (_fn_imgui_status == null) return;
        int* v = stackalloc int[6];
        if (_fn_imgui_status(v, 6) < 6) return;
        string where = v[1] == 2 ? "on the virtual screen" : v[1] == 1 ? "head-locked" : "not shown";
        string head = v[0] != 0 ? (v[2] != 0 ? "save screen open: on the screen, placed where you faced; " : "save screen open: on the screen; ") : "";
        string size = v[4] > 0 ? $", {v[4]}x{v[5]}" : "";
        string refused = v[3] > 0 ? $", {v[3]} draws refused (no blend state)" : "";
        ImGui.SameLine();
        ImGui.TextDisabled(esc($"{head}layer {where}{size}{refused}"));
    }

    /// <summary>Every setting to the native side, exactly once per frame, whether the window is open or not. The window
    /// only draws widgets, reads status and runs one-shot buttons. Several setters are not idempotent (xr_set and fp_set
    /// are edge-triggered, player_set / player_head_set integrate per call, the stats reads clear), so none of them may
    /// be called anywhere else.</summary>
    private void push_all() {
        if (_push_frame == _frames) return;
        _push_frame = _frames;
        // The headset needs the override and both eyes. What they were before it went on comes back when it goes off.
        if (_xr_want != _xr_want_prev) {
            if (_xr_want) { _ovr_before_xr = _ovr_enabled; _stereo_before_xr = _stereo_on; }
            else { _ovr_enabled = _ovr_before_xr; _stereo_on = _stereo_before_xr; }
            _xr_want_prev = _xr_want;
        }
        if (_xr_want) { _ovr_enabled = true; _stereo_on = true; }
        // xr_set first: on the untick frame native clears g_xr_want before the override and eye 2 are pushed (it ORs
        // g_xr_want into both), so the next frame is already flat; and _xr_on is current for the hide rules below.
        if (_fn_xr_set != null) _xr_on = _fn_xr_set(_xr_want ? 1 : 0, _xr_scale, _xr_rotate ? 1 : 0, _xr_translate ? 1 : 0, _xr_fov ? 1 : 0, _xr_seat ? 1 : 0) != 0;
        if (_fn_override_set != null) _fn_override_set(_ovr_enabled ? 1 : 0, _ovr_freeze ? 1 : 0, _ovr_yaw, _ovr_pitch, _ovr_right, _ovr_up, _ovr_forward);
        if (_fn_stereo_set != null) _fn_stereo_set(_stereo_on ? 1 : 0, _ipd, 0, 0, 0);
        if (_fn_turn_set != null) _fn_turn_set(_turn_stick ? 1 : 0, _turn_mode, _turn_snap, _turn_rate, _turn_blink ? 1 : 0);
        if (_fn_move_set != null) _fn_move_set(_move_remap ? 1 : 0, _move_invert ? -1f : 1f);
        if (_fn_cull_dist_set != null && _fn_cull_dist_get != null && (_fn_cull_dist_get() != 0) != _cull_dist) {
            _cull_dist_rc = _fn_cull_dist_set(_cull_dist ? 1 : 0);
            if (_cull_dist_rc != 0) _cull_dist = _fn_cull_dist_get() != 0;      // could not be applied: show the real state
            if (_patch_on.Length > 2) { _patch_on[0] = _patch_on[1] = _patch_on[2] = _fn_cull_dist_get() != 0; }
        }
        if (_fn_msaa_set != null) _fn_msaa_set(_msaa);
        if (_fn_vrpad_set != null) _fn_vrpad_set(_vrpad ? 1 : 0);
        if (_fn_haptics_set != null) _haptics_status = _fn_haptics_set(_haptics ? 1 : 0, _haptics_gain);
        bt_push();
        lp_push();   // load lock build: diag_load_prof, load_lock_release
        log_push();  // release prep: diag_verbose and the log cap (only when changed; logging.cs)
        {   // saveui: one guarded read of the save-screen state a frame; the menu flag is latched from OPEN to CLOSED (no flapping
            // through the unnamed states 1 / 3 / 4), so the switch to the screen comes with Fahrenheit's first frame of the menu
            uint st = save_screen_state();
            _save_st = st;   // cpu-lean: the tap gate's bit 4 uses this frame's read (hands2_tap_game ran its own until now)
            if (st == 2) _fh_menu_open = true; else if (st == 0) _fh_menu_open = false;
            // menu: 0 not latched; latched: 1 | (the raw state << 8), so native can tell the frames Fahrenheit draws the menu in
            // (state 2) from the latched ones without it (3 / 4 at a close, 1 in a reopen: SU-1's black-out, SU-2's watchdog)
            int menu = (_fh_menu_on_screen && _imgui_layer && _fh_menu_open) ? 1 | (int)((st & 0xFF) << 8) : 0;
            if (_fn_imgui_layer_set != null) _fn_imgui_layer_set(_imgui_layer ? 1 : 0, (_window_open || st != 0) ? 1 : 0, _imgui_min_draws, menu);
        }
        if (_fn_gesture_set != null) _fn_gesture_set(_gestures ? 1 : 0, _gest_tilt);
        if (_fp_head_parts != _head_mask_src) { _head_mask_src = _fp_head_parts; _head_mask = parse_mask(_fp_head_parts); }   // R22: parsed only when the text changes
        if (_fn_fp_body_set != null) _fn_fp_body_set(_fp_body_mode, _head_mask, _fp_solo);
        if (_fn_cull_scene_set != null && _fn_cull_scene_get != null && (_fn_cull_scene_get() != 0) != _cull_scene) {
            _cull_scene_rc = _fn_cull_scene_set(_cull_scene ? 1 : 0);
            if (_cull_scene_rc != 0) _cull_scene = _fn_cull_scene_get() != 0;
            if (_patch_on.Length > 4) _patch_on[3] = _patch_on[4] = _fn_cull_scene_get() != 0;
        }
        if (_fn_xr_pitch_follow != null) _fn_xr_pitch_follow(_xr_pitch_follow);
        if (_fn_cuts_set != null) _fn_cuts_set(_cut_recentre ? 1 : 0, _cut_blink ? 1 : 0, _follow_pans ? 1 : 0, _follow_rate, ImGui.GetIO().DeltaTime);   // dt: a fallback only; native measures its own
        ej_push();   // eye jumps (eye_jump.cs)
        if (_fn_backdrop_set != null) _fn_backdrop_set(_backdrop_mode);
        if (_fn_hist_set != null) _fn_hist_set(_hist_mode);
        if (_fn_ui_set != null) _fn_ui_set(_ui_on ? 1 : 0, _ui_dist, _ui_width);
        uidepth_push();   // adaptive UI panel (every frame, so a change applies at once)
        dream_push();   // dream overlay: dream_overlay (every frame, so a change applies at once)
        if (_fn_noimg_set != null) _fn_noimg_set(_noimg_mode);
        shadow_push();   // shadow camera: shadow_cam, fp_self_shadow (every frame)
        haze_push();     // heat haze: heat_haze (every frame; applied at the next Present)
        fmv3d_push();    // fmv-relief (S3): fmv_depth and its settings (every frame; applied at the next Present)
        slow_push();     // the slow-GPU guard: slow_guard, slow_guard_restart (every frame)
        sr_push();       // stickrep: stick_reproject and its Debug knobs, the turn keys (every frame)
        if (_fn_bisect_set != null) _fn_bisect_set(_bisect_mode, _bisect_a, _bisect_b);
        { long t = cpu_now(); push_player(); _cpu_player += cpu_now() - t; }   // before fp_set: it can switch the view for a battle (cpu-lean: timed)
        push_hands();
        { long t = cpu_now(); hands2_push(); _cpu_hands += cpu_now() - t; }   // VR hands v2: vr_hands_*, the calibration (hands.cs)
        qc_push();   // Quest-only controls: the walk gate's game bits, the grips, the save screen's feed (quest_ctl.cs; after hands2_push: its tap read)
        odg_push();  // overdrive gestures: the settings, the Debug test aid (od_gest.cs)
        if (_fn_tp_set != null) _fn_tp_set(_tp_dist, _tp_h, _tp_pitch * 0.0174533f, _tp_follow_rate);
        if (_fn_view_smooth_set != null) _fn_view_smooth_set(_anchor_smooth_ms, _battle_anchor_smooth_ms);
        if (_fn_xr_debug != null) { _fn_xr_debug(_xr_drop_image ? 1 : 0, _xr_sim_timeouts_req); _xr_sim_timeouts_req = 0; }
        if (_fn_fp_set != null) _fn_fp_set(_fp_mode, _fp_eye_h, _fp_follow_dist, _fp_follow_h, _fp_hide_body ? 1 : 0, _fp_hide_radius);
    }

    /// <summary>Shows the eye-2 render in its own window, scaled down.</summary>
    private void draw_eye2_preview() {
        if (!_eye2_preview || _xr_on || !_stereo_on || _fn_eye2_srv == null || _fn_eye2_size == null) return;
        int w, h;
        if (_fn_eye2_size(&w, &h, _eye2_source) == 0 || w == 0) return;
        nint srv = _fn_eye2_srv(_eye2_source);
        if (srv == 0) return;

        ImGui.SetNextWindowSize(new Vector2(960, 540 + 40), ImGuiCond.FirstUseEver);
        if (ImGui.Begin("Eye 2", ImGuiWindowFlags.NoDocking)) {
            Vector2 avail = ImGui.GetContentRegionAvail();
            float scale = Math.Min(avail.X / w, avail.Y / h);
            ImGui.Image(new ImTextureRef(null, (ImTextureID)(ulong)srv), new Vector2(w * scale, h * scale));
        }
        ImGui.End();
    }


    // -- in-world battle markers --------------------------------------------
    //
    // The game's target and turn cursors are 2D sprites positioned by CPU code
    // through the game's own camera, so they cannot follow an override. These
    // are ours: the same information (selected targets, whose turn it is, each
    // combatant's position) read from the game, projected through the camera
    // the player actually sees. Every game-memory read goes through a native
    // SEH-guarded copy, because the battle pointers are stale outside battle.

    private bool read<T>(void* src, out T value) where T : unmanaged {
        T tmp = default;
        bool ok = _fn_safe_read != null && _fn_safe_read(src, &tmp, (uint)sizeof(T)) != 0;
        value = ok ? tmp : default;   // vq-lean (review N2): a failed kernel copy over 256 B may leave tmp partly written
        return ok;
    }

    /// <summary>R22: does the actor name at p equal `want`? One 16-byte guarded read (byte by byte only if that fails: the
    /// name can end a region), no allocation. actor_name stays for logs and the UI.</summary>
    private bool name_is(char* p, string want) {
        if (p == null || want.Length == 0 || want.Length > 15 || _fn_safe_read == null) return false;
        byte* b = stackalloc byte[16];
        if (_fn_safe_read(p, b, 16) == 0)
            for (int i = 0; i < 16; i++) { if (_fn_safe_read((byte*)p + i, b + i, 1) == 0) return false; if (b[i] == 0) break; }
        return RideLogic.NameMatch(new ReadOnlySpan<byte>(b, 16), want);   // a NUL before the end mismatches
    }
    // cpu-lean (e): the follow pick's name test, once per slot. fp_name_rank = the first fp_actor name the slot's model name
    // matches (name_is's test for each, in order), -1 none, from one read of the name; kept per slot while the slot's name
    // pointer, chr_id and the list are the same (the name is the model's: a slot showing another model has another chr_id),
    // and read again every 30 scan ticks (with the periodic rescan). A failed read is never kept (the next frame tries again,
    // as name_is did).
    private readonly nint[] _fpn_ptr = new nint[48]; private readonly int[] _fpn_id = new int[48], _fpn_rank = new int[48];
    private string[] _fpn_names; private bool _fpn_frame_clear;
    private int fp_name_rank(int i, char* p, int chr_id, string[] names) {
        if (_fpn_frame_clear || !ReferenceEquals(names, _fpn_names)) { Array.Clear(_fpn_ptr, 0, _fpn_ptr.Length); _fpn_names = names; _fpn_frame_clear = false; }
        if (p != null && _fpn_ptr[i] == (nint)p && _fpn_id[i] == chr_id) return _fpn_rank[i];
        int r = name_rank(p, names, out bool ok);
        if (ok && p != null) { _fpn_ptr[i] = (nint)p; _fpn_id[i] = chr_id; _fpn_rank[i] = r; } else _fpn_ptr[i] = 0;
        return r;
    }
    /// <summary>name_is for every name of the list at once: the same read (16 bytes, byte by byte if that fails), the same test.
    /// ok = false when the name could not be read (then no name matches, as with name_is).</summary>
    private int name_rank(char* p, string[] names, out bool ok) {
        ok = false;
        if (p == null || _fn_safe_read == null) return -1;
        if (names.Length == 0) { ok = true; return -1; }   // (name_is was not called at all: nothing to read)
        byte* b = stackalloc byte[16];
        if (_fn_safe_read(p, b, 16) == 0)
            for (int i = 0; i < 16; i++) { if (_fn_safe_read((byte*)p + i, b + i, 1) == 0) return -1; if (b[i] == 0) break; }
        ok = true;
        return RideLogic.NameRank(new ReadOnlySpan<byte>(b, 16), names);
    }
    private string actor_name(char* p) {
        if (p == null) return "";
        var sb = new System.Text.StringBuilder();
        for (int i = 0; i < 24; i++) { byte b = 0; if (_fn_safe_read == null || _fn_safe_read(((byte*)p) + i, &b, 1) == 0 || b == 0) break; if (b >= 32 && b < 127) sb.Append((char)b); }
        return sb.ToString();
    }

    private Vector4[] _actor_prev = new Vector4[64];
    private float[]   _actor_moved = new float[64];
    /// <summary>Every 10 s: which live actors had no draw at their position this frame, with the
    /// state bytes that decide it (hide byte +180, flag words, model visible byte). Evidence for the
    /// culling work without anyone pressing a button.</summary>
    private double _actors_last_log = 0;
    // The current map name (exe-analysis section 14: gva 0x23CBC60, NUL-terminated, e.g. "bltz0000").
    private static string map_name() {
        try { var sb = new System.Text.StringBuilder(); for (int i = 0; i < 15; i++) { byte b = FhUtil.get_at<byte>(0x1FCBC60 + i); if (b == 0 || b < 32 || b > 126) break; sb.Append((char)b); } return sb.ToString(); }
        catch { return ""; }
    }
    // The name carries a folder prefix ("bl/bltz0000", "te/test20"): match on the part after the slash.
    private static string map_base(string m) { int k = m.LastIndexOf('/'); return k >= 0 ? m[(k + 1)..] : m; }
    private static bool in_blitz() => map_base(map_name()).StartsWith("bltz", StringComparison.Ordinal);
    private bool _in_blitz;   // R22: set where _map_last changes (blitz_auto_log), ordinal

    // Blitzball: twice a second while on a bltz map, every live actor's logical position (actor table) next to
    // where it is actually drawn (the nearest skinned-root translation in the ground plane), to learn how the
    // game turns its flat match positions into the 3D pool. Plus Farplane's blitz state words. Capped per session.
    private double _blitz_last_log = 0; private int _blitz_log_lines = 0; private string _map_last = "";
    private bool _blitz_overlay = true; private uint _blitz_overlay_n = 0; private delegate* unmanaged[Cdecl]<int, uint> _fn_blitz_overlay;   // scoreboard / radar disc onto the UI panel
    private void blitz_auto_log() {
        double now = ImGui.GetTime(); if (now - _blitz_last_log < 0.5) return; _blitz_last_log = now;
        string map = map_name();
        if (map != _map_last) { _logger.Info($"map: {(_map_last.Length > 0 ? _map_last : "?")} -> {map} (event_id {(Globals.event_id != null ? *Globals.event_id : -1)})"); _map_last = map; _in_blitz = map_base(map).StartsWith("bltz", StringComparison.Ordinal); }
        if (!_in_blitz || !_diag_verbose || _blitz_log_lines >= 1200 || _fn_world_positions == null) return;   // the blitz lines: diag_verbose only
        Actor* actors = Globals.actors; if (actors == null) return;
        float* wp = stackalloc float[128 * 3]; int nwp = _fn_world_positions(wp, 128);
        var sb = new System.Text.StringBuilder($"blitz {map}:");
        try {
            uint gp = FhUtil.get_at<uint>(0x14763E0); sb.Append($" game@{gp:X8} mode");
            for (int k = 0; k < 8; k++) sb.Append($" {FhUtil.get_at<byte>(0x12AD8A0 + k):X2}");
        } catch { }
        sb.Append(" |");
        for (int i = 0; i < 48; i++) {
            if (!read(actors + i, out Actor a)) break;
            if (!live(a)) continue;
            float rx = -a.chr_pos_vec.X, ry = -a.chr_pos_vec.Y, rz = a.chr_pos_vec.Z;
            int best = -1; float bd = float.MaxValue;
            for (int k = 0; k < nwp; k++) { float dx = wp[k*3] - rx, dz = wp[k*3+2] - rz; float d = dx*dx + dz*dz; if (d < bd) { bd = d; best = k; } }
            sb.Append($" {i}:{actor_name(a.chr_name)} ({a.chr_pos_vec.X:F0},{a.chr_pos_vec.Y:F0},{a.chr_pos_vec.Z:F0})");
            if (best >= 0) sb.Append($"->drawn ({-wp[best*3]:F0},{-wp[best*3+1]:F0},{wp[best*3+2]:F0}) dxz {MathF.Sqrt(bd):F0}");
        }
        _logger.Info(sb.ToString()); _blitz_log_lines++;
    }

    private void actors_auto_log() {
        if (_fn_world_positions == null || _fn_safe_read == null) return;
        double now = ImGui.GetTime(); if (now - _actors_last_log < 10.0) return;
        Actor* actors = Globals.actors; if (actors == null) return;
        float* wp = stackalloc float[128 * 3]; int nwp = _fn_world_positions(wp, 128);
        int live_n = 0, drawn_n = 0; var sb = new System.Text.StringBuilder();
        for (int i = 0; i < 48; i++) {
            if (!read(actors + i, out Actor a)) break;
            if (!live(a)) continue;
            live_n++;
            Vector3 apos = new Vector3(a.chr_pos_vec.X, a.chr_pos_vec.Y, a.chr_pos_vec.Z); nint par = 0;
            if (_fn_safe_read((byte*)(actors + i) + 0x19C, &par, 4) != 0 && par != 0 && read((byte*)(actors + i) + 0x200, out Vector3 td) && finite(td)) apos = td;   // riding: the drawn root
            float rx = -apos.X, ry = -apos.Y, rz = apos.Z; bool drawn = false;
            for (int k = 0; k < nwp; k++) { float dx = wp[k*3] - rx, dy = wp[k*3+1] - ry, dz = wp[k*3+2] - rz; if (dx*dx + dy*dy + dz*dz < 9f) { drawn = true; break; } }
            if (drawn) { drawn_n++; continue; }
            byte* raw = (byte*)(actors + i); byte hide = 0; uint f194 = 0, f198 = 0; float dist = 0; nint model = 0; byte mv = 0xFF;
            _fn_safe_read(raw + 0x180, &hide, 1); _fn_safe_read(raw + 0x194, &f194, 4); _fn_safe_read(raw + 0x198, &f198, 4); _fn_safe_read(raw + 0x178, &dist, 4); _fn_safe_read(raw + 0x830, &model, 4);
            if (model > 0x10000 && model < 0x7FFF0000) _fn_safe_read((void*)(model + 0x14), &mv, 1);
            bool at_origin = MathF.Abs(a.chr_pos_vec.X) + MathF.Abs(a.chr_pos_vec.Y) + MathF.Abs(a.chr_pos_vec.Z) < 0.01f;
            sb.Append($"  {i}:{actor_name(a.chr_name)} hide {hide:X2} f194 {f194:X8} f198 {f198:X8} d {dist:F0} mv {mv:X2}{(at_origin ? " (at origin)" : "")}");
        }
        _actors_last_log = now;
        if (live_n > 0) _logger.Info($"actors: live {live_n} drawn {drawn_n} uploads {nwp} | not drawn:{sb}");
    }
    private void draw_actors() {
        Actor* actors = Globals.actors;
        if (actors == null) { ImGui.Text("no actor table"); return; }
        bool log_it = ImGui.Button("log this table"); ImGui.SameLine();
        ImGui.TextDisabled("drawn = a draw at the actor's position this frame; hide = byte +180; f194/f198 = flag words; cd = cull distance (+174), d = distance to the game camera (+178); a = alpha (+354); mv = model visible byte; att = attached to actor # (+19C; 'ext' outside the table): riding, its position is the drawn root (+200)");
        ImGui.Text("idx  id   name          position                 drawn hide  f194      f198     cd      d      a   mv  att");
        float* wp = stackalloc float[128 * 3];
        int nwp = _fn_world_positions != null ? _fn_world_positions(wp, 128) : 0;
        var sb = log_it ? new System.Text.StringBuilder("actor table:") : null;
        for (int i = 0; i < 48; i++) {
            if (!read(actors + i, out Actor a)) break;
            if (!live(a)) continue;
            float d = Vector4.Distance(a.chr_pos_vec, _actor_prev[i]);
            _actor_prev[i] = a.chr_pos_vec;
            _actor_moved[i] = _actor_moved[i] * 0.95f + d;        // a decaying "how much it moves" score
            byte* raw = (byte*)(actors + i);
            byte hide = 0; uint f194 = 0, f198 = 0; float cd = 0, dist = 0, alpha = 0; nint model = 0; byte mv = 0xFF;
            if (_fn_safe_read != null) {
                _fn_safe_read(raw + 0x180, &hide, 1); _fn_safe_read(raw + 0x194, &f194, 4); _fn_safe_read(raw + 0x198, &f198, 4);
                _fn_safe_read(raw + 0x174, &cd, 4); _fn_safe_read(raw + 0x178, &dist, 4); _fn_safe_read(raw + 0x354, &alpha, 4);
                _fn_safe_read(raw + 0x830, &model, 4);
                if (model > 0x10000 && model < 0x7FFF0000) _fn_safe_read((void*)(model + 0x14), &mv, 1);
            }
            nint par = 0; Vector3 apos = new Vector3(a.chr_pos_vec.X, a.chr_pos_vec.Y, a.chr_pos_vec.Z);
            if (_fn_safe_read != null) _fn_safe_read(raw + 0x19C, &par, 4);
            if (par != 0 && read(raw + 0x200, out Vector3 td) && finite(td)) apos = td;   // riding: drawn from the parent's bone, not from +0x0C
            string att = par == 0 ? "-" : RideLogic.ParentIndex((long)actors, (long)par, sizeof(Actor), 48, i) is int pix && pix >= 0 ? pix.ToString(CI) : "ext";
            float rx = -apos.X, ry = -apos.Y, rz = apos.Z; bool drawn = false;
            for (int k = 0; k < nwp; k++) { float dx = wp[k*3] - rx, dy = wp[k*3+1] - ry, dz = wp[k*3+2] - rz; if (dx*dx + dy*dy + dz*dz < 9f) { drawn = true; break; } }
            string line = $"{i,3} {a.chr_id,4}  {actor_name(a.chr_name),-12} ({a.chr_pos_vec.X,7:F1},{a.chr_pos_vec.Y,7:F1},{a.chr_pos_vec.Z,7:F1})  {(drawn ? "yes" : " no"),4}  {hide:X2}  {f194:X8} {f198:X8} {cd,6:F1} {dist,6:F1} {alpha,5:F2}  {mv:X2}  {att,3}" +
                          (par != 0 ? $"  drawn root ({apos.X:F1},{apos.Y:F1},{apos.Z:F1})" : "");
            ImGui.Text(esc(line));   // actor names read from the game
            sb?.Append("\n  ").Append(line);
        }
        if (sb != null) { optpos_log(actors, sb); _logger.Info(sb.ToString()); }
    }

    /// <summary>The first-person head of an actor: FphWalk.HeadPick (the one rule; see there) on its 26 sub-positions. reach is
    /// _fp_reach_live (fp_head_reach in battle, 0 elsewhere). old = the old-rule pick. Returns the index, or -1.</summary>
    private static int fp_head_pick(ref Actor a, float reach, out int old) {
        Span<Vector4> op = stackalloc Vector4[26];
        for (int k = 0; k < 26; k++) op[k] = a.chr_optpos[k];
        return FphWalk.HeadPick(op, reach, out old);
    }
    /// <summary>The reach the head rule uses this frame: fp_head_reach in battle, 0 (the old rule) in the field, riding, cutscenes and
    /// blitzball. Written by push_player (render thread) with Volatile.Write before _d7_follow_actor; d7_join_record (the main
    /// thread's snapshot) reads it with Volatile.Read, so the snapshot's head and push_player's agree.</summary>
    private float _fp_reach_live;
    private long _fph_skip_key = -1; private int _fph_skip_n;   // the "head: skipped" log: last line's key (chr_id, old, new), lines this battle (at most 6)
    /// <summary>One log line when the reach changed the followed actor's head pick (rate-limited: a new model / index pair only, at
    /// most 6 a battle), so the owner's log shows the rule is live. No allocation unless a line is written.</summary>
    private void fph_skip_log(ref Actor a, int now, int old, float reach) {
        if (now < 0 || old < 0 || now == old || _fph_skip_n >= 6) return;
        long key = ((long)(uint)a.chr_id << 16) | ((long)old << 8) | (long)now; if (key == _fph_skip_key) return;
        _fph_skip_key = key; _fph_skip_n++;
        string name = actor_name(a.chr_name);
        Vector4 b = a.chr_optpos[0], o = a.chr_optpos[old];
        float d = MathF.Sqrt((o.X - b.X) * (o.X - b.X) + (o.Z - b.Z) * (o.Z - b.Z));
        _logger.Info(System.FormattableString.Invariant($"head: skipped index {old}, {d:F1} units out, for model {name} (index {now} instead; reach {reach:F1}, map {map_name()})"));
    }

    /// <summary>"log this table" only (diagnostics: Wakka's first-person eye in battle sits ~7 units high): for the followed
    /// actor and every live party model (c0xx), the 26 chr_optpos sub-positions (+0x524; all-zero ones left out), with the
    /// index push_player's head rule picks marked '*' (the lowest actor-space Y with |X| + |Z| > 0.01: the highest point;
    /// fp_head_anchor puts the eye fp_head_up above it), the root, and whether push_player would fold the pick as an offset
    /// (more than 100 units from the root in X / Z). Read only; nothing here changes what push_player does.</summary>
    private void optpos_log(Actor* actors, System.Text.StringBuilder sb) {
        for (int i = 0; i < 48; i++) {
            if (!read(actors + i, out Actor a)) break;
            if (!live(a)) continue;
            string name = actor_name(a.chr_name);
            bool party = name.Length == 4 && name[0] == 'c' && name[1] == '0';
            if (i != _fp_actor_idx && !party) continue;
            int pick = fp_head_pick(ref a, _fp_reach_live, out _);   // push_player's rule (shared), with this frame's reach
            sb.Append(System.FormattableString.Invariant($"\n  optpos {i} {name}{(i == _fp_actor_idx ? " (followed)" : "")}: root ({a.chr_pos_vec.X:F2},{a.chr_pos_vec.Y:F2},{a.chr_pos_vec.Z:F2})"));
            for (int k = 0; k < 26; k++) {
                Vector4 sp = a.chr_optpos[k];
                if (sp.X == 0f && sp.Y == 0f && sp.Z == 0f && sp.W == 0f) continue;
                sb.Append(System.FormattableString.Invariant($" {k}{(k == pick ? "*" : "")}:({sp.X:F2},{sp.Y:F2},{sp.Z:F2},{sp.W:F2})"));
            }
            if (pick < 0) sb.Append(" | head: none (no sub-position off the axis)");
            else {
                Vector4 h = a.chr_optpos[pick];
                bool fold = MathF.Abs(h.X - a.chr_pos_vec.X) + MathF.Abs(h.Z - a.chr_pos_vec.Z) > 100f;
                sb.Append(System.FormattableString.Invariant($" | head = {pick} (y {h.Y:F2}, {a.chr_pos_vec.Y - h.Y:F2} above the root{(fold ? "; push_player would fold it as an offset from the root" : "")}); eye {(_fp_head_anchor != 0 ? $"{_fp_head_up:F2} above it (fp_head_anchor {_fp_head_anchor})" : "fixed (fp_head_anchor 0)")}"));
            }
        }
    }

    /// <summary>The player: the enabled field actor whose model name is in the fp_actor list, best first (Tidus is
    /// c001 for most of the game, c101 in the opening); index 0 is not reliable (in Luca it is an object, f001).
    /// Falls back to the party model with the lowest id, then actor 0 (FP-1). Render space negates X and Y.</summary>
    private const string k_fp_actor_default = "c001,c101";
    private string _fp_actor = k_fp_actor_default; private int _fp_actor_idx = -1;
    private string _fp_how = "", _fp_how_logged = ""; private int _fp_scan_tick, _fp_chr_id = -1;
    private string _fp_names_src = ""; private string[] _fp_names_arr = System.Array.Empty<string>();
    /// <summary>FP-2: the game's script hide on an actor: bits 0x01, 0x02 or 0x10 of actor+0x180 (0x08 and 0x20 are the frustum and distance
    /// culls). Guarded read; false if it cannot be read.</summary>
    private bool script_hidden(Actor* a) {
        if (_fn_safe_read == null) return false;
        byte h = 0;
        return _fn_safe_read((byte*)a + 0x180, &h, 1) != 0 && (h & 0x13) != 0;   // FP-H1: 0x01 / 0x02 / 0x10 are the persistent script hides
    }
    /// <summary>FP-1: fp_actor split at commas (names of 1-15 characters; others dropped), cached until the text changes.</summary>
    private string[] fp_names() {
        string s = _fp_actor;
        if (!ReferenceEquals(s, _fp_names_src) && s != _fp_names_src) {
            var l = new System.Collections.Generic.List<string>();
            foreach (string t in s.Split(',')) { string n = t.Trim(); if (n.Length >= 1 && n.Length <= 15 && !l.Contains(n)) l.Add(n); }
            _fp_names_arr = l.ToArray(); _fp_names_src = s;
        }
        return _fp_names_arr;
    }
    /// <summary>An actor slot that is really in use. Before the field is up the table is uninitialised memory
    /// (0xCDCDCDCD fill), so only a plain 1 counts as enabled and the name pointer must look like one.</summary>
    private static bool live(in Actor a) => a.chr_enabled == 1 && (nint)a.chr_name > 0x10000 && (nint)a.chr_name < 0x7FFF0000;
    /// <summary>In battle the diorama anchors to the centre of the combatants (party and monsters
    /// from the battle lists, whoever is in the fight), not the player, who may be swapped out.</summary>
    private void push_battle_anchor() {
        if (_fn_anchor_override == null) return;
        _battle_anchor_n = 0;
        if (!_battle_anchor) { _fn_anchor_override(0, 0, 0, 0); return; }
        Btl* btl = Globals.Battle.btl;
        if (!read(btl, out Btl btl_v) || btl_v.battle_state == 0) { _fn_anchor_override(0, 0, 0, 0); return; }
        Chr* mons = Globals.Battle.monster_characters; Chr* plys = Globals.Battle.player_characters;
        float sx = 0, sy = 0, sz = 0; int n = 0;
        for (int i = 0; i < k_ply_n; i++)
            if (plys != null && read(plys + i, out Chr pc) && pc.actor != null && read(pc.actor, out Actor pa) && live(pa)) { sx += pa.chr_pos_vec.X; sy += pa.chr_pos_vec.Y; sz += pa.chr_pos_vec.Z; n++; }
        for (int i = 0; i < k_mon_n; i++)
            if (mons != null && read(mons + i, out Chr mc) && mc.actor != null && read(mc.actor, out Actor ma) && live(ma) && mc.stat_visible != 0) { sx += ma.chr_pos_vec.X; sy += ma.chr_pos_vec.Y; sz += ma.chr_pos_vec.Z; n++; }
        _battle_anchor_n = n;
        if (n == 0) { _fn_anchor_override(0, 0, 0, 0); return; }
        _fn_anchor_override(-sx / n, -sy / n, sz / n, 1);   // render space negates X and Y
    }
    private void push_hands() {
        if (!_hands_cal_loaded && _fn_hands_cal_data != null && _hands_cal_ini.Length > 0) {
            _hands_cal_loaded = true; var t = _hands_cal_ini.Split(','); if (t.Length == 18) { float* rc = stackalloc float[18]; bool ok = true; for (int i = 0; i < 18; i++) ok &= float.TryParse(t[i], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out rc[i]); if (ok) { _fn_hands_cal_data(rc, 1); _hands_cal_seen = _fn_hands_cal_data(null, 0); } }
        }
        if (_fn_hands_side != null) {
            if (!_hands_side_pushed) { _hands_side_pushed = true; if (_hands_px_left >= 0) _fn_hands_side(_hands_px_left, _hands_stretch); }
            int r = _fn_hands_side(-1, _hands_stretch); if ((r & 2) != 0) _hands_px_left = r & 1;   // the measurement, once made, is what gets saved
        }
        if (_fn_hands_set != null) _fn_hands_set((_hands_dev && _hands_on) ? 1 : 0, (_hands_head ? 1 : 0) | (_hands_show << 1), _hands_yaw * 0.0174533f, _hands_pitch * 0.0174533f, _hands_roll * 0.0174533f, _hands_off_r, _hands_off_u, _hands_off_b, _hands_swap ? 1 : 0, _hands_mirror ? 1 : 0, _hands_only); }
    private void push_player() {
        if (_fn_player_set == null) return;
        Actor* actors = Globals.actors;
        _ride_att = false; _ride_mount_ok = false;   // ride_probe sets them again for an attached followed actor
        _btg_fed = false; _btg_carry = false;   // battle tag: set again when the hold is sent / ends this frame
        if (actors == null) { btg_end("no actor table"); System.Threading.Volatile.Write(ref _d7_follow_actor, 0); _fn_player_set(0, 0, 0, 0); body_hide_push(null, -1, -1, false, default, 0, default); _fp_actor_idx = -1; _btl_cycle_req = 0; push_view_rules(false); return; }
        // Before the field is up the table is uninitialised memory (0xCDCDCDCD
        // fill): only a plain 1 counts as enabled, and a name pointer must look
        // like one before it is read.
        int pick = -1;
        // Battle: the eyes follow a chr id chosen from the game's field roster (see the state above).
        Btl* btl = Globals.Battle.btl; bool in_battle = read(btl, out Btl btl_v) && btl_v.battle_state != 0;
        System.Threading.Volatile.Write(ref _fp_reach_live, in_battle ? _fp_head_reach : 0f);   // the head rule's reach: battle only (review of 9b406b9, SF); before _d7_follow_actor below
        if (in_battle != _btl_was) {
            _fph_skip_n = 0; _fph_skip_key = -1;
            btg_end(in_battle ? "a new battle" : "the battle ended"); btg_log(in_battle ? "a new battle" : "the battle ended"); _btg_state = 0;   // battle tag
            _btl_was = in_battle; _btl_chr = -1; _btl_follow_id = -1; _btl_follow_slot = -1; _btl_pre_summon_id = -1; _btl_pending_id = -1; _btl_pending_frames = 0; _btl_who = ""; _btl_aeon_out = false;
            for (int i = 0; i < 3; i++) _btl_roster_prev[i] = 0xFF;
            ej_battle_edge(in_battle);   // eye jumps: keep the field heading at the start, restore it after the end (native, before fp_set changes the view)
            if (in_battle) { if (_battle_view >= 0 && _battle_view != _fp_mode) { _btl_mode_before = _fp_mode; apply_mode_scale(_fp_mode, _battle_view); _fp_mode = _battle_view; } }
            else if (_btl_mode_before >= 0) { if (_btl_mode_before != _fp_mode) { apply_mode_scale(_fp_mode, _btl_mode_before); _fp_mode = _btl_mode_before; } _btl_mode_before = -1; }
        }
        if (in_battle && _battle_follow) {
            Chr* plys = Globals.Battle.player_characters;
            // The roster: three chr ids, 0xFF = empty slot. Kept by the game at setup, on swaps, summons and dismissals.
            bool roster_ok = true;
            try { for (int i = 0; i < 3; i++) _btl_roster[i] = FhUtil.get_at<byte>(k_roster_off + i); } catch { roster_ok = false; }
            // before setup has run the bytes are stale (seen: [0,0,0]); a real roster never repeats an id
            if (_btl_roster[0] == _btl_roster[1] && _btl_roster[0] != 0xFF || _btl_roster[1] == _btl_roster[2] && _btl_roster[1] != 0xFF || _btl_roster[0] == _btl_roster[2] && _btl_roster[0] != 0xFF) roster_ok = false;
            if (roster_ok && plys != null) {
                static bool is_aeon(byte id) => id >= 8 && id <= 0x11;
                static bool is_party(byte id) => id <= 6;
                bool on_field(int id) { for (int i = 0; i < 3; i++) if (_btl_roster[i] == id) return true; return false; }
                int slot_of(int id) { for (int i = 0; i < 3; i++) if (_btl_roster[i] == id) return i; return -1; }
                bool was_on(int id) { for (int i = 0; i < 3; i++) if (_btl_roster_prev[i] == id) return true; return false; }
                int lowest_on() { int b = -1; for (int i = 0; i < 3; i++) if (is_party(_btl_roster[i]) && (b < 0 || _btl_roster[i] < b)) b = _btl_roster[i]; return b; }
                // entity index of a chr id (the roster is ids; the actor comes from the entity)
                int ply_of(int id) { for (int i = 0; i < 18; i++) if (read(plys + i, out Chr pc) && pc.chr_id == id && pc.actor != null) return i; return -1; }

                bool changed = false; for (int i = 0; i < 3; i++) if (_btl_roster[i] != _btl_roster_prev[i]) changed = true;
                if (changed) _logger.Info($"battle follow: roster [{_btl_roster[0]},{_btl_roster[1]},{_btl_roster[2]}] (was [{_btl_roster_prev[0]},{_btl_roster_prev[1]},{_btl_roster_prev[2]}])");

                int aeon_id = -1; for (int i = 0; i < 3; i++) if (is_aeon(_btl_roster[i])) aeon_id = _btl_roster[i];
                int want = _btl_follow_id; string why = null;

                if (aeon_id >= 0) {
                    // Summon: the party walks off and the summoner stays to command the Aeon - the summoner's eyes.
                    // The aeon entity records the summoner's chr id (+0x6D1); Yuna if that does not read sensibly.
                    if (!_btl_aeon_out) { _btl_aeon_out = true; _btl_pre_summon_id = _btl_follow_id; }
                    int summoner = 1; int ai = ply_of(aeon_id);
                    if (ai >= 0 && read((byte*)(plys + ai) + k_aeon_summoner_off, out byte sb) && sb <= 6) summoner = sb;
                    if (want != summoner) { want = summoner; why = $"summon (aeon {aeon_id}) -> summoner {summoner}'s eyes"; }
                }
                else {
                    if (_btl_aeon_out) {
                        // dismissed: back to whoever had the eyes before the summon, if still on the field
                        _btl_aeon_out = false;
                        if (_btl_pre_summon_id >= 0 && on_field(_btl_pre_summon_id)) { want = _btl_pre_summon_id; why = "aeon dismissed -> back to " + want; }
                        _btl_pre_summon_id = -1;
                    }
                    // Tidus joining the field takes the eyes, whoever they were on.
                    if (on_field(0) && !was_on(0) && want != 0) { want = 0; why = "Tidus joined the field"; }
                    // Manual: next party member on the field (left grip + right stick click, or Insert), in slot order.
                    if (_btl_cycle_req > 0) {
                        _btl_cycle_req = 0; int k = slot_of(want);
                        for (int step = 1; step <= 3; step++) { byte id = _btl_roster[(k + step + 3) % 3]; if (is_party(id) && id != want) { want = id; why = "manual"; break; } }
                    }
                    // Nobody yet, or the followed character left the field: the one who took the slot, else Tidus, else the lowest id.
                    if (want < 0 || !on_field(want)) {
                        int succ = _btl_follow_slot >= 0 && is_party(_btl_roster[_btl_follow_slot]) ? _btl_roster[_btl_follow_slot] : -1;
                        int next = on_field(0) ? 0 : (succ >= 0 ? succ : lowest_on());
                        if (next >= 0) { want = next; why = _btl_follow_id < 0 ? "battle start" : $"{_btl_follow_id} left the field"; }
                    }
                }
                for (int i = 0; i < 3; i++) _btl_roster_prev[i] = _btl_roster[i];
                // a switch still waiting for the newcomer to arrive stays wanted (the join is a one-frame event)
                if (want == _btl_follow_id && _btl_pending_id >= 0 && _btl_pending_id != want && on_field(_btl_pending_id)) { want = _btl_pending_id; why = _btl_pending_why; }
                else if (_btl_pending_id >= 0 && !on_field(_btl_pending_id)) { _btl_pending_id = -1; _btg_state = 0; btg_log("he left the field again before the switch"); }
                if (_btg_state == 2 && want != _btl_follow_id) btg_end($"the follow changed ({why})");   // battle tag: a manual pick, a summon or another swap during the hold

                if (want >= 0 && want != _btl_follow_id) {
                    // A swap: the newcomer runs in from the bench. Wait until he has stopped moving (or a few seconds)
                    // so the eyes do not ride the run-in; battle start, summons and manual picks are immediate.
                    bool swap_in = _btl_follow_id >= 0 && why != null && (why == "Tidus joined the field" || why.EndsWith("left the field"));
                    int wi = ply_of(want);
                    if (_btl_pending_id != want) {
                        _btl_pending_id = want; _btl_pending_why = why ?? ""; _btl_pending_frames = 0; _btl_pending_still = 0; _logger.Info($"battle follow: {why} -> chr {want}{(swap_in ? ", waiting for him to arrive" : "")}");
                        if (swap_in) btg_begin(plys, why, want, wi, on_field(_btl_follow_id));   // battle tag: the outgoing character's spot, the watch
                        else btg_interrupt(why);   // battle tag: a summon or a manual pick ends a watched swap (its line, with this reason)
                    }
                    bool ready = !swap_in || wi < 0;
                    string ready_why = wi < 0 ? "he has no entity" : "";
                    if (!ready && read(plys + wi, out Chr wc) && wc.actor != null && read(wc.actor, out Actor wa) && live(wa)) {
                        _btl_pending_still = btl_still_next(_btl_pending_still, wa.chr_pos_vec.X - _btl_pending_pos.X, wa.chr_pos_vec.Z - _btl_pending_pos.Z);
                        _btl_pending_pos = wa.chr_pos_vec;
                        // the arrival rule (bt_tag.cs, shared with the hold): ran in, then stood still a quarter second
                        if (btl_arrived(_btl_pending_frames, _btl_pending_still)) { ready = true; ready_why = "he arrived and stood still"; }
                    }
                    if (++_btl_pending_frames > k_btl_swap_max && !ready) { ready = true; ready_why = I($"the {k_btl_swap_max}-frame limit"); }
                    // battle tag (bt_tag.cs): the two cross -> switch now (bt_tag 1 / 2; bt_tag 0 only measures it)
                    bool tagged = !ready && swap_in && btg_watch(plys, want, wi, _btl_pending_frames);
                    if (tagged) ready = true;
                    if (ready) {
                        _btl_follow_id = want; _btl_pending_id = -1;   // (_btl_pending_frames is cleared after the battle tag's line below)
                        _btl_chr = ply_of(want); _btl_who = _btl_chr >= 0 && read(plys + _btl_chr, out Chr fc) && fc.actor != null && read(fc.actor, out Actor fa) ? actor_name(fa.chr_name) : $"chr {want}";
                        _logger.Info($"battle follow: {why} -> following {_btl_who} (chr {want}, ply {_btl_chr}){(tagged ? ", tagged where the two crossed" : "")}");
                        ej_follow(why);   // eye jumps: face the enemies (battle_face)
                        if (swap_in) btg_switched(plys, tagged, I($"switched after {_btl_pending_frames} frames: {ready_why}"));   // battle tag: hold the eye on the spot, or blink; the swap's line
                        _btl_pending_frames = 0;
                    }
                }
                btg_hold_tick(plys);   // battle tag: the eye held on the spot until he arrives
                if (_btl_follow_id >= 0) {
                    if (_btl_chr < 0 || !read(plys + _btl_chr, out Chr cc) || cc.chr_id != _btl_follow_id) _btl_chr = ply_of(_btl_follow_id);
                    int sl = slot_of(_btl_follow_id); if (sl >= 0) _btl_follow_slot = sl;
                    if (_btl_chr >= 0 && read(plys + _btl_chr, out Chr fc) && fc.actor != null && read(fc.actor, out Actor fa) && live(fa)) {
                        pick = (int)(((byte*)fc.actor - (byte*)actors) / sizeof(Actor)); if (pick < 0 || pick >= 48) pick = -1;
                    }
                }
            }
        }
        btg_guard(in_battle);   // battle tag: a hold the follow logic did not send this frame
        if (in_battle) ej_face_tick(btl_v.battle_state);   // eye jumps: the pending face request (waits through the load chain: review B1)
        _btl_cycle_req = 0;   // a request (Insert, the chord) is used by the manual branch above this frame, or dropped (field, summon, no roster)
        if (pick >= 0) { _fp_how = "battle: follows the roster"; _fp_chr_id = -1; }
        else {
            // FP-1: the field player. fp_actor is a list of model names, best first (Tidus is c001 for most of the game,
            // c101 in the opening). Every live actor gets a score, lower is better: a listed name = its rank; a party
            // model (chr_id 1..0xFFF, the "c" models) = 1000 + chr_id, so Tidus beats a follower; actor 0 = 100000 (the
            // last resort); anything else (NPCs, objects) is no candidate. In Luca actor 0 is an object (f001), which
            // the old "actor 0" fallback followed. The current actor is kept while it is live, is the same actor (its
            // chr_id: a slot is reused across a map change, FP-R2) and scores 0; otherwise the table is rescanned at
            // once if it died or changed, else every 30 frames; ties keep the current actor.
            string[] names = fp_names();
            // FP-2: a listed model the game hides (bit 0x01 of actor+0x180; 0x08 / 0x20 are the frustum and distance
            // culls, not counted) ranks after every visible listed model but still before the party fallback. In the
            // Luca cutscene lc/lchb1800 the field Tidus (c001, actor 0) is hidden and parked off-stage while the event
            // Tidus (c101, actor 12) plays the scene.
            // Chocobo: a party model the game hides (c002..c007 parked at (0, 0, 0) while Tidus is swapped onto the bird) ranks
            // after actor 0 (RideLogic.FpScore).
            // cpu-lean (e): the scan reads the frame's snapshot of the table (tbl_snap: one guarded read, shared with the body
            // hide) instead of one read per slot, and a slot's name rank is kept while its name pointer, chr_id and the list are
            // the same (fp_name_rank). The rule, the order and the cadence are unchanged: the actor-0 last resort, a hidden
            // listed model and a hidden party model still rescan every frame, they just cost no reads when nothing changed.
            Actor* snap = null;
            int score_of(in Actor a, int i, Actor* tbl) {
                if ((ulong)i >= 48) return -1;   // (never: the callers pass table indices)
                int r = fp_name_rank(i, a.chr_name, a.chr_id, names);
                if (r >= 0) return RideLogic.FpScore(r, slot_hidden(tbl, snap, i), (int)a.chr_id, i);
                int cid = (int)a.chr_id;
                return RideLogic.FpScore(-1, cid > 0 && cid < 0x1000 && slot_hidden(tbl, snap, i), cid, i);
            }
            int cur_score = -1;
            if (_fp_actor_idx >= 0 && read(actors + _fp_actor_idx, out Actor cur) && live(cur) && (int)cur.chr_id == _fp_chr_id) cur_score = score_of(cur, _fp_actor_idx, actors);
            _fp_scan_tick++;
            if ((_fp_scan_tick % 30) == 0) _fpn_frame_clear = true;   // the name ranks are read again with the 30-frame rescan (a backstop)
            if (cur_score == 0 || (cur_score > 0 && cur_score < 100000 && (cur_score < 500 || cur_score >= 1000) && (_fp_scan_tick % 30) != 0)) pick = _fp_actor_idx;   // the actor-0 last resort (FP-R3) and a hidden listed model (FP-2) rescan every frame
            else {
                int best = int.MaxValue, best_i = -1;
                if (cur_score >= 0) { best = cur_score; best_i = _fp_actor_idx; }   // ties keep the current actor
                snap = tbl_snap(actors);
                Actor slot = default;
                for (int i = 0; i < 48; i++) {
                    Actor* ap = snap != null ? snap + i : &slot;
                    if (snap == null && !read(actors + i, out slot)) break;
                    if (!live(*ap) || i == best_i) continue;
                    int s = score_of(*ap, i, actors);
                    if (s >= 0 && s < best) { best = s; best_i = i; }
                }
                pick = best_i;
                if (pick >= 0 && (pick != _fp_actor_idx || cur_score < 0) && read(actors + pick, out Actor pa2)) {
                    string how = RideLogic.FpHow(best, names);
                    _fp_chr_id = (int)pa2.chr_id;
                    _fp_how = $"actor {pick} '{actor_name(pa2.chr_name)}' ({how})";
                    string mp = map_name();
                    if (_fp_how + mp != _fp_how_logged) { _fp_how_logged = _fp_how + mp; _logger.Info($"first person: following {_fp_how} on map {mp}"); }
                }
            }
            if (pick < 0) { _fp_how = ""; _fp_chr_id = -1; }
            // gates (4 Nov): the pick against the game's own player actor ([0x1300788], Sg_GetPlayer), every 10th scan tick in
            // the field in first person: a mismatch is logged (rate-limited), the pick is not changed yet (game_gates.cs).
            if (!in_battle && _fp_mode == 1 && (_fp_scan_tick % 10) == 0) gates_pick_check(actors, pick);
        }
        _fp_actor_idx = pick;
        System.Threading.Volatile.Write(ref _d7_follow_actor, pick >= 0 ? (nint)(actors + pick) : 0);   // build 7: the actor the main-thread snapshot reads
        if (pick >= 0 && read(actors + pick, out Actor p)) {
            // The head: the highest sub-position that is not at the origin. Actor
            // space has Y pointing down (render space negates it), so the head is
            // the one with the LOWEST actor-space Y.
            float hy = float.MaxValue; Vector4 head = default; bool have = false;
            { int hk = fp_head_pick(ref p, _fp_reach_live, out int hold); if (hk >= 0) { head = p.chr_optpos[hk]; hy = head.Y; have = true; } fph_skip_log(ref p, hk, hold, _fp_reach_live); }
            // Riding (chocobo, ride.cs): an actor attached to another's bone (+0x19C) is drawn from the parent's bone and root, never
            // from its +0x0C (Tidus on the bird sits at a stale (0, 36.3, 0)): its position is the drawn root +0x200, and its
            // sub-positions, built from that root, are world positions already. Field only: battle follows the roster as before.
            long t_ride = cpu_now();
            bool att = !in_battle && ride_probe(actors, pick);
            _cpu_ride += cpu_now() - t_ride;
            if (att && !_ride_built) have = false;   // CR-1: the rider's root is not built yet, nor his sub-positions: the eye is the stand-in root + eye height
            // npc-anchor round 3: is the body drawn away from its root (a shared scene anchor)? Then the eye's horizontal position
            // comes from the head (bit 1 below). First person, field, on foot, not blitzball; gated on another actor on his root
            // (eye review SF2). Decided before the fold below: a displaced head is absolute however far it is (eye review SF1).
            bool eye_on = have && eye_disp_step(actors, ref p, pick, _fp_mode == 1 && !att && !in_battle && !_in_blitz);
            // The sub-positions look absolute in the actor table; if one ever turns out to be relative, fold the actor position in.
            // Never for an attached actor: its +0x0C is not where it is (the fold put the riding eye 36 units low); nor for a body
            // drawn away from its root (eye review SF1: the fold moved that eye ~257 units off once the body was 100+ units out).
            if (have && !att && !eye_on && MathF.Abs(head.X - p.chr_pos_vec.X) + MathF.Abs(head.Z - p.chr_pos_vec.Z) > 100f) head += p.chr_pos_vec;
            // Smooth follow anchor: the post-join snapshot's drawn (interpolated) root when fresh, else +0x0C as before (riding: +0x200 now)
            Vector3 pos3 = att ? _ride_rider : new Vector3(p.chr_pos_vec.X, p.chr_pos_vec.Y, p.chr_pos_vec.Z), head3 = new Vector3(head.X, head.Y, head.Z);
            sa_apply((nint)(actors + pick), ref pos3, ref head3, ref have, att);
            if (att) ride_eye(ref pos3, ref head3);   // the body-hide centre stays the rider's root; the eye base gets the bob filtered
            _fn_player_set(-pos3.X, -pos3.Y, pos3.Z, 1);   // first: the head is stored as an offset from it
            // npc-anchor round 3: bit 1 = the body is drawn away from its root (eye_on above): the eye's horizontal position from the
            // head (fp_head_anchor 0 / 1 used the root's). Riding, battle and blitzball are unchanged.
            int hv = have ? 1 : 0;
            if (have && eye_on) hv |= 2;
            if (_fn_player_head_set != null) _fn_player_head_set(-head3.X, -head3.Y, head3.Z, hv);
            // the body hide's ownership (body_hide.cs): after player_set, so a re-pick resets the step margin it just widened
            // the re-pick key: the index and the model (N1: lc/lchb0500 swaps c001 -> c101 in the same slot)
            int root_kind = body_hide_root(actors, pick, att, out Vector3 root3);
            body_hide_push(actors, pick, (pick << 16) | ((int)p.chr_id & 0xFFFF), !in_battle, root3, root_kind, att ? _ride_rider : pos3);
        }
        else { _fn_player_set(0, 0, 0, 0); if (_fn_player_head_set != null) _fn_player_head_set(0, 0, 0, 0); body_hide_push(null, -1, -1, false, default, 0, default); _sa_used = false; _sa_status = "no followed actor"; }   // SM-5
        push_view_rules(in_battle);
    }
    /// <summary>The rest of push_player: head anchor, battle anchor, floor, hide rules, stereo screen, virtual screen,
    /// markers. Each call has its own null check (they used to sit inside an unrelated `_fn_ui_hide != null`), and they
    /// also run when there is no actor table yet (the title screen's virtual-screen mode).</summary>
    private void push_view_rules(bool in_battle) {
        ride_push();   // riding: the rider / mount roots for the body hide, the riding line (ride.cs)
        if (_fn_fp_head_set != null) _fn_fp_head_set(_fp_head_anchor, _fp_head_up, _fp_head_smooth, _fp_head_fwd);
        push_battle_anchor();
        if (_fn_floor_set != null) _fn_floor_set(_diorama_floor);
        if (_fn_ui_hide != null) {
            bool hide = _ui_hide == 1 || _ui_hide == 2 || (_ui_hide == 3 && in_battle);
            _fn_ui_hide(hide ? 1024u : 0u, 1024u, (_ui_hide == 2 || (_ui_hide == 4 && in_battle)) ? 1 : (_ui_hide == 5 && in_battle) ? 2 : 0);
        }
        if (_fn_ui_hidden_draws != null) _ui_hidden_n = _fn_ui_hidden_draws();
        if (_fn_ui_hide_cursor != null) _fn_ui_hide_cursor(_hide_cursor && in_battle && _xr_on ? 1 : 0);   // headset only: on the monitor the game's cursor is right where it should be
        if (_fn_ui_hide_pointers != null) _fn_ui_hide_pointers(_hide_pointers && in_battle && _xr_on && !_c2_active ? 1 : 0);   // C2: the game's cursors are skipped at the source
        if (_fn_ui_pointer_stats != null) _pointer_stats = _fn_ui_pointer_stats();
        if (_fn_blitz_overlay != null) _blitz_overlay_n = _fn_blitz_overlay(_blitz_overlay && _in_blitz ? 1 : 0);
        // stereo screen (pushed with the window hidden too; the screen's size and mode go with it)
        if (_fn_screen3d != null) _fn_screen3d((_s3_all || (_s3_blitz && _in_blitz)) ? 1 : 0, _s3_depth, _s3_plane, _s3_auto ? 1 : 0, _s3_max_pop * 0.01f);
        if (_fn_xr_screen_set != null) _fn_xr_screen_set(_xr_screen_mode, _xr_screen_width, _xr_screen_dist);
        if (_fn_markers_game_art != null) _markers_game_art_n = _fn_markers_game_art(0);   // C2: the captured-sprite experiment is retired (wrong UVs); markers_game_art in the ini is kept but ignored. Stage C3 brings the game's look back from battle.dds
        if (_fn_markers_scale != null) _fn_markers_scale(_marker_scale);
        if (_hide_list_dirty && _fn_ui_hide_list != null) {
            _hide_list_dirty = false; float* buf = stackalloc float[16 * 6];
            int cnt = c2_hide_list(buf);   // C2: without the two default cursor entries while the game source is in use
            _fn_ui_hide_list(buf, cnt);
        }
        if (_fn_ui_hide_hits != null) { fixed (uint* hp = _hide_hits) _fn_ui_hide_hits(hp, 16); }
        if (_fn_ui_rect_stats != null) _rect_stats = _fn_ui_rect_stats();
    }

    private void push_markers() {
        if (_fn_markers_set == null) return;
        fixed (float* p = _mk_pos) fixed (uint* c = _mk_col) _fn_markers_set(p, c, _mk_n);
        c3_push();   // C3: the kinds (after the set, which resets them)
        dmg3d_push();   // 3D damage numbers: the latest snapshot (dmg3d.cs; nothing while off)
        if (_fn_cursor_game != null) _fn_cursor_game(_c2_active && _markers_on ? 1 : 0);   // only then may native skip the game's cursors (in the headset)
        if (_fn_markers_3d != null) _fn_markers_3d(_markers_3d ? 1 : 0);
    }

    private void draw_markers() {
        _markers_drawn = 0; _mk_n = 0; Array.Clear(_mk_kind);   // kind 0 (our triangle) unless the game source says otherwise
        try { c2_decide(); if (_c2_active) c2_markers(); else { c3_look(false); draw_markers_inner(); } }   // cursor stage C2: the game's snapshots, else the legacy memory path
        finally { push_markers(); }
    }

    // The game's own field roster (exe-analysis.md section 15): three chr ids at
    // 0xD2C895 (0xFF = empty slot), the party saved while an Aeon is out at
    // 0xD2C89C (7 bytes), reserves at 0xD2C8A3 (17 bytes). Logged for now;
    // meant to replace the distance heuristics in btl_follow once confirmed.
    private static string btl_field_str() {
        try {
            var sb = new System.Text.StringBuilder("[");
            for (int i = 0; i < 3; i++) { if (i > 0) sb.Append(','); sb.Append(FhUtil.get_at<byte>(0xD2C895 + i)); }
            sb.Append("] saved=[");
            for (int i = 0; i < 7; i++) { if (i > 0) sb.Append(','); sb.Append(FhUtil.get_at<byte>(0xD2C89C + i)); }
            sb.Append("] reserve=[");
            for (int i = 0; i < 7; i++) { if (i > 0) sb.Append(','); sb.Append(FhUtil.get_at<byte>(0xD2C8A3 + i)); }
            sb.Append(']');
            return sb.ToString();
        } catch { return "?"; }
    }

    private void draw_markers_inner() {
        if (!_markers_on || _fn_project == null || _fn_safe_read == null) return;

        Btl* btl = Globals.Battle.btl;
        if (!read(btl, out Btl btl_v) || btl_v.battle_state == 0) return;

        BtlWindow* win = Globals.Battle.cur_window;
        if (!read(win, out BtlWindow w)) return;

        Chr* mons = Globals.Battle.monster_characters;
        Chr* plys = Globals.Battle.player_characters;

        // Once a second, log the game's actor positions next to the renderer's
        // world positions, to work out the relation between the two spaces.
        double now = ImGui.GetTime();
        if (_diag_verbose && now - _last_pos_log > 1.0 && _fn_world_positions != null) {   // release prep: diag_verbose only
            _last_pos_log = now;
            var sb = new System.Text.StringBuilder(
                $"window: state={w.state} type={w.type} cur_chr_id={w.cur_chr_id} depth={w.submenu_depth} cmd={w.window_command_id} "
              + $"sel_idx={w.sel_idx} sel_idx_render={w.sel_idx_render} sel_cmd={w.sel_command_id} sel_chrs=0x{w.sel_chrs:X4} sel_mons=0x{w.sel_mons:X4}"
              + $"  field={btl_field_str()}  actors:");
            for (int i = 0; i < k_ply_n; i++) {
                if (i < k_mon_n && mons != null && read(mons + i, out Chr mc) && mc.actor != null && read(mc.actor, out Actor ma))
                    sb.Append($" mon{i}[id={mc.id} chr_id={mc.chr_id} vis={mc.stat_visible}]=({ma.chr_pos_vec.X:F1},{ma.chr_pos_vec.Y:F1},{ma.chr_pos_vec.Z:F1})");
                if (plys != null && read(plys + i, out Chr pc) && pc.actor != null && read(pc.actor, out Actor pa))
                    sb.Append($" ply{i}[id={pc.id} chr_id={pc.chr_id} vis={pc.stat_visible} hide={pc.stat_hide}]=({pa.chr_pos_vec.X:F1},{pa.chr_pos_vec.Y:F1},{pa.chr_pos_vec.Z:F1})");
            }
            float* wp = stackalloc float[128 * 3];
            int n = _fn_world_positions(wp, 128);
            sb.Append($"  | rendered skinned world positions ({n}):");
            for (int i = 0; i < n; i++) sb.Append($" ({wp[i*3]:F1},{wp[i*3+1]:F1},{wp[i*3+2]:F1})");
            _logger.Info(sb.ToString());
        }

        // Selection is one 32-bit mask over combatant ids: sel_chrs holds ids
        // 0..15 (party), sel_mons ids 16..31 (monsters are 20 + slot). Keyed on
        // the id each slot reports, so sparse or reordered slots don't matter.
        uint selected = w.sel_chrs | ((uint)w.sel_mons << 16);
        for (int i = 0; i < k_ply_n; i++)   // 18: Aeon chr ids run to 0x11, so Sandy and Mindy are combatant ids 16-17 (bits 16-17 of the mask above)
            if (plys != null && read(plys + i, out Chr pc) && pc.id < 32 && (selected & (1u << pc.id)) != 0) mark(plys + i, 0xFF4040FF, "target");
        for (int i = 0; i < k_mon_n; i++)
            if (mons != null && read(mons + i, out Chr mc) && mc.id < 32 && (selected & (1u << mc.id)) != 0) mark(mons + i, 0xFF4040FF, "target");
        if (plys != null) {
            for (int i = 0; i < k_ply_n; i++) {
                if (read(plys + i, out Chr pc) && pc.id == w.cur_chr_id) { mark(plys + i, 0xFF40E0FF, "turn"); break; }
            }
        }
    }

    private void mark(Chr* chr, uint color, string label) {
        if (!read(chr, out Chr c) || c.actor == null) return;
        if (!read(c.actor, out Actor a)) return;
        if (c.stat_visible == 0 && label == "target") { /* still mark; the game does */ }

        // Where the pointer's tip goes: just above the actor's head - the highest sub-position (actor space
        // has Y down, so the lowest Y), which follows each monster's own height - plus the clearance slider.
        float head_y = a.chr_pos_vec.Y; bool have_head = false;
        for (int k = 0; k < 26; k++) { Vector4 sp = a.chr_optpos[k]; if (MathF.Abs(sp.X) + MathF.Abs(sp.Z) > 0.01f && sp.Y < head_y && a.chr_pos_vec.Y - sp.Y < 400f) { head_y = sp.Y; have_head = true; } }
        float tip_y = -(have_head ? head_y : a.chr_pos_vec.Y - 60f) + _marker_height;   // render space (Y negated)
        // For the headset: the same point as 3D geometry, drawn natively per eye.
        if (_mk_n < 32) {
            _mk_pos[_mk_n * 3 + 0] = -a.chr_pos_vec.X; _mk_pos[_mk_n * 3 + 1] = tip_y; _mk_pos[_mk_n * 3 + 2] = a.chr_pos_vec.Z;
            _mk_col[_mk_n] = color; _mk_n++;
        }

        float* ndc = stackalloc float[2];
        // Actor positions are in the game's PS2-era space; the renderer's world
        // space negates X and Y (verified against the skinned world matrices).
        if (_fn_project(-a.chr_pos_vec.X, tip_y, a.chr_pos_vec.Z, ndc) == 0) return;

        Vector2 size = ImGui.GetIO().DisplaySize;
        float sx = (ndc[0] * 0.5f + 0.5f) * size.X;
        float sy = (0.5f - ndc[1] * 0.5f) * size.Y;

        ImDrawListPtr dl = ImGui.GetForegroundDrawList();
        // a downward-pointing triangle, like the game's cursor
        dl.AddTriangleFilled(new Vector2(sx - 14, sy - 30), new Vector2(sx + 14, sy - 30), new Vector2(sx, sy - 4), color);
        dl.AddTriangle(new Vector2(sx - 14, sy - 30), new Vector2(sx + 14, sy - 30), new Vector2(sx, sy - 4), 0xFF000000, 2f);
        dl.AddText(new Vector2(sx - 16, sy - 48), color, label);
        _markers_drawn++;
    }

    private int _render_faults = 0;
    /// <summary>Fahrenheit calls this from inside Present. An exception escaping it fail-fasts the whole
    /// game (exit 0xC0000409, no dump), so everything runs under a catch that logs the stack instead.</summary>
    public override void render_imgui() {
        cpu_frame_start();   // cpu-lean: this frame's Stopwatch sections (cpu_report.cs)
        try { dpi_apply(); _our_px = our_font_px(); }
        catch (Exception e) { _our_px = k_fh_font_px; if (_render_faults++ < 5) _logger.Error($"dpi_apply fault #{_render_faults}: {e}"); }
        ImGui.PushFont(null, _our_px);   // FH-2: one push around all our ImGui output (outside the try: a Pop never runs without it)
        try { render_imgui_inner(); }
        catch (Exception e) {
            if (_render_faults++ < 5) _logger.Error($"render_imgui fault #{_render_faults}: {e}");
        }
        finally { ImGui.PopFont(); }     // before Fahrenheit's next module; every Begin inside ends in its own finally / End
        if (_native_ok) cpu_frame_report();   // cpu-lean: one ffxvr_cpu_report a frame (no allocation)
    }

    /// <summary>FH-1, every frame before our push (render thread, after Fahrenheit's NewFrame, so always after its platform_bind
    /// wrote the flags). Writes only on an edge: the first frame, a context Fahrenheit made again, someone turning the flags on
    /// again (logged 5 times, then only counted in the Debug row), or fh_dpi_fix going to 0.</summary>
    private void dpi_apply() {
        ImGuiIOPtr io = ImGui.GetIO(); ImGuiStylePtr st = ImGui.GetStyle();
        bool fonts = io.ConfigDpiScaleFonts, vps = io.ConfigDpiScaleViewports;
        if (_dpi_fh_fonts < 0) {
            _dpi_fh_fonts = fonts ? 1 : 0; _dpi_fh_vps = vps;
            _logger.Info(I($"ImGui: Fahrenheit's DPI font scaling {(fonts ? "on" : "off")} (viewports {(vps ? "on" : "off")}), Windows scale {ImGui.GetMainViewport().DpiScale:F2}, FontScaleDpi {st.FontScaleDpi:F2}"));
        }
        if (_fh_dpi_fix && (fonts || vps)) {
            float was = st.FontScaleDpi;
            io.ConfigDpiScaleFonts = false; io.ConfigDpiScaleViewports = false; st.FontScaleDpi = 1f; _dpi_ours = true;
            if (++_dpi_offs <= 5) _logger.Info(I($"ImGui: DPI font scaling off (fh_dpi_fix=1, as Fahrenheit #376); FontScaleDpi {was:F2} -> 1{(_dpi_offs == 5 ? "; further repeats are only counted (Debug row)" : "")}"));
        } else if (!_fh_dpi_fix && _dpi_ours) {   // ImGui sets FontScaleDpi again at the next frame
            io.ConfigDpiScaleFonts = _dpi_fh_fonts == 1; io.ConfigDpiScaleViewports = _dpi_fh_vps; _dpi_ours = false;
            _logger.Info("ImGui: DPI font scaling back to Fahrenheit's own setting (fh_dpi_fix=0)");
        }
    }

    /// <summary>FH-2: our text keeps the size it had under Fahrenheit's DPI scaling, 20 px x the Windows scale, whether
    /// FontScaleDpi is now 1 (fh_dpi_fix=1, alpha13) or the scale itself (fh_dpi_fix=0), times overlay_text_scale. Called
    /// before our push: inside it, style.FontSizeBase holds the pushed size (imgui.cpp UpdateCurrentFontSize), so it is
    /// never read here; the base is Fahrenheit's constant 20 px.</summary>
    private float our_font_px() {
        float s = ImGui.GetMainViewport().DpiScale; if (!(s >= 0.5f && s <= 4f)) s = 1f;
        float f = ImGui.GetStyle().FontScaleDpi;   if (!(f >= 0.5f && f <= 4f)) f = 1f;   // someone else's odd value: no 512 px bake
        return k_fh_font_px * s * text_scale_step(_overlay_text_scale) / f;   // PushFont's size is applied before FontScaleDpi (imgui.cpp:9836)
    }
    /// <summary>overlay_text_scale as used: 0.5..2 in steps of 0.05 (a hand-typed 1.234 reads as 1.25; NaN as 1).</summary>
    private static float text_scale_step(float k) => float.IsFinite(k) ? Math.Clamp(MathF.Round(k * 20f) / 20f, 0.5f, 2f) : 1f;

    private void render_imgui_inner() {
        _frames++;
        _now = ImGui.GetTime();   // R33: one clock per frame for the settings timers and the pick poll

        diag7_resolve();   // build 7 (B7-1): the game-function detours' originals, before try_install_hooks adds our late hooks
        ld_resolve();      // load diagnostics: the same, for their detours
        sr_resolve();      // stickrep: the same, for the flush gate's two detours
        dmg3d_resolve();   // 3D damage numbers: the same, for the queue draw's detour (dmg3d_hide2d)
        try_install_hooks();
        if (_native_ready) draw_markers();
        if (_native_ready && _diag_actors) actors_auto_log();   // LD2-8: diag_actors (default 0)
        if (_native_ready) log_hello();   // release prep: once, the version and the diagnostics at the top of ffxvr_diag.log (logging.cs)
        if (_native_ready && _ini_diag_note != null && _fn_diag_line != null) { foreach (string l in _ini_diag_note.Split('\n')) d7_log(l); _ini_diag_note = null; }   // LD2-5: once, into ffxvr_diag.log
        if (_native_ready) { try { blitz_auto_log(); } catch (Exception e) { if (_blitz_log_lines < 1200) { _blitz_log_lines = 1200; _logger.Info($"blitz log off: {e.Message}"); } } }
        if (_native_ready) draw_eye2_preview();   // its own top-level window, never nested

        if (_native_ready) ld_present();     // load diagnostics: battle phases, the summary lines
        if (_native_ready) diag7_present();   // build 7 (log only). The Motion log's managed columns come from push_player (after begin_frame) and print at the next begin_frame, paired with that line's "actor" (the same push_player)

        // Marks the frame boundary for the native helper. Fahrenheit calls
        // render_imgui from inside Present, so this fires exactly once a frame.
        if (_native_ok && _fn_begin_frame != null) { _cpu_t_bf0 = cpu_now(); _fn_begin_frame(); _cpu_t_bf1 = cpu_now(); }

        handle_hotkeys();
        if (_native_ready) {
            draw_hover_highlight();
            if (_fn_slowmo != null) _fn_slowmo(_slowmo_on ? _slowmo_ms : 0);
            if (_fn_ui_pick_result != null && (_now < _pick_poll_until || _pick_poll_frames > 0)) {   // R22: only for 3 s (and 3 more frames, for slow motion) after an F9
                if (_now >= _pick_poll_until) _pick_poll_frames--;
                try { string r = Marshal.PtrToStringAnsi(_fn_ui_pick_result()) ?? ""; if (r.Length > 0) _pick_text = r; } catch { }
            }
            poll_gestures();
        }
        if (_window_open) {   // widgets, status and one-shot buttons only; a fault in there must not stop push_all
            _cpu_t_win0 = cpu_now();
            try { draw_window(); }
            catch (Exception e) { if (e.Message != _ui_fault_last) { _ui_fault_last = e.Message; _logger.Error($"overlay fault: {e}"); } }
            _cpu_t_win1 = cpu_now();
        }
        if (_native_ready) { _cpu_t_push0 = cpu_now(); push_all(); _cpu_t_push1 = cpu_now(); }      // every setting, once per frame, open or closed
        settings_reload_check();   // R31/R33: a hand edit, every second of _now
        settings_save_tick();      // R21: debounced save
        _cpu_t_end = cpu_now(); _cpu_done = true;
    }

    /// <summary>The F8 window. Without a working native helper it shows why (and the hint), never an empty frame.</summary>
    private void draw_window() {
        ImGui.SetNextWindowSize(new Vector2(520, 0), ImGuiCond.FirstUseEver);
        try {
            if (ImGui.Begin("FFX VR", ref _window_open)) {
                if (!_native_ready) {
                    ImGui.TextWrapped($"Native helper: {esc(_native_status)}");
                    if (_ini_note.Length > 0) { ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.8f, 0.3f, 1f)); ImGui.TextWrapped(esc(_ini_note)); ImGui.PopStyleColor(1); }   // R30 / B8-12
                    if (_native_mismatch) ImGui.TextWrapped("The VR mod is not running. Reinstall the mod, so that ffxvr.dll and ffxvr_native.dll come from the same build; core.log has the details.");
                    else if (_native_failed) ImGui.TextWrapped("The VR mod is not running: it could not hook the game's renderer. core.log and ffxvr_native.log have the details.");
                    else ImGui.TextDisabled("starting: waiting for the game's renderer");
                    ImGui.TextDisabled("F8: hide");
                }
                else {
                    if (_detour_status != "installed") {
                        ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.35f, 0.35f, 1f));
                        ImGui.TextWrapped($"Draw detours: {esc(_detour_status)}. Stereo, cursor hiding and the UI panel may not work; core.log has the details.");
                        ImGui.PopStyleColor(1);
                    }
                    if (_ini_note.Length > 0) { ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.8f, 0.3f, 1f)); ImGui.TextWrapped(esc(_ini_note)); ImGui.PopStyleColor(1); }   // R30, for the session
                    draw_main();
                    draw_debug();
                }
            }
        }
        finally { ImGui.End(); }
    }
}
