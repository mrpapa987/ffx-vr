// SPDX-License-Identifier: MIT
// ffxvr_perf.h -- perf-gpu (22 Nov 2026): pure helpers for the GPU phase timers on the perf: line, the 5 s eye-2 test
// (Developer: "Measure: eye 2 off for 5 s") and the busy-GPU gate's counts on the sr: line (sr_skip_busy). No Windows, no
// D3D, no logging: ffxvr_native.cpp feeds them (the block "perf-gpu"), perf_chk drives them.
#pragma once
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace pgpu {

// -- 1. The phase timers -------------------------------------------------------------------------------------------------
// One D3D11 timestamp per marker, stamped at most once a frame (in this order on the GPU queue when the frame is the usual
// one), inside a per-frame disjoint query; a ring of k_ring frames, each read when its slot is about to be reused (k_ring - 1
// frames after its frame was closed), with D3D11_ASYNC_GETDATA_DONOTFLUSH: never a stall; a frame whose results are not
// ready then is skipped (counted). Eye 2 is re-issued draw by draw right after each eye-1 draw, so every phase holds both
// eyes' work: eye 2's share is the eye-2 test's measurement, not a phase.
enum Mark : int {
    M_START = 0,   // the game's first clear of the frame (the GPU span's own start)
    M_SCENE,       // the first bind of the scene target with a depth target (multisampled, or last frame's scene target)
    M_POST,        // the first resolve of the scene target (the MSAA resolve that starts the post chain)
    M_UI,          // the UI pass opens (ui_pass_begin, before its two eye-image copies)
    M_PRESENT,     // our Present hook, before xr_tick (the GPU span's end)
    M_OURS,        // right before the fresh frame's xrEndFrame: after our eye copies, UI panel, ImGui layer, plates (XrHost::before_end)
    M_N
};
enum Phase : int {
    P_PRE = 0,     // M_START -> M_SCENE: shadows and whatever the game draws before the scene
    P_SCENE,       // M_SCENE -> M_POST: the scene (opaque + transparent, both eyes) and its first MSAA resolve
    P_POST,        // M_POST -> M_UI (or M_PRESENT without a UI pass): HDAO / SMAA / unsharp / glow / colour as the game orders them
    P_UI,          // M_UI -> M_PRESENT: the UI pass, uidepth, the final copy to the backbuffer
    P_OURS,        // M_PRESENT -> M_OURS: the mod's work after Present
    P_FRAME,       // M_START -> M_OURS (M_PRESENT when the frame has no xrEndFrame of ours: flat, no open frame)
    P_GAP,         // the previous frame's end -> this frame's M_START: the runtime's own work in its calls, Fahrenheit's ImGui, idle
    P_N
};
constexpr int k_ring = 4;
constexpr double k_far_ms = 1000.0;   // a phase or gap longer than this is not a frame's (a load, a pause): left out

struct Acc {
    double sum = 0.0, mx = 0.0; uint32_t n = 0;
    void add(double v) { sum += v; if (n == 0 || v > mx) mx = v; n++; }
    double avg() const { return n ? sum / n : 0.0; }
    void merge(const Acc& o) { if (o.n == 0) return; sum += o.sum; if (n == 0 || o.mx > mx) mx = o.mx; n += o.n; }
};

/// One frame's stamps: the raw timestamps and which were stamped (bit per Mark).
struct Stamps { uint64_t t[M_N] = {}; unsigned have = 0; };

inline double ticks_ms(uint64_t a, uint64_t b, double freq) { return static_cast<double>(b - a) * 1000.0 / freq; }

/// The phases of one frame (ms) into out[]; returns the mask of the valid ones (bit per Phase; P_GAP is the caller's). Needs
/// M_START and M_PRESENT with PRESENT >= START (else 0: incomplete). M_SCENE, M_POST and M_UI are kept only in their order
/// between START and PRESENT (each at or after the last one kept); M_OURS only at or after PRESENT. A stamped marker that
/// breaks the order is dropped and *unordered set. Without M_POST (no MSAA resolve, e.g. msaa off) scene and post cannot be
/// told apart: neither is reported (the frame still counts).
inline unsigned phases(const Stamps& s, double freq, double out[P_N], bool* unordered) {
    if (unordered) *unordered = false;
    for (int i = 0; i < P_N; ++i) out[i] = 0.0;
    if (!(freq > 0.0)) return 0;
    const unsigned need = (1u << M_START) | (1u << M_PRESENT);
    if ((s.have & need) != need || s.t[M_PRESENT] < s.t[M_START]) return 0;
    unsigned keep = need; uint64_t last = s.t[M_START];
    for (int m = M_SCENE; m <= M_UI; ++m) {
        if (!(s.have & (1u << m))) continue;
        if (s.t[m] >= last && s.t[m] <= s.t[M_PRESENT]) { keep |= 1u << m; last = s.t[m]; }
        else if (unordered) *unordered = true;
    }
    if (s.have & (1u << M_OURS)) {
        if (s.t[M_OURS] >= s.t[M_PRESENT]) keep |= 1u << M_OURS; else if (unordered) *unordered = true;
    }
    auto k = [&](int m) { return (keep & (1u << m)) != 0; };
    unsigned v = 0;
    auto put = [&](int p, int a, int b) { const double ms = ticks_ms(s.t[a], s.t[b], freq); if (ms <= k_far_ms) { out[p] = ms; v |= 1u << p; } };
    if (k(M_SCENE)) put(P_PRE, M_START, M_SCENE);
    if (k(M_SCENE) && k(M_POST)) put(P_SCENE, M_SCENE, M_POST);
    if (k(M_POST)) put(P_POST, M_POST, k(M_UI) ? M_UI : M_PRESENT);
    if (k(M_UI)) put(P_UI, M_UI, M_PRESENT);
    if (k(M_OURS)) put(P_OURS, M_PRESENT, M_OURS);
    put(P_FRAME, M_START, k(M_OURS) ? M_OURS : M_PRESENT);
    return v;
}
/// The frame's end for the next frame's gap: M_OURS when kept, else M_PRESENT (call only for a frame phases() accepted).
inline uint64_t frame_end(const Stamps& s) {
    return ((s.have & (1u << M_OURS)) && s.t[M_OURS] >= s.t[M_PRESENT]) ? s.t[M_OURS] : s.t[M_PRESENT];
}
/// The gap from the previous frame's end to this frame's start (ms), or a negative value when it cannot be told.
inline double gap_ms(bool prev_ok, uint64_t prev_end, const Stamps& s, double freq) {
    if (!prev_ok || !(freq > 0.0) || !(s.have & (1u << M_START)) || s.t[M_START] < prev_end) return -1.0;
    const double ms = ticks_ms(prev_end, s.t[M_START], freq);
    return ms <= k_far_ms ? ms : -1.0;
}

/// The perf: line's window (since the last perf: line).
struct Window {
    Acc ph[P_N];
    uint32_t frames = 0;       // frames whose phases were read
    uint32_t not_ready = 0;    // a slot not ready when it was due (skipped, never waited for)
    uint32_t disjoint = 0;     // the disjoint query said the clock was unreliable (skipped)
    uint32_t incomplete = 0;   // no first clear or no Present stamp (a frame without a clear: loads, some menus)
    uint32_t unordered = 0;    // a marker out of order was left out
    void reset() { *this = Window{}; }
    void add(const double v[P_N], unsigned mask) {
        frames++;
        for (int p = 0; p < P_N; ++p) if (mask & (1u << p)) ph[p].add(v[p]);
    }
};

inline size_t addf(char* b, size_t cap, size_t len, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;
inline size_t addf(char* b, size_t cap, size_t len, const char* fmt, ...) {
    if (b == nullptr || cap == 0 || len >= cap) return len;
    va_list ap; va_start(ap, fmt);
    const int r = vsnprintf(b + len, cap - len, fmt, ap);
    va_end(ap);
    if (r < 0) { b[len] = 0; return len; }
    const size_t w = static_cast<size_t>(r);
    return len + w < cap ? len + w : cap - 1;
}
inline size_t acc_text(char* b, size_t cap, size_t len, const Acc& a, uint32_t frames) {
    if (a.n == 0) return addf(b, cap, len, "-");
    if (a.n == frames) return addf(b, cap, len, "%.2f/%.2f", a.avg(), a.mx);
    return addf(b, cap, len, "%.2f/%.2f (%u frames)", a.avg(), a.mx, a.n);
}
/// The perf: line's part (it starts with " | "). secs: the window's length.
inline size_t window_text(char* b, size_t cap, const Window& w, double secs) {
    if (b == nullptr || cap == 0) return 0;
    b[0] = 0;
    size_t n = addf(b, cap, 0, " | gpu phases (timestamps, diag_verbose; %.0f s, %u frames read; both eyes in every phase: eye 2 is re-issued draw by draw, its own cost is the Developer eye-2 test's), avg/max ms:", secs, w.frames);
    static const char* const label[P_N] = {
        " shadows+pre (first clear to the scene target's first bind with depth) ",
        " | scene (to its first MSAA resolve, both eyes' resolves in) ",
        " | post (from that resolve to the UI pass, or to Present without one) ",
        " | ui (UI pass to Present: the UI, uidepth, the copy to the backbuffer) ",
        " | ours after Present (eye copies, UI panel, ImGui layer, plates, to our xrEndFrame) ",
        " | frame (first clear to our xrEndFrame, or to Present without one) ",
        " | between frames (our xrEndFrame to the next first clear: the runtime's calls, ImGui, idle) " };
    for (int p = 0; p < P_N; ++p) { n = addf(b, cap, n, "%s", label[p]); n = acc_text(b, cap, n, w.ph[p], w.frames); }
    n = addf(b, cap, n, " | not ready %u, disjoint %u, incomplete %u, out of order %u", w.not_ready, w.disjoint, w.incomplete, w.unordered);
    return n;
}

// -- 2. The eye-2 test ---------------------------------------------------------------------------------------------------
// The Developer button runs: 3 s before (eye 2 on), 5 s with eye 2's re-issue skipped (both eyes are sent eye 1's image),
// 3 s after (eye 2 on), then a few frames for the late GPU results; then one perf: line with the span (and, with the phase
// timers, every phase) with and without eye 2. Frames that may not skip (not the 3D headset view) are left out of all three
// windows. Each frame carries a tag: the test's generation (6 bits) and its window (2 bits); results of another run are
// ignored, so a late result never lands in the next test.
struct E2Test {
    enum St : int { IDLE = 0, PRE = 1, OFF = 2, POST = 3, DRAIN = 4 };
    static constexpr double k_pre_ms = 3000.0, k_off_ms = 5000.0, k_post_ms = 3000.0;
    static constexpr int k_drain_frames = 10;   // more than the span ring's (k_gpuq_ring 6) and the phase ring's (k_ring 4) delays
    int st = IDLE; double t0 = 0.0; int drain = 0; uint32_t gen = 0;
    bool done = false;                          // a result or a stop is in `text`, not logged yet (take_done)
    uint32_t off_skip = 0, off_kept = 0;        // frames of the off window that skipped eye 2 / kept it (not the 3D headset view)
    Acc span[4], cpu[4], ph[4][P_N];            // by window: 1 before, 2 eye 2 off, 3 after (0 unused)
    char text[1400] = "eye-2 test: not run yet";

    bool active() const { return st != IDLE; }
    uint8_t tag_of(int w) const { return w == 0 ? 0 : static_cast<uint8_t>(((gen & 63u) << 2) | static_cast<unsigned>(w)); }
    static bool tag_off(uint8_t tag) { return (tag & 3u) == static_cast<unsigned>(OFF); }
    bool mine(uint8_t tag) const { return active() && tag != 0 && (tag >> 2) == (gen & 63u); }
    void start(double now) {
        gen++; st = PRE; t0 = now; drain = 0; done = false; off_skip = off_kept = 0;
        for (int i = 0; i < 4; ++i) { span[i] = Acc{}; cpu[i] = Acc{}; for (int p = 0; p < P_N; ++p) ph[i][p] = Acc{}; }
        snprintf(text, sizeof(text), "eye-2 test: running (3 s with eye 2, 5 s without, 3 s with)");
    }
    void stop(const char* why) {
        if (!active()) return;
        st = IDLE; done = true;
        snprintf(text, sizeof(text), "eye-2 test: stopped (%s); eye 2 is back on, no result", why ? why : "?");
    }
    /// Once per Present, after the view of the frame about to render is decided: that frame's tag (0: not part of the test).
    /// view_ok: it may skip eye 2 (the 3D headset view); abort: non-null stops the test (the headset went off, ...).
    uint8_t step(double now, bool view_ok, const char* abort) {
        if (!active()) return 0;
        if (abort != nullptr) { stop(abort); return 0; }
        if (st == PRE && now - t0 >= k_pre_ms) { st = OFF; t0 = now; }
        if (st == OFF && now - t0 >= k_off_ms) { st = POST; t0 = now; }
        if (st == POST && now - t0 >= k_post_ms) { st = DRAIN; drain = 0; }
        if (st == DRAIN) { if (++drain >= k_drain_frames) finish(); return 0; }
        if (!view_ok) { if (st == OFF) off_kept++; return 0; }
        if (st == OFF) off_skip++;
        return tag_of(st);
    }
    void add_span(uint8_t tag, double ms) { if (mine(tag)) span[tag & 3u].add(ms); }
    void add_cpu(uint8_t tag, double ms) { if (mine(tag)) cpu[tag & 3u].add(ms); }
    void add_phases(uint8_t tag, const double v[P_N], unsigned mask) {
        if (!mine(tag)) return;
        for (int p = 0; p < P_N; ++p) if (mask & (1u << p)) ph[tag & 3u][p].add(v[p]);
    }
    bool take_done() { const bool d = done; done = false; return d; }
    void finish() {
        st = IDLE; done = true;
        Acc with = span[PRE]; with.merge(span[POST]);
        Acc cw = cpu[PRE]; cw.merge(cpu[POST]);
        if (with.n == 0 || span[OFF].n == 0) {
            addf(text, sizeof(text), 0, "eye-2 test: no result: GPU spans read with eye 2 %u, without %u (eye 2 was skipped on %u frames, kept on %u that were not the 3D headset view)",
                     with.n, span[OFF].n, off_skip, off_kept);
            return;
        }
        const double d = with.avg() - span[OFF].avg();
        size_t n = addf(text, sizeof(text), 0, "eye-2 test (eye 2 skipped on %u frames; both eyes showed eye 1): GPU span (first clear to Present) with eye 2 %.2f ms (before %.2f, %u frames; after %.2f, %u frames), "
                 "without %.2f ms (%u frames): eye 2 costs %.2f ms a frame, %.0f%% of the span; eye 2's render-thread CPU %.2f -> %.2f ms a frame",
                 off_skip, with.avg(), span[PRE].avg(), span[PRE].n, span[POST].avg(), span[POST].n, span[OFF].avg(), span[OFF].n, d, with.avg() > 0.0 ? 100.0 * d / with.avg() : 0.0,
                 cw.avg(), cpu[OFF].avg());
        bool any = false;
        for (int p = 0; p < P_N; ++p) if (ph[OFF][p].n > 0) any = true;
        if (any) {
            static const char* const nm[P_N] = { "shadows+pre", "scene", "post", "ui", "ours after Present", "frame", "between frames" };
            n = addf(text, sizeof(text), n, "; phases with -> without eye 2 (avg ms):");
            for (int p = 0; p < P_N; ++p) {
                Acc w = ph[PRE][p]; w.merge(ph[POST][p]);
                if (w.n == 0 || ph[OFF][p].n == 0) n = addf(text, sizeof(text), n, "%s %s -", p ? "," : "", nm[p]);
                else n = addf(text, sizeof(text), n, "%s %s %.2f -> %.2f", p ? "," : "", nm[p], w.avg(), ph[OFF][p].avg());
            }
        }
        n = addf(text, sizeof(text), n, " | skipped: eye 2's re-issued draws and its mirrored MSAA resolves; kept: the hand parts' split draws, the mirrored clears and copies");
        if (off_kept) addf(text, sizeof(text), n, "; %u frames of the off window kept eye 2 (not the 3D headset view)", off_kept);
    }
};

// -- 3. The busy-GPU gate (sr_skip_busy) -----------------------------------------------------------------------------------
// Before an extra frame (stick reprojection) the native side asks, without waiting, whether the GPU has finished the last fresh
// frame's work (an event query issued right before that frame's xrEndFrame; GetData with DONOTFLUSH). busy: -1 unknown (no
// query yet, the gate off, an error), 0 finished, 1 still busy. sr_skip_busy: 0 off (no query at all), 1 log only (every
// extra is sent; the counts and End times split by the answer), 2 on (a busy answer skips that slot: no xrEndFrame blocked
// behind the game's GPU queue).
enum GateAct : int { G_SEND = 0, G_SKIP = 1 };
inline int gate_decide(int mode, int busy) { return (mode == 2 && busy == 1) ? G_SKIP : G_SEND; }
struct GateStats {
    uint32_t checked = 0, would = 0, skipped = 0, unknown = 0;
    Acc end_busy, end_done;     // xrEndFrame of the extras sent, by the answer before them
    Acc dl_busy, dl_done;       // list start delay after an extra, by its answer
};
inline void gate_count(GateStats& s, int mode, int busy) {
    if (mode == 0) return;
    s.checked++;
    if (busy < 0) s.unknown++;
    else if (busy == 1) { s.would++; if (mode == 2) s.skipped++; }
}
inline void gate_end(GateStats& s, int busy, double end_ms) { if (busy == 1) s.end_busy.add(end_ms); else if (busy == 0) s.end_done.add(end_ms); }
inline void gate_delay(GateStats& s, int busy, double ms) { if (busy == 1) s.dl_busy.add(ms); else if (busy == 0) s.dl_done.add(ms); }
/// The sr: line's part (it starts with " | ").
inline size_t gate_text(char* b, size_t cap, const GateStats& s, int mode) {
    if (b == nullptr || cap == 0) return 0;
    b[0] = 0;
    if (mode == 0) return addf(b, cap, 0, " | busy-GPU gate off (sr_skip_busy=0)");
    size_t n = addf(b, cap, 0, " | busy-GPU gate (sr_skip_busy=%d, %s): extras checked %u: would skip %u, skipped %u, unknown %u | xrEndFrame of extras sent with the GPU busy avg %.2f max %.2f ms (%u), with it done avg %.2f max %.2f ms (%u)"
                    " | list start delay after a busy extra avg %.2f max %.2f ms (%u), after a done one avg %.2f max %.2f ms (%u)",
                    mode, mode == 2 ? "on: a busy slot is skipped" : "log only: every extra sent", s.checked, s.would, s.skipped, s.unknown,
                    s.end_busy.avg(), s.end_busy.mx, s.end_busy.n, s.end_done.avg(), s.end_done.mx, s.end_done.n,
                    s.dl_busy.avg(), s.dl_busy.mx, s.dl_busy.n, s.dl_done.avg(), s.dl_done.mx, s.dl_done.n);
    return n;
}

}  // namespace pgpu
