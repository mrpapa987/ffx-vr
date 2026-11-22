// SPDX-License-Identifier: MIT

namespace FFXVR;

// perf-gpu (22 Nov 2026, ABI 73; design-notes "GPU phase timers, the eye-2 test and the busy-GPU gate"; the native side: the
// block "perf-gpu" in ffxvr_native.cpp, the rules in ffxvr_perf.h). This side owns one setting and one Developer button:
//
//   sr_skip_busy (ini, Developer tab; 0 off, 1 log only, 2 on; default 1 in this test build): before a stick-reprojection extra
//     frame the native side asks, without waiting, whether the GPU has finished the last fresh frame. 1 sends every extra and
//     splits the sr: line's counts and xrEndFrame times by the answer; 2 skips the slot while the GPU is busy (the runtime then
//     repeats the last frame there, as before stick reprojection) instead of blocking in xrEndFrame behind the game's queue.
//   "Measure: eye 2 off for 5 s" (Developer > Rendering, not saved): 3 s with eye 2, 5 s without its re-issue (both eyes see
//     eye 1: the view goes flat), 3 s with; then a perf: line in ffxvr_diag.log with the GPU span (and, with the verbose log,
//     every GPU phase) with and without eye 2. Eye 2 comes back by itself; the 3D headset view only.
//
// The GPU phase timers on the perf: line need nothing from here: they run while diag_verbose (developer mode) is on.
public sealed unsafe partial class FFXVRModule {

    private int  _sr_skip_busy = 1;           // ini sr_skip_busy (= native g_srb_mode's default)
    private bool _pt_req;                     // the Developer button was pressed: sent with the next push, once
    private int  _pt_state;                   // the eye-2 test's state as native returned it (0 idle, 1 before, 2 eye 2 off, 3 after, 4 collecting)
    private delegate* unmanaged[Cdecl]<int, int, int> _fn_perf_set;      // ABI 73
    private delegate* unmanaged[Cdecl]<byte*, int, int> _fn_perf_text;   // ABI 73

    private bool perf_parse(string k, string v) {
        switch (k) {
            case "sr_skip_busy": pe(k, v, ref _sr_skip_busy, 0, 1, 2); return true;   // perf-gpu: the busy-GPU gate for the extra frames (at once)
            default: return false;
        }
    }
    private string perf_ini_text() => $"sr_skip_busy={_sr_skip_busy.ToString(CI)}\n";
    private void perf_bind(Func<string, nint> req) {
        _fn_perf_set = (delegate* unmanaged[Cdecl]<int, int, int>)req("ffxvr_perf_set");
        _fn_perf_text = (delegate* unmanaged[Cdecl]<byte*, int, int>)req("ffxvr_perf_text");
    }
    private void perf_unbind() { _fn_perf_set = null; _fn_perf_text = null; }

    /// <summary>From push_all, every frame (render thread): the setting, and the button's press once.</summary>
    private void perf_push() {
        if (_fn_perf_set == null) return;
        _pt_state = _fn_perf_set(_sr_skip_busy, _pt_req ? 1 : 0);
        _pt_req = false;
    }
    private string perf_text() {
        var f = _fn_perf_text; if (f == null) return "";
        byte* b = stackalloc byte[1536];
        f(b, 1536);
        return Marshal.PtrToStringAnsi((nint)b) ?? "";
    }

    private const string k_srb_tip = "Before an extra frame (smooth stick turning at the headset rate) the mod asks, without waiting, whether the GPU has finished the last game frame. "
                                   + "Virtual Desktop's runtime queues every frame behind the game's GPU work, so an extra sent while the GPU is busy blocks the render thread for several ms. "
                                   + "0 off; 1 log only (the sr: line counts the extras the rule would skip and their xrEndFrame time); 2 skip that slot (the runtime repeats the last frame there).";
    /// <summary>Developer > Rendering > GPU timing and the busy-GPU gate.</summary>
    private void perf_dev_ui() {
        ImGui.TextWrapped("GPU phases: with the verbose log on, every perf: line in ffxvr_diag.log (every 30 s) ends with the GPU time per phase "
                        + "(shadows, scene, post, UI, the mod's work after Present, the whole frame, the gap to the next frame), measured with timestamp queries.");
        ImGui.TextDisabled("busy-GPU gate for the extra frames (sr_skip_busy):");
        ImGui.SameLine(); ui_radio("sr_skip_busy", "off##srb", ref _sr_skip_busy, 0, UiTag.Dev, k_srb_tip);
        ImGui.SameLine(); ui_radio("sr_skip_busy", "log only##srb", ref _sr_skip_busy, 1, UiTag.Dev, k_srb_tip);
        ImGui.SameLine(); ui_radio("sr_skip_busy", "skip a busy slot##srb", ref _sr_skip_busy, 2, UiTag.Dev, k_srb_tip);
        if (_pt_state == 0) {
            if (ImGui.Button("Measure: eye 2 off for 5 s##pt")) _pt_req = true;
            ui_tip("3 s with eye 2, then 5 s with eye 2's re-issue skipped (both eyes see eye 1: the view goes flat), then 3 s with it again; "
                                         + "the result is a perf: line in ffxvr_diag.log. Eye 2 comes back by itself. In the 3D headset view only (not on a screen).");
        }
        else ImGui.TextDisabled(_pt_state == 2 ? "eye-2 test: eye 2 OFF (both eyes see eye 1)" : _pt_state == 1 ? "eye-2 test: measuring with eye 2 (before)" : _pt_state == 3 ? "eye-2 test: measuring with eye 2 (after)" : "eye-2 test: collecting the results");
        string s = perf_text();
        if (s.Length > 0) ImGui.TextWrapped(esc(s));
    }
}
