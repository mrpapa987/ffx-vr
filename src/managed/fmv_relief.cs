// SPDX-License-Identifier: MIT

namespace FFXVR;

// FMV relief screen (S3 of the FMV depth plan, 4 Nov 2026; design-notes "FMV relief screen (S3, 4 Nov 2026)"): FMVs with a
// converted depth file (tools/fmvdepth, <mod>\fmvdepth\<name>.fvd) play on the virtual screen with real depth in the
// headset. All of it is native (ffxvr_native.cpp relief_present and the FMV worker, ffxvr_relief.h); this side owns the
// settings and F8's text:
//   fmv_depth            0 / 1 (default 0 until tested): FMVs in 3D when a valid depth file exists; anything else, and 0,
//                        is exactly the flat virtual screen of before;
//   the "3D movie" mapping (owner test, 4 Nov 2026; design-notes "FMV relief screen"): each shot's farthest content goes
//   fmv_depth_range behind the screen, its nearest fmv_depth_pop in front of it, linear in disparity between, every pixel
//   along its ray from the nominal eye (the picture from there is the flat screen's):
//   fmv_depth_amount     0..1 (default 1): a multiplier on that (0 = flat); followed smoothly. Replaces fmv_depth_strength
//                        (0..0.25, the old linear relief): an ini with only the old key gets amount = strength / 0.08, at most 1;
//   fmv_depth_range      0.5..10 m (default 3): how far behind the screen the farthest content goes;
//   fmv_depth_pop        0..0.5 m (default 0.15): how far in front of it the nearest content comes;
//   fmv_depth_smooth     0..0.95 (default 0.6): the per-pixel temporal filter within a shot. Since round 7.4 the worker's
//                        centred one (frames -3 .. +3, edge-aware, never across a cut, no lag; its time sigma is 3 x this);
//                        with "old causal filter" (Debug, session-only) the EMA as before (changes over 24 of 255 pass at once);
//   fmv_depth_median     0 / 1 (default 1; Debug): a 3x3 median on each decoded depth frame (speckle);
//   fmv_depth_edge_fill  0 / 1 (default 0; Debug; was fmv_depth_edges, default 1, ignored since round 6): the edge fill: a triangle stretched across a depth edge shows the colour of
//                        the far side instead of a smear;
//   fmv_depth_edge_band  0.01..0.15 (default 0.04; Debug; never 0, review3 N-5): the band at the border over which content in front of the screen is
//                        brought back to the screen plane; content behind it is never feathered (the window rule, owner
//                        test round 3: the old feather on everything made a "shelf" at the top edge);
//   each frame to its own range  Debug, session-only (round 7): the depth normalised per frame as the converter stored it
//                        (the default since round 7 is one fixed range per shot from the index: the subject stops sinking back
//                        when something nearer enters);
//   old cut ramp         Debug, session-only (round 7.3): every cut starts flat and ramps in over 150 ms (the default
//                        switches at once at a cut whose depth is ready; the 150 ms ramp stays for returns from flat);
//   depth as colour      Debug, session-only: the depth drawn as colour (red near, green middle, blue far) to judge it;
//   fmv_depth_flat_subs  0 / 1 (default 0): the bottom 15 % of the video stays flat (the subtitles are in the same image and
//                        would otherwise lie on the relief);
//   fmv_depth_tight_fov  0 / 1 (default 1; Debug): each eye's projection is narrowed to the screen (review SF-1), so the eye
//                        image's rows go to the video; 0 = the eye's whole field of view (an A/B against Virtual Desktop);
//   depth frame offset   -3..3 (Debug, session-only: not saved, 0 at every start; review N6): the depth frame shown is the
//                        video frame + this (a sync test knob).
// ABI 41: ffxvr_fmv3d_set (every frame from push_all) and ffxvr_fmv3d_text (the text, only while it is drawn); ABI 42:
// ffxvr_fmv3d_set(on, amount, flags, offset, range, pop, smooth), flags 1 flatten the bottom, 2 tight fov, 4 edge fill, 8 median,
// 16 depth as colour, 32 per-frame range, 64 the causal filter, 128 the old cut ramp; bits 8-9 the grid (0 the 320 grid,
// 1 the 640 grid on the plain depth, 2 the guided upsample; round 10b), bits 10-13 the edge softening in tenths of a texel.
// ABI 43: + edge (the edge band).
public sealed unsafe partial class FFXVRModule {

    private bool  _fmv_depth = false;              // ini fmv_depth
    private float _fmv_depth_amount = 1f;          // ini fmv_depth_amount (fmv_depth_strength / 0.08 from an older ini)
    private bool  _fmv_depth_amount_seen = false;  // the ini had fmv_depth_amount: an old fmv_depth_strength line is not migrated over it
    private float _fmv_depth_range = 10f;          // ini fmv_depth_range (m; 0.5-20, default 10 since round 10c, was 3)
    private float _fmv_depth_pop = 0.5f;           // ini fmv_depth_pop (m; 0-2, default 0.5 since round 10c, was 0.15)
    private float _fmv_depth_smooth = 0.6f;        // ini fmv_depth_smooth
    private bool  _fmv_depth_median = true;        // ini fmv_depth_median (Debug)
    private bool  _fmv_depth_edges = false;        // ini fmv_depth_edges (Debug; default 0 since round 6: the fill made blocks along silhouettes)
    private bool  _fmv_depth_colour = false;       // Debug, session-only
    private bool  _fmv_depth_causal = false;       // Debug, session-only (round 7.4): the old causal filter (EMA) instead of the centred one (A/B)
    private bool  _fmv_depth_old_cuts = false;     // Debug, session-only (round 7.3): every cut starts flat and ramps in over 150 ms, as before (A/B)
    private bool  _fmv_depth_per_frame = false;    // Debug, session-only (round 7): each frame normalised to its own range, as before (A/B)
    private int   _fmv_depth_grid = 1;             // ini fmv_depth_grid (round 10c): 0 round 7's 320 grid, 1 the 640 grid on the plain depth (default), 2 the guided upsample (Debug only: streaks) (flags bits 8-9)
    private float _fmv_depth_soften = 1f;          // ini fmv_depth_soften (round 10c): edge softening, depth texels 0..1.5, default 1.0 (flags bits 10-13, tenths)
    private float _fmv_depth_edge_band = 0.04f;    // ini fmv_depth_edge_band (Debug)
    private bool  _fmv_depth_flat_subs = false;    // ini fmv_depth_flat_subs
    private bool  _fmv_depth_tight_fov = true;     // ini fmv_depth_tight_fov (Debug)
    private int   _fmv_depth_offset = 0;           // Debug, session-only (not in the ini)
    private delegate* unmanaged[Cdecl]<int, float, int, int, float, float, float, float, void> _fn_fmv3d_set;   // ABI 43
    private delegate* unmanaged[Cdecl]<int, byte*, int, int> _fn_fmv3d_text;         // ABI 41: (which, buf, cap) -> length
    private readonly byte[][] _fmv3d_raw = { new byte[600], new byte[600] };         // the bytes last turned into _fmv3d_str (per which)
    private readonly int[] _fmv3d_len = { -1, -1 };
    private readonly string[] _fmv3d_str = { "", "" };

    /// <summary>From push_all, every frame (applies at the next Present).</summary>
    private void fmv3d_push() {
        int flags = (_fmv_depth_flat_subs ? 1 : 0) | (_fmv_depth_tight_fov ? 2 : 0) | (_fmv_depth_edges ? 4 : 0) | (_fmv_depth_median ? 8 : 0) | (_fmv_depth_colour ? 16 : 0) | (_fmv_depth_per_frame ? 32 : 0) | (_fmv_depth_causal ? 64 : 0) | (_fmv_depth_old_cuts ? 128 : 0)
                  | ((_fmv_depth_grid == 1 || _fmv_depth_grid == 2 ? _fmv_depth_grid : 0) << 8) | ((int)MathF.Round(Math.Clamp(_fmv_depth_soften, 0f, 1.5f) * 10f) << 10);
        if (_fn_fmv3d_set != null) _fn_fmv3d_set(_fmv_depth ? 1 : 0, _fmv_depth_amount, flags, _fmv_depth_offset, _fmv_depth_range, _fmv_depth_pop, _fmv_depth_smooth, _fmv_depth_edge_band);
    }

    /// <summary>The ini's old linear strength (0..0.25, default 0.08) as the new amount, unless fmv_depth_amount was read.</summary>
    private void fmv3d_migrate_strength(float old) {
        if (!_fmv_depth_amount_seen && !float.IsNaN(old)) _fmv_depth_amount = Math.Clamp(old / 0.08f, 0f, 1f);
    }

    /// <summary>The angular disparity (degrees) against the screen plane of a point at distance z, eyes 63 mm apart.</summary>
    /// <summary>The pop-out and range the native side applies at this screen distance: relief::make_map's comfort limits
    /// (review3 SF-2: crossed at most 1 deg, uncrossed at most 1.5 deg, eyes 63 mm apart; the nearest at least 0.5 m away).</summary>
    private void fmv3d_limits(out float pop, out float range, out bool pop_limited, out bool range_limited) {
        float ds = Math.Max(_xr_screen_dist, 0.6f);
        pop = Math.Clamp(_fmv_depth_pop, 0f, ds - 0.5f); range = Math.Clamp(_fmv_depth_range, 0f, 50f);
        float c = 1.0f / 57.29578f / 0.063f, u = 1.5f / 57.29578f / 0.063f;
        float zn_min = 1f / (1f / ds + c);
        pop_limited = ds - pop < zn_min; if (pop_limited) pop = ds - zn_min;
        range_limited = false;
        if (1f / ds - u > 0f) { float zf_max = 1f / (1f / ds - u); if (ds + range > zf_max) { range = zf_max - ds; range_limited = true; } }
    }

    private float fmv3d_disparity(float z) {
        float ds = Math.Max(_xr_screen_dist, 0.6f);
        return 0.063f * (1f / z - 1f / ds) * 57.29578f;
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
        ImGui.SameLine(); ImGui.SetNextItemWidth(120); ImGui.SliderFloat("amount##fmv3d", ref _fmv_depth_amount, 0f, 1f, "%.2f");
        ImGui.SameLine(); ImGui.Checkbox("flatten the bottom 15% (subtitles)##fmv3d", ref _fmv_depth_flat_subs);
        ImGui.SetNextItemWidth(160); ImGui.SliderFloat("depth range: the farthest, behind the screen##fmv3d", ref _fmv_depth_range, 0.5f, 20f, "%.1f m");
        ImGui.SameLine(); ImGui.SetNextItemWidth(160); ImGui.SliderFloat("pop-out: the nearest, in front##fmv3d", ref _fmv_depth_pop, 0f, 2f, "%.2f m");
        ImGui.TextDisabled("grid:"); ImGui.SameLine();
        ImGui.RadioButton("640 grid, smooth depth (recommended)##fmv3dgrid", ref _fmv_depth_grid, 1); ImGui.SameLine();
        ImGui.RadioButton("320 grid (round 7)##fmv3dgrid", ref _fmv_depth_grid, 0); ImGui.SameLine(); ImGui.SetNextItemWidth(120);
        ImGui.SliderFloat("edge softening (depth texels)##fmv3d", ref _fmv_depth_soften, 0f, 1.5f, "%.1f");
        ImGui.SetNextItemWidth(160); ImGui.SliderFloat("smoothing (less shimmer, within a shot)##fmv3d", ref _fmv_depth_smooth, 0f, 0.95f, "%.2f");
        fmv3d_limits(out float pop, out float range, out bool pl, out bool rl);
        float ds = Math.Max(_xr_screen_dist, 0.6f);
        ImGui.SameLine(); ImGui.TextDisabled(esc(FormattableString.Invariant($"disparity {fmv3d_disparity(ds - pop):+0.00;-0.00} deg in front{(pl ? $" (pop-out limited to {pop:0.00} m: 1 deg at this screen distance)" : "")}, {fmv3d_disparity(ds + range):+0.00;-0.00} deg behind{(rl ? $" (range limited to {range:0.0} m: 1.5 deg)" : "")}")));
        string note = fmv3d_text(1);
        if (note.Length > 0) ImGui.TextDisabled("  " + esc(note));
    }

    /// <summary>The Debug header's line (after the FMV sync line), the tight field of view and the sync test knob.</summary>
    private void fmv3d_debug_ui() {
        if (_fn_fmv3d_text == null) return;
        ImGui.TextDisabled(esc(fmv3d_text(0)));
        if (_fmv_depth) {
            ImGui.Checkbox("FMV 3D: depth as colour (red near, blue far)##fmv3d", ref _fmv_depth_colour); ImGui.SameLine();
            ImGui.Checkbox("each frame to its own range (old; A/B)##fmv3d", ref _fmv_depth_per_frame); ImGui.SameLine();
            ImGui.Checkbox("old cut ramp (flat at every cut; A/B)##fmv3d", ref _fmv_depth_old_cuts); ImGui.SameLine();
            ImGui.Checkbox("old causal filter (EMA, lags; A/B)##fmv3d", ref _fmv_depth_causal);
            ImGui.RadioButton("FMV 3D: 640 grid, guided (round 10; experimental: streaks at silhouettes)##fmv3dgrid", ref _fmv_depth_grid, 2);
            ImGui.SameLine(); ImGui.TextDisabled("(the grid choice in the 3D FMV section returns to the others)");
            ImGui.Checkbox("edge fill (experimental)##fmv3d", ref _fmv_depth_edges); ImGui.SameLine();
            ImGui.Checkbox("3x3 median##fmv3d", ref _fmv_depth_median); ImGui.SameLine(); ImGui.SetNextItemWidth(120);
            ImGui.SliderFloat("edge band (pop-out only)##fmv3d", ref _fmv_depth_edge_band, 0.01f, 0.15f, "%.3f");
            ImGui.Checkbox("FMV 3D: tight field of view (sharper; off = the eye's whole, A/B)##fmv3d", ref _fmv_depth_tight_fov);
            ImGui.SameLine(); ImGui.SetNextItemWidth(100);
            ImGui.SliderInt("depth frame offset (sync test, not saved)##fmv3d", ref _fmv_depth_offset, -3, 3);
        }
    }
}
