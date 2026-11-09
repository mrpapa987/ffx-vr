// SPDX-License-Identifier: MIT
// ffxvr_panel.h -- vr-menu S2 (research vr-menu-design.md section 3): the F8 window's own panel, fixed in the room. Pure logic:
// no Windows, no D3D, no OpenXR, no logging, no allocation. The native side (ffxvr_native.cpp, the vpanel_* glue) sends the F8
// window's ImGui draws into the panel's own capture (scissor_inside picks them), places the panel and runs the grab once a frame
// and at every stickrep extra frame; ffxvr_xr.cpp submits the quad. A unit test (vrmenu_chk/s2/panel_test.cpp in the session scratchpad) drives all of it.
//
// Conventions: the OpenXR reference space (LOCAL): metres, x right, y up, z back; a pose's forward is its -Z. A quad's visible
// face is its +Z, so a panel faces the eye when its +Z points at it. Yaw: forward = (-sin yaw, 0, -cos yaw), as the native
// file's head yaw (atan2(-f.x, -f.z)); a positive yaw turns left.
//
// The owner's decisions (8 Nov): grab anywhere on the panel; F8 stays the only way to open it; Practice targets slides it aside
// for the round, then back (review S2: far enough to clear the targets, slide_plan; hidden when that is too far); it stays
// visible during movies.
//
// The grab (S3: with the laser pointer's beam showing where the ray goes): a grip latches to the panel at its press edge when the
// controller's aim ray hits the window's rectangle itself (k_ray_margin 0; S2 widened it by 3 cm without a beam) or the controller
// touches it (the grip within k_reach of the rectangle). A latched grip
// belongs to the panel until it is let go (the native side hides it from the world grab, quick squeezes, LB / RB, the hands tap
// and the chords). One hand moves the panel rigidly with the grip; two hands also scale it by the change of their distance.
#pragma once
#define FFXVR_PANEL_REV 4
#include <cmath>
#include <cstdint>

namespace vpanel {

struct V3 { float x, y, z; };
struct Q { float x, y, z, w; };
struct Pose { Q q; V3 p; };

constexpr float k_pi = 3.14159265f;
constexpr float k_em_deg = 0.9f;                         // one em at scale 1, seen from 1 m (design 3.2: ~22 Quest pixels an em)
constexpr float k_scale_min = 0.6f, k_scale_max = 2.0f;  // vr_menu_scale (and the two-hand resize)
constexpr float k_dist_min = 0.4f, k_dist_max = 2.5f;    // vr_menu_distance; also the grab's clamp from the eye
constexpr float k_height_min = -0.6f, k_height_max = 0.3f;   // vr_menu_height: the centre against eye height
constexpr int   k_side = 1536;                            // the panel swapchain's side (pixels)
constexpr float k_keep_deg = 50.f, k_keep_m = 3.f;        // reopen at the last pose when it is this near the gaze
constexpr float k_ray_margin = 0.f;                       // the grab's aim ray: the rectangle itself (S3; S2's proxy had 3 cm)
constexpr float k_reach = 0.15f;                          // the grip within 15 cm of the rectangle also grabs
constexpr float k_resize_min_d = 0.05f;                   // two hands closer than this do not scale
constexpr float k_slide_rate = 120.f;                     // a targets round: the slide's degrees a second
constexpr float k_slide_margin = 4.f, k_slide_max = 60.f; // ... clear of the targets by this much; further than this: hidden instead
constexpr float k_target_glow = 1.22f;                    // a Bushido plate's glow ring, of its radius (ffxvr_odtarget.h k_glow_out)
constexpr int   k_route_slack = 4;                        // a draw's scissor may stick out of the panel's rectangle by this many pixels

inline V3 add(V3 a, V3 b) { return V3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
inline V3 sub(V3 a, V3 b) { return V3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
inline V3 mul(V3 a, float s) { return V3{ a.x * s, a.y * s, a.z * s }; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float len(V3 a) { return sqrtf(dot(a, a)); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
inline bool finite3(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
inline bool finiteq(Q a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z) && std::isfinite(a.w); }
inline Q qmul(Q a, Q b) {
    return Q{ a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
              a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
              a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
              a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
inline Q qconj(Q a) { return Q{ -a.x, -a.y, -a.z, a.w }; }
inline V3 qrot(Q q, V3 v) {   // q v q*
    const V3 u{ q.x, q.y, q.z };
    const V3 t = mul(V3{ u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x }, 2.f);
    return add(add(v, mul(t, q.w)), V3{ u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x });
}
inline Q q_yaw(float a) { return Q{ 0.f, sinf(a * 0.5f), 0.f, cosf(a * 0.5f) }; }   // about +Y (a > 0: left, seen from above)
inline float wrap_pi(float a) { while (a > k_pi) a -= 2.f * k_pi; while (a < -k_pi) a += 2.f * k_pi; return a; }
/// The head's yaw (its forward's heading); 0 when the forward is unusable.
inline float head_yaw(Q hq) {
    const V3 f = qrot(hq, V3{ 0.f, 0.f, -1.f });
    return (std::isfinite(f.x) && std::isfinite(f.z) && (f.x * f.x + f.z * f.z) > 1e-8f) ? atan2f(-f.x, -f.z) : 0.f;
}
inline V3 yaw_fwd(float yaw) { return V3{ -sinf(yaw), 0.f, -cosf(yaw) }; }

/// Metres per capture pixel: one em (em_px capture pixels) spans k_em_deg at 1 m, times the scale. The panel is a physical object:
/// pulled closer it looks bigger. At the owner's 35 px and scale 1: 0.449 mm a pixel, a 30-em window 0.471 m wide.
inline float m_per_px(float em_px, float scale) {
    if (!(em_px >= 4.f)) em_px = 4.f;
    if (!std::isfinite(scale)) scale = 1.f;
    return 2.f * tanf(k_em_deg * 0.5f * k_pi / 180.f) * clampf(scale, k_scale_min, k_scale_max) / em_px;
}

/// The panel's orientation at pos: its +Z points at the eye, its X stays horizontal (no roll), upright (yaw, then pitch).
inline Q face_eye(V3 pos, V3 eye) {
    const V3 d = sub(eye, pos);
    const float h = sqrtf(d.x * d.x + d.z * d.z);
    if (!(len(d) > 1e-4f) || !finite3(d)) return Q{ 0.f, 0.f, 0.f, 1.f };
    const float yaw = atan2f(d.x, d.z);                  // about +Y: +Z -> (sin yaw, 0, cos yaw)
    const float pitch = -atan2f(d.y, h);                 // about +X: +Z -> (0, -sin a, cos a)
    return qmul(q_yaw(yaw), Q{ sinf(pitch * 0.5f), 0.f, 0.f, cosf(pitch * 0.5f) });
}

/// Opening in front: dist metres ahead along the head's heading (level), height metres against the eye, facing the eye.
inline Pose place_front(V3 eye, Q hq, float dist, float height) {
    dist = clampf(std::isfinite(dist) ? dist : 1.f, k_dist_min, k_dist_max);
    height = clampf(std::isfinite(height) ? height : -0.15f, k_height_min, k_height_max);
    const V3 p = add(add(eye, mul(yaw_fwd(head_yaw(hq)), dist)), V3{ 0.f, height, 0.f });
    return Pose{ face_eye(p, eye), p };
}

/// Reopening: the last pose of this session is kept when it is within k_keep_deg of the gaze's heading and k_keep_m of the eye.
inline bool keep_pose(const Pose& last, V3 eye, Q hq) {
    const V3 v = sub(last.p, eye);
    const float hl = sqrtf(v.x * v.x + v.z * v.z);
    if (!finite3(v) || !(hl > 0.05f) || len(v) > k_keep_m) return false;
    const float rel = wrap_pi(atan2f(-v.x, -v.z) - head_yaw(hq));
    return fabsf(rel) <= k_keep_deg * k_pi / 180.f;
}

/// After a grab: the distance (level) and height (against the eye) to open at next time, clamped to the settings' ranges.
inline void remember(const Pose& pn, V3 eye, float* dist, float* height) {
    const V3 v = sub(pn.p, eye);
    *dist = clampf(sqrtf(v.x * v.x + v.z * v.z), k_dist_min, k_dist_max);
    *height = clampf(v.y, k_height_min, k_height_max);
}

// -- the crop -------------------------------------------------------------------------------------------------------------
struct Rect { int x = 0, y = 0, w = 0, h = 0; };
inline bool empty(const Rect& r) { return r.w <= 0 || r.h <= 0; }
/// The F8 window and the popups it opened, ImGui coordinates [x0, x1) x [y0, y1), to capture pixels: scaled by capture /
/// display size, rounded outward, clamped to the capture, then cut to max_side from the top-left (the title bar and the header
/// stay). Empty when anything is unusable.
inline Rect crop(float x0, float y0, float x1, float y1, float ds_w, float ds_h, int cap_w, int cap_h, int max_side = k_side) {
    if (!(std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1)) || cap_w <= 0 || cap_h <= 0
        || !(ds_w > 0.f) || !(ds_h > 0.f) || !std::isfinite(ds_w) || !std::isfinite(ds_h) || !(x1 > x0) || !(y1 > y0) || max_side <= 0) return Rect{};
    const float sx = static_cast<float>(cap_w) / ds_w, sy = static_cast<float>(cap_h) / ds_h;
    const float fx0 = clampf(x0 * sx, 0.f, static_cast<float>(cap_w)), fx1 = clampf(x1 * sx, 0.f, static_cast<float>(cap_w));
    const float fy0 = clampf(y0 * sy, 0.f, static_cast<float>(cap_h)), fy1 = clampf(y1 * sy, 0.f, static_cast<float>(cap_h));
    const int ix0 = static_cast<int>(floorf(fx0)), iy0 = static_cast<int>(floorf(fy0));
    int ix1 = static_cast<int>(ceilf(fx1)), iy1 = static_cast<int>(ceilf(fy1));
    if (ix1 - ix0 > max_side) ix1 = ix0 + max_side;
    if (iy1 - iy0 > max_side) iy1 = iy0 + max_side;
    if (ix1 <= ix0 || iy1 <= iy0) return Rect{};
    Rect r; r.x = ix0; r.y = iy0; r.w = ix1 - ix0; r.h = iy1 - iy0;
    return r;
}
/// Which ImGui draws are the panel's (review S1): ImGui's D3D11 backend sets one scissor rectangle per draw command, the clip rect
/// of the window (or popup) that draws it. A draw belongs to the F8 window when its scissor (l, t, r, b, render target pixels)
/// lies inside the panel's rectangle widened by slack; the background and foreground lists and every other window clip to the
/// whole display or to their own rectangles. (The F8 window and its popups draw no decorations with the display-wide clip:
/// managed draws their backgrounds inside them.) An empty scissor is nobody's.
inline bool scissor_inside(int l, int t, int r, int b, const Rect& rr, int slack) {
    if (empty(rr) || r <= l || b <= t) return false;
    return l >= rr.x - slack && t >= rr.y - slack && r <= rr.x + rr.w + slack && b <= rr.y + rr.h + slack;
}
/// The quad's centre against the panel's pose, in metres along the panel's right (dx) and up (dy). The pose is the F8 window's
/// centre; the crop c also holds the popups the window opened (a combo list), so the quad is shifted by the crop's centre against
/// the window's and the window part stays where it is while a list widens the crop (design 3.6). win: the window alone (crop() of
/// its own rectangle); mpp: metres per capture pixel. 0, 0 when either rectangle is empty.
inline void crop_offset(const Rect& win, const Rect& c, float mpp, float* dx, float* dy) {
    *dx = 0.f; *dy = 0.f;
    if (empty(win) || empty(c) || !(mpp > 0.f) || !std::isfinite(mpp)) return;
    // in half pixels, exact in integers (image y runs down, the panel's up runs up)
    *dx = static_cast<float>((2 * c.x + c.w) - (2 * win.x + win.w)) * 0.5f * mpp;
    *dy = -static_cast<float>((2 * c.y + c.h) - (2 * win.y + win.h)) * 0.5f * mpp;
}
/// pn's centre moved dx along its right and dy along its up (the quad's centre from crop_offset).
inline V3 offset_centre(const Pose& pn, float dx, float dy) {
    return add(pn.p, add(mul(qrot(pn.q, V3{ 1.f, 0.f, 0.f }), dx), mul(qrot(pn.q, V3{ 0.f, 1.f, 0.f }), dy)));
}
// -- the grab ---------------------------------------------------------------------------------------------------------------
/// The ray o + t d (t > 0) against the panel's rectangle (w x h metres in its XY plane), widened by margin on every side. u, v:
/// where it meets the plane (0..1 across the image, v down), set whenever it meets it.
inline bool ray_hit(const Pose& pn, float w, float h, V3 o, V3 d, float margin, float* u = nullptr, float* v = nullptr) {
    const Q inv = qconj(pn.q);
    const V3 lo = qrot(inv, sub(o, pn.p)), ld = qrot(inv, d);
    if (!(fabsf(ld.z) > 1e-6f) || !finite3(lo) || !finite3(ld)) return false;
    const float t = -lo.z / ld.z;
    if (!(t > 0.f)) return false;
    const float x = lo.x + t * ld.x, y = lo.y + t * ld.y;
    if (u) *u = x / w + 0.5f;
    if (v) *v = 0.5f - y / h;
    return fabsf(x) <= w * 0.5f + margin && fabsf(y) <= h * 0.5f + margin;
}
/// The distance from a point to the panel's rectangle (0 on it).
inline float rect_dist(const Pose& pn, float w, float h, V3 p) {
    const V3 l = qrot(qconj(pn.q), sub(p, pn.p));
    const float dx = fabsf(l.x) - w * 0.5f, dy = fabsf(l.y) - h * 0.5f;
    const float ex = dx > 0.f ? dx : 0.f, ey = dy > 0.f ? dy : 0.f;
    return sqrtf(ex * ex + ey * ey + l.z * l.z);
}

struct HandIn { bool down = false; bool grip_ok = false; Pose grip{}; bool aim_ok = false; Pose aim{}; };
/// The grab's "anywhere on the panel": its ray on the window (S3, the beam shows it), or the controller touching it.
inline bool reaches(const Pose& pn, float w, float h, const HandIn& in) {
    if (in.aim_ok && ray_hit(pn, w, h, in.aim.p, qrot(in.aim.q, V3{ 0.f, 0.f, -1.f }), k_ray_margin)) return true;
    return in.grip_ok && rect_dist(pn, w, h, in.grip.p) <= k_reach;
}

struct Grab {
    bool prev[2] = { false, false };   // the raw grip last frame (press edges)
    bool held[2] = { false, false };   // latched to the panel until let go
    int  anchor = 0;                   // the hands the move is anchored to (bits 1 left, 2 right)
    V3   t_local{ 0.f, 0.f, 0.f };     // one hand: the panel's centre in that grip's frame
    float d0 = 0.f, scale0 = 1.f;      // two hands: their distance and the scale when the pair formed
    V3   c_off{ 0.f, 0.f, 0.f };       // ... and the centre against their midpoint
};
enum { G_LATCH_L = 1, G_LATCH_R = 2, G_MOVED = 4, G_END = 8 };
inline int mask(const Grab& g) { return (g.held[0] ? 1 : 0) | (g.held[1] ? 2 : 0); }
inline void reset(Grab& g) { g = Grab{}; }
/// The distance from the eye clamped to k_dist_min..k_dist_max along the eye's line.
inline V3 clamp_dist(V3 p, V3 eye) {
    const V3 v = sub(p, eye); const float L = len(v);
    if (!(L > 1e-4f) || !finite3(v)) return p;
    const float c = clampf(L, k_dist_min, k_dist_max);
    return c == L ? p : add(eye, mul(v, c / L));
}
/// Once a frame. in: both hands (raw grips, unmasked); on: the panel is shown (no latch, no move otherwise; a latch stays until its
/// grip is let go); allow: a new latch may form (not inside an injecting Bushido window, nor during Practice targets); pn / scale:
/// the panel (moved, rescaled); w, h: its size now (metres); eye: the eye centre. Returns G_* bits.
inline int grab_step(Grab& g, const HandIn in[2], bool on, bool allow, Pose& pn, float& scale, float w, float h, V3 eye) {
    int ev = 0;
    bool had = g.held[0] || g.held[1];
    for (int k = 0; k < 2; ++k) {
        if (!in[k].down) g.held[k] = false;
        else if (!g.prev[k] && on && allow && reaches(pn, w, h, in[k])) { g.held[k] = true; ev |= k ? G_LATCH_R : G_LATCH_L; }
        g.prev[k] = in[k].down;
    }
    if (had && !g.held[0] && !g.held[1]) ev |= G_END;
    int act = 0;
    for (int k = 0; k < 2; ++k) if (g.held[k] && in[k].grip_ok && on && finite3(in[k].grip.p) && finiteq(in[k].grip.q)) act |= 1 << k;
    if (act != g.anchor) {   // the hands changed: anchor again (no move this frame)
        g.anchor = act;
        if (act == 1 || act == 2) {
            const HandIn& a = in[act == 1 ? 0 : 1];
            g.t_local = qrot(qconj(a.grip.q), sub(pn.p, a.grip.p));
        }
        else if (act == 3) {
            g.d0 = len(sub(in[1].grip.p, in[0].grip.p)); g.scale0 = scale;
            g.c_off = sub(pn.p, mul(add(in[0].grip.p, in[1].grip.p), 0.5f));
        }
        return ev;
    }
    if (act == 0) return ev;
    V3 p;
    if (act != 3) {
        const HandIn& a = in[act == 1 ? 0 : 1];
        p = add(a.grip.p, qrot(a.grip.q, g.t_local));
    }
    else {
        const float d1 = len(sub(in[1].grip.p, in[0].grip.p));
        if (!(g.d0 > k_resize_min_d) && d1 > k_resize_min_d) { g.d0 = d1; g.scale0 = scale; }   // a pair that formed too close: it starts here
        else if (g.d0 > k_resize_min_d && d1 > k_resize_min_d) scale = clampf(g.scale0 * d1 / g.d0, k_scale_min, k_scale_max);
        p = add(mul(add(in[0].grip.p, in[1].grip.p), 0.5f), g.c_off);
    }
    p = clamp_dist(p, eye);
    if (!finite3(p)) return ev;
    pn.p = p; pn.q = face_eye(p, eye);
    return ev | G_MOVED;
}

// -- a Bushido targets round (Practice targets or a real one): aside for the round ------------------------------------------
/// The targets' half-extent in azimuth (degrees) seen from the eyes (ffxvr_odtarget.h: two spots spread apart, d ahead of the
/// eyes and h below them; plates of radius rad facing the eyes, with their glow). A plate's glow ring is a cone of angular radius
/// rho around its centre's direction (az_c, el_c); at that elevation the cone reaches az_c + asin(sin rho / cos el_c) in azimuth.
inline float targets_half_deg(float d, float h, float spread, float rad) {
    if (!(d > 0.f) || !std::isfinite(d) || !std::isfinite(h) || !std::isfinite(spread) || !std::isfinite(rad)) return 0.f;
    const float x = 0.5f * fabsf(spread), r = k_target_glow * fabsf(rad);
    const float dist = sqrtf(x * x + h * h + d * d), hz = sqrtf(x * x + d * d);
    const float sr = clampf(r / dist, 0.f, 1.f), ce = hz / dist;   // sin rho; cos of the centre's elevation
    const float spread_az = ce > 1e-4f ? asinf(clampf(sr / ce, 0.f, 1.f)) : k_pi * 0.5f;
    return (atan2f(x, d) + spread_az) * 180.f / k_pi;
}
struct SlidePlan { float deg = 0.f; bool hide = false; };
/// Where the panel goes for a round. a0: its centre's azimuth against the targets' forward (degrees, + left); hw: its angular
/// half-width; t: targets_half_deg. Already clear (its inner edge t + k_slide_margin out or more): it stays. Otherwise it turns
/// to its own side (left of the forward: left) until its inner edge is k_slide_margin past the targets; if its centre would then
/// be more than k_slide_max from the forward, it is hidden for the round instead.
inline SlidePlan slide_plan(float a0, float hw, float t) {
    SlidePlan p;
    if (!std::isfinite(a0) || !std::isfinite(hw) || !std::isfinite(t)) { p.hide = true; return p; }
    const float need = t + k_slide_margin + fabsf(hw);
    if (fabsf(a0) >= need) return p;
    if (need > k_slide_max) { p.hide = true; return p; }
    p.deg = (a0 >= 0.f ? need : -need) - a0;
    return p;
}
struct Slide { float cur = 0.f, target = 0.f; bool active = false, hide = false; V3 pivot{ 0.f, 0.f, 0.f }; };
/// Once a frame (dt seconds, capped at 0.1). on: a round is running; plan: slide_plan, taken at the round's start only (the side
/// and the angle stay for the round); the pivot is the eye at the start of a slide. The angle eases at k_slide_rate both ways.
inline void slide_step(Slide& s, bool on, const SlidePlan& plan, V3 eye, float dt) {
    if (!(dt >= 0.f)) dt = 0.f;
    if (dt > 0.1f) dt = 0.1f;
    if (on && !s.active) {
        s.active = true; s.hide = plan.hide; s.target = plan.hide ? 0.f : plan.deg;
        if (s.cur == 0.f) s.pivot = eye;
    }
    if (!on) { s.active = false; s.hide = false; s.target = 0.f; }
    const float step = k_slide_rate * dt;
    if (s.cur < s.target) s.cur = s.cur + step > s.target ? s.target : s.cur + step;
    else if (s.cur > s.target) s.cur = s.cur - step < s.target ? s.target : s.cur - step;
}
/// The pose shown: the panel turned s.cur degrees about the vertical through the pivot, facing the eye.
inline Pose slid(const Pose& pn, const Slide& s, V3 eye) {
    if (s.cur == 0.f) return pn;
    const V3 p = add(s.pivot, qrot(q_yaw(s.cur * k_pi / 180.f), sub(pn.p, s.pivot)));
    return Pose{ face_eye(p, eye), p };
}

} // namespace vpanel
