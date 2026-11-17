// SPDX-License-Identifier: MIT

namespace FFXVR;

// Battle visibility, build 2 (branch battle-vis2, ABI 69; design-notes "Battle visibility, build 2"; the rules are native:
// ffxvr_battlevis.h "the battle camera's hide" and ffxvr_fxface.h, unit-tested). Two parts, each with one game hook that is
// inserted only when it is needed, late, on the render thread (as build 1's detour is), never at module init.
//
// 1. The battle camera's hide (F8 > Battle > "Characters the game's battle camera hides", ini battle_cam_hide, default 0 =
//    keep them shown). The owner's recording: the game's battle camera hides one character per action through the camera's
//    visibility request (Armor Break its target, Tidus's Attack hid Yuna, an enemy's Blizzard another enemy), for about a
//    second each. While the mod draws a battle in VR in first person or the diorama (both replace the game's camera), a call
//    msSetChrVisibleReq(i, 3, 0), the camera's request set to "hide", is not passed on to the game. Every other call runs the
//    game's own function with its own arguments: the other three requests (main, appear, effect), every "show", and the
//    camera's hides outside VR battles or with the setting at 1. Nothing is written to the game.
//    The detour is inserted the first frame the setting is 0 in a battle drawn in VR in first person or the diorama.
// 2. Effect sprites aimed at the eyes (F8 > Battle > "Aim spell effects at your eyes", ini fx_face_eyes, default 0). The game's
//    PS2 effect code builds its sprites facing the game's camera, which in VR looks somewhere else (up to 170 degrees off in
//    the owner's recording). With the setting on, in a battle drawn in VR in first person or the diorama, the game's six
//    effect matrices are substituted around each run of an effect part (pppRunPart) so the sprites are built facing the
//    head, and put back right after (ffxvr_fxface.h has the rule and why nothing moves). The detour is inserted the first
//    frame the setting is on in such a battle; with the setting off it only calls the game's function.
// 3. Late 3D layers (branch late3d, ABI 70; F8 > Battle > "Characters a spell draws by itself", ini late_3d_layer, default 1 =
//    in 3D; design-notes "Late 3D layers: Holy"; the rule is native, ffxvr_late3d.h, unit-tested). In its second phase the
//    spell Holy draws its target alone after the game's post-processing, behind a depth-only clear: the frame structure the
//    mod knows the 2D UI pass by. Native tells the two apart at the pass's first draw; this file only pushes the setting,
//    the battle state and the magic file's id (for the log lines) once a frame. No game hook, nothing written to the game.
//
// Game facts (FhUtil offsets = VA - 0x400000; ffx-rust's symbols/ffx.sym, docs/formats/battle.md and vfx.md, static research of
// the same FFX.exe, and Fahrenheit's names; facts only). Not seen in the running game by us unless said:
//   0x797080 msSetChrVisibleReq(i, k, v): cdecl, three arguments, returns 1 when actor i & 0xFF has a character, else 0
//     (ffx-rust's port, run against the exe in its own harness; Fahrenheit's table has the address without a signature).
//     Request k of the actor (+0x16 + k) = v's low byte; visible (+0x15) while all four are set; it then shows or hides the
//     character, its shadow request, its body hit, the actor it is bound to, the actors on it and its two weapons. k 3 is the
//     camera's (+0x19; the owner's recording shows it going 1 -> 0 -> 1 around each of the hides above).
//     Callers of k 3: MsSetChrVisibleCamera (0x7967A0: sets g_BtlCameraHides 0x112C9FE to 1 first; the scripts' property
//     0xD8), MsSetChrVisibleCameraAll (0x7967C0), MsReturnCameraVisible (0x795C70: clears the flag and shows all 31 again).
//   0x11363B8 / 0x1136B58 g_ActorPropertyLatch / g_BtlHideMask: while they share a bit the game queues a hide instead of
//     hiding (one battle scene's fix); such a call is passed on.
//   0x112C9D7 g_BtlEffectCameraMode (u8; not 0 while an effect camera runs): read for the skip line only.
//   0x712250 pppRunPart(part, flags): cdecl, two arguments, returns a count (Fahrenheit's own call table: _pppRunPart).
//   0x7C0070 MsCopyCameraMatrix(out): cdecl, one argument (ffx-rust's hook table, run in the game through that hook). The
//     magic files import it; a call inside a re-aimed part run is counted (review S2), nothing is answered or changed.
//   The effect code's matrices (4 x 4 floats): 0x230FF60 g_pppCameraMatrix, 0x230FF20 g_pppViewMatrix, 0x230FE20
//     g_pppViewMatrixCopy, 0x230FCA0 g_pppFillMatrix, 0x230FC20 g_pppFillMatrix2, 0x230FBE0 g_pppFillMatrix3.
public sealed unsafe partial class FFXVRModule {

    private int  _battle_cam_hide;        // ini battle_cam_hide: 0 (default) the battle camera's hides are skipped in VR battles, 1 the game's behaviour
    private bool _fx_face_eyes;           // ini fx_face_eyes (default 0): effect sprites aimed at the eyes
    private int  _late_3d_layer = 1;      // ini late_3d_layer: 1 (default) a character a spell draws by itself stays in the 3D view, 0 the old behaviour (it lands on the UI panel)
    private delegate* unmanaged[Cdecl]<int, int, int, int> _fn_late3d_set;                                           // ABI 70
    private int _l3d_frames, _l3d_faults; // the frames with a late 3D layer this session (native's answer); failed reads of the magic id
    private delegate* unmanaged[Cdecl]<int, int, int, int, int, int*, int, int> _fn_bch_frame;                       // ABI 69
    private delegate* unmanaged[Cdecl]<uint, uint, uint, uint, uint, int> _fn_bch_decide;                            // ABI 69
    private delegate* unmanaged[Cdecl]<void> _fn_fxf_camcopy;                                                        // ABI 69
    private delegate* unmanaged[Cdecl]<int, int, int*, int, int> _fn_fxf_frame;                                      // ABI 69
    private delegate* unmanaged[Cdecl]<float*, float*, float*, float*, float*, float*, float*, int> _fn_fxf_begin;   // ABI 69
    private delegate* unmanaged[Cdecl]<float*, float*, float*, float*, float*, float*, float*, void> _fn_fxf_end;    // ABI 69

    private const int k_bch_req = 0x397080;                            // VA 0x797080 msSetChrVisibleReq
    private const int k_bch_latch = 0xD363B8, k_bch_mask = 0xD36B58;   // VA 0x11363B8, 0x1136B58
    private const int k_bch_fxcam = 0xD2C9D7;                          // VA 0x112C9D7 g_BtlEffectCameraMode (u8)
    private const int k_fxf_camcopy = 0x3C0070;                        // VA 0x7C0070 MsCopyCameraMatrix
    private const int k_fxf_run = 0x312250;                            // VA 0x712250 pppRunPart
    private const int k_fxf_cam = 0x1F0FF60, k_fxf_view = 0x1F0FF20, k_fxf_copy = 0x1F0FE20;     // VA 0x230FF60, 0x230FF20, 0x230FE20
    private const int k_fxf_fill = 0x1F0FCA0, k_fxf_fill2 = 0x1F0FC20, k_fxf_fill3 = 0x1F0FBE0;  // VA 0x230FCA0, 0x230FC20, 0x230FBE0
    private const int k_fxf_save = 96;                                 // = native fxf::k_save (six matrices)

    // the battle camera's hide: the detour on msSetChrVisibleReq
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate uint d_bch_req(uint i, uint k, uint v);
    private d_bch_req? _h_bch, _o_bch; private nint _a_bch;
    private int _bch_hook;   // 0 not tried, 1 inserted, 3 refused / failed, 4 FFX.exe not found, 5 no native helper
    private string _bch_status = "not inserted (it is inserted the first frame battle_cam_hide is 0 in a battle drawn in VR in first person or the diorama)";
    private int _bch_faults;                           // (int: written on the game's thread, read on the render thread for F8)
    private readonly int[] _bch_st = new int[4];

    // fx_face_eyes: the detour on pppRunPart
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate uint d_fxf_run(uint part, uint flags);
    private d_fxf_run? _h_fxf, _o_fxf; private nint _a_fxf;
    private int _fxf_hook;   // 0 not tried, 1 inserted, 3 refused / failed, 4 FFX.exe not found, 5 no native helper
    private string _fxf_status = "not inserted (it is inserted the first frame fx_face_eyes is on in a battle drawn in VR in first person or the diorama)";
    private int _fxf_live, _fxf_faults, _fxf_calls;    // _fxf_live: 1 while the render thread wants part runs re-aimed (read on the game's thread)
    private float* _fxf_cam, _fxf_view, _fxf_copy, _fxf_fill, _fxf_fill2, _fxf_fill3;   // the game's statics, set once before the insertion
    private readonly int[] _fxf_st = new int[10];
    // fx_face_eyes: the counting detour on MsCopyCameraMatrix (review S2), inserted with the one above
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate uint d_fxf_cc(uint dst);
    private d_fxf_cc? _h_fxcc, _o_fxcc; private nint _a_fxcc;
    private int _fxcc_hook;   // 0 not tried, 1 inserted, 3 refused / failed, 4 FFX.exe not found
    private string _fxcc_status = "not inserted";

    private bool bv2_parse(string k, string v) {
        switch (k) {
            case "battle_cam_hide": pi(k, v, ref _battle_cam_hide, 0, 1); return true;
            case "fx_face_eyes":    pb(k, v, ref _fx_face_eyes); return true;
            case "late_3d_layer":   pi(k, v, ref _late_3d_layer, 0, 1); return true;
            default: return false;
        }
    }
    private string bv2_ini_text() => $"battle_cam_hide={_battle_cam_hide.ToString(CI)}\nfx_face_eyes={(_fx_face_eyes ? 1 : 0)}\nlate_3d_layer={_late_3d_layer.ToString(CI)}\n";
    private void bv2_bind(Func<string, nint> req) {
        _fn_bch_frame = (delegate* unmanaged[Cdecl]<int, int, int, int, int, int*, int, int>)req("ffxvr_bch_frame");
        _fn_bch_decide = (delegate* unmanaged[Cdecl]<uint, uint, uint, uint, uint, int>)req("ffxvr_bch_decide");
        _fn_fxf_camcopy = (delegate* unmanaged[Cdecl]<void>)req("ffxvr_fxf_camcopy");
        _fn_fxf_frame = (delegate* unmanaged[Cdecl]<int, int, int*, int, int>)req("ffxvr_fxf_frame");
        _fn_fxf_begin = (delegate* unmanaged[Cdecl]<float*, float*, float*, float*, float*, float*, float*, int>)req("ffxvr_fxf_begin");
        _fn_fxf_end = (delegate* unmanaged[Cdecl]<float*, float*, float*, float*, float*, float*, float*, void>)req("ffxvr_fxf_end");
        _fn_late3d_set = (delegate* unmanaged[Cdecl]<int, int, int, int>)req("ffxvr_late3d_set");
    }
    private void bv2_unbind() { _fn_bch_frame = null; _fn_bch_decide = null; _fn_fxf_frame = null; _fn_fxf_begin = null; _fn_fxf_end = null; _fn_fxf_camcopy = null; _fn_late3d_set = null; }

    /// From push_all, every frame, before bv_tick (render thread; after push_player: the battle state and the view are this
    /// frame's). The battle camera hide's state to native (which hands the skipped hides to the battle research log), its
    /// detour the first frame it is needed; fx_face_eyes' head pose to native, its detour the first frame it is needed.
    private void bv2_tick() {
        // late 3D layers (ABI 70): the setting, the battle state and the magic file's id in play (read only in a battle; for
        // the log lines only) to native, every frame
        if (_fn_late3d_set != null) {
            int magic = -1;
            if (_btl_was) { try { magic = FhUtil.get_at<int>(k_bv_magic_id); } catch { _l3d_faults++; magic = -1; } }
            _l3d_frames = _fn_late3d_set(_late_3d_layer, _btl_was ? 1 : 0, magic);
        }
        if (_fn_bch_frame == null || _fn_fxf_frame == null) return;
        int* st = stackalloc int[10];
        for (int i = 0; i < 10; i++) st[i] = 0;
        int state = _fn_bch_frame(_battle_cam_hide, _btl_was ? 1 : 0, _xr_on ? 1 : 0, _fp_mode, _bch_hook, st, 4);
        for (int i = 0; i < 4; i++) _bch_st[i] = st[i];
        if (state == 2 && _bch_hook == 0 && _native_ready) bch_install();   // 2 = native bvis::CHS_NOHOOK: wanted now, and the detour is not in
        bool fx = _fx_face_eyes && _btl_was && _xr_on && (_fp_mode == 1 || _fp_mode == 2);
        if (fx && _fxf_hook == 0 && _native_ready) fxf_install();
        // review round 2 (S-R2-1): the counting detour only for a recording: the box ticked AND the battle research log on
        if (fx && _diag_battle && _fxf_hook == 1 && _fxcc_hook == 0 && _native_ready) fxcc_install();
        System.Threading.Volatile.Write(ref _fxf_live, fx && _fxf_hook == 1 ? 1 : 0);
        for (int i = 0; i < 10; i++) st[i] = 0;
        _fn_fxf_frame(fx ? 1 : 0, _fxf_hook | (_fxcc_hook == 1 ? 16 : _fxcc_hook > 1 ? 32 : 0), st, 10);
        for (int i = 0; i < 10; i++) _fxf_st[i] = st[i];
    }

    // -- the battle camera's hide --------------------------------------------------------------------------------------------

    /// Render thread, once (from bv2_tick; a late insertion, bracketed like sfast_install's): the detour on the game's
    /// msSetChrVisibleReq. Fahrenheit and its bundled modules do not hook this function (alpha12's sources), so there is no
    /// chain to keep but Fahrenheit's own.
    private void bch_install() {
        if (_bch_hook != 0) return;
        if (!_native_ready || _fn_bch_decide == null || _fn_safe_read == null) { _bch_hook = 5; _bch_status = "not inserted: the native helper did not load"; return; }
        _h_bch = h_bch_req;
        new FhCallLocation("FFX.exe", k_bch_req).try_resolve(out _a_bch);
        if (_a_bch == 0) { _bch_hook = 4; _bch_status = "not inserted: FFX.exe not found"; _logger.Warning("battle camera hide: " + _bch_status); return; }
        // the function's first bytes, read before the insertion and logged (they are not known from the notes, so nothing is
        // compared: no game bytes are kept in the repository)
        string first = "";
        try { for (int i = 0; i < 8; i++) first += FhUtil.get_at<byte>(k_bch_req + i).ToString("X2", CI) + (i < 7 ? " " : ""); } catch { first = "not read"; }
        bool ok = false; string why = "";
        diag7_insert_begin();   // no main-thread detour resolves its original while this writes Fahrenheit's table (B7-1)
        try { ok = new FhCallHandle<d_bch_req>(new FhCallLocation((void*)_a_bch)).hook(this, _h_bch!); }
        catch (Exception e) { why = e.GetType().Name; _logger.Error($"battle camera hide: the detour: {e}"); }
        finally { diag7_insert_end(); }
        d_bch_req? o = null;
        if (ok) { try { o = d7_chain(_a_bch, _h_bch!); } catch { } }
        if (o != null) System.Threading.Volatile.Write(ref _o_bch, o);
        _bch_hook = ok ? 1 : 3;
        _bch_status = ok ? I($"inserted at 0x{_a_bch:X8} (msSetChrVisibleReq 0x797080, its first bytes before the insertion: {first}; the insertion stops the game's threads for a moment: one hitch, once a session){(o == null ? "; its original resolves at its first call" : "")}")
                         : "NOT inserted: Fahrenheit refused the insertion" + (why.Length > 0 ? " (" + why + ")" : " (another hook on 0x797080? core.log)") + ": the battle camera's hides happen as in the game (its first bytes: " + first + ")";
        _logger.Info("battle camera hide: the msSetChrVisibleReq detour: " + _bch_status); d7_log("vis: the msSetChrVisibleReq detour: " + _bch_status);
    }

    /// The game's msSetChrVisibleReq (0x797080; the game's main thread, at every change of a battle actor's visibility
    /// request). Any call that is not a hide through the camera's request (k 3, v's low byte 0) runs the game's own function
    /// at once, with the game's own arguments, and returns its result. A camera hide is put to native (bvis::cam_hide_skip:
    /// battle_cam_hide 0, a battle drawn in VR in first person or the diorama, the game's hide queue not armed): skipped =
    /// the game's function is not called and the answer is what it would have returned (1 when the actor has a character);
    /// anything else, or any doubt, runs the game's function. Nothing is written to the game. Never throws into the caller.
    private uint h_bch_req(uint i, uint k, uint v) {
        d_bch_req o = System.Threading.Volatile.Read(ref _o_bch) ?? d7_resolve_main(ref _o_bch, _a_bch, _h_bch!);
        if (k != 3 || (v & 0xFF) != 0) return o(i, k, v);
        int skip = 0;
        try {
            var f = _fn_bch_decide;
            if (f != null) {
                uint queue = FhUtil.get_at<uint>(k_bch_latch) & FhUtil.get_at<uint>(k_bch_mask);
                skip = f(i, k, v, queue, FhUtil.get_at<byte>(k_bch_fxcam));
            }
        }
        catch { _bch_faults++; skip = 0; }
        if (skip != 0) return bch_has_chr(i);
        return o(i, k, v);
    }
    /// What the game's function returns: 1 when battle actor i & 0xFF has a character (its record's first word), else 0
    /// (one guarded read; the records 0..0x1E are one array, as build 1's log found). Only for a skipped call. Never throws.
    private uint bch_has_chr(uint i) {
        try {
            var rd = _fn_safe_read;
            int idx = (int)(i & 0xFF);
            if (rd == null || idx >= k_bv_actors) return 0;
            nint chrs = FhUtil.get_at<nint>(k_bv_chrs);
            if (chrs == 0) return 0;
            uint chr = 0;
            if (rd((byte*)chrs + idx * k_bv_chr_size, &chr, 4) == 0) return 0;
            return chr != 0 ? 1u : 0u;
        }
        catch { _bch_faults++; return 0; }
    }

    // -- fx_face_eyes --------------------------------------------------------------------------------------------------------

    /// Render thread, once (from bv2_tick; a late insertion, bracketed like sfast_install's): the detour on the game's
    /// pppRunPart. Fahrenheit names the function in its call table and does not hook it, nor do its bundled modules
    /// (alpha12's sources).
    private void fxf_install() {
        if (_fxf_hook != 0) return;
        if (!_native_ready || _fn_fxf_begin == null || _fn_fxf_end == null) { _fxf_hook = 5; _fxf_status = "not inserted: the native helper did not load"; return; }
        _fxf_cam = FhUtil.ptr_at<float>(k_fxf_cam); _fxf_view = FhUtil.ptr_at<float>(k_fxf_view); _fxf_copy = FhUtil.ptr_at<float>(k_fxf_copy);
        _fxf_fill = FhUtil.ptr_at<float>(k_fxf_fill); _fxf_fill2 = FhUtil.ptr_at<float>(k_fxf_fill2); _fxf_fill3 = FhUtil.ptr_at<float>(k_fxf_fill3);
        _h_fxf = h_fxf_run;
        new FhCallLocation("FFX.exe", k_fxf_run).try_resolve(out _a_fxf);
        if (_a_fxf == 0) { _fxf_hook = 4; _fxf_status = "not inserted: FFX.exe not found"; _logger.Warning("fx face eyes: " + _fxf_status); return; }
        string first = "";
        try { for (int i = 0; i < 8; i++) first += FhUtil.get_at<byte>(k_fxf_run + i).ToString("X2", CI) + (i < 7 ? " " : ""); } catch { first = "not read"; }
        bool ok = false; string why = "";
        diag7_insert_begin();   // no main-thread detour resolves its original while this writes Fahrenheit's table (B7-1)
        try { ok = new FhCallHandle<d_fxf_run>(new FhCallLocation((void*)_a_fxf)).hook(this, _h_fxf!); }
        catch (Exception e) { why = e.GetType().Name; _logger.Error($"fx face eyes: the detour: {e}"); }
        finally { diag7_insert_end(); }
        d_fxf_run? o = null;
        if (ok) { try { o = d7_chain(_a_fxf, _h_fxf!); } catch { } }
        if (o != null) System.Threading.Volatile.Write(ref _o_fxf, o);
        _fxf_hook = ok ? 1 : 3;
        _fxf_status = ok ? I($"inserted at 0x{_a_fxf:X8} (pppRunPart 0x712250, its first bytes before the insertion: {first}; the insertion stops the game's threads for a moment: one hitch, once a session){(o == null ? "; its original resolves at its first call" : "")}")
                         : "NOT inserted: Fahrenheit refused the insertion" + (why.Length > 0 ? " (" + why + ")" : " (another hook on 0x712250? core.log)") + ": the effects are drawn as the game builds them (its first bytes: " + first + ")";
        _logger.Info("fx face eyes: the pppRunPart detour: " + _fxf_status); d7_log("fx: the pppRunPart detour: " + _fxf_status);
    }
    /// Render thread, once (from bv2_tick), the first frame the box is ticked in a VR battle with the pppRunPart detour in
    /// AND the battle research log (diag_battle) on (review S2, and round 2's S-R2-1): a counting detour on the game's
    /// MsCopyCameraMatrix. It always runs the game's function and returns its result; a refusal only means the count is
    /// not available (the log says so). Fahrenheit and its bundled modules do not hook this function (alpha12's sources).
    /// Why only with the research log on: the function is small and the main loop calls it every frame in every scene; a
    /// caller inside the exe may rely on ECX / EDX surviving the call (the exe is built with whole-program optimisation;
    /// ffx-rust's own hook keeps both for that reason), and a managed detour does not keep them. Not measured: so play
    /// with the box ticked does not carry this detour, only a recording session does (it stays in until the game closes).
    private void fxcc_install() {
        if (_fxcc_hook != 0 || _fn_fxf_camcopy == null) return;
        _h_fxcc = h_fxf_cc;
        new FhCallLocation("FFX.exe", k_fxf_camcopy).try_resolve(out _a_fxcc);
        if (_a_fxcc == 0) { _fxcc_hook = 4; _fxcc_status = "not inserted: FFX.exe not found"; return; }
        string first = "";
        try { for (int i = 0; i < 8; i++) first += FhUtil.get_at<byte>(k_fxf_camcopy + i).ToString("X2", CI) + (i < 7 ? " " : ""); } catch { first = "not read"; }
        bool ok = false; string why = "";
        diag7_insert_begin();   // no main-thread detour resolves its original while this writes Fahrenheit's table (B7-1)
        try { ok = new FhCallHandle<d_fxf_cc>(new FhCallLocation((void*)_a_fxcc)).hook(this, _h_fxcc!); }
        catch (Exception e) { why = e.GetType().Name; _logger.Error($"fx face eyes: the counting detour: {e}"); }
        finally { diag7_insert_end(); }
        d_fxf_cc? o = null;
        if (ok) { try { o = d7_chain(_a_fxcc, _h_fxcc!); } catch { } }
        if (o != null) System.Threading.Volatile.Write(ref _o_fxcc, o);
        _fxcc_hook = ok ? 1 : 3;
        _fxcc_status = ok ? I($"inserted at 0x{_a_fxcc:X8} (MsCopyCameraMatrix 0x7C0070, its first bytes before the insertion: {first}){(o == null ? "; its original resolves at its first call" : "")}")
                          : "NOT inserted: Fahrenheit refused the insertion" + (why.Length > 0 ? " (" + why + ")" : " (another hook on 0x7C0070? core.log)") + ": the calls inside re-aimed runs are not counted (its first bytes: " + first + ")";
        _logger.Info("fx face eyes: the MsCopyCameraMatrix counting detour: " + _fxcc_status); d7_log("fx: the MsCopyCameraMatrix counting detour: " + _fxcc_status);
    }
    /// The game's MsCopyCameraMatrix (0x7C0070; the game's main thread, a few times a frame and from the magic files). The
    /// game's own function always runs first, with the game's own argument, and its result is returned unchanged; while
    /// part runs are being re-aimed native is told of the call (it counts the ones inside a run). Never throws into the caller.
    private uint h_fxf_cc(uint dst) {
        d_fxf_cc o = System.Threading.Volatile.Read(ref _o_fxcc) ?? d7_resolve_main(ref _o_fxcc, _a_fxcc, _h_fxcc!);
        uint r = o(dst);
        if (System.Threading.Volatile.Read(ref _fxf_live) == 0 || !_diag_battle) return r;
        try { var f = _fn_fxf_camcopy; if (f != null) f(); } catch { _fxf_faults++; }
        return r;
    }

    /// The game's pppRunPart (0x712250; the game's main thread, once per effect part per frame). The game's own function
    /// always runs, with the game's own arguments, and its result is returned unchanged. While the render thread wants it
    /// (fx_face_eyes on, a battle drawn in VR in first person or the diorama), native substitutes the game's six effect
    /// matrices right before the call (fxf::substitute: only when a head pose is published and the matrices are what the rule
    /// needs) and puts them back right after it. Never throws into the caller.
    private uint h_fxf_run(uint part, uint flags) {
        d_fxf_run o = System.Threading.Volatile.Read(ref _o_fxf) ?? d7_resolve_main(ref _o_fxf, _a_fxf, _h_fxf!);
        var begin = _fn_fxf_begin; var end = _fn_fxf_end;
        if (System.Threading.Volatile.Read(ref _fxf_live) == 0 || begin == null || end == null || _fxf_cam == null) return o(part, flags);
        float* save = stackalloc float[k_fxf_save];
        int did = 0;
        _fxf_calls++;
        try { did = begin(_fxf_cam, _fxf_view, _fxf_copy, _fxf_fill, _fxf_fill2, _fxf_fill3, save); } catch { _fxf_faults++; did = 0; }
        uint r = o(part, flags);
        if (did != 0) { try { end(_fxf_cam, _fxf_view, _fxf_copy, _fxf_fill, _fxf_fill2, _fxf_fill3, save); } catch { _fxf_faults++; } }
        return r;
    }

    // -- F8 ------------------------------------------------------------------------------------------------------------------

    private const string k_bch_tip = "The game's battle camera hides a character for about a second during some actions (the one its camera would sit in or pass through: the target of Armor Break, the party member beside an attacker). In the headset your eyes are not that camera. Keep them shown: while a battle is drawn in VR in first person or the diorama (not on a screen in the headset), those hides are skipped; everything else the game hides (a spell's fade, a defeated enemy, party members off the field) stays the game's. Hide them: the game's own behaviour.";
    private const string k_fxf_tip = "The game builds spell sprites (glows, sparks, beams) facing its own battle camera, which in the headset looks somewhere else, so they can look flat or squashed. On: in a battle drawn in VR in first person or the diorama they are built facing your head instead (level, the same for both eyes). Experimental: if an effect looks wrong or sits in the wrong place with it on, untick it and send ffxvr_diag.log. With the battle research log (Developer > Inspect) also on, one more game hook counts how often an effect takes the game's camera by a route this box does not cover; without the log that hook is not installed.";
    private const string k_l3d_tip = "Holy and some other spells draw one character a second time, by itself, after the rest of the picture is finished, and then draw their effects over it. In 3D: that character and those effects stay in the 3D view, and the battle menus stay on their panel. On the menu panel, as before: the old behaviour, where that character lands flat on the menu panel and the spell's effects and the battle menus are not shown for that part of the spell. If a spell looks wrong with \"in 3D\", pick the other one and send ffxvr_diag.log.";
    /// F8 > Battle.
    private void bv2_ui() {
        ImGui.SeparatorText("Characters and effects");
        ImGui.Text("Characters the game's battle camera hides:"); ImGui.SameLine();
        ui_radio("battle_cam_hide", "keep them shown##bch", ref _battle_cam_hide, 0, UiTag.None, k_bch_tip); ImGui.SameLine();
        ui_radio("battle_cam_hide", "hide them, as the game does##bch", ref _battle_cam_hide, 1, UiTag.None, k_bch_tip);
        if (_battle_cam_hide == 0 && _bch_hook > 1) { ImGui.SameLine(); ImGui.TextDisabled("(no effect: see Developer > Inspect)"); }
        ImGui.Text("Characters a spell draws by itself:"); ImGui.SameLine();
        ui_radio("late_3d_layer", "in 3D##l3d", ref _late_3d_layer, 1, UiTag.None, k_l3d_tip); ImGui.SameLine();
        ui_radio("late_3d_layer", "on the menu panel, as before##l3d", ref _late_3d_layer, 0, UiTag.None, k_l3d_tip);
        ui_check("fx_face_eyes", "Aim spell effects at your eyes##fxf", ref _fx_face_eyes, UiTag.Experimental, k_fxf_tip);
        if (_fx_face_eyes && _fxf_hook > 1) { ImGui.SameLine(); ImGui.TextDisabled("(no effect: see Developer > Inspect)"); }
    }
    private static readonly string[] k_bch_state = { "the game's behaviour (battle_cam_hide 1)", "skipping the camera's hides", "wanted, the detour is not in", "not in effect now (not a VR battle in first person or the diorama, or the headset shows a screen)" };   // = native bvis::CHS_*
    /// F8 > Developer > Inspect.
    private void bv2_dev_ui() {
        int s = _bch_st[0];
        ImGui.TextDisabled(esc(I($"battle camera hide: {(s >= 0 && s < k_bch_state.Length ? k_bch_state[s] : "?")} | hides skipped this session {_bch_st[1]}, camera hides asked {_bch_st[2]}, events lost {_bch_st[3]}, faults {_bch_faults} | msSetChrVisibleReq detour: {_bch_status}")));
        ImGui.TextDisabled(esc(I($"late 3D layers: late_3d_layer {_late_3d_layer} | frames with one this session {_l3d_frames} | magic id reads failed {_l3d_faults} | the counts per 10 s are in the ui: line of ffxvr_diag.log")));
        ImGui.TextDisabled(esc(I($"fx face eyes: {(_fx_face_eyes ? (_fxf_st[0] != 0 ? "on, a head pose is published" : "on, no head pose now (not a VR battle in first person or the diorama, or the headset shows a screen)") : "off")} | part runs seen {_fxf_st[1]}, re-aimed {_fxf_st[2]}, not: no pose {_fxf_st[3]}, the view's copy differs {_fxf_st[4]}, a matrix did not invert {_fxf_st[5]}, no basis {_fxf_st[6]}, nested {_fxf_st[7]} | MsCopyCameraMatrix calls inside re-aimed runs {_fxf_st[9]} | frame censuses asked {_fxf_st[8]} | detour calls {_fxf_calls}, faults {_fxf_faults} | pppRunPart detour: {_fxf_status} | MsCopyCameraMatrix counting detour: {_fxcc_status}")));
    }
}
