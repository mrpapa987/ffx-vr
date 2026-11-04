// SPDX-License-Identifier: MIT

namespace FFXVR;

// The slow-GPU guard (design-notes "Slow-GPU guard and overdrive freeze diagnostics"; research overdrive-freeze.md). At Tidus's
// Slice & Dice the headset view fell to about 15 fps for good: the GPU span went from ~12 to ~60 ms and xrEndFrame to ~55 ms,
// on the flat screen too. The native side (ffxvr_native.cpp, "slow-GPU guard"; the decisions in ffxvr_slow.h) watches every
// Present for that state and, when it lasts ~120 frames against a learnt baseline, logs it (slow: lines), arms a frame census,
// switches the optional work off one item at a time for ~2.5 s (ui_capture, ui_depth, vr_hands, screen_mono; never the ImGui
// layer, which carries this overlay) and, if two measured steps did not help and it is still measurably slow, restarts the
// VR session (at most once per 5 min, 3 a session). This side owns the two settings and the Debug row.
//
//   slow_guard (ini, default 1): the guard; 0 = it only keeps its frame ring and baselines (no step, no restart).
//   slow_guard_restart (ini, default 1): its last step, the VR session restart.
public sealed unsafe partial class FFXVRModule {

    private bool _slow_guard = true, _slow_guard_restart = true;          // ini slow_guard, slow_guard_restart
    private delegate* unmanaged[Cdecl]<int, int, void> _fn_slow_set;       // ABI 36
    private delegate* unmanaged[Cdecl]<int*, int, byte*> _fn_slow_status;  // ABI 36: (out, n) -> the Debug line (native, static)

    /// <summary>From push_all, every frame (render thread).</summary>
    private void slow_push() {
        if (_fn_slow_set != null) _fn_slow_set(_slow_guard ? 1 : 0, _slow_guard_restart ? 1 : 0);
    }

    /// <summary>Debug: the two settings and the guard's line (its state, the baselines, the last trigger and what fixed it).</summary>
    private void slow_debug_ui() {
        ImGui.Checkbox("slow-GPU guard (slow_guard)##slowg", ref _slow_guard); ImGui.SameLine();
        ImGui.Checkbox("may restart the VR session (slow_guard_restart)##slowr", ref _slow_guard_restart);
        if (_fn_slow_status == null) return;
        int* v = stackalloc int[6];
        byte* p = _fn_slow_status(v, 6);
        string line = p != null ? System.Runtime.InteropServices.Marshal.PtrToStringAnsi((nint)p) ?? "" : "";
        if (v[0] >= 2 && v[0] <= 9) { ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.8f, 0.3f, 1f)); ImGui.TextWrapped(esc(line)); ImGui.PopStyleColor(1); }   // an episode under way
        else ImGui.TextDisabled(esc(line));
    }
}
