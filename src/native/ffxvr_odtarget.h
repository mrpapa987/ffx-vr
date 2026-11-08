// SPDX-License-Identifier: MIT
// ffxvr_odtarget.h -- Auron's Bushido, "strike the targets" (od_gest_auron = 2; 7 Nov 2026), and F8's Practice targets. Pure:
// no Windows, no D3D, no logging, no allocation. ffxvr_native.cpp ticks the Runner once a Present on the render thread
// (od_gest_tick, right after the gesture driver), builds its triangles once a Present in the XR space of the controller poses
// and appends them to the 3D marker pass with the VR hands' map (as the overdrive visuals, ffxvr_odvis.h); the input thread's
// arbitration (Arb) and trigger hold-back (trig_hold) run in odg_input / vrpad_triggers. A unit test drives it with
// synthetic blade strokes and a transcription of the game's Bushido input state with the press slot's poll model.
//
//   geo / latch / slot  where the targets are: two alternating lanes, the strike row 0.60 m ahead and 0.20 m below the eyes,
//                       queued rungs behind and above; the frame is latched once a round from the head (and the hands) and
//                       stays fixed in the room (a recentre re-latches)
//   sweep / judge       the swept blade (sub-stepped by rotation, 8 points along a slightly curved blade) against a target's
//                       hit sphere; a hit needs 1.2 m/s at the contact and, for an arrow, the in-plane velocity within 55
//                       degrees of the arrow; the face and shoulder buttons take any direction (a stab too)
//   Runner              a round (a Bushido window or a practice round): the fly-in, the two hittable fronts in either order,
//                       the re-arm per hand and lane, deflects, pre-hits, rewinds, the end; the closed-loop injection on the
//                       game's own progress (a press of seq[progress] only when target `progress` was struck, the slot is free
//                       and no player button was pressed in the last 80 ms; one resend 150 ms after a press the game did not
//                       show); the cut halves, sparks, cut lines and the blades; the odt: lines
//   Arb / trig_hold     the input thread: a new press of a Bushido button by the player cancels our press while no poll has
//                       carried it yet; the VR triggers are held back during a targets round
//
// Game facts (our exe; ffxvr_odgest.h has the addresses): Auron's state 4 compares the new-press word (u16 0x25D09D4, the
// newly pressed bits of a new pad packet) with seq[progress]: equal -> progress + 1, other non-zero -> progress 0; progress
// == length -> state 5; the timer word 2 = running with f32 seconds left. The press values come from the map in use
// (odg::Driver::map: the game's pad bindings read at the window's open, else odg::k_auron_map: 0x0020 = A (confirm),
// 0x0040 = B, 0x0010 = Y, 0x0080 = X). Nothing here copies code from any project; the look (an octagonal seal plate with an
// engraved stroke symbol, a single-edged curved blade with a round guard, gold left and steel-cyan right) is this mod's own.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ffxvr_odgest.h"
#include "ffxvr_odvis.h"

namespace odt {
using odg::V3; using odg::v3; using odg::dot; using odg::cross; using odg::len;
using odv::Vtx; using odv::Out; using odv::Col; using odv::col; using odv::norm; using odv::finite3; using odv::qrot;

constexpr int k_n_max = odg::k_seq_max;    // 32
constexpr int k_max_verts = 4608;          // the targets' own share of the marker pass (6 plates, halves, sparks, blades, trails)
constexpr int k_blade_reserve = 640;       // kept for the two blades and trails (drawn last, on top): the rest drops first
constexpr float k_deg = 0.01745329252f;
constexpr float k_g = 9.81f;
constexpr double k_fly_s = 0.40, k_fly_gap_s = 0.06, k_slide_s = 0.10, k_rewind_s = 0.15;   // the fly-in, the queue's slide, a rewind
// The fly-in starts beyond, above and outside its spot (review N6: from 3 m straight ahead the ease-out came at the eyes at about
// 20 m/s): 0.22 m deeper, 0.40 m higher, 0.15 m out on its lane's side, over 0.40 s, so it swoops down into place; the ease-out's
// opening speed toward the eyes stays under 3 m/s (unit-tested). Never nearer than its spot, for every slider setting (review
// R2-N3): the straight path's distance to the eyes falls monotonically to the spot's exactly when dot(offset, spot - eyes) >= 0,
// so for a low, near spot the upward part is cut down until that holds (spawn_offset).
constexpr float k_spawn_fwd = 0.22f, k_spawn_up = 0.40f, k_spawn_out = 0.15f;
// Every motion (the fly-in, the queue's slide, a rewind) lasts at least its base time and long enough that the ease-out's opening
// speed toward the eyes stays under k_toward_mps (review round 3: over the slider ranges the fly-in reached 3.25 m/s and the 100 ms
// slide 8.7 m/s); the fly-in stays within 0.5 s, the slide takes about 0.15-0.3 s.
constexpr float k_toward_mps = 2.8f;
constexpr double k_resend_s = 0.15, k_settle_s = 0.08, k_ours_s = 0.3;                      // the injection (design 6.2)
// The settle in frames as well as time (review S2): after the player's new press, and after our press's slot frees before a
// resend, k_settle_frames Presents with a fresh read of the game's state must pass: the game compares at its next logic frame
// and the render sees it at the Present that ends that frame (the same thread), so a slow frame stretches the time, not the count.
constexpr int k_settle_frames = 2;
constexpr double k_gate_grace_s = 0.25;                                                    // the injection gate may drop this long without ending the round (review S1)
constexpr double k_rearm_s = 0.25; constexpr float k_rearm_m = 0.02f;                      // the re-arm (design 3.3)
constexpr float k_snap_min = 0.04f, k_snap_rest = 0.08f, k_jump_speed = 8.f;               // a pose snap; a jump (m/s at the grip)
constexpr double k_gap_s = 0.12;                                                           // a longer gap between samples ends the pair
constexpr double k_practice_s = 4.0, k_end_s = 0.35;                                       // practice's bar; the end's fade
constexpr double k_half_s = 0.45, k_spark_s = 0.20, k_line_s = 0.12;                       // the cut's pieces
constexpr int k_cut_n = 6;

inline float clampf(float v, float lo, float hi) { return odg::clampf(v, lo, hi); }
inline float fin(float v, float d) { return std::isfinite(v) ? v : d; }
/// Ease-out cubic: fast start, gentle arrival, never past 1 (no overshoot: a flying target never passes its spot).
inline float ease_out(float u) { u = odv::clamp01(u); const float a = 1.f - u; return 1.f - a * a * a; }
/// v rotated about the unit axis by ang (radians).
inline V3 rot_axis(V3 v, V3 axis, float ang) { const float c = std::cos(ang), s = std::sin(ang); return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.f - c)); }

// -- settings and the layout ----------------------------------------------------------------------------------------------
/// ini od_targets_blade_pitch / _dist / _height / _size (F8 sliders); the rest are the design's defaults (section 8).
struct Cfg {
    float reach = 0.45f;       // m: the blade's length; under 5 cm: a fist (the knuckles) and the fist layout
    float pitch_deg = 0.f;     // the blade's tilt from the grip's -Z (+: tip up), -45..45
    float dist = 0.60f, height = -0.20f, spread = 0.32f;   // the strike row: forward, height from the eyes, lane to lane (m)
    float size = 1.f;          // 0.6..1.6: the drawn size, the hit sphere and the spacing scale with it
    float hit_cm = 10.f, dir_deg = 55.f, speed = 1.2f;     // hit sphere at size 1, arrow tolerance, contact speed (m/s)
    int preview = 2;           // queued targets shown per lane (0..3); the rest are dots
    bool shoulder_hand = false, body = true, deflect_buzz = false, haptics = true;
};
struct Geo {
    bool fist = false;
    float D = 0.6f, H = -0.2f, X = 0.16f, dz = 0.15f, dy = 0.10f, dx = 0.06f;   // the strike row and the rung steps (m)
    float R = 0.10f, rad = 0.065f, th = 0.02f;                                  // hit sphere, plate circumradius, plate thickness
    float speed = 1.2f, cos_dir = 0.5736f, pitch = 0.f, s0 = 0.06f, s1 = 0.51f, curve = 0.025f; int npts = 8;
    float size = 1.f;
};
inline Geo geo(const Cfg& c) {
    Geo g;
    const float size = clampf(fin(c.size, 1.f), 0.6f, 1.6f), reach = clampf(fin(c.reach, 0.45f), 0.f, 0.8f);
    const float dist = clampf(fin(c.dist, 0.6f), 0.35f, 0.9f), height = clampf(fin(c.height, -0.2f), -0.5f, 0.f), spread = clampf(fin(c.spread, 0.32f), 0.2f, 0.5f);
    g.size = size; g.fist = reach < 0.05f;
    g.D = g.fist ? fmaxf(0.30f, dist - 0.15f) : dist;
    g.H = g.fist ? fmaxf(-0.55f, height - 0.08f) : height;
    g.X = (0.5f * spread - (g.fist ? 0.01f : 0.f)) * size;
    g.dz = (g.fist ? 0.12f : 0.15f) * size; g.dy = (g.fist ? 0.08f : 0.10f) * size; g.dx = (g.fist ? 0.05f : 0.06f) * size;
    g.rad = (g.fist ? 0.06f : 0.065f) * size; g.th = 0.02f * size;
    g.R = (clampf(fin(c.hit_cm, 10.f), 6.f, 16.f) + (g.fist ? 1.f : 0.f)) * 0.01f * size;
    g.speed = clampf(fin(c.speed, 1.2f), 0.6f, 3.f); if (g.fist && g.speed > 1.f) g.speed = 1.f;
    g.cos_dir = std::cos(clampf(fin(c.dir_deg, 55.f), 30.f, 80.f) * k_deg);
    g.pitch = clampf(fin(c.pitch_deg, 0.f), -45.f, 45.f) * k_deg;
    if (g.fist) { g.s0 = 0.f; g.s1 = 0.10f; g.npts = 3; g.curve = 0.f; }
    else { g.s0 = 0.06f; g.s1 = 0.06f + reach; g.npts = 8; g.curve = 0.025f * reach / 0.45f; }
    return g;
}
/// The round's frame: e0 between the eyes, f the forward (horizontal), r its right, u world up (the XR space, y up, -z ahead).
struct Frame { bool ok = false; V3 e0 = v3(0.f, 1.6f, 0.f), f = v3(0.f, 0.f, -1.f), r = v3(1.f, 0.f, 0.f), u = v3(0.f, 1.f, 0.f); };
/// Latched at a round's start: the head's yaw (odv::head_yaw: never its pitch); with `body`, both hands tracked and their
/// midpoint at least 15 cm ahead, the forward leans half way toward the hands (at most 30 degrees).
inline Frame latch(V3 eye, const float* head_q, const bool hand_ok[2], const V3 hand_p[2], bool body) {
    Frame fr; fr.e0 = eye; fr.ok = finite3(eye);
    const V3 f0 = odv::head_yaw(head_q); V3 f = f0;
    if (body && hand_ok[0] && hand_ok[1] && finite3(hand_p[0]) && finite3(hand_p[1])) {
        V3 m = (hand_p[0] + hand_p[1]) * 0.5f - eye; m.y = 0.f;
        if (dot(m, f0) >= 0.15f) {
            V3 b = norm(f0 + m * 0.5f);
            if (dot(b, f0) < std::cos(30.f * k_deg)) { const V3 p = norm(b - f0 * dot(b, f0)); b = f0 * std::cos(30.f * k_deg) + p * std::sin(30.f * k_deg); }
            if (len(b) > 0.5f) f = norm(b);
        }
    }
    fr.f = f; fr.r = v3(-f.z, 0.f, f.x); fr.u = v3(0.f, 1.f, 0.f);
    return fr;
}
/// The spot of a lane's rung (lane 0 left, 1 right; rung 0 the strike row).
inline V3 slot(const Frame& fr, const Geo& g, int lane, int rung) {
    const float s = lane == 0 ? -1.f : 1.f, k = static_cast<float>(rung);
    return fr.e0 + fr.f * (g.D + k * g.dz) + fr.u * (g.H + k * g.dy) + fr.r * (s * (g.X + k * g.dx));
}
inline float rung_scale(int rung) { float s = 1.f; for (int i = 0; i < rung && i < 4; ++i) s *= 0.75f; return s; }
/// A plate's axes: N toward e0 (fixed per spot, not billboarded each frame), R = the frame's right in its plane, U = N x R.
struct Axes3 { V3 R = v3(1.f, 0.f, 0.f), U = v3(0.f, 1.f, 0.f), N = v3(0.f, 0.f, 1.f); };
inline Axes3 plate_axes(const Frame& fr, V3 c) {
    Axes3 a; a.N = norm(fr.e0 - c); if (len(a.N) < 0.5f) a.N = fr.f * -1.f;
    a.R = norm(fr.r - a.N * dot(fr.r, a.N)); if (len(a.R) < 0.5f) a.R = fr.r;
    a.U = cross(a.N, a.R);
    return a;
}

// -- symbols ----------------------------------------------------------------------------------------------------------------
// odv::Glyph values: G_UP..G_LEFT arrows, G_L1 / G_R1 shoulders, G_Y / G_B / G_A / G_X faces, G_NONE a value off the map ("?",
// not hittable: the player uses the button). The symbol follows the map in use (odv::glyph_of).
static const char* const k_sym_name[odv::G_N] = { "? (off the map)", "Up", "Right", "Down", "Left", "L1", "R1", "Y", "B", "A", "X" };
inline const char* sym_name(int s) { return s >= 0 && s < odv::G_N ? k_sym_name[s] : "?"; }
inline bool sym_arrow(int s) { return s >= odv::G_UP && s <= odv::G_LEFT; }
inline V3 arrow_dir(int s, const Axes3& a) {
    switch (s) { case odv::G_UP: return a.U; case odv::G_RIGHT: return a.R; case odv::G_DOWN: return a.U * -1.f; case odv::G_LEFT: return a.R * -1.f; default: return v3(0.f, 0.f, 0.f); }
}
/// The palette (design 4.1): cool white-cyan arrows, amber face buttons, violet shoulders; grey off the map.
inline Col sym_col(int s) {
    if (sym_arrow(s)) return col(0.80f, 0.97f, 1.f, 1.f);
    if (s >= odv::G_Y && s <= odv::G_X) return col(1.f, 0.74f, 0.28f, 1.f);
    if (s == odv::G_L1 || s == odv::G_R1) return col(0.76f, 0.54f, 1.f, 1.f);
    return col(0.6f, 0.6f, 0.6f, 1.f);
}
/// Lanes: alternating from the left (design 2.3); with shoulder_hand L1 goes left and R1 right, the rest alternate from their
/// predecessor's lane.
inline void assign_lanes(const int* sym, int n, bool shoulder, int8_t* lane) {
    for (int i = 0; i < n; ++i) {
        int l = i == 0 ? 0 : 1 - lane[i - 1];
        if (shoulder && sym[i] == odv::G_L1) l = 0; else if (shoulder && sym[i] == odv::G_R1) l = 1;
        lane[i] = static_cast<int8_t>(l);
    }
}

// -- the blade and the swept test ----------------------------------------------------------------------------------------------
/// A blade pose: the grip p, the blade's direction d (the grip's -Z tilted by the pitch about the grip's X), the spine side u.
struct Blade { V3 p, d = v3(0.f, 0.f, -1.f), u = v3(0.f, 1.f, 0.f); };
inline Blade blade_of(V3 p, const float* q, float pitch) {
    Blade b; b.p = p; const float s = std::sin(pitch), c = std::cos(pitch);
    b.d = norm(qrot(q, v3(0.f, s, -c))); b.u = norm(qrot(q, v3(0.f, c, s)));
    if (len(b.d) < 0.5f) b.d = v3(0.f, 0.f, -1.f);
    if (len(b.u) < 0.5f) b.u = v3(0.f, 1.f, 0.f);
    return b;
}
/// The point at s metres along the blade (curving toward the spine: the sagitta g.curve at the tip).
inline V3 blade_pt(const Blade& b, const Geo& g, float s) {
    const float w = g.s1 > g.s0 ? (s - g.s0) / (g.s1 - g.s0) : 0.f;
    return b.p + b.d * s + b.u * (g.curve * w * w);
}
inline Blade blade_lerp(const Blade& a, const Blade& b, float t) {
    Blade r; r.p = a.p + (b.p - a.p) * t;
    r.d = norm(a.d + (b.d - a.d) * t); if (len(r.d) < 0.5f) r.d = t < 0.5f ? a.d : b.d;
    V3 u = a.u + (b.u - a.u) * t; u = norm(u - r.d * dot(u, r.d)); r.u = len(u) > 0.5f ? u : a.u;
    return r;
}
/// The blade's nearest sampled point to C.
inline float blade_dist(const Blade& b, const Geo& g, V3 C) {
    float m = 1e9f;
    for (int i = 0; i < g.npts; ++i) { const float s = g.s0 + (g.s1 - g.s0) * static_cast<float>(i) / static_cast<float>(g.npts - 1); const float d = len(blade_pt(b, g, s) - C); if (d < m) m = d; }
    return m;
}
struct Contact { float dist = 1e9f; V3 v, at; float u = 0.f; };   // the closest approach, its velocity (m/s), the point, its time in the pair (0..1)
/// The swept blade from pose a to pose b (dt seconds) against C (design 3.2): k = ceil(angle / 0.15 rad) sub-steps (1..6),
/// npts points along the blade, each point's segment's closest approach; the contact is the closest one.
inline Contact sweep(const Blade& a, const Blade& b, float dt, V3 C, const Geo& g) {
    Contact best;
    if (!(dt > 0.f) || g.npts < 2) return best;
    const float ang = std::acos(clampf(dot(a.d, b.d), -1.f, 1.f));
    int k = static_cast<int>(std::ceil(ang / 0.15f)); if (k < 1) k = 1; if (k > 6) k = 6;
    for (int j = 0; j < k; ++j) {
        const Blade pa = blade_lerp(a, b, static_cast<float>(j) / static_cast<float>(k)), pb = blade_lerp(a, b, static_cast<float>(j + 1) / static_cast<float>(k));
        for (int i = 0; i < g.npts; ++i) {
            const float s = g.s0 + (g.s1 - g.s0) * static_cast<float>(i) / static_cast<float>(g.npts - 1);
            const V3 A = blade_pt(pa, g, s), B = blade_pt(pb, g, s), AB = B - A;
            const float ab2 = dot(AB, AB);
            const float w = ab2 > 1e-12f ? clampf(dot(C - A, AB) / ab2, 0.f, 1.f) : 0.f;
            const V3 P = A + AB * w; const float d = len(P - C);
            if (d < best.dist) { best.dist = d; best.v = AB * (static_cast<float>(k) / dt); best.at = P; best.u = (static_cast<float>(j) + w) / static_cast<float>(k); }
        }
    }
    return best;
}
enum Judge { J_NONE = 0, J_HIT, J_WRONG, J_SLOW };   // J_WRONG: an arrow struck fast in the wrong direction (a deflect at the pass's end)
/// A contact against a target (design 3.2): an arrow needs |v in its plane| >= speed and within the tolerance of the arrow;
/// a button |v| >= speed in any direction (slash, chop, stab). deg: the arrow's direction error.
inline int judge(const Contact& c, int sym, const Axes3& a, const Geo& g, float* deg = nullptr) {
    if (deg) *deg = 0.f;
    if (sym_arrow(sym)) {
        const V3 vp = c.v - a.N * dot(c.v, a.N); const float sp = len(vp);
        if (sp < g.speed) return J_SLOW;
        const float cs = dot(vp, arrow_dir(sym, a)) / sp;
        if (deg) *deg = std::acos(clampf(cs, -1.f, 1.f)) / k_deg;
        return cs >= g.cos_dir ? J_HIT : J_WRONG;
    }
    if (sym == odv::G_NONE) return J_NONE;
    return len(c.v) >= g.speed ? J_HIT : J_SLOW;
}

// -- the input thread: arbitration and the trigger hold-back --------------------------------------------------------------------
constexpr uint16_t k_bushido_mask = 0x000F | 0x0300 | 0xF000;   // the d-pad, LB / RB, A B X Y (XInput)
/// The buttons whose new press cancels ours and starts the settle (review N5): every XInput button the game reads (all but the
/// guide bits 0x0400 / 0x0800): the d-pad, Start, Back, both stick clicks, LB / RB, A B X Y. The game's pad bindings give each a
/// bit (0x632A30's table: X A B Y LB RB LT RT Back Start LS RS and the d-pad), and Bushido resets on ANY non-zero new press, so a
/// Start or a stick click in the poll ours first goes out in would merge into a wrong word as surely as a face button. (The
/// triggers are held back during a targets round; Virtual Desktop's both-clicks chord never reaches the game.)
constexpr uint16_t k_cancel_mask = 0xF3FF;
/// One poll (input thread): phys = the pad's buttons before ours are ORed (the controllers and a pad: the player's own),
/// pkt = this poll's packet number, unpolled = our press is set and no new packet has carried it yet. Returns true: cancel ours
/// (design 6.3: the game ORs every newly pressed button into one word, so ours and the player's in one poll, or the player's
/// advancing the progress while ours waits, would make a wrong input). newly: this poll's NEW packet carries a new press of the
/// player's to the game (the settle starts there, review round 3: a press seen at a poll whose packet did not change reaches the
/// game only with the next one, which could put the game's compare one Present past the two-Present count).
struct Arb {
    uint16_t last = 0;                  // the player's buttons at the last poll
    uint16_t seen = 0, before = 0;      // ... at the last new packet (what the game holds), and before this poll's packet
    uint32_t pkt = 0; uint32_t presses = 0, cancels = 0;
    bool step(uint16_t phys, uint32_t pkt_now, bool on, bool unpolled, bool* newly = nullptr) {
        const uint16_t nw = static_cast<uint16_t>(phys & ~last & k_cancel_mask); last = phys;   // new at this poll: the cancel
        uint16_t gnw = 0;                                                                         // new to the game with this packet: the settle
        before = seen; if (pkt_now != pkt) { pkt = pkt_now; gnw = static_cast<uint16_t>(phys & ~seen & k_cancel_mask); seen = phys; }
        if (newly) *newly = gnw != 0;
        if (gnw) presses++;
        if (on && (nw | gnw) && unpolled) { cancels++; return true; }   // new at this poll, or new to the game with this packet (review R3-S1: a press seen
                                                                        // during a render stall reaches the game with the next packet: ours must not ride it)
        return false;
    }
    /// Our unpolled press waits (is not ORed in yet) while the player holds that same button, or the game still holds it from the
    /// player's last packet (released between two polls): ORed into a held button it would make no new-press edge.
    bool wait(uint16_t phys, uint16_t ours, bool on, bool unpolled) const { return on && unpolled && ((phys | before) & ours) != 0; }
};
/// The press slot's elapsed time for press_hook (input thread; review N2): while our press waits its clock restarts, so the 400 ms
/// cap counts from the first poll it may go out in, not from its posting (a long wait no longer ends in a release unsent).
inline uint32_t hook_el(uint32_t now, uint32_t& t0, bool wait) { if (wait) t0 = now; return now - t0; }
/// The VR triggers during an injecting Bushido window (any scheme) and a targets round (window or practice): a grip
/// squeezed hard on a swing pulls the trigger, and a trigger's new press would be a wrong input.
inline bool hold_triggers(int inject, bool round_on) { return inject == odg::OD_AURON || round_on; }
/// The grips during a targets round (review N8): with grip_lbrb=0 they pass as LB / RB at once, so a hard grip on a swing would be
/// L1 / R1 (a reset): held back (hv_grip_strip's latch: until let go) while the tap build holds them or a round is on.
inline bool strip_grips(bool tap_hold, bool round_on) { return tap_hold || round_on; }
/// The latch (input thread): a trigger held when the hold starts or that is pulled during it reads 0 until it is let go (no
/// edge when the hold ends).
inline void trig_hold(uint8_t& v, bool& back, bool hold) { if (v == 0) back = false; else if (hold) back = true; if (back) v = 0; }
/// The overdrive visuals' window: none for Auron in the targets scheme (the first scheme's glyph row and trails must not draw
/// under the targets).
inline int odv_window(int win, bool targets) { return win == odg::OD_AURON && targets ? odg::OD_NONE : win; }

// -- the runner -----------------------------------------------------------------------------------------------------------------
enum End { E_NONE = 0, E_SUCCESS, E_TIMEOUT, E_CLOSED, E_ABORTED, E_N };
static const char* const k_end_name[E_N] = { "-", "success", "timed out", "closed (the window ended another way)", "interrupted (an overdrive window opened)" };
struct HandIn { bool ok = false; V3 p; float q[4] = { 0.f, 0.f, 0.f, 1.f }; };
struct In {
    double t = 0.0; bool sample_new = true;                     // the poses' clock (s); new poses since the last tick
    HandIn hand[2]; V3 eye = v3(0.f, 1.6f, 0.f); float head_q[4] = { 0.f, 0.f, 0.f, 1.f }; uint32_t space_epoch = 0;
    bool targets_mode = false;                                  // od_gest_auron = 2 with the gestures on
    int win = odg::OD_NONE, inject = odg::OD_NONE;              // the gesture driver's window and gate (odg::FrameOut)
    bool fresh = false; int16_t prog = 0; int auron = 0; int timer = 0; float left = 0.f;   // the od block this frame
    bool game_press_ok = false; uint16_t game_press = 0;        // the new-press word 0x25D09D4 this frame
    const uint16_t* seq = nullptr; int seq_n = 0; odg::AuronMap map = odg::k_auron_map;
    bool slot_free = true;                                      // the press slot is empty (ours and the quick squeeze's)
    uint16_t cancelled = 0;                                     // the input thread cancelled our press (the value), since the last tick
    double phys_age = 1e9;                                      // s since the player last newly pressed a game button (k_cancel_mask)
    uint32_t phys_n = 0;                                        // ... how many such presses so far (a new value: a new press to settle)
    bool read_now = true;                                       // the od block was read at this Present (a fresh read of the game's state)
};
struct Hit { int i = -1, hand = 0; float speed = 0.f, deg = 0.f; double t = 0.0; bool pre = false; };
struct TickOut {
    uint16_t press = 0; int press_hand = 1, press_idx = -1; bool resent = false;   // a press for the slot now
    bool opened = false, closed = false, line = false, stopped = false, rewound = false, cancel = false;
    bool hold = false;                                          // a round is on: triggers held back, the grips do not grab, no quick squeeze
    int nh = 0; odg::Haptic hap[8];
    int nhit = 0; Hit hit[8]; int ndefl = 0; int defl[8] = {}, defl_hand[8] = {};
    void pulse(int hand, float amp, float sec) { if (nh < 8) { hap[nh].hand = hand; hap[nh].amp = amp; hap[nh].sec = sec; nh++; } }
};
struct Tgt {
    uint16_t val = 0; int sym = odv::G_NONE; int8_t lane = 0;
    bool struck = false, done = false, landed = false;          // done: the game's progress passed it (a button, or ours)
    int rung = -2;                                              // -2: placed without a slide; -1: gone
    double t_hit = -1.0; int hand = -1;
    V3 a0; double at0 = 0.0, adur = k_fly_s;                    // the motion: from a0 at at0 over adur to the spot of its rung
    double defl_t = -1e300, gone_t = -1e300;
};
struct HandTrack {
    bool have = false; Blade b; double t = 0.0; float step = 0.f; bool have_step = false; float tip_speed = 0.f; uint32_t ns = 0;   // ns: samples seen
    int pend_n = 0; int pend_i[4] = {}; float pend_deg[4] = {}; V3 pend_v[4]; double pend_t[4] = {}; float pend_step = 0.f; V3 pend_from;   // pend_from: the pose before the candidate step
    uint32_t in_contact = 0, fast_wrong = 0;                    // per target: in contact last pair; struck fast the wrong way in this pass
    bool rearm[2] = { false, false }; double rearm_t[2] = { 0.0, 0.0 }; uint32_t rearm_k[2] = { 0, 0 };
    V3 tr_root[6], tr_tip[6]; double tr_t[6] = {}; bool tr_fast[6] = {}; int tr_n = 0, tr_head = 0;
    void clear_round() { pend_n = 0; in_contact = fast_wrong = 0; rearm[0] = rearm[1] = false; }
    void trail(V3 root, V3 tip, double when, bool fast) { tr_root[tr_head] = root; tr_tip[tr_head] = tip; tr_t[tr_head] = when; tr_fast[tr_head] = fast; tr_head = (tr_head + 1) % 6; if (tr_n < 6) tr_n++; }
    int tr_at(int i) const { return (tr_head + 6 - 1 - i) % 6; }
};
struct Inject {
    int inflight = -1; uint16_t val = 0; double free_t = -1.0; bool resent = false;
    double ours_t = -1e300, prev_ours_t = -1e300; uint16_t ours_val = 0;
    int16_t last_prog = 0; bool have_prog = false;
    int free_frames = 0;                                        // fresh Presents since the slot freed after our press (the resend's settle)
    bool other_seen = false;                                    // a non-zero new-press word not ours seen since our last press (review N3)
    uint32_t phys_at = 0;                                       // the player's press count when our last press went
    bool phys_after = false;                                    // a press of the player's noticed within k_ours_s after ours, in a Present before the one
                                                                // that sees a reset (review R2-N4: one noticed with the reset may have come after the compare)
    uint32_t presses = 0, resends = 0, cancels = 0; int stop_why = 0;   // 1: a reset right after our press; 2: the game missed a press twice
    void reset() { *this = Inject{}; }
};
struct Cut {
    double t0 = -1e300; V3 c; Axes3 ax; float rad = 0.065f, th = 0.02f; V3 cd, cn; Col k; float off = 0.f;
    float hx[2][12] = {}, hy[2][12] = {}; bool hcut[2][12] = {}; int hn[2] = { 0, 0 };   // the halves in the plate's plane (centroid-relative); hcut: edge j..j+1 is the cut
    V3 hc0[2], hv[2]; float spin[2] = { 0.f, 0.f }, tumble[2] = { 0.f, 0.f };
    float spark_a[6] = {};
};
struct Stats {
    int n = 0, struck = 0, hits[2] = { 0, 0 }, pre = 0, sweeps = 0, deflects = 0, rewinds = 0, snaps = 0, jumps = 0;
    double first = -1.0, last = -1.0, done = -1.0; uint32_t presses = 0, resends = 0, cancels = 0; int end = E_NONE; bool practice = false; int stop_why = 0;
    uint32_t verts_max = 0, dropped = 0;
};
struct Round {
    bool on = false, practice = false, stopped = false; int end = E_NONE; double t_open = 0.0, t_end = -1e300;
    int n = 0; uint16_t seq[k_n_max] = {}; Tgt tg[k_n_max]; odg::AuronMap map = odg::k_auron_map;
    Frame fr; Geo g; int preview = 2; uint32_t epoch = 0; float left0 = 4.f, left_now = 4.f; int prog_shown = 0;
};

/// Splits the convex polygon (x, y; n points) by the line through o along d: side >= 0 -> half 0, <= 0 -> half 1 (both
/// include the crossing points; cut[j]: the edge j -> j+1 lies on the line).
inline int clip_half(const float* x, const float* y, int n, float ox, float oy, float dx, float dy, float sign, float* rx, float* ry, bool* cut, int cap) {
    int m = 0; bool isx[16] = {};
    for (int j = 0; j < n && m + 2 <= cap; ++j) {
        const int k = (j + 1) % n;
        const float sa = sign * (dx * (y[j] - oy) - dy * (x[j] - ox)), sb = sign * (dx * (y[k] - oy) - dy * (x[k] - ox));
        if (sa >= 0.f) { rx[m] = x[j]; ry[m] = y[j]; isx[m] = sa == 0.f; m++; }
        if ((sa > 0.f && sb < 0.f) || (sa < 0.f && sb > 0.f)) { const float t = sa / (sa - sb); rx[m] = x[j] + (x[k] - x[j]) * t; ry[m] = y[j] + (y[k] - y[j]) * t; isx[m] = true; m++; }
    }
    for (int j = 0; j < m; ++j) cut[j] = isx[j] && isx[(j + 1) % m];
    return m;
}

struct Runner {
    Cfg cfg; Round r; HandTrack ht[2]; Inject inj; Stats st, last_st; Cut cut[k_cut_n]; int cut_next = 0;
    uint32_t rounds = 0; double hap_t[2] = { -1.0, -1.0 };
    uint32_t phys_seen = 0; int phys_frames = 1000; double phys_note_t = -1e300;   // the settle in frames (review S2); when the last new press was noticed
    double gate_off_t = -1.0;                                    // the injection gate off since (review S1); -1 on
    /// The player's new presses: a new count starts the frame count (the noticing Present itself does not count).
    void note_phys(const In& in) { if (in.phys_n != phys_seen) { phys_seen = in.phys_n; phys_frames = 0; phys_note_t = in.t; } else if (in.read_now && phys_frames < 1000) phys_frames++; }
    bool settled(const In& in) const { return phys_frames >= k_settle_frames && in.phys_age >= k_settle_s; }
    char line_buf[400] = "";
    uint32_t verts_max = 0, verts_dropped = 0;
    bool gone(int i) const { return r.tg[i].struck || r.tg[i].done; }
    /// The global current target: the lowest index neither struck nor done.
    int front() const { for (int i = 0; i < r.n; ++i) if (!gone(i)) return i; return r.n; }
    /// Hittable (design 2.3, 3.3): a round on and not stopped, not struck or done, landed (the fly-in arrived), its lane's front,
    /// at most one past the current target, on the map, and with shoulder_hand an L1 / R1 only for its own hand.
    bool hittable(int i, int h) const {
        if (!r.on || r.stopped || i < 0 || i >= r.n) return false;
        const Tgt& g = r.tg[i];
        if (g.struck || g.done || !g.landed || g.rung != 0 || g.sym == odv::G_NONE) return false;
        if (i > front() + 1) return false;
        if (cfg.shoulder_hand && h >= 0 && ((g.sym == odv::G_L1 && h != 0) || (g.sym == odv::G_R1 && h != 1))) return false;
        return true;
    }
    bool active(double t) const {
        if (r.on || t - r.t_end < k_end_s) return true;
        for (const Cut& c : cut) if (t - c.t0 < k_half_s) return true;
        return false;
    }
    bool holding() const { return r.on; }
    // -- the spots and the motion ----------------------------------------------------------------------------------------
    int ndots(int lane) const { int n = 0; for (int i = 0; i < r.n; ++i) if (!gone(i) && r.tg[i].lane == lane && r.tg[i].rung > r.preview) n++; return n; }
    V3 spot(int i, int rung) const {
        const Tgt& g = r.tg[i]; const int k = rung < 0 ? 0 : rung;
        if (k <= r.preview) return slot(r.fr, r.g, g.lane, k);
        const V3 a = slot(r.fr, r.g, g.lane, r.preview), b = slot(r.fr, r.g, g.lane, r.preview + 1);   // a dot: past the last rung
        const int nd = ndots(g.lane); const float step = fminf(0.25f, 2.0f / static_cast<float>(nd > 0 ? nd : 1));
        return a + (b - a) * (0.7f + step * static_cast<float>(k - r.preview - 1));
    }
    V3 pos_rung(int i, double t, int rung) const {
        const Tgt& g = r.tg[i]; const V3 S = spot(i, rung);
        if (t <= g.at0) return g.a0;
        const float u = g.adur > 0.0 ? static_cast<float>((t - g.at0) / g.adur) : 1.f;
        return g.a0 + (S - g.a0) * ease_out(u);
    }
    V3 pos(int i, double t) const { return pos_rung(i, t, r.tg[i].rung); }
    V3 far_spot(int lane) const { return slot(r.fr, r.g, lane, r.preview + 1); }
    /// A motion's duration from a0 to S: at least base, and long enough that the ease-out's opening speed toward the eyes (3x the
    /// path's part along the line of sight at its start, over the duration) stays under k_toward_mps.
    double motion_dur(V3 a0, V3 S, double base) const {
        const V3 u = norm(a0 - r.fr.e0); const float toward = dot(a0 - S, u);
        const double need = toward > 0.f ? 3.0 * static_cast<double>(toward) / static_cast<double>(k_toward_mps) : 0.0;
        return need > base ? need : base;
    }
    /// The fly-in's start offset from spot S: k_spawn_* (beyond, above, outside), its upward part reduced so that
    /// dot(offset, S - e0) >= 0 (then the distance to the eyes only falls along the path, to the spot's).
    V3 spawn_offset(V3 S, int lane) const {
        const V3 d = S - r.fr.e0;
        const V3 base = r.fr.f * k_spawn_fwd + r.fr.r * ((lane == 0 ? -1.f : 1.f) * k_spawn_out);
        float up = k_spawn_up; const float du = dot(r.fr.u, d), db = dot(base, d);
        if (du < 0.f && db + up * du < 0.f) up = fmaxf(0.f, db / -du);
        return base + r.fr.u * up;
    }
    void update_motion(double t) {
        int cnt[2] = { 0, 0 };
        for (int i = 0; i < r.n; ++i) {
            Tgt& g = r.tg[i];
            if (gone(i)) { g.rung = -1; continue; }
            const int k = cnt[g.lane & 1]++;
            if (g.rung == -2 || g.rung == -1) g.rung = k;
            else if (k != g.rung) {
                if (t > g.at0) {   // a slide from where it is now (the hit sphere follows the drawn target)
                    const V3 p = pos_rung(i, t, g.rung); const double rest = g.at0 + g.adur - t;
                    g.a0 = p; g.adur = motion_dur(p, spot(i, k), g.landed ? k_slide_s : (rest > k_slide_s ? rest : k_slide_s)); g.at0 = t;
                }
                g.rung = k;
            }
            if (!g.landed && t >= g.at0 + g.adur) g.landed = true;
        }
    }
    // -- a round ------------------------------------------------------------------------------------------------------------
    void open(const uint16_t* seq, int n, const odg::AuronMap& m, bool practice, const In& in) {
        r = Round{}; r.on = true; r.practice = practice; r.t_open = in.t; r.n = n < 0 ? 0 : n > k_n_max ? k_n_max : n; r.map = m;
        r.g = geo(cfg); r.preview = cfg.preview < 0 ? 0 : cfg.preview > 3 ? 3 : cfg.preview; r.epoch = in.space_epoch;
        latch_now(in);
        int sym[k_n_max]; int8_t lane[k_n_max];
        for (int i = 0; i < r.n; ++i) { r.seq[i] = seq[i]; sym[i] = odv::glyph_of(m, seq[i]); }
        assign_lanes(sym, r.n, cfg.shoulder_hand, lane);
        for (int i = 0; i < r.n; ++i) { Tgt& g = r.tg[i]; g = Tgt{}; g.val = seq[i]; g.sym = sym[i]; g.lane = lane[i]; }
        update_motion(in.t);   // the rungs (placed: no slide)
        // the fly-in: fronts first, 60 ms apart, each from beyond, above and outside its spot (k_spawn_*)
        int order = 0;
        for (int k = 0; k <= k_n_max; ++k) for (int i = 0; i < r.n; ++i) {
            Tgt& g = r.tg[i]; if (g.rung != k) continue;
            const int o = order < 6 ? order : 6; order++;
            g.a0 = spot(i, k) + spawn_offset(spot(i, k), g.lane); g.at0 = in.t + k_fly_gap_s * o; g.adur = motion_dur(g.a0, spot(i, k), k_fly_s); g.landed = false;
        }
        r.left0 = practice ? static_cast<float>(k_practice_s) : (in.left > 0.2f ? in.left : 4.f); r.left_now = r.left0;
        inj.reset(); ht[0].clear_round(); ht[1].clear_round();
        st = Stats{}; st.n = r.n; st.practice = practice; rounds++;
        hap_t[0] = hap_t[1] = -1.0;
    }
    void latch_now(const In& in) {
        bool ok[2] = { in.hand[0].ok, in.hand[1].ok }; V3 hp[2] = { in.hand[0].p, in.hand[1].p };
        r.fr = latch(in.eye, in.head_q, ok, hp, cfg.body);
    }
    /// Practice targets (design 8): a round with no injection, its own 4 s bar. False while a window's round is on.
    bool start_practice(const uint16_t* seq, int n, const odg::AuronMap& m, const In& in) {
        if ((r.on && !r.practice) || n < 1) return false;
        open(seq, n, m, true, in);
        return true;
    }
    void finish(int kind, double t, TickOut& o) {
        if (!r.on) return;
        r.on = false; r.end = kind; r.t_end = t; inj.inflight = -1;
        st.end = kind; st.presses = inj.presses; st.resends = inj.resends; st.cancels = inj.cancels; st.stop_why = r.stopped ? inj.stop_why : 0;
        st.verts_max = verts_max; st.dropped = verts_dropped; verts_max = 0; verts_dropped = 0;
        last_st = st;
        format_line(line_buf, sizeof(line_buf), st);
        o.closed = true; o.line = true;
        if (kind == E_SUCCESS && cfg.haptics) { o.pulse(0, 0.5f, 0.03f); o.pulse(1, 0.5f, 0.03f); hap_t[0] = t + 0.09; hap_t[1] = t + 0.18; }
    }
    /// The game reset the progress with no press of ours just before (a wrong button of the player's): every target back.
    void rewind(double t, TickOut& o) {
        V3 cur[k_n_max]; bool again[k_n_max];   // struck / done / not yet flown: they swoop in again as in the fly-in; the rest slide from where they are
        for (int i = 0; i < r.n; ++i) { again[i] = gone(i) || t < r.tg[i].at0; cur[i] = again[i] ? v3(0.f, 0.f, 0.f) : pos(i, t); }
        for (int i = 0; i < r.n; ++i) {
            Tgt& g = r.tg[i];
            g.struck = g.done = false; g.t_hit = -1.0; g.hand = -1; g.landed = false; g.at0 = t; g.adur = k_rewind_s; g.rung = -2;
        }
        update_motion(t);
        for (int i = 0; i < r.n; ++i) {
            Tgt& g = r.tg[i]; const V3 S = spot(i, g.rung);
            g.a0 = again[i] ? S + spawn_offset(S, g.lane) : cur[i]; g.adur = motion_dur(g.a0, S, k_rewind_s);
        }
        ht[0].clear_round(); ht[1].clear_round(); inj.inflight = -1;
        st.rewinds++; r.prog_shown = 0; o.rewound = true;
    }
    // -- the injection (design 6.2; render thread, once a Present) -----------------------------------------------------------------
    void inj_observe(const In& in, TickOut& o) {
        const double t = in.t;
        if (in.cancelled && inj.inflight >= 0) { inj.inflight = -1; inj.cancels++; inj.ours_t = inj.prev_ours_t; o.cancel = true; }   // never reached the game
        if (!in.fresh) return;
        const int16_t p = in.prog;
        if (inj.have_prog && p < inj.last_prog) {   // a reset
            const bool other = inj.other_seen || inj.phys_after || (in.game_press_ok && in.game_press != 0 && in.game_press != inj.ours_val);   // a word not ours, or an earlier-noticed press of the player's, since our press
            if (t - inj.ours_t < k_ours_s && !other) { if (!r.stopped) { r.stopped = true; inj.stop_why = 1; o.stopped = true; } inj.inflight = -1; }
            else rewind(t, o);
        }
        inj.last_prog = p; inj.have_prog = true;
        if (in.game_press_ok && in.game_press != 0 && in.game_press != inj.ours_val && t >= inj.ours_t) inj.other_seen = true;
        // (after the reset test: a press noticed at this Present counts only for a reset seen at a later one)
        if (phys_seen != inj.phys_at && phys_note_t >= inj.ours_t && phys_note_t - inj.ours_t < k_ours_s) inj.phys_after = true;
        for (int i = 0; i < r.n && i < p; ++i) if (!r.tg[i].done) { r.tg[i].done = true; if (!r.tg[i].struck) r.tg[i].gone_t = t; }
        if (p >= r.n && st.done < 0.0) st.done = t - r.t_open;
        r.prog_shown = p;
        if (inj.inflight >= 0 && p > inj.inflight) inj.inflight = -1;   // the game showed it
    }
    uint16_t inj_decide(const In& in, TickOut& o) {
        if (in.inject != odg::OD_AURON) {   // the gate is off (the glue clears the slot): nothing now; a press in flight may be lost, so its resend is not a miss
            if (inj.inflight >= 0) { inj.free_t = -1.0; inj.resent = false; }
            return 0;
        }
        if (!in.fresh || r.stopped || r.practice || !(in.auron == 4 && in.timer == 2)) return 0;
        const double t = in.t; const int p = in.prog;
        if (inj.inflight >= 0) {
            if (!in.slot_free) { inj.free_t = -1.0; return 0; }
            if (inj.free_t < 0.0) { inj.free_t = t; inj.free_frames = 0; } else if (in.read_now) inj.free_frames++;
            if (t - inj.free_t < k_resend_s || inj.free_frames < k_settle_frames || !settled(in)) return 0;   // (review S2, N1)
            if (p != inj.inflight) { inj.inflight = -1; return 0; }
            if (inj.resent) { r.stopped = true; inj.stop_why = 2; o.stopped = true; inj.inflight = -1; return 0; }   // a second miss: buttons only
            const uint16_t b = r.map.button_of(inj.val); if (!b) { inj.inflight = -1; return 0; }
            inj.resent = true; inj.resends++; inj.free_t = -1.0; inj.prev_ours_t = inj.ours_t; inj.ours_t = t; inj.other_seen = false; inj.phys_at = phys_seen; inj.phys_after = false; o.resent = true;
            return b;
        }
        if (!in.slot_free || !settled(in)) return 0;
        if (p < 0 || p >= r.n || !r.tg[p].struck) return 0;
        const uint16_t b = r.map.button_of(r.seq[p]);   // ALWAYS seq[progress] by the map in use: a stroke cannot make a wrong press
        if (!b) return 0;
        inj.inflight = p; inj.val = r.seq[p]; inj.resent = false; inj.free_t = -1.0; inj.prev_ours_t = inj.ours_t; inj.ours_t = t; inj.ours_val = inj.val; inj.other_seen = false; inj.phys_at = phys_seen; inj.phys_after = false; inj.presses++;
        return b;
    }
    // -- the hands ----------------------------------------------------------------------------------------------------------------
    void apply_hit(int i, int h, const Contact& c, double t_hit, double t_now, float deg, TickOut& o) {
        Tgt& g = r.tg[i]; HandTrack& k = ht[h];
        const bool pre = i > front();
        const V3 C = pos(i, t_now); const Axes3 ax = plate_axes(r.fr, spot(i, g.rung));
        g.struck = true; g.t_hit = t_hit; g.hand = h;
        st.struck++; st.hits[h]++; if (pre) st.pre++;
        const double rel = t_hit - r.t_open; if (st.first < 0.0) st.first = rel; if (rel > st.last) st.last = rel;
        k.rearm[g.lane & 1] = true; k.rearm_t[g.lane & 1] = t_now; k.rearm_k[g.lane & 1] = k.ns;
        const uint32_t bit = 1u << i; k.in_contact &= ~bit; k.fast_wrong &= ~bit;
        V3 vp = c.v - ax.N * dot(c.v, ax.N); const float sp = len(vp);
        const V3 cd = sp > 0.3f ? vp * (1.f / sp) : norm(ax.R - ax.U);   // a stab: a diagonal cut
        add_cut(C, ax, cd, c.at, c.v, sym_col(g.sym), t_now);
        if (o.nhit < 8) { Hit& x = o.hit[o.nhit++]; x.i = i; x.hand = h; x.speed = sym_arrow(g.sym) ? sp : len(c.v); x.deg = deg; x.t = t_hit; x.pre = pre; }
    }
    /// For the tests and the injection model: hand h strikes target i now if it is hittable (a plain downward cut).
    bool strike(int i, int h, double t, TickOut& o) {
        if (!hittable(i, h) || (h >= 0 && h < 2 && ht[h].rearm[r.tg[i].lane & 1])) return false;
        Contact c; c.v = r.fr.u * -2.f; c.at = pos(i, t); apply_hit(i, h, c, t, t, 0.f, o); return true;
    }
    void deflect(int i, int h, double t, TickOut& o) {
        r.tg[i].defl_t = t; st.deflects++;
        if (o.ndefl < 8) { o.defl[o.ndefl] = i; o.defl_hand[o.ndefl] = h; o.ndefl++; }
        if (cfg.deflect_buzz && cfg.haptics) o.pulse(h, 0.25f, 0.03f);
    }
    void hand_sample(int h, const HandIn& hi, double t, TickOut& o, int* nh) {
        HandTrack& k = ht[h];
        const Geo& g = r.g;
        if (!hi.ok || !finite3(hi.p)) { k.have = false; k.pend_n = 0; k.in_contact = k.fast_wrong = 0; return; }
        const Blade b = blade_of(hi.p, hi.q, g.pitch);
        if (!k.have) { k.have = true; k.b = b; k.t = t; k.have_step = false; k.pend_n = 0; k.trail(blade_pt(b, g, g.s0), blade_pt(b, g, g.s1), t, false); return; }
        const double dt = t - k.t;
        if (!(dt > 1e-4) || dt > k_gap_s) { k.b = b; k.t = t; k.have_step = false; k.pend_n = 0; k.in_contact = k.fast_wrong = 0; k.trail(blade_pt(b, g, g.s0), blade_pt(b, g, g.s1), t, false); return; }
        const float step = len(b.p - k.b.p);
        bool snap = false;
        if (k.pend_n > 0) {   // the last pair's hits waited: a snap (rest after a big step with rest before, or a spike straight back) drops them, and this pair
            snap = step <= k_snap_rest * k.pend_step || len(b.p - k.pend_from) <= k_snap_rest * k.pend_step;
            if (snap) st.snaps++;
            else for (int j = 0; j < k.pend_n; ++j) {
                const int i = k.pend_i[j];
                if (hittable(i, h) && !k.rearm[r.tg[i].lane & 1]) { Contact c; c.v = k.pend_v[j]; c.at = pos(i, t); apply_hit(i, h, c, k.pend_t[j], t, k.pend_deg[j], o); nh[h]++; }
            }
            k.pend_n = 0;
        }
        const bool jump = step > k_jump_speed * static_cast<float>(dt);
        const bool cand = !jump && step >= k_snap_min && (!k.have_step || k.step <= k_snap_rest * step);
        k.tip_speed = len(blade_pt(b, g, g.s1) - blade_pt(k.b, g, g.s1)) / static_cast<float>(dt);
        int hits_now = 0;
        if (jump) { st.jumps++; k.in_contact = k.fast_wrong = 0; }
        else if (snap) { k.in_contact = k.fast_wrong = 0; }
        else if (r.on) {
            uint32_t cand_mask = 0; int ci[k_n_max]; int nc = 0;
            for (int i = 0; i < r.n; ++i) if (hittable(i, h) && !k.rearm[r.tg[i].lane & 1]) { ci[nc++] = i; cand_mask |= 1u << i; }
            for (int j = 0; j < nc; ++j) {
                const int i = ci[j]; const uint32_t bit = 1u << i;
                const V3 C = pos(i, t); const Axes3 ax = plate_axes(r.fr, spot(i, r.tg[i].rung));
                const Contact c = sweep(k.b, b, static_cast<float>(dt), C, g);
                if (c.dist <= g.R) {
                    k.in_contact |= bit; float deg = 0.f;
                    const int jd = judge(c, r.tg[i].sym, ax, g, &deg);
                    if (jd == J_HIT) {
                        if (cand) { if (k.pend_n < 4) { const int q = k.pend_n++; k.pend_i[q] = i; k.pend_v[q] = c.v; k.pend_deg[q] = deg; k.pend_t[q] = k.t + dt * c.u; } k.pend_step = step; k.pend_from = k.b.p; }
                        else { apply_hit(i, h, c, k.t + dt * c.u, t, deg, o); hits_now++; }
                    }
                    else if (jd == J_WRONG) k.fast_wrong |= bit;
                }
                else if (k.in_contact & bit) { if (k.fast_wrong & bit) deflect(i, h, t, o); k.in_contact &= ~bit; k.fast_wrong &= ~bit; }
            }
            k.in_contact &= cand_mask; k.fast_wrong &= cand_mask;
        }
        if (hits_now >= 2) st.sweeps++;
        nh[h] += hits_now;
        k.ns++;   // the re-arm's out test starts at the sample after the hit's (the blade out of the spot for one sample)
        for (int L = 0; L < 2; ++L)
            if (k.rearm[L] && (t - k.rearm_t[L] >= k_rearm_s || (k.ns > k.rearm_k[L] + 1 && blade_dist(b, g, slot(r.fr, g, L, 0)) > g.R + k_rearm_m))) k.rearm[L] = false;
        k.step = step; k.have_step = true; k.b = b; k.t = t;
        k.trail(blade_pt(b, g, g.s0), blade_pt(b, g, g.s1), t, k.tip_speed > g.speed);
    }
    // -- the tick -------------------------------------------------------------------------------------------------------------------
    void tick(const In& in, TickOut& o) {
        o = TickOut{};
        const double t = in.t;
        note_phys(in);
        // the round lives with the driver's window (review S1): a gate drop (the VR pad's sync, the headset's focus) only stops
        // the presses; past k_gate_grace_s it ends the round, and a new one opens when the gate is back in the same window
        const bool win_live = in.targets_mode && in.win == odg::OD_AURON && in.seq != nullptr && in.seq_n >= 1;
        const bool gate = in.inject == odg::OD_AURON;
        if (gate) gate_off_t = -1.0; else if (gate_off_t < 0.0) gate_off_t = t;
        if (win_live) {
            const int n = in.seq_n > k_n_max ? k_n_max : in.seq_n;
            const bool same = r.on && !r.practice && r.n == n && std::memcmp(r.seq, in.seq, static_cast<size_t>(n) * 2u) == 0;
            if (r.on && r.practice) finish(E_ABORTED, t, o);
            if (r.on && !same) finish(E_CLOSED, t, o);
            if (r.on && !gate && t - gate_off_t >= k_gate_grace_s) finish(E_CLOSED, t, o);
            if (!r.on && gate) { open(in.seq, n, in.map, false, in); o.opened = true; }
        }
        else if (r.on && r.practice && in.win != odg::OD_NONE) finish(E_ABORTED, t, o);   // any overdrive window ends practice (its own gestures, or the buttons)
        else if (r.on && !r.practice) {
            if (in.fresh) r.prog_shown = in.prog;
            finish(in.auron == 5 ? E_SUCCESS : in.auron == 7 ? E_TIMEOUT : E_CLOSED, t, o);
        }
        if (r.on && in.space_epoch != r.epoch) { r.epoch = in.space_epoch; latch_now(in); }   // a recentre: the room moved
        r.g.pitch = geo(cfg).pitch;   // the pitch slider is live (practice is where it is set)
        if (r.on && !r.practice) inj_observe(in, o);
        if (r.on) update_motion(t);
        if (in.sample_new) {
            int nh[2] = { 0, 0 };
            for (int h = 0; h < 2; ++h) hand_sample(h, in.hand[h], t, o, nh);
            if (cfg.haptics) for (int h = 0; h < 2; ++h) { if (nh[h] >= 2) o.pulse(h, 0.9f, 0.025f); else if (nh[h] == 1) o.pulse(h, 0.7f, 0.015f); }
        }
        if (r.on) update_motion(t);
        if (r.on && !r.practice) {
            r.left_now = in.left;
            const uint16_t b = inj_decide(in, o);
            if (b) { o.press = b; o.press_idx = inj.inflight; const int hh = inj.inflight >= 0 ? r.tg[inj.inflight].hand : 1; o.press_hand = hh < 0 ? 1 : hh; }
        }
        if (r.on && r.practice) {
            int s = 0; for (int i = 0; i < r.n; ++i) if (r.tg[i].struck) s++;
            r.prog_shown = s; r.left_now = static_cast<float>(k_practice_s - (t - r.t_open));
            if (s == r.n) finish(E_SUCCESS, t, o);
            else if (t - r.t_open >= k_practice_s) finish(E_TIMEOUT, t, o);
        }
        for (int j = 0; j < 2; ++j) if (hap_t[j] > 0.0 && t >= hap_t[j]) { o.pulse(0, 0.5f, 0.03f); o.pulse(1, 0.5f, 0.03f); hap_t[j] = -1.0; }
        o.hold = r.on;
    }
    // -- the lines -------------------------------------------------------------------------------------------------------------------
    static int format_line(char* s, size_t n, const Stats& x) {
        if (n == 0) return 0;
        s[0] = 0; int k = 0;
        if (x.practice) {
            if (x.end == E_ABORTED) return odg::fmt_append(s, n, k, "practice: interrupted (an overdrive window opened) after %d of %d", x.struck, x.n);
            const double total = x.end == E_SUCCESS ? x.last : k_practice_s;
            k = odg::fmt_append(s, n, k, "practice: %d of %d in %.2f s%s", x.struck, x.n, total, x.end == E_SUCCESS ? "" : ", time out");
            if (x.struck >= 1) {
                k = odg::fmt_append(s, n, k, " (first %.2f s", x.first);
                if (x.struck >= 2) k = odg::fmt_append(s, n, k, ", then %.2f s an input", (x.last - x.first) / (x.struck - 1));
                k = odg::fmt_append(s, n, k, "; deflects %d)", x.deflects);
            }
            else k = odg::fmt_append(s, n, k, " (no strike; deflects %d)", x.deflects);
            return k;
        }
        k = odg::fmt_append(s, n, k, "Auron targets: %d symbols, struck %d (left blade %d, right %d; pre-hits %d, sweeps hitting two %d)", x.n, x.struck, x.hits[0], x.hits[1], x.pre, x.sweeps);
        if (x.struck) k = odg::fmt_append(s, n, k, ", first strike %.2f s, last %.2f s", x.first, x.last);
        if (x.done >= 0.0) k = odg::fmt_append(s, n, k, ", game done %.2f s", x.done);
        k = odg::fmt_append(s, n, k, "; presses %u (resends %u, cancelled by a button %u), deflects %d, rewinds %d, snaps dropped %d, jumps skipped %d",
                            x.presses, x.resends, x.cancels, x.deflects, x.rewinds, x.snaps, x.jumps);
        if (x.stop_why) k = odg::fmt_append(s, n, k, "; STOPPED injecting (%s): buttons only", x.stop_why == 1 ? "a reset right after our press: the map or a racing button" : "the game missed a press twice");
        k = odg::fmt_append(s, n, k, "; verts max %u, dropped %u; closed: %s", x.verts_max, x.dropped, k_end_name[x.end < 0 || x.end >= E_N ? 0 : x.end]);
        return k;
    }
    // -- the effects ------------------------------------------------------------------------------------------------------------------
    /// A cut: the plate splits along the stroke (in its plane) through the blade's crossing (clamped near the centre); the halves
    /// part perpendicular to the cut at 0.8 m/s with a little of the blade's velocity, fall, spin and fade (450 ms); never toward
    /// the eye: the velocity's part toward e0 is removed, the rise is capped at eye height and the cut knocks both halves 0.3 m/s
    /// back from e0 (a half starting above eye level would otherwise come a hair nearer as it falls back through it), so with
    /// gravity the distance to e0 only grows.
    void add_cut(V3 C, const Axes3& ax, V3 cd, V3 at, V3 bv, Col k, double t) {
        Cut& c = cut[cut_next]; cut_next = (cut_next + 1) % k_cut_n;
        c = Cut{}; c.t0 = t; c.c = C; c.ax = ax; c.rad = r.g.rad; c.th = r.g.th; c.k = k;
        c.cd = norm(cd); c.cn = cross(ax.N, c.cd);
        c.off = clampf(dot(at - C, c.cn), -0.35f * c.rad, 0.35f * c.rad);
        float ox[8], oy[8];
        for (int j = 0; j < 8; ++j) { const float a = (22.5f + 45.f * static_cast<float>(j)) * k_deg; ox[j] = c.rad * std::cos(a); oy[j] = c.rad * std::sin(a); }
        const float dx = dot(c.cd, ax.R), dy = dot(c.cd, ax.U), nx = -dy, ny = dx;   // the cut's direction and normal in the plane (x, y)
        for (int s = 0; s < 2; ++s) {
            float hx[12], hy[12]; bool hc[12];
            const int m = clip_half(ox, oy, 8, nx * c.off, ny * c.off, dx, dy, s == 0 ? 1.f : -1.f, hx, hy, hc, 12);
            float cx = 0.f, cy = 0.f; for (int j = 0; j < m; ++j) { cx += hx[j]; cy += hy[j]; }
            if (m > 0) { cx /= static_cast<float>(m); cy /= static_cast<float>(m); }
            c.hn[s] = m; for (int j = 0; j < m; ++j) { c.hx[s][j] = hx[j] - cx; c.hy[s][j] = hy[j] - cy; c.hcut[s][j] = hc[j]; }
            c.hc0[s] = C + ax.R * cx + ax.U * cy;
            const V3 nd = c.cn * (s == 0 ? 1.f : -1.f);
            V3 v = nd * 0.8f + bv * 0.15f;
            const float vn = dot(v, nd); if (vn < 0.8f) v = v + nd * (0.8f - vn);   // the blade adds to the parting, never takes from it
            const V3 tow = norm(r.fr.e0 - c.hc0[s]);
            const float tv = dot(v, tow); if (tv > 0.f) v = v - tow * tv;
            v = v - tow * 0.3f;   // knocked back
            const float rise = r.fr.e0.y - c.hc0[s].y;
            const float vmax = rise > 0.f ? std::sqrt(2.f * k_g * rise) : 0.f;
            if (v.y > vmax) v.y = vmax;
            c.hv[s] = v; c.spin[s] = s == 0 ? 3.f : -3.5f; c.tumble[s] = s == 0 ? 2.5f : -2.f;
        }
        for (int j = 0; j < 6; ++j) c.spark_a[j] = (-60.f + 24.f * static_cast<float>(j) + static_cast<float>((rounds * 7 + static_cast<uint32_t>(cut_next) * 13 + static_cast<uint32_t>(j) * 5) % 9) - 4.f) * k_deg;
    }
    /// A half's centroid at age a (s): hc0 + v a + gravity.
    static V3 half_c(const Cut& c, int s, float a) { return c.hc0[s] + c.hv[s] * a + v3(0.f, -0.5f * k_g * a * a, 0.f); }
    /// A half's vertex j (front face when side > 0, back when < 0) at age a.
    static V3 half_v(const Cut& c, int s, int j, float a, float side) {
        V3 R = c.ax.R, U = c.ax.U, N = c.ax.N;
        const float sp = c.spin[s] * a, tu = c.tumble[s] * a;
        R = rot_axis(R, c.ax.N, sp); U = rot_axis(U, c.ax.N, sp);
        R = rot_axis(R, c.cd, tu); U = rot_axis(U, c.cd, tu); N = rot_axis(N, c.cd, tu);
        return half_c(c, s, a) + R * c.hx[s][j] + U * c.hy[s][j] + N * (0.5f * c.th * side);
    }
    // -- drawing ------------------------------------------------------------------------------------------------------------------------
    static void stroke(Out& o, V3 c, V3 R, V3 U, float x0, float y0, float x1, float y1, float w, Col k) {
        const float dx = x1 - x0, dy = y1 - y0, l = std::sqrt(dx * dx + dy * dy); if (l < 1e-6f) return;
        const float ux = dx / l * 0.5f * w, uy = dy / l * 0.5f * w;   // square caps: half a stroke past each end (joints fill)
        const float ax = x0 - ux, ay = y0 - uy, bx = x1 + ux, by = y1 + uy, px = -uy, py = ux;
        o.quad(c + R * (ax + px) + U * (ay + py), k, c + R * (bx + px) + U * (by + py), k, c + R * (bx - px) + U * (by - py), k, c + R * (ax - px) + U * (ay - py), k);
    }
    /// The engraved symbol (units of the plate's radius s): a glow under a bright core.
    static void symbol(Out& o, int sym, V3 c, V3 R, V3 U, float s, Col k) {
        static const float A[][4] = { { -0.36f, -0.46f, 0.f, 0.46f }, { 0.f, 0.46f, 0.36f, -0.46f }, { -0.19f, -0.07f, 0.19f, -0.07f } };
        static const float X[][4] = { { -0.34f, -0.46f, 0.34f, 0.46f }, { -0.34f, 0.46f, 0.34f, -0.46f } };
        static const float Y[][4] = { { -0.34f, 0.46f, 0.f, 0.f }, { 0.34f, 0.46f, 0.f, 0.f }, { 0.f, 0.f, 0.f, -0.46f } };
        static const float B[][4] = { { -0.30f, -0.46f, -0.30f, 0.46f }, { -0.30f, 0.46f, 0.12f, 0.46f }, { 0.12f, 0.46f, 0.28f, 0.31f }, { 0.28f, 0.31f, 0.28f, 0.15f },
                                      { 0.28f, 0.15f, 0.12f, 0.f }, { -0.30f, 0.f, 0.16f, 0.f }, { 0.16f, 0.f, 0.33f, -0.15f }, { 0.33f, -0.15f, 0.33f, -0.31f },
                                      { 0.33f, -0.31f, 0.16f, -0.46f }, { 0.16f, -0.46f, -0.30f, -0.46f } };
        static const float L[][4] = { { -0.22f, 0.46f, -0.22f, -0.46f }, { -0.22f, -0.46f, 0.28f, -0.46f } };
        static const float Rr[][4] = { { -0.26f, -0.46f, -0.26f, 0.46f }, { -0.26f, 0.46f, 0.10f, 0.46f }, { 0.10f, 0.46f, 0.26f, 0.31f }, { 0.26f, 0.31f, 0.26f, 0.15f },
                                       { 0.26f, 0.15f, 0.10f, 0.f }, { 0.10f, 0.f, -0.26f, 0.f }, { 0.f, 0.f, 0.30f, -0.46f } };
        const Col glow = col(k.r, k.g, k.b, 0.22f * k.a);
        auto strokes = [&](const float (*t)[4], int n, float sc, float ox, float oy, float w) {
            for (int pass = 0; pass < 2; ++pass)
                for (int i = 0; i < n; ++i) stroke(o, c, R, U, (ox + t[i][0] * sc) * s, (oy + t[i][1] * sc) * s, (ox + t[i][2] * sc) * s, (oy + t[i][3] * sc) * s, (pass ? w : 2.4f * w) * s, pass ? k : glow);
        };
        if (sym_arrow(sym)) {
            V3 a = U, b = R;
            if (sym == odv::G_RIGHT) { a = R; b = U * -1.f; } else if (sym == odv::G_DOWN) { a = U * -1.f; b = R * -1.f; } else if (sym == odv::G_LEFT) { a = R * -1.f; b = U; }
            for (int pass = 0; pass < 2; ++pass) {
                const float g = pass ? 1.f : 1.35f; const Col cc = pass ? k : glow;
                o.tri(c + a * (0.62f * s * g), cc, c + a * (0.05f * s) - b * (0.42f * s * g), cc, c + a * (0.05f * s) + b * (0.42f * s * g), cc);
                const float hw = 0.13f * s * (pass ? 1.f : 2.f);
                o.quad(c + a * (0.08f * s) - b * hw, cc, c + a * (0.08f * s) + b * hw, cc, c - a * (0.55f * s) + b * hw, cc, c - a * (0.55f * s) - b * hw, cc);
            }
            return;
        }
        switch (sym) {
            case odv::G_A: strokes(A, 3, 1.f, 0.f, 0.f, 0.17f); return;
            case odv::G_X: strokes(X, 2, 1.f, 0.f, 0.f, 0.17f); return;
            case odv::G_Y: strokes(Y, 3, 1.f, 0.f, 0.f, 0.17f); return;
            case odv::G_B: strokes(B, 10, 1.f, 0.f, 0.f, 0.15f); return;
            case odv::G_L1: case odv::G_R1: {   // a wide tab with a notch on its side and the letter in it
                const bool lf = sym == odv::G_L1;
                odv::rect(o, c, R, U, -0.62f * s, -0.30f * s, 0.62f * s, 0.30f * s, col(k.r, k.g, k.b, 0.28f * k.a));
                const float tab[][4] = { { -0.62f, -0.30f, 0.62f, -0.30f }, { 0.62f, -0.30f, 0.62f, 0.30f }, { 0.62f, 0.30f, -0.62f, 0.30f }, { -0.62f, 0.30f, -0.62f, -0.30f } };
                for (int i = 0; i < 4; ++i) stroke(o, c, R, U, tab[i][0] * s, tab[i][1] * s, tab[i][2] * s, tab[i][3] * s, 0.08f * s, k);
                odv::rect(o, c, R, U, (lf ? -0.76f : 0.62f) * s, -0.10f * s, (lf ? -0.62f : 0.76f) * s, 0.10f * s, k);
                if (lf) strokes(L, 2, 0.5f, 0.f, 0.f, 0.11f); else strokes(Rr, 7, 0.5f, 0.f, 0.f, 0.11f);
                return;
            }
            default: odv::arc(o, c, R, U, 0.22f * s, 0.10f * s, 0.f, odg::k_two_pi, k, 8); return;   // off the map: a ring ("use the button")
        }
    }
    /// The seal plate: a short octagonal prism (sides, a bevel lit from above, a darker face), front toward ax.N.
    static void plate(Out& o, V3 c, const Axes3& ax, float rad, float th, Col tint, float rim, float alpha, bool grey) {
        const Col base = grey ? col(0.35f, 0.35f, 0.37f, 1.f) : tint;
        V3 dir[8];
        for (int j = 0; j < 8; ++j) { const float a = (22.5f + 45.f * static_cast<float>(j)) * k_deg; dir[j] = ax.R * std::cos(a) + ax.U * std::sin(a); }
        const V3 F = ax.N * (0.5f * th), Bk = ax.N * (-0.5f * th), Fo = ax.N * (0.5f * th - 0.25f * th);
        const float ri = 0.84f * rad;
        for (int j = 0; j < 8; ++j) {   // the sides (behind the front: painted first)
            const int k = (j + 1) % 8; const V3 en = norm(dir[j] + dir[k]); const float lit = 0.35f + 0.25f * dot(en, ax.U);
            const Col sc = col(0.10f + base.r * 0.12f * lit, 0.11f + base.g * 0.12f * lit, 0.14f + base.b * 0.12f * lit, 0.95f * alpha);
            o.quad(c + Bk + dir[j] * rad, sc, c + Bk + dir[k] * rad, sc, c + Fo + dir[k] * rad, sc, c + Fo + dir[j] * rad, sc);
        }
        for (int j = 0; j < 8; ++j) {   // the bevel
            const int k = (j + 1) % 8; const V3 en = norm(dir[j] + dir[k]); const float lit = clampf(0.55f + 0.45f * dot(en, ax.U * 0.95f + ax.R * 0.3f), 0.15f, 1.f) * rim;
            const Col bc = col(0.18f + base.r * 0.62f * lit, 0.18f + base.g * 0.62f * lit, 0.20f + base.b * 0.62f * lit, 0.95f * alpha);
            o.quad(c + Fo + dir[j] * rad, bc, c + Fo + dir[k] * rad, bc, c + F + dir[k] * ri, bc, c + F + dir[j] * ri, bc);
        }
        const Col fi = col(0.07f + base.r * 0.06f, 0.08f + base.g * 0.06f, 0.10f + base.b * 0.06f, 0.92f * alpha), fe = col(0.04f, 0.045f, 0.06f, 0.92f * alpha);
        for (int j = 0; j < 8; ++j) { const int k = (j + 1) % 8; o.tri(c + F, fi, c + F + dir[j] * ri, fe, c + F + dir[k] * ri, fe); }
    }
    void draw_target(int i, double t, float A, Out& o) const {
        const Tgt& g = r.tg[i];
        if (g.struck || t < g.at0) return;
        float fade = A, sc = 1.f;
        if (g.done) { const double a = t - g.gone_t; if (a >= 0.15 || a < 0.0) return; fade *= static_cast<float>(1.0 - a / 0.15); sc *= static_cast<float>(1.0 - 0.4 * a / 0.15); }
        const int k = g.rung < 0 ? 0 : g.rung;
        const V3 C = pos(i, t);
        const bool grey = r.stopped || (!r.on && r.end == E_TIMEOUT);
        if (k > r.preview && !g.done) {   // a dot: the count of what is behind
            const Axes3 ax = plate_axes(r.fr, C);
            odv::rect(o, C, ax.R, ax.U, -0.005f * r.g.size, -0.005f * r.g.size, 0.005f * r.g.size, 0.005f * r.g.size, grey ? col(0.5f, 0.5f, 0.5f, 0.5f * fade) : col(sym_col(g.sym).r, sym_col(g.sym).g, sym_col(g.sym).b, 0.6f * fade));
            return;
        }
        sc *= rung_scale(k); fade *= k > 0 ? rung_scale(k) : 1.f;
        Axes3 ax = plate_axes(r.fr, spot(i, k));
        const bool cur = r.on && i == front();
        const float ph = static_cast<float>(std::sin((t - r.t_open) * odg::k_two_pi * 2.5));
        if (cur) sc *= 1.03f + 0.03f * ph;
        float rim = cur ? 0.85f + 0.15f * ph : (k == 0 ? 0.8f : 0.6f);
        const double da = t - g.defl_t;
        if (da >= 0.0 && da < 0.12) {   // a deflect: tilts 8 degrees and back, the rim flashes
            const float tilt = 8.f * k_deg * static_cast<float>(std::sin(3.14159265 * da / 0.12));
            ax.R = rot_axis(ax.R, ax.N, tilt); ax.U = rot_axis(ax.U, ax.N, tilt); rim = 1.6f;
        }
        const Col tint = sym_col(g.sym);
        plate(o, C, ax, r.g.rad * sc, r.g.th * sc, tint, rim, fade, grey);
        const Col sk = grey ? col(0.6f, 0.6f, 0.62f, 0.8f * fade) : col(tint.r, tint.g, tint.b, fade);
        symbol(o, g.sym, C + ax.N * (0.5f * r.g.th * sc + 0.0005f), ax.R, ax.U, r.g.rad * sc, sk);
    }
    void draw_fx(const In& in, Out& o) const {
        const double t = in.t;
        for (const Cut& c : cut) {
            const double a = t - c.t0; if (!(a >= 0.0 && a < k_half_s)) continue;
            const float af = static_cast<float>(a), k = 1.f - static_cast<float>(a / k_half_s);
            for (int s = 0; s < 2; ++s) {
                const int m = c.hn[s]; if (m < 3) continue;
                const Col side = col(0.12f, 0.13f, 0.16f, 0.9f * k), hot = col(0.6f + 0.4f * c.k.r, 0.6f + 0.4f * c.k.g, 0.6f + 0.4f * c.k.b, k);
                for (int j = 0; j < m; ++j) {
                    const int q = (j + 1) % m; const Col e = c.hcut[s][j] ? hot : side;
                    o.quad(half_v(c, s, j, af, -1.f), e, half_v(c, s, q, af, -1.f), e, half_v(c, s, q, af, 1.f), e, half_v(c, s, j, af, 1.f), e);
                }
                const V3 fc = half_c(c, s, af); const V3 N = rot_axis(c.ax.N, c.cd, c.tumble[s] * af);
                const Col fi = col(0.10f + 0.3f * c.k.r, 0.11f + 0.3f * c.k.g, 0.13f + 0.3f * c.k.b, 0.9f * k), fe = col(0.05f, 0.05f, 0.07f, 0.9f * k);
                for (int j = 0; j < m; ++j) { const int q = (j + 1) % m; o.tri(fc + N * (0.5f * c.th), fi, half_v(c, s, j, af, 1.f), fe, half_v(c, s, q, af, 1.f), fe); }
            }
            if (a < k_spark_s) {
                const float u = static_cast<float>(a / k_spark_s);
                for (int j = 0; j < 6; ++j) {
                    const V3 d = rot_axis(c.cd, c.ax.N, c.spark_a[j]);
                    const V3 p = c.c + c.cn * c.off + d * (1.2f * af + 0.5f * c.rad);
                    const V3 perp = norm(cross(d, p - in.eye)) * 0.0013f;
                    const Col sc = col(1.f, 0.85f + 0.15f * c.k.g, 0.6f + 0.4f * c.k.b, 1.f - u);
                    o.quad(p - d * 0.012f - perp, sc, p + d * 0.012f - perp, sc, p + d * 0.012f + perp, sc, p - d * 0.012f + perp, sc);
                }
            }
            if (a < k_line_s) {
                const Col lc = col(1.f, 1.f, 1.f, 1.f - static_cast<float>(a / k_line_s));
                const V3 m = c.c + c.cn * c.off + c.ax.N * (0.5f * c.th + 0.001f);
                odv::glow_line(o, m - c.cd * (1.25f * c.rad), m + c.cd * (1.25f * c.rad), 0.0025f, lc, lc, in.eye);
            }
        }
    }
    void draw_hud(const In& in, float A, Out& o) const {
        const double t = in.t; const float S = r.g.size;
        const V3 CR = r.fr.e0 + r.fr.f * r.g.D + r.fr.u * r.g.H;
        const Axes3 ax = plate_axes(r.fr, CR);
        // the progress pips: the game's progress (the truth), practice: the strikes
        const int n = r.n; const float sp = fminf(0.022f, 0.30f / static_cast<float>(n > 0 ? n : 1)) * S, w = 0.68f * sp;
        const double sa = r.end == E_SUCCESS ? t - r.t_end : 1e9; const bool flash = sa >= 0.0 && sa < 0.25;
        for (int i = 0; i < n; ++i) {
            const V3 c = CR + ax.U * (-0.09f * S) + ax.R * ((static_cast<float>(i) - 0.5f * static_cast<float>(n - 1)) * sp);
            const bool lit = i < r.prog_shown || flash;
            const Col k = flash ? col(1.f, 1.f, 1.f, A) : lit ? col(0.85f, 0.97f, 1.f, 0.95f * A) : col(0.5f, 0.55f, 0.6f, 0.35f * A);
            odv::rect(o, c, ax.R, ax.U, -0.5f * w, -0.5f * w, 0.5f * w, 0.5f * w, k);
        }
        // the timer bar: shrinking toward its centre
        const float left = r.on ? r.left_now : (r.end == E_SUCCESS ? r.left_now : 0.f);
        const float frac = clampf(r.left0 > 0.f ? left / r.left0 : 0.f, 0.f, 1.f), W = 0.40f * S, H = 0.01f * S;
        const V3 bc = CR + ax.U * (-0.12f * S);
        odv::rect(o, bc, ax.R, ax.U, -0.5f * W, -0.5f * H, 0.5f * W, 0.5f * H, col(0.1f, 0.1f, 0.12f, 0.35f * A));
        const Col bk = left < 0.75f ? col(1.f, 0.32f, 0.25f, 0.95f * A) : left < 1.5f ? col(1.f, 0.72f, 0.22f, 0.95f * A) : col(0.95f, 0.95f, 0.95f, 0.9f * A);
        if (frac > 0.f) odv::rect(o, bc, ax.R, ax.U, -0.5f * W * frac, -0.5f * H, 0.5f * W * frac, 0.5f * H, bk);
        if (flash) {   // success: a ring expands from the strike row
            const float u = static_cast<float>(sa / 0.25);
            odv::arc(o, CR, ax.R, ax.U, (0.05f + 0.40f * u) * S, 0.012f * S, 0.f, odg::k_two_pi, col(1.f, 0.95f, 0.8f, (1.f - u) * 0.9f), 24);
        }
    }
    /// A blade: a single-edged, slightly curved, tapered blade from 6 cm beyond the grip, a small round guard, gold (left) or
    /// steel-cyan (right); brighter above the speed threshold; a ribbon trail of the last 6 samples.
    void draw_blade(int h, const In& in, float A, Out& o) const {
        const HandIn& hi = in.hand[h]; if (!hi.ok || !finite3(hi.p) || r.g.fist) return;
        const Geo& g = r.g; const Blade b = blade_of(hi.p, hi.q, g.pitch);
        const HandTrack& k = ht[h]; const bool fast = k.tip_speed > g.speed;
        const Col tint = h == 0 ? col(1.f, 0.78f, 0.36f, 1.f) : col(0.64f, 0.88f, 0.96f, 1.f);
        // the trail (oldest first, under the blade)
        for (int i = k.tr_n - 2; i >= 0; --i) {
            const int a = k.tr_at(i), c = k.tr_at(i + 1);
            const double age = in.t - k.tr_t[a]; if (age > 0.15 || age < -0.05) continue;
            const float al = A * (1.f - static_cast<float>(i) / 6.f) * (k.tr_fast[a] ? 0.32f : 0.10f) * static_cast<float>(1.0 - (age > 0.0 ? age : 0.0) / 0.15);
            const V3 ma = (k.tr_root[a] + k.tr_tip[a]) * 0.5f, mc = (k.tr_root[c] + k.tr_tip[c]) * 0.5f;
            const Col t0 = col(tint.r, tint.g, tint.b, 0.25f * al), t1 = col(tint.r, tint.g, tint.b, al);
            o.quad(k.tr_root[a], t0, k.tr_root[c], t0, mc, t0, ma, t0);
            o.quad(ma, t1, mc, t1, k.tr_tip[c], t1, k.tr_tip[a], t1);
        }
        const V3 side = norm(cross(b.d, b.u));
        const float gl = fast ? 0.9f : 0.5f;
        const int NS = 6;
        auto width = [&](float w01) { return w01 < 0.82f ? 0.024f + (0.013f - 0.024f) * (w01 / 0.82f) : 0.013f * (1.f - (w01 - 0.82f) / 0.18f); };
        for (int j = 0; j < NS; ++j) {
            const float wa = static_cast<float>(j) / NS, wb = static_cast<float>(j + 1) / NS;
            const float sa = g.s0 + (g.s1 - g.s0) * wa, sb = g.s0 + (g.s1 - g.s0) * wb;
            const V3 ca = blade_pt(b, g, sa), cb = blade_pt(b, g, sb);
            const float wa_ = width(wa) * g.size, wb_ = width(wb) * g.size;
            // the glow, facing the eye (keeps the blade visible edge-on)
            const V3 perp = norm(cross(cb - ca, (ca + cb) * 0.5f - in.eye));
            const float gw = (fast ? 0.013f : 0.008f) * g.size;
            const Col gc = col(tint.r, tint.g, tint.b, (fast ? 0.30f : 0.12f) * A), ge = col(tint.r, tint.g, tint.b, 0.f);
            o.quad(ca, gc, cb, gc, cb + perp * gw, ge, ca + perp * gw, ge);
            o.quad(ca - perp * gw, ge, cb - perp * gw, ge, cb, gc, ca, gc);
            // the flat: spine (u side) to edge
            const Col fc = col(0.25f + 0.55f * tint.r, 0.25f + 0.55f * tint.g, 0.25f + 0.55f * tint.b, gl * A);
            const V3 Sa = ca + b.u * (0.4f * wa_), Sb = cb + b.u * (0.4f * wb_), Ea = ca - b.u * (0.6f * wa_), Eb = cb - b.u * (0.6f * wb_);
            o.quad(Sa, fc, Sb, fc, Eb, fc, Ea, fc);
            // the edge's bright line and the spine's darker strip
            const Col ec = col(0.75f + 0.25f * tint.r, 0.75f + 0.25f * tint.g, 0.75f + 0.25f * tint.b, (fast ? 1.f : 0.75f) * A);
            o.quad(Ea, ec, Eb, ec, Eb + b.u * (0.18f * wb_), ec, Ea + b.u * (0.18f * wa_), ec);
            const Col pc = col(0.5f * tint.r, 0.5f * tint.g, 0.5f * tint.b, 0.7f * A); const float sw = 0.0025f * g.size;
            o.quad(Sa - side * sw, pc, Sb - side * sw, pc, Sb + side * sw, pc, Sa + side * sw, pc);
        }
        // the guard: a small round disc across the blade at its root
        const V3 gc = b.p + b.d * (g.s0 - 0.012f);
        odv::disc(o, gc, b.u, side, 0.022f * g.size, col(0.30f + 0.2f * tint.r, 0.28f + 0.2f * tint.g, 0.26f + 0.2f * tint.b, 0.95f * A), col(0.55f * tint.r, 0.55f * tint.g, 0.55f * tint.b, 0.95f * A), 8);
    }
    /// This Present's triangles (the XR space of the poses; in: t, eye, head, hands). Blades and trails last, with their reserve.
    int build(const In& in, Out& o) {
        const int n0 = o.n; const double t = in.t;
        const bool show = r.on || (r.end != E_NONE && t - r.t_end < k_end_s && t >= r.t_end);
        const float A = r.on ? 1.f : static_cast<float>(1.0 - (t - r.t_end) / k_end_s);
        const int keep = o.max; const int room = o.max - o.n;
        if (room > 2 * k_blade_reserve) o.max = keep - k_blade_reserve;
        const int d0 = o.dropped;
        if (show) {
            int maxr = 0; for (int i = 0; i < r.n; ++i) if (r.tg[i].rung > maxr) maxr = r.tg[i].rung;
            for (int k = maxr; k >= 0; --k) for (int i = 0; i < r.n; ++i) if ((r.tg[i].rung < 0 ? 0 : r.tg[i].rung) == k) draw_target(i, t, A, o);
        }
        draw_fx(in, o);
        if (show) draw_hud(in, A, o);
        o.max = keep;
        if (show) for (int h = 0; h < 2; ++h) draw_blade(h, in, A, o);
        const V3 rr = qrot(in.head_q, v3(1.f, 0.f, 0.f)) * odv::k_half_ipd;
        odv::near_eye_fade(o.v, n0, o.n, in.eye - rr, in.eye + rr);
        const int nv = o.n - n0;
        if (static_cast<uint32_t>(nv) > verts_max) verts_max = static_cast<uint32_t>(nv);
        verts_dropped += static_cast<uint32_t>(o.dropped - d0);
        return nv;
    }
};

}  // namespace odt
