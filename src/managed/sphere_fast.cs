// SPDX-License-Identifier: MIT

namespace FFXVR;

// The fast sphere sequence, and the sphere in the palm through the sequence (branch sphere-fast, ABI 67; the rules native:
// ffxvr_sphere.h Fast / fast_answer / fast_exec and ffxvr_carry.h SeqHold, unit-tested; the glue "The fast sequence" in
// ffxvr_native.cpp). Both act only in first person, only during a grab sequence the mod itself started from a grip squeeze in
// a Cloister of Trials (sphere_hands.cs): never for the A button's interaction.
//
// Fast: while such a sequence runs with control off, the game's world runs sphere_fast_factor ticks per main-loop pass, so
// the take / place animation and the script's waits pass in a fraction of the time (the owner's log, 14 Nov: a take 4.1 s and
// a place 3.0 s from the squeeze to control, most of it the game's animation and waits).
//   The mechanism is the game's own "exec frames" count: Sg_MainLoop's update loop runs Sg_GetCurExecFrames() world ticks per
//   pass and builds one draw (ffx-rust docs/formats/frame-timing.md; 0x82D7E0, cdecl int()). This file detours that getter
//   with Fahrenheit's hooking and hands the game's own answer to native's ffxvr_sph_fast_exec, which turns an answer of
//   exactly 1 into the factor while native's per-frame rule says so, and passes everything else through.
//   Nothing is written to the game, so there is no value to restore: the game's byte (0xC496D4) is only read, for the log.
//   Why not write the byte: FFX-60fps sets it before every main-loop pass (1 on a logic pass, 0 on a draw-only pass; its
//   pacing.cpp), so a written value would be overwritten at once, or would turn draw-only passes into logic passes. An
//   answer of 0 is passed through, so FFX-60fps' pattern of passes stays as it is.
//   The detour is inserted late, on the render thread, the first time the feature is wanted in a cloister (never at module
//   init): FFX-60fps checks the getter's bytes when it starts (a required signature), and by then it has long done so.
// ini: sphere_fast 0/1 (default 1), sphere_fast_factor 1..12 (default 6; 1 = off), sphere_carry_seq 0/1 (default 1). Hands tab.
public sealed unsafe partial class FFXVRModule {

    private bool _sphere_fast = true;         // ini sphere_fast
    private int  _sphere_fast_factor = 6;     // ini sphere_fast_factor (= native sph::k_fast_def; 1..12 = sph::k_fast_min .. k_fast_max)
    private bool _sphere_carry_seq = true;    // ini sphere_carry_seq
    private delegate* unmanaged[Cdecl]<int, int, int, int, void> _fn_sph_fast_set;   // ABI 67
    private delegate* unmanaged[Cdecl]<int, int> _fn_sph_fast_exec;                  // ABI 67

    private const int k_sfast_get  = 0x42D7E0;   // VA 0x82D7E0 Sg_GetCurExecFrames: cdecl int(), plain ret
    private const int k_sfast_byte = 0x8496D4;   // VA 0xC496D4 the exec-frames byte (read only: the getter's operand check, the log)
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int d_sfast_get();
    private d_sfast_get? _h_sfast, _o_sfast; private nint _a_sfast;
    private int _sfast_hook;                     // = native sfast_hook_name: 0 not tried, 1 inserted, 2 the bytes differ, 3 refused / failed, 4 FFX.exe not found, 5 no native helper
    private string _sfast_status = "not tried yet (it is inserted the first time the feature is wanted in a cloister)";
    private long _sfast_calls, _sfast_faults;

    private bool sfast_parse(string k, string v) {
        switch (k) {
            case "sphere_fast":        pb(k, v, ref _sphere_fast); return true;
            case "sphere_fast_factor": pi(k, v, ref _sphere_fast_factor, 1, 12); return true;
            case "sphere_carry_seq":   pb(k, v, ref _sphere_carry_seq); return true;
            default: return false;
        }
    }
    private string sfast_ini_text() => $"sphere_fast={(_sphere_fast ? 1 : 0)}\nsphere_fast_factor={_sphere_fast_factor.ToString(CI)}\nsphere_carry_seq={(_sphere_carry_seq ? 1 : 0)}\n";
    private void sfast_bind(Func<string, nint> req) {
        _fn_sph_fast_set = (delegate* unmanaged[Cdecl]<int, int, int, int, void>)req("ffxvr_sph_fast_set");
        _fn_sph_fast_exec = (delegate* unmanaged[Cdecl]<int, int>)req("ffxvr_sph_fast_exec");
    }
    private void sfast_unbind() { _fn_sph_fast_set = null; _fn_sph_fast_exec = null; }

    /// From push_all, every frame, after carry_tick (render thread): the settings and the detour's state to native; the
    /// detour's insertion the first time the feature is wanted.
    private void sfast_push() {
        if (_fn_sph_fast_set == null) return;
        if (_sfast_hook == 0 && _sphere_fast && _sphere_fast_factor > 1 && sphh_wanted()) sfast_install();
        _fn_sph_fast_set(_sphere_fast ? 1 : 0, _sphere_fast_factor, _sphere_carry_seq ? 1 : 0, _sfast_hook);
    }

    /// Render thread, once (from sfast_push; a late insertion, bracketed like imgui_pad_install's): the detour on the game's
    /// exec-frames getter. Only when its first bytes read as that function (cmp byte [abs], 0; push esi; movsx esi, byte
    /// [abs]) and its third operand is the exec-frames byte: anything else (another mod's patch, another exe) is left alone.
    private void sfast_install() {
        if (_sfast_hook != 0) return;
        if (!_native_ready || _fn_sph_fast_exec == null) { _sfast_hook = 5; _sfast_status = "not inserted: the native helper did not load"; return; }
        byte b0 = 0, b1 = 0, b6 = 0, b7 = 0, b8 = 0, b9 = 0, b10 = 0, b17 = 0, b18 = 0, b19 = 0; uint op = 0, want = 1;
        try {
            b0 = FhUtil.get_at<byte>(k_sfast_get); b1 = FhUtil.get_at<byte>(k_sfast_get + 1); b6 = FhUtil.get_at<byte>(k_sfast_get + 6); b7 = FhUtil.get_at<byte>(k_sfast_get + 7);
            b8 = FhUtil.get_at<byte>(k_sfast_get + 8); b9 = FhUtil.get_at<byte>(k_sfast_get + 9); b10 = FhUtil.get_at<byte>(k_sfast_get + 10);
            b17 = FhUtil.get_at<byte>(k_sfast_get + 17); b18 = FhUtil.get_at<byte>(k_sfast_get + 18); b19 = FhUtil.get_at<byte>(k_sfast_get + 19);
            op = FhUtil.get_at<uint>(k_sfast_get + 20); want = (uint)(nint)FhUtil.ptr_at<byte>(k_sfast_byte);
        } catch { }
        bool shape = b0 == 0x80 && b1 == 0x3D && b6 == 0x00 && b7 == 0x56 && b8 == 0x0F && b9 == 0xBE && b10 == 0x35 && b17 == 0x0F && b18 == 0xBE && b19 == 0x35 && op == want;
        if (!shape) {
            _sfast_hook = 2;
            _sfast_status = I($"not inserted: 0x82D7E0 starts {b0:X2} {b1:X2} .. {b7:X2} {b8:X2} {b9:X2} {b10:X2}, operand 0x{op:X8} (expected 80 3D .. 56 0F BE 35 and the exec-frames byte at 0x{want:X8}): another mod's patch, or another exe; the game keeps its own speed");
            _logger.Warning("sphere fast: " + _sfast_status); d7_log("sph: fast: the detour: " + _sfast_status);
            return;
        }
        _h_sfast = h_sfast_get; new FhCallLocation("FFX.exe", k_sfast_get).try_resolve(out _a_sfast);
        if (_a_sfast == 0) { _sfast_hook = 4; _sfast_status = "not inserted: FFX.exe not found"; _logger.Warning("sphere fast: " + _sfast_status); return; }
        bool ok = false; string why = "";
        diag7_insert_begin();   // no main-thread detour resolves its original while this writes Fahrenheit's table (B7-1)
        try { ok = new FhCallHandle<d_sfast_get>(new FhCallLocation((void*)_a_sfast)).hook(this, _h_sfast!); }
        catch (Exception e) { why = e.GetType().Name; _logger.Error($"sphere fast: the detour: {e}"); }
        finally { diag7_insert_end(); }
        d_sfast_get? o = null;
        if (ok) { try { o = d7_chain(_a_sfast, _h_sfast!); } catch { } }
        if (o != null) System.Threading.Volatile.Write(ref _o_sfast, o);
        _sfast_hook = ok ? 1 : 3;
        _sfast_status = ok ? I($"inserted at 0x{_a_sfast:X8} (the exec-frames getter 0x82D7E0){(o == null ? "; its original resolves at its first call" : "")}")
                           : "NOT inserted: Fahrenheit refused the insertion" + (why.Length > 0 ? " (" + why + ")" : " (another hook on 0x82D7E0? core.log)") + ": the game keeps its own speed";
        _logger.Info("sphere fast: " + _sfast_status); d7_log("sph: fast: the detour: " + _sfast_status);
    }

    /// The game's exec-frames getter (0x82D7E0; the game's main thread, several times per main-loop pass). The original always
    /// runs, first; an answer of 1 goes to native, which returns it or the fast sequence's factor (sph::fast_exec). Never
    /// throws into the caller; any doubt returns the game's own answer.
    private int h_sfast_get() {
        d_sfast_get o = System.Threading.Volatile.Read(ref _o_sfast) ?? d7_resolve_main(ref _o_sfast, _a_sfast, _h_sfast!);
        int v = o();
        _sfast_calls++;
        if (v != 1) return v;
        try {
            var f = _fn_sph_fast_exec;
            if (f != null) { int r = f(v); if (r >= 1 && r <= 12) return r; }
        }
        catch { _sfast_faults++; }
        return v;
    }

    /// F8 > Hands, under the sphere settings.
    private void sfast_ui() {
        ui_check("sphere_fast", "Fast sphere grab##sfast", ref _sphere_fast, UiTag.None,
                 "In a Cloister of Trials, in first person, after a squeeze of your grip took or placed a sphere: the game runs several times faster until you have control again, so its take / place animation and its pauses pass in a fraction of the time. Only for a grab started with the grip (never for the A button), for 3 seconds at most, and the game is back at its own speed the moment the sequence ends or anything interrupts it (a menu, a battle, another map, leaving first person). Sounds of the sped-up moment may overlap. Djose only for now.");
        if (_sphere_fast) {
            ImGui.SameLine();
            ui_slider_int("sphere_fast_factor", "speed##sfast", ref _sphere_fast_factor, 1, 12, _sphere_fast_factor <= 1 ? "off" : "x%d", UiTag.None,
                          "How many times faster the game runs during a grip-started sphere grab (1 = off). 6 by default.", 110f);
        }
        if (_sphere_fast && !_sphere_hands) { ImGui.SameLine(); ImGui.TextDisabled("(needs \"Grab spheres with your hands\" on)"); }
        if (_sphere_fast && _sphere_fast_factor > 1 && _sfast_hook > 1) { ImGui.SameLine(); ImGui.TextDisabled("(no effect: see Developer > Inspect)"); }
        ui_check("sphere_carry_seq", "Sphere straight to and from your hand##sfast", ref _sphere_carry_seq, UiTag.None,
                 "In a Cloister of Trials, in first person, during a grab started with your grip: a sphere you take is drawn in your palm from the moment the game shows it, and a sphere you place stays in your palm until it is in the socket, instead of following the game's own hand animation in between. Off: the sphere is in your palm only while you have control, as before.");
        if (_sphere_carry_seq && !_sphere_carry) { ImGui.SameLine(); ImGui.TextDisabled("(needs \"Carried sphere in your hand\" on)"); }
    }
    /// F8 > Developer > Inspect: the detour's state (native's part is in the "sphere hands:" line).
    private void sfast_dev_ui() {
        ImGui.TextDisabled(esc(I($"sphere fast: detour on the exec-frames getter: {_sfast_status} | calls {_sfast_calls}, faults {_sfast_faults} | the game's exec-frames byte now {sfast_byte_now()}")));
    }
    private int sfast_byte_now() { try { return FhUtil.get_at<sbyte>(k_sfast_byte); } catch { return -1; } }
}
