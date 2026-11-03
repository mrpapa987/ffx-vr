// SPDX-License-Identifier: MIT

namespace FFXVR;

// VR hands v2, stages S1 and S2 (the spec: research hands-v2-spec.md; the native half: ffxvr_native.cpp "VR hands v2",
// ffxvr_hands.cpp). Our own hands drawn at the controllers, in the scene, in both eyes, depth-tested against the world and
// under the UI panel. S2 draws the shipped generic hand (<mod>\hands\generic-glove or generic: tools/hands/make-generic-hand.py,
// ours, MIT), skinned on the GPU, its fingers following the controllers' trigger, grip and touch sensors; the S1 boxes are
// the fallback (no pack, a pack that failed to load) and a debug option. User hand packs come in S3. Off by default.
//
// The owner's rules (2 Nov, 3 Nov 2026): in first person only with the player's body hidden (fp_body_mode 0, "hide my whole
// body", the default; no forearm cutting), and by choice in the diorama ("to give our hands some context when adjusting the
// view"), third person and the seat view, where Tidus is part of the scene. No optical hand tracking.
//
// ini keys (all applied at once):
//   vr_hands            0/1, default 0: the master switch. 0 costs one branch per resolve: no shaders, nothing drawn.
//   vr_hands_views      bits, default 1: 1 first person (always), 2 the diorama, 4 third person, 8 the seat view.
//   vr_hands_pack       generic-glove (default) | generic: the look (S2: only the two shipped packs; the other is the fallback).
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
// Not here (spec 1.4, owner's decisions): the light / tracking keys of the later stages (tracking dropped).
public sealed unsafe partial class FFXVRModule {

    [StructLayout(LayoutKind.Sequential)]
    private struct VrHandsCfg { public int bytes, on, views, debug; public float yaw, pitch, roll, off_r, off_u, off_b, rest_curl, scale; }   // = native VrHandsCfg (ABI 28)
    [StructLayout(LayoutKind.Sequential)]
    private struct VrHandsPackDesc {   // = native VrHandsPackDesc (ABI 28; 84 bytes on x86): absolute UTF-16 paths, copied by native at once
        public int bytes, format; public char* right_glb, left_glb; public int mirror; public float scale;
        public fixed float wrist_off_m[3]; public fixed float wrist_rot_rad[3]; public fixed float tint[3]; public fixed float rest_curl[5]; public byte* name;
    }
    private const int k_h2_status_n = 17;           // ffxvr_hands2_status's values (ABI 28)

    private bool   _vr_hands = false;            // ini vr_hands (default 0)
    private int    _vr_hands_debug = 0;          // ini vr_hands_debug (bits 1, 2, 4)
    private int    _vr_hands_views = 1;          // ini vr_hands_views (bit 0 first person, always; 2 diorama, 4 third person, 8 seat)
    private string _vr_hands_pack = "generic-glove";   // ini vr_hands_pack (S2: generic-glove | generic)
    private float  _vr_hands_rest_curl = 0.20f, _vr_hands_scale = 1.00f;
    private string _h2_mod_dir = "";             // the mod folder (load_native): the packs are under <mod>\hands\
    private string? _h2_pack_sent;               // the pack native was last given ("" = none: the boxes); null = choose again
    private string _h2_pack_want = "";           // the selection the fallback walk below is for
    private int    _h2_pack_try;                 // the candidate being tried (the selected pack, then the other shipped one)
    private string _vr_hands_cal = "";           // ini vr_hands_cal ("" = the default placement)
    private bool   _vr_hands_cal_read;           // the key was in the file (the hands_cal migration runs only without it)
    private float  _vr_hands_yaw, _vr_hands_pitch, _vr_hands_roll;      // degrees
    private float  _vr_hands_off_r, _vr_hands_off_u, _vr_hands_off_b;   // metres
    private string? _h2_cal_pushed;              // the vr_hands_cal text native has (null: nothing sent yet)
    private uint   _h2_cal_seen;                 // native's capture count when last read
    private double _h2_cal_at = -1;              // the calibration countdown's end (_now), -1: none
    private bool   _h2_migrate_done;
    private int    _h2_dump_frames;              // "log one pose dump": debug bit 8 held for this many frames (native acts on its rising edge)
    private int    _h2_cancels_seen = -1;        // native's cancelled-calibration count when last seen (SF-2)
    private double _h2_cancel_note_until = -1;   // show "cancelled" in the header until this _now
    private delegate* unmanaged[Cdecl]<nint, uint, nint, uint, uint, void> _fn_pre_resolve;   // ABI 27
    private delegate* unmanaged[Cdecl]<VrHandsCfg*, int, void> _fn_hands2_set;               // ABI 27
    private delegate* unmanaged[Cdecl]<int*, int, byte*> _fn_hands2_status;                  // ABI 27: (out, n) -> the status line
    private delegate* unmanaged[Cdecl]<float*, int, uint> _fn_hands2_cal;                    // ABI 27: op 0 read, 1 write, 2 capture -> captures
    private delegate* unmanaged[Cdecl]<VrHandsPackDesc*, int, void> _fn_hands2_pack;        // ABI 28: the pack to draw (null: none, the boxes)

    /// <summary>settings_parse's default branch: the vr_hands_* keys. False: not ours.</summary>
    private bool hands2_parse(string k, string v) {
        switch (k) {
            case "vr_hands":           pb(k, v, ref _vr_hands); return true;
            case "vr_hands_debug":     pi(k, v, ref _vr_hands_debug, 0, 15); return true;
            case "vr_hands_views":     pi(k, v, ref _vr_hands_views, 0, 15); _vr_hands_views |= 1; return true;
            case "vr_hands_pack":      { string t = v.Trim().ToLowerInvariant(); if (t == "generic" || t == "generic-glove") _vr_hands_pack = t;
                                         else _logger.Warning($"settings: vr_hands_pack={v}: this build ships generic-glove and generic only (own packs come later); generic-glove is used"); } return true;
            case "vr_hands_rest_curl": pf(k, v, ref _vr_hands_rest_curl, 0f, 1f); return true;
            case "vr_hands_scale":     pf(k, v, ref _vr_hands_scale, 0.8f, 1.25f); return true;
            case "vr_hands_cal":       _vr_hands_cal = v.Trim(); _vr_hands_cal_read = true; return true;
            case "vr_hands_yaw":       pf(k, v, ref _vr_hands_yaw, -45f, 45f); return true;
            case "vr_hands_pitch":     pf(k, v, ref _vr_hands_pitch, -45f, 45f); return true;
            case "vr_hands_roll":      pf(k, v, ref _vr_hands_roll, -45f, 45f); return true;
            case "vr_hands_off_right": pf(k, v, ref _vr_hands_off_r, -0.10f, 0.10f); return true;
            case "vr_hands_off_up":    pf(k, v, ref _vr_hands_off_u, -0.10f, 0.10f); return true;
            case "vr_hands_off_back":  pf(k, v, ref _vr_hands_off_b, -0.10f, 0.10f); return true;
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
            $"vr_hands_debug={(_vr_hands_debug & 7).ToString(ci)}\n";   // bit 8 (the dump) is a one-shot, never saved
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
        _fn_hands2_set(&c, sizeof(VrHandsCfg));
        hands2_pack_push();
    }

    /// <summary>S2: which pack native draws. The selected one (vr_hands_pack), else the other shipped one, else none (the boxes),
    /// walked once per selection: a missing right.glb here, or native's "failed" (status [11] = -1) for the pack it was given,
    /// moves to the next. Native loads it at Present while vr_hands=1, so this only says where the files are.</summary>
    private void hands2_pack_push() {
        if (_fn_hands2_pack == null) return;
        if (_h2_pack_want != _vr_hands_pack) { _h2_pack_want = _vr_hands_pack; _h2_pack_try = 0; _h2_pack_sent = null; }
        if (!string.IsNullOrEmpty(_h2_pack_sent) && _fn_hands2_status != null) {
            int* v = stackalloc int[k_h2_status_n];
            _fn_hands2_status(v, k_h2_status_n);
            if (v[11] == -1) { _h2_pack_try++; _h2_pack_sent = null; }   // native rejected it (it logged why): the next candidate
        }
        if (_h2_pack_sent != null) return;
        string[] order = _vr_hands_pack == "generic" ? new[] { "generic", "generic-glove" } : new[] { "generic-glove", "generic" };
        while (_h2_pack_try < order.Length) {
            string name = order[_h2_pack_try];
            string path = _h2_mod_dir.Length > 0 ? Path.Join(_h2_mod_dir, "hands", name, "right.glb") : "";
            if (path.Length > 0 && File.Exists(path)) {
                if (_h2_pack_try > 0) { string m = I($"hands: using the pack '{name}' instead of '{order[0]}'"); _logger.Info(m); d7_log(m); }
                hands2_pack_send(name, path);
                _h2_pack_sent = name;
                return;
            }
            string miss = I($"hands: pack '{name}' not found (hands\\{name}\\right.glb in the mod folder; reinstall the mod)");
            _logger.Warning(miss); d7_log(miss);
            _h2_pack_try++;
        }
        _fn_hands2_pack(null, 0);   // nothing usable: the boxes
        _h2_pack_sent = "";
        string none = "hands: no usable hand pack (generic-glove, generic): the boxes are drawn";
        _logger.Warning(none); d7_log(none);
    }
    private void hands2_pack_send(string name, string path) {
        var d = new VrHandsPackDesc { bytes = sizeof(VrHandsPackDesc), format = 1, mirror = 1, scale = 1f };
        d.tint[0] = d.tint[1] = d.tint[2] = 1f;   // PackFix and the rest curls stay 0 (hand.json fields: S3)
        int n = Math.Min(name.Length, 63); byte* nb = stackalloc byte[n + 1];
        for (int i = 0; i < n; i++) { char ch = name[i]; nb[i] = ch >= 32 && ch < 127 ? (byte)ch : (byte)'_'; }
        nb[n] = 0; d.name = nb;
        fixed (char* p = path) { d.right_glb = p; d.left_glb = null; _fn_hands2_pack(&d, sizeof(VrHandsPackDesc)); }
    }

    /// <summary>The native status line ("drawing | generic-glove 2144 v | eye1 1198 eye2 1198 / 1200 frames (last 10 s) | last
    /// hidden: ..."), and v[0..16].</summary>
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
        if (_vr_hands && (v[11] == -1 || _h2_pack_sent == "")) {
            ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.8f, 0.3f, 1f));
            ImGui.TextWrapped("The hand model could not be loaded (see ffxvr_diag.log): boxes are drawn instead. Reinstalling the mod puts hands\\generic-glove and hands\\generic back.");
            ImGui.PopStyleColor(1);
        }
        ImGui.Text("look:"); ImGui.SameLine();
        if (ImGui.RadioButton("dark glove##h2pk", _vr_hands_pack == "generic-glove")) _vr_hands_pack = "generic-glove";
        ImGui.SameLine(); if (ImGui.RadioButton("bare hand##h2pk", _vr_hands_pack == "generic")) _vr_hands_pack = "generic";
        ImGui.SameLine(); ImGui.TextDisabled("(our own generic hand; your own models come in a later build)");
        ImGui.Text("show in: first person (with your body hidden), and"); ImGui.SameLine();
        bool dio = (_vr_hands_views & 2) != 0, tp = (_vr_hands_views & 4) != 0, seat = (_vr_hands_views & 8) != 0;
        if (ImGui.Checkbox("the diorama##h2v2", ref dio)) _vr_hands_views = dio ? _vr_hands_views | 2 : _vr_hands_views & ~2;
        ImGui.SameLine(); if (ImGui.Checkbox("third person##h2v4", ref tp)) _vr_hands_views = tp ? _vr_hands_views | 4 : _vr_hands_views & ~4;
        ImGui.SameLine(); if (ImGui.Checkbox("the seat view##h2v8", ref seat)) _vr_hands_views = seat ? _vr_hands_views | 8 : _vr_hands_views & ~8;
        ImGui.TextDisabled("In the diorama and the seat view the hands are your own size in the scaled world: life-size to you, giant beside the miniature, and they go behind its walls and ground like a real hand would.");
        ImGui.TextDisabled("Never on screens, menus, FMVs, battle transitions or under the dream filter. A controller put down for 10 s (nothing touched) hides its hand.");
        ImGui.SetNextItemWidth(160); ImGui.SliderFloat("idle finger curl##h2rc", ref _vr_hands_rest_curl, 0f, 1f, "%.2f"); ImGui.SameLine();
        ImGui.SetNextItemWidth(160); ImGui.SliderFloat("size##h2sc", ref _vr_hands_scale, 0.8f, 1.25f, "%.2f x"); ImGui.SameLine();
        if (ImGui.SmallButton("defaults##h2rs")) { _vr_hands_rest_curl = 0.20f; _vr_hands_scale = 1.00f; }
        if (_vr_hands && v[16] == 0 && v[5] == 1) ImGui.TextDisabled("The controllers' touch sensors are not bound (another runtime or profile?): the fingers follow the trigger and grip only.");
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
