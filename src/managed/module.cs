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
    private delegate* unmanaged[Cdecl]<float*, int> _fn_scan;
    private delegate* unmanaged[Cdecl]<int, int>  _fn_counter;
    private delegate* unmanaged[Cdecl]<int, void> _fn_on_draw;
    private delegate* unmanaged[Cdecl]<int*, int> _fn_diff;
    private delegate* unmanaged[Cdecl]<nint, nint, nint, nint> _fn_on_updatesub;
    private delegate* unmanaged[Cdecl]<int, int, float, float, float, float, float, void> _fn_override_set;
    private delegate* unmanaged[Cdecl]<int> _fn_override_applied;
    private delegate* unmanaged[Cdecl]<int> _fn_override_mask;
    private delegate* unmanaged[Cdecl]<int> _fn_memscan;
    private delegate* unmanaged[Cdecl]<nuint, nuint, int> _fn_watch;
    private delegate* unmanaged[Cdecl]<int> _fn_watch_hits;
    private delegate* unmanaged[Cdecl]<nuint> _fn_exe_camera;
    private int _watch_rc = -99;
    private delegate* unmanaged[Cdecl]<nuint, uint, int> _fn_xrefs;
    private int _xrefs_n = -99;
    private delegate* unmanaged[Cdecl]<nuint, uint, int> _fn_ptrscan;
    private delegate* unmanaged[Cdecl]<nuint> _fn_scan_struct;
    private int _ptrscan_n = -99;
    private delegate* unmanaged[Cdecl]<int, nuint, void> _fn_cam_mem_set;
    private delegate* unmanaged[Cdecl]<int> _fn_cam_mem_writes;
    private int _cam_mem_mode = 0;
    private delegate* unmanaged[Cdecl]<int> _fn_dump_structs;
    private int _structs_dumped = 0;
    private delegate* unmanaged[Cdecl]<int> _fn_scan_cams;
    private int _memscan_cams = 0;
    private delegate* unmanaged[Cdecl]<uint, uint, void> _fn_census_arm;
    private int _census_n = 0;
    private int _memscan_last = -99;

    // -- camera override state (owned here, pushed to native every frame) ----
    private bool  _ovr_enabled, _ovr_freeze;
    private float _ovr_yaw, _ovr_pitch;
    private float _ovr_right, _ovr_up, _ovr_forward;

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
    private nint _a_drawindexed, _a_draw, _a_drawidxinst, _a_drawinst, _a_updatesub;
    private string _detour_status = "not installed";

    public FFXVRModule() { }

    public override bool init(FhModContext mod_context, FileStream global_state_file) {
        _logger.Info("FFX VR module initialized.");
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
            _fn_scan         = (delegate* unmanaged[Cdecl]<float*, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_scan");
            _fn_counter      = (delegate* unmanaged[Cdecl]<int, int>) NativeLibrary.GetExport(_native_handle, "ffxvr_counter");
            _fn_on_draw      = (delegate* unmanaged[Cdecl]<int, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_on_draw");
            _fn_diff         = (delegate* unmanaged[Cdecl]<int*, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_diff");
            _fn_on_updatesub = (delegate* unmanaged[Cdecl]<nint, nint, nint, nint>)NativeLibrary.GetExport(_native_handle, "ffxvr_on_updatesub");
            _fn_override_set = (delegate* unmanaged[Cdecl]<int, int, float, float, float, float, float, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_override_set");
            _fn_override_applied = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_override_applied");
            _fn_override_mask    = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_override_mask");
            _fn_memscan          = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_memscan");
            _fn_watch            = (delegate* unmanaged[Cdecl]<nuint, nuint, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_watch");
            _fn_watch_hits       = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_watch_hits");
            _fn_exe_camera       = (delegate* unmanaged[Cdecl]<nuint>)NativeLibrary.GetExport(_native_handle, "ffxvr_exe_camera");
            _fn_xrefs            = (delegate* unmanaged[Cdecl]<nuint, uint, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_xrefs");
            _fn_ptrscan          = (delegate* unmanaged[Cdecl]<nuint, uint, int>)NativeLibrary.GetExport(_native_handle, "ffxvr_ptrscan");
            _fn_scan_struct      = (delegate* unmanaged[Cdecl]<nuint>)NativeLibrary.GetExport(_native_handle, "ffxvr_scan_struct");
            _fn_cam_mem_set      = (delegate* unmanaged[Cdecl]<int, nuint, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_cam_mem_set");
            _fn_cam_mem_writes   = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_cam_mem_writes");
            _fn_dump_structs     = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_dump_structs");
            _fn_scan_cams        = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_scan_cams");
            _fn_census_arm       = (delegate* unmanaged[Cdecl]<uint, uint, void>)NativeLibrary.GetExport(_native_handle, "ffxvr_census_arm");

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

        _h_drawindexed = h_drawindexed;
        _h_draw        = h_draw;
        _h_drawidxinst = h_drawidxinst;
        _h_drawinst    = h_drawinst;
        _h_updatesub   = h_updatesub;

        bool ok = true;
        ok &= new FhCallHandle<d_DrawIndexed>         (new FhCallLocation((void*)_a_drawindexed)).hook(this, _h_drawindexed);
        ok &= new FhCallHandle<d_Draw>                (new FhCallLocation((void*)_a_draw))       .hook(this, _h_draw);
        ok &= new FhCallHandle<d_DrawIndexedInstanced>(new FhCallLocation((void*)_a_drawidxinst)).hook(this, _h_drawidxinst);
        ok &= new FhCallHandle<d_DrawInstanced>       (new FhCallLocation((void*)_a_drawinst))   .hook(this, _h_drawinst);
        ok &= new FhCallHandle<d_UpdateSubresource>   (new FhCallLocation((void*)_a_updatesub))  .hook(this, _h_updatesub);

        _detour_status = ok ? "installed" : "FAILED (see core.log)";
        _logger.Info($"Draw detours {_detour_status}: DrawIndexed 0x{_a_drawindexed:X8}, Draw 0x{_a_draw:X8}, "
                   + $"DrawIndexedInstanced 0x{_a_drawidxinst:X8}, DrawInstanced 0x{_a_drawinst:X8}, UpdateSubresource 0x{_a_updatesub:X8}");
    }

    // Each detour reports to the native side, then continues down the chain.
    // The chain pointer is resolved once and cached; resolving it per call
    // would cost a delegate marshal on every draw.

    private void h_drawindexed(nint ctx, uint n, uint start, int base_vertex) {
        if (_fn_on_draw != null) _fn_on_draw(1);
        _o_drawindexed ??= new FhCallHandle<d_DrawIndexed>(new FhCallLocation((void*)_a_drawindexed)).chain_from(_h_drawindexed!).fnptr;
        _o_drawindexed!(ctx, n, start, base_vertex);
    }

    private void h_draw(nint ctx, uint n, uint start) {
        if (_fn_on_draw != null) _fn_on_draw(2);
        _o_draw ??= new FhCallHandle<d_Draw>(new FhCallLocation((void*)_a_draw)).chain_from(_h_draw!).fnptr;
        _o_draw!(ctx, n, start);
    }

    private void h_drawidxinst(nint ctx, uint a, uint b, uint c, int d, uint e) {
        if (_fn_on_draw != null) _fn_on_draw(3);
        _o_drawidxinst ??= new FhCallHandle<d_DrawIndexedInstanced>(new FhCallLocation((void*)_a_drawidxinst)).chain_from(_h_drawidxinst!).fnptr;
        _o_drawidxinst!(ctx, a, b, c, d, e);
    }

    private void h_drawinst(nint ctx, uint a, uint b, uint c, uint d) {
        if (_fn_on_draw != null) _fn_on_draw(4);
        _o_drawinst ??= new FhCallHandle<d_DrawInstanced>(new FhCallLocation((void*)_a_drawinst)).chain_from(_h_drawinst!).fnptr;
        _o_drawinst!(ctx, a, b, c, d);
    }

    private void h_updatesub(nint ctx, nint resource, uint sub, nint box, nint src, uint row_pitch, uint depth_pitch) {
        // The native side records the game's camera and, when the override is
        // on, hands back a patched private copy to upload in its place.
        if (_fn_on_updatesub != null) src = _fn_on_updatesub(resource, box, src);
        _o_updatesub ??= new FhCallHandle<d_UpdateSubresource>(new FhCallLocation((void*)_a_updatesub)).chain_from(_h_updatesub!).fnptr;
        _o_updatesub!(ctx, resource, sub, box, src, row_pitch, depth_pitch);
    }

    /// <summary>
    ///     Shows the camera read live out of the game's own constant buffer.
    ///     Offsets came from a RenderDoc capture; see docs/camera-found.md.
    ///     Translation sits in the last column, so the world position of the
    ///     camera is -R^T * t.
    /// </summary>
    private void draw_camera() {
        if (_fn_get_camera == null) return;

        float* c = stackalloc float[48];
        if (_fn_get_camera(c) == 0) {
            ImGui.Text("Camera: not read yet");
            return;
        }

        ImGui.Separator();
        ImGui.Text($"Camera reads: {(_fn_camera_reads != null ? _fn_camera_reads() : -1)}"
                 + $"   slot-0 buffers/frame: {(_fn_slot0_count != null ? _fn_slot0_count() : -1)}");

        // view matrix, world -> view
        float t0 = c[3], t1 = c[7], t2 = c[11];
        float px = -(c[0] * t0 + c[4] * t1 + c[8]  * t2);
        float py = -(c[1] * t0 + c[5] * t1 + c[9]  * t2);
        float pz = -(c[2] * t0 + c[6] * t1 + c[10] * t2);

        // the inverse view at byte 320 carries the same position directly
        float qx = c[32 + 3], qy = c[32 + 7], qz = c[32 + 11];

        float yaw   = MathF.Atan2(c[8], c[10]) * 180f / MathF.PI;
        float pitch = MathF.Asin(Math.Clamp(-c[9], -1f, 1f)) * 180f / MathF.PI;
        ImGui.Text($"view matrix changed in {(_fn_camera_changes != null ? _fn_camera_changes() : -1)} frames   yaw {yaw,7:F1}  pitch {pitch,6:F1}");
        ImGui.Text($"position (from view):    {px,9:F2} {py,9:F2} {pz,9:F2}");
        ImGui.Text($"position (from inverse): {qx,9:F2} {qy,9:F2} {qz,9:F2}");

        float m0 = c[16 + 0], m5 = c[16 + 5];
        if (MathF.Abs(m0) > 1e-6f && MathF.Abs(m5) > 1e-6f) {
            float fovy = 2f * MathF.Atan(1f / m5) * 180f / MathF.PI;
            float fovx = 2f * MathF.Atan(1f / m0) * 180f / MathF.PI;
            ImGui.Text($"FOV: {fovy:F1} vertical, {fovx:F1} horizontal, aspect {m5 / m0:F3}");
        }

        if (ImGui.TreeNode("view matrix")) {
            for (int r = 0; r < 4; r++)
                ImGui.Text($"{c[r*4+0],10:F4} {c[r*4+1],10:F4} {c[r*4+2],10:F4} {c[r*4+3],10:F4}");
            ImGui.TreePop();
        }
        if (ImGui.TreeNode("projection matrix")) {
            for (int r = 0; r < 4; r++)
                ImGui.Text($"{c[16+r*4+0],10:F4} {c[16+r*4+1],10:F4} {c[16+r*4+2],10:F4} {c[16+r*4+3],10:F4}");
            ImGui.TreePop();
        }
    }


    /// <summary>
    ///     Lists every view-matrix-shaped block the native scanner found in the
    ///     constant buffers written recently, with the camera position each one
    ///     implies.
    /// </summary>
    /// <remarks>
    ///     The write path is the tell. A <c>CreateBuffer</c> hit was filled once,
    ///     when the area loaded, so it is expected to sit still while walking. A
    ///     <c>Map</c> or <c>UpdateSubresource</c> hit was written this frame, so a
    ///     camera that tracks the player must be one of those.
    /// </remarks>

    /// <summary>
    ///     Shows how much of the latched slot-0 buffer changed since the last
    ///     frame. While walking, a live camera must change something in here;
    ///     the offsets say where.
    /// </summary>
    private void draw_diff() {
        if (_fn_diff == null) return;

        int* d = stackalloc int[11];
        _fn_diff(d);

        if (d[1] == 0) {
            ImGui.Text("Latched buffer:        none yet (no draw with a big slot-0 buffer)");
            return;
        }

        ImGui.Text($"Latched buffer:        {d[1]} bytes, taken at draw #{d[2]} of the frame");
        ImGui.Text($"Changed since last frame: {d[0]} floats");

        if (d[0] > 0) {
            var sb = new System.Text.StringBuilder("  at bytes:");
            for (int i = 0; i < Math.Min(8, d[0]); i++) sb.Append(' ').Append(d[3 + i]);
            if (d[0] > 8) sb.Append(" ...");
            ImGui.Text(sb.ToString());
        }
    }


    /// <summary>
    ///     Camera override controls. Numpad works without focusing the overlay:
    ///     4/6 yaw, 8/2 pitch, +/- forward/back, 7/9 up/down, 0 reset.
    /// </summary>
    private void draw_override() {
        if (_fn_override_set == null) return;

        ImGui.Separator();
        ImGui.Text("Camera override");
        ImGui.Checkbox("Enable", ref _ovr_enabled);
        ImGui.SameLine();
        ImGui.Checkbox("Freeze game camera", ref _ovr_freeze);
        ImGui.SameLine();
        if (ImGui.Button("Reset")) { _ovr_yaw = _ovr_pitch = 0; _ovr_right = _ovr_up = _ovr_forward = 0; }

        ImGui.SliderFloat("yaw",     ref _ovr_yaw,     -180f, 180f);
        ImGui.SliderFloat("pitch",   ref _ovr_pitch,    -89f,  89f);
        ImGui.SliderFloat("right",   ref _ovr_right,  -2000f, 2000f);
        ImGui.SliderFloat("up",      ref _ovr_up,     -2000f, 2000f);
        ImGui.SliderFloat("forward", ref _ovr_forward, -2000f, 2000f);

        // numpad nudges, scaled by frame time so they feel the same at any fps
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

        _fn_override_set(_ovr_enabled ? 1 : 0, _ovr_freeze ? 1 : 0, _ovr_yaw, _ovr_pitch, _ovr_right, _ovr_up, _ovr_forward);

        int mask = _fn_override_mask != null ? _fn_override_mask() : 0;
        ImGui.Text($"buffers patched: {(_fn_override_applied != null ? _fn_override_applied() : -1)}   blocks:"
                 + ((mask & 1)  != 0 ? " view" : "") + ((mask & 2)  != 0 ? " inv" : "") + ((mask & 4) != 0 ? " pos" : "")
                 + ((mask & 8)  != 0 ? " VP" : "")   + ((mask & 16) != 0 ? " VW" : "")  + ((mask & 32) != 0 ? " PVW" : "")
                 + ((mask & 64) != 0 ? " IVW" : "") + ((mask & 128) != 0 ? " shifted" : "") + ((mask & 256) != 0 ? " product-only" : ""));
        ImGui.TextWrapped("Numpad: 4/6 yaw, 8/2 pitch, +/- forward/back, 7/9 up/down, 0 reset.");

        if (_fn_cam_mem_set != null) {
            ImGui.Separator();
            ImGui.Text("Also write the camera into game memory (for CPU-projected UI):");
            ImGui.RadioButton("off", ref _cam_mem_mode, 0); ImGui.SameLine();
            ImGui.RadioButton("at first upload", ref _cam_mem_mode, 1); ImGui.SameLine();
            ImGui.RadioButton("at present", ref _cam_mem_mode, 2); ImGui.SameLine();
            ImGui.RadioButton("both", ref _cam_mem_mode, 3); ImGui.SameLine();
            ImGui.RadioButton("array + static, both timings", ref _cam_mem_mode, 7);
            if (_fn_dump_structs != null) {
                if (ImGui.IsKeyPressed(ImGuiKey.F8)) { _fn_dump_structs(); _structs_dumped++; }
                ImGui.Text($"F8: dump the game's camera structs ({_structs_dumped} so far, ffxvr_structs.log)");
            if (_fn_census_arm != null) {
                if (ImGui.IsKeyPressed(ImGuiKey.F6)) { _fn_census_arm(16, 512); _census_n++; }
                ImGui.Text($"F6: record call stacks of 2D-UI uploads for one frame ({_census_n} taken, ffxvr_stacks.log)");
            }
            }
            nuint obj = _fn_scan_struct != null ? _fn_scan_struct() : 0;
            _fn_cam_mem_set(_cam_mem_mode, obj);
            ImGui.Text($"camera object: {(obj != 0 ? $"0x{(nint)obj:X8}" : "none")}   array entries: {_memscan_cams}   writes: {(_fn_cam_mem_writes != null ? _fn_cam_mem_writes() : -1)}   (press F10 first)");
        }

        if (_fn_memscan != null) {
            if (ImGui.IsKeyPressed(ImGuiKey.F10)) { _memscan_last = _fn_memscan(); if (_fn_scan_cams != null) _memscan_cams = _fn_scan_cams(); }
            ImGui.Text(_memscan_last == -99 ? "F10: scan memory for the game's camera"
                     : $"memory scan: {_memscan_last} hits (ffxvr_memscan.log)");
        }

        if (_fn_watch != null && _fn_exe_camera != null) {
            if (ImGui.IsKeyPressed(ImGuiKey.F11) && _fn_xrefs != null) {
                // exact matrix, then a wide window, so a struct base that
                // encloses it shows up with its distance
                _xrefs_n  = _fn_xrefs(_fn_exe_camera(), 64);
                _xrefs_n += _fn_xrefs(_fn_exe_camera() - 0x4000, 0x4000 + 0x400);
            }
            ImGui.Text(_xrefs_n == -99 ? $"F11: list code touching the game camera at 0x{(nint)_fn_exe_camera():X8}"
                     : $"code references: {_xrefs_n} (ffxvr_xrefs.log)");

            if (ImGui.IsKeyPressed(ImGuiKey.F12) && _fn_ptrscan != null) {
                // Pointers to the static matrix (or a struct enclosing it), and to
                // the heap camera object the F10 scan found, then who reads them.
                _ptrscan_n  = _fn_ptrscan(_fn_exe_camera(), 0x800);
                nuint obj = _fn_scan_struct != null ? _fn_scan_struct() : 0;
                if (obj != 0) _ptrscan_n += _fn_ptrscan(obj, 0x800);
            }
            ImGui.Text(_ptrscan_n == -99 ? "F12: find pointers to the camera (run F10 first), then who reads them"
                     : $"pointer variables found: {_ptrscan_n} (ffxvr_xrefs.log)");
        }
    }

    private void draw_scan() {
        if (_fn_scan == null) return;

        float* s = stackalloc float[6 * 6];
        int n = _fn_scan(s);

        ImGui.Separator();
        ImGui.Text($"Scanner: {n} view-shaped block(s)");

        if (n == 0) {
            ImGui.TextWrapped("No rotation-plus-translation block found in any written "
                            + "constant buffer. The camera may not reach the GPU as a "
                            + "view matrix at all.");
            return;
        }

        for (int i = 0; i < n; i++) {
            float* h   = s + i * 6;
            string via = ((int)h[2]) switch {
                0 => "CreateBuffer (area load)",
                1 => "Map (per frame)",
                3 => "latched buffer, read back",
                _ => "UpdateSubresource (per frame)",
            };
            ImGui.Text($"  +{(int)h[0],-5} in {(int)h[1],6}B  {h[3],9:F2} {h[4],9:F2} {h[5],9:F2}  {via}");
        }
    }

    public override void render_imgui() {
        _frames++;

        try_install_hooks();

        // Marks the frame boundary for the native helper. Fahrenheit calls
        // render_imgui from inside Present, so this fires exactly once a frame.
        if (_fn_begin_frame != null) _fn_begin_frame();

        if (!_window_open) return;

        if (ImGui.Begin("FFX VR", ref _window_open)) {
            ImGui.Text($"Frames drawn:  {_frames}");
            ImGui.Text($"Framerate:     {ImGui.GetIO().Framerate:F1} fps");

            ImGui.Separator();
            ImGui.Text($"Native helper: {_native_status}");

            if (_game_context != 0) {
                ImGui.Text($"Game context:  0x{_game_context:X8}");
            }

            if (_fn_buffer_count != null && _native_init_done) {
                int calls = _fn_hook_calls != null ? _fn_hook_calls() : -1;

                ImGui.Text($"Hooked calls seen:     {calls}");
                ImGui.Text($"Constant buffers seen: {_fn_buffer_count()}");
                ImGui.Text($"Dumps written:         {_fn_dump_count()}");
                if (_fn_frame_writes != null)
                    ImGui.Text($"Writes in last frame:  {_fn_frame_writes()}");

                ImGui.Text($"Draw detours:          {_detour_status}");
                if (_fn_counter != null) {
                    ImGui.Text($"Draws: indexed {_fn_counter(1)}, plain {_fn_counter(2)}, "
                             + $"indexed-inst {_fn_counter(3)}, inst {_fn_counter(4)}   UpdateSubresource {_fn_counter(7)}");
                }
                draw_diff();
                ImGui.Separator();

                draw_camera();
                draw_override();
                draw_scan();

                if (calls == 0) {
                    ImGui.TextWrapped("No hooked call has fired yet. If this stays at zero "
                                    + "while the game renders, the patch is not on the "
                                    + "context the game draws with.");
                }
                else {
                    ImGui.TextWrapped("Walk around and watch the numbers. A line that "
                                    + "tracks you is the live camera; one that only "
                                    + "changes on area load is a buffer filled at load "
                                    + "time. F9 still dumps every constant buffer.");
                }
            }
        }

        ImGui.End();
    }
}
