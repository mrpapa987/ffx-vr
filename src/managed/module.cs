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
    private delegate* unmanaged[Cdecl]<int> _fn_cut_count;
    private bool _cut_recentre = true, _cut_blink = false, _follow_pans = false; private float _follow_rate = 20f;
    private delegate* unmanaged[Cdecl]<float*, uint*, int, void> _fn_markers_set;
    private delegate* unmanaged[Cdecl]<int, void> _fn_markers_3d;
    private float[] _mk_pos = new float[32 * 3]; private uint[] _mk_col = new uint[32]; private int _mk_n;
    private bool _markers_3d = true;
    private bool _ui_on = true; private float _ui_dist = 1.5f, _ui_width = 1.6f;
    private delegate* unmanaged[Cdecl]<int> _fn_xr_screen_now;
    private int _xr_screen_mode = 0; private float _xr_screen_width = 2.5f, _xr_screen_dist = 2.5f;
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
    private bool  _eye2_preview = true;
    private delegate* unmanaged[Cdecl]<nint, nint, nint, nint> _fn_on_updatesub;
    private delegate* unmanaged[Cdecl]<uint, nint*, nint, void> _fn_on_rendertargets;
    private delegate* unmanaged[Cdecl]<uint, nint*, nint, void> _fn_on_rendertargets_uav;
    private delegate* unmanaged[Cdecl]<nint, uint, nint, uint, uint, void> _fn_on_resolve;
    private delegate* unmanaged[Cdecl]<float, float, float, float, void> _fn_on_viewport;
    private delegate* unmanaged[Cdecl]<nint, int, void> _fn_on_clear;
    private delegate* unmanaged[Cdecl]<void> _fn_frame_arm;
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
    private float _marker_height = 25f;
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
            _fn_cut_count    = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_cut_count");
            _fn_markers_set  = (delegate* unmanaged[Cdecl]<float*, uint*, int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_markers_set");
            _fn_markers_3d   = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_markers_3d");
            _fn_xr_screen_now = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_xr_screen_now");
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
        int eye2 = (ours(ctx) && _fn_on_draw != null) ? _fn_on_draw(1) : 0;
        _o_drawindexed ??= new FhCallHandle<d_DrawIndexed>(new FhCallLocation((void*)_a_drawindexed)).chain_from(_h_drawindexed!).fnptr;
        _o_drawindexed!(ctx, n, start, base_vertex);
        for (int k = 0; k < eye2; k++) if (_fn_eye2_begin(k) != 0) { _o_drawindexed!(ctx, n, start, base_vertex); _fn_eye2_end(); }
    }

    private void h_draw(nint ctx, uint n, uint start) {
        if (_fn_draw_ctx != null) _fn_draw_ctx(ctx);
        int eye2 = (ours(ctx) && _fn_on_draw != null) ? _fn_on_draw(2) : 0;
        _o_draw ??= new FhCallHandle<d_Draw>(new FhCallLocation((void*)_a_draw)).chain_from(_h_draw!).fnptr;
        _o_draw!(ctx, n, start);
        for (int k = 0; k < eye2; k++) if (_fn_eye2_begin(k) != 0) { _o_draw!(ctx, n, start); _fn_eye2_end(); }
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
    private bool _headset_autostart = false;

    private void settings_load() {
        try {
            string dir = Path.GetDirectoryName(typeof(FFXVRModule).Assembly.Location) ?? ".";
            _ini_path = Path.Combine(dir, "ffxvr.ini");
            if (!File.Exists(_ini_path)) return;
            foreach (string raw in File.ReadAllLines(_ini_path)) {
                string line = raw.Trim();
                int eq = line.IndexOf('=');
                if (line.Length == 0 || line[0] == '#' || eq < 0) continue;
                string k = line[..eq].Trim(), v = line[(eq + 1)..].Trim();
                var ci = System.Globalization.CultureInfo.InvariantCulture;
                switch (k) {
                    case "headset_autostart": _headset_autostart = v == "1"; break;
                    case "world_scale":       float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _xr_scale); break;
                    case "head_position":     _xr_translate = v == "1"; break;
                    case "seat":              _xr_seat = v == "1"; break;
                    case "headset_fov":       _xr_fov = v == "1"; break;
                    case "screen_mode":       int.TryParse(v, out _xr_screen_mode); break;
                    case "screen_width":      float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _xr_screen_width); break;
                    case "screen_distance":   float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _xr_screen_dist); break;
                    case "overlay_open":      _window_open = v != "0"; break;
                    case "ui_panel":          _ui_on = v != "0"; break;
                    case "ui_distance":       float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _ui_dist); break;
                    case "ui_width":          float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _ui_width); break;
                    case "pitch_follow":      float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _xr_pitch_follow); break;
                    case "seat_height":       float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _ovr_up); break;
                    case "seat_forward":      float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _ovr_forward); break;
                    case "cut_recentre":      _cut_recentre = v != "0"; break;
                    case "cut_blink":         _cut_blink = v == "1"; break;
                    case "follow_pans":       _follow_pans = v == "1"; break;
                    case "follow_rate":       float.TryParse(v, System.Globalization.NumberStyles.Float, ci, out _follow_rate); break;
                }
            }
            if (_headset_autostart) _xr_want = true;
        }
        catch (Exception e) { _logger.Warning($"settings: {e.Message}"); }
    }

    private void settings_save_if_changed() {
        if (_ini_path.Length == 0) return;
        var ci = System.Globalization.CultureInfo.InvariantCulture;
        string text =
            "# FFX VR settings. Edited by the overlay; hand edits are fine.\n" +
            $"headset_autostart={(_headset_autostart ? 1 : 0)}\n" +
            $"world_scale={_xr_scale.ToString("F1", ci)}\n" +
            $"head_position={(_xr_translate ? 1 : 0)}\n" +
            $"seat={(_xr_seat ? 1 : 0)}\n" +
            $"headset_fov={(_xr_fov ? 1 : 0)}\n" +
            $"screen_mode={_xr_screen_mode}\n" +
            $"screen_width={_xr_screen_width.ToString("F2", ci)}\n" +
            $"screen_distance={_xr_screen_dist.ToString("F2", ci)}\n" +
            $"overlay_open={(_window_open ? 1 : 0)}\n" +
            $"ui_panel={(_ui_on ? 1 : 0)}\n" +
            $"ui_distance={_ui_dist.ToString("F2", ci)}\n" +
            $"ui_width={_ui_width.ToString("F2", ci)}\n" +
            $"pitch_follow={_xr_pitch_follow.ToString("F2", ci)}\n" +
            $"seat_height={_ovr_up.ToString("F0", ci)}\n" +
            $"seat_forward={_ovr_forward.ToString("F0", ci)}\n" +
            $"cut_recentre={(_cut_recentre ? 1 : 0)}\n" +
            $"cut_blink={(_cut_blink ? 1 : 0)}\n" +
            $"follow_pans={(_follow_pans ? 1 : 0)}\n" +
            $"follow_rate={_follow_rate.ToString("F0", ci)}\n";
        if (text == _ini_last) return;
        try { File.WriteAllText(_ini_path, text); _ini_last = text; }
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
        if (want) { _ovr_enabled = true; _stereo_on = true; }   // the headset needs the override and both eyes
        _xr_on = _fn_xr_set(want ? 1 : 0, _xr_scale, _xr_rotate ? 1 : 0, _xr_translate ? 1 : 0, _xr_fov ? 1 : 0, _xr_seat ? 1 : 0) != 0;
        _xr_want = want;   // asked each frame until the native side accepts (it waits for the first 3D frame)
        ImGui.TextWrapped(xr_status_text());
        if (_fn_xr_screen_now != null && _xr_on) ImGui.Text(_fn_xr_screen_now() != 0 ? "showing: virtual screen" : "showing: 3D scene");

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

        // View
        ImGui.Separator();
        ImGui.Checkbox("headset field of view", ref _xr_fov);
        ImGui.Text("2D screens:"); ImGui.SameLine();
        ImGui.RadioButton("auto", ref _xr_screen_mode, 0); ImGui.SameLine();
        ImGui.RadioButton("always", ref _xr_screen_mode, 1); ImGui.SameLine();
        ImGui.RadioButton("never", ref _xr_screen_mode, 2);
        if (_fn_xr_screen_set != null) _fn_xr_screen_set(_xr_screen_mode, _xr_screen_width, _xr_screen_dist);
        ImGui.Checkbox("UI on its own panel (menus, subtitles, HUD)", ref _ui_on);
        if (_fn_ui_set != null) _fn_ui_set(_ui_on ? 1 : 0, _ui_dist, _ui_width);

        ImGui.Separator();
        ImGui.TextDisabled("Home: recentre   F7: hide/show this overlay");
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
            ImGui.TextDisabled("Numpad: 4/6 yaw, 8/2 pitch, +/- forward/back, 7/9 up/down, 0 reset");
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

        if (ImGui.TreeNode("Battle markers")) {
            ImGui.Checkbox("draw target / turn markers", ref _markers_on); ImGui.SameLine();
            ImGui.Checkbox("as 3D geometry in the headset", ref _markers_3d);
            ImGui.SliderFloat("marker height", ref _marker_height, 0f, 100f);
            ImGui.Text($"drawn this frame: {_markers_drawn}");
            ImGui.TreePop();
        }

        if (ImGui.TreeNode("Captures (files land next to the mod, collected by run-ffx.ps1)")) {
            if (_fn_frame_arm != null) { if (ImGui.Button("Frame census")) { _fn_frame_arm(); _frame_census_n++; } ImGui.SameLine(); ImGui.Text($"{_frame_census_n}  ffxvr_frame.log: one frame's targets, draws, resolves"); }
            if (_fn_measure != null)   { if (ImGui.Button("Measure + images")) { _fn_measure(); _measure_n++; } ImGui.SameLine(); ImGui.Text($"{_measure_n}  ffxvr_diag.log + img_*.bmp: eye 1 vs eye 2"); }
            if (_fn_dump_seq != null)  { if (ImGui.Button("8-frame sequence")) _fn_dump_seq(8); ImGui.SameLine(); ImGui.Text("seq_*.bmp: both eyes, 8 consecutive frames"); }
            ImGui.TreePop();
        }

        push_override_and_stereo();
    }

    /// <summary>Numpad nudges and the per-frame push of override + eye-2 state to the native side.</summary>
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
        if (ImGui.IsKeyPressed(ImGuiKey.Keypad0))     { _ovr_yaw = _ovr_pitch = 0; _ovr_right = _ovr_up = _ovr_forward = 0; }
        _ovr_pitch = Math.Clamp(_ovr_pitch, -89f, 89f);
        if (_fn_override_set != null) _fn_override_set(_ovr_enabled ? 1 : 0, _ovr_freeze ? 1 : 0, _ovr_yaw, _ovr_pitch, _ovr_right, _ovr_up, _ovr_forward);
        if (_fn_stereo_set != null) _fn_stereo_set(_stereo_on ? 1 : 0, _ipd, 0, 0, 0);
    }

    /// <summary>Shows the eye-2 render in its own window, scaled down.</summary>
    private void draw_eye2_preview() {
        if (!_eye2_preview || !_stereo_on || _fn_eye2_srv == null || _fn_eye2_size == null) return;
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

    private void push_markers() {
        if (_fn_markers_set == null) return;
        fixed (float* p = _mk_pos) fixed (uint* c = _mk_col) _fn_markers_set(p, c, _mk_n);
        if (_fn_markers_3d != null) _fn_markers_3d(_markers_3d ? 1 : 0);
    }

    private void draw_markers() {
        _markers_drawn = 0; _mk_n = 0;
        try { draw_markers_inner(); } finally { push_markers(); }
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
              + $"sel_idx={w.sel_idx} sel_idx_render={w.sel_idx_render} sel_cmd={w.sel_command_id} sel_chrs=0x{w.sel_chrs:X4} sel_mons=0x{w.sel_mons:X4}  actors:");
            for (int i = 0; i < 16; i++) {
                if (mons != null && read(mons + i, out Chr mc) && mc.actor != null && read(mc.actor, out Actor ma))
                    sb.Append($" mon{i}[id={mc.id} chr_id={mc.chr_id} vis={mc.stat_visible}]=({ma.chr_pos_vec.X:F1},{ma.chr_pos_vec.Y:F1},{ma.chr_pos_vec.Z:F1})");
                if (plys != null && read(plys + i, out Chr pc) && pc.actor != null && read(pc.actor, out Actor pa))
                    sb.Append($" ply{i}[id={pc.id} chr_id={pc.chr_id}]=({pa.chr_pos_vec.X:F1},{pa.chr_pos_vec.Y:F1},{pa.chr_pos_vec.Z:F1})");
            }
            float* wp = stackalloc float[32 * 3];
            int n = _fn_world_positions(wp, 32);
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

        // For the headset: the same point as 3D geometry, drawn natively per eye.
        if (_mk_n < 32) {
            _mk_pos[_mk_n * 3 + 0] = -a.chr_pos_vec.X; _mk_pos[_mk_n * 3 + 1] = -a.chr_pos_vec.Y + _marker_height; _mk_pos[_mk_n * 3 + 2] = a.chr_pos_vec.Z;
            _mk_col[_mk_n] = color; _mk_n++;
        }

        float* ndc = stackalloc float[2];
        // Actor positions are in the game's PS2-era space; the renderer's world
        // space negates X and Y (verified against the skinned world matrices).
        if (_fn_project(-a.chr_pos_vec.X, -a.chr_pos_vec.Y + _marker_height, a.chr_pos_vec.Z, ndc) == 0) return;

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

    public override void render_imgui() {
        _frames++;

        try_install_hooks();
        if (_native_init_done) draw_markers();
        if (_native_init_done) draw_eye2_preview();   // its own top-level window, never nested

        // Marks the frame boundary for the native helper. Fahrenheit calls
        // render_imgui from inside Present, so this fires exactly once a frame.
        if (_fn_begin_frame != null) _fn_begin_frame();

        if (ImGui.IsKeyPressed(ImGuiKey.F7)) _window_open = !_window_open;
        if (ImGui.IsKeyPressed(ImGuiKey.Home) && _fn_xr_recenter != null) _fn_xr_recenter();

        if (_window_open && _native_init_done) {
            ImGui.SetNextWindowSize(new Vector2(520, 0), ImGuiCond.FirstUseEver);
            if (ImGui.Begin("FFX VR", ref _window_open)) {
                draw_main();
                draw_debug();
            }
            ImGui.End();
        }
        else if (_native_init_done) {
            push_override_and_stereo();   // state still flows to the native side with the window hidden
            if (_fn_xr_set != null) {
                bool want = _xr_want;
                if (want) { _ovr_enabled = true; _stereo_on = true; }
                _xr_on = _fn_xr_set(want ? 1 : 0, _xr_scale, _xr_rotate ? 1 : 0, _xr_translate ? 1 : 0, _xr_fov ? 1 : 0, _xr_seat ? 1 : 0) != 0;
            }
        }
        settings_save_if_changed();
    }
}
