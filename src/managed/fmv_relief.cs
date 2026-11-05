// SPDX-License-Identifier: MIT

namespace FFXVR;

// FMV relief screen (S3 of the FMV depth plan, 4 Nov 2026; design-notes "FMV relief screen (S3, 4 Nov 2026)"): FMVs with a
// converted depth file (tools/fmvdepth, <mod>\fmvdepth\<name>.fvd) play on the virtual screen with real depth in the
// headset. All of it is native (ffxvr_native.cpp relief_present and the FMV worker, ffxvr_relief.h); this side owns the
// settings and F8's text:
//   fmv_depth            0 / 1 (default 0 until tested): FMVs in 3D when a valid depth file exists; anything else, and 0,
//                        is exactly the flat virtual screen of before;
//   fmv_depth_strength   0..0.25 (default 0.08): the relief's depth as a share of the screen's width (0.08 on a 2.5 m screen:
//                        10 cm into the screen at most, 5 cm out of it); followed smoothly, so the slider never pops;
//   fmv_depth_flat_subs  0 / 1 (default 0): the bottom 15 % of the video stays flat (the subtitles are in the same image and
//                        would otherwise lie on the relief);
//   fmv_depth_offset     -3..3 (default 0; Debug): the depth frame shown is the video frame + this (a sync test knob).
// ABI 41: ffxvr_fmv3d_set (every frame from push_all) and ffxvr_fmv3d_text (the text, only while it is drawn).
public sealed unsafe partial class FFXVRModule {

    private bool  _fmv_depth = false;              // ini fmv_depth
    private float _fmv_depth_strength = 0.08f;     // ini fmv_depth_strength
    private bool  _fmv_depth_flat_subs = false;    // ini fmv_depth_flat_subs
    private int   _fmv_depth_offset = 0;           // ini fmv_depth_offset (Debug)
    private delegate* unmanaged[Cdecl]<int, float, int, int, void> _fn_fmv3d_set;    // ABI 41
    private delegate* unmanaged[Cdecl]<int, byte*, int, int> _fn_fmv3d_text;         // ABI 41: (which, buf, cap) -> length
    private readonly byte[][] _fmv3d_raw = { new byte[600], new byte[600] };         // the bytes last turned into _fmv3d_str (per which)
    private readonly int[] _fmv3d_len = { -1, -1 };
    private readonly string[] _fmv3d_str = { "", "" };

    /// <summary>From push_all, every frame (applies at the next Present).</summary>
    private void fmv3d_push() {
        if (_fn_fmv3d_set != null) _fn_fmv3d_set(_fmv_depth ? 1 : 0, _fmv_depth_strength, _fmv_depth_flat_subs ? 1 : 0, _fmv_depth_offset);
    }

    /// <summary>Native's text (0 the Debug line, 1 the main window's note); a new string only when its bytes changed.</summary>
    private string fmv3d_text(int which) {
        var f = _fn_fmv3d_text; if (f == null) return "";
        byte* b = stackalloc byte[600];
        int n = f(which, b, 600);
        if (n < 0) n = 0; if (n > 599) n = 599;
        var now = new ReadOnlySpan<byte>(b, n);
        if (n == _fmv3d_len[which] && now.SequenceEqual(new ReadOnlySpan<byte>(_fmv3d_raw[which], 0, n))) return _fmv3d_str[which];
        now.CopyTo(_fmv3d_raw[which]); _fmv3d_len[which] = n;
        return _fmv3d_str[which] = System.Text.Encoding.ASCII.GetString(now);
    }

    /// <summary>The main window's row (in the View block, after "2D screens").</summary>
    private void fmv3d_ui() {
        ImGui.Checkbox("FMVs in 3D (needs converted depth)##fmv3d", ref _fmv_depth);
        if (!_fmv_depth) return;
        ImGui.SameLine(); ImGui.SetNextItemWidth(160); ImGui.SliderFloat("depth strength##fmv3d", ref _fmv_depth_strength, 0f, 0.25f, "%.3f");
        ImGui.SameLine(); ImGui.Checkbox("flatten the bottom 15% (subtitles)##fmv3d", ref _fmv_depth_flat_subs);
        string note = fmv3d_text(1);
        if (note.Length > 0) ImGui.TextDisabled("  " + esc(note));
    }

    /// <summary>The Debug header's line (after the FMV sync line) and the sync test knob.</summary>
    private void fmv3d_debug_ui() {
        if (_fn_fmv3d_text == null) return;
        ImGui.TextDisabled(esc(fmv3d_text(0)));
        if (_fmv_depth) {
            ImGui.SameLine(); ImGui.SetNextItemWidth(100);
            ImGui.SliderInt("depth frame offset (sync test)##fmv3d", ref _fmv_depth_offset, -3, 3);
        }
    }
}
