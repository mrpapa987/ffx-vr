// SPDX-License-Identifier: MIT

namespace FFXVR;

// Battle transition, the "glass break" (design-notes "Battle transition (glass break)"; research battle-transition.md).
// Everything that decides runs in the native helper (ffxvr_native.cpp bt_tick, the capture / shard detection in
// ffxvr_on_draw, the pin in ffxvr_xr.cpp): memory polls at Present plus D3D-level detection, no game-code hook. The managed
// side only owns the setting, the diagnostics switch and the overlay line.
//
//   battle_transition (ini, applies at once): 2 = frozen world (default): no UI split during the transition, the view stays
//     in the mode it started in, the head pose is frozen and the layer is submitted with the poses of the captured frame, so
//     the field breaks apart world-locked where it was; after the shards the pinned last frame is held (no 30-frame cap)
//     over the game's load black until the battle is live. 1 = head-locked: no UI split and the mode held only (the glass
//     shows, following the head). 0 = the old behaviour (the native phases still run for the bt: lines).
//   diag_transition (ini, default 1 in this test build): one "bt:" line per Present inside a transition window in
//     ffxvr_diag.log (capped at 900 a window, 6000 a session), plus the phase changes and one summary per window (those
//     are always logged).
public sealed unsafe partial class FFXVRModule {

    private int  _battle_transition = 2;       // ini battle_transition: 0 old, 1 head-locked, 2 frozen world
    private bool _diag_transition = true;      // ini diag_transition: the per-frame bt: lines
    private delegate* unmanaged[Cdecl]<int, int, void> _fn_bt_set;          // (mode | 0x100 on a user view action, diag); ABI 16
    private bool _bt_user_ev;                  // a user view action this frame (hotkeys, gestures, overlay): an open transition window ends at once
    private static readonly ImGuiKey[] k_bt_view_keys = {
        ImGuiKey.Home, ImGuiKey.End, ImGuiKey.Insert, ImGuiKey.Keypad1, ImGuiKey.Keypad2, ImGuiKey.Keypad3, ImGuiKey.Keypad4, ImGuiKey.Keypad5,
        ImGuiKey.Keypad6, ImGuiKey.Keypad7, ImGuiKey.Keypad8, ImGuiKey.Keypad9, ImGuiKey.KeypadAdd, ImGuiKey.KeypadSubtract };
    private delegate* unmanaged[Cdecl]<byte*, int, int> _fn_bt_status;      // (buf, n) -> phase: 0 idle, 1 armed, 2 shatter, 3 after

    /// <summary>ini battle_transition: 0 / 1 / 2, or off (old) / headlocked / world.</summary>
    private void bt_parse_mode(string k, string v) {
        string t = v.Trim().ToLowerInvariant();
        switch (t) {
            case "0": case "off": case "old": _battle_transition = 0; break;
            case "1": case "headlocked": case "head-locked": case "head_locked": _battle_transition = 1; break;
            case "2": case "world": case "frozen": case "frozen-world": case "frozen_world": _battle_transition = 2; break;
            default: _ini_bad.Add($"{k}='{v}'"); break;
        }
    }

    /// <summary>From handle_hotkeys (after its text-input check): any view hotkey held or pressed counts as a user view action.
    /// The player must never be locked out of the views by a transition window.</summary>
    private void bt_user_keys() {
        foreach (ImGuiKey k in k_bt_view_keys) if (ImGui.IsKeyDown(k)) { _bt_user_ev = true; return; }
    }

    /// <summary>From push_all, every frame. An overlay widget in use counts as a user action too (any change, view or not).
    /// The battle's own automatic view switch (battle_view) is not one: it does not set the flag.</summary>
    private void bt_push() {
        if (_window_open && Convert.ToBoolean(ImGui.IsAnyItemActive())) _bt_user_ev = true;   // Convert: builds whether Hexa exposes bool or byte
        if (_fn_bt_set != null) _fn_bt_set(_battle_transition | (_bt_user_ev ? 0x100 : 0), _diag_transition ? 1 : 0);
        _bt_user_ev = false;
    }

    /// <summary>A transition window is open (render thread: the native phase is kept on the same thread).</summary>
    private bool bt_active() {
        var f = _fn_bt_status; return f != null && f(null, 0) != 0;
    }

    private string bt_status_text() {
        var f = _fn_bt_status; if (f == null) return "";
        byte* buf = stackalloc byte[1280];   // the native summary is up to 1 KB
        buf[0] = 0; f(buf, 1280);
        return Marshal.PtrToStringAnsi((nint)buf) ?? "";
    }

    /// <summary>The main window's row (next to the UI panel's).</summary>
    private void bt_ui() {
        ImGui.Text("battle start (glass break):"); ImGui.SameLine();
        ImGui.RadioButton("frozen world##bt", ref _battle_transition, 2); ImGui.SameLine();
        ImGui.RadioButton("head-locked##bt", ref _battle_transition, 1); ImGui.SameLine();
        ImGui.RadioButton("old (as before)##bt", ref _battle_transition, 0);
        if (_fn_bt_status != null) ImGui.TextDisabled("  transition: " + bt_status_text());
    }

    /// <summary>Debug > Battle loading: the diagnostics switch.</summary>
    private void bt_debug_ui() {
        ImGui.Checkbox("battle transition lines (diag_transition): one \"bt:\" line per frame inside a battle transition window in ffxvr_diag.log (capped)", ref _diag_transition);
    }
}
