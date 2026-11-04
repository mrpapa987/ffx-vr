// SPDX-License-Identifier: MIT

namespace FFXVR;

// VR hands v2, stages S1 and S2 (the spec: research hands-v2-spec.md; the native half: ffxvr_native.cpp "VR hands v2",
// ffxvr_hands.cpp). Our own hands drawn at the controllers, in the scene, in both eyes, depth-tested against the world and
// under the UI panel. S2 draws the shipped generic hand (<mod>\hands\generic-glove or generic: tools/hands/make-generic-hand.py,
// ours, MIT), skinned on the GPU, its fingers following the controllers' trigger, grip and touch sensors; the S1 boxes are
// the fallback (no pack, a pack that failed to load) and a debug option. S3: every pack folder under <mod>\hands\ (the two
// shipped ones and the user's own, e.g. made by tools\import-hands.ps1 from a model they own) is listed in F8 with its
// hand.json (hand_pack.cs: parsed, validated, clamped); "rescan" reads the folder again. Off by default.
//
// The owner's rules (2 Nov, 3 Nov 2026): in first person only with the player's body hidden (fp_body_mode 0, "hide my whole
// body", the default; no forearm cutting), and by choice in the diorama ("to give our hands some context when adjusting the
// view"), third person and the seat view, where Tidus is part of the scene. No optical hand tracking.
//
// ini keys (all applied at once):
//   vr_hands            0/1, default 0: the master switch. 0 costs one branch per resolve: no shaders, nothing drawn.
//   vr_hands_views      bits, default 1: 1 first person (always), 2 the diorama, 4 third person, 8 the seat view.
//   vr_hands_pack       a folder under <mod>\hands\, default generic-glove: the hand model. Missing or unusable: generic-glove,
//                       then generic, then the boxes (logged).
//   vr_hands_rest_curl  0..1, default 0.20: how far the middle, ring and little fingers curl with the grip released.
//   vr_hands_scale      0.80..1.25, default 1.00: the hand's size.
//   vr_hands_cal        18 floats or empty: the grip -> hand rotation per controller (left 9, right 9, row-major, the
//                       OpenXR hand-joint axes), captured by "calibrate". Empty, or 9 zeros for a hand, = the default
//                       placement (Meta's palm pose on the Touch grip). Migrated once from the old bone hands' hands_cal.
//   vr_hands_yaw / _pitch / _roll       degrees, +-45, default 0: fine tune in the hand frame (yaw and roll mirrored for the left hand)
//   vr_hands_off_right / _up / _back    metres, +-0.10, default 0: offset in the hand frame (out from the thumb side, back of the
//                       hand, toward the elbow; "out" mirrored for the left hand)
//   vr_hands_debug      bits, default 0 (Debug only): 1 the wrist axes (red X, green Y, blue Z), 2 the boxes instead of the hand
//                       mesh, 4 one placement line per drawn frame in ffxvr_diag.log (300 at most per switch-on); 8 is the "log
//                       one pose dump" button's one-shot.
//   Tap build (design-notes "Hands: the tap gesture"; native ffxvr_hands2_tap_set, hv_tap_update):
//   vr_hands_tap          0/1, default 1: with VR hands drawn in first person, squeezing the grip arms that hand and pushing it
//                         forward about a hand's width toward what you face presses the game's confirm button (only in the
//                         field: never in battle, the sphere grid, the save screen, blitzball, a menu or a movie, nor with a choice
//                         box up or without control of the character, nor while a text box is up unless vr_hands_tap_dialogue=1).
//   vr_hands_tap_sens     0.50..2.00, default 1.00: the push that taps is 10 cm / this (0.5: 20 cm, 2: 5 cm).
//   vr_hands_tap_point    0/1, default 0: also require a pointing finger (the index off the trigger) to arm.
//   vr_hands_tap_hold_grip 0/1, default 0: don't pass the grips (LB / RB) to the game while a tap can fire (the field, first
//                         person): a grip squeezed there is held back until you let go; one squeezed in battle or a menu goes through.
//   vr_hands_tap_dialogue 0/1, default 0: also while a text box is up (advances the text; a choice box takes the highlighted
//                         answer, so off until the text box signal is confirmed in play).
//   vr_hands_tap_haptic   0/1, default 1: a short tick on that hand when a tap presses.
//   vr_hands_tap_button   a / b / x / y, default a (ini only): the XInput button a tap presses (A = cross = confirm in the PC build).
//   vr_hands_tap_press_ms 60..300, default 100 (ini only): how long it is held (and at least 3 new pad packets).
// Not here (spec 1.4, owner's decisions): the light / tracking keys of the later stages (tracking dropped).
public sealed unsafe partial class FFXVRModule {

    [StructLayout(LayoutKind.Sequential)]
    private struct VrHandsCfg { public int bytes, on, views, debug; public float yaw, pitch, roll, off_r, off_u, off_b, rest_curl, scale; }   // = native VrHandsCfg (ABI 28)
    [StructLayout(LayoutKind.Sequential)]
    private struct VrHandsPackDesc {   // = native VrHandsPackDesc (ABI 29; 88 bytes on x86): absolute UTF-16 paths, copied by native at once
        public int bytes, format; public char* right_glb, left_glb; public int mirror; public float scale;
        public fixed float wrist_off_m[3]; public fixed float wrist_rot_rad[3]; public fixed float tint[3]; public fixed float rest_curl[5]; public byte* name;
        public uint stamp;   // S3: the files' change stamp (a re-made pack loads again)
    }
    private const int k_h2_status_n = 21;           // ffxvr_hands2_status's values (ABI 29)
    [StructLayout(LayoutKind.Sequential)]
    private struct VrTapCfg { public int bytes, flags; public float sens; public int button, game, press_ms; }   // = native VrTapCfg (ABI 30; 24 bytes)
    private const int k_tap_status_n = 18;          // ffxvr_hands2_tap_status's values (ABI 31)
    private bool   _vr_hands_tap = true, _vr_hands_tap_dialogue = false, _vr_hands_tap_haptic = true, _vr_hands_tap_point = false, _vr_hands_tap_hold_grip = false;
    private float  _vr_hands_tap_sens = 1.00f;
    private string _vr_hands_tap_button = "a";
    private int    _vr_hands_tap_press_ms = 100;
    private int    _tap_game_last = -1;             // the game gate bits last logged
    private int    _tap_plan = TapPlan.k_off;       // cpu-lean (review N7): this frame's TapPlan, for F8's game field
    private int    _tap_game_logs;                  // game gate lines this session (200 at most)
    private string _tap_text_info = "";             // the text box windows as last read (F8)
    private delegate* unmanaged[Cdecl]<VrTapCfg*, int, void> _fn_hands2_tap_set;              // ABI 30: the tap's settings and the game gate
    private delegate* unmanaged[Cdecl]<float*, int, byte*> _fn_hands2_tap_status;            // ABI 30: the F8 readout

    private bool   _vr_hands = false;            // ini vr_hands (default 0)
    private int    _vr_hands_debug = 0;          // ini vr_hands_debug (bits 1, 2, 4)
    private int    _vr_hands_views = 1;          // ini vr_hands_views (bit 0 first person, always; 2 diorama, 4 third person, 8 seat)
    private string _vr_hands_pack = "generic-glove";   // ini vr_hands_pack: a folder under <mod>\hands\
    private float  _vr_hands_rest_curl = 0.20f, _vr_hands_scale = 1.00f;
    private string _h2_mod_dir = "";             // the mod folder (load_native): the packs are under <mod>\hands\
    private string? _h2_pack_sent;               // the pack native was last given ("" = none: the boxes); null = choose again
    private string _h2_pack_want = "";           // the selection the fallback walk below is for
    private int    _h2_pack_try;                 // the candidate being tried (the selected pack, then generic-glove, then generic)
    private List<HandPack> _h2_packs = new();    // S3: the scan of <mod>\hands\ (hand_pack.cs)
    private bool   _h2_scanned, _h2_scan_tried;  // a scan finished / was tried (at the first push; again on "rescan")
    private HandPack? _h2_pack_sent_p;           // the pack native was last given
    private string _vr_hands_cal = "";           // ini vr_hands_cal ("" = the default placement)
    private bool   _vr_hands_cal_read;           // the key was in the file (the hands_cal migration runs only without it)
    private float  _vr_hands_yaw, _vr_hands_pitch, _vr_hands_roll;      // degrees
    private float  _vr_hands_off_r, _vr_hands_off_u, _vr_hands_off_b;   // metres
    private string? _h2_cal_pushed;              // the vr_hands_cal text native has (null: nothing sent yet)
    private uint   _h2_cal_seen;                 // native's capture count when last read
    private double _h2_cal_at = -1;              // the calibration countdown's end (_now), -1: none
    private bool   _h2_migrate_done;
    private bool   _h2_can_draw;   // cpu-lean (b): ffxvr_hands2_set's answer this frame (hands2_tap_push reads the game state only then)
    private int    _h2_dump_frames;              // "log one pose dump": debug bit 8 held for this many frames (native acts on its rising edge)
    private int    _h2_cancels_seen = -1;        // native's cancelled-calibration count when last seen (SF-2)
    private double _h2_cancel_note_until = -1;   // show "cancelled" in the header until this _now
    private delegate* unmanaged[Cdecl]<nint, uint, nint, uint, uint, void> _fn_pre_resolve;   // ABI 27
    private delegate* unmanaged[Cdecl]<VrHandsCfg*, int, int> _fn_hands2_set;                // ABI 27; ABI 38: 1 = the coming frame can draw the hands
    private delegate* unmanaged[Cdecl]<int*, int, byte*> _fn_hands2_status;                  // ABI 27: (out, n) -> the status line
    private delegate* unmanaged[Cdecl]<float*, int, uint> _fn_hands2_cal;                    // ABI 27: op 0 read, 1 write, 2 capture -> captures
    private delegate* unmanaged[Cdecl]<VrHandsPackDesc*, int, void> _fn_hands2_pack;        // ABI 28: the pack to draw (null: none, the boxes)
    private delegate* unmanaged[Cdecl]<byte*> _fn_hands2_pack_error;                         // ABI 29: why the last pack asked for was rejected ("" none)

    /// <summary>settings_parse's default branch: the vr_hands_* keys. False: not ours.</summary>
    private bool hands2_parse(string k, string v) {
        switch (k) {
            case "vr_hands":           pb(k, v, ref _vr_hands); return true;
            case "vr_hands_debug":     pi(k, v, ref _vr_hands_debug, 0, 15); return true;
            case "vr_hands_views":     pi(k, v, ref _vr_hands_views, 0, 15); _vr_hands_views |= 1; return true;
            case "vr_hands_pack":      { string t = v.Trim();   // a folder name; whether it exists is the scan's business (it falls back, logged)
                                         if (t.Length > 0 && t.Length <= 255 && t.IndexOfAny(new[] { '\\', '/', ':', '*', '?', '"', '<', '>', '|' }) < 0 && t != "." && t != "..") _vr_hands_pack = t;
                                         else _ini_bad.Add($"{k}='{v}'"); } return true;
            case "vr_hands_rest_curl": pf(k, v, ref _vr_hands_rest_curl, 0f, 1f); return true;
            case "vr_hands_scale":     pf(k, v, ref _vr_hands_scale, 0.8f, 1.25f); return true;
            case "vr_hands_cal":       _vr_hands_cal = v.Trim(); _vr_hands_cal_read = true; return true;
            case "vr_hands_yaw":       pf(k, v, ref _vr_hands_yaw, -45f, 45f); return true;
            case "vr_hands_pitch":     pf(k, v, ref _vr_hands_pitch, -45f, 45f); return true;
            case "vr_hands_roll":      pf(k, v, ref _vr_hands_roll, -45f, 45f); return true;
            case "vr_hands_off_right": pf(k, v, ref _vr_hands_off_r, -0.10f, 0.10f); return true;
            case "vr_hands_off_up":    pf(k, v, ref _vr_hands_off_u, -0.10f, 0.10f); return true;
            case "vr_hands_off_back":  pf(k, v, ref _vr_hands_off_b, -0.10f, 0.10f); return true;
            case "vr_hands_tap":          pb(k, v, ref _vr_hands_tap); return true;
            case "vr_hands_tap_sens":     pf(k, v, ref _vr_hands_tap_sens, 0.5f, 2f); return true;
            case "vr_hands_tap_dialogue": pb(k, v, ref _vr_hands_tap_dialogue); return true;
            case "vr_hands_tap_haptic":   pb(k, v, ref _vr_hands_tap_haptic); return true;
            case "vr_hands_tap_point":    pb(k, v, ref _vr_hands_tap_point); return true;
            case "vr_hands_tap_hold_grip": pb(k, v, ref _vr_hands_tap_hold_grip); return true;
            case "vr_hands_tap_button":   { string t = v.Trim().ToLowerInvariant(); if (t == "a" || t == "b" || t == "x" || t == "y") _vr_hands_tap_button = t; else _ini_bad.Add($"{k}='{v}'"); } return true;
            case "vr_hands_tap_press_ms": pi(k, v, ref _vr_hands_tap_press_ms, 60, 300); return true;
            default: return false;
        }
    }

    /// <summary>ini_text's lines for the vr_hands_* keys.</summary>
    private string hands2_ini_text() {
        var ci = CI;
        return
            $"vr_hands={(_vr_hands ? 1 : 0)}\n" +
            $"vr_hands_views={(_vr_hands_views | 1).ToString(ci)}\n" +
            $"vr_hands_pack={_vr_hands_pack}\n" +
            $"vr_hands_rest_curl={_vr_hands_rest_curl.ToString("F2", ci)}\n" +
            $"vr_hands_scale={_vr_hands_scale.ToString("F2", ci)}\n" +
            $"vr_hands_cal={_vr_hands_cal}\n" +
            $"vr_hands_yaw={_vr_hands_yaw.ToString("F0", ci)}\n" +
            $"vr_hands_pitch={_vr_hands_pitch.ToString("F0", ci)}\n" +
            $"vr_hands_roll={_vr_hands_roll.ToString("F0", ci)}\n" +
            $"vr_hands_off_right={_vr_hands_off_r.ToString("F3", ci)}\n" +
            $"vr_hands_off_up={_vr_hands_off_u.ToString("F3", ci)}\n" +
            $"vr_hands_off_back={_vr_hands_off_b.ToString("F3", ci)}\n" +
            $"vr_hands_debug={(_vr_hands_debug & 7).ToString(ci)}\n" +   // bit 8 (the dump) is a one-shot, never saved
            $"vr_hands_tap={(_vr_hands_tap ? 1 : 0)}\n" +
            $"vr_hands_tap_sens={_vr_hands_tap_sens.ToString("F2", ci)}\n" +
            $"vr_hands_tap_dialogue={(_vr_hands_tap_dialogue ? 1 : 0)}\n" +
            $"vr_hands_tap_haptic={(_vr_hands_tap_haptic ? 1 : 0)}\n" +
            $"vr_hands_tap_point={(_vr_hands_tap_point ? 1 : 0)}\n" +
            $"vr_hands_tap_hold_grip={(_vr_hands_tap_hold_grip ? 1 : 0)}\n" +
            $"vr_hands_tap_button={_vr_hands_tap_button}\n" +
            $"vr_hands_tap_press_ms={_vr_hands_tap_press_ms.ToString(ci)}\n";
    }

    /// <summary>18 comma-separated floats into rc (false: empty, the wrong count, or not numbers).</summary>
    private static bool hands2_parse18(string s, float* rc) {
        if (string.IsNullOrWhiteSpace(s)) return false;
        var t = s.Split(',');
        if (t.Length != 18) return false;
        for (int i = 0; i < 18; i++) if (!float.TryParse(num(t[i]), System.Globalization.NumberStyles.Float, CI, out rc[i]) || !float.IsFinite(rc[i])) return false;
        return true;
    }
    /// <summary>rc as the ini text; "" when neither hand is calibrated (both 9 zeros).</summary>
    private static string hands2_fmt18(float* rc) {
        bool any = false; for (int i = 0; i < 18; i++) if (rc[i] != 0f) any = true;
        if (!any) return "";
        var sb = new System.Text.StringBuilder();
        for (int i = 0; i < 18; i++) { if (i > 0) sb.Append(','); sb.Append(rc[i].ToString("F5", CI)); }
        return sb.ToString();
    }

    /// <summary>Spec section 4, once: an ini with the old bone hands' hands_cal and no vr_hands_cal gets it in the new frame. The
    /// old capture's target frame was (f x t, t, -f), the new one is OpenXR's wrist frame (Y dorsal, Z proximal, X = Y x Z):
    /// Cal_new = Cal_old . Rz(+90 deg) for the left hand, . Rz(-90 deg) for the right, re-orthonormalised. A hand still at the
    /// old default (identity: never captured) stays uncalibrated.</summary>
    private void hands2_migrate() {
        if (_vr_hands_cal_read || _vr_hands_cal.Length > 0) return;
        float* old = stackalloc float[18];
        if (!hands2_parse18(_hands_cal_ini, old)) return;
        float* rc = stackalloc float[18];
        int moved = 0;
        for (int h = 0; h < 2; h++) {
            float* o = old + h * 9, n = rc + h * 9;
            bool ident = true; for (int i = 0; i < 9; i++) if (MathF.Abs(o[i] - ((i % 4) == 0 ? 1f : 0f)) > 1e-3f) ident = false;
            if (ident) { for (int i = 0; i < 9; i++) n[i] = 0f; continue; }
            // O . Rz(a): Rz(+90) = [0 -1 0; 1 0 0; 0 0 1], Rz(-90) = [0 1 0; -1 0 0; 0 0 1]; so column 0 <- +-column 1, column 1 <- -+column 0
            float s = h == 0 ? 1f : -1f;
            for (int r = 0; r < 3; r++) { n[r * 3 + 0] = s * o[r * 3 + 1]; n[r * 3 + 1] = -s * o[r * 3 + 0]; n[r * 3 + 2] = o[r * 3 + 2]; }
            if (!hands2_orthonormalize(n)) { for (int i = 0; i < 9; i++) n[i] = 0f; continue; }
            moved++;
        }
        if (moved == 0) return;
        _vr_hands_cal = hands2_fmt18(rc);
        string m = I($"settings: hands_cal migrated to vr_hands_cal (old bone-hands frame; {moved} hand{(moved > 1 ? "s" : "")})");
        _logger.Info(m); d7_log(m);
    }
    private static bool hands2_orthonormalize(float* R) {   // Gram-Schmidt on the columns, as native hv_orthonormalize
        float x0 = R[0], x1 = R[3], x2 = R[6], y0 = R[1], y1 = R[4], y2 = R[7];
        float lx = MathF.Sqrt(x0 * x0 + x1 * x1 + x2 * x2); if (!(lx > 1e-6f)) return false; x0 /= lx; x1 /= lx; x2 /= lx;
        float d = x0 * y0 + x1 * y1 + x2 * y2; y0 -= d * x0; y1 -= d * x1; y2 -= d * x2;
        float ly = MathF.Sqrt(y0 * y0 + y1 * y1 + y2 * y2); if (!(ly > 1e-6f)) return false; y0 /= ly; y1 /= ly; y2 /= ly;
        float z0 = x1 * y2 - x2 * y1, z1 = x2 * y0 - x0 * y2, z2 = x0 * y1 - x1 * y0;
        R[0] = x0; R[3] = x1; R[6] = x2; R[1] = y0; R[4] = y1; R[7] = y2; R[2] = z0; R[5] = z1; R[8] = z2;
        return true;
    }

    /// <summary>From push_all, every frame: the calibration both ways (the ini to native when the text changed: launch, a hand
    /// edit, reset; a capture back into the ini), the countdown, and the settings.</summary>
    private void hands2_push() {
        if (_fn_hands2_set == null) return;
        if (!_h2_migrate_done) { _h2_migrate_done = true; hands2_migrate(); }
        if (_fn_hands2_cal != null) {
            float* rc = stackalloc float[18];
            if (_h2_cal_pushed != _vr_hands_cal) {
                if (hands2_parse18(_vr_hands_cal, rc)) {
                    bool had0 = false, had1 = false; for (int i = 0; i < 9; i++) { had0 |= rc[i] != 0f; had1 |= rc[9 + i] != 0f; }
                    _fn_hands2_cal(rc, 1);
                    _fn_hands2_cal(rc, 0);   // N10: what native accepted (a rejected hand reads 9 zeros), so a bad value is not saved again
                    bool got0 = false, got1 = false; for (int i = 0; i < 9; i++) { got0 |= rc[i] != 0f; got1 |= rc[9 + i] != 0f; }
                    if ((had0 && !got0) || (had1 && !got1)) {
                        _logger.Warning($"settings: vr_hands_cal: the {(had0 && !got0 ? "left" : "right")} hand's value is not a rotation; that hand uses the default placement");
                        _vr_hands_cal = hands2_fmt18(rc);
                    }
                }
                else {
                    _fn_hands2_cal(null, 1);   // the default for both hands
                    if (_vr_hands_cal.Length > 0) { _logger.Warning($"settings: vr_hands_cal is not 18 numbers; the default placement is used ({_vr_hands_cal})"); _vr_hands_cal = ""; }
                }
                _h2_cal_pushed = _vr_hands_cal;
                _h2_cal_seen = _fn_hands2_cal(null, 0);
            }
            if (_h2_cal_at > 0 && _now >= _h2_cal_at) { _h2_cal_at = -1; _fn_hands2_cal(null, 2); }   // native captures at the next tick with a controller
            uint seen = _fn_hands2_cal(null, 0);
            if (seen != _h2_cal_seen) {   // a capture: persist it
                _h2_cal_seen = seen;
                _fn_hands2_cal(rc, 0);
                _vr_hands_cal = hands2_fmt18(rc); _h2_cal_pushed = _vr_hands_cal;
            }
        }
        int dbg = _vr_hands_debug & 7;
        if (_h2_dump_frames > 0) { _h2_dump_frames--; dbg |= 8; }
        const float d2r = 0.0174533f;
        var c = new VrHandsCfg {
            bytes = sizeof(VrHandsCfg), on = _vr_hands ? 1 : 0, views = _vr_hands_views | 1, debug = dbg,
            yaw = _vr_hands_yaw * d2r, pitch = _vr_hands_pitch * d2r, roll = _vr_hands_roll * d2r,
            off_r = _vr_hands_off_r, off_u = _vr_hands_off_u, off_b = _vr_hands_off_b,
            rest_curl = _vr_hands_rest_curl, scale = _vr_hands_scale };
        _h2_can_draw = _fn_hands2_set(&c, sizeof(VrHandsCfg)) != 0;
        hands2_pack_push();
        hands2_tap_push();
    }

    /// <summary>Tap build: the tap's settings and the game gate to native, every frame (native treats a gate older than 3
    /// frames as shut). Bits: 1 battle (Btl.battle_state != 0), 2 the sphere grid (Fahrenheit Globals.SphereGrid.is_open),
    /// 4 Fahrenheit's save / load screen (0x8CB994 not closed, or latched open), 8 a text box is up (a TOMesWinWork window of
    /// the current bank with a non-zero status), 16 blitzball (a bltz map), 32 riding (the followed actor is attached), 0x100 a read failed.
    /// gates (4 Nov, game_gates.cs, the game's own flags): 0x40 no player control, 0x80 a field menu, 0x800 a choice box, 0x1000
    /// a line fully shown (render side: a window in state 2), 0x2000 a movie (the skippable flag with the player playing). Native
    /// (ffxvr_gates.h tap_game_gate) shuts on any of them; a choice box always; a text box (and no control) unless "also advance
    /// dialogue" is on and the line is fully shown (8 with 0x1000).
    /// cpu-lean (b): read only when native can use them (TapPlan): all of them when the tap is on in first person and the frame
    /// can draw the hands; the view bits (battle, a field menu, a movie: native hides the hands on them) | 0x400 when the frame
    /// can draw them otherwise; nothing (0x200) when it cannot draw them (the virtual screen: menus, the sphere grid; the stereo
    /// screen; a backdrop room). Hands off costs nothing, as before.</summary>
    private void hands2_tap_push() {
        if (_fn_hands2_tap_set == null) return;
        bool on = _vr_hands && _vr_hands_tap;
        long t_tap = cpu_now();
        int plan = TapPlan.Of(_vr_hands, on, _h2_can_draw, _fp_mode); _tap_plan = plan;
        int game = plan == TapPlan.k_full ? hands2_tap_game() : plan == TapPlan.k_battle ? hands2_view_bits() | 0x400 : plan == TapPlan.k_none ? 0x200 : 0;
        _cpu_tap += cpu_now() - t_tap;
        int btn = _vr_hands_tap_button switch { "b" => 0x2000, "x" => 0x4000, "y" => 0x8000, _ => 0x1000 };
        var t = new VrTapCfg { bytes = sizeof(VrTapCfg), flags = (on ? 1 : 0) | (_vr_hands_tap_dialogue ? 2 : 0) | (_vr_hands_tap_haptic ? 4 : 0) | (_vr_hands_tap_point ? 8 : 0) | (_vr_hands_tap_hold_grip ? 16 : 0),
                               sens = _vr_hands_tap_sens, button = btn, game = game, press_ms = _vr_hands_tap_press_ms };
        _fn_hands2_tap_set(&t, sizeof(VrTapCfg));
        if (!on) { _tap_game_last = -1; return; }
        if ((game & 0x600) != 0) return;   // not read this frame: no event line (F8 says "not read this frame"), the last full read is kept for the next change
        if (game != _tap_game_last) {   // a change of the game gate: an event line (200 at most a session), so a test shows what the gate saw
            if (_tap_game_last >= 0 && _tap_game_logs < 200) {
                _tap_game_logs++;
                string m = I($"hands: tap gate (game): {hands2_tap_game_text(game)}{((game & 0x1808) != 0 ? " (" + _tap_text_info + gates_text_note(game) + ")" : "")}{(_tap_game_logs == 200 ? " (no more of these lines this session)" : "")}");
                _logger.Info(m); d7_log(m);
            }
            _tap_game_last = game;
        }
    }
    private static string hands2_tap_game_text(int g) {
        if (g == 0) return "the field (open)";
        var l = new List<string>();
        if ((g & 1) != 0) l.Add("battle"); if ((g & 2) != 0) l.Add("sphere grid"); if ((g & 4) != 0) l.Add("save / load screen");
        if ((g & 8) != 0) l.Add("a text box"); if ((g & 16) != 0) l.Add("blitzball"); if ((g & 32) != 0) l.Add("riding"); if ((g & 0x100) != 0) l.Add("not readable");
        if ((g & 0x2000) != 0) l.Add("a movie"); if ((g & 0x80) != 0) l.Add("a field menu"); if ((g & 0x800) != 0) l.Add("a choice box");
        if ((g & 0x1000) != 0) l.Add("a line fully shown"); if ((g & 0x40) != 0) l.Add("no player control");
        return string.Join(", ", l);
    }
    /// <summary>The log line's render-side part: the windows shown and the choice box.</summary>
    private string gates_text_note(int g) =>
        I($" | render side: windows open 0x{_gs_text.open:X2} shown 0x{_gs_text.shown:X2}, byte 0x{_gs_text_byte:X2}{((g & 0x800) != 0 ? I($", choice box slot {_gs_text.choice_slot} row {_gs_text.choice_row}") : "")}");
    /// <summary>Bit 1 of the gate alone: Btl.battle_state (byte at +0x10; 1 the battle is live, 0 none); 0x100 not readable.</summary>
    private int hands2_battle_bit() {
        if (_fn_safe_read == null) return 0x100;
        return !read((byte*)Globals.Battle.btl + 0x10, out byte bstate) ? 0x100 : bstate != 0 ? 1 : 0;
    }
    /// <summary>The game gate bits (hands2_tap_push). Every read guarded (native safe_read).</summary>
    private int hands2_tap_game() {
        if (_fn_safe_read == null) return 0x100;
        int g = hands2_view_bits() | gates_direct_bits();   // gates: battle, a field menu, a movie; no control, a choice box, a line fully shown
        if (!read(FhUtil.ptr_at<byte>(0x1685F70), out byte sg)) g |= 0x100; else if (sg != 0) g |= 2;   // Fahrenheit Globals.SphereGrid.is_open (FhUtil 0x1685F70)
        if (_save_st != 0 || _fh_menu_open) g |= 4;   // cpu-lean: push_all's read of this frame (save_screen_state)
        if (_in_blitz) g |= 16;
        if (_ride_att) g |= 32;   // riding (ride.cs, push_player runs first): no taps from the bird (the owner, 3 Nov: press A; you cannot touch anyone from up there)
        // Text boxes: TOMesWinWork[bank * 8 + i] at FhUtil 0xF26D70 (0x2C bytes each), the bank a signed byte at FhUtil 0xF26B82
        // (getter 0x86BE90). The game's own per-frame loop (0x871F30) copies each window's state into +0x14 (u16; 4 = closed is
        // written back as 0) and counts a window with a non-zero state and +0x1D bit 1 as one the player waits on; it writes that
        // result into bit 2 of byte +1 of the current Atel controller ([FhUtil 0xF26B28], Fahrenheit Atel.current_controller;
        // 0x872025..0x87204C). Review S3: either signal gates (any non-zero state: the stricter reading; or the game's own bit).
        if (!read(FhUtil.ptr_at<sbyte>(0xF26B82), out sbyte bank) || bank < 0 || bank > 15) { g |= 0x100; tap_text_set(-1, 0, 0, 0, false); return g; }
        byte* w0 = FhUtil.ptr_at<byte>(0xF26D70) + bank * 8 * 0x2C;
        ulong st8 = 0; uint fl8 = 0; int open = 0;   // 8 states (16 bits each would not fit: the low byte of each is kept for the signature) and flags
        byte* wb = stackalloc byte[8 * 0x2C];   // gates: the bank's 8 windows in one guarded read (was 16)
        if (_fn_safe_read(w0, wb, 8 * 0x2C) == 0) g |= 0x100;
        else for (int i = 0; i < 8; i++) {
            ushort st = (ushort)(wb[i * 0x2C + 0x14] | wb[i * 0x2C + 0x15] << 8); byte fl = wb[i * 0x2C + 0x1D];
            if (st != 0) { g |= 8; open |= 1 << i; st8 |= (ulong)(st & 0xFF) << (i * 8); fl8 ^= (uint)fl << (i % 4 * 8); }
        }
        bool wait = false;   // the game's own "a window waits on the player" bit (null controller: no event running, nothing to read)
        if (read(FhUtil.ptr_at<uint>(0xF26B28), out uint ctl) && ctl != 0 && read((byte*)(nint)ctl + 1, out byte cb1)) wait = (cb1 & 4) != 0;
        if (wait) g |= 8;
        tap_text_set(bank, open, st8, fl8, wait);
        return g;
    }
    // Review N4: the F8 / log text of the text boxes is rebuilt only when what was read changes (no per-frame strings).
    private long _tap_text_sig = long.MinValue;
    private void tap_text_set(int bank, int open, ulong st8, uint fl8, bool wait) {
        long sig = (long)((ulong)bank & 0xFF) ^ ((long)open << 8) ^ (long)(st8 * 31) ^ ((long)fl8 << 17) ^ (wait ? 1L << 62 : 0);
        if (sig == _tap_text_sig) return;
        _tap_text_sig = sig;
        if (bank < 0) { _tap_text_info = "the text box bank is not readable"; return; }
        var sb = new System.Text.StringBuilder();
        byte* w0 = FhUtil.ptr_at<byte>(0xF26D70) + bank * 8 * 0x2C;
        for (int i = 0; i < 8; i++) {
            if ((open & (1 << i)) == 0) continue;
            read(w0 + i * 0x2C + 0x14, out ushort st); read(w0 + i * 0x2C + 0x1D, out byte fl);
            if (sb.Length > 0) sb.Append(", ");
            sb.Append(I($"#{i} state {st} flags 0x{fl:X2}"));
        }
        _tap_text_info = I($"bank {bank}: {(sb.Length > 0 ? sb.ToString() : "none")} | the game's waiting bit {(wait ? "set" : "clear")}");
    }

    /// <summary>S3: reads <mod>\hands\ (every pack folder, its hand.json; hand_pack.cs) and logs what it found. At the first
    /// push and on "rescan"; a rescan also forgets the native loader's rejections and walks the selection again (a pack
    /// re-made on disk has a new stamp, so native loads it again).</summary>
    private void hands2_scan(bool rescan) {
        _h2_scan_tried = true;   // once by itself; again only on "rescan" (a failed listing is not retried every frame)
        string dir = _h2_mod_dir.Length > 0 ? Path.Join(_h2_mod_dir, "hands") : "";
        string? note = "no mod folder";
        List<HandPack> found;
        try { found = dir.Length > 0 ? HandPackJson.scan(dir, out note) : new List<HandPack>(); }
        catch (Exception e) {   // review B1: never past here (scan catches per folder; this is the folder listing itself)
            string m0 = "hands: the hand packs could not be listed: " + e.Message; _logger.Warning(m0); d7_log(m0); return;   // the old list stays; retried at the next rescan
        }
        _h2_packs = found; _h2_scanned = true;   // only a finished scan counts
        var sb = new System.Text.StringBuilder();
        int usable = 0;
        foreach (HandPack k in _h2_packs) {
            if (sb.Length > 0) sb.Append(", ");
            sb.Append(k.id); if (k.priv) sb.Append(" [private]"); if (k.error != null) sb.Append(" [unusable]");
            if (k.error == null) usable++;
        }
        string m = I($"hands: {(rescan ? "rescan: " : "")}packs found {_h2_packs.Count} ({(sb.Length > 0 ? sb.ToString() : "none")}){(usable < _h2_packs.Count ? I($", {usable} usable") : "")} | selected {_vr_hands_pack}{(note != null ? " | " + note : "")}");
        _logger.Info(m); d7_log(m);
        foreach (HandPack k in _h2_packs) {
            if (k.error != null) { string e = $"hands: pack '{k.id}' cannot be used: {k.error}"; _logger.Warning(e); d7_log(e); }
            if (k.notes.Count > 0) { string e = $"hands: pack '{k.id}' hand.json: {string.Join("; ", k.notes)}"; _logger.Info(e); d7_log(e); }
        }
        if (rescan) { _h2_pack_want = ""; _h2_pack_sent = null; _h2_pack_sent_p = null; }   // walk again from the selection
    }
    private HandPack? hands2_find(string id) => _h2_packs.Find(k => string.Equals(k.id, id, StringComparison.OrdinalIgnoreCase));

    /// <summary>Which pack native draws: the selected one (vr_hands_pack), else generic-glove, else generic, else none (the boxes),
    /// walked once per selection (or rescan): a pack missing or unusable here, or rejected by native (status [11] = -1 for the
    /// pack it was given: the reason is kept on the pack, greyed out in F8), moves to the next. Native loads it at Present while
    /// vr_hands=1 and keeps drawing the old pack until the new one is in, so this only says where the files are.</summary>
    private void hands2_pack_push() {
        if (_fn_hands2_pack == null) return;
        if (!_h2_scan_tried) hands2_scan(false);
        if (!string.Equals(_h2_pack_want, _vr_hands_pack, StringComparison.Ordinal)) { _h2_pack_want = _vr_hands_pack; _h2_pack_try = 0; _h2_pack_sent = null; }
        if (!string.IsNullOrEmpty(_h2_pack_sent) && _fn_hands2_status != null) {
            int* v = stackalloc int[k_h2_status_n];
            _fn_hands2_status(v, k_h2_status_n);
            if (v[11] == -1) {   // native rejected it (it logged why): remember the reason, the next candidate
                string why = "";
                if (_fn_hands2_pack_error != null) { byte* e = _fn_hands2_pack_error(); if (e != null) why = Marshal.PtrToStringUTF8((nint)e) ?? ""; }
                if (_h2_pack_sent_p != null) _h2_pack_sent_p.native_error = why.Length > 0 ? why : "the loader rejected it (see ffxvr_diag.log)";
                _h2_pack_try++; _h2_pack_sent = null;
            }
        }
        if (_h2_pack_sent != null) return;
        var order = new List<string> { _vr_hands_pack };
        foreach (string sh in HandPackJson.Shipped) if (!order.Exists(o => string.Equals(o, sh, StringComparison.OrdinalIgnoreCase))) order.Add(sh);
        while (_h2_pack_try < order.Count) {
            string name = order[_h2_pack_try];
            HandPack? k = hands2_find(name);
            if (k != null && k.usable && _h2_mod_dir.Length > 0) {
                if (_h2_pack_try > 0) { string m = $"hands: using the pack '{k.id}' instead of '{order[0]}'"; _logger.Info(m); d7_log(m); }
                hands2_pack_send(k);
                _h2_pack_sent = k.id; _h2_pack_sent_p = k;
                return;
            }
            string miss = k == null ? (Array.Exists(HandPackJson.Shipped, x => string.Equals(x, name, StringComparison.OrdinalIgnoreCase)) ? $"hands: pack '{name}' not found (hands\\{name}\\ in the mod folder; reinstall the mod)" : $"hands: pack '{name}' not found under hands\\ in the mod folder")
                        : $"hands: pack '{k.id}' skipped: {k.error ?? k.native_error}";
            _logger.Warning(miss); d7_log(miss);
            _h2_pack_try++;
        }
        _fn_hands2_pack(null, 0);   // nothing usable: the boxes
        _h2_pack_sent = ""; _h2_pack_sent_p = null;
        string none = $"hands: no usable hand pack ({string.Join(", ", order)}): the boxes are drawn";
        _logger.Warning(none); d7_log(none);
    }
    private void hands2_pack_send(HandPack k) {
        var d = new VrHandsPackDesc { bytes = sizeof(VrHandsPackDesc), format = 1, mirror = k.mirror ? 1 : 0, scale = k.scale, stamp = k.stamp };
        const float d2r = 0.0174533f;
        for (int i = 0; i < 3; i++) { d.wrist_off_m[i] = k.off_m[i]; d.wrist_rot_rad[i] = k.rot_deg[i] * d2r; d.tint[i] = k.tint[i]; }
        for (int i = 0; i < 5; i++) d.rest_curl[i] = k.rest[i];
        string name = k.id;   // the folder name: the log's and the status line's (native keeps printable ASCII, the rest '_')
        int n = Math.Min(name.Length, 63); byte* nb = stackalloc byte[n + 1];
        for (int i = 0; i < n; i++) { char ch = name[i]; nb[i] = ch >= 32 && ch < 127 ? (byte)ch : (byte)'_'; }
        nb[n] = 0; d.name = nb;
        fixed (char* pr = k.right, pl = k.left) { d.right_glb = pr; d.left_glb = pl; _fn_hands2_pack(&d, sizeof(VrHandsPackDesc)); }
    }

    /// <summary>The native status line ("drawing | generic-glove 2144 v | eye1 1198 eye2 1198 / 1200 frames (last 10 s) | last
    /// hidden: ..."), and v[0..20].</summary>
    private string hands2_status(int* v) {
        for (int i = 0; i < k_h2_status_n; i++) v[i] = 0;
        if (_fn_hands2_status == null) return "";
        byte* s = _fn_hands2_status(v, k_h2_status_n);
        return s != null ? Marshal.PtrToStringAnsi((nint)s) ?? "" : "";
    }

    /// <summary>The main window: a collapsible "VR hands (experimental)" header under the view mode and turning controls.</summary>
    private void hands2_ui() {
        if (_fn_hands2_set == null) return;
        if (!ImGui.CollapsingHeader("VR hands (experimental)##h2")) return;
        ImGui.Checkbox("VR hands: your own hands at the controllers##h2on", ref _vr_hands);
        int* v = stackalloc int[k_h2_status_n];
        string st = hands2_status(v);
        if (st.Length > 0) { ImGui.SameLine(); ImGui.TextDisabled(esc(st)); }
        bool body = _fp_body_mode != 0 || !_fp_hide_body || (_hands_dev && _hands_on);
        if (_vr_hands && body) {
            ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.8f, 0.3f, 1f));
            ImGui.TextWrapped("Not drawn in first person while your body is shown: pick \"hide my whole body\" and tick \"hide the player's body\" (Debug > First person / follow) to see the hands"
                              + (_hands_dev && _hands_on ? ", and turn the old bone hands off." : "."));
            ImGui.PopStyleColor(1);
        }
        if (_vr_hands && _h2_pack_sent == "") {   // S3: the whole fallback list failed (a single rejection moves on to the next pack)
            ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.8f, 0.3f, 1f));
            ImGui.TextWrapped("No hand model could be loaded (see ffxvr_diag.log): boxes are drawn instead. Reinstalling the mod puts hands\\generic-glove and hands\\generic back.");
            ImGui.PopStyleColor(1);
        }
        hands2_pack_ui(v);
        ImGui.Text("show in: first person (with your body hidden), and"); ImGui.SameLine();
        bool dio = (_vr_hands_views & 2) != 0, tp = (_vr_hands_views & 4) != 0, seat = (_vr_hands_views & 8) != 0;
        if (ImGui.Checkbox("the diorama##h2v2", ref dio)) _vr_hands_views = dio ? _vr_hands_views | 2 : _vr_hands_views & ~2;
        ImGui.SameLine(); if (ImGui.Checkbox("third person##h2v4", ref tp)) _vr_hands_views = tp ? _vr_hands_views | 4 : _vr_hands_views & ~4;
        ImGui.SameLine(); if (ImGui.Checkbox("the seat view##h2v8", ref seat)) _vr_hands_views = seat ? _vr_hands_views | 8 : _vr_hands_views & ~8;
        ImGui.TextDisabled("In the diorama and the seat view the hands are your own size in the scaled world: life-size to you, giant beside the miniature, and they go behind its walls and ground like a real hand would.");
        ImGui.TextDisabled("Never in battle, on screens, menus, FMVs, battle transitions or under the dream filter. A controller put down for 10 s (nothing touched) hides its hand (with the touch sensors only).");
        ImGui.SetNextItemWidth(160); ImGui.SliderFloat("idle finger curl##h2rc", ref _vr_hands_rest_curl, 0f, 1f, "%.2f"); ImGui.SameLine();
        ImGui.SetNextItemWidth(160); ImGui.SliderFloat("size##h2sc", ref _vr_hands_scale, 0.8f, 1.25f, "%.2f x"); ImGui.SameLine();
        if (ImGui.SmallButton("defaults##h2rs")) { _vr_hands_rest_curl = 0.20f; _vr_hands_scale = 1.00f; }
        if (_vr_hands && v[16] == 0 && v[5] == 1) ImGui.TextDisabled("The controllers' touch sensors are not bound (another runtime or profile?): the fingers follow the trigger and grip only, and a controller put down is not hidden.");
        if (_fn_hands2_cal != null) {
            if (_h2_cancels_seen >= 0 && v[10] != _h2_cancels_seen) _h2_cancel_note_until = _now + 10.0;   // SF-2: native dropped a late request
            _h2_cancels_seen = v[10];
            if (_h2_cal_at > 0) {
                ImGui.Text(I($"{(int)Math.Ceiling(_h2_cal_at - _now)} ... hold both arms STRAIGHT OUT IN FRONT, shoulder width, PALMS DOWN, fingers forward"));
                ImGui.SameLine(); if (ImGui.SmallButton("cancel##h2cal")) _h2_cal_at = -1;
            }
            else if (v[9] != 0) ImGui.Text("capturing ...");
            else if (ImGui.Button("calibrate: 10 s countdown##h2cal")) _h2_cal_at = _now + 10.0;
            ImGui.SameLine(); if (ImGui.SmallButton("reset calibration##h2cal")) _vr_hands_cal = "";   // hands2_push sends the default
            ImGui.SameLine();
            ImGui.TextDisabled(I($"calibrated: left {((v[7] & 1) != 0 ? "yes" : "no (default)")}, right {((v[7] & 2) != 0 ? "yes" : "no (default)")}"));
            if (_now < _h2_cancel_note_until) {
                ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.8f, 0.3f, 1f));
                ImGui.TextWrapped("The last calibration was cancelled: the countdown ended without a 3D frame and a tracked controller (a menu, a movie or a load?). Nothing changed; try again in the field.");
                ImGui.PopStyleColor(1);
            }
        }
        hands2_tap_ui();
        if (ImGui.TreeNode("fine tune##h2ft")) {
            ImGui.SetNextItemWidth(110); ImGui.SliderFloat("yaw##h2", ref _vr_hands_yaw, -45f, 45f, "%.0f deg"); ImGui.SameLine();
            ImGui.SetNextItemWidth(110); ImGui.SliderFloat("pitch##h2", ref _vr_hands_pitch, -45f, 45f, "%.0f deg"); ImGui.SameLine();
            ImGui.SetNextItemWidth(110); ImGui.SliderFloat("roll##h2", ref _vr_hands_roll, -45f, 45f, "%.0f deg");
            ImGui.SetNextItemWidth(110); ImGui.SliderFloat("out##h2", ref _vr_hands_off_r, -0.10f, 0.10f, "%.3f m"); ImGui.SameLine();
            ImGui.SetNextItemWidth(110); ImGui.SliderFloat("up##h2", ref _vr_hands_off_u, -0.10f, 0.10f, "%.3f m"); ImGui.SameLine();
            ImGui.SetNextItemWidth(110); ImGui.SliderFloat("back##h2", ref _vr_hands_off_b, -0.10f, 0.10f, "%.3f m"); ImGui.SameLine();
            if (ImGui.SmallButton("reset##h2ft")) { _vr_hands_yaw = _vr_hands_pitch = _vr_hands_roll = 0f; _vr_hands_off_r = _vr_hands_off_u = _vr_hands_off_b = 0f; }
            ImGui.TextDisabled("In the hand's own frame: yaw turns about the back of the hand, pitch tips the fingers up or down, roll turns about the fingers; out, up and back move it from the thumb side, out of the back of the hand, toward the elbow.");
            ImGui.TreePop();
        }
    }

    /// <summary>Tap build: the "gestures" sub-section: the tap's options and a live readout (the fingertip's forward speed, the
    /// point pose, the gate, the last tap and near miss, the counters).</summary>
    private void hands2_tap_ui() {
        if (_fn_hands2_tap_set == null || !ImGui.TreeNode("gestures##h2g")) return;
        ImGui.Checkbox("tap to press action (squeeze the grip and push your hand forward)##h2tap", ref _vr_hands_tap);
        ImGui.SetNextItemWidth(160); ImGui.SliderFloat("tap sensitivity##h2ts", ref _vr_hands_tap_sens, 0.5f, 2f, "%.2f"); ImGui.SameLine();
        _vr_hands_tap_sens = float.IsFinite(_vr_hands_tap_sens) ? Math.Clamp(_vr_hands_tap_sens, 0.5f, 2f) : 1f;   // review N2: Ctrl+click typing
        ImGui.TextDisabled(I($"(tap distance: a push of {10f / _vr_hands_tap_sens:F0} cm)")); ImGui.SameLine();
        if (ImGui.SmallButton("default##h2ts")) _vr_hands_tap_sens = 1.00f;
        ImGui.Checkbox("also advance dialogue (tap while a text box is up)##h2tapd", ref _vr_hands_tap_dialogue); ImGui.SameLine();
        ImGui.Checkbox("haptic tick##h2taph", ref _vr_hands_tap_haptic);
        ImGui.Checkbox("require a pointing finger (index off the trigger)##h2tapp", ref _vr_hands_tap_point); ImGui.SameLine();
        ImGui.Checkbox("don't pass the grip to the game while a tap can fire (LB / RB)##h2tapg", ref _vr_hands_tap_hold_grip);
        ImGui.TextDisabled(esc("Squeeze the grip and push your hand forward about a hand's width (" + (10f / _vr_hands_tap_sens).ToString("F0", CI) + " cm) toward what you are facing. "
                           + "It presses " + _vr_hands_tap_button.ToUpperInvariant() + (_vr_hands_tap_button == "a" ? " (confirm: talk, examine, open)" : " (vr_hands_tap_button in the ini; A is confirm)") + ". Only in first person in the field: never in battle, menus, the sphere grid, the save screen, movies or blitzball, "
                           + "while you turn with the stick or move your head fast, while you have no control of the character or a choice box is up, or (unless ticked above) while a text box is up. A reach with the grip squeezed presses too: let go of the grip when you don't mean it. "
                           + "With \"don't pass the grip\" ticked, a grip squeezed where a tap can fire is held back from the game (LB / RB) until you let go."));
        if (_fn_hands2_tap_status != null) {
            float* v = stackalloc float[k_tap_status_n];
            for (int i = 0; i < k_tap_status_n; i++) v[i] = 0f;
            byte* line = _fn_hands2_tap_status(v, k_tap_status_n);
            string ln = line != null ? Marshal.PtrToStringAnsi((nint)line) ?? "" : "";
            string arm = v[2] > 0.5f && v[3] > 0.5f ? "armed (grip L + R)" : v[3] > 0.5f ? "armed (grip R)" : v[2] > 0.5f ? "armed (grip L)" : "not armed (squeeze a grip)";
            ImGui.Text(esc(I($"{arm} | push forward: L {v[15] * 100f,4:F1}  R {v[16] * 100f,4:F1} cm of {v[7] * 100f:F0} cm (the last push stays 2 s){(v[8] > 0.5f ? " | PRESSING" : "")}{(v[17] > 0.5f ? " | grips held back from the game" : "")}")));
            ImGui.TextDisabled(esc(ln));
            ImGui.TextDisabled(esc(I($"taps: L {v[5]:F0} R {v[6]:F0}, not taps {v[9]:F0} | game: {(!(_vr_hands && _vr_hands_tap) ? "-" : _tap_plan == TapPlan.k_none ? "not read this frame (hands can't show)" : _tap_plan == TapPlan.k_battle ? "not read this frame (no tap outside first person)" : hands2_tap_game_text(_tap_game_last < 0 ? 0 : _tap_game_last))} | text boxes: {(_tap_text_info.Length > 0 ? _tap_text_info : "-")}")));
            ImGui.TextDisabled(esc(gates_readout()));   // gates: the game's own flags as last read (the movie id read now)
            if (_gs_pick_last.Length > 0) ImGui.TextDisabled(esc("last pick mismatch: " + _gs_pick_last));
        }
        ImGui.TreePop();
    }

    /// <summary>S3: the "hand model" combo (every pack: name, author, licence; "(private)"; an unusable one greyed out with
    /// the reason as its tooltip), "rescan", and what the drawn pack is.</summary>
    private void hands2_pack_ui(int* v) {
        if (!_h2_scan_tried && _fn_hands2_pack != null) hands2_scan(false);
        HandPack? sel = hands2_find(_vr_hands_pack);
        ImGui.Text("hand model:"); ImGui.SameLine(); ImGui.SetNextItemWidth(340);
        string preview = sel != null ? sel.label : _vr_hands_pack + " (not found)";
        if (ImGui.BeginCombo("##h2pk", preview)) {
            for (int i = 0; i < _h2_packs.Count; i++) {
                HandPack k = _h2_packs[i];
                bool ok = k.usable;
                if (!ok) ImGui.BeginDisabled(true);
                if (ImGui.Selectable(k.label + (ok ? "" : "  (cannot be used)") + "##h2pk" + i.ToString(CI), sel == k) && ok) _vr_hands_pack = k.id;
                if (!ok) ImGui.EndDisabled();
                if (ImGui.IsItemHovered(ImGuiHoveredFlags.AllowWhenDisabled))
                    ImGui.SetTooltip(esc((ok ? "" : "Cannot be used: " + (k.error ?? k.native_error) + "\n") + "folder hands\\" + k.id + (k.source.Length > 0 ? "\nsource: " + k.source : "")
                                         + (k.priv ? "\nMade from a model you own: keep it on this PC, do not share it." : "")));
            }
            if (_h2_packs.Count == 0) ImGui.TextDisabled("no hand packs in the mod folder's hands\\ (reinstall the mod)");
            ImGui.EndCombo();
        }
        ImGui.SameLine(); if (ImGui.SmallButton("rescan##h2pk")) hands2_scan(true);
        if (_h2_pack_try > 0 && !string.IsNullOrEmpty(_h2_pack_sent)) {   // review N6: the fallback is drawn, not the selection
            ImGui.SameLine(); ImGui.TextDisabled(esc($"(using '{_h2_pack_sent}': '{_vr_hands_pack}' could not be loaded)"));
        }
        if (sel != null) {
            string by = (sel.author.Length > 0 ? "by " + sel.author : "") + (sel.licence.Length > 0 ? (sel.author.Length > 0 ? " | " : "") + "licence: " + sel.licence : "");
            string load = v[11] == 2 && _h2_pack_sent_p == sel ? I($" | {v[12]} vertices, {v[17]} texture{(v[17] == 1 ? "" : "s")}{(v[18] > 0 ? I($" ({v[18]} failed: base colour)") : "")}, loaded in {v[19] / 10.0:F1} ms") : v[11] == 1 ? " | loading ..." : "";
            ImGui.TextDisabled(esc(by + load));
            if (sel.priv) ImGui.TextDisabled("Made from a model you own: it stays on this PC. Do not share the pack folder.");
        }
        ImGui.TextDisabled("Your own hands from a model you own: tools\\import-hands.ps1 (README: \"Your own hands\"), then rescan.");
    }

    /// <summary>Debug > First person / follow: the vr_hands_debug bits and the one-shot pose dump.</summary>
    private void hands2_debug_ui() {
        if (_fn_hands2_set == null) return;
        ImGui.Text("VR hands v2:"); ImGui.SameLine();
        bool axes = (_vr_hands_debug & 1) != 0, boxes = (_vr_hands_debug & 2) != 0, lines = (_vr_hands_debug & 4) != 0;
        if (ImGui.Checkbox("wrist axes (red X, green Y out of the back of the hand, blue Z toward the elbow)##h2d1", ref axes)) _vr_hands_debug = axes ? _vr_hands_debug | 1 : _vr_hands_debug & ~1;
        ImGui.SameLine();
        if (ImGui.Checkbox("boxes instead of the hand mesh##h2d2", ref boxes)) _vr_hands_debug = boxes ? _vr_hands_debug | 2 : _vr_hands_debug & ~2;
        ImGui.SameLine();
        if (ImGui.Checkbox("a placement line per drawn frame in ffxvr_diag.log (300 at most)##h2d4", ref lines)) _vr_hands_debug = lines ? _vr_hands_debug | 4 : _vr_hands_debug & ~4;
        ImGui.SameLine(); if (ImGui.SmallButton("log one pose dump##h2d8")) _h2_dump_frames = 120;   // the next drawn frame within about 2 s
    }
}

/// <summary>cpu-lean (b): which of the tap gate's game bits hands2_tap_push reads this frame. Native uses them only on a frame
/// that can draw the hands (can_draw: ffxvr_hands2_set's answer; otherwise an earlier hide rule decides): bit 1 for the battle
/// hide, and the rest for the tap, whose gate is open only in first person (native's view is fp_set's mode, which push_all
/// sends after this with the same _fp_mode). Kept free of game types for the scratch test.</summary>
internal static class TapPlan {
    public const int k_off = 0;      // VR hands off: 0, as before
    public const int k_none = 1;     // nothing read: 0x200 (native hides the hands and keeps the gate shut)
    public const int k_battle = 2;   // only the view bits (battle, a field menu, a movie; gates): | 0x400 (native keeps the gate shut)
    public const int k_full = 3;     // every bit, as before
    public static int Of(bool vr_hands, bool tap_on, bool can_draw, int fp_mode) =>
        !vr_hands ? k_off : !can_draw ? k_none : tap_on && fp_mode == 1 ? k_full : k_battle;
}
