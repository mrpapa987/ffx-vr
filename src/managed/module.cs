// SPDX-License-Identifier: MIT

namespace FFXVR;

/// <summary>
///     Scaffold module for the FFX VR mod.
/// </summary>
/// <remarks>
///     Two jobs at this stage. It proves our code runs in-process and can draw
///     through Fahrenheit's ImGui overlay — which lives inside the hooked
///     <c>IDXGISwapChain::Present</c>, where the VR renderer will eventually go.
///     And it loads the native helper, which hooks D3D11 constant buffer uploads
///     so we can find the game's view and projection matrices.
///
///     The reported framerate is worth watching: game logic is locked to 30 Hz,
///     and VR will need 72–90.
/// </remarks>
[FhLoad(FhGameId.FFX)]
public sealed unsafe class FFXVRModule : FhModule {

    private const string NativeDll = "ffxvr_native.dll";

    private ulong _frames;
    private bool  _window_open = true;

    private nint   _native_handle;
    private string _native_status = "not loaded";

    private delegate* unmanaged[Cdecl]<int> _fn_buffer_count;
    private delegate* unmanaged[Cdecl]<int> _fn_dump_count;

    public FFXVRModule() { }

    public override bool init(FhModContext mod_context, FileStream global_state_file) {
        _logger.Info("FFX VR module initialized.");
        load_native(mod_context);
        return true;
    }

    /// <summary>
    ///     Loads the native helper by absolute path, rather than relying on the
    ///     default DLL search order finding it inside the mod folder.
    /// </summary>
    private void load_native(FhModContext mod_context) {
        string path = Path.Join(mod_context.Paths.ModDir.FullName, NativeDll);

        if (!File.Exists(path)) {
            _native_status = "missing: " + NativeDll;
            _logger.Warning($"Native helper not found at {path}. Constant buffer scanning is off.");
            return;
        }

        try {
            _native_handle = NativeLibrary.Load(path);

            var fn_init      = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_init");
            _fn_buffer_count = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_buffer_count");
            _fn_dump_count   = (delegate* unmanaged[Cdecl]<int>)NativeLibrary.GetExport(_native_handle, "ffxvr_dump_count");

            int rc = fn_init();

            _native_status = rc switch {
                 0 => "hooked",
                -1 => "failed: could not create a D3D11 device",
                -2 => "failed: could not patch the device context vtable",
                 _ => $"failed: code {rc}",
            };

            if (rc == 0) _logger.Info("Native helper loaded; D3D11 constant buffer hooks installed.");
            else         _logger.Error($"Native helper init failed with code {rc}.");
        }
        catch (Exception ex) {
            _native_status = "load threw: " + ex.GetType().Name;
            _logger.Error($"Failed to load the native helper: {ex}");
        }
    }

    public override void render_imgui() {
        _frames++;

        if (!_window_open) return;

        if (ImGui.Begin("FFX VR", ref _window_open)) {
            ImGui.Text($"Frames drawn:  {_frames}");
            ImGui.Text($"Framerate:     {ImGui.GetIO().Framerate:F1} fps");

            ImGui.Separator();
            ImGui.Text($"Native helper: {_native_status}");

            if (_fn_buffer_count != null) {
                ImGui.Text($"Constant buffers seen: {_fn_buffer_count()}");
                ImGui.Text($"Dumps written:         {_fn_dump_count()}");
                ImGui.Separator();
                ImGui.TextWrapped("Press F9 to dump every constant buffer. Take one "
                                + "dump standing still, move somewhere else, take "
                                + "another, then diff them to find the camera.");
            }
        }

        ImGui.End();
    }
}
