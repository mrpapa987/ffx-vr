// SPDX-License-Identifier: MIT

namespace FFXVR;

/// <summary>
///     Scaffold module for the FFX VR mod.
/// </summary>
/// <remarks>
///     At this stage it does nothing but prove two things: that our own code loads
///     inside the game, and that we can draw through Fahrenheit's ImGui overlay —
///     which runs inside the hooked <c>IDXGISwapChain::Present</c>, the same place
///     the VR renderer will eventually live.
///
///     The reported framerate is worth watching: the game's logic is locked to
///     30 Hz, and VR will need 72–90.
/// </remarks>
[FhLoad(FhGameId.FFX)]
public sealed class FFXVRModule : FhModule {

    private ulong _frames;
    private bool  _window_open = true;

    public FFXVRModule() { }

    public override bool init(FhModContext mod_context, FileStream global_state_file) {
        _logger.Info("FFX VR module initialized.");
        return true;
    }

    public override void render_imgui() {
        _frames++;

        if (!_window_open) return;

        if (ImGui.Begin("FFX VR", ref _window_open)) {
            ImGui.Text("Our code is running inside the game.");
            ImGui.Separator();
            ImGui.Text($"Frames drawn:  {_frames}");
            ImGui.Text($"Framerate:     {ImGui.GetIO().Framerate:F1} fps");
        }

        ImGui.End();
    }
}
