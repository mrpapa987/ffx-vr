// SPDX-License-Identifier: MIT
// ffxvr_odvis.h -- overdrive visuals (7 Nov 2026, the owner's play2 request: "some kind of object or indicator in our VR hand to
// give some visual representation of what we are doing"). Pure: no Windows, no D3D, no allocation. Builds coloured triangles in
// the XR space the controller poses come in (metres, y up); ffxvr_native.cpp maps them to render space with the VR hands' map
// and appends them to the 3D marker pass (both eyes, at Present, no depth test, the marker shader: colour, or colour x battle.dds
// for the spin count's digits). A unit test checks the geometry.
//
// Only while the matching overdrive's input window is open with its gestures on (plus a 0.4 s fade):
//   every overdrive  a short glowing trail of the active hand (0.4 s), a burst on each recognised gesture (green sent, grey refused)
//   Lulu (Fury)      a ring in the fitted circle's plane at the hand, a progress arc for the game's current turn (its recorded
//                    quadrants, 0x133D731), the spins counted this window (battle.dds digits; dots without the atlas)
//   Tidus            a plain light blade from the right controller (a capsule of light: no game sword), a small timing bar above the
//                    right hand (the bar, the zone and the cursor from the od block) with a mark where each swing landed
//   Auron            the expected sequence as a row of glyphs ahead (arrows; L1 / R1 a bar on that side; the face buttons a
//                    diamond of four dots, the one to press lit in its Quest place and colour: Y top yellow, B right red, A bottom
//                    green, X left blue); done ones grey, the current one larger and ringed
// Sizes (x the size setting, 0.5-2): trail 1 cm wide, bursts up to 7 cm, ring 6 mm wide, digits 3.5 cm, blade 55 cm x 1.6 cm, bar
// 22 x 1.8 cm 16 cm above the hand, glyphs 3.5 cm in a row 0.5 m ahead and 22 cm below the eyes (at most 9 shown; the row
// follows the head's yaw in 0.3 s and never its pitch). Comfort: every vertex fades out between 30 and 20 cm from either eye.
// The bar art (19 Nov 2026, ffxvr_odbar.h; In::bar_art: the local pack loaded, F8 "overdrive bar art" on): Tidus's bar drawn
// from the pack's pieces instead (the same place, opening 22 cm x size, the same zone, cursor and marks), and Lulu gets a
// countdown bar above her ring (16 cm x size; the game's time left); both into a second sink (build's `art`), which the glue
// draws with the targets' shader and the atlas. Without the art everything above is exactly as it was.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

#include "ffxvr_odgest.h"
#include "ffxvr_dmg3d.h"   // dmg::uv: the digits of battle.dds
#include "ffxvr_odbar.h"   // the bar art's atlas and layout

namespace odv {
using odg::V3; using odg::v3; using odg::dot; using odg::cross; using odg::len;

struct Vtx { float p[3]; float c[4]; float t[3]; };   // the marker pass's vertex (MkVertex): t.z 1 = textured
constexpr int k_max_verts = 3072;
constexpr int k_bar_verts = 256;   // the bar art's own sink (a Swordplay bar is at most 102 vertices, Lulu's countdown 54)
struct Col { float r = 1.f, g = 1.f, b = 1.f, a = 1.f; };
inline Col col(float r, float g, float b, float a) { Col c; c.r = r; c.g = g; c.b = b; c.a = a; return c; }
inline Col fade(Col c, float k) { c.a *= k; return c; }
inline V3 norm(V3 a) { const float l = len(a); return l > 1e-9f ? a * (1.f / l) : v3(0.f, 0.f, 0.f); }
inline bool finite3(V3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
/// Rotates v by the unit quaternion q (x, y, z, w).
inline V3 qrot(const float* q, V3 v) {
    const V3 u = v3(q[0], q[1], q[2]); const float w = q[3];
    const V3 t = cross(u, v) * 2.f;
    return v + t * w + cross(u, t);
}

// -- the triangle sink ----------------------------------------------------------------------------------------------------
struct Out {
    Vtx* v = nullptr; int max = 0; int n = 0; int dropped = 0;
    void put(V3 p, Col c, float u = 0.f, float w = 0.f, float tex = 0.f) {
        Vtx& x = v[n++]; x.p[0] = p.x; x.p[1] = p.y; x.p[2] = p.z; x.c[0] = c.r; x.c[1] = c.g; x.c[2] = c.b; x.c[3] = c.a; x.t[0] = u; x.t[1] = w; x.t[2] = tex;
    }
    bool tri(V3 a, Col ca, V3 b, Col cb, V3 c, Col cc) {
        if (n + 3 > max || !finite3(a) || !finite3(b) || !finite3(c)) { dropped++; return false; }
        put(a, ca); put(b, cb); put(c, cc); return true;
    }
    /// a b c d around the quad.
    void quad(V3 a, Col ca, V3 b, Col cb, V3 c, Col cc, V3 d, Col cd) { tri(a, ca, b, cb, c, cc); tri(a, ca, c, cc, d, cd); }
    /// A textured quad (u0 v0 u1 v1 over a b c d = top-left, top-right, bottom-right, bottom-left).
    void quad_uv(V3 a, V3 b, V3 c, V3 d, Col k, const float uv[4]) {
        if (n + 6 > max || !finite3(a) || !finite3(c)) { dropped++; return; }
        put(a, k, uv[0], uv[1], 1.f); put(b, k, uv[2], uv[1], 1.f); put(c, k, uv[2], uv[3], 1.f);
        put(a, k, uv[0], uv[1], 1.f); put(c, k, uv[2], uv[3], 1.f); put(d, k, uv[0], uv[3], 1.f);
    }
};

// -- primitives (billboards face `eye`) ------------------------------------------------------------------------------------
/// A glowing line a -> b: a bright core of half-width w fading to nothing at 2.5 w, facing the eye.
inline void glow_line(Out& o, V3 a, V3 b, float w, Col ca, Col cb, V3 eye) {
    const V3 d = b - a; if (len(d) < 1e-5f) return;
    const V3 s = norm(cross(d, (a + b) * 0.5f - eye)); if (len(s) < 0.5f) return;
    const V3 si = s * w, so = s * (2.5f * w);
    const Col ea = fade(ca, 0.f), eb = fade(cb, 0.f);
    o.quad(a - si, ca, b - si, cb, b + si, cb, a + si, ca);
    o.quad(a + si, ca, b + si, cb, b + so, eb, a + so, ea);
    o.quad(a - so, ea, b - so, eb, b - si, cb, a - si, ca);
}
/// A flat sector of an annulus in the plane (e1, e2) around c: radius r, width w, angles a0..a1 (radians, e1 toward e2).
inline void arc(Out& o, V3 c, V3 e1, V3 e2, float r, float w, float a0, float a1, Col k, int seg) {
    if (seg < 1 || !(a1 > a0)) return;
    const float r0 = r - 0.5f * w, r1 = r + 0.5f * w;
    for (int i = 0; i < seg; ++i) {
        const float t0 = a0 + (a1 - a0) * i / seg, t1 = a0 + (a1 - a0) * (i + 1) / seg;
        const V3 d0 = e1 * std::cos(t0) + e2 * std::sin(t0), d1 = e1 * std::cos(t1) + e2 * std::sin(t1);
        o.quad(c + d0 * r0, k, c + d1 * r0, k, c + d1 * r1, k, c + d0 * r1, k);
    }
}
/// A filled disc (a fan) in the plane (e1, e2): colour ki at the centre, ke at the rim.
inline void disc(Out& o, V3 c, V3 e1, V3 e2, float r, Col ki, Col ke, int seg) {
    for (int i = 0; i < seg; ++i) {
        const float t0 = odg::k_two_pi * i / seg, t1 = odg::k_two_pi * (i + 1) / seg;
        o.tri(c, ki, c + (e1 * std::cos(t0) + e2 * std::sin(t0)) * r, ke, c + (e1 * std::cos(t1) + e2 * std::sin(t1)) * r, ke);
    }
}
/// A quad in the plane (R, U) from local (x0, y0) to (x1, y1) around c.
inline void rect(Out& o, V3 c, V3 R, V3 U, float x0, float y0, float x1, float y1, Col k) {
    o.quad(c + R * x0 + U * y0, k, c + R * x1 + U * y0, k, c + R * x1 + U * y1, k, c + R * x0 + U * y1, k);
}
/// A billboard frame at p: R, U facing the eye, world up kept (yaw-only: falls back to the view's up when looking straight down).
inline void face_axes(V3 p, V3 eye, V3 view_up, V3* R, V3* U) {
    V3 f = p - eye; f.y = 0.f;
    if (len(f) < 1e-4f) { *U = view_up; *R = norm(cross(*U, p - eye)); if (len(*R) < 0.5f) *R = v3(1.f, 0.f, 0.f); return; }
    f = norm(f); *U = v3(0.f, 1.f, 0.f); *R = v3(-f.z, 0.f, f.x);
}

// -- glyphs (Auron) -----------------------------------------------------------------------------------------------------
enum Glyph { G_NONE = 0, G_UP, G_RIGHT, G_DOWN, G_LEFT, G_L1, G_R1, G_Y, G_B, G_A, G_X, G_N };
/// The glyph for a sequence value by the map in use (odg::Driver::map: the game's bindings or the built-in k_auron_map).
inline int glyph_of(const odg::AuronMap& m, uint16_t v) {
    switch (m.button_of(v)) {
        case odg::B_DPAD_UP: return G_UP; case odg::B_DPAD_RIGHT: return G_RIGHT; case odg::B_DPAD_DOWN: return G_DOWN; case odg::B_DPAD_LEFT: return G_LEFT;
        case odg::B_LSHOULDER: return G_L1; case odg::B_RSHOULDER: return G_R1;
        case odg::B_Y: return G_Y; case odg::B_B: return G_B; case odg::B_A: return G_A; case odg::B_X: return G_X;
        default: return G_NONE;
    }
}
/// The face buttons: their place in the diamond (x, y in units of the glyph) and Xbox colour.
inline void face_place(int g, float* x, float* y, Col* c) {
    switch (g) {
        case G_Y: *x = 0.f; *y = 0.32f; *c = col(1.f, 0.85f, 0.15f, 1.f); return;
        case G_B: *x = 0.32f; *y = 0.f; *c = col(0.95f, 0.25f, 0.2f, 1.f); return;
        case G_A: *x = 0.f; *y = -0.32f; *c = col(0.3f, 0.9f, 0.3f, 1.f); return;
        default:  *x = -0.32f; *y = 0.f; *c = col(0.25f, 0.5f, 1.f, 1.f); return;   // G_X
    }
}
/// One glyph of size s (metres, its box) at c on the plane (R, U); k: its colour (alpha = its state).
inline void glyph(Out& o, int g, V3 c, V3 R, V3 U, float s, Col k) {
    if (g >= G_UP && g <= G_LEFT) {   // an arrow: a head and a stem, pointing up, turned to its direction
        V3 a = U, b = R;              // a: the arrow's direction, b: across it
        if (g == G_RIGHT) { a = R; b = U * -1.f; } else if (g == G_DOWN) { a = U * -1.f; b = R * -1.f; } else if (g == G_LEFT) { a = R * -1.f; b = U; }
        o.tri(c + a * (0.5f * s), k, c + a * (0.02f * s) - b * (0.42f * s), k, c + a * (0.02f * s) + b * (0.42f * s), k);
        o.quad(c + a * (0.05f * s) - b * (0.14f * s), k, c + a * (0.05f * s) + b * (0.14f * s), k, c - a * (0.48f * s) + b * (0.14f * s), k, c - a * (0.48f * s) - b * (0.14f * s), k);
        return;
    }
    if (g == G_L1 || g == G_R1) {     // a shoulder: a bar along the top on its side, and a short drop toward the middle
        const float sx = g == G_L1 ? -1.f : 1.f;
        rect(o, c, R, U, sx < 0.f ? -0.5f * s : 0.02f * s, 0.18f * s, sx < 0.f ? -0.02f * s : 0.5f * s, 0.42f * s, k);
        rect(o, c, R, U, sx < 0.f ? -0.5f * s : 0.32f * s, -0.3f * s, sx < 0.f ? -0.32f * s : 0.5f * s, 0.18f * s, k);
        return;
    }
    if (g >= G_Y && g <= G_X) {       // the face buttons: four dots in a diamond, the one to press lit
        for (int f = G_Y; f <= G_X; ++f) {
            float x = 0.f, y = 0.f; Col fc; face_place(f, &x, &y, &fc);
            const bool lit = f == g;
            const Col cc = lit ? col(fc.r, fc.g, fc.b, k.a) : col(0.45f, 0.45f, 0.45f, 0.45f * k.a);
            disc(o, c + R * (x * s) + U * (y * s), R, U, (lit ? 0.17f : 0.11f) * s, cc, cc, 8);
        }
        return;
    }
}

// -- the state ----------------------------------------------------------------------------------------------------------
constexpr double k_fade = 0.4, k_trail = 0.4, k_flash = 0.35, k_mark = 0.8;
constexpr int k_trail_n = 32, k_flash_n = 8, k_mark_n = 4, k_row_max = 9;
struct Trail {
    V3 p[k_trail_n]; double t[k_trail_n] = {}; int n = 0, head = 0;
    void clear() { n = 0; head = 0; }
    /// A sample at most every 1/60 s (the trail's cost does not grow with a 120 Hz headset), only forward in time.
    void push(V3 x, double tt) {
        if (n > 0) { const int last = (head + k_trail_n - 1) % k_trail_n; if (!(tt >= t[last] + 1.0 / 60.0 - 1e-6)) return; }
        p[head] = x; t[head] = tt; head = (head + 1) % k_trail_n; if (n < k_trail_n) n++;
    }
    /// The i-th newest sample (0 = newest).
    int at(int i) const { return (head + k_trail_n - 1 - i) % k_trail_n; }
};
struct Flash { V3 p; double t0 = -1e300; bool sent = false; float r0 = 0.02f; };
struct Mark { double t0 = -1e300; float x = 0.f; bool in_zone = false; };

struct HandIn { bool ok = false; V3 p; float q[4] = { 0.f, 0.f, 0.f, 1.f }; bool aim_ok = false; V3 aim_p; float aim_q[4] = { 0.f, 0.f, 0.f, 1.f }; };
struct In {
    double t = 0.0; bool on = true; float size = 1.f;
    int win = odg::OD_NONE; bool inject = false;              // the window open and whether its gestures are on (the visuals need both)
    HandIn hand[2]; V3 eye = v3(0.f, 1.6f, 0.f); float head_q[4] = { 0.f, 0.f, 0.f, 1.f };   // eye: the head centre (between the eyes)
    // Lulu
    int lead = 1; bool ring_ok = false; V3 c, n; float radius = 0.f; int hist_n = 0; int spins = 0; bool atlas = false, flip = false;
    // Tidus
    int cursor = 0, zone_lo = 0, zone_hi = 0, start = 0, blen = 0;
    // the bar art (ffxvr_odbar.h): drawn into build's `art` sink; with the ornate plate; Lulu's countdown (the shared timer)
    bool bar_art = false, bar_plate = false; float od_left = 0.f, od_total = 0.f;
    // Auron
    uint16_t seq[odg::k_seq_max] = {}; int seq_n = 0; int prog = 0; odg::AuronMap map = odg::k_auron_map;
};
inline float clamp01(float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }
inline float smoothstep(float e0, float e1, float x) { const float t = clamp01((x - e0) / (e1 - e0)); return t * t * (3.f - 2.f * t); }
/// Comfort (od-visual review S4): each vertex's alpha x smoothstep(0.20, 0.30, its distance to the nearer eye), so geometry
/// that comes within 20 cm of an eye (Tidus's blade on a wind-up over the shoulder, a hand's trail at the face, the bar) fades
/// out instead of filling the view. Vertices [from, o.n) of o, in the XR space the eyes are given in. No geometry is added.
constexpr float k_near0 = 0.20f, k_near1 = 0.30f, k_half_ipd = 0.032f;
inline void near_eye_fade(Vtx* v, int from, int to, V3 e0, V3 e1) {
    for (int i = from; i < to; ++i) {
        const V3 p = v3(v[i].p[0], v[i].p[1], v[i].p[2]);
        const float d0 = len(p - e0), d1 = len(p - e1), d = d0 < d1 ? d0 : d1;
        v[i].c[3] *= smoothstep(k_near0, k_near1, d);
    }
}
/// The head's yaw direction (unit, horizontal) from its orientation: the forward's horizontal part, plus the up vector's when
/// the head pitches (looking straight down the forward has none; the up then points ahead), so a pitch alone never turns it.
inline V3 head_yaw(const float* q) {
    const V3 fw = qrot(q, v3(0.f, 0.f, -1.f)), up = qrot(q, v3(0.f, 1.f, 0.f));
    V3 h = v3(fw.x, 0.f, fw.z) - v3(up.x, 0.f, up.z) * fw.y;
    if (!finite3(h) || len(h) < 1e-3f) return v3(0.f, 0.f, -1.f);
    return norm(h);
}
/// The Tidus bar's 0..1 position of a game value v (the bar spans [start, start + len]).
inline float bar_x(int v, int start, int blen) { return blen > 0 ? clamp01(static_cast<float>(v - start) / static_cast<float>(blen)) : 0.f; }

struct Vis {
    Trail trail[2]; Flash fl[k_flash_n]; int fl_next = 0; Mark mk[k_mark_n]; int mk_next = 0;
    In last; bool have_last = false; double close_t = -1e300; int last_win = odg::OD_NONE;
    V3 bar_anchor; bool bar_have = false; double bar_t = 0.0;
    V3 row_f; bool row_have = false; double row_t = 0.0;   // Auron's row: its yaw direction, following the head's slowly (0.3 s; review N5)
    double t_now = 0.0;
    /// Events (the render tick).
    void flash(V3 p, bool sent, double t, float r0 = 0.02f) { Flash& f = fl[fl_next]; fl_next = (fl_next + 1) % k_flash_n; f.p = p; f.t0 = t; f.sent = sent; f.r0 = r0; }
    void swing(double t, float x, bool in_zone) { Mark& m = mk[mk_next]; mk_next = (mk_next + 1) % k_mark_n; m.t0 = t; m.x = x; m.in_zone = in_zone; }
    /// Something still to draw after the window (its fade, a burst): the caller runs the pass while this is true or a window is
    /// open with its gestures on, and skips it otherwise.
    bool active(double t) const {
        if (have_last && t < close_t) return true;
        for (const Flash& f : fl) if (t - f.t0 < k_flash) return true;
        return false;
    }
    /// art: the bar art's sink (drawn by the glue with the targets' shader and the atlas in slot 1), or nullptr: the plain bar.
    int build(const In& in, Out& o, Out* art = nullptr) {
        t_now = in.t;
        const bool live = in.on && in.win != odg::OD_NONE && in.inject;
        if (live) { last = in; have_last = true; close_t = in.t + k_fade; last_win = in.win; }   // close_t: the fade ends k_fade after the last live frame
        else if (have_last && in.t >= close_t) { have_last = false; for (Trail& tr : trail) tr.clear(); bar_have = false; row_have = false; }
        if (!in.on) { have_last = false; return 0; }
        const int n0 = o.n, a0 = art != nullptr ? art->n : 0;
        // the fade: the last live frame's picture, its hands and head now
        float k = 1.f; In s = in;
        if (!live) {
            if (!have_last) { draw_flashes(in, o, odg::clampf(in.size, 0.5f, 2.f)); fade_near(in, o, n0); return o.n - n0; }
            k = static_cast<float>(clamp01(static_cast<float>((close_t - in.t) / k_fade)));
            s = last; s.t = in.t; s.eye = in.eye; std::memcpy(s.head_q, in.head_q, sizeof(s.head_q)); s.hand[0] = in.hand[0]; s.hand[1] = in.hand[1];
        }
        const float S = odg::clampf(s.size, 0.5f, 2.f);
        const V3 view_up = qrot(s.head_q, v3(0.f, 1.f, 0.f));
        // the trails
        for (int h = 0; h < 2; ++h) {
            const bool act = s.win == odg::OD_AURON || (s.win == odg::OD_TIDUS && h == 1) || (s.win == odg::OD_LULU && h == (s.lead < 0 ? 1 : s.lead));
            if (live && s.hand[h].ok) trail[h].push(s.hand[h].p, s.t);
            if (act) draw_trail(trail[h], s, o, 0.005f * S, k, view_up);
        }
        Out* ar = s.bar_art ? art : nullptr;   // the bar art's sink (nullptr: the plain bar, as before)
        if (s.win == odg::OD_LULU) { draw_lulu(s, o, S, k, view_up); if (ar != nullptr) draw_lulu_bar(s, *ar, S, k, view_up, live); }
        else if (s.win == odg::OD_TIDUS) draw_tidus(s, o, S, k, view_up, live, ar);
        else if (s.win == odg::OD_AURON) draw_auron(s, o, S, k);
        draw_flashes(s, o, S);
        fade_near(s, o, n0);
        if (art != nullptr) fade_near(s, *art, a0);
        return o.n - n0;
    }
    /// The near-eye fade over this build's vertices, the eyes half an IPD either side of s.eye (the head centre: the caller
    /// passes the midpoint of the two eye views) along the head's right.
    static void fade_near(const In& s, Out& o, int n0) {
        const V3 r = qrot(s.head_q, v3(1.f, 0.f, 0.f)) * k_half_ipd;
        near_eye_fade(o.v, n0, o.n, s.eye - r, s.eye + r);
    }
    void draw_trail(const Trail& tr, const In& s, Out& o, float w, float k, V3) const {
        for (int i = 0; i + 1 < tr.n; ++i) {
            const int a = tr.at(i), b = tr.at(i + 1);
            const double age_a = s.t - tr.t[a], age_b = s.t - tr.t[b];
            if (age_b > k_trail) break;
            const Col ca = col(0.55f, 0.85f, 1.f, k * 0.8f * static_cast<float>(1.0 - age_a / k_trail)), cb = col(0.55f, 0.85f, 1.f, k * 0.8f * static_cast<float>(1.0 - age_b / k_trail));
            glow_line(o, tr.p[a], tr.p[b], w, ca, cb, s.eye);
        }
    }
    void draw_flashes(const In& s, Out& o, float S) const {
        for (const Flash& f : fl) {
            const double a = s.t - f.t0; if (!(a >= 0.0 && a < k_flash)) continue;
            const float u = static_cast<float>(a / k_flash);
            V3 R, U; face_axes(f.p, s.eye, qrot(s.head_q, v3(0.f, 1.f, 0.f)), &R, &U);
            const Col c = f.sent ? col(0.35f, 1.f, 0.45f, 0.9f * (1.f - u)) : col(0.6f, 0.6f, 0.6f, 0.7f * (1.f - u));
            const float r = (f.r0 + (0.07f - f.r0) * u) * odg::clampf(S, 0.5f, 2.f);
            arc(o, f.p, R, U, r, 0.25f * r, 0.f, odg::k_two_pi, c, 12);
        }
    }
    void draw_lulu(const In& s, Out& o, float S, float k, V3 view_up) const {
        if (!s.ring_ok || !finite3(s.c) || len(s.n) < 0.5f) return;
        const V3 n = norm(s.n);
        V3 e1 = norm(view_up - n * dot(view_up, n)); if (len(e1) < 0.5f) e1 = norm(cross(n, v3(1.f, 0.f, 0.f)));
        const V3 e2 = cross(n, e1);
        const float r = odg::clampf(s.radius, 0.04f, 0.3f);
        arc(o, s.c, e1, e2, r, 0.006f * S, 0.f, odg::k_two_pi, col(1.f, 0.6f, 0.9f, 0.25f * k), 40);                 // the guide
        const int q = s.hist_n < 1 ? 0 : s.hist_n > 5 ? 4 : s.hist_n - 1;                                                // quadrants done of this turn
        if (q > 0) arc(o, s.c, e1, e2, r, 0.009f * S, 0.f, odg::k_two_pi * q / 4.f, col(1.f, 0.55f, 0.95f, 0.9f * k), 10 * q);
        V3 R, U; face_axes(s.c, s.eye, view_up, &R, &U);
        const int sp = s.spins < 0 ? 0 : s.spins;
        if (s.atlas) {   // the spins: battle.dds's digits, 3.5 cm tall, at the ring's centre
            const float h = 0.035f * S, w = h * 49.f / 65.f;
            int d[4]; int nd = 0; int v = sp > 9999 ? 9999 : sp; do { d[nd++] = v % 10; v /= 10; } while (v > 0 && nd < 4);
            for (int i = 0; i < nd; ++i) {
                const float x0 = (0.5f * nd - 1 - i) * w;   // the ones digit rightmost, the number centred
                float uv[4]; dmg::uv(d[i], s.flip, uv);
                const V3 a = s.c + R * x0 + U * (0.5f * h), b = s.c + R * (x0 + w) + U * (0.5f * h);
                o.quad_uv(a, b, b - U * h, a - U * h, col(1.f, 1.f, 1.f, k), uv);
            }
        }
        else for (int i = 0; i < sp && i < 20; ++i) {   // no atlas: a dot a spin (at most 20)
            const float x = (i - 0.5f * ((sp < 20 ? sp : 20) - 1)) * 0.012f * S;
            disc(o, s.c + R * x, R, U, 0.004f * S, col(1.f, 1.f, 1.f, k), col(1.f, 1.f, 1.f, k), 6);
        }
    }
    void draw_tidus(const In& s, Out& o, float S, float k, V3 view_up, bool live, Out* art = nullptr) {
        const HandIn& hd = s.hand[1];
        if (hd.ok) {   // the blade: from the aim pose when there is one (the pointing ray), else the grip's -Z
            const V3 p = hd.aim_ok ? hd.aim_p : hd.p; const V3 f = norm(qrot(hd.aim_ok ? hd.aim_q : hd.q, v3(0.f, 0.f, -1.f)));
            double since = 1e9; for (const Mark& m : mk) if (s.t - m.t0 < since) since = s.t - m.t0;
            const float hot = since >= 0.0 && since < 0.2 ? static_cast<float>(1.0 - since / 0.2) : 0.f;   // a swing: the blade whitens
            const Col c = col(0.6f + 0.4f * hot, 0.9f + 0.1f * hot, 1.f, 0.85f * k);
            glow_line(o, p + f * 0.05f, p + f * (0.05f + 0.55f * S), 0.008f * S, c, fade(c, 0.6f), s.eye);
        }
        // the bar: 16 cm above the right hand, following it slowly (0.5 s), facing the eye
        if (hd.ok && live) {
            const V3 want = hd.p + v3(0.f, 0.16f * S, 0.f);
            if (!bar_have) { bar_anchor = want; bar_have = true; }
            else { const float a = static_cast<float>(1.0 - std::exp(-(s.t - bar_t > 0.0 ? s.t - bar_t : 0.0) / 0.5)); bar_anchor = bar_anchor + (want - bar_anchor) * a; }
            bar_t = s.t;
        }
        if (!bar_have || s.blen <= 0) return;
        V3 R, U; face_axes(bar_anchor, s.eye, view_up, &R, &U);
        if (art != nullptr) { draw_bar_art(*art, bar_anchor, R, U, 0.22f * S, s, k, true); return; }   // the bar art: the same place and width
        const float W = 0.22f * S, H = 0.018f * S;
        auto X = [&](float x01) { return (x01 - 0.5f) * W; };
        rect(o, bar_anchor, R, U, -0.5f * W, -0.5f * H, 0.5f * W, 0.5f * H, col(0.1f, 0.1f, 0.15f, 0.6f * k));
        const float z0 = bar_x(s.zone_lo, s.start, s.blen), z1 = bar_x(s.zone_hi, s.start, s.blen);
        if (z1 > z0) rect(o, bar_anchor, R, U, X(z0), -0.5f * H, X(z1), 0.5f * H, col(0.3f, 0.9f, 0.35f, 0.85f * k));
        const float cx = X(bar_x(s.cursor, s.start, s.blen));
        rect(o, bar_anchor, R, U, cx - 0.0015f * S, -0.9f * H, cx + 0.0015f * S, 0.9f * H, col(1.f, 1.f, 1.f, k));
        for (const Mark& m : mk) {   // where each swing landed: green in the zone, red outside (0.8 s)
            const double a = s.t - m.t0; if (!(a >= 0.0 && a < k_mark)) continue;
            const float u = 1.f - static_cast<float>(a / k_mark), mx = X(m.x);
            rect(o, bar_anchor, R, U, mx - 0.003f * S, -1.2f * H, mx + 0.003f * S, 1.2f * H, m.in_zone ? col(0.4f, 1.f, 0.45f, u * k) : col(1.f, 0.3f, 0.25f, u * k));
        }
    }
    /// One textured rectangle of a piece of the bar art in the plane (R, U) at c, local x0..x1, y0..y1 (y up), the piece's
    /// columns p0..p1 and all its rows; t.z 2 (the atlas in slot 1), 6 additive (unused so far). Dropped and counted like Out::tri when full.
    static void tex_rect(Out& o, V3 c, V3 R, V3 U, float x0, float y0, float x1, float y1, int piece, float p0, float p1, Col k, bool add) {
        if (!(x1 > x0) || !(y1 > y0)) return;
        const odb::Cell cl = odb::cell(piece);
        const float u0 = odb::ux(cl, p0), u1 = odb::ux(cl, p1), v0 = odb::vy(cl, 0.f), v1 = odb::vy(cl, static_cast<float>(cl.h));
        const V3 a = c + R * x0 + U * y1, b = c + R * x1 + U * y1, d = c + R * x1 + U * y0, e = c + R * x0 + U * y0;
        if (o.n + 6 > o.max || !finite3(a) || !finite3(d)) { o.dropped++; return; }
        const float code = add ? 6.f : 2.f;
        o.put(a, k, u0, v0, code); o.put(b, k, u1, v0, code); o.put(d, k, u1, v1, code);
        o.put(a, k, u0, v0, code); o.put(d, k, u1, v1, code); o.put(e, k, u0, v1, code);
    }
    /// The slices of a piece at the height y0..y1.
    static void tex_segs(Out& o, V3 c, V3 R, V3 U, const odb::Seg* sg, int n, float y0, float y1, int piece, Col k, bool add) {
        for (int i = 0; i < n; ++i) tex_rect(o, c, R, U, sg[i].x0, y0, sg[i].x1, y1, piece, sg[i].p0, sg[i].p1, k, add);
    }
    /// The bar art (ffxvr_odbar.h): a bar whose opening is W wide (grown by the readable-size guard only when it would show under
    /// 0.9 degrees at arm's length) at c on the plane (R, U), into `a`, drawn by the glue with the targets' shader and the atlas in slot 1. From
    /// the back: the ornate plate (In::bar_plate), a dark backing in the opening (the sheet's black, so the light pieces look as
    /// drawn), the empty track; Tidus: the golden zone over the game's zone (blended as a light piece: drawn additively it washed
    /// out to white over the track in the mock-up), the frame, the cursor's pinch pair and the
    /// swing marks (plain colour, as before); Lulu: the lit track over the time left, then the frame. k: the fade.
    void draw_bar_art(Out& a, V3 c, V3 R, V3 U, float W, const In& s, float k, bool tidus) const {
        odb::Layout L = odb::layout(W);
        const float g = odb::readable_scale(L.hi, odb::k_guard_dist);   // (review N8: at a fixed arm's length, so the size never pumps with the hand)
        if (g > 1.f) L = odb::layout(W * g);
        if (!(L.W > 0.f)) return;
        const Col wk = col(1.f, 1.f, 1.f, k);
        if (s.bar_plate) tex_rect(a, c, R, U, -0.5f * L.pw, L.pcy - 0.5f * L.ph, 0.5f * L.pw, L.pcy + 0.5f * L.ph, odb::P_PLATE, 0.f, static_cast<float>(odb::cell(odb::P_PLATE).w), wk, false);
        rect(a, c, R, U, -0.5f * L.W, -0.5f * L.hi, 0.5f * L.W, 0.5f * L.hi, col(0.01f, 0.015f, 0.03f, 0.9f * k));
        odb::Seg sg[5];
        int n = odb::empty_segs(L, sg);
        tex_segs(a, c, R, U, sg, n, -0.5f * L.th, 0.5f * L.th, odb::P_EMPTY, tidus ? wk : col(0.6f, 0.6f, 0.6f, k), false);   // (Lulu: dimmed, so the time left stands out)
        if (!tidus) {   // Lulu: the time left
            n = odb::fill_segs(L, odb::countdown(s.od_left, s.od_total), sg);
            tex_segs(a, c, R, U, sg, n, -0.5f * L.th, 0.5f * L.th, odb::P_FILL, wk, false);
        }
        else {          // Tidus: the game's zone (its end bars at its ends), under the frame
            n = odb::zone_segs(L, odb::bar_lx(L, bar_x(s.zone_lo, s.start, s.blen)), odb::bar_lx(L, bar_x(s.zone_hi, s.start, s.blen)), sg);
            tex_segs(a, c, R, U, sg, n, -0.5f * L.zh, 0.5f * L.zh, odb::P_ZONE, wk, false);
        }
        tex_rect(a, c, R, U, L.fcx - 0.5f * L.fw, L.fcy - 0.5f * L.fh, L.fcx + 0.5f * L.fw, L.fcy + 0.5f * L.fh, odb::P_FRAME, 0.f, static_cast<float>(odb::cell(odb::P_FRAME).w), wk, false);
        if (!tidus) return;
        const float cx = bar_x(s.cursor, s.start, s.blen);
        const odb::Rect dn = odb::cursor_rect(L, cx, true), up = odb::cursor_rect(L, cx, false);
        tex_rect(a, c, R, U, dn.x0, dn.y0, dn.x1, dn.y1, odb::P_DOWN, 0.f, static_cast<float>(odb::cell(odb::P_DOWN).w), wk, false);
        tex_rect(a, c, R, U, up.x0, up.y0, up.x1, up.y1, odb::P_UP, 0.f, static_cast<float>(odb::cell(odb::P_UP).w), wk, false);
        for (const Mark& m : mk) {   // where each swing landed: green in the zone, red outside (0.8 s), as the plain bar's
            const double ag = s.t - m.t0; if (!(ag >= 0.0 && ag < k_mark)) continue;
            const float u = 1.f - static_cast<float>(ag / k_mark), mx = odb::bar_lx(L, m.x), hw = 0.003f * L.W / 0.22f;   // (3 mm a side at 22 cm, as before)
            rect(a, c, R, U, mx - hw, -0.7f * L.hi, mx + hw, 0.7f * L.hi, m.in_zone ? col(0.4f, 1.f, 0.45f, u * k) : col(1.f, 0.3f, 0.25f, u * k));
        }
    }
    /// Lulu's countdown (the bar art only): above her ring (its fitted centre and radius), or 16 cm above the leading hand until
    /// there is a ring, following in 0.5 s (as Tidus's bar), facing the eye; 16 cm x the size wide; the game's time left.
    void draw_lulu_bar(const In& s, Out& a, float S, float k, V3 view_up, bool live) {
        const int h = s.lead < 0 ? 1 : s.lead > 1 ? 1 : s.lead;
        const bool ring = s.ring_ok && finite3(s.c) && len(s.n) >= 0.5f;
        const float W = 0.16f * S;
        const odb::Layout L = odb::layout(W);
        if (live && (ring || s.hand[h].ok)) {
            const float r = odg::clampf(s.radius, 0.04f, 0.3f);
            const V3 want = ring ? s.c + v3(0.f, r + 0.03f * S + 0.5f * L.fh, 0.f) : s.hand[h].p + v3(0.f, 0.16f * S, 0.f);
            if (!bar_have) { bar_anchor = want; bar_have = true; }
            else { const float al = static_cast<float>(1.0 - std::exp(-(s.t - bar_t > 0.0 ? s.t - bar_t : 0.0) / 0.5)); bar_anchor = bar_anchor + (want - bar_anchor) * al; }
            bar_t = s.t;
        }
        if (!bar_have) return;
        V3 R, U; face_axes(bar_anchor, s.eye, view_up, &R, &U);
        draw_bar_art(a, bar_anchor, R, U, W, s, k, false);
    }
    void draw_auron(const In& s, Out& o, float S, float k) {
        if (s.seq_n <= 0) return;
        // the row's direction: the head's yaw (never its pitch: looking down at the hands does not move it), followed in 0.3 s
        const V3 want = head_yaw(s.head_q);
        if (!row_have) { row_f = want; row_have = true; }
        else {
            const float a = static_cast<float>(1.0 - std::exp(-(s.t - row_t > 0.0 ? s.t - row_t : 0.0) / 0.3));
            const V3 m = row_f + (want - row_f) * a;
            row_f = len(m) > 1e-3f ? norm(m) : want;   // a half turn in one step: take the new yaw
        }
        row_t = s.t;
        const V3 f = row_f;
        const V3 R = v3(-f.z, 0.f, f.x), U = v3(0.f, 1.f, 0.f);
        const V3 A = s.eye + f * 0.5f + v3(0.f, -0.22f, 0.f);
        const int n = s.seq_n > odg::k_seq_max ? odg::k_seq_max : s.seq_n;
        int first = 0; if (n > k_row_max) { first = s.prog - k_row_max / 2; if (first < 0) first = 0; if (first > n - k_row_max) first = n - k_row_max; }
        const int shown = n < k_row_max ? n : k_row_max;
        const float gs = 0.035f * S, gap = 0.048f * S;
        for (int i = 0; i < shown; ++i) {
            const int j = first + i;
            const V3 c = A + R * ((i - 0.5f * (shown - 1)) * gap);
            const bool done = j < s.prog, cur = j == s.prog;
            const Col base = done ? col(0.5f, 0.5f, 0.5f, 0.3f * k) : cur ? col(1.f, 1.f, 1.f, k) : col(0.85f, 0.9f, 1.f, 0.7f * k);
            if (cur) arc(o, c, R, U, 0.62f * gs * 1.25f, 0.07f * gs, 0.f, odg::k_two_pi, col(1.f, 0.85f, 0.3f, 0.9f * k), 24);
            const int g = glyph_of(s.map, s.seq[j]);
            if (g == G_NONE) { disc(o, c, R, U, 0.1f * gs, base, base, 8); continue; }   // a value off the map: a small dot
            glyph(o, g, c, R, U, cur ? gs * 1.25f : gs, base);
        }
    }
};

}  // namespace odv
