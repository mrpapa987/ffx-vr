// SPDX-License-Identifier: MIT

namespace FFXVR;

// cpu-lean (design-notes "Sphere grid frame pacing and CPU timings (4 Nov 2026)"): the render thread's managed time per frame,
// in Stopwatch sections, sent once a frame to native (ffxvr_cpu_report, ABI 38), which prints the window's average and maximum
// on the cpu: line with every perf: line. Stopwatch.GetTimestamp only (QPC); no allocation, nothing logged here. Always on.
// The order of the values is native's cpu_line::M_*.
public sealed unsafe partial class FFXVRModule {

    private delegate* unmanaged[Cdecl]<float*, int, void> _fn_cpu_report;   // ABI 38

    private const int k_cpu_n = 15;
    // the frame's stamps (render thread)
    private long _cpu_t_top, _cpu_t_bf0, _cpu_t_bf1, _cpu_t_win0, _cpu_t_win1, _cpu_t_push0, _cpu_t_push1, _cpu_t_end;
    // accumulated inside push_all (a section can run more than once a frame: body_hide_push has three call sites)
    private long _cpu_player, _cpu_body, _cpu_ride, _cpu_hands, _cpu_tap;
    private int _cpu_gc0 = -1, _cpu_gc1, _cpu_gc2;
    private bool _cpu_done;   // render_imgui_inner reached its end this frame (a fault mid-way sends nothing)

    private static long cpu_now() => System.Diagnostics.Stopwatch.GetTimestamp();
    private static readonly double k_cpu_ms = 1000.0 / System.Diagnostics.Stopwatch.Frequency;
    private static float cpu_ms(long t0, long t1) => t0 != 0 && t1 >= t0 ? (float)((t1 - t0) * k_cpu_ms) : 0f;

    /// <summary>Top of render_imgui: a new frame's stamps.</summary>
    private void cpu_frame_start() {
        _cpu_t_top = cpu_now();
        _cpu_t_bf0 = _cpu_t_bf1 = _cpu_t_win0 = _cpu_t_win1 = _cpu_t_push0 = _cpu_t_push1 = _cpu_t_end = 0;
        _cpu_player = _cpu_body = _cpu_ride = _cpu_hands = _cpu_tap = 0;
        _cpu_done = false;
    }

    /// <summary>End of render_imgui (after PopFont): one report, when begin_frame ran and the frame got to its end.</summary>
    private void cpu_frame_report() {
        if (!_cpu_done || _fn_cpu_report == null || _cpu_t_bf1 == 0) return;
        long end = cpu_now();
        int g0 = GC.CollectionCount(0), g1 = GC.CollectionCount(1), g2 = GC.CollectionCount(2);
        if (_cpu_gc0 < 0) { _cpu_gc0 = g0; _cpu_gc1 = g1; _cpu_gc2 = g2; }
        float* v = stackalloc float[k_cpu_n];
        v[0] = cpu_ms(_cpu_t_top, _cpu_t_bf0);                                       // before begin_frame
        v[1] = cpu_ms(_cpu_t_bf0, _cpu_t_bf1);                                       // the begin_frame call (native's own sections are on the line too)
        v[2] = cpu_ms(_cpu_t_bf1, _cpu_t_win0 != 0 ? _cpu_t_win0 : _cpu_t_push0 != 0 ? _cpu_t_push0 : _cpu_t_end);   // hotkeys, hover, pick poll, gestures
        v[3] = cpu_ms(_cpu_t_win0, _cpu_t_win1);                                     // draw_window (0 with F8 closed)
        v[4] = cpu_ms(_cpu_t_push0, _cpu_t_push1);                                   // push_all
        v[5] = (float)(_cpu_player * k_cpu_ms);                                      //   push_player (with body_hide_push and ride_probe)
        v[6] = (float)(_cpu_body * k_cpu_ms);                                        //     body_hide_push
        v[7] = (float)(_cpu_ride * k_cpu_ms);                                        //     ride_probe
        v[8] = (float)(_cpu_hands * k_cpu_ms);                                       //   hands2_push (with hands2_tap_game)
        v[9] = (float)(_cpu_tap * k_cpu_ms);                                         //     hands2_tap_game
        v[10] = cpu_ms(_cpu_t_push1 != 0 ? _cpu_t_push1 : _cpu_t_win1 != 0 ? _cpu_t_win1 : _cpu_t_bf1, end);   // after push_all: the settings reload / save, PopFont
        v[11] = cpu_ms(_cpu_t_top, end);                                             // all of render_imgui
        v[12] = g0 - _cpu_gc0; v[13] = g1 - _cpu_gc1; v[14] = g2 - _cpu_gc2;
        _cpu_gc0 = g0; _cpu_gc1 = g1; _cpu_gc2 = g2;
        _fn_cpu_report(v, k_cpu_n);
    }
}
