// SPDX-License-Identifier: MIT
namespace FFXVR;
// Quest-only controls (5 Nov 2026; design-notes "Quest controllers only"; the pure rules in src/native/ffxvr_pad.h): the walk
// gate (move_remap_walk_only: the first-person stick remap only while the player can walk; battle and menus get the raw
// stick), quick-squeeze grips (grip_lbrb, grip_tap_ms: a quick squeeze of a grip is LB / RB in battle and in the seat /
// diorama, a held grip stays the grab and the chord modifier), vd_overlay_mask (both stick clicks, Virtual Desktop's overlay,
// send nothing to the game), and the settings of the ImGui feed (imgui_pad.cs). Native decides; this file reads the game
// bits, pushes the settings every frame and draws the F8 rows.
public sealed unsafe partial class FFXVRModule {
    private delegate* unmanaged[Cdecl]<int, int, void> _fn_move_gate_set;     // ABI 44
    private delegate* unmanaged[Cdecl]<int, int, int, int, void> _fn_grip_set;
    private delegate* unmanaged[Cdecl]<int, byte*, int, int> _fn_ctl_text;
    private bool _move_walk_only = true;     // ini move_remap_walk_only (1): the remap only while walking; 0 = in every first-person view (the old behaviour, for an A/B)
    private int  _grip_lbrb = 1;             // ini grip_lbrb: 0 the grips are LB / RB at once (old), 1 a quick squeeze in battle and the seat / diorama, 2 everywhere
    private int  _grip_tap_ms = 300;         // ini grip_tap_ms (150..500): a quick squeeze is released within this
    private int  _grip_tap_press_ms = 0;     // ini grip_tap_press_ms (0..150): the LB / RB press lasts 2 game polls and at least this (0: one CTB row a squeeze; over ~200 the game's repeat doubles it)
    private bool _vd_overlay_mask = true;    // ini vd_overlay_mask (1): both stick clicks send neither Back nor R3
    private int  _imgui_vrpad = 1;           // ini imgui_vrpad: 0 off, 1 the Quest controllers in Fahrenheit's save screen (default), 2 in all of ImGui
    private bool _imgui_vrpad_strict = false;   // ini imgui_vrpad_strict (0): 1 = refuse when the game's XInput import resolves into ImGui's DLL (at the install)
    private int  _qc_tap_game; private ulong _qc_tap_frame = ulong.MaxValue;   // hands2_tap_push's game bits and the frame they were read in (the choice box)

    /// <summary>The exports (load_native's req: a missing one is a mismatched pair).</summary>
    private void qc_bind(Func<string, nint> req) {
        _fn_move_gate_set = (delegate* unmanaged[Cdecl]<int, int, void>)req("ffxvr_move_gate_set");
        _fn_grip_set = (delegate* unmanaged[Cdecl]<int, int, int, int, void>)req("ffxvr_grip_set");
        _fn_ctl_text = (delegate* unmanaged[Cdecl]<int, byte*, int, int>)req("ffxvr_ctl_text");
        imgui_pad_bind(req);
    }
    private void qc_clear() {
        _fn_move_gate_set = null; _fn_grip_set = null; _fn_ctl_text = null;
        imgui_pad_clear();
    }
    private bool qc_parse(string k, string v) {
        switch (k) {
            case "move_remap_walk_only": pb(k, v, ref _move_walk_only); return true;
            case "grip_lbrb":            pe(k, v, ref _grip_lbrb, 0, 1, 2); return true;
            case "grip_tap_ms":          pi(k, v, ref _grip_tap_ms, 150, 500); return true;
            case "grip_tap_press_ms":    pi(k, v, ref _grip_tap_press_ms, 0, 150); return true;
            case "vd_overlay_mask":      pb(k, v, ref _vd_overlay_mask); return true;
            case "imgui_vrpad":          pe(k, v, ref _imgui_vrpad, 0, 1, 2); return true;
            case "imgui_vrpad_strict":   pb(k, v, ref _imgui_vrpad_strict); return true;
            default: return false;
        }
    }
    private string qc_ini_text() {
        var ci = CI;
        return
            $"move_remap_walk_only={(_move_walk_only ? 1 : 0)}\n" +
            $"grip_lbrb={_grip_lbrb.ToString(ci)}\n" +
            $"grip_tap_ms={_grip_tap_ms.ToString(ci)}\n" +
            $"grip_tap_press_ms={_grip_tap_press_ms.ToString(ci)}\n" +
            $"vd_overlay_mask={(_vd_overlay_mask ? 1 : 0)}\n" +
            $"imgui_vrpad={_imgui_vrpad.ToString(ci)}\n" +
            $"imgui_vrpad_strict={(_imgui_vrpad_strict ? 1 : 0)}\n";
    }

    /// <summary>The game bits for the walk gate and the grips (the tap gate's bits, ffxvr_gates.h GB_*). The battle bit every
    /// frame (the grips need it in every view); the rest only where the remap can apply (first / third person / diorama with
    /// the walk gate on): a field menu, no player control (one byte, two dwords, all guarded), the save screen and blitzball
    /// (already known), the sphere grid (one byte), riding, and the choice box only from a frame whose tap read made it.</summary>
    private int qc_game_bits() {
        if (_fn_safe_read == null) return 0x100;
        int g = hands2_battle_bit();
        if (_fp_mode == 0 || !_move_remap || !_move_walk_only) return g;
        if (!read(FhUtil.ptr_at<uint>(GameGates.k_menu), out uint menu)) g |= 0x100; else if (menu != 0) g |= 0x80;
        if (!gs_player(out uint player)) g |= 0x100; else if (player == 0) g |= 0x40;
        if (!read(FhUtil.ptr_at<byte>(0x1685F70), out byte sg)) g |= 0x100; else if (sg != 0) g |= 2;   // Fahrenheit Globals.SphereGrid.is_open
        if (_save_st != 0 || _fh_menu_open) g |= 4;
        if (_in_blitz) g |= 16;
        if (_ride_att) g |= 32;
        if (_qc_tap_frame == _frames && (_qc_tap_game & 0x700) == 0) g |= _qc_tap_game & 0x800;   // a choice box (the tap's full read this frame)
        return g;
    }
    /// <summary>Every frame from push_all, after hands2_push (its tap read gives the choice box) and push_player (riding).</summary>
    private void qc_push() {
        if (_fn_move_gate_set != null) _fn_move_gate_set(_move_walk_only ? 1 : 0, qc_game_bits());
        if (_fn_grip_set != null) _fn_grip_set(_grip_lbrb, _grip_tap_ms, _grip_tap_press_ms, _vd_overlay_mask ? 1 : 0);
        imgui_pad_push();
    }
    private string qc_text(int which) {
        if (_fn_ctl_text == null) return "";
        byte* b = stackalloc byte[512];
        int n = _fn_ctl_text(which, b, 512);
        return n > 0 ? Marshal.PtrToStringAnsi((nint)b) ?? "" : "";
    }
    /// <summary>F8, next to "left stick follows where I look".</summary>
    private void qc_walk_ui() {
        ImGui.Checkbox("left stick: raw in battle and menus (rotated only while walking)", ref _move_walk_only);
        ImGui.SameLine(); ImGui.TextDisabled(esc(qc_text(0)));
    }
    /// <summary>F8, the Controllers section: the grips, the VD overlay mask, the save screen.</summary>
    private void qc_ui() {
        bool q = _grip_lbrb != 0, all = _grip_lbrb == 2;
        if (ImGui.Checkbox("grips: quick squeeze = LB / RB in battle and the seat / diorama", ref q)) _grip_lbrb = q ? (all ? 2 : 1) : 0;
        if (q) {
            ImGui.SameLine(); if (ImGui.Checkbox("everywhere##griplbrb", ref all)) _grip_lbrb = all ? 2 : 1;
            ImGui.SameLine(); ImGui.SetNextItemWidth(140f); ImGui.SliderInt("quick squeeze (ms)", ref _grip_tap_ms, 150, 500);
        }
        ImGui.TextDisabled(esc(qc_text(1)));
        ImGui.Checkbox("both stick clicks (Virtual Desktop's overlay) send nothing to the game", ref _vd_overlay_mask);
        bool im = _imgui_vrpad != 0, im_all = _imgui_vrpad == 2;
        if (ImGui.Checkbox("Quest controllers in Fahrenheit's save screen", ref im)) _imgui_vrpad = im ? (im_all ? 2 : 1) : 0;
        if (im) {
            ImGui.SameLine();
            if (ImGui.Checkbox("in every ImGui window too (Start and Back as well: both grips + menu = Fahrenheit's soft reset, a game over in battle)", ref im_all)) _imgui_vrpad = im_all ? 2 : 1;
        }
        ImGui.TextDisabled(esc(imgui_pad_status()));
    }
}
