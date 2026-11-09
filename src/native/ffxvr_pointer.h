// SPDX-License-Identifier: MIT
// ffxvr_pointer.h -- vr-menu S3 (research vr-menu-design.md section 4): the laser pointer on the F8 window's panel. Pure logic, as
// ffxvr_panel.h: no Windows, no D3D, no OpenXR, no logging, no allocation. The native side (ffxvr_native.cpp, vpanel_pointer in
// vrpad_take: every game frame and every stickrep extra frame) runs it on the raw controller input before the copy becomes g_vrpad,
// masks what the panel took, and hands managed the ImGui mouse (ffxvr_panel_pointer). A unit test (vrmenu_chk/s3/pointer_test.cpp in
// the session scratchpad) drives all of it.
//
//   the ray       each controller's aim pose, its -Z, against the quad shown (the F8 window and its popups); u, v across the image
//   which hand    vr_menu_hand: auto (either; the owner keeps it while its ray stays on, or k_leave_s after, or while its trigger
//                 is held; a hand that comes onto the panel, or pulls its trigger on it, takes it over), right only, left only
//   the button    the owner's trigger, with the XInput hysteresis (k_trig_on / k_trig_off, ffxvr_pad.h's 30 / 20 of 255)
//   the mask      a hand whose ray is on the panel loses its trigger and its stick to the panel; a trigger pressed there stays
//                 the panel's until it is released, a stick until it is back in its dead zone (mask_step)
//   the wheel     the owner's stick y over the panel: k_wheel_rate notches a second at full deflection, past k_stick_dz
//   steadiness    a 1-euro filter on the hit (panel metres) and a press lock (k_lock_s, or until the ray turned k_lock_deg)
//   the beam      a thin quad from the aim pose to the plane, facing the eye (beam_pose), while the ray is near the panel
//   the edge      a ray comes on at the quad's edge and goes off k_edge_m beyond it (on_step: no flicker on the border)
//   a gap         a drag survives a frame or two without the panel (drag_grace: a pairing miss, a lost head pose)
//   handing back  ImGui's mouse goes back where the OS cursor was before pointing (or to a point off the F8 window), with
//                 WantSetMousePos kept k_leave_frames more frames, then "no mouse" (Feed: review S3 B1)
#pragma once
#define FFXVR_POINTER_REV 2
#include "ffxvr_panel.h"

namespace vptr {
using vpanel::V3; using vpanel::Q; using vpanel::Pose; using vpanel::Rect;

constexpr float k_trig_on = 30.f / 255.f, k_trig_off = 20.f / 255.f;   // XINPUT_GAMEPAD_TRIGGER_THRESHOLD and its release (ffxvr_pad.h)
constexpr float k_stick_dz = 0.2f;                       // the stick's dead zone (as the turning's)
constexpr float k_wheel_rate = 4.f;                      // wheel notches a second at full deflection (design 4.4: ~20 lines a second)
constexpr float k_leave_s = 0.15f;                       // the owner keeps the pointer this long after its ray left (design 4.2)
constexpr float k_near = 0.25f;                          // the beam shows while the ray meets the quad widened by 25 % (design 3.5)
constexpr float k_lock_s = 0.10f, k_lock_deg = 1.5f;     // the press lock (design 4.1)
constexpr float k_beam_w = 0.004f;                       // the beam's width (m)
constexpr float k_oe_min = 1.0f, k_oe_beta = 20.f, k_oe_d = 1.0f;   // 1-euro: Hz at rest, Hz per m/s, the speed's own cutoff
constexpr float k_edge_m = 0.015f;                       // a ray on the quad goes off this far beyond its edge (review N1)
constexpr float k_gap_s = 0.10f;                         // a drag survives this long without the panel (review N2)
constexpr int   k_leave_frames = 3;                      // frames ImGui's mouse is held at the park point after the pointer left
constexpr float k_park_pad = 8.f;                        // the park point stays this far (ImGui px) off the F8 window

// -- the 1-euro filter (Casiez et al. 2012) -------------------------------------------------------------------------------
struct OneEuro { bool init = false; float x = 0.f, dx = 0.f; };
inline float oe_alpha(float cutoff, float dt) {
    const float tau = 1.f / (2.f * vpanel::k_pi * cutoff);
    return 1.f / (1.f + tau / dt);
}
/// One sample x after dt seconds; the filtered value. dt <= 0 or a first sample: taken as it is.
inline float oe_step(OneEuro& f, float x, float dt) {
    if (!std::isfinite(x)) return f.init ? f.x : 0.f;
    if (!f.init || !(dt > 0.f)) { if (!f.init) { f.init = true; f.x = x; f.dx = 0.f; } return f.x; }
    const float dx = (x - f.x) / dt;
    f.dx += oe_alpha(k_oe_d, dt) * (dx - f.dx);
    const float cutoff = k_oe_min + k_oe_beta * fabsf(f.dx);
    f.x += oe_alpha(cutoff, dt) * (x - f.x);
    return f.x;
}

// -- the ray on the quad --------------------------------------------------------------------------------------------------
struct QuadHit {
    bool plane = false;   // the ray meets the quad's plane ahead of it
    bool on = false;      // ... inside the quad (0 <= u, v <= 1)
    bool near_ = false;   // ... inside the quad widened by k_near of its size on every side
    float u = 0.f, v = 0.f, t = 0.f;   // across the image (v down); the distance along the ray (m)
    float lx = 0.f, ly = 0.f;          // the point in the quad's plane, metres from its centre (x right, y up)
    V3 p{ 0.f, 0.f, 0.f };             // the point itself
};
/// The ray o + t d (d need not be unit; t is scaled to metres) against the quad (pose, w x h metres, facing +Z).
inline QuadHit hit_quad(const Pose& q, float w, float h, V3 o, V3 d) {
    QuadHit r;
    const float dl = vpanel::len(d);
    if (!(w > 0.f) || !(h > 0.f) || !(dl > 1e-6f) || !vpanel::finite3(o) || !vpanel::finite3(d) || !vpanel::finiteq(q.q)) return r;
    const V3 dn = vpanel::mul(d, 1.f / dl);
    const Q inv = vpanel::qconj(q.q);
    const V3 lo = vpanel::qrot(inv, vpanel::sub(o, q.p)), ld = vpanel::qrot(inv, dn);
    if (!(fabsf(ld.z) > 1e-6f)) return r;
    const float t = -lo.z / ld.z;
    if (!(t > 0.f)) return r;
    r.plane = true; r.t = t;
    r.lx = lo.x + t * ld.x; r.ly = lo.y + t * ld.y;
    r.u = r.lx / w + 0.5f; r.v = 0.5f - r.ly / h;
    r.p = vpanel::add(o, vpanel::mul(dn, t));
    r.on = r.u >= 0.f && r.u <= 1.f && r.v >= 0.f && r.v <= 1.f;
    r.near_ = r.u >= -k_near && r.u <= 1.f + k_near && r.v >= -k_near && r.v <= 1.f + k_near;
    return r;
}
/// u, v across the quad (the crop: cap, in backbuffer pixels, its origin and size) to ImGui coordinates (DisplaySize ds against the
/// backbuffer bb). False when a size is unusable.
inline bool uv_to_imgui(float u, float v, const Rect& cap, float ds_w, float ds_h, float bb_w, float bb_h, float* x, float* y) {
    if (vpanel::empty(cap) || !(ds_w > 0.f) || !(ds_h > 0.f) || !(bb_w > 0.f) || !(bb_h > 0.f) || !std::isfinite(u) || !std::isfinite(v)) return false;
    *x = (static_cast<float>(cap.x) + u * static_cast<float>(cap.w)) * ds_w / bb_w;
    *y = (static_cast<float>(cap.y) + v * static_cast<float>(cap.h)) * ds_h / bb_h;
    return true;
}

/// A hand's ray on the quad with hysteresis (review N1): it comes on inside the quad, and goes off only beyond k_edge_m past an edge
/// (or off the plane).
inline bool on_step(bool was, const QuadHit& h, float w, float hh) {
    if (!h.plane) return false;
    if (!was) return h.on;
    return fabsf(h.lx) <= 0.5f * w + k_edge_m && fabsf(h.ly) <= 0.5f * hh + k_edge_m;
}
/// A drag through a gap (review N2): with the panel gone for a moment (can false) while the owner's button, pressed on the panel, is
/// still down, the pointer keeps feeding its last position for k_gap_s. gap: seconds without it (reset whenever it is there).
inline bool drag_grace(float& gap, bool can, bool btn, bool down, float dt) {
    if (can) { gap = 0.f; return false; }
    if (!btn || !down) { gap = 0.f; return false; }
    gap += (dt > 0.f && std::isfinite(dt)) ? dt : 0.f;
    return gap <= k_gap_s;
}

// -- handing ImGui's mouse back (review S3 B1) ------------------------------------------------------------------------------------
/// While the pointer feeds ImGui, managed writes io.MousePos and sets io.WantSetMousePos, so the Win32 backend moves the OS cursor
/// there (onto the F8 window). When the pointer leaves, a single "no mouse" is not enough: a WM_MOUSEMOVE from the last cursor move
/// (the message-pump thread) and the backend's GetCursorPos poll both put ImGui's mouse back over F8, WantCaptureMouse stays on and
/// Fahrenheit withholds the game's input. So for k_leave_frames frames the mouse is written at a park point with the flag still set
/// (ImGui drops every queued position meanwhile, and the backend moves the OS cursor there), then the flag goes and one "no mouse"
/// is queued. The park point is where ImGui's mouse was before pointing, unless that is unknown or on the F8 window; then the
/// display's left edge, half way down (the F8 window is at the right).
struct Feed { int phase = 0; int left = 0; float park_x = 0.f, park_y = 0.f; };   // phase 0 idle, 1 feeding, 2 handing back
struct FeedIn {
    bool active = false; float px = 0.f, py = 0.f;          // the pointer feeds ImGui, at (px, py)
    float mx = 0.f, my = 0.f;                               // io.MousePos now (before this frame's write)
    float ds_w = 0.f, ds_h = 0.f;                           // DisplaySize
    float wx0 = 0.f, wy0 = 0.f, wx1 = 0.f, wy1 = 0.f;       // the F8 window (ImGui coordinates; empty: none)
};
struct FeedOut { bool write = false; float x = 0.f, y = 0.f; bool none = false; };   // write io.MousePos = (x, y) + WantSetMousePos; queue "no mouse"
inline bool park_ok(float x, float y, const FeedIn& in) {
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0.f || y < 0.f || x >= in.ds_w || y >= in.ds_h) return false;
    const bool win = in.wx1 > in.wx0 && in.wy1 > in.wy0;
    return !(win && x >= in.wx0 - k_park_pad && x <= in.wx1 + k_park_pad && y >= in.wy0 - k_park_pad && y <= in.wy1 + k_park_pad);
}
inline void park_choose(Feed& f, const FeedIn& in) {
    if (park_ok(in.mx, in.my, in)) { f.park_x = in.mx; f.park_y = in.my; return; }
    f.park_x = k_park_pad; f.park_y = 0.5f * in.ds_h;
    if (!park_ok(f.park_x, f.park_y, in)) { f.park_x = k_park_pad; f.park_y = k_park_pad; }   // (a window reaching the left edge)
}
/// Once a frame (managed render_imgui, after the window).
inline FeedOut feed_step(Feed& f, const FeedIn& in) {
    FeedOut o;
    if (in.active && std::isfinite(in.px) && std::isfinite(in.py)) {
        if (f.phase == 0) park_choose(f, in);               // (handing back and pointing again: the first park stays)
        f.phase = 1; o.write = true; o.x = in.px; o.y = in.py;
        return o;
    }
    if (f.phase == 1) { f.phase = 2; f.left = k_leave_frames; }
    if (f.phase == 2) {
        if (f.left > 0) { f.left--; o.write = true; o.x = f.park_x; o.y = f.park_y; return o; }
        f.phase = 0; o.none = true;
    }
    return o;
}

// -- the trigger, the hand, the mask, the wheel ---------------------------------------------------------------------------
/// The trigger's button with hysteresis: pressed from k_trig_on, released below k_trig_off.
inline bool trig_step(bool down, float v) { return down ? v >= k_trig_off : v >= k_trig_on; }

enum { HAND_AUTO = 0, HAND_RIGHT = 1, HAND_LEFT = 2 };
struct Owner {
    int h = -1;                          // the hand that owns the pointer (0 left, 1 right; -1 none)
    float off_t[2] = { 1e9f, 1e9f };     // seconds since each hand's ray was last on the panel
};
/// Once a call (dt seconds). on: the hand's ray is on the panel (and it may point: tracked, its aim valid); press: its trigger's
/// press edge this call; held: its trigger is down. mode: HAND_*. Returns the owner.
inline int owner_step(Owner& o, const bool on[2], const bool press[2], const bool held[2], int mode, float dt) {
    if (!(dt >= 0.f) || !std::isfinite(dt)) dt = 0.f;
    bool ok[2], enter[2];
    for (int k = 0; k < 2; ++k) {
        ok[k] = mode == HAND_AUTO || (mode == HAND_RIGHT && k == 1) || (mode == HAND_LEFT && k == 0);
        const bool on_k = ok[k] && on[k];
        enter[k] = on_k && o.off_t[k] >= k_leave_s;       // came onto it (after being off long enough: no flicker at an edge)
        o.off_t[k] = on_k ? 0.f : o.off_t[k] + dt;
    }
    if (o.h >= 0 && !ok[o.h]) o.h = -1;                    // the setting changed
    const bool dragging = o.h >= 0 && held[o.h];
    if (!dragging) {
        // a hand that comes onto the panel or pulls its trigger on it takes the pointer (both at once: the trigger, then the right)
        int take = -1;
        for (int k = 1; k >= 0; --k) if (ok[k] && on[k] && press[k] && k != o.h) { take = k; break; }
        if (take < 0) for (int k = 1; k >= 0; --k) if (enter[k] && k != o.h) { take = k; break; }
        if (take >= 0) o.h = take;
    }
    if (o.h >= 0 && !(o.off_t[o.h] < k_leave_s || held[o.h])) o.h = -1;   // its ray has been off too long (and no drag)
    if (o.h < 0) {
        if (ok[1] && on[1]) o.h = 1;
        else if (ok[0] && on[0]) o.h = 0;
    }
    return o.h;
}

struct Mask { bool trig[2] = { false, false }, stick[2] = { false, false }; };
/// on: the hand's ray is on the panel (and it may point); down: its trigger's button (trig_step); stick: its stick's deflection.
/// The trigger is the panel's while the ray is on it, and a trigger pressed there until it is released; the stick while the ray is
/// on it, and until it is back in the dead zone. Returns the hands masked (bit 0 left, 1 right; trigger or stick).
inline int mask_step(Mask& m, const bool on[2], const bool down[2], const float stick[2]) {
    int bits = 0;
    for (int k = 0; k < 2; ++k) {
        m.trig[k] = on[k] || (m.trig[k] && down[k]);
        m.stick[k] = on[k] || (m.stick[k] && std::isfinite(stick[k]) && fabsf(stick[k]) > k_stick_dz);
        if (m.trig[k] || m.stick[k]) bits |= 1 << k;
    }
    return bits;
}
/// The stick's y (up +) over dt seconds as wheel notches (ImGui: + scrolls up), past the dead zone, rescaled.
inline float wheel(float y, float dt) {
    if (!std::isfinite(y) || !(dt > 0.f)) return 0.f;
    const float a = fabsf(y);
    if (a <= k_stick_dz) return 0.f;
    const float s = (vpanel::clampf(a, 0.f, 1.f) - k_stick_dz) / (1.f - k_stick_dz);
    return (y > 0.f ? s : -s) * k_wheel_rate * dt;
}

// -- the press lock ----------------------------------------------------------------------------------------------------------
struct Lock { bool on = false; float t = 0.f; V3 d0{ 0.f, 0.f, -1.f }; float x = 0.f, y = 0.f; };
/// At the trigger's press edge the position (x, y, in and out) is held for k_lock_s, or until the ray (dir) has turned k_lock_deg
/// from where it was, so a click does not slip off a checkbox while a slider drag still starts. A release ends it.
inline void lock_step(Lock& l, bool press, bool down, V3 dir, float dt, float* x, float* y) {
    const float dl = vpanel::len(dir);
    const V3 d = dl > 1e-6f ? vpanel::mul(dir, 1.f / dl) : V3{ 0.f, 0.f, -1.f };
    if (press) { l.on = true; l.t = 0.f; l.d0 = d; l.x = *x; l.y = *y; return; }
    if (!l.on) return;
    if (!down) { l.on = false; return; }
    l.t += (dt > 0.f && std::isfinite(dt)) ? dt : 0.f;
    const float c = vpanel::clampf(vpanel::dot(d, l.d0), -1.f, 1.f);
    if (l.t >= k_lock_s || acosf(c) * 180.f / vpanel::k_pi >= k_lock_deg) { l.on = false; return; }
    *x = l.x; *y = l.y;
}

// -- the beam ------------------------------------------------------------------------------------------------------------------
/// The beam from a to b as a quad: centred between them, its +Y along a -> b (its length), turned about that axis so its face (+Z)
/// looks at the eye as squarely as it can. Identity when a and b meet.
inline Pose beam_pose(V3 a, V3 b, V3 eye) {
    const V3 c = vpanel::mul(vpanel::add(a, b), 0.5f);
    V3 y = vpanel::sub(b, a);
    const float L = vpanel::len(y);
    if (!(L > 1e-5f) || !vpanel::finite3(y)) return Pose{ Q{ 0.f, 0.f, 0.f, 1.f }, c };
    y = vpanel::mul(y, 1.f / L);
    V3 z = vpanel::sub(eye, c);
    z = vpanel::sub(z, vpanel::mul(y, vpanel::dot(z, y)));   // the eye's direction across the beam
    float zl = vpanel::len(z);
    if (!(zl > 1e-5f)) {   // looking straight along it: any face across it
        z = fabsf(y.y) < 0.9f ? V3{ 0.f, 1.f, 0.f } : V3{ 1.f, 0.f, 0.f };
        z = vpanel::sub(z, vpanel::mul(y, vpanel::dot(z, y))); zl = vpanel::len(z);
    }
    z = vpanel::mul(z, 1.f / zl);
    const V3 x{ y.y * z.z - y.z * z.y, y.z * z.x - y.x * z.z, y.x * z.y - y.y * z.x };   // y x z
    // the rotation matrix [x y z] (columns) to a quaternion
    const float m00 = x.x, m11 = y.y, m22 = z.z, tr = m00 + m11 + m22;
    Q q;
    if (tr > 0.f) { const float s = sqrtf(tr + 1.f) * 2.f; q = Q{ (y.z - z.y) / s, (z.x - x.z) / s, (x.y - y.x) / s, 0.25f * s }; }
    else if (m00 > m11 && m00 > m22) { const float s = sqrtf(1.f + m00 - m11 - m22) * 2.f; q = Q{ 0.25f * s, (y.x + x.y) / s, (z.x + x.z) / s, (y.z - z.y) / s }; }
    else if (m11 > m22) { const float s = sqrtf(1.f + m11 - m00 - m22) * 2.f; q = Q{ (y.x + x.y) / s, 0.25f * s, (z.y + y.z) / s, (z.x - x.z) / s }; }
    else { const float s = sqrtf(1.f + m22 - m00 - m11) * 2.f; q = Q{ (z.x + x.z) / s, (z.y + y.z) / s, 0.25f * s, (x.y - y.x) / s }; }
    return Pose{ q, c };
}

} // namespace vptr
