// SPDX-License-Identifier: MIT
// ffxvr_render.h -- fix-render (7 Nov 2026): pure helpers for the frame-type decision (flat screen or 3D) and its reason,
// the second 3D camera's comparison with the frame's main one, the GPU-budget readout and the runtime's xrEndFrame trend.
// No Windows, no D3D, no logging: ffxvr_native.cpp feeds them, a scratch unit test drives them.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace rnd {

// -- 1. The frame-type decision: flat screen or 3D (logs-1107 behaviour E1 / E2) ------------------------------------------
// A frame goes flat when it has no real 3D camera, fewer than 3 scene draws, or (backdrop_mode 1) it is a painted-backdrop
// room: the game's layered-2D background system (a dummy camera, design-notes "Painted-backdrop rooms"). Until 7 Nov a single
// layer upload made the room: a 3D cutscene in kino0400 went flat for 9.3 s on 2 layer uploads beside 349 real camera
// uploads. Now a room needs at least k_bd_min layer uploads, or the layers must dominate (or the real 3D uploads be absent): a
// room is entered when layers >= k_bd_min or layers x k_bd_enter >= real 3D uploads, and kept while layers >= k_bd_min or
// layers x k_bd_stay >= real (the hysteresis band), leaving after k_bd_exit_frames frames in a row outside it, or at once when
// no layer is drawn. The 2D verdict still needs k_flat_frames frames in a row before the screen; back to 3D at once, as before.
// (Review B1: a painted room with many 3D characters has real > 4 x layers; the floor keeps every room of 4+ layers flat, as
// before, so only a stray layer or two beside a real 3D scene, E1's 2 vs 349, changes.)
// Separately, first person, the hands, the stick remap and the stereo screen stay off whenever any layer is drawn, this frame
// or the one before (any_layers): only the screen decision uses the ratio.
constexpr uint32_t k_bd_min = 4, k_bd_enter = 4, k_bd_stay = 8, k_bd_exit_frames = 10, k_flat_frames = 2, k_few_draws = 3;

/// Why the frame is shown as it is. The first group is the frame's own verdict (ModeRule); the rest are the native side's
/// overrides (forced by a setting or a state), in the order it applies them.
enum Why : int {
    W_3D = 0,           // a real 3D camera and scene draws
    W_3D_MINOR,         // 3D: painted layers drawn too, but they do not dominate the real 3D uploads
    W_NO_CAM,           // no real 3D camera uploaded
    W_FEW_DRAWS,        // a 3D camera but fewer than k_few_draws scene draws
    W_ROOM,             // a painted-backdrop room (backdrop_mode 1)
    W_FORCED,           // screen_mode 1 (always the virtual screen)
    W_NEVER,            // screen_mode 2 (never)
    W_BT,               // a battle transition keeps the view it started in
    W_MENU,             // Fahrenheit's save screen
    W_SLOW,             // the slow-GPU guard tests the screen
    W_COUNT
};
inline bool why_flat(int w) { return w == W_NO_CAM || w == W_FEW_DRAWS || w == W_ROOM; }

/// One frame's counts: real 3D camera uploads (|P0|, |P5| > 0.2, light cameras excluded), painted-layer (dummy camera)
/// uploads, scene draws, and the backdrop_mode setting (1: a room goes to the screen; 0: 3D props over the flat painting).
struct FrameIn { uint32_t real3d = 0, layers = 0, draws = 0; int backdrop_mode = 1; };

struct ModeRule {
    bool room = false;       // a painted-backdrop room (the layer rule, with its hysteresis; any backdrop_mode)
    uint32_t room_out = 0;   // frames in a row a room failed the stay test
    uint32_t without = 0;    // frames in a row with a 2D verdict
    bool flat = false;       // this frame's decision after the hysteresis: the virtual screen
    bool any_layers = false; // a layer drawn this frame or the one before (first person, the hands, the remap, the stereo screen off)
    uint32_t prev_layers = 0;
    int why = W_3D;          // this frame's own verdict (before the hysteresis)
    FrameIn last;            // this frame's counts (for the reason text)
    void reset() { *this = ModeRule{}; }
    static bool dominate(uint32_t layers, uint32_t real3d, uint32_t ratio) {
        return layers >= k_bd_min || real3d == 0 || static_cast<uint64_t>(layers) * ratio >= static_cast<uint64_t>(real3d);
    }
    void step(const FrameIn& f) {
        last = f;
        any_layers = f.layers > 0 || prev_layers > 0; prev_layers = f.layers;
        if (f.layers == 0) { room = false; room_out = 0; }
        else if (!room) { room = dominate(f.layers, f.real3d, k_bd_enter); room_out = 0; }
        else if (dominate(f.layers, f.real3d, k_bd_stay)) room_out = 0;
        else if (++room_out >= k_bd_exit_frames) { room = false; room_out = 0; }
        if (room && f.backdrop_mode == 1) why = W_ROOM;
        else if (f.real3d == 0) why = W_NO_CAM;
        else if (f.draws < k_few_draws) why = W_FEW_DRAWS;
        else why = f.layers > 0 ? W_3D_MINOR : W_3D;
        without = why_flat(why) ? without + 1 : 0;
        flat = without >= k_flat_frames;
    }
};

/// The reason as text for the mode lines (the counts and the rule that fired).
inline void why_text(char* out, size_t n, int why, const FrameIn& f) {
    if (out == nullptr || n == 0) return;
    switch (why) {
    case W_3D: snprintf(out, n, "a real 3D camera (%u real 3D uploads, %u scene draws)", f.real3d, f.draws); break;
    case W_3D_MINOR: snprintf(out, n, "3D: %u painted-layer uploads do not dominate %u real 3D uploads (a room needs %u+ layers or layers x %u >= real, x %u to stay; first person stays off while layers are drawn)",
                              f.layers, f.real3d, k_bd_min, k_bd_enter, k_bd_stay); break;
    case W_NO_CAM: snprintf(out, n, "no real 3D camera (%u painted-layer uploads, %u scene draws)", f.layers, f.draws); break;
    case W_FEW_DRAWS: snprintf(out, n, "a 3D camera but %u scene draws (< %u; %u real 3D uploads)", f.draws, k_few_draws, f.real3d); break;
    case W_ROOM: snprintf(out, n, "a painted-backdrop room: %u layer uploads vs %u real 3D uploads (%u+ layers, or layers x %u >= real to enter, x %u to stay; backdrop_mode 1)",
                          f.layers, f.real3d, k_bd_min, k_bd_enter, k_bd_stay); break;
    case W_FORCED: snprintf(out, n, "screen_mode 1 (always the virtual screen)"); break;
    case W_NEVER: snprintf(out, n, "screen_mode 2 (never the virtual screen)"); break;
    case W_BT: snprintf(out, n, "a battle transition keeps the view it started in"); break;
    case W_MENU: snprintf(out, n, "Fahrenheit's save screen is open"); break;
    case W_SLOW: snprintf(out, n, "the slow-GPU guard is testing the screen"); break;
    default: snprintf(out, n, "?"); break;
    }
}

/// The reason watch: while the mode stays the same, a new reason is logged once it has held for k_why_hold frames (a flat
/// room entered through a load is logged as "no real 3D camera" at the switch; the room's own reason follows a moment later).
constexpr uint32_t k_why_hold = 30;
struct WhyWatch {
    int logged = -1, cand = -1; uint32_t run = 0; bool mode = false;
    /// The mode switched this frame (its line carries the reason): that reason is the logged one.
    void switched(bool flat, int why) { mode = flat; logged = why; cand = -1; run = 0; }
    /// Every frame without a switch: true when `why` differs from the logged reason and has held k_why_hold frames.
    bool step(int why) {
        if (why == logged) { cand = -1; run = 0; return false; }
        if (why != cand) { cand = why; run = 1; } else run++;
        if (run >= k_why_hold) { logged = why; cand = -1; run = 0; return true; }
        return false;
    }
};

// -- 3. A second 3D camera in the frame (logs-1107 behaviour G) ---------------------------------------------------------
/// How far another camera's 3x4 view (row-major rows: right, up, back; translation in column 3) is from the frame's main one:
/// the eye positions' distance (game units) and the angle between their forward axes (degrees).
struct CamDiff { float dpos = 0.f, dang = 0.f; };
inline void cam_pos(const float v[12], float P[3]) {
    for (int i = 0; i < 3; ++i) P[i] = -(v[0 * 4 + i] * v[3] + v[1 * 4 + i] * v[7] + v[2 * 4 + i] * v[11]);
}
inline CamDiff cam_diff(const float a[12], const float b[12]) {
    CamDiff d; float pa[3], pb[3]; cam_pos(a, pa); cam_pos(b, pb);
    const float dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
    d.dpos = sqrtf(dx * dx + dy * dy + dz * dz);
    const float la = sqrtf(a[8] * a[8] + a[9] * a[9] + a[10] * a[10]), lb = sqrtf(b[8] * b[8] + b[9] * b[9] + b[10] * b[10]);
    float c = (la > 1e-6f && lb > 1e-6f) ? (a[8] * b[8] + a[9] * b[9] + a[10] * b[10]) / (la * lb) : 1.f;
    c = c > 1.f ? 1.f : c < -1.f ? -1.f : c;
    d.dang = acosf(c) * 57.29578f;
    return d;
}
/// Review S2 / round 2 (R2-1): which camera of the frame is the main one, decided upload by upload (the main-camera state is
/// written as the uploads come). The views of the frame's real 3D eye-1 uploads get slots in order of first upload (a fifth
/// view folds into the last slot). The main camera is tracked by its view, not by its slot number (a camera that is there in
/// some frames only, and uploads first, would shift the numbers):
/// - with a reference (the previous frame's main view), a slot whose view lies within the tracking gate of it is main: k_pick_pos
///   units and k_pick_deg degrees, widened by twice the main camera's recent per-frame motion (a moving camera; each frame's
///   motion counts capped at k_pick_mot_pos / k_pick_mot_deg, so a cut widens the gate only a little); a later slot nearer the reference takes
///   over (the nearest wins; the earlier one's uploads were drawn right, the state they wrote is overwritten);
/// - until one does (a cut moves every camera; the first frame has no reference), the previous frame's busiest slot (the most
///   uploads into the scene target, then the most uploads; ties: the earlier) is main from its second upload of the frame on,
///   or else the first slot to reach k_pick_busy uploads this frame (a one-upload stray never qualifies);
/// The next frame's reference is the gate-picked main slot's view, else the busiest slot's (so a stray picked by a fallback,
/// or first in the very first frame, never becomes the reference). The window counts frames with
/// a later camera than the first as main, and frames with no main upload.
constexpr int k_cams = 4;
constexpr uint32_t k_pick_busy = 8;
constexpr float k_pick_pos = 4.f, k_pick_deg = 4.f, k_pick_mot_pos = 25.f, k_pick_mot_deg = 10.f;
struct MainPick {
    float v[k_cams][12] = {}; uint32_t n[k_cams] = {}, scene[k_cams] = {}; int k = 0;
    int main = -1;                     // this frame's main slot (-1: none yet)
    bool by_gate = false; float main_d = 0.f;   // ... picked by the tracking gate, and its distance score
    int fallback = 0;                  // the previous frame's busiest slot
    float ref[12] = {}; bool ref_ok = false;   // the previous frame's main view
    float mot_pos = 0.f, mot_deg = 0.f;        // the main camera's recent per-frame motion (decaying maximum)
    bool open = false;                         // a frame is in progress
    uint32_t frames = 0, later = 0, none = 0;  // frames closed, of them with a later camera as main / with no main upload (the window: the caller resets)
    int busiest() const {
        int best = 0;
        for (int i = 1; i < k; ++i) if (scene[i] > scene[best] || (scene[i] == scene[best] && n[i] > n[best])) best = i;
        return best;
    }
    /// Within the gate: true, and *score = the distance (units, plus a unit a degree) for the nearest-wins rule.
    bool near_ref(const float vv[12], float* score) const {
        const CamDiff d = cam_diff(ref, vv);
        *score = d.dpos + d.dang;
        return d.dpos <= k_pick_pos + 2.f * mot_pos && d.dang <= k_pick_deg + 2.f * mot_deg;
    }
    void new_frame() {
        if (open) {   // close the previous frame
            frames++;
            if (main < 0) none++; else if (main != 0) later++;
            if (k > 0) {
                const int r = main >= 0 && by_gate ? main : busiest();   // a fallback pick is no reference: the busiest camera is
                if (ref_ok && main >= 0) {
                    const CamDiff d = cam_diff(ref, v[r]);
                    const float dp = d.dpos < k_pick_mot_pos ? d.dpos : k_pick_mot_pos, da = d.dang < k_pick_mot_deg ? d.dang : k_pick_mot_deg;
                    mot_pos = dp > 0.8f * mot_pos ? dp : 0.8f * mot_pos;
                    mot_deg = da > 0.8f * mot_deg ? da : 0.8f * mot_deg;
                }
                else { mot_pos = 0.f; mot_deg = 0.f; }
                for (int j = 0; j < 12; ++j) ref[j] = v[r][j];
                ref_ok = true; fallback = busiest();
            }
            else { ref_ok = false; fallback = 0; }
        }
        open = true;
        main = -1; by_gate = false; main_d = 0.f; k = 0; for (int i = 0; i < k_cams; ++i) { n[i] = 0; scene[i] = 0; }
    }
    /// One real 3D eye-1 upload of this frame: its slot (it may make that slot the main one; compare with `main` after the call).
    int note(const float vv[12], bool scene_rt) {
        int s = -1;
        for (int i = 0; i < k && s < 0; ++i) { bool eq = true; for (int j = 0; j < 12 && eq; ++j) eq = v[i][j] == vv[j]; if (eq) s = i; }
        if (s < 0) { if (k < k_cams) { s = k++; for (int j = 0; j < 12; ++j) v[s][j] = vv[j]; } else s = k_cams - 1; }
        n[s]++; if (scene_rt) scene[s]++;
        float d = 0.f;
        if (main < 0) {
            if (ref_ok && near_ref(v[s], &d)) { main = s; by_gate = true; main_d = d; }
            else if ((s == fallback && n[s] >= 2) || n[s] >= k_pick_busy) main = s;   // no gate match (yet): the busiest
        }
        else if (n[s] == 1 && s != main && ref_ok && near_ref(v[s], &d) && (!by_gate || d < main_d)) { main = s; by_gate = true; main_d = d; }   // nearer: takes over
        return s;
    }
};

/// The other camera's uploads over the view: line's 10 s: how many, how many while the scene target was bound, the largest
/// distance / angle from the main camera, and the first one's buffer size and projection.
struct OtherCam {
    uint32_t n = 0, into_scene = 0, ortho_near = 0; float dpos_max = 0.f, dang_max = 0.f; uint32_t size0 = 0; float p0 = 0.f, p0_main = 0.f;
    uint32_t main_n = 0;               // review S2: the main camera's own uploads over the same window
    void note(const CamDiff& d, bool scene_rt, bool ortho, uint32_t size, float p0_other, float p0_of_main) {
        if (n == 0) { size0 = size; p0 = p0_other; p0_main = p0_of_main; }
        n++; if (scene_rt) into_scene++; if (ortho) ortho_near++;
        if (d.dpos > dpos_max) dpos_max = d.dpos;
        if (d.dang > dang_max) dang_max = d.dang;
    }
    /// A plain reading: the same camera a frame or so apart (a few units, a few degrees), or a different one.
    const char* verdict() const {
        if (n == 0) return "none";
        if (ortho_near > 0) return "LIGHT-LIKE (orthographic near projection: a shadow light the light-camera rule missed?)";
        if (into_scene < n) return "partly into another render target (an off-screen pass: reflection, monitor?)";
        if (dpos_max <= 5.f && dang_max <= 3.f) return "into the scene target, within 5 units / 3 deg of the main camera (the same camera a moment apart)";
        return "into the scene target, a different viewpoint (a second scene camera)";
    }
};

// -- 4. The GPU budget (logs-1107 perf finding 2) -----------------------------------------------------------------------
// The 120 Hz extra frames (stickrep) collapse once the game's GPU span passes about 13.5 ms: fresh frames reach Present after
// their slot's deadline and the runtime repeats slots. The readout: the span against that knee, the extra frames per second
// and the slots the runtime repeated (extra frames dropped) per second.
constexpr double k_gpu_knee_ms = 13.5;
struct GpuBudget {
    double gpu_ms = 0.0, gpu_max_ms = 0.0;          // the perf window's span (game's first clear to our Present hook)
    double extras_s = 0.0, dropped_s = 0.0;         // extra frames a second, slots repeated by the runtime a second (last sr window)
    double late_pct = 0.0;                          // fresh frames that reached Present after their slot's deadline (last sr window)
    bool over() const { return gpu_ms > k_gpu_knee_ms; }
};
/// sr window counts -> rates: late share of the fresh frames (percent), extras and dropped (skipped) slots a second.
inline void gpu_budget_rates(GpuBudget& g, uint32_t fresh, uint32_t late, uint32_t extras, uint32_t skipped, double secs) {
    g.late_pct = fresh ? 100.0 * late / fresh : 0.0;
    g.extras_s = secs > 0.0 ? extras / secs : 0.0;
    g.dropped_s = secs > 0.0 ? skipped / secs : 0.0;
}

// -- 6. The runtime's xrEndFrame trend (logs-1107 perf finding 3) -------------------------------------------------------
// Virtual Desktop's xrEndFrame grew from 0.1-0.2 ms to 4-8 ms after 25-37 minutes in both sessions (its wait for the
// previous frame's submission into the streamer). The trend: a baseline from the session's first k_end_base_s, and an onset
// line the first time a 10 s window's extra-frame End average passes each threshold (1 ms, then 3 ms), with the session age.
constexpr double k_end_base_s = 300.0;
constexpr double k_end_onset_ms[2] = { 1.0, 3.0 };
struct EndTrend {
    double base_sum = 0.0; uint32_t base_n = 0;     // windows in the first k_end_base_s
    double max_ms = 0.0;                            // the largest window average this session
    int onset = 0;                                  // thresholds passed (0..2)
    void reset() { *this = EndTrend{}; }
    double base() const { return base_n ? base_sum / base_n : 0.0; }
    /// One window: age_s since the headset session started, end_ms the window's average End of the extra frames (< 0: none).
    /// Returns the threshold index crossed for the first time (0 or 1), or -1.
    int step(double age_s, double end_ms) {
        if (!(end_ms >= 0.0)) return -1;
        if (age_s < k_end_base_s) { base_sum += end_ms; base_n++; }
        if (end_ms > max_ms) max_ms = end_ms;
        if (onset < 2 && end_ms > k_end_onset_ms[onset] && age_s >= 0.0) return onset++;
        return -1;
    }
};

}  // namespace rnd
