// SPDX-License-Identifier: MIT
namespace FFXVR;
// The Quest controllers in Fahrenheit's save screen (ini imgui_vrpad; design-notes "Quest controllers only", 1). The save /
// load screen is Fahrenheit's ImGui UI, and ImGui's Win32 backend reads pad 0 through its own XInput DLL (the first of
// xinput1_4, 1_3, 9_1_0, 1_2, 1_1 that loads), not through the game's XINPUT9_1_0 import, so our game-side VR pad never
// reached it. Here: detour that DLL's XInputGetState and XInputGetCapabilities with Fahrenheit's own hooking (FhCallHandle,
// as the D3D11 detours in module.cs), call the original, then native merges the VR pad in (ffxvr_imgui_xstate / _xcaps:
// no Start or Back unless imgui_vrpad=2, no stick rotation, no tap, the buttons already held when the screen opens masked).
// Native posts one WM_DEVICECHANGE so the backend asks the capabilities again. The game itself gets nothing more: Fahrenheit
// locks its input while the save screen is open. Installed once, at the first frame with imgui_vrpad != 0; native refuses
// only when ImGui uses the game's own XInput DLL without our import hook (no re-entry guard), or, with imgui_vrpad_strict=1,
// whenever the game's import resolves into ImGui's DLL. imgui_vrpad=0 at launch: never installed (the escape hatch).
public sealed unsafe partial class FFXVRModule {
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate uint d_XInputGetState(uint idx, nint state);
    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate uint d_XInputGetCapabilities(uint idx, uint flags, nint caps);
    private d_XInputGetState? _h_im_state, _o_im_state;
    private d_XInputGetCapabilities? _h_im_caps, _o_im_caps;
    private nint _a_im_state, _a_im_caps, _im_hwnd;
    private bool _im_tried, _im_installed, _im_hwnd_tried;
    private string _im_status = "not installed yet (it installs at the first frame with this on)";
    private int  _im_real_absent_until;   // Environment.TickCount (32-bit, wraps; Volatile): ImGui's pad 0 did not answer; ImGui's reads skip it for a second
    private int  _im_render_tid = -1;     // the render thread's managed id (the install runs there; ImGui's reads come from it): only its calls are throttled
    private int  _im_faults;              // exceptions caught in the two detours (never thrown into the native caller)
    private const uint k_xi_not_connected = 1167;   // ERROR_DEVICE_NOT_CONNECTED

    private delegate* unmanaged[Cdecl]<nint*, nint*, int, int> _fn_imgui_pad_find;     // ABI 44
    private delegate* unmanaged[Cdecl]<int, uint, nint, int, void> _fn_imgui_pad_set;
    private delegate* unmanaged[Cdecl]<uint, nint, uint, uint> _fn_imgui_xstate;
    private delegate* unmanaged[Cdecl]<uint, nint, uint, uint> _fn_imgui_xcaps;

    private void imgui_pad_bind(Func<string, nint> req) {
        _fn_imgui_pad_find = (delegate* unmanaged[Cdecl]<nint*, nint*, int, int>)req("ffxvr_imgui_pad_find");
        _fn_imgui_pad_set = (delegate* unmanaged[Cdecl]<int, uint, nint, int, void>)req("ffxvr_imgui_pad_set");
        _fn_imgui_xstate = (delegate* unmanaged[Cdecl]<uint, nint, uint, uint>)req("ffxvr_imgui_xstate");
        _fn_imgui_xcaps = (delegate* unmanaged[Cdecl]<uint, nint, uint, uint>)req("ffxvr_imgui_xcaps");
    }
    // The detours stay in once installed (a hook cannot be taken out); with the pointers cleared they only call the originals.
    private void imgui_pad_clear() { _fn_imgui_pad_find = null; _fn_imgui_pad_set = null; _fn_imgui_xstate = null; _fn_imgui_xcaps = null; }

    /// <summary>Render thread, push_all (inside Present, after Fahrenheit's platform_bind ran ImGui's Init, so its DLL is
    /// loaded and its pointers taken). Once; the late insertions are bracketed like install_draw_detours'.</summary>
    private void imgui_pad_install() {
        if (_im_tried || _imgui_vrpad == 0 || !_native_ready || _fn_imgui_pad_find == null) return;
        _im_tried = true;
        nint gs = 0, gc = 0;
        _im_render_tid = Environment.CurrentManagedThreadId;
        int rc = _fn_imgui_pad_find(&gs, &gc, _imgui_vrpad_strict ? 1 : 0);   // native logs the DLL and the functions' first bytes, or why not
        if (rc != 0 || gs == 0 || gc == 0) {
            _im_status = rc == 2 ? "refused: ImGui reads the game's own XInput DLL (ffxvr_diag.log; imgui_vrpad_strict)" : "not installed (ffxvr_diag.log says why)";
            _logger.Info("imgui pad: " + _im_status);
            return;
        }
        _a_im_state = gs; _a_im_caps = gc;
        _h_im_state = h_im_state; _h_im_caps = h_im_caps;
        bool ok_s = false, ok_c = false;
        diag7_insert_begin();   // no main-thread detour resolves its original while this writes Fahrenheit's table (B7-1)
        try {
            ok_s = new FhCallHandle<d_XInputGetState>(new FhCallLocation((void*)_a_im_state)).hook(this, _h_im_state);
            ok_c = new FhCallHandle<d_XInputGetCapabilities>(new FhCallLocation((void*)_a_im_caps)).hook(this, _h_im_caps);
        }
        catch (Exception e) { _logger.Error($"imgui pad: detours: {e}"); }
        finally { diag7_insert_end(); }
        _im_installed = ok_s && ok_c;   // both, or no device-change post (the state detour alone still merges for a connected Xbox pad)
        _im_status = _im_installed ? "detoured" : $"detours FAILED (XInputGetState {(ok_s ? "ok" : "refused")}, XInputGetCapabilities {(ok_c ? "ok" : "refused")}; core.log)";
        string m = I($"imgui pad: {_im_status}: XInputGetState 0x{_a_im_state:X8}, XInputGetCapabilities 0x{_a_im_caps:X8}");
        _logger.Info(m); d7_log(m);
    }
    private d_XInputGetState? im_orig_state() {
        d_XInputGetState? o = System.Threading.Volatile.Read(ref _o_im_state);
        if (o == null && _h_im_state != null) { o = new FhCallHandle<d_XInputGetState>(new FhCallLocation((void*)_a_im_state)).chain_from(_h_im_state).fnptr; System.Threading.Volatile.Write(ref _o_im_state, o); }
        return o;
    }
    private d_XInputGetCapabilities? im_orig_caps() {
        d_XInputGetCapabilities? o = System.Threading.Volatile.Read(ref _o_im_caps);
        if (o == null && _h_im_caps != null) { o = new FhCallHandle<d_XInputGetCapabilities>(new FhCallLocation((void*)_a_im_caps)).chain_from(_h_im_caps).fnptr; System.Threading.Volatile.Write(ref _o_im_caps, o); }
        return o;
    }
    /// <summary>XInputGetState of ImGui's DLL: ImGui's backend (its NewFrame, the render thread) and, when the game's import
    /// resolves into the same DLL, the game's own polls (its input thread; native passes those through untouched). Never
    /// throws into the caller.</summary>
    private uint h_im_state(uint idx, nint st) {
        uint r = k_xi_not_connected;
        try {
            d_XInputGetState? o = im_orig_state();
            // Once ImGui believes in a pad (ours) it asks every frame; a disconnected slot's query can be slow (the backend's own
            // comment), so after a failure ImGui's reads of pad 0 skip the real call for a second (a plugged-in pad shows within
            // that). Only ImGui's: every other thread's call (the game's) always goes to the real XInput.
            bool imgui = idx == 0 && Environment.CurrentManagedThreadId == _im_render_tid;
            int now = Environment.TickCount;
            int left = unchecked(System.Threading.Volatile.Read(ref _im_real_absent_until) - now);   // wrap-safe: skip only inside the second
            bool skip = imgui && left > 0 && left <= 1000;
            if (o != null && !skip) {
                r = o(idx, st);
                if (imgui && r != 0) System.Threading.Volatile.Write(ref _im_real_absent_until, unchecked(now + 1000));
            }
            if (_fn_imgui_xstate != null) r = _fn_imgui_xstate(idx, st, r);
        }
        catch { _im_faults++; }
        return r;
    }
    /// <summary>ImGui's XInputGetCapabilities (after a WM_DEVICECHANGE). Never throws into the caller.</summary>
    private uint h_im_caps(uint idx, uint flags, nint caps) {
        uint r = k_xi_not_connected;
        try {
            d_XInputGetCapabilities? o = im_orig_caps();
            if (o != null) r = o(idx, flags, caps);
            if (idx == 0 && r == 0) System.Threading.Volatile.Write(ref _im_real_absent_until, Environment.TickCount);   // a real pad: its state is asked again at once
            if (_fn_imgui_xcaps != null) r = _fn_imgui_xcaps(idx, caps, r);
        }
        catch { _im_faults++; }
        return r;
    }
    /// <summary>Every frame (qc_push): install on the first frame it is wanted, then the mode, the save screen's state and
    /// ImGui's window to native (which posts the device change and logs the feed's open / close).</summary>
    private void imgui_pad_push() {
        imgui_pad_install();
        if (_fn_imgui_pad_set == null) return;
        if (_im_installed && !_im_hwnd_tried) {
            _im_hwnd_tried = true;
            try { _im_hwnd = (nint)ImGui.GetMainViewport().PlatformHandleRaw; } catch { _im_hwnd = 0; }   // the backend's window; 0: native looks for the game's
        }
        uint save = _save_st != 0 ? _save_st : (_fh_menu_open ? 1u : 0u);
        _fn_imgui_pad_set(_imgui_vrpad, save, _im_hwnd, _im_installed ? 1 : 0);
    }
    private string imgui_pad_status() {
        string n = qc_text(2);
        string f = _im_faults > 0 ? I($" | {_im_faults} faults caught in the detours") : "";
        return _imgui_vrpad == 0 && !_im_tried ? "save screen: off (imgui_vrpad=0: not installed)" : $"save screen: {_im_status}{f}{(n.Length > 0 ? " | " + n : "")}";
    }
}
