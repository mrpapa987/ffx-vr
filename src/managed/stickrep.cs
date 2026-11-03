// SPDX-License-Identifier: MIT

namespace FFXVR;

// Stick reprojection (design-notes "Stick reprojection: extra frames at the headset rate (branch `stickrep`)"; research
// stick-reproject.md). The game draws about 60 frames a second, the headset refreshes faster; a stick turn used to step at
// the game's rate and show twice at 120 Hz. The native helper now fills every display slot that has no new game frame with
// the last one, turned by the stick yaw added since it was rendered (ffxvr_native.cpp, the "Stick reprojection" block); the
// same extra frames keep the headset fed while the game loads. This side owns the settings, the overlay rows, and the two
// main-thread hooks of the flush gate.
//
//   stick_reproject (ini and overlay, at once): 0 off (one frame per Present, the stick on the Present clock, as before);
//     1 first person and the seat (default); 2 also third person and the diorama (approximate: their pivot is the player).
//   stick_reproject_margin_ms (0.5-6, 2.5), stick_reproject_predict (1), stick_overscan (0-4 deg, 0): Debug.
//   keepalive (0): the extra frames only during stalls, unturned (with stick_reproject=0). keepalive_mt_protect (1): the
//     immediate context's multithread protection, applied once per game session at the first headset start
//     (never turned off again; a change needs a restart).
//
// The flush gate (spec 3.5): a full flush 0x615EA0 hands the immediate context to the game's main thread until the next kick
// 0x615210; native makes no extra frame in between. Both functions run on the main thread; the detours only call the
// original and one native store (ffxvr_sr_gate). They are queued at launch when stick_reproject or keepalive is on, LAST of
// all our hooks (Fahrenheit's commit stops at the first failing insertion), after their prologues were checked against
// FFX.exe with guarded reads. The originals are resolved as build 7's (B7-1): on the render thread before our late D3D
// insertions (sr_resolve), or by the detour itself on the main thread before the first Present (d7_resolve_main).
public sealed unsafe partial class FFXVRModule {

    private int   _stick_reproject = 1;                // ini stick_reproject: 0 off, 1 first person + seat (default), 2 also third person + diorama
    private int   _sr_ui_mode = 1;                     // the overlay's last on-mode (the checkbox restores it)
    private float _sr_margin_ms = 2.5f;                // ini stick_reproject_margin_ms
    private bool  _sr_predict = true;                  // ini stick_reproject_predict
    private float _stick_overscan = 0f;                // ini stick_overscan (degrees per side, horizontal)
    private bool  _keepalive = false;                  // ini keepalive: the loading keepalive alone (meaningful with stick_reproject=0)
    private bool  _keepalive_mt = false;               // ini keepalive_mt_protect: SR-X2, no longer turns protection on (it re-routed the context's entry points past the detours: the mod went blind); read for compatibility, always written 0
    private bool  _sr_inject = false;                  // Debug, never saved: every extra frame turned 5 deg right (the direction test)
    private volatile bool _sr_stall_req;               // Debug: the main thread sleeps 3 s in the job join (the keepalive test)
    private int   _turn_key_dir;                       // Numpad 1 / 3 this frame: -1 / 0 / +1 (handle_hotkeys; ffxvr_turn_keys)
    private delegate* unmanaged[Cdecl]<int, float, int, float, int, int, int, float, void> _fn_sr_set;   // ABI 25
    private delegate* unmanaged[Cdecl]<int, void> _fn_sr_gate;                                          // ABI 25 (the main-thread hooks)
    private delegate* unmanaged[Cdecl]<byte*, int, int> _fn_sr_status;                                  // ABI 25
    private delegate* unmanaged[Cdecl]<int, void> _fn_turn_keys;                                        // ABI 25

    // -- the flush gate hooks (FhUtil offsets; the exe is DYNAMIC_BASE) --------------------------------------------------
    private const int ksr_flush = 0x215EA0;   // VA 0x615EA0, full flush: thiscall-like (ecx = the ring), no stack arguments, plain ret; returns 0
    private const int ksr_kick  = 0x215210;   // VA 0x615210, kick: thiscall(ring, list, end, sem), ret 0xC; returns 0 (only caller: the context method 0x5B6C50)
    private static readonly byte[] ksr_flush_bytes = { 0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x4C, 0xA1 };                  // push ebp; mov ebp,esp; sub esp,0x4C; mov eax,[...]
    private static readonly byte[] ksr_kick_bytes  = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0xF1, 0x8D, 0x8E, 0x5C, 0x01 };  // push ebp; mov ebp,esp; push esi; mov esi,ecx; lea ecx,[esi+0x15C]
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] private delegate int dsr_flush(nint ring);
    [UnmanagedFunctionPointer(CallingConvention.ThisCall)] private delegate int dsr_kick(nint ring, nint list, nint end, nint sem);
    private dsr_flush? _hsr_flush, _osr_flush;
    private dsr_kick?  _hsr_kick,  _osr_kick;
    private nint _asr_flush, _asr_kick;
    private bool _sr_queued, _sr_hooks_ok, _sr_resolved_rt;
    private string _sr_status = "not set up";

    /// <summary>At init, after every other hook of ours is queued (the last insertions: a failure here takes nothing with it).</summary>
    private void sr_init() {
        if (_sr_queued) return;
        _sr_queued = true;
        if (_stick_reproject == 0 && !_keepalive) {
            _sr_status = "flush gate hooks not installed (stick_reproject=0 and keepalive=0 at launch; they are installed at the next launch with either on)";
            _logger.Info("stick reprojection: " + _sr_status); return;
        }
        if (!_native_ok || _fn_safe_read == null || _fn_sr_gate == null) { _sr_status = "off (the native helper did not load)"; _logger.Info("stick reprojection: " + _sr_status); return; }
        new FhCallLocation("FFX.exe", ksr_flush).try_resolve(out _asr_flush);
        new FhCallLocation("FFX.exe", ksr_kick).try_resolve(out _asr_kick);
        // The prologues, against FFX.exe (guarded reads: Fahrenheit's stage-0 debugger makes a fault fatal). Another mod's hook
        // (or another game build) refuses both: the extra frames then stay off, which is the old behaviour.
        bool pf = sr_prologue(_asr_flush, ksr_flush_bytes), pk = sr_prologue(_asr_kick, ksr_kick_bytes);
        if (!pf || !pk) {
            _sr_status = $"flush gate hooks NOT queued: the prologue of {(!pf ? "the full flush 0x615EA0" : "")}{(!pf && !pk ? " and " : "")}{(!pk ? "the kick 0x615210" : "")} differs from FFX.exe's (another mod's hook, or another game build); the extra frames stay off";
            _logger.Error("stick reprojection: " + _sr_status); return;
        }
        _hsr_flush = hsr_flush; _hsr_kick = hsr_kick;
        int ok = 0;
        string one(string name, Func<bool> f) {
            try { bool r = f(); if (r) ok++; _logger.Info($"stick reprojection: hook {name} {(r ? "queued" : "REFUSED")}"); return name + (r ? " queued" : " REFUSED"); }
            catch (Exception e) { _logger.Error($"stick reprojection: hook {name}: {e}"); return name + " FAILED (" + e.GetType().Name + ")"; }
        }
        string a = one("full flush 0x615EA0", () => new FhCallHandle<dsr_flush>(new FhCallLocation((void*)_asr_flush)).hook(this, _hsr_flush!));
        string b = one("kick 0x615210", () => new FhCallHandle<dsr_kick>(new FhCallLocation((void*)_asr_kick)).hook(this, _hsr_kick!));
        _sr_hooks_ok = ok == 2;
        _sr_status = $"flush gate hooks: {a}, {b} (queued last of all; native waits for the kick hook's first call)";
    }
    private bool sr_prologue(nint addr, byte[] want) {
        if (addr == 0 || _fn_safe_read == null || want.Length > 16) return false;
        byte* b = stackalloc byte[16];
        if (_fn_safe_read((void*)addr, b, (uint)want.Length) == 0) return false;
        for (int i = 0; i < want.Length; i++) if (b[i] != want[i]) return false;
        return true;
    }
    /// <summary>Render thread, with diag7_resolve / ld_resolve before try_install_hooks: both originals, once (B7-1).</summary>
    private void sr_resolve() {
        if (!_sr_queued || _sr_resolved_rt) return;
        _sr_resolved_rt = true;
        try {
            if (_hsr_flush != null && _osr_flush == null) System.Threading.Volatile.Write(ref _osr_flush, d7_chain(_asr_flush, _hsr_flush));
            if (_hsr_kick  != null && _osr_kick  == null) System.Threading.Volatile.Write(ref _osr_kick,  d7_chain(_asr_kick,  _hsr_kick));
        }
        catch (Exception e) { _logger.Error($"stick reprojection: resolving the originals: {e.Message}"); }   // the detours then resolve on their own thread
    }
    // -- the detours (main thread): the original always runs; one native store each, nothing that can throw ------------------
    private int hsr_flush(nint ring) {
        dsr_flush o = System.Threading.Volatile.Read(ref _osr_flush) ?? d7_resolve_main(ref _osr_flush, _asr_flush, _hsr_flush!);
        var g = _fn_sr_gate; if (g != null) g(1);   // before the original: its own list (0x2C) runs only after an extra frame in progress returned
        return o(ring);
    }
    private int hsr_kick(nint ring, nint list, nint end, nint sem) {
        dsr_kick o = System.Threading.Volatile.Read(ref _osr_kick) ?? d7_resolve_main(ref _osr_kick, _asr_kick, _hsr_kick!);
        int r = o(ring, list, end, sem);
        var g = _fn_sr_gate; if (g != null) g(2);   // after: the main thread handed the context back with this list
        return r;
    }

    /// <summary>From push_all, every frame.</summary>
    private void sr_push() {
        if (_fn_sr_set != null) _fn_sr_set(_stick_reproject, _sr_margin_ms, _sr_predict ? 1 : 0, _stick_overscan, _keepalive ? 1 : 0, _keepalive_mt ? 1 : 0, _sr_hooks_ok ? 1 : 0, _sr_inject ? 5f : 0f);
        if (_fn_turn_keys != null) _fn_turn_keys(_turn_key_dir);
    }
    private string sr_status_text(out bool on) {
        on = false;
        var f = _fn_sr_status; if (f == null) return "";
        byte* b = stackalloc byte[512];
        on = f(b, 512) != 0;
        return Marshal.PtrToStringAnsi((nint)b) ?? "";
    }

    /// <summary>The main window's row, under the turn speed.</summary>
    private void sr_ui() {
        bool on = _stick_reproject != 0;
        if (on) _sr_ui_mode = _stick_reproject;
        if (ImGui.Checkbox("smooth stick turns at the headset rate##sr", ref on)) _stick_reproject = on ? (_sr_ui_mode == 2 ? 2 : 1) : 0;
        if (_stick_reproject != 0) {
            ImGui.SameLine(); ImGui.RadioButton("first person + seat##sr", ref _stick_reproject, 1);
            ImGui.SameLine(); ImGui.RadioButton("all views (third person, diorama: approximate)##sr", ref _stick_reproject, 2);
        }
        if (_xr_want) { string s = sr_status_text(out _); if (s.Length > 0) ImGui.TextDisabled("  " + esc(s)); }
    }

    /// <summary>Debug > Stick reprojection and the loading keepalive.</summary>
    private void sr_debug_ui() {
        ImGui.TextWrapped("Between two game frames the headset shows the last frame again, turned by the stick yaw added since it was drawn "
                        + "(first person and the seat; with 'all views' also third person and the diorama, where the turn's pivot is the player and "
                        + "the result is approximate). The same extra frames keep the headset fed while the game loads (the stick stops a few degrees "
                        + "past the held frame). Screens, the stereo screen, the save screen and the glass break are held unturned. Test with Virtual "
                        + "Desktop at 120 Hz and SSW Disabled: the sr: line in ffxvr_diag.log has the numbers (once per headset session, and every 10 s with "
                        + "the verbose log, diag_verbose, at the top of Debug).");
        ImGui.Text("stick_reproject:"); ImGui.SameLine();
        ImGui.RadioButton("off##srd", ref _stick_reproject, 0); ImGui.SameLine();
        ImGui.RadioButton("first person + seat (default)##srd", ref _stick_reproject, 1); ImGui.SameLine();
        ImGui.RadioButton("all views##srd", ref _stick_reproject, 2);
        ImGui.SetNextItemWidth(160f); ImGui.SliderFloat("margin before the slot's deadline (stick_reproject_margin_ms)##srd", ref _sr_margin_ms, 0.5f, 6f, "%.1f ms");
        ImGui.Checkbox("an extra frame first when a game frame cannot make the open slot (stick_reproject_predict)##srd", ref _sr_predict);
        ImGui.SetNextItemWidth(160f); ImGui.SliderFloat("wider rendered view per side (stick_overscan)##srd", ref _stick_overscan, 0f, 4f, "%.1f deg");
        ImGui.Checkbox("loading keepalive only, unturned (keepalive; with stick_reproject off)##srd", ref _keepalive); ImGui.SameLine();
        ImGui.TextDisabled("context multithread protection: never turned on (SR-X2: it hid the game's draws from the mod); the flush gate is the protection");
        ImGui.Checkbox("direction test: every extra frame turned 5 deg right (not saved)##srd", ref _sr_inject);
        ImGui.SameLine(); ImGui.TextDisabled("the held frames should jump the way a right push turns the view");
        if (ImGui.Button("stall the main thread 3 s##srd")) _sr_stall_req = true;
        ImGui.SameLine(); ImGui.TextDisabled(_h7_join_ok ? "the headset should keep a head-tracked frame, then the game resumes" : "needs the job join hook (smooth_anchor=1 at launch)");
        string s = sr_status_text(out _);
        if (s.Length > 0) ImGui.TextDisabled(esc(s));
        ImGui.TextDisabled(esc(_sr_status));
    }
}
