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
#pragma once
#define FFXVR_POINTER_REV 1
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
