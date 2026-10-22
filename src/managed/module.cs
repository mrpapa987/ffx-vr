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
public sealed unsafe class FFXVRModule : FhModule {

    private const string NativeDll = "ffxvr_native.dll";

    private ulong _frames;
    private bool  _window_open = true;

    private nint   _native_handle;
    private string _native_status = "not loaded";
    private bool   _native_init_done;

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
    private delegate* unmanaged[Cdecl]<float, void> _fn_turn;
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
    private delegate* unmanaged[Cdecl]<float*, int, void> _fn_ui_hide_list; private delegate* unmanaged[Cdecl]<uint*, int, void> _fn_ui_hide_hits; private delegate* unmanaged[Cdecl]<uint*, uint*, void> _fn_draw_skip;
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
    private delegate* unmanaged[Cdecl]<float, void> _fn_floor_set; private float _diorama_floor = 8f;
    private delegate* unmanaged[Cdecl]<int, float, float, float, void> _fn_fp_head_set;
    private int _fp_head_anchor = 1; private float _fp_head_up = 1.5f, _fp_head_smooth = 0.35f, _fp_head_fwd = 1.5f;   // 0 fixed, 1 head-bone height (default: swimming, crouching), 2 full head bone
    private delegate* unmanaged[Cdecl]<int> _fn_fp_hidden;
    private int _fp_mode = 0; private float _fp_eye_h = 18f, _fp_follow_dist = 60f, _fp_follow_h = 25f, _fp_hide_radius = 0.75f;
    private float _tp_dist = 24f, _tp_h = 22f, _tp_pitch = -8f, _tp_follow_rate = 0f;   // third person: units, units, degrees, deg/s (0 = heading never follows)
    private delegate* unmanaged[Cdecl]<float, float, float, float, void> _fn_tp_set;
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
            foreach (string l in File.ReadAllLines(_bone_labels_path)) { var t = l.Split('\t'); if (t.Length >= 3 && int.TryParse(t[0], out int r) && int.TryParse(t[1], out int s)) _bone_labels[(r, s)] = t[2]; } }
        catch { }
    }
    private void bone_labels_save() {
        try { var lines = new List<string>(); foreach (var kv in _bone_labels) lines.Add($"{kv.Key.Item1}\t{kv.Key.Item2}\t{kv.Value}"); lines.Sort(); File.WriteAllLines(_bone_labels_path, lines); }
        catch { }
    }
    private bool _fp_hide_body = true; private float _scale_before_fp = 94f;
    private int _fp_body_mode = 0; private string _fp_head_parts = "0,1,2,3,9"; private int _fp_solo = -1;   // 0 = whole body hidden (default). Tidus (c101): parts 0-3 hair, 9 face/head, kept for later   // 0 hide all, 1 hide head parts only, 2 show all
    private delegate* unmanaged[Cdecl]<int, uint, int, void> _fn_fp_body_set;
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
    private delegate* unmanaged[Cdecl]<int, int, int, void> _fn_imgui_layer_set; private delegate* unmanaged[Cdecl]<uint> _fn_imgui_draws; private bool _imgui_layer = true; private int _imgui_min_draws = 12;
    /// <summary>Fahrenheit's save-screen state word (FhSavePal is internal): 0 closed, 1 opening, 2 open.</summary>
    private static byte save_screen_state() { try { return FhUtil.get_at<byte>(0x8CB994); } catch { return 0; } }
    private delegate* unmanaged[Cdecl]<int, float, void> _fn_gesture_set;
    private delegate* unmanaged[Cdecl]<void> _fn_gesture_reset;
    private delegate* unmanaged[Cdecl]<float*, float*, int*, uint> _fn_gesture_poll;
    private bool _chord_toggle_pending = false; private bool _gestures = true; private float _gest_tilt = 1f; private float _gest_scale_mul = 1f; private int _gest_active = 0; private float _gest_move_x, _gest_move_y, _gest_move_z;
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
    private string _pick_text = ""; private int _pick_n = 0; private int _slowmo_ms = 150; private bool _slowmo_on = false;
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
        _logger.Info("FFX VR module initialized.");
        settings_load();
        load_native(mod_context);
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

        if (!File.Exists(path)) {
            _native_status = "missing: " + NativeDll;
            _logger.Warning($"Native helper not found at {path}. Constant buffer capture is off.");
            return;
        }

        try {
            _native_handle = NativeLibrary.Load(path);

            _fn_init         = (delegate* unmanaged[Cdecl]<nint, nint, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_init");
            _fn_buffer_count = (delegate* unmanaged[Cdecl]<int>)      NativeLibrary.GetExport(_native_handle, "ffxvr_buffer_count");
            _fn_dump_count   = (delegate* unmanaged[Cdecl]<int>)      NativeLibrary.GetExport(_native_handle, "ffxvr_dump_count");
            _fn_hook_calls   = (delegate* unmanaged[Cdecl]<int>)      NativeLibrary.GetExport(_native_handle, "ffxvr_hook_calls");
            _fn_begin_frame  = (delegate* unmanaged[Cdecl]<void>)     NativeLibrary.GetExport(_native_handle, "ffxvr_begin_frame");
            _fn_frame_writes = (delegate* unmanaged[Cdecl]<int>)      NativeLibrary.GetExport(_native_handle, "ffxvr_frame_writes");
            _fn_get_camera   = (delegate* unmanaged[Cdecl]<float*, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_get_camera");
            _fn_camera_reads = (delegate* unmanaged[Cdecl]<int>)      NativeLibrary.GetExport(_native_handle, "ffxvr_camera_reads");
            _fn_slot0_count  = (delegate* unmanaged[Cdecl]<int>)      NativeLibrary.GetExport(_native_handle, "ffxvr_slot0_count");
            _fn_camera_changes = (delegate* unmanaged[Cdecl]<int>)    NativeLibrary.GetExport(_native_handle, "ffxvr_camera_changes");
            _fn_counter      = (delegate* unmanaged[Cdecl]<int, int>) NativeLibrary.GetExport(_native_handle, "ffxvr_counter");
            _fn_on_draw      = (delegate* unmanaged[Cdecl]<int, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_on_draw");
            _fn_draw_ctx     = (delegate* unmanaged[Cdecl]<nint, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_draw_ctx");
            try { _fn_draw_args = (delegate* unmanaged[Cdecl]<int, uint, uint, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_draw_args"); } catch { }
            _fn_eye2_begin   = (delegate* unmanaged[Cdecl]<int, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_eye2_begin");
            _fn_eye2_end     = (delegate* unmanaged[Cdecl]<void>)NativeLibrary.GetExport(_native_handle, "ffxvr_eye2_end");
            _fn_stereo_set   = (delegate* unmanaged[Cdecl]<int, float, int, int, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_stereo_set");
            _fn_xr_set       = (delegate* unmanaged[Cdecl]<int, float, int, int, int, int, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_xr_set");
            _fn_xr_recenter  = (delegate* unmanaged[Cdecl]<void>)NativeLibrary.GetExport(_native_handle, "ffxvr_xr_recenter");
            _fn_xr_screen_set = (delegate* unmanaged[Cdecl]<int, float, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_xr_screen_set");
            _fn_ui_set       = (delegate* unmanaged[Cdecl]<int, float, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_set");
            _fn_ui_draws     = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_draws");
            _fn_xr_pitch_follow = (delegate* unmanaged[Cdecl]<float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_xr_pitch_follow");
            _fn_cuts_set     = (delegate* unmanaged[Cdecl]<int, int, int, float, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_cuts_set");
            _fn_turn_set     = (delegate* unmanaged[Cdecl]<int, int, float, float, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_turn_set");
            _fn_turn         = (delegate* unmanaged[Cdecl]<float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_turn");
            _fn_turn_count   = (delegate* unmanaged[Cdecl]<uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_turn_count");
            _fn_perf_status  = (delegate* unmanaged[Cdecl]<byte*, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_perf_status");
            _fn_noimg_set    = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_noimg_set");
            _fn_noimg_draws  = (delegate* unmanaged[Cdecl]<uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_noimg_draws");
            _fn_bisect_set   = (delegate* unmanaged[Cdecl]<int, int, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_bisect_set");
            _fn_scene_draws  = (delegate* unmanaged[Cdecl]<uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_scene_draws");
            _fn_blend_off    = (delegate* unmanaged[Cdecl]<void>)NativeLibrary.GetExport(_native_handle, "ffxvr_blend_off");
            _fn_blend_restore= (delegate* unmanaged[Cdecl]<void>)NativeLibrary.GetExport(_native_handle, "ffxvr_blend_restore");
            _fn_hist_set     = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_hist_set");
            _fn_hist_draws   = (delegate* unmanaged[Cdecl]<uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_hist_draws");
            _fn_move_set     = (delegate* unmanaged[Cdecl]<int, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_move_set");
            _fn_move_status  = (delegate* unmanaged[Cdecl]<float*, float*, float*, uint*, int*, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_move_status");
            _fn_cut_count    = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_cut_count");
            _fn_fp_set       = (delegate* unmanaged[Cdecl]<int, float, float, float, int, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_fp_set");
            _fn_player_set   = (delegate* unmanaged[Cdecl]<float, float, float, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_player_set");
            _fn_player_head_set = (delegate* unmanaged[Cdecl]<float, float, float, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_player_head_set");
            _fn_anchor_override = (delegate* unmanaged[Cdecl]<float, float, float, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_anchor_override");
            _fn_floor_set    = (delegate* unmanaged[Cdecl]<float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_floor_set");
            _fn_fp_head_set  = (delegate* unmanaged[Cdecl]<int, float, float, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_fp_head_set");
            _fn_fp_hidden    = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_fp_hidden");
            _fn_markers_set  = (delegate* unmanaged[Cdecl]<float*, uint*, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_markers_set");
            _fn_markers_3d   = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_markers_3d");
            try { _fn_markers_game_art = (delegate* unmanaged[Cdecl]<int, uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_markers_game_art"); _fn_markers_scale = (delegate* unmanaged[Cdecl]<float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_markers_scale"); _fn_ui_atlas_dump = (delegate* unmanaged[Cdecl]<void>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_atlas_dump"); _fn_motion_log = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_motion_log"); } catch { }
            _fn_xr_screen_now = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_xr_screen_now");
            try { _fn_screen3d = (delegate* unmanaged[Cdecl]<int, float, float, int, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_screen3d"); _fn_screen3d_status = (delegate* unmanaged[Cdecl]<int*, float*, float*, float*, float*, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_screen3d_status"); } catch { }
            _fn_dump_seq     = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_dump_seq");
            _fn_shutdown     = (delegate* unmanaged[Cdecl]<void>)NativeLibrary.GetExport(_native_handle, "ffxvr_shutdown");
            _fn_xr_status    = (delegate* unmanaged[Cdecl]<byte*, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_xr_status");
            _fn_eye2_srv     = (delegate* unmanaged[Cdecl]<int, nint>)NativeLibrary.GetExport(_native_handle, "ffxvr_eye2_srv");
            _fn_eye2_draws   = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_eye2_draws");
            _fn_eye2_size    = (delegate* unmanaged[Cdecl]<int*, int*, int, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_eye2_size");
            _fn_on_updatesub = (delegate* unmanaged[Cdecl]<nint, nint, nint, nint>)NativeLibrary.GetExport(_native_handle, "ffxvr_on_updatesub");
            _fn_on_rendertargets = (delegate* unmanaged[Cdecl]<uint, nint*, nint, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_on_rendertargets");
            _fn_on_rendertargets_uav = (delegate* unmanaged[Cdecl]<uint, nint*, nint, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_on_rendertargets_uav");
            _fn_on_resolve = (delegate* unmanaged[Cdecl]<nint, uint, nint, uint, uint, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_on_resolve");
            _fn_on_viewport  = (delegate* unmanaged[Cdecl]<float, float, float, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_on_viewport");
            _fn_on_clear     = (delegate* unmanaged[Cdecl]<nint, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_on_clear");
            _fn_frame_arm    = (delegate* unmanaged[Cdecl]<void>)NativeLibrary.GetExport(_native_handle, "ffxvr_frame_arm");
            try { _fn_ui_hide = (delegate* unmanaged[Cdecl]<uint, uint, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_hide"); _fn_ui_hidden_draws = (delegate* unmanaged[Cdecl]<uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_hidden_draws"); _fn_ui_hide_cursor = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_hide_cursor"); _fn_ui_rect_stats = (delegate* unmanaged[Cdecl]<uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_rect_stats");
                  _fn_ui_hide_list = (delegate* unmanaged[Cdecl]<float*, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_hide_list"); _fn_ui_hide_hits = (delegate* unmanaged[Cdecl]<uint*, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_hide_hits"); _fn_draw_skip = (delegate* unmanaged[Cdecl]<uint*, uint*, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_draw_skip");
                  _fn_ui_hide_pointers = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_hide_pointers"); _fn_ui_pointer_stats = (delegate* unmanaged[Cdecl]<uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_pointer_stats");
                  _fn_blitz_overlay = (delegate* unmanaged[Cdecl]<int, uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_blitz_overlay");
                  _fn_ui_pick = (delegate* unmanaged[Cdecl]<float, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_pick"); _fn_ui_pick_result = (delegate* unmanaged[Cdecl]<nint>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_pick_result");
                  _fn_slowmo = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_slowmo");
                  _fn_ui_hover = (delegate* unmanaged[Cdecl]<float, float, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_hover"); _fn_ui_hover_rects = (delegate* unmanaged[Cdecl]<float*, int, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_ui_hover_rects"); } catch { }
            _fn_dump_cb      = (delegate* unmanaged[Cdecl]<uint, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_dump_cb");
            _fn_census_arm   = (delegate* unmanaged[Cdecl]<uint, uint, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_census_arm");
            _fn_backdrop_set = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_backdrop_set");
            _fn_cull_dist_set = (delegate* unmanaged[Cdecl]<int, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_cull_dist_set");
            _fn_cull_dist_get = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_cull_dist_get");
            _fn_cull_scene_set = (delegate* unmanaged[Cdecl]<int, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_cull_scene_set");
            _fn_cull_scene_get = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_cull_scene_get");
            _fn_msaa_set     = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_msaa_set");
            _fn_vrpad_set    = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_vrpad_set");
            _fn_imgui_layer_set = (delegate* unmanaged[Cdecl]<int, int, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_imgui_layer_set");
            _fn_imgui_draws  = (delegate* unmanaged[Cdecl]<uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_imgui_draws");
            _fn_vrpad_status = (delegate* unmanaged[Cdecl]<float*, byte*, byte*, int, uint*, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_vrpad_status");
            _fn_haptic       = (delegate* unmanaged[Cdecl]<int, float, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_haptic");
            _fn_gesture_set  = (delegate* unmanaged[Cdecl]<int, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_gesture_set");
            _fn_gesture_reset = (delegate* unmanaged[Cdecl]<void>)NativeLibrary.GetExport(_native_handle, "ffxvr_gesture_reset");
            _fn_tp_set       = (delegate* unmanaged[Cdecl]<float, float, float, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_tp_set");
            _fn_bones_capture = (delegate* unmanaged[Cdecl]<float*, int, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_bones_capture");
            _fn_bone_wiggle  = (delegate* unmanaged[Cdecl]<int, int, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_bone_wiggle");
            _fn_bone_status  = (delegate* unmanaged[Cdecl]<int*, int*, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_bone_status");
            _fn_bones_seq    = (delegate* unmanaged[Cdecl]<int, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_bones_seq");
            _fn_bones_seq_left = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_bones_seq_left");
            _fn_hands_set    = (delegate* unmanaged[Cdecl]<int, int, float, float, float, float, float, float, int, int, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_hands_set");
            _fn_hands_status = (delegate* unmanaged[Cdecl]<uint*, nint>)NativeLibrary.GetExport(_native_handle, "ffxvr_hands_status");
            _fn_hands_calibrate = (delegate* unmanaged[Cdecl]<void>)NativeLibrary.GetExport(_native_handle, "ffxvr_hands_calibrate");
            _fn_hands_dump   = (delegate* unmanaged[Cdecl]<void>)NativeLibrary.GetExport(_native_handle, "ffxvr_hands_dump");
            _fn_hands_method = (delegate* unmanaged[Cdecl]<int, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_hands_method");
            _fn_hands_side   = (delegate* unmanaged[Cdecl]<int, float, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_hands_side");
            _fn_hands_cal_data  = (delegate* unmanaged[Cdecl]<float*, int, uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_hands_cal_data");
            _fn_gesture_poll = (delegate* unmanaged[Cdecl]<float*, float*, int*, uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_gesture_poll");
            _fn_fp_body_set  = (delegate* unmanaged[Cdecl]<int, uint, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_fp_body_set");
            _fn_fp_parts     = (delegate* unmanaged[Cdecl]<uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_fp_parts");
            _fn_msaa_status  = (delegate* unmanaged[Cdecl]<uint*, uint*, uint*, uint*, uint*, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_msaa_status");
            _fn_patch_set    = (delegate* unmanaged[Cdecl]<int, int, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_patch_set");
            _fn_patch_count  = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_patch_count");
            _fn_patch_name   = (delegate* unmanaged[Cdecl]<int, byte*>)NativeLibrary.GetExport(_native_handle, "ffxvr_patch_name");
            _fn_backdrop_draws = (delegate* unmanaged[Cdecl]<uint>)NativeLibrary.GetExport(_native_handle, "ffxvr_backdrop_draws");
            _fn_measure      = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_measure");
            _fn_on_ps_srvs   = (delegate* unmanaged[Cdecl]<uint, uint, nint*, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_on_ps_srvs");
            _fn_on_clear_twin = (delegate* unmanaged[Cdecl]<nint, int, float*, uint, float, byte, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_on_clear_twin");
            _fn_set_backbuffer = (delegate* unmanaged[Cdecl]<nint, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_set_backbuffer");
            _fn_on_copy      = (delegate* unmanaged[Cdecl]<nint, uint, uint, uint, uint, nint, uint, nint, int, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_on_copy");
            _fn_override_set = (delegate* unmanaged[Cdecl]<int, int, float, float, float, float, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_override_set");
            _fn_override_applied = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_override_applied");
            _fn_override_mask    = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_override_mask");
            _fn_safe_read        = (delegate* unmanaged[Cdecl]<void*, void*, uint, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_safe_read");
            _fn_project          = (delegate* unmanaged[Cdecl]<float, float, float, float*, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_project");
            _fn_world_positions  = (delegate* unmanaged[Cdecl]<float*, int, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_world_positions");

            _native_status = "loaded, waiting for the render backend";
            _logger.Info("Native helper loaded. Waiting for ImGui's D3D11 backend to come up.");
        }
        catch (Exception ex) {
            _native_status = "load threw: " + ex.GetType().Name;
            _logger.Error($"Failed to load the native helper: {ex}");
        }
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
        if (_native_init_done || _fn_init == null) return;

        nint backend = (nint)ImGui.GetIO().BackendRendererUserData;
        if (backend == 0) return;   // backend not up yet; try again next frame

        _native_init_done = true;

        nint* fields = (nint*)backend;
        _game_device  = fields[0];
        _game_context = fields[1];

        if (_game_context == 0) {
            _native_status = "ImGui backend gave no device context";
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
        else         _logger.Error($"Native init failed with code {rc} for context 0x{_game_context:X8}.");

        if (rc == 0) install_draw_detours();
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
        // partial hide: a range of indices (a batched 2D sprite on the hide list) is left out, the rest drawn in two pieces
        uint skip_first = 0, skip_count = 0; bool partial = (r & 0x20000) != 0 && _fn_draw_skip != null;
        if (partial) { _fn_draw_skip(&skip_first, &skip_count); if (skip_first + skip_count > n) partial = false; }
        if (noblend) _fn_blend_off();
        draw_pieces(ctx, n, start, base_vertex, partial, skip_first, skip_count);
        for (int k = 0; k < eye2; k++) if (_fn_eye2_begin(k) != 0) { draw_pieces(ctx, n, start, base_vertex, partial, skip_first, skip_count); _fn_eye2_end(); }
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
    private void draw_pieces(nint ctx, uint n, uint start, int base_vertex, bool partial, uint skip_first, uint skip_count) {
        if (!partial) { _o_drawindexed!(ctx, n, start, base_vertex); return; }
        if (skip_first > 0) _o_drawindexed!(ctx, skip_first, start, base_vertex);
        uint after = skip_first + skip_count; if (after < n) _o_drawindexed!(ctx, n - after, start + after, base_vertex);
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
        for (int k = 0; k < eye2; k++) if (_fn_eye2_begin(k) != 0) { _o_draw!(ctx, n, start); _fn_eye2_end(); }
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
        try { if (_fn_shutdown != null) _fn_shutdown(); } catch { }
        _o_exitprocess ??= new FhCallHandle<d_ExitProcess>(new FhCallLocation((void*)_a_exitprocess)).chain_from(_h_exitprocess!).fnptr;
        _o_exitprocess!(code);
    }

    private void h_resolve(nint ctx, nint dst, uint dst_sub, nint src, uint src_sub, uint fmt) {
        _o_resolve ??= new FhCallHandle<d_ResolveSubresource>(new FhCallLocation((void*)_a_resolve)).chain_from(_h_resolve!).fnptr;
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

    private bool _settings_ok = false;   // a load succeeded (or there was no file): saving is allowed
    private void settings_load() {
        try {
            string dir = Path.GetDirectoryName(typeof(FFXVRModule).Assembly.Location) ?? ".";
            _ini_path = Path.Combine(dir, "ffxvr.ini");
            bone_labels_load();
            if (!File.Exists(_ini_path)) { _settings_ok = true; _logger.Info($"settings: no {_ini_path} yet; defaults"); return; }
            // The file can be briefly locked (an editor, a sync tool, the previous game process on its way out):
            // retry rather than fall through to defaults, and never overwrite it with defaults on a failed read.
            string[] lines_in = Array.Empty<string>(); Exception? last = null;
            for (int attempt = 0; attempt < 10; attempt++) {
                try { lines_in = File.ReadAllLines(_ini_path); last = null; break; }
                catch (IOException e) { last = e; System.Threading.Thread.Sleep(100); }
            }
            if (last != null) { _settings_ok = false; _logger.Info($"settings: could not read {_ini_path} ({last.Message}); keeping defaults in memory and NOT saving this session"); return; }
            foreach (string raw in lines_in) {
                string line = raw.Trim();
                int eq = line.IndexOf('=');
                if (line.Length == 0 || line[0] == '#' || eq < 0) continue;
                string k = line[..eq].Trim(), v = line[(eq + 1)..].Trim();
                var ci = System.Globalization.CultureInfo.InvariantCulture;
                switch (k) {
                    case "headset_autostart": _headset_autostart = v == "1"; break;
                    case "start_view":        int.TryParse(v, out _start_view); break;
                    case "overlay_at_start":  _overlay_at_start = v != "0"; break;
                    case "world_scale":       float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _xr_scale); break;
                    case "head_position":     _xr_translate = v == "1"; break;
                    case "seat":              _xr_seat = v == "1"; break;
                    case "headset_fov":       _xr_fov = v == "1"; break;
                    case "screen_mode":       int.TryParse(v, out _xr_screen_mode); break;
                    case "stereo_screen_blitz": _s3_blitz = v != "0"; break;
                    case "stereo_screen_all": _s3_all = v != "0"; break;
                    case "stereo_screen_depth": float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _s3_depth); break;
                    case "stereo_screen_plane": float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _s3_plane); break;
                    case "stereo_screen_auto": _s3_auto = v != "0"; break;
                    case "stereo_screen_max_pop": float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _s3_max_pop); break;
                    case "backdrop_mode":     int.TryParse(v, out _backdrop_mode); break;
                    case "cull_dist":         _cull_dist = v != "0"; break;
                    case "cull_scene":        _cull_scene = v != "0"; break;
                    case "msaa":              int.TryParse(v, out _msaa); break;
                    case "vrpad":             _vrpad = v != "0"; break;
                    case "imgui_layer":       _imgui_layer = v != "0"; break;
                    case "gestures":          _gestures = v != "0"; break;
                    case "gest_tilt":         float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _gest_tilt); break;
                    case "fp_body_mode":      int.TryParse(v, out _fp_body_mode); break;
                    case "battle_anchor":     _battle_anchor = v != "0"; break;
                    case "ui_hide":           int.TryParse(v, out _ui_hide); break;
                    case "hide_cursor":       _hide_cursor = v != "0"; break;
                    case "hide_pointers":     _hide_pointers = v != "0"; break;
                    case "blitz_overlay":     _blitz_overlay = v != "0"; break;
                    case "markers_game_art":  _markers_game_art = v != "0"; break;
                    case "marker_scale":      float.TryParse(v, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out _marker_scale); break;
                    case "hide_list":         hide_list_parse(v); break;
                    case "battle_follow":     _battle_follow = v != "0"; break;
                    case "battle_view":       int.TryParse(v, out _battle_view); break;
                    case "diorama_floor":     float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _diorama_floor); break;
                    case "fp_head_anchor":    if (!int.TryParse(v, out _fp_head_anchor)) _fp_head_anchor = v != "0" ? 2 : 0; break;
                    case "fp_head_up":        float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _fp_head_up); break;
                    case "fp_head_smooth":    float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _fp_head_smooth); break;
                    case "fp_head_fwd":       float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _fp_head_fwd); break;
                    case "fp_head_parts":     _fp_head_parts = v; break;
                    case "screen_width":      float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _xr_screen_width); break;
                    case "screen_distance":   float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _xr_screen_dist); break;
                    case "overlay_open":      _window_open = v != "0"; break;
                    case "eye2_preview":      _eye2_preview = v == "1"; break;
                    case "ui_panel":          _ui_on = v != "0"; break;
                    case "ui_distance":       float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _ui_dist); break;
                    case "ui_width":          float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _ui_width); break;
                    case "pitch_follow":      float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _xr_pitch_follow); break;
                    case "seat_height":       float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _ovr_up); break;
                    case "seat_forward":      float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _ovr_forward); break;
                    case "cut_recentre":      _cut_recentre = v != "0"; break;
                    case "cut_blink":         _cut_blink = v == "1"; break;
                    case "turn_stick":        _turn_stick = v != "0"; break;
                    case "turn_mode":         int.TryParse(v, out _turn_mode); break;
                    case "turn_snap":         float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _turn_snap); break;
                    case "turn_rate":         float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _turn_rate); break;
                    case "turn_blink":        _turn_blink = v != "0"; break;
                    case "move_remap":        _move_remap = v != "0"; break;
                    case "move_invert":       _move_invert = v == "1"; break;
                    case "fp_actor":          _fp_actor = v.Trim(); break;
                    case "noimg_mode":        int.TryParse(v, out _noimg_mode); break;
                    case "hist_mode":         int.TryParse(v, out _hist_mode); break;
                    case "follow_pans":       _follow_pans = v == "1"; break;
                    case "follow_rate":       float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _follow_rate); break;
                    case "fp_eye_height":     float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _fp_eye_h); break;
                    case "follow_distance":   float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _fp_follow_dist); break;
                    case "follow_height":     float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _fp_follow_h); break;
                    case "tp_distance":       float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _tp_dist); break;
                    case "tp_height":         float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _tp_h); break;
                    case "tp_pitch":          float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _tp_pitch); break;
                    case "tp_follow_rate":    float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _tp_follow_rate); break;
                    case "hands_dev":         _hands_dev = v == "1"; break;
                    case "hands":             _hands_on = v == "1"; break;
                    case "hands_head":        _hands_head = v != "0"; break;
                    case "hands_show":        int.TryParse(v, out _hands_show); break;
                    case "hands_yaw":         float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _hands_yaw); break;
                    case "hands_pitch":       float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _hands_pitch); break;
                    case "hands_roll":        float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _hands_roll); break;
                    case "hands_off_right":   float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _hands_off_r); break;
                    case "hands_off_up":      float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _hands_off_u); break;
                    case "hands_off_back":    float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _hands_off_b); break;
                    case "hands_swap":        _hands_swap = v == "1"; break;
                    case "hands_mirror":      _hands_mirror = v == "1"; break;
                    case "hands_cal":         _hands_cal_ini = v; break;
                    case "hands_px_left":     int.TryParse(v, out _hands_px_left); break;
                    case "hands_stretch":     float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _hands_stretch); break;
                }
            }
            if (_headset_autostart) {
                _xr_want = true;
                if (_start_view >= 0 && _start_view <= 3 && _start_view != _fp_mode) { apply_mode_scale(_fp_mode, _start_view); _fp_mode = _start_view; }
                _window_open = _overlay_at_start;   // straight into the headset: no overlay until F8
            }
            _logger.Info($"settings: loaded {_ini_path}: headset_autostart={_headset_autostart} start_view={_start_view} overlay_at_start={_overlay_at_start} overlay_open={_window_open} eye2_preview={_eye2_preview}");
            _overlay_at_start_loaded = _overlay_at_start; _settings_ok = true;
        }
        catch (Exception e) { _settings_ok = false; _logger.Info($"settings: load failed ({e.GetType().Name}: {e.Message}); not saving this session"); }
    }

    /// <summary>An edit of the ini while the game runs (by hand, or by a tool) is picked up instead of being
    /// overwritten: every second the file is compared with what was last written, and reloaded if it differs.
    /// Keys that only apply at launch (start_view, overlay_at_start) just update their stored values.</summary>
    private DateTime _ini_mtime_seen = DateTime.MinValue; private int _ini_check = 0;
    private void settings_reload_if_edited() {
        if (_ini_path.Length == 0 || ++_ini_check < 60) return;
        _ini_check = 0;
        try {
            DateTime mt = File.GetLastWriteTimeUtc(_ini_path);
            if (mt == _ini_mtime_seen) return;
            _ini_mtime_seen = mt;
            string on_disk = File.ReadAllText(_ini_path);
            if (on_disk == _ini_last) return;
            bool autostart = _headset_autostart; bool xr_want = _xr_want; bool win = _window_open; int mode = _fp_mode; bool ok_before = _settings_ok;
            settings_load(); _settings_ok = _settings_ok || ok_before;
            _headset_autostart = autostart || _headset_autostart; _xr_want = xr_want; _window_open = win; _fp_mode = mode;   // launch-only effects are not re-applied mid-session
            _ini_last = on_disk;
            _logger.Info("settings: ffxvr.ini was edited on disk; reloaded");
        }
        catch { }
    }
    private void settings_save_if_changed() {
        if (_ini_path.Length == 0 || !_settings_ok) return;
        var ci = System.Globalization.CultureInfo.InvariantCulture;
        string text =
            "# FFX VR settings. Edited by the overlay; hand edits are fine.\n" +
            $"headset_autostart={(_headset_autostart ? 1 : 0)}\n" +
            $"start_view={_start_view}\n" +
            $"overlay_at_start={(_overlay_at_start ? 1 : 0)}\n" +
            $"world_scale={(life_size(_fp_mode) ? _scale_before_fp : _xr_scale).ToString("F1", ci)}\n" +   // the seat / diorama scale; first and third person are always true scale
            $"head_position={(_xr_translate ? 1 : 0)}\n" +
            $"seat={(_xr_seat ? 1 : 0)}\n" +
            $"headset_fov={(_xr_fov ? 1 : 0)}\n" +
            $"screen_mode={_xr_screen_mode}\n" +
            $"stereo_screen_blitz={(_s3_blitz ? 1 : 0)}\n" +
            $"stereo_screen_all={(_s3_all ? 1 : 0)}\n" +
            $"stereo_screen_depth={_s3_depth.ToString("F2", ci)}\n" +
            $"stereo_screen_plane={_s3_plane.ToString("F0", ci)}\n" +
            $"stereo_screen_auto={(_s3_auto ? 1 : 0)}\n" +
            $"stereo_screen_max_pop={_s3_max_pop.ToString("F1", ci)}\n" +
            $"backdrop_mode={_backdrop_mode}\n" +
            $"cull_dist={(_cull_dist ? 1 : 0)}\n" +
            $"cull_scene={(_cull_scene ? 1 : 0)}\n" +
            $"msaa={_msaa}\n" +
            $"vrpad={(_vrpad ? 1 : 0)}\n" +
            $"imgui_layer={(_imgui_layer ? 1 : 0)}\n" +
            $"gestures={(_gestures ? 1 : 0)}\n" +
            $"gest_tilt={_gest_tilt.ToString("F2", ci)}\n" +
            $"fp_body_mode={_fp_body_mode}\n" +
            $"fp_head_anchor={_fp_head_anchor}\n" +
            $"battle_anchor={(_battle_anchor ? 1 : 0)}\n" +
            $"ui_hide={_ui_hide}\n" +
            $"hide_cursor={(_hide_cursor ? 1 : 0)}\n" +
            $"hide_pointers={(_hide_pointers ? 1 : 0)}\n" +
            $"blitz_overlay={(_blitz_overlay ? 1 : 0)}\n" +
            $"markers_game_art={(_markers_game_art ? 1 : 0)}\n" +
            $"marker_scale={_marker_scale.ToString("0.##", System.Globalization.CultureInfo.InvariantCulture)}\n" +
            $"hide_list={hide_list_str()}\n" +
            $"battle_follow={(_battle_follow ? 1 : 0)}\n" +
            $"battle_view={_battle_view}\n" +
            $"diorama_floor={_diorama_floor.ToString("F1", ci)}\n" +
            $"fp_head_up={_fp_head_up.ToString("F2", ci)}\n" +
            $"fp_head_smooth={_fp_head_smooth.ToString("F2", ci)}\n" +
            $"fp_head_fwd={_fp_head_fwd.ToString("F2", ci)}\n" +
            $"fp_head_parts={_fp_head_parts}\n" +
            $"screen_width={_xr_screen_width.ToString("F2", ci)}\n" +
            $"screen_distance={_xr_screen_dist.ToString("F2", ci)}\n" +
            $"overlay_open={(_window_open ? 1 : 0)}\n" +
            $"eye2_preview={(_eye2_preview ? 1 : 0)}\n" +
            $"ui_panel={(_ui_on ? 1 : 0)}\n" +
            $"ui_distance={_ui_dist.ToString("F2", ci)}\n" +
            $"ui_width={_ui_width.ToString("F2", ci)}\n" +
            $"pitch_follow={_xr_pitch_follow.ToString("F2", ci)}\n" +
            $"seat_height={_ovr_up.ToString("F0", ci)}\n" +
            $"seat_forward={_ovr_forward.ToString("F0", ci)}\n" +
            $"cut_recentre={(_cut_recentre ? 1 : 0)}\n" +
            $"cut_blink={(_cut_blink ? 1 : 0)}\n" +
            $"turn_stick={(_turn_stick ? 1 : 0)}\n" +
            $"turn_mode={_turn_mode}\n" +
            $"turn_snap={_turn_snap.ToString("F0", ci)}\n" +
            $"turn_rate={_turn_rate.ToString("F0", ci)}\n" +
            $"turn_blink={(_turn_blink ? 1 : 0)}\n" +
            $"move_remap={(_move_remap ? 1 : 0)}\n" +
            $"move_invert={(_move_invert ? 1 : 0)}\n" +
            $"fp_actor={_fp_actor}\n" +
            $"noimg_mode={_noimg_mode}\n" +
            $"hist_mode={_hist_mode}\n" +
            $"follow_pans={(_follow_pans ? 1 : 0)}\n" +
            $"follow_rate={_follow_rate.ToString("F0", ci)}\n" +
            $"fp_eye_height={_fp_eye_h.ToString("F0", ci)}\n" +
            $"follow_distance={_fp_follow_dist.ToString("F0", ci)}\n" +
            $"follow_height={_fp_follow_h.ToString("F0", ci)}\n" +
            $"tp_distance={_tp_dist.ToString("F0", ci)}\n" +
            $"tp_height={_tp_h.ToString("F0", ci)}\n" +
            $"tp_pitch={_tp_pitch.ToString("F0", ci)}\n" +
            $"tp_follow_rate={_tp_follow_rate.ToString("F0", ci)}\n" +
            $"hands_dev={(_hands_dev ? 1 : 0)}\n" +
            $"hands={(_hands_on ? 1 : 0)}\n" +
            $"hands_head={(_hands_head ? 1 : 0)}\n" +
            $"hands_show={_hands_show}\n" +
            $"hands_yaw={_hands_yaw.ToString("F0", ci)}\n" +
            $"hands_pitch={_hands_pitch.ToString("F0", ci)}\n" +
            $"hands_roll={_hands_roll.ToString("F0", ci)}\n" +
            $"hands_off_right={_hands_off_r.ToString("F3", ci)}\n" +
            $"hands_off_up={_hands_off_u.ToString("F3", ci)}\n" +
            $"hands_off_back={_hands_off_b.ToString("F3", ci)}\n" +
            $"hands_swap={(_hands_swap ? 1 : 0)}\n" +
            $"hands_mirror={(_hands_mirror ? 1 : 0)}\n" +
            $"hands_cal={_hands_cal_ini}\n" +
            $"hands_px_left={_hands_px_left}\n" +
            $"hands_stretch={_hands_stretch.ToString("F2", ci)}\n";
        if (text == _ini_last) return;
        if (_overlay_at_start != _overlay_at_start_loaded) { _logger.Info($"settings: overlay_at_start changed to {_overlay_at_start} (window open {_window_open}, frame {_frames})"); _overlay_at_start_loaded = _overlay_at_start; }
        try {
            if (_ini_last.Length == 0 && File.Exists(_ini_path)) File.Copy(_ini_path, _ini_path + ".bak", true);   // the first write of a session keeps the previous file
            File.WriteAllText(_ini_path, text); _ini_last = text; _ini_mtime_seen = File.GetLastWriteTimeUtc(_ini_path);
        }
        catch (Exception e) { _logger.Warning($"settings: {e.Message}"); }
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
        if (_fn_xr_set == null) { ImGui.Text($"Native helper: {_native_status}"); return; }

        // Headset
        bool want = _xr_want;
        ImGui.Checkbox("Headset", ref want);
        ImGui.SameLine(); if (ImGui.Button("Recentre (Home)")) _fn_xr_recenter();
        ImGui.SameLine(); ImGui.Checkbox("start automatically", ref _headset_autostart);
        if (_headset_autostart) {
            ImGui.SameLine(); ImGui.Checkbox("overlay open at start", ref _overlay_at_start); ImGui.SameLine(); ImGui.Text("start in:"); ImGui.SameLine();
            ImGui.RadioButton("seat##sv", ref _start_view, 0); ImGui.SameLine(); ImGui.RadioButton("first person##sv", ref _start_view, 1); ImGui.SameLine();
            ImGui.RadioButton("third person##sv", ref _start_view, 3); ImGui.SameLine(); ImGui.RadioButton("diorama##sv", ref _start_view, 2);
        }
        if (want) { _ovr_enabled = true; _stereo_on = true; }   // the headset needs the override and both eyes
        _xr_on = _fn_xr_set(want ? 1 : 0, _xr_scale, _xr_rotate ? 1 : 0, _xr_translate ? 1 : 0, _xr_fov ? 1 : 0, _xr_seat ? 1 : 0) != 0;
        _xr_want = want;   // asked each frame until the native side accepts (it waits for the first 3D frame)
        ImGui.TextWrapped(xr_status_text());
        if (_fn_xr_screen_now != null && _xr_on) { int sn = _fn_xr_screen_now(); ImGui.Text(sn == 1 ? "showing: virtual screen" : sn == 2 ? "showing: stereo screen" : "showing: 3D scene"); }

        // Controllers
        ImGui.Separator();
        ImGui.Checkbox("Quest controllers as the gamepad", ref _vrpad); ImGui.SameLine();
        ImGui.TextDisabled("A/B/X/Y, sticks, triggers = LT/RT, grips = LB/RB (first person), left menu = Start, left stick click = Select");
        if (_fn_vrpad_status != null) {
            float* st = stackalloc float[24]; byte* pl = stackalloc byte[64]; byte* pr = stackalloc byte[64]; uint frames;
            int ready = _fn_vrpad_status(st, pl, pr, 64, &frames);
            if (ready == 0) ImGui.TextDisabled(_xr_on ? "controllers: no input this frame (headset not focused, or no bindings)" : "controllers: headset off");
            else {
                ImGui.Text($"controllers: syncing ({frames} frames)   left: {System.Runtime.InteropServices.Marshal.PtrToStringAnsi((nint)pl)}   right: {System.Runtime.InteropServices.Marshal.PtrToStringAnsi((nint)pr)}");
                for (int h = 0; h < 2; h++) {
                    float* o = st + h * 12;
                    ImGui.Text($"  {(h == 0 ? "L" : "R")}: {(o[0] > 0 ? "on " : "off")} stick ({o[1],5:F2},{o[2],5:F2}){(o[3] > 0 ? "*" : " ")}  {(h == 0 ? "X" : "A")}{(o[4] > 0 ? "#" : "-")} {(h == 0 ? "Y" : "B")}{(o[5] > 0 ? "#" : "-")}  trig {o[6]:F2} grip {o[7]:F2}  {(h == 0 ? (o[8] > 0 ? "menu#" : "menu-") : "     ")}  pose {(o[9] > 0 ? "grip" : "----")}/{(o[10] > 0 ? "aim" : "---")}");
                }
                if (_fn_haptic != null) { if (ImGui.SmallButton("buzz left")) _fn_haptic(0, 0.5f, 0.1f); ImGui.SameLine(); if (ImGui.SmallButton("buzz right")) _fn_haptic(1, 0.5f, 0.1f); }
            }
        }
        ImGui.Checkbox("grips grab the world in the seat view (one hand pans; two hands pan, turn, tilt and zoom)", ref _gestures);
        ImGui.SameLine(); ImGui.SetNextItemWidth(120); ImGui.SliderFloat("tilt (wrist roll) gain", ref _gest_tilt, -2f, 2f, "%.1f");
        ImGui.TextDisabled($"right grip + right stick click: first person on/off.   {(_gest_active != 0 ? "GRABBING   " : "")}world scale now {_xr_scale * _gest_scale_mul:F0} units/m (base {_xr_scale:F0} x {_gest_scale_mul:F2}), grab offset ({_gest_move_x:F0}, {_gest_move_y:F0}, {_gest_move_z:F0})");
        ImGui.SameLine(); if (ImGui.SmallButton("reset grab")) { if (_fn_gesture_reset != null) _fn_gesture_reset(); }

        // Comfort
        ImGui.Separator();
        ImGui.SliderFloat("world scale", ref _xr_scale, 10f, 300f, "%.0f units/m");
        ImGui.SameLine(); ImGui.TextDisabled(_xr_scale < 20f ? "life-size" : _xr_scale < 60f ? "large model" : "tabletop diorama");
        ImGui.Checkbox("head position (lean)", ref _xr_translate); ImGui.SameLine();
        ImGui.Checkbox("seat (head owns orientation)", ref _xr_seat);
        ImGui.SliderFloat("pitch follow", ref _xr_pitch_follow, 0f, 1f, "%.2f");
        ImGui.SameLine(); ImGui.TextDisabled("0 level horizon .. 1 the original view");
        if (_fn_xr_pitch_follow != null) _fn_xr_pitch_follow(_xr_pitch_follow);
        ImGui.Text("camera cuts:"); ImGui.SameLine();
        ImGui.Checkbox("recentre on cut", ref _cut_recentre); ImGui.SameLine();
        ImGui.Checkbox("blink", ref _cut_blink); ImGui.SameLine();
        ImGui.Checkbox("follow pans", ref _follow_pans);
        if (_follow_pans) ImGui.SliderFloat("pan follow rate (deg/s)", ref _follow_rate, 5f, 90f);
        if (_fn_cuts_set != null) _fn_cuts_set(_cut_recentre ? 1 : 0, _cut_blink ? 1 : 0, _follow_pans ? 1 : 0, _follow_rate, ImGui.GetIO().DeltaTime);
        if (_fn_cut_count != null) { ImGui.SameLine(); ImGui.TextDisabled($"cuts seen: {_fn_cut_count()}"); }
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
        if (ImGui.IsKeyPressed(ImGuiKey.End)) _fp_mode = next_view_mode(_fp_mode);
        if (_chord_toggle_pending) { _chord_toggle_pending = false; _fp_mode = next_view_mode(_fp_mode); }

        // Turning (first person and seat alike)
        ImGui.Text("turning:"); ImGui.SameLine();
        ImGui.Checkbox("right stick", ref _turn_stick); ImGui.SameLine();
        ImGui.SliderFloat("turn speed", ref _turn_rate, 30f, 240f, "%.0f deg/s");
        ImGui.SameLine(); ImGui.TextDisabled("Numpad 1/3: turn left/right");
        ImGui.Checkbox("left stick follows where I look (first person)", ref _move_remap); ImGui.SameLine();
        ImGui.Checkbox("invert", ref _move_invert);
        if (_fn_move_status != null) {
            float yv, yc, dd; uint calls; int hooked;
            _fn_move_status(&yv, &yc, &dd, &calls, &hooked);
            ImGui.SameLine(); ImGui.TextDisabled(hooked != 0 ? $"pad reads: {calls}  view {yv:F0}\u00b0 camera {yc:F0}\u00b0 stick turned {dd:F0}\u00b0" : "pad hook not installed");
        }
        if (_fp_mode != fp_before) apply_mode_scale(fp_before, _fp_mode);
        push_player();
        if (_fn_tp_set != null) _fn_tp_set(_tp_dist, _tp_h, _tp_pitch * 0.0174533f, _tp_follow_rate);
        push_hands();
        if (_fn_fp_set != null) _fn_fp_set(_fp_mode, _fp_eye_h, _fp_follow_dist, _fp_follow_h, _fp_hide_body ? 1 : 0, _fp_hide_radius);

        // View
        ImGui.Separator();
        ImGui.Checkbox("headset field of view", ref _xr_fov);
        ImGui.Text("2D screens:"); ImGui.SameLine();
        ImGui.RadioButton("auto", ref _xr_screen_mode, 0); ImGui.SameLine();
        ImGui.RadioButton("always", ref _xr_screen_mode, 1); ImGui.SameLine();
        ImGui.RadioButton("never", ref _xr_screen_mode, 2);
        if (_fn_xr_screen_set != null) _fn_xr_screen_set(_xr_screen_mode, _xr_screen_width, _xr_screen_dist);
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
            ImGui.SliderFloat("screen width (m)##s3", ref _xr_screen_width, 1f, 8f, "%.1f"); ImGui.SameLine(); ImGui.TextDisabled("distance: Debug > Virtual screen panel; Home recentres the screen in front of you");
        }
        ImGui.Text("painted-backdrop rooms (hut interiors):"); ImGui.SameLine();
        ImGui.RadioButton("whole room on the screen", ref _backdrop_mode, 1); ImGui.SameLine();
        ImGui.RadioButton("3D props over the flat backdrop", ref _backdrop_mode, 0);
        if (_fn_backdrop_draws != null) { ImGui.SameLine(); ImGui.TextDisabled($"backdrop draws seen: {_fn_backdrop_draws()}"); }
        if (_fn_backdrop_set != null) _fn_backdrop_set(_backdrop_mode);
        ImGui.Checkbox("UI on its own panel (menus, subtitles, HUD)", ref _ui_on); ImGui.SameLine();
        ImGui.Checkbox("Fahrenheit menus (save screen) and this overlay in the headset", ref _imgui_layer);
        if (_fn_imgui_draws != null) { ImGui.SameLine(); ImGui.TextDisabled($"layer draws {_fn_imgui_draws()} (shown when this overlay or the save screen is open, or at {_imgui_min_draws}+ draws)"); }
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
        if (_fn_hist_set != null) _fn_hist_set(_hist_mode);
        if (_fn_ui_set != null) _fn_ui_set(_ui_on ? 1 : 0, _ui_dist, _ui_width);

        ImGui.Separator();
        if (_fn_perf_status != null) {
            byte* pb = stackalloc byte[256]; _fn_perf_status(pb, 256);
            ImGui.TextDisabled(System.Runtime.InteropServices.Marshal.PtrToStringAnsi((nint)pb) ?? "");
        }
        ImGui.TextDisabled("Home: recentre   F8: hide/show this overlay (F7 is Fahrenheit's own mod-settings menu)");
    }

    /// <summary>Camera numbers, override controls, second eye, captures. Collapsed by default.</summary>
    private void draw_debug() {
        if (!ImGui.CollapsingHeader("Debug")) {
            // The override and eye-2 settings must still reach the native side.
            push_override_and_stereo();
            return;
        }

        ImGui.Text($"{ImGui.GetIO().Framerate:F1} fps   native: {_native_status}   detours: {_detour_status}");
        if (_fn_counter != null)
            ImGui.Text($"draws: indexed {_fn_counter(1)}, plain {_fn_counter(2)}   UpdateSubresource {_fn_counter(7)}   buffers {(_fn_buffer_count != null ? _fn_buffer_count() : -1)}");

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
            if (_fn_eye2_draws != null) ImGui.Text($"eye-2 draws last frame: {_fn_eye2_draws()}");
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("UI panel")) {
            ImGui.SliderFloat("distance (m)", ref _ui_dist, 0.5f, 4f);
            ImGui.SliderFloat("width (m)", ref _ui_width, 0.5f, 4f);
            if (_fn_ui_draws != null) ImGui.Text($"UI draws last frame: {_fn_ui_draws()}");
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Virtual screen panel")) {
            ImGui.SliderFloat("width (m)", ref _xr_screen_width, 1f, 6f);
            ImGui.SliderFloat("distance (m)", ref _xr_screen_dist, 1f, 6f);
            ImGui.TextDisabled("applied at the next recentre");
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
        if (_fn_noimg_set != null) _fn_noimg_set(_noimg_mode);

        if (ImGui.TreeNode("Culling (code patches)")) {
            ImGui.TextWrapped("Patches to the game's visibility tests, verified against the expected bytes before writing; untick restores. The two checkboxes above (characters, scene) are the normal way to drive these.");
            if (_fn_patch_set != null && _fn_patch_count != null) {
                int np = Math.Min(_fn_patch_count(), _patch_on.Length);
                for (int i = 0; i < np; i++) {
                    string name = System.Runtime.InteropServices.Marshal.PtrToStringAnsi((nint)_fn_patch_name(i)) ?? $"patch {i}";
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
        if (_fn_bisect_set != null) _fn_bisect_set(_bisect_mode, _bisect_a, _bisect_b);

        if (ImGui.TreeNode("First person / follow")) {
            ImGui.Checkbox("in battle the diorama centres on the fight, not the player", ref _battle_anchor); ImGui.SameLine(); ImGui.TextDisabled(_battle_anchor_n > 0 ? $"combatants {_battle_anchor_n}" : "not in battle");
            ImGui.Checkbox("in battle, first person follows the field: Tidus when he is on it, swaps move the eyes, a summon gives them to Yuna", ref _battle_follow); ImGui.SameLine(); ImGui.TextDisabled(_btl_who.Length > 0 ? $"now: {_btl_who}" : "");
            ImGui.TextDisabled("Tidus joining the field takes the eyes; left grip + right stick click (or Insert) moves them to the next party member");
            ImGui.Checkbox("hide the game's battle pointers by position, headset only: any small plain or 1024-atlas quad in the column above where a character projects through the game's camera", ref _hide_pointers); ImGui.SameLine();
            ImGui.TextDisabled($"hidden last frame: {_pointer_stats & 0xFFFF}, characters projected: {_pointer_stats >> 16}");
            ImGui.Checkbox("hide the game's battle cursors by sprite signature, headset only (the list is filled by picking, below)", ref _hide_cursor); ImGui.SameLine();
            ImGui.TextDisabled($"candidates {_rect_stats >> 16}, rect unknown {_rect_stats & 0xFFFF}");
            for (int i = 0; i < _hide_list.Count; i++) {
                float[] e = _hide_list[i];
                ImGui.Text($"  {(e[0] == 0 ? "textureless" : $"{e[0]:F0}x{e[1]:F0} atlas")} quad {e[2]:F0}-{e[3]:F0} x {e[4]:F0}-{e[5]:F0} units   hidden last frame: {(i < 16 ? _hide_hits[i] : 0)}"); ImGui.SameLine();
                if (ImGui.SmallButton($"remove##hl{i}")) { _hide_list.RemoveAt(i); _hide_list_dirty = true; break; }
            }
            if (ImGui.SmallButton("reset the list to the two known cursors")) { _hide_list.Clear(); _hide_list.AddRange(hide_list_defaults()); _hide_list_dirty = true; }
            ImGui.Checkbox("blitzball: put the scoreboard and radar on the UI panel (they are drawn into the scene with no camera: double vision)", ref _blitz_overlay); ImGui.SameLine();
            ImGui.TextDisabled(map_base(_map_last).StartsWith("bltz") ? $"routed last frame: {_blitz_overlay_n}" : "not in blitzball");
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
            ImGui.Checkbox("hide the player's body", ref _fp_hide_body); ImGui.SameLine();
            ImGui.SliderFloat("hide radius", ref _fp_hide_radius, 0.1f, 12f); ImGui.SameLine(); ImGui.TextDisabled("the player's parts sit exactly on the actor position; an NPC you talk to is 1+ units away, so keep this small");
            ImGui.Text("in first person:"); ImGui.SameLine();
            ImGui.RadioButton("hide my whole body", ref _fp_body_mode, 0); ImGui.SameLine();
            ImGui.RadioButton("show my body, hide the head parts", ref _fp_body_mode, 1); ImGui.SameLine();
            ImGui.RadioButton("show everything", ref _fp_body_mode, 2);
            ImGui.SetNextItemWidth(160); ImGui.InputText("head parts (indices, e.g. 2,3)", ref _fp_head_parts, 64); ImGui.SameLine();
            ImGui.TextDisabled($"player parts drawn into the scene last frame: {(_fn_fp_parts != null ? _fn_fp_parts() : 0)}");
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
                if (_fn_hands_status != null) { uint hf; nint why = _fn_hands_status(&hf); ImGui.SameLine(); ImGui.TextDisabled($"{System.Runtime.InteropServices.Marshal.PtrToStringAnsi(why)}  frames {hf}"); }
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
                ImGui.Text($"record {_wig_rec} slot {_wig_slot}: {cur}"); ImGui.SameLine();
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
            ImGui.InputText("player model name", ref _fp_actor, 16); ImGui.SameLine();
            ImGui.TextDisabled(_fp_actor_idx >= 0 ? $"actor {_fp_actor_idx}" : "not found (using actor 0)");
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
                    ImGui.TextWrapped(text);
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

        if (ImGui.TreeNode("Battle markers")) {
            ImGui.Checkbox("draw target / turn markers", ref _markers_on); ImGui.SameLine();
            ImGui.Checkbox("as 3D geometry in the headset", ref _markers_3d);
            ImGui.Checkbox("experimental: the game's own cursor art at the markers (did not work: the atlas coordinates' source is unknown)", ref _markers_game_art); ImGui.SameLine(); ImGui.TextDisabled($"sprites drawn last frame: {_markers_game_art_n}");
            ImGui.SliderFloat("pointer clearance above the head", ref _marker_height, -20f, 60f); ImGui.SliderFloat("pointer size", ref _marker_scale, 0.3f, 3f);
            ImGui.Text($"drawn this frame: {_markers_drawn}");
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Captures (files land next to the mod, collected by run-ffx.ps1)")) {
            if (_fn_frame_arm != null) { if (ImGui.Button("Frame census")) { _fn_frame_arm(); _frame_census_n++; } ImGui.SameLine(); ImGui.Text($"{_frame_census_n}  ffxvr_frame.log: one frame's targets, draws, resolves"); }
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

        push_override_and_stereo();
    }

    /// <summary>Numpad nudges and the per-frame push of override + eye-2 state to the native side.</summary>
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
    private void push_override_and_stereo() {
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
        if (ImGui.IsKeyPressed(ImGuiKey.Keypad5))     { _ovr_yaw = _ovr_pitch = 0; _ovr_right = _ovr_up = _ovr_forward = 0; if (_fn_gesture_reset != null) _fn_gesture_reset(); }
        _ovr_pitch = Math.Clamp(_ovr_pitch, -89f, 89f);
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
        if (_fn_imgui_layer_set != null) _fn_imgui_layer_set(_imgui_layer ? 1 : 0, (_window_open || save_screen_state() != 0) ? 1 : 0, _imgui_min_draws);
        if (_fn_gesture_set != null) _fn_gesture_set(_gestures ? 1 : 0, _gest_tilt);
        if (_fn_gesture_poll != null) {
            float mv0, mv1, mv2; float* mv = stackalloc float[3]; float mul; int act;
            uint toggles = _fn_gesture_poll(mv, &mul, &act);
            mv0 = mv[0]; mv1 = mv[1]; mv2 = mv[2];
            _gest_move_x = mv0; _gest_move_y = mv1; _gest_move_z = mv2; _gest_scale_mul = mul; _gest_active = act;
            if ((toggles & 0xFF) != 0) { if (_window_open) _chord_toggle_pending = true; else toggle_first_person(); }
            if (((toggles >> 8) & 0xFF) != 0) _btl_cycle_req++;
        }
        if (_fn_fp_body_set != null) _fn_fp_body_set(_fp_body_mode, parse_mask(_fp_head_parts), _fp_solo);
        if (_fn_cull_scene_set != null && _fn_cull_scene_get != null && (_fn_cull_scene_get() != 0) != _cull_scene) {
            _cull_scene_rc = _fn_cull_scene_set(_cull_scene ? 1 : 0);
            if (_cull_scene_rc != 0) _cull_scene = _fn_cull_scene_get() != 0;
            if (_patch_on.Length > 4) _patch_on[3] = _patch_on[4] = _fn_cull_scene_get() != 0;
        }
        if (_fn_turn != null) {
            float step = _turn_rate * ImGui.GetIO().DeltaTime;
            if (ImGui.IsKeyDown(ImGuiKey.Keypad1)) _fn_turn(-step);
            if (ImGui.IsKeyDown(ImGuiKey.Keypad3)) _fn_turn(step);
        }
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
        value = tmp;
        return ok;
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
    private static bool in_blitz() => map_base(map_name()).StartsWith("bltz");

    // Blitzball: twice a second while on a bltz map, every live actor's logical position (actor table) next to
    // where it is actually drawn (the nearest skinned-root translation in the ground plane), to learn how the
    // game turns its flat match positions into the 3D pool. Plus Farplane's blitz state words. Capped per session.
    private double _blitz_last_log = 0; private int _blitz_log_lines = 0; private string _map_last = "";
    private bool _blitz_overlay = true; private uint _blitz_overlay_n = 0; private delegate* unmanaged[Cdecl]<int, uint> _fn_blitz_overlay;   // scoreboard / radar disc onto the UI panel
    private void blitz_auto_log() {
        double now = ImGui.GetTime(); if (now - _blitz_last_log < 0.5) return; _blitz_last_log = now;
        string map = map_name();
        if (map != _map_last) { _logger.Info($"map: {(_map_last.Length > 0 ? _map_last : "?")} -> {map} (event_id {(Globals.event_id != null ? *Globals.event_id : -1)})"); _map_last = map; }
        if (!map_base(map).StartsWith("bltz") || _blitz_log_lines >= 1200 || _fn_world_positions == null) return;
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
            float rx = -a.chr_pos_vec.X, ry = -a.chr_pos_vec.Y, rz = a.chr_pos_vec.Z; bool drawn = false;
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
        ImGui.TextDisabled("drawn = a draw at the actor's position this frame; hide = byte +180; f194/f198 = flag words; cd = cull distance (+174), d = distance to the game camera (+178); a = alpha (+354); mv = model visible byte");
        ImGui.Text("idx  id   name          position                 drawn hide  f194      f198     cd      d      a   mv");
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
            float rx = -a.chr_pos_vec.X, ry = -a.chr_pos_vec.Y, rz = a.chr_pos_vec.Z; bool drawn = false;
            for (int k = 0; k < nwp; k++) { float dx = wp[k*3] - rx, dy = wp[k*3+1] - ry, dz = wp[k*3+2] - rz; if (dx*dx + dy*dy + dz*dz < 9f) { drawn = true; break; } }
            string line = $"{i,3} {a.chr_id,4}  {actor_name(a.chr_name),-12} ({a.chr_pos_vec.X,7:F1},{a.chr_pos_vec.Y,7:F1},{a.chr_pos_vec.Z,7:F1})  {(drawn ? "yes" : " no"),4}  {hide:X2}  {f194:X8} {f198:X8} {cd,6:F1} {dist,6:F1} {alpha,5:F2}  {mv:X2}";
            ImGui.Text(line);
            sb?.Append("\n  ").Append(line);
        }
        if (sb != null) _logger.Info(sb.ToString());
    }

    /// <summary>The player: the enabled field actor whose model name matches (Tidus is c101); index 0
    /// is not reliable. Falls back to actor 0. Render space negates X and Y.</summary>
    private string _fp_actor = "c101"; private int _fp_actor_idx = -1;
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
        for (int i = 0; i < 16; i++) {
            if (plys != null && read(plys + i, out Chr pc) && pc.actor != null && read(pc.actor, out Actor pa) && live(pa)) { sx += pa.chr_pos_vec.X; sy += pa.chr_pos_vec.Y; sz += pa.chr_pos_vec.Z; n++; }
            if (mons != null && read(mons + i, out Chr mc) && mc.actor != null && read(mc.actor, out Actor ma) && live(ma) && mc.stat_visible != 0) { sx += ma.chr_pos_vec.X; sy += ma.chr_pos_vec.Y; sz += ma.chr_pos_vec.Z; n++; }
        }
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
        if (_fn_hands_set != null) _fn_hands_set(_hands_on ? 1 : 0, (_hands_head ? 1 : 0) | (_hands_show << 1), _hands_yaw * 0.0174533f, _hands_pitch * 0.0174533f, _hands_roll * 0.0174533f, _hands_off_r, _hands_off_u, _hands_off_b, _hands_swap ? 1 : 0, _hands_mirror ? 1 : 0, _hands_only); }
    private void push_player() {
        if (_fn_player_set == null) return;
        Actor* actors = Globals.actors;
        if (actors == null) { _fn_player_set(0, 0, 0, 0); _fp_actor_idx = -1; return; }
        // Before the field is up the table is uninitialised memory (0xCDCDCDCD
        // fill): only a plain 1 counts as enabled, and a name pointer must look
        // like one before it is read.
        int pick = -1;
        // Battle: the eyes follow a chr id chosen from the game's field roster (see the state above).
        Btl* btl = Globals.Battle.btl; bool in_battle = read(btl, out Btl btl_v) && btl_v.battle_state != 0;
        if (in_battle != _btl_was) {
            _btl_was = in_battle; _btl_chr = -1; _btl_follow_id = -1; _btl_follow_slot = -1; _btl_pre_summon_id = -1; _btl_pending_id = -1; _btl_pending_frames = 0; _btl_who = ""; _btl_aeon_out = false;
            for (int i = 0; i < 3; i++) _btl_roster_prev[i] = 0xFF;
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
                    if (ImGui.IsKeyPressed(ImGuiKey.Insert)) _btl_cycle_req++;
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
                else if (_btl_pending_id >= 0 && !on_field(_btl_pending_id)) _btl_pending_id = -1;

                if (want >= 0 && want != _btl_follow_id) {
                    // A swap: the newcomer runs in from the bench. Wait until he has stopped moving (or a few seconds)
                    // so the eyes do not ride the run-in; battle start, summons and manual picks are immediate.
                    bool swap_in = _btl_follow_id >= 0 && why != null && (why == "Tidus joined the field" || why.EndsWith("left the field"));
                    int wi = ply_of(want);
                    if (_btl_pending_id != want) { _btl_pending_id = want; _btl_pending_why = why ?? ""; _btl_pending_frames = 0; _btl_pending_still = 0; _logger.Info($"battle follow: {why} -> chr {want}{(swap_in ? ", waiting for him to arrive" : "")}"); }
                    bool ready = !swap_in || wi < 0;
                    if (!ready && read(plys + wi, out Chr wc) && wc.actor != null && read(wc.actor, out Actor wa) && live(wa)) {
                        float dx = wa.chr_pos_vec.X - _btl_pending_pos.X, dz = wa.chr_pos_vec.Z - _btl_pending_pos.Z;
                        _btl_pending_still = dx * dx + dz * dz < 0.3f * 0.3f ? _btl_pending_still + 1 : 0;
                        _btl_pending_pos = wa.chr_pos_vec;
                        if (_btl_pending_frames > 30 && _btl_pending_still > 15) ready = true;   // ran in, then stood still a quarter second
                    }
                    if (++_btl_pending_frames > 300) ready = true;
                    if (ready) {
                        _btl_follow_id = want; _btl_pending_id = -1; _btl_pending_frames = 0;
                        _btl_chr = ply_of(want); _btl_who = _btl_chr >= 0 && read(plys + _btl_chr, out Chr fc) && fc.actor != null && read(fc.actor, out Actor fa) ? actor_name(fa.chr_name) : $"chr {want}";
                        _logger.Info($"battle follow: {why} -> following {_btl_who} (chr {want}, ply {_btl_chr})");
                    }
                }
                if (_btl_follow_id >= 0) {
                    if (_btl_chr < 0 || !read(plys + _btl_chr, out Chr cc) || cc.chr_id != _btl_follow_id) _btl_chr = ply_of(_btl_follow_id);
                    int sl = slot_of(_btl_follow_id); if (sl >= 0) _btl_follow_slot = sl;
                    if (_btl_chr >= 0 && read(plys + _btl_chr, out Chr fc) && fc.actor != null && read(fc.actor, out Actor fa) && live(fa)) {
                        pick = (int)(((byte*)fc.actor - (byte*)actors) / sizeof(Actor)); if (pick < 0 || pick >= 48) pick = -1;
                    }
                }
            }
        }
        if (pick >= 0) { }
        else if (_fp_actor_idx >= 0 && read(actors + _fp_actor_idx, out Actor cur) && live(cur) && actor_name(cur.chr_name) == _fp_actor) pick = _fp_actor_idx;
        else {
            for (int i = 0; i < 48 && pick < 0; i++) {
                if (!read(actors + i, out Actor a)) break;
                if (live(a) && actor_name(a.chr_name) == _fp_actor) pick = i;
            }
            if (pick < 0 && read(actors, out Actor a0) && live(a0)) pick = 0;
        }
        _fp_actor_idx = pick;
        if (pick >= 0 && read(actors + pick, out Actor p)) {
            _fn_player_set(-p.chr_pos_vec.X, -p.chr_pos_vec.Y, p.chr_pos_vec.Z, 1);
            // The head: the highest sub-position that is not at the origin. Actor
            // space has Y pointing down (render space negates it), so the head is
            // the one with the LOWEST actor-space Y.
            float hy = float.MaxValue; Vector4 head = default; bool have = false;
            for (int k = 0; k < 26; k++) { Vector4 sp = p.chr_optpos[k]; if (sp.Y < hy && MathF.Abs(sp.X) + MathF.Abs(sp.Z) > 0.01f) { hy = sp.Y; head = sp; have = true; } }
            // The sub-positions look absolute in the actor table; if one ever turns out to be relative, fold the actor position in.
            if (have && MathF.Abs(head.X - p.chr_pos_vec.X) + MathF.Abs(head.Z - p.chr_pos_vec.Z) > 100f) head += p.chr_pos_vec;
            if (_fn_player_head_set != null) _fn_player_head_set(-head.X, -head.Y, head.Z, have ? 1 : 0);
        }
        else { _fn_player_set(0, 0, 0, 0); if (_fn_player_head_set != null) _fn_player_head_set(0, 0, 0, 0); }
        if (_fn_fp_head_set != null) _fn_fp_head_set(_fp_head_anchor, _fp_head_up, _fp_head_smooth, _fp_head_fwd);
        push_battle_anchor();
        if (_fn_floor_set != null) _fn_floor_set(_diorama_floor);
        if (_fn_ui_hide != null) {
            bool hide = _ui_hide == 1 || _ui_hide == 2 || (_ui_hide == 3 && in_battle);
            _fn_ui_hide(hide ? 1024u : 0u, 1024u, (_ui_hide == 2 || (_ui_hide == 4 && in_battle)) ? 1 : (_ui_hide == 5 && in_battle) ? 2 : 0);
            if (_fn_ui_hidden_draws != null) _ui_hidden_n = _fn_ui_hidden_draws();
            if (_fn_ui_hide_cursor != null) _fn_ui_hide_cursor(_hide_cursor && in_battle && _xr_on ? 1 : 0);   // headset only: on the monitor the game's cursor is right where it should be
            if (_fn_ui_hide_pointers != null) _fn_ui_hide_pointers(_hide_pointers && in_battle && _xr_on ? 1 : 0);
            if (_fn_ui_pointer_stats != null) _pointer_stats = _fn_ui_pointer_stats();
            if (_fn_blitz_overlay != null) _blitz_overlay_n = _fn_blitz_overlay(_blitz_overlay && map_base(_map_last).StartsWith("bltz") ? 1 : 0);
            // stereo screen (pushed with the window hidden too; the screen's size and mode go with it)
            if (_fn_screen3d != null) _fn_screen3d((_s3_all || (_s3_blitz && map_base(_map_last).StartsWith("bltz"))) ? 1 : 0, _s3_depth, _s3_plane, _s3_auto ? 1 : 0, _s3_max_pop * 0.01f);
            if (_fn_xr_screen_set != null) _fn_xr_screen_set(_xr_screen_mode, _xr_screen_width, _xr_screen_dist);
            if (_fn_markers_game_art != null) _markers_game_art_n = _fn_markers_game_art(_markers_game_art ? 1 : 0);
            if (_fn_markers_scale != null) _fn_markers_scale(_marker_scale);
            if (_hide_list_dirty && _fn_ui_hide_list != null) {
                _hide_list_dirty = false; float* buf = stackalloc float[16 * 6]; int cnt = Math.Min(_hide_list.Count, 16);
                for (int i = 0; i < cnt; i++) for (int k = 0; k < 6; k++) buf[i * 6 + k] = _hide_list[i][k];
                _fn_ui_hide_list(buf, cnt);
            }
            if (_fn_ui_hide_hits != null) { fixed (uint* hp = _hide_hits) _fn_ui_hide_hits(hp, 16); }
            if (_fn_ui_rect_stats != null) _rect_stats = _fn_ui_rect_stats();
        }
    }

    private void push_markers() {
        if (_fn_markers_set == null) return;
        fixed (float* p = _mk_pos) fixed (uint* c = _mk_col) _fn_markers_set(p, c, _mk_n);
        if (_fn_markers_3d != null) _fn_markers_3d(_markers_3d ? 1 : 0);
    }

    private void draw_markers() {
        _markers_drawn = 0; _mk_n = 0;
        try { draw_markers_inner(); } finally { push_markers(); }
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
        if (now - _last_pos_log > 1.0 && _fn_world_positions != null) {
            _last_pos_log = now;
            var sb = new System.Text.StringBuilder(
                $"window: state={w.state} type={w.type} cur_chr_id={w.cur_chr_id} depth={w.submenu_depth} cmd={w.window_command_id} "
              + $"sel_idx={w.sel_idx} sel_idx_render={w.sel_idx_render} sel_cmd={w.sel_command_id} sel_chrs=0x{w.sel_chrs:X4} sel_mons=0x{w.sel_mons:X4}"
              + $"  field={btl_field_str()}  actors:");
            for (int i = 0; i < 16; i++) {
                if (mons != null && read(mons + i, out Chr mc) && mc.actor != null && read(mc.actor, out Actor ma))
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
        for (int i = 0; i < 16; i++) {
            if (plys != null && read(plys + i, out Chr pc) && pc.id < 32 && (selected & (1u << pc.id)) != 0) mark(plys + i, 0xFF4040FF, "target");
            if (mons != null && read(mons + i, out Chr mc) && mc.id < 32 && (selected & (1u << mc.id)) != 0) mark(mons + i, 0xFF4040FF, "target");
        }
        if (plys != null) {
            for (int i = 0; i < 16; i++) {
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
        try { render_imgui_inner(); }
        catch (Exception e) {
            if (_render_faults++ < 5) _logger.Error($"render_imgui fault #{_render_faults}: {e}");
        }
    }

    private void render_imgui_inner() {
        _frames++;

        try_install_hooks();
        if (_native_init_done) draw_markers();
        if (_native_init_done) actors_auto_log();
        if (_native_init_done) { try { blitz_auto_log(); } catch (Exception e) { if (_blitz_log_lines < 1200) { _blitz_log_lines = 1200; _logger.Info($"blitz log off: {e.Message}"); } } }
        if (_native_init_done) draw_eye2_preview();   // its own top-level window, never nested

        // Marks the frame boundary for the native helper. Fahrenheit calls
        // render_imgui from inside Present, so this fires exactly once a frame.
        if (_fn_begin_frame != null) _fn_begin_frame();

        if (ImGui.IsKeyPressed(ImGuiKey.F8)) _window_open = !_window_open;
        if (ImGui.IsKeyPressed(ImGuiKey.F10)) _slowmo_on = !_slowmo_on;
        if (ImGui.IsKeyPressed(ImGuiKey.F11)) _hover_mode = (_hover_mode + 1) % 3;
        if (ImGui.IsKeyPressed(ImGuiKey.Keypad5) && _fn_motion_log != null) _fn_motion_log(180);
        draw_hover_highlight();
        if (_fn_slowmo != null) _fn_slowmo(_slowmo_on ? _slowmo_ms : 0);
        if (ImGui.IsKeyPressed(ImGuiKey.F9) && _fn_ui_pick != null) {
            Vector2 m = ImGui.GetMousePos(), ds = ImGui.GetIO().DisplaySize;
            if (ds.X > 0 && ds.Y > 0) { _fn_ui_pick(m.X / ds.X, m.Y / ds.Y); _pick_n++; _logger.Info($"pick at ({m.X:F0},{m.Y:F0}) of {ds.X:F0}x{ds.Y:F0}"); }
        }
        if (_fn_ui_pick_result != null) { try { string r = Marshal.PtrToStringAnsi(_fn_ui_pick_result()) ?? ""; if (r.Length > 0) _pick_text = r; } catch { } }
        if (ImGui.IsKeyPressed(ImGuiKey.Home) && _fn_xr_recenter != null) _fn_xr_recenter();

        if (_window_open && _native_init_done) {
            ImGui.SetNextWindowSize(new Vector2(520, 0), ImGuiCond.FirstUseEver);
            try {
                if (ImGui.Begin("FFX VR", ref _window_open)) {
                    draw_main();
                    draw_debug();
                }
            }
            finally { ImGui.End(); }
        }
        else if (_native_init_done) {
            push_override_and_stereo();   // state still flows to the native side with the window hidden
            push_player();
            if (ImGui.IsKeyPressed(ImGuiKey.End)) toggle_first_person();
            if (_fn_tp_set != null) _fn_tp_set(_tp_dist, _tp_h, _tp_pitch * 0.0174533f, _tp_follow_rate);
            push_hands();
            if (_fn_fp_set != null) _fn_fp_set(_fp_mode, _fp_eye_h, _fp_follow_dist, _fp_follow_h, _fp_hide_body ? 1 : 0, _fp_hide_radius);
            if (_fn_xr_set != null) {
                bool want = _xr_want;
                if (want) { _ovr_enabled = true; _stereo_on = true; }
                _xr_on = _fn_xr_set(want ? 1 : 0, _xr_scale, _xr_rotate ? 1 : 0, _xr_translate ? 1 : 0, _xr_fov ? 1 : 0, _xr_seat ? 1 : 0) != 0;
            }
        }
        settings_reload_if_edited();
        settings_save_if_changed();
    }
}
