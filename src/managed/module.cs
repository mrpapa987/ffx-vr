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
                ImGui.Separator();

                draw_camera();

                if (calls == 0) {
                    ImGui.TextWrapped("No hooked call has fired yet. If this stays at zero "
                                    + "while the game renders, the patch is not on the "
                                    + "context the game draws with.");
                }
                else {
                    ImGui.TextWrapped("Press F9 to dump every constant buffer. Take one "
                                    + "dump standing still, move somewhere else, take "
                                    + "another, then diff them to find the camera.");
                }
            }
        }

        ImGui.End();
    }
}
