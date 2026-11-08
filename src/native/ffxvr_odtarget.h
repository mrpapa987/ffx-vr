// SPDX-License-Identifier: MIT
// ffxvr_odtarget.h -- Auron's Bushido, "strike the targets" (od_gest_auron = 2; 7 Nov 2026), and F8's Practice targets. Pure:
// no Windows, no D3D, no logging, no allocation. ffxvr_native.cpp ticks the Runner once a Present on the render thread
// (od_gest_tick, right after the gesture driver), builds its triangles once a Present in the XR space of the controller poses
// and appends them to the 3D marker pass with the VR hands' map (as the overdrive visuals, ffxvr_odvis.h); the input thread's
// arbitration (Arb) and trigger hold-back (trig_hold) run in odg_input / vrpad_triggers. A unit test drives it with
// synthetic blade strokes and a transcription of the game's Bushido input state with the press slot's poll model.
//
//   geo / latch / spot  where the targets are (the owner's redesign "one big target", 8 Nov 2026, then "two places", 9 Nov):
//                       ONE plate at a time, 24 cm across, at chest height (0.50 m forward of the eyes, 0.30 m below them) in
//                       one of two spots 0.36 m apart (od_targets_spread): the LEFT spot holds the d-pad arrows, the RIGHT spot
//                       A / B / X / Y and L1 / R1; latched once a round from the head's yaw (and the hands) and fixed in the room
//                       (a recentre re-latches); the next symbol small and faint above its own spot; thin progress pips and a
//                       timer bar under and between the spots
//   sweep / judge       the swept blade (sub-stepped by rotation, 8 points along a slightly curved blade) against the target's
//                       15 cm hit sphere; a hit needs 1.2 m/s at the contact and the pass's in-plane path (from its last turn
//                       back) within 65 degrees of the symbol's cut direction: the arrows theirs; the face buttons as on the
//                       Xbox pad (A down, B right, X left, Y up); L1 left, R1 right
//   Runner              a round (a Bushido window or a practice round): only the current target can be cut (no pre-hits); when
//                       it is, the next materialises in its spot (0.12 s); the re-arm per hand (the hand that cut must leave
//                       that sphere, or 0.2 s, and cannot cut at the other spot for 0.25 s: a follow-through; the other hand
//                       cuts at once), deflects (the rim flashes red, the cut
//                       direction's chevrons swell, a light buzz; held back 0.3 s and dropped if the plate is cut meanwhile,
//                       and none for a plate that became current with the blade already in it), rewinds, the end; the closed-loop injection on the
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
// 0x0040 = B, 0x0010 = Y, 0x0080 = X). Nothing here copies code from any project; the look (a bright octagonal seal plate with a
// white rim and a dark stroke symbol on a dark halo, chevrons along the cut, a single-edged curved blade with a round guard, gold
// left and steel-cyan right) is this mod's own.
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
// The one-target presentation (the owner's play test, 8 Nov 2026: "a little unintuitive", "difficult to see": low contrast, too
// many at once, which one is next, the cut direction): one plate at one spot; the next materialises there (scale and alpha) as
// soon as the current is cut; nothing flies or slides (nothing moves toward the face but the cut's halves and sparks, which never
// come nearer the eyes).
constexpr double k_appear_s = 0.12;                                                        // the materialise
constexpr float k_plate_r = 0.12f;                                                         // the plate's circumradius at size 1 (24 cm across)
constexpr float k_preview_scale = 0.40f;                                                   // the next symbol, above and behind
constexpr double k_chev_period_s = 0.9; constexpr float k_guide_m = 0.12f;                 // the cut's chevrons; the guide line beyond each edge
constexpr double k_defl_show_s = 0.30, k_flash_s = 0.25;                                   // a deflect's red rim and big chevrons; a pip's confirmation
// A deflect is trusted only when it was not part of a cut (review R5-S2): it is held k_defl_hold_s after its pass ends and dropped
// if the plate goes meanwhile (a wind-up through the sphere, then the chop), and a pass that began less than k_defl_new_s after
// its plate became current never deflects (a two-handed grip: the second blade is already in the sphere when the next appears).
constexpr double k_defl_hold_s = 0.30, k_defl_new_s = 0.15;
constexpr double k_twin_s = 0.25;                                                          // a plate cut while it materialised: a flash ring at the spot (R5-N1)
constexpr double k_resend_s = 0.15, k_settle_s = 0.08, k_ours_s = 0.3;                      // the injection (design 6.2)
// The settle in frames as well as time (review S2): after the player's new press, and after our press's slot frees before a
// resend, k_settle_frames Presents with a fresh read of the game's state must pass: the game compares at its next logic frame
// and the render thread sees it at the Present after that frame (the game's logic runs on its main thread, at most about one frame
// ahead of the Present, so a read at a Present is that frame's or newer, never older: review R6-S1), so a slow frame stretches the
// time, not the count.
constexpr int k_settle_frames = 2;
constexpr double k_gate_grace_s = 0.25;                                                    // the injection gate may drop this long without ending the round (review S1)
constexpr double k_rearm_s = 0.20; constexpr float k_rearm_m = 0.02f;                      // the re-arm per hand (out of the sphere by 2 cm, or 0.2 s)
constexpr double k_cross_s = 0.25;                                                         // ... and no cut at the OTHER spot by that hand this soon (a follow-through)
constexpr float k_snap_min = 0.04f, k_snap_rest = 0.08f, k_jump_speed = 8.f;               // a pose snap; a jump (m/s at the grip)
constexpr double k_gap_s = 0.12;                                                           // a longer gap between samples ends the pair
constexpr double k_practice_s = 4.0, k_end_s = 0.35;                                       // practice's bar; the end's fade
constexpr double k_half_s = 0.45, k_spark_s = 0.20, k_line_s = 0.12;                       // the cut's pieces
constexpr int k_cut_n = 6;

inline float clampf(float v, float lo, float hi) { return odg::clampf(v, lo, hi); }
inline float fin(float v, float d) { return std::isfinite(v) ? v : d; }
/// Ease-out cubic: fast start, gentle arrival, never past 1 (the materialise's scale).
inline float ease_out(float u) { u = odv::clamp01(u); const float a = 1.f - u; return 1.f - a * a * a; }
/// v rotated about the unit axis by ang (radians).
inline V3 rot_axis(V3 v, V3 axis, float ang) { const float c = std::cos(ang), s = std::sin(ang); return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.f - c)); }

// -- settings and the layout ----------------------------------------------------------------------------------------------
/// ini od_targets_blade_pitch / od_targets_dist2 / od_targets_height2 / od_targets_size / od_targets_spread (F8 sliders); the rest are defaults.
struct Cfg {
    float reach = 0.45f;       // m: the blade's length; under 5 cm: a fist (the knuckles) and the fist layout
    float pitch_deg = 0.f;     // the blade's tilt from the grip's -Z (+: tip up), -45..45
    float dist = 0.50f, height = -0.30f;   // the spot: forward of the eyes, height from them (m)
    float size = 1.f;          // 0.6..1.6: a multiplier on the 24 cm plate (the hit sphere and the preview scale with it)
    float hit_cm = 15.f, dir_deg = 65.f, speed = 1.2f;     // hit sphere at size 1, arrow tolerance, contact speed (m/s)
    bool preview = true, shoulder_hand = false, body = true, deflect_buzz = true, haptics = true;
    float time_mult = 1.f;     // od_targets_time, 1..3: the game's countdown runs this many times slower in a targets window (Stretch); practice's bar too
    float spread = 0.36f;      // od_targets_spread, 0.25..0.6 m: between the left and the right spot's centres (at least the plate + 2 cm)
};
struct Geo {
    bool fist = false;
    float D = 0.50f, H = -0.30f;                                 // the spot (m)
    float R = 0.15f, rad = 0.12f, th = 0.025f;                   // hit sphere, plate circumradius, plate thickness
    float speed = 1.2f, cos_dir = 0.4226f, pitch = 0.f, s0 = 0.06f, s1 = 0.51f, curve = 0.025f; int npts = 8;
    float size = 1.f;
    float spread = 0.36f;                                        // between the two spots' centres (m)
};
inline Geo geo(const Cfg& c) {
    Geo g;
    const float size = clampf(fin(c.size, 1.f), 0.6f, 1.6f), reach = clampf(fin(c.reach, 0.45f), 0.f, 0.8f);
    const float dist = clampf(fin(c.dist, 0.5f), 0.35f, 0.9f), height = clampf(fin(c.height, -0.3f), -0.5f, 0.f);
    g.size = size; g.fist = reach < 0.05f;
    g.D = g.fist ? fmaxf(0.30f, dist - 0.10f) : dist;
    g.H = g.fist ? fmaxf(-0.55f, height - 0.05f) : height;
    g.rad = k_plate_r * size; g.th = 0.025f * size;
    g.spread = (std::max)(clampf(fin(c.spread, 0.36f), 0.25f, 0.6f), 2.f * g.rad + 0.02f);   // never overlapping: the plate + 2 cm
    g.R = (clampf(fin(c.hit_cm, 15.f), 8.f, 22.f) + (g.fist ? 1.f : 0.f)) * 0.01f * size;
    g.speed = clampf(fin(c.speed, 1.2f), 0.6f, 3.f); if (g.fist && g.speed > 1.f) g.speed = 1.f;
    g.cos_dir = std::cos(clampf(fin(c.dir_deg, 65.f), 30.f, 80.f) * k_deg);
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
/// The spot: straight ahead (the frame's forward) and below the eyes.
/// A spot: side -1 the left (the d-pad arrows), +1 the right (A / B / X / Y, L1 / R1), 0 between them (spot_of(fr, g) alone).
inline V3 spot_of(const Frame& fr, const Geo& g, int side = 0) { return fr.e0 + fr.f * g.D + fr.u * g.H + fr.r * (0.5f * static_cast<float>(side) * g.spread); }
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
/// Every symbol on the map is cut in a direction (the owner, 9 Nov): the arrows theirs; the face buttons where they sit on the
/// Xbox pad (A down, B right, X left, Y up; the glyph is the map in use's, so the direction follows the button the game wants);
/// L1 left, R1 right. Off the map (G_NONE): none (not hittable).
inline bool sym_directed(int s) { return s >= odv::G_UP && s <= odv::G_X; }
inline V3 arrow_dir(int s, const Axes3& a) {
    switch (s) {
        case odv::G_UP: case odv::G_Y: return a.U;
        case odv::G_RIGHT: case odv::G_B: case odv::G_R1: return a.R;
        case odv::G_DOWN: case odv::G_A: return a.U * -1.f;
        case odv::G_LEFT: case odv::G_X: case odv::G_L1: return a.R * -1.f;
        default: return v3(0.f, 0.f, 0.f);
    }
}
/// The spot: the d-pad arrows on the left, everything else (A / B / X / Y, L1 / R1, off the map) on the right.
inline int side_of(int s) { return sym_arrow(s) ? -1 : 1; }
/// The palette: brightness first. A saturated face (arrows cyan, face buttons amber, L1 / R1 violet; grey off the map) under a
/// near-black symbol (dark on bright reads at a glance), a white rim, all on a dark halo that keeps a bright battle effect behind
/// from washing it out.
inline Col sym_col(int s) {
    if (sym_arrow(s)) return col(0.10f, 0.82f, 1.f, 1.f);
    if (s >= odv::G_Y && s <= odv::G_X) return col(1.f, 0.68f, 0.08f, 1.f);
    if (s == odv::G_L1 || s == odv::G_R1) return col(0.68f, 0.38f, 1.f, 1.f);
    return col(0.62f, 0.62f, 0.64f, 1.f);
}
inline Col ink(float a) { return col(0.04f, 0.04f, 0.06f, a); }
/// The symbol's colour (contrast pass, the owner's second test): near-white tinted toward the plate's colour, the brightest thing
/// in view on the near-black face.
inline Col bright(Col k, float a) { return col(0.80f + 0.20f * k.r, 0.80f + 0.20f * k.g, 0.80f + 0.20f * k.b, a); }
/// The contrast pass's proportions (of the plate's radius): the coloured rim from k_rim_in to 1 (twice the first build's 0.18),
/// its soft outer glow to k_glow_out; the symbol's stroke weight (1.6x the first build's 1.35), each symbol scaled as large as fits
/// inside the rim (k_sym_fit of its inner edge: Runner::sym_scale); the face's alpha.
constexpr float k_rim_in = 0.64f, k_glow_out = 1.22f, k_sym_fit = 0.97f, k_sym_w = 1.35f * 1.6f, k_face_a = 0.92f;

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
/// A contact against a target (design 3.2): |v in its plane| >= speed and within the tolerance of the symbol's cut direction
/// (every symbol on the map since the two places; off the map nothing). deg: the direction error.
inline int judge(const Contact& c, int sym, const Axes3& a, const Geo& g, float* deg = nullptr) {
    if (deg) *deg = 0.f;
    if (sym_directed(sym)) {
        const V3 vp = c.v - a.N * dot(c.v, a.N); const float sp = len(vp);
        if (sp < g.speed) return J_SLOW;
        const float cs = dot(vp, arrow_dir(sym, a)) / sp;
        if (deg) *deg = std::acos(clampf(cs, -1.f, 1.f)) / k_deg;
        return cs >= g.cos_dir ? J_HIT : J_WRONG;
    }
    return J_NONE;   // off the map (since the two places every symbol on the map is cut in a direction)
}
/// The one-target rule for a pass (consecutive pairs in contact with the sphere): an arrow's DIRECTION is judged on the pass's
/// in-plane path so far (the contact point's displacement summed over the pairs), once it is k_dir_min_m long, and its speed on
/// the pair; a button as judge(). With the 15 cm sphere and the 65-degree tolerance a single pair's velocity (a wrist flick, a bow,
/// tracking noise) let 7-12 % of perpendicular strokes through in the noise model; the path through the sphere is the stroke's own
/// direction. 7 cm of path (about 35 ms of a 2 m/s stroke, under a quarter of the sphere): true strokes 100 %, perpendicular about
/// 3 %, opposite 0 (3 cm: 5 %; unit-tested). A turn back starts the path afresh (review R5-S1): a wind-up inside the 30 cm sphere
/// (the ready pose is often in it) would otherwise have to be cancelled by the chop before the chop counts.
constexpr float k_dir_min_m = 0.07f;
struct Pass {
    V3 d; bool on = false;
    void reset() { d = v3(0.f, 0.f, 0.f); on = false; }
    int step(const Contact& c, float dt, int sym, const Axes3& a, const Geo& g, float* deg = nullptr) {
        if (deg) *deg = 0.f;
        on = true;
        if (!sym_directed(sym)) return judge(c, sym, a, g, deg);
        const V3 vp = c.v - a.N * dot(c.v, a.N); const float sp = len(vp);
        if (dot(vp, d) < 0.f) d = v3(0.f, 0.f, 0.f);   // a turn back (a wind-up's apex): the stroke's path starts here
        d = d + vp * dt;
        if (sp < g.speed) return J_SLOW;
        const float dl = len(d);
        if (dl < k_dir_min_m) return J_NONE;   // not yet a direction
        const float cs = dot(d, arrow_dir(sym, a)) / dl;
        if (deg) *deg = std::acos(clampf(cs, -1.f, 1.f)) / k_deg;
        return cs >= g.cos_dir ? J_HIT : J_WRONG;
    }
};

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
    bool elapsed_ok = false; float elapsed = 0.f;               // the countdown's frame count f32 0x133F788, read at this Present (Stretch)
    float total = 0.f;                                          // ... and the window's total seconds f32 0x133F78C (the close's line)
};
struct Hit { int i = -1, hand = 0; float speed = 0.f, deg = 0.f; double t = 0.0; bool pre = false; };
struct TickOut {
    uint16_t press = 0; int press_hand = 1, press_idx = -1; bool resent = false;   // a press for the slot now
    bool opened = false, closed = false, line = false, stopped = false, rewound = false, cancel = false;
    bool hold = false;                                          // a round is on: triggers held back, the grips do not grab, no quick squeeze
    int nh = 0; odg::Haptic hap[8];
    int nhit = 0; Hit hit[8]; int ndefl = 0; int defl[8] = {}, defl_hand[8] = {};
    bool time_write = false; float time_val = 0.f;              // write time_val to the countdown's frame count (f32 0x133F788) now (Stretch) ...
    uint32_t time_expect = 0;                                   // ... by a compare-exchange against these bits (the count as read); report with time_result
    void pulse(int hand, float amp, float sec) { if (nh < 8) { hap[nh].hand = hand; hap[nh].amp = amp; hap[nh].sec = sec; nh++; } }
};
struct Tgt {
    uint16_t val = 0; int sym = odv::G_NONE;
    bool struck = false, done = false;                          // done: the game's progress passed it (a button, or ours)
    double t_hit = -1.0; int hand = -1;
    double defl_t = -1e300;                                     // the last deflect on it (the red rim, the big chevrons)
};
struct HandTrack {
    bool have = false; Blade b; double t = 0.0; float step = 0.f; bool have_step = false; float tip_speed = 0.f; uint32_t ns = 0;   // ns: samples seen
    int pend_n = 0; int pend_i[4] = {}; float pend_deg[4] = {}; V3 pend_v[4]; double pend_t[4] = {}; float pend_step = 0.f; V3 pend_from;   // pend_from: the pose before the candidate step
    uint32_t in_contact = 0, fast_wrong = 0;                    // per target: in contact last pair; struck fast the wrong way in this pass
    Pass pass;                                                  // the pass through the one sphere (an arrow's direction)
    bool rearm = false; double rearm_t = 0.0; uint32_t rearm_k = 0;   // this hand cut: it cannot cut again until out of the sphere (or 0.2 s)
    V3 rearm_c; int cut_side = 0; double cut_t = -1e300;        // ... that sphere's centre; the spot it cut at and when (the cross-spot follow-through)
    double pass_t0 = 0.0;                                       // when this hand's pass began
    int defl_i = -1; double defl_at = 0.0;                      // a deflect held back (k_defl_hold_s): its target and when its pass ended
    V3 tr_root[6], tr_tip[6]; double tr_t[6] = {}; bool tr_fast[6] = {}; int tr_n = 0, tr_head = 0;
    void clear_round() { pend_n = 0; in_contact = fast_wrong = 0; rearm = false; pass.reset(); defl_i = -1; cut_t = -1e300; }
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
    bool twin = false;                                          // cut while it materialised (a second plate in one stroke): the flash ring
};
struct Stats {
    int n = 0, struck = 0, hits[2] = { 0, 0 }, pre = 0, sweeps = 0, deflects = 0, defl_dropped = 0, rewinds = 0, snaps = 0, jumps = 0;
    double first = -1.0, last = -1.0, done = -1.0; uint32_t presses = 0, resends = 0, cancels = 0; int end = E_NONE; bool practice = false; int stop_why = 0;
    uint32_t verts_max = 0, dropped = 0;
    float mult = 1.f; uint32_t time_writes = 0, time_fails = 0; // the countdown's multiplier for the round; the frame-count writes landed; refused (changed since the read)
    bool have_count = false; float count0 = 0.f, count1 = 0.f; double tc0 = 0.0, tc1 = 0.0;   // the game's count at the first and last read in the window, and when (R6-N4)
    bool have_left = false; float g_left = 0.f, g_total = 0.f;  // the game's seconds left and total at the last read (the close: its damage factor, R6-S2)
};
struct Round {
    bool on = false, practice = false, stopped = false; int end = E_NONE; double t_open = 0.0, t_end = -1e300;
    int n = 0; uint16_t seq[k_n_max] = {}; Tgt tg[k_n_max]; odg::AuronMap map = odg::k_auron_map;
    Frame fr; Geo g; uint32_t epoch = 0; float left0 = 4.f, left_now = 4.f; int prog_shown = 0;
    int cur = -1; double cur_t = 0.0;                           // the current target (the lowest neither struck nor done) and when it began to materialise
    int flash_i = -1; double flash_t = -1e300;                  // the pip whose progress just registered, and when (the confirmation flash)
    float mult = 1.f;                                           // od_targets_time at the open (the countdown's stretch; practice's bar)
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

/// More time for Bushido (od_targets_time; the owner's second test). The game's countdown (disassembly, our exe): each logic frame
/// while the timer word 0x133F77C == 2, 0x891AC0 adds 1.0 to the f32 frame count 0x133F788 and recomputes the seconds left
/// 0x133F790 = total 0x133F78C - count / 30 (25 in the 50 Hz mode), clamped at 0; Auron's state 4 (0x890FE5) then goes to state 7
/// when the seconds left are not above 0. The seconds left are derived every frame, so the lever is the frame count: each Present
/// with a fresh read, while a strike-the-targets window is open and the timer runs, the count is written back as
/// prev + (now - prev) / mult (prev: what we wrote or saw at the last such Present), so a frame counts 1 / mult. A count that went
/// down (the game's reset at a window's end or a battle's start: a new window) or a Present without a fresh read starts afresh with
/// no write; mult 1 never writes.
/// The game's logic runs on its main thread and the Present on the render thread (review R6-S1: the first build said "one thread",
/// which was wrong), so the game may tick, or reset the count at a window's end, between our read (od_frame) and our write. The
/// write is therefore a compare-exchange of the float's 32 bits against the exact bits read (TickOut::time_expect): it lands only
/// if the count is unchanged; otherwise nothing is written (time_result: refused) and the stretch keeps its value while its base
/// (the game's count it corresponds to) becomes the count read, so the next write stretches only the frames since the read and a
/// tick in between is not lost; a reset (a count below the base) starts afresh. A reset can never be overwritten with an old count. (The count is 4-byte aligned in .data; a
/// read torn across a tick cannot match the current bits unless it equals them, so a torn read only fails the exchange.)
struct Stretch {
    bool have = false; float prev = 0.f, base = 0.f, read = 0.f;   // the stretched count; the game's count it stands for; the last read
    void reset() { have = false; }
    void refused() { base = read; }                                  // the write did not land: the game's count is (at least) what we read
    /// on: the window is open and the count was read now; returns true with *out when a write is due.
    bool step(bool on, float now, float mult, float* out) {
        if (!on || !(mult > 1.0001f) || !std::isfinite(now) || now < 0.f) { have = false; return false; }
        if (!have || now < base) { have = true; prev = base = now; return false; }   // first sight, or the count went back (a reset: a new window)
        const float w = prev + (now - base) / mult;
        read = now; prev = base = w;                                              // (as if it lands; refused() corrects the base)
        if (!(w < now)) return false;                                           // no logic frame since: nothing to pull back
        *out = w; return true;
    }
};

struct Runner {
    Cfg cfg; Round r; HandTrack ht[2]; Inject inj; Stats st, last_st; Cut cut[k_cut_n]; int cut_next = 0;
    uint32_t rounds = 0; double hap_t[2] = { -1.0, -1.0 };
    Stretch tm;                                                  // the countdown's stretch (od_targets_time)
    uint32_t phys_seen = 0; int phys_frames = 1000; double phys_note_t = -1e300;   // the settle in frames (review S2); when the last new press was noticed
    double gate_off_t = -1.0;                                    // the injection gate off since (review S1); -1 on
    /// The player's new presses: a new count starts the frame count (the noticing Present itself does not count).
    void note_phys(const In& in) { if (in.phys_n != phys_seen) { phys_seen = in.phys_n; phys_frames = 0; phys_note_t = in.t; } else if (in.read_now && phys_frames < 1000) phys_frames++; }
    bool settled(const In& in) const { return phys_frames >= k_settle_frames && in.phys_age >= k_settle_s; }
    char line_buf[400] = "";
    uint32_t verts_max = 0, verts_dropped = 0;
    bool gone(int i) const { return r.tg[i].struck || r.tg[i].done; }
    /// The current target: the lowest index neither struck nor done.
    int front() const { for (int i = 0; i < r.n; ++i) if (!gone(i)) return i; return r.n; }
    /// The one after it (the preview), or -1.
    int next_after(int f) const { for (int i = f + 1; i < r.n; ++i) if (!gone(i)) return i; return -1; }
    /// Hittable: a round on and not stopped; ONLY the current target (no pre-hits), on the map, and with shoulder_hand an L1 / R1
    /// only for its own hand. (It may be cut while it materialises: the spot and the sphere do not move.)
    bool hittable(int i, int h) const {
        if (!r.on || r.stopped || i < 0 || i >= r.n) return false;
        const Tgt& g = r.tg[i];
        if (g.struck || g.done || g.sym == odv::G_NONE || i != front()) return false;
        if (cfg.shoulder_hand && h >= 0 && ((g.sym == odv::G_L1 && h != 0) || (g.sym == odv::G_R1 && h != 1))) return false;
        return true;
    }
    bool active(double t) const {
        if (r.on || t - r.t_end < k_end_s) return true;
        for (const Cut& c : cut) if (t - c.t0 < k_half_s) return true;
        return false;
    }
    bool holding() const { return r.on; }
    /// Target i's spot (its symbol's side); spot(): the current target's (the left spot when none).
    V3 spot_i(int i) const { return spot_of(r.fr, r.g, i >= 0 && i < r.n ? side_of(r.tg[i].sym) : -1); }
    V3 spot() const { return spot_i(r.cur >= 0 && r.cur < r.n ? r.cur : front() < r.n ? front() : r.n - 1); }
    /// The pips' and bar's centre: between the spots, pushed back to the spots' distance from the eyes (never nearer than a plate).
    V3 hud_centre() const { const float hs = 0.5f * r.g.spread; return r.fr.e0 + r.fr.f * std::sqrt(r.g.D * r.g.D + hs * hs) + r.fr.u * r.g.H; }
    /// The preview's place: above and a little behind its own spot's plate (never in front of it, never nearer the eyes).
    V3 preview_spot(int i) const {
        const V3 c = spot_i(i); const Axes3 ax = plate_axes(r.fr, c);
        return c + ax.U * (r.g.rad * (1.f + k_preview_scale) + 0.03f * r.g.size) + r.fr.f * (0.05f * r.g.size);
    }
    /// Hand h may cut target i at t: armed (the re-arm), and not a follow-through into the other spot (k_cross_s after its cut).
    bool armed(int i, int h, double t) const {
        if (h < 0 || h > 1) return true;
        const HandTrack& k = ht[h];
        if (k.rearm) return false;
        return !(i >= 0 && i < r.n && side_of(r.tg[i].sym) != k.cut_side && t - k.cut_t < k_cross_s);
    }
    /// The materialise: 0 -> 1 over k_appear_s from when target i became current (1 for any other).
    float appear(int i, double t) const { if (i != r.cur) return 1.f; return odv::clamp01(static_cast<float>((t - r.cur_t) / k_appear_s)); }
    /// The current target follows the strikes and the game's progress; a new one starts its materialise.
    void update_current(double t) { const int f = front(); if (f != r.cur) { r.cur = f; r.cur_t = t; } }
    // -- a round ------------------------------------------------------------------------------------------------------------
    void open(const uint16_t* seq, int n, const odg::AuronMap& m, bool practice, const In& in) {
        r = Round{}; r.on = true; r.practice = practice; r.t_open = in.t; r.n = n < 0 ? 0 : n > k_n_max ? k_n_max : n; r.map = m;
        r.g = geo(cfg); r.epoch = in.space_epoch;
        latch_now(in);
        for (int i = 0; i < r.n; ++i) { Tgt& g = r.tg[i]; g = Tgt{}; g.val = seq[i]; r.seq[i] = seq[i]; g.sym = odv::glyph_of(m, seq[i]); }
        update_current(in.t);   // the first target materialises at once (no fly-in)
        r.mult = clampf(fin(cfg.time_mult, 1.f), 1.f, 3.f);
        r.left0 = practice ? static_cast<float>(k_practice_s) * r.mult : (in.left > 0.2f ? in.left : 4.f) * r.mult; r.left_now = r.left0;   // real seconds (the stretched count)
        tm.reset();
        inj.reset(); ht[0].clear_round(); ht[1].clear_round();
        st = Stats{}; st.n = r.n; st.practice = practice; st.mult = r.mult; rounds++;
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
        r.on = false; r.end = kind; r.t_end = t; inj.inflight = -1; tm.reset();   // (no countdown write after the window closed)
        st.end = kind; st.presses = inj.presses; st.resends = inj.resends; st.cancels = inj.cancels; st.stop_why = r.stopped ? inj.stop_why : 0;
        st.verts_max = verts_max; st.dropped = verts_dropped; verts_max = 0; verts_dropped = 0;
        last_st = st;
        format_line(line_buf, sizeof(line_buf), st);
        o.closed = true; o.line = true;
        if (kind == E_SUCCESS && cfg.haptics) { o.pulse(0, 0.5f, 0.03f); o.pulse(1, 0.5f, 0.03f); hap_t[0] = t + 0.09; hap_t[1] = t + 0.18; }
    }
    /// The game reset the progress with no press of ours just before (a wrong button of the player's): every target back; the
    /// first one materialises again.
    void rewind(double t, TickOut& o) {
        for (int i = 0; i < r.n; ++i) { Tgt& g = r.tg[i]; g.struck = g.done = false; g.t_hit = -1.0; g.hand = -1; }
        r.cur = -1; update_current(t);
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
        if (inj.have_prog && p > inj.last_prog && p >= 1) { r.flash_i = p - 1; r.flash_t = t; }   // the pip's confirmation flash
        inj.last_prog = p; inj.have_prog = true;
        if (in.game_press_ok && in.game_press != 0 && in.game_press != inj.ours_val && t >= inj.ours_t) inj.other_seen = true;
        // (after the reset test: a press noticed at this Present counts only for a reset seen at a later one)
        if (phys_seen != inj.phys_at && phys_note_t >= inj.ours_t && phys_note_t - inj.ours_t < k_ours_s) inj.phys_after = true;
        for (int i = 0; i < r.n && i < p; ++i) r.tg[i].done = true;   // (a target the player's button did: the next one materialises)
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
        const V3 C = spot_i(i); const Axes3 ax = plate_axes(r.fr, C);
        g.struck = true; g.t_hit = t_hit; g.hand = h;
        st.struck++; st.hits[h]++;
        const double rel = t_hit - r.t_open; if (st.first < 0.0) st.first = rel; if (rel > st.last) st.last = rel;
        k.rearm = true; k.rearm_t = t_now; k.rearm_k = k.ns; k.rearm_c = C; k.cut_side = side_of(g.sym); k.cut_t = t_now;
        const uint32_t bit = 1u << i; k.in_contact &= ~bit; k.fast_wrong &= ~bit; k.pass.reset();
        V3 vp = c.v - ax.N * dot(c.v, ax.N); const float sp = len(vp);
        const V3 cd = sp > 0.3f ? vp * (1.f / sp) : norm(ax.R - ax.U);   // a stab: a diagonal cut
        add_cut(C, ax, cd, c.at, c.v, sym_col(g.sym), t_now);
        if (i == r.cur && t_now - r.cur_t < k_appear_s) cut[(cut_next + k_cut_n - 1) % k_cut_n].twin = true;   // never fully seen: flash (R5-N1)
        if (o.nhit < 8) { Hit& x = o.hit[o.nhit++]; x.i = i; x.hand = h; x.speed = sym_directed(g.sym) ? sp : len(c.v); x.deg = deg; x.t = t_hit; x.pre = false; }
    }
    /// For the tests and the injection model: hand h strikes target i now if it is hittable and the hand is armed (a plain downward cut).
    bool strike(int i, int h, double t, TickOut& o) {
        if (!hittable(i, h) || (h >= 0 && h < 2 && ht[h].rearm)) return false;   // (a deliberate cut: the cross-spot follow-through rule is the swept blade's)
        Contact c; c.v = r.fr.u * -2.f; c.at = spot_i(i); apply_hit(i, h, c, t, t, 0.f, o); return true;
    }
    /// A wrong-direction pass on an arrow: the rim flashes red and the chevrons swell (k_defl_show_s), a light buzz on that hand
    /// (on by default in this presentation: the direction rule is learnt from the feedback).
    void deflect(int i, int h, double t, TickOut& o) {
        r.tg[i].defl_t = t; st.deflects++;
        if (o.ndefl < 8) { o.defl[o.ndefl] = i; o.defl_hand[o.ndefl] = h; o.ndefl++; }
        if (cfg.deflect_buzz && cfg.haptics) o.pulse(h, 0.3f, 0.02f);
    }
    /// A held deflect (R5-S2): dropped if its plate went meanwhile (cut by either hand, or done by a button) or the round ended;
    /// shown k_defl_hold_s after its pass ended otherwise.
    void settle_deflect(int h, double t, TickOut& o) {
        HandTrack& k = ht[h]; if (k.defl_i < 0) return;
        const int i = k.defl_i;
        if (!r.on || i >= r.n || gone(i)) { k.defl_i = -1; if (r.on) st.defl_dropped++; return; }
        if (t - k.defl_at >= k_defl_hold_s) { k.defl_i = -1; deflect(i, h, t, o); }
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
                if (hittable(i, h) && armed(i, h, t)) { Contact c; c.v = k.pend_v[j]; c.at = spot_i(i); apply_hit(i, h, c, k.pend_t[j], t, k.pend_deg[j], o); nh[h]++; }
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
            const int i = front();
            const bool can = i < r.n && hittable(i, h) && armed(i, h, t);   // the one target, for an armed hand (not a follow-through from the other spot)
            const uint32_t bit = can ? 1u << i : 0u;
            if (can) {
                const V3 C = spot_i(i); const Axes3 ax = plate_axes(r.fr, C);
                const Contact c = sweep(k.b, b, static_cast<float>(dt), C, g);
                if (c.dist <= g.R) {
                    if (!(k.in_contact & bit)) { k.pass.reset(); k.pass_t0 = t; }   // a new pass
                    k.in_contact |= bit; float deg = 0.f;
                    const int jd = k.pass.step(c, static_cast<float>(dt), r.tg[i].sym, ax, g, &deg);
                    if (jd == J_HIT) {
                        if (cand) { if (k.pend_n < 4) { const int q = k.pend_n++; k.pend_i[q] = i; k.pend_v[q] = c.v; k.pend_deg[q] = deg; k.pend_t[q] = k.t + dt * c.u; } k.pend_step = step; k.pend_from = k.b.p; }
                        else { apply_hit(i, h, c, k.t + dt * c.u, t, deg, o); hits_now++; }
                    }
                    else if (jd == J_WRONG) k.fast_wrong |= bit;
                }
                else if (k.in_contact & bit) {   // the pass ends (the next starts afresh above): a fast wrong way is held back (R5-S2)
                    if ((k.fast_wrong & bit) && k.pass_t0 - r.cur_t >= k_defl_new_s && k.defl_i < 0) { k.defl_i = i; k.defl_at = t; }
                    k.in_contact &= ~bit; k.fast_wrong &= ~bit;
                }
            }
            k.in_contact &= bit; k.fast_wrong &= bit;
        }
        nh[h] += hits_now;
        k.ns++;   // the re-arm's out test starts at the sample after the hit's (the blade out of the sphere for one sample)
        if (k.rearm && (t - k.rearm_t >= k_rearm_s || (k.ns > k.rearm_k + 1 && blade_dist(b, g, k.rearm_c) > g.R + k_rearm_m))) k.rearm = false;   // out of the sphere it cut
        k.step = step; k.have_step = true; k.b = b; k.t = t;
        k.trail(blade_pt(b, g, g.s0), blade_pt(b, g, g.s1), t, k.tip_speed > g.speed);
    }
    // -- the tick -------------------------------------------------------------------------------------------------------------------
    /// The glue's answer to o.time_write: landed, or refused (the count changed since the read: a tick or a reset on the game's
    /// thread): then nothing was written; the next write stretches the frames since the read (a reset starts afresh).
    void time_result(bool landed) { if (landed) st.time_writes++; else { st.time_fails++; tm.refused(); } }
    void tick(const In& in, TickOut& o) {
        o = TickOut{};
        const double t = in.t;
        if (r.on && !r.practice && in.elapsed_ok && in.auron >= 4) { st.have_left = true; st.g_left = in.left; st.g_total = in.total; }   // (before a close at this Present: its last read)
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
        {   // the countdown's stretch: only in an open strike-the-targets window of Auron's with the timer running and the count read now
            const bool on = r.on && !r.practice && in.targets_mode && in.win == odg::OD_AURON && in.auron == 4 && in.timer == 2 && in.elapsed_ok;
            float w = 0.f;
            if (tm.step(on, in.elapsed, r.mult, &w)) { o.time_write = true; o.time_val = w; std::memcpy(&o.time_expect, &in.elapsed, 4); }
            if (on) { if (!st.have_count) { st.have_count = true; st.count0 = in.elapsed; st.tc0 = t; } st.count1 = in.elapsed; st.tc1 = t; }
        }
        if (r.on) update_current(t);
        if (in.sample_new) {
            int nh[2] = { 0, 0 };
            for (int h = 0; h < 2; ++h) { hand_sample(h, in.hand[h], t, o, nh); if (r.on) update_current(t); }   // (a cut: the next is current for the other hand at once)
            if (cfg.haptics) for (int h = 0; h < 2; ++h) if (nh[h] >= 1) o.pulse(h, 0.7f, 0.015f);   // a hit (one target, one hand re-armed: at most one a sample)
        }
        if (r.on) update_current(t);
        for (int h = 0; h < 2; ++h) settle_deflect(h, t, o);
        if (r.on && !r.practice) {
            r.left_now = in.left * r.mult;   // the game's seconds left run 1 / mult as fast: real seconds
            const uint16_t b = inj_decide(in, o);
            if (b) { o.press = b; o.press_idx = inj.inflight; const int hh = inj.inflight >= 0 ? r.tg[inj.inflight].hand : 1; o.press_hand = hh < 0 ? 1 : hh; }
        }
        if (r.on && r.practice) {
            int s = 0; for (int i = 0; i < r.n; ++i) if (r.tg[i].struck) s++;
            if (s > r.prog_shown && s >= 1) { r.flash_i = s - 1; r.flash_t = t; }   // practice's pips follow the strikes
            r.prog_shown = s; r.left_now = static_cast<float>(k_practice_s * r.mult - (t - r.t_open));
            if (s == r.n) finish(E_SUCCESS, t, o);
            else if (t - r.t_open >= k_practice_s * r.mult) finish(E_TIMEOUT, t, o);
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
            const double total = x.end == E_SUCCESS ? x.last : k_practice_s * x.mult;
            k = odg::fmt_append(s, n, k, "practice: %d of %d in %.2f s%s", x.struck, x.n, total, x.end == E_SUCCESS ? "" : ", time out");
            if (x.mult > 1.0001f) k = odg::fmt_append(s, n, k, " (time x%.2f)", static_cast<double>(x.mult));
            if (x.struck >= 1) {
                k = odg::fmt_append(s, n, k, " (first %.2f s", x.first);
                if (x.struck >= 2) k = odg::fmt_append(s, n, k, ", then %.2f s an input", (x.last - x.first) / (x.struck - 1));
                k = odg::fmt_append(s, n, k, "; deflects %d)", x.deflects);
            }
            else k = odg::fmt_append(s, n, k, " (no strike; deflects %d)", x.deflects);
            return k;
        }
        k = odg::fmt_append(s, n, k, "Auron targets: %d symbols, struck %d (left blade %d, right %d)", x.n, x.struck, x.hits[0], x.hits[1]);
        if (x.struck) k = odg::fmt_append(s, n, k, ", first strike %.2f s, last %.2f s", x.first, x.last);
        if (x.done >= 0.0) k = odg::fmt_append(s, n, k, ", game done %.2f s", x.done);
        k = odg::fmt_append(s, n, k, "; presses %u (resends %u, cancelled by a button %u), deflects %d (dropped as part of a cut %d), rewinds %d, snaps dropped %d, jumps skipped %d",
                            x.presses, x.resends, x.cancels, x.deflects, x.defl_dropped, x.rewinds, x.snaps, x.jumps);
        if (x.stop_why) k = odg::fmt_append(s, n, k, "; STOPPED injecting (%s): buttons only", x.stop_why == 1 ? "a reset right after our press: the map or a racing button" : "the game missed a press twice");
        k = odg::fmt_append(s, n, k, "; time x%.2f", static_cast<double>(x.mult));
        if (x.have_count) {   // R6-N4: the effect: the game's frames against the real ones (30 a second; 25 in the 50 Hz mode)
            const double real = x.tc1 - x.tc0, gf = static_cast<double>(x.count1 - x.count0);
            k = odg::fmt_append(s, n, k, ": the game counted %.1f frames in %.2f s (%.1f at 30 a second: x%.2f)", gf, real, real * 30.0, gf > 0.5 ? real * 30.0 / gf : 0.0);
        }
        k = odg::fmt_append(s, n, k, ", %u writes, %u refused (changed since the read)", x.time_writes, x.time_fails);
        if (x.have_left && x.g_total > 0.f)   // R6-S2: the game's time bonus at the close: damage x (left + 2 total) / (2 total)
            k = odg::fmt_append(s, n, k, "; at the close the game's left %.2f of %.2f s (its damage factor x%.2f)", static_cast<double>(x.g_left), static_cast<double>(x.g_total),
                                static_cast<double>((x.g_left + 2.f * x.g_total) / (2.f * x.g_total)));
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
    /// The symbol (units of s): a slightly larger, dimmer copy of the same strokes (gk; its alpha 0 for none) under the core k;
    /// wm: the stroke weight.
    static void symbol(Out& o, int sym, V3 c, V3 R, V3 U, float s, Col k, float wm = 1.f, Col gk = col(0.f, 0.f, 0.f, 0.f)) {
        static const float A[][4] = { { -0.36f, -0.46f, 0.f, 0.46f }, { 0.f, 0.46f, 0.36f, -0.46f }, { -0.19f, -0.07f, 0.19f, -0.07f } };
        static const float X[][4] = { { -0.34f, -0.46f, 0.34f, 0.46f }, { -0.34f, 0.46f, 0.34f, -0.46f } };
        static const float Y[][4] = { { -0.34f, 0.46f, 0.f, 0.f }, { 0.34f, 0.46f, 0.f, 0.f }, { 0.f, 0.f, 0.f, -0.46f } };
        static const float B[][4] = { { -0.30f, -0.46f, -0.30f, 0.46f }, { -0.30f, 0.46f, 0.12f, 0.46f }, { 0.12f, 0.46f, 0.28f, 0.31f }, { 0.28f, 0.31f, 0.28f, 0.15f },
                                      { 0.28f, 0.15f, 0.12f, 0.f }, { -0.30f, 0.f, 0.16f, 0.f }, { 0.16f, 0.f, 0.33f, -0.15f }, { 0.33f, -0.15f, 0.33f, -0.31f },
                                      { 0.33f, -0.31f, 0.16f, -0.46f }, { 0.16f, -0.46f, -0.30f, -0.46f } };
        static const float L[][4] = { { -0.22f, 0.46f, -0.22f, -0.46f }, { -0.22f, -0.46f, 0.28f, -0.46f } };
        static const float Rr[][4] = { { -0.26f, -0.46f, -0.26f, 0.46f }, { -0.26f, 0.46f, 0.10f, 0.46f }, { 0.10f, 0.46f, 0.26f, 0.31f }, { 0.26f, 0.31f, 0.26f, 0.15f },
                                       { 0.26f, 0.15f, 0.10f, 0.f }, { 0.10f, 0.f, -0.26f, 0.f }, { 0.f, 0.f, 0.30f, -0.46f } };
        const bool gl = gk.a > 0.f; const Col glow = gk;
        auto strokes = [&](const float (*t)[4], int n, float sc, float ox, float oy, float w) {
            for (int pass = gl ? 0 : 1; pass < 2; ++pass)
                for (int i = 0; i < n; ++i) stroke(o, c, R, U, (ox + t[i][0] * sc) * s, (oy + t[i][1] * sc) * s, (ox + t[i][2] * sc) * s, (oy + t[i][3] * sc) * s, (pass ? w * wm : 1.6f * w * wm) * s, pass ? k : glow);
        };
        if (sym_arrow(sym)) {   // a bold arrow: the head widens with the shaft (never under 0.42)
            V3 a = U, b = R;
            if (sym == odv::G_RIGHT) { a = R; b = U * -1.f; } else if (sym == odv::G_DOWN) { a = U * -1.f; b = R * -1.f; } else if (sym == odv::G_LEFT) { a = R * -1.f; b = U; }
            const float hw0 = 0.13f * wm, hb = (std::max)(0.42f, 2.f * hw0);
            for (int pass = gl ? 0 : 1; pass < 2; ++pass) {
                const float g = pass ? 1.f : 1.12f; const Col cc = pass ? k : glow;
                o.tri(c + a * (0.62f * s * g), cc, c + a * (0.05f * s) - b * (hb * s * g), cc, c + a * (0.05f * s) + b * (hb * s * g), cc);
                const float hw = hw0 * s * (pass ? 1.f : 1.45f);
                o.quad(c + a * (0.08f * s) - b * hw, cc, c + a * (0.08f * s) + b * hw, cc, c - a * (0.55f * s * g) + b * hw, cc, c - a * (0.55f * s * g) - b * hw, cc);
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
                if (gl) for (int i = 0; i < 4; ++i) stroke(o, c, R, U, tab[i][0] * s, tab[i][1] * s, tab[i][2] * s, tab[i][3] * s, 0.08f * wm * 1.6f * s, glow);
                for (int i = 0; i < 4; ++i) stroke(o, c, R, U, tab[i][0] * s, tab[i][1] * s, tab[i][2] * s, tab[i][3] * s, 0.08f * wm * s, k);
                odv::rect(o, c, R, U, (lf ? -0.76f : 0.62f) * s, -0.10f * s, (lf ? -0.62f : 0.76f) * s, 0.10f * s, k);
                if (lf) strokes(L, 2, 0.5f, 0.f, 0.f, 0.11f); else strokes(Rr, 7, 0.5f, 0.f, 0.f, 0.11f);
                return;
            }
            default: odv::arc(o, c, R, U, 0.22f * s, 0.10f * s, 0.f, odg::k_two_pi, k, 8); return;   // off the map: a ring ("use the button")
        }
    }
    /// The symbol's extent at scale 1 with the contrast pass's weight (its core's farthest vertex from the centre), per glyph,
    /// measured once; and the scale that makes it as large as the plate allows inside the rim.
    static float sym_extent(int sym) {
        static float ext[odv::G_N] = {};
        static const bool init = [] {
            for (int gl = 0; gl < odv::G_N; ++gl) {
                Vtx b[192]; Out o; o.v = b; o.max = 192;
                symbol(o, gl, v3(0.f, 0.f, 0.f), v3(1.f, 0.f, 0.f), v3(0.f, 1.f, 0.f), 1.f, col(1.f, 1.f, 1.f, 1.f), k_sym_w);
                float m = 0.f; for (int v = 0; v < o.n; ++v) m = (std::max)(m, std::sqrt(b[v].p[0] * b[v].p[0] + b[v].p[1] * b[v].p[1]));
                ext[gl] = m > 0.05f ? m : 1.f;
            }
            return true;
        }();
        (void)init;
        return sym >= 0 && sym < odv::G_N ? ext[sym] : 1.f;
    }
    static float sym_scale(int sym, float rad) { return k_sym_fit * k_rim_in * rad / sym_extent(sym); }
    /// The plate (contrast pass): a short octagonal prism with a near-black face (k_face_a), a thick bright rim in the plate's
    /// colour (k_rim_in .. 1 of the radius; red on a deflect) and a soft glow ring outside it (to k_glow_out), front toward ax.N.
    /// (Alpha-blended layers: the marker pass blends straight alpha only, so the glows are dim layers drawn under what they light.)
    static void plate(Out& o, V3 c, const Axes3& ax, float rad, float th, Col rim, float alpha) {
        V3 dir[8];
        for (int j = 0; j < 8; ++j) { const float a = (22.5f + 45.f * static_cast<float>(j)) * k_deg; dir[j] = ax.R * std::cos(a) + ax.U * std::sin(a); }
        const V3 F = ax.N * (0.5f * th), Bk = ax.N * (-0.5f * th), Fo = ax.N * (0.25f * th);
        const float ri = k_rim_in * rad;
        {   // the glow ring outside the rim (in the back face's plane, under the plate)
            const Col gi = col(rim.r, rim.g, rim.b, 0.55f * alpha), go = col(rim.r, rim.g, rim.b, 0.f);
            for (int j = 0; j < 8; ++j) { const int k = (j + 1) % 8; o.quad(c + Bk + dir[j] * rad, gi, c + Bk + dir[k] * rad, gi, c + Bk + dir[k] * (k_glow_out * rad), go, c + Bk + dir[j] * (k_glow_out * rad), go); }
        }
        for (int j = 0; j < 8; ++j) {   // the sides (behind the front: painted first): the rim's colour, darker
            const int k = (j + 1) % 8; const V3 en = norm(dir[j] + dir[k]); const float lit = 0.45f + 0.20f * dot(en, ax.U);
            const Col sc = col(rim.r * lit, rim.g * lit, rim.b * lit, alpha);
            o.quad(c + Bk + dir[j] * rad, sc, c + Bk + dir[k] * rad, sc, c + Fo + dir[k] * rad, sc, c + Fo + dir[j] * rad, sc);
        }
        for (int j = 0; j < 8; ++j) {   // the rim: thick, bright, lit a little from above
            const int k = (j + 1) % 8; const V3 en = norm(dir[j] + dir[k]); const float lit = clampf(0.90f + 0.10f * dot(en, ax.U), 0.8f, 1.f);
            const Col bc = col(fminf(1.f, rim.r * lit + 0.08f), fminf(1.f, rim.g * lit + 0.08f), fminf(1.f, rim.b * lit + 0.08f), alpha);
            o.quad(c + Fo + dir[j] * rad, bc, c + Fo + dir[k] * rad, bc, c + F + dir[k] * ri, bc, c + F + dir[j] * ri, bc);
        }
        const Col fi = col(0.035f + 0.05f * rim.r, 0.035f + 0.05f * rim.g, 0.04f + 0.05f * rim.b, k_face_a * alpha), fe = col(0.02f, 0.02f, 0.025f, k_face_a * alpha);   // near-black, a hint of the colour at the centre
        for (int j = 0; j < 8; ++j) { const int k = (j + 1) % 8; o.tri(c + F, fi, c + F + dir[j] * ri, fe, c + F + dir[k] * ri, fe); }
    }
    /// The soft dark halo behind a plate: alpha a out to 1.15 rad, fading to nothing at 1.4 rad.
    static void halo(Out& o, V3 c, const Axes3& ax, float rad, float a) {
        const Col in_ = col(0.f, 0.f, 0.f, a), out_ = col(0.f, 0.f, 0.f, 0.f);
        odv::disc(o, c, ax.R, ax.U, 1.15f * rad, in_, in_, 16);
        for (int j = 0; j < 16; ++j) {
            const float a0 = odg::k_two_pi * static_cast<float>(j) / 16.f, a1 = odg::k_two_pi * static_cast<float>(j + 1) / 16.f;
            const V3 d0 = ax.R * std::cos(a0) + ax.U * std::sin(a0), d1 = ax.R * std::cos(a1) + ax.U * std::sin(a1);
            o.quad(c + d0 * (1.15f * rad), in_, c + d1 * (1.15f * rad), in_, c + d1 * (1.4f * rad), out_, c + d0 * (1.4f * rad), out_);
        }
    }
    /// One chevron: a V pointing along d (in the plate's plane, across b), its tip at c; dark outline under a white core.
    static void chevron(Out& o, V3 c, V3 d, V3 b, float hw, float depth, float w, float a, Col core = col(1.f, 1.f, 1.f, 1.f)) {
        const V3 l = c - d * depth + b * hw, rr = c - d * depth - b * hw;
        auto seg = [&](V3 p0, V3 p1, float wd, Col k) {
            const V3 dir = norm(p1 - p0), side = norm(cross(dir, cross(d, b))) * (0.5f * wd);
            const V3 e0 = p0 - dir * (0.5f * wd), e1 = p1 + dir * (0.5f * wd);
            o.quad(e0 - side, k, e1 - side, k, e1 + side, k, e0 + side, k);
        };
        seg(l, c, 1.9f * w, ink(0.75f * a)); seg(rr, c, 1.9f * w, ink(0.75f * a));
        seg(l, c, w, col(core.r, core.g, core.b, a)); seg(rr, c, w, col(core.r, core.g, core.b, a));
    }
    /// The current target: halo, guide line (arrows), plate, symbol, chevrons (arrows), all materialising and pulsing.
    void draw_current(int i, double t, float A, const In& in, Out& o) const {
        const Tgt& g = r.tg[i];
        const float u = appear(i, t), e = ease_out(u);
        const float pulse = 1.02f + 0.02f * static_cast<float>(std::sin((t - r.t_open) * odg::k_two_pi * 1.5));   // 1.00-1.04
        const float sc = (0.6f + 0.4f * e) * pulse, al = A * u;
        if (al <= 0.f) return;
        const V3 C = spot_i(i); const Axes3 ax = plate_axes(r.fr, C);
        const float rad = r.g.rad * sc, th = r.g.th * sc;
        const bool grey = r.stopped || (!r.on && r.end == E_TIMEOUT);
        const double da = t - g.defl_t; const bool defl = da >= 0.0 && da < k_defl_show_s;
        const Col face = grey ? col(0.55f, 0.55f, 0.57f, 1.f) : sym_col(g.sym);   // the plate's colour (the rim, the glows, the symbol's tint)
        const Col rim = defl ? col(1.f, 0.18f, 0.12f, 1.f) : face;
        const Col ink_k = bright(face, al), glow_k = col(face.r, face.g, face.b, 0.45f * al);
        halo(o, C - ax.N * th, ax, rad, 0.6f * al);
        const bool arrow = sym_directed(g.sym) && !grey;   // (every symbol on the map: the arrows, the face buttons, L1 / R1)
        const bool letter = !sym_arrow(g.sym);                  // a letter or the L / R tab: the chevrons give way over it
        const V3 d = arrow_dir(g.sym, ax), b = cross(ax.N, d);
        if (arrow) {   // the guide: the stroke's whole path, 12 cm beyond both edges (under the plate: the face hides it)
            const float L = rad + k_guide_m * r.g.size;
            odv::glow_line(o, C - d * L, C + d * L, 0.0025f * r.g.size, col(1.f, 1.f, 1.f, 0.35f * al), col(1.f, 1.f, 1.f, 0.35f * al), in.eye);
        }
        plate(o, C, ax, rad, th, rim, al);
        const V3 F = C + ax.N * (0.5f * th + 0.0008f);
        symbol(o, g.sym, F, ax.R, ax.U, sym_scale(g.sym, rad), ink_k, k_sym_w, glow_k);
        if (arrow) {   // three chevrons sliding along the cut, looping every 0.9 s, in the symbol's tint (thicker: 0.08 of the radius); they swell on a deflect
            const float L = rad + k_guide_m * r.g.size, big = defl ? 1.5f : 1.f;
            const float ph = static_cast<float>(std::fmod((t - r.t_open) / k_chev_period_s, 1.0));
            for (int j = 0; j < 3; ++j) {
                float q = ph + static_cast<float>(j) / 3.f; if (q >= 1.f) q -= 1.f;
                const float env = std::sin(3.14159265f * q), x = (-1.f + 2.f * q) * L;
                // on a letter or the tab, the chevrons give way where they cross the symbol (the letter stays the main thing to
                // read): while any part of one is over the face (its tip less than its depth past the rim's inner edge) it is at 20 %
                // alpha and 65 % size, full again by the rim's middle and beyond the plate (a lane around the letter); on an arrow
                // they run over it at full strength (they point the same way)
                float fa = 1.f, fs = 1.f;
                if (letter) { const float u2 = clampf((std::fabs(x) - 0.18f * rad * big - k_rim_in * rad) / (0.15f * rad), 0.f, 1.f), sm = u2 * u2 * (3.f - 2.f * u2); fa = 0.2f + 0.8f * sm; fs = 0.65f + 0.35f * sm; }
                const float a = al * fa * std::sqrt(env > 0.f ? env : 0.f);   // brighter for longer than the first build's plain sine
                chevron(o, F + ax.N * 0.0008f + d * x, d, b, 0.30f * rad * big * fs, 0.18f * rad * big * fs, 0.08f * rad * big * fs, a, ink_k);
            }
        }
    }
    /// The next symbol: small (40 %), faint, above and behind; never hittable.
    void draw_preview(int i, float A, Out& o) const {
        const Tgt& g = r.tg[i];
        const V3 P = preview_spot(i); const Axes3 ax = plate_axes(r.fr, P);
        const float rad = r.g.rad * k_preview_scale, th = r.g.th * k_preview_scale, al = 0.5f * A;    // a little brighter than the first build's 0.45, clearly dimmer than the plate (R6-N2: 0.65 competed)
        const Col k = r.stopped ? col(0.55f, 0.55f, 0.57f, 1.f) : sym_col(g.sym);
        halo(o, P - ax.N * th, ax, rad, 0.4f * al);
        plate(o, P, ax, rad, th, col(0.8f * k.r, 0.8f * k.g, 0.8f * k.b, 1.f), al);
        symbol(o, g.sym, P + ax.N * (0.5f * th + 0.0005f), ax.R, ax.U, sym_scale(g.sym, rad), bright(k, 0.85f * al), k_sym_w);
    }
    void draw_fx(const In& in, Out& o) const {
        const double t = in.t;
        for (const Cut& c : cut) {
            const double a = t - c.t0; if (!(a >= 0.0 && a < k_half_s)) continue;
            const float af = static_cast<float>(a), k = 1.f - static_cast<float>(a / k_half_s);
            for (int s = 0; s < 2; ++s) {
                const int m = c.hn[s]; if (m < 3) continue;
                const Col side = col(0.8f * c.k.r, 0.8f * c.k.g, 0.8f * c.k.b, 0.9f * k), hot = col(1.f, 1.f, 1.f, k);   // the plate's look: bright coloured edges, a dark face
                for (int j = 0; j < m; ++j) {
                    const int q = (j + 1) % m; const Col e = c.hcut[s][j] ? hot : side;
                    o.quad(half_v(c, s, j, af, -1.f), e, half_v(c, s, q, af, -1.f), e, half_v(c, s, q, af, 1.f), e, half_v(c, s, j, af, 1.f), e);
                }
                const V3 fc = half_c(c, s, af); const V3 N = rot_axis(c.ax.N, c.cd, c.tumble[s] * af);
                const Col fi = col(0.04f + 0.08f * c.k.r, 0.04f + 0.08f * c.k.g, 0.05f + 0.08f * c.k.b, k_face_a * k), fe = col(c.k.r, c.k.g, c.k.b, 0.95f * k);
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
            if (c.twin && a < k_twin_s) {   // a plate cut before it was fully seen: a ring flashes out from the spot in its colour
                const float u = static_cast<float>(a / k_twin_s);
                odv::arc(o, c.c, c.ax.R, c.ax.U, c.rad * (1.f + 0.6f * u), 0.014f * (c.rad / k_plate_r), 0.f, odg::k_two_pi, col(0.5f + 0.5f * c.k.r, 0.5f + 0.5f * c.k.g, 0.5f + 0.5f * c.k.b, 1.f - u), 24);
            }
            if (a < k_line_s) {
                const Col lc = col(1.f, 1.f, 1.f, 1.f - static_cast<float>(a / k_line_s));
                const V3 m = c.c + c.cn * c.off + c.ax.N * (0.5f * c.th + 0.001f);
                odv::glow_line(o, m - c.cd * (1.25f * c.rad), m + c.cd * (1.25f * c.rad), 0.0025f, lc, lc, in.eye);
            }
        }
    }
    /// Thin progress pips and a timer bar under the plate (and its halo); the pip whose progress just registered flashes.
    void draw_hud(const In& in, float A, Out& o) const {
        const double t = in.t; const float S = r.g.size;
        const V3 C = hud_centre(); const Axes3 ax = plate_axes(r.fr, C);   // between the two spots
        const float below = 1.4f * r.g.rad;
        const int n = r.n; const float sp = fminf(0.016f, 0.24f / static_cast<float>(n > 0 ? n : 1)) * S, w = 0.55f * sp;
        const double sa = r.end == E_SUCCESS ? t - r.t_end : 1e9; const bool success = sa >= 0.0 && sa < 0.25;
        for (int i = 0; i < n; ++i) {
            const V3 c = C + ax.U * (-(below + 0.012f * S)) + ax.R * ((static_cast<float>(i) - 0.5f * static_cast<float>(n - 1)) * sp);
            const bool lit = i < r.prog_shown || success;
            const double fa = t - r.flash_t; const bool fl = i == r.flash_i && fa >= 0.0 && fa < k_flash_s;
            const float g = fl ? 1.f + 0.9f * static_cast<float>(1.0 - fa / k_flash_s) : 1.f;
            const Col k = (success || fl) ? col(1.f, 1.f, 1.f, A) : lit ? col(0.85f, 0.97f, 1.f, 0.85f * A) : col(0.6f, 0.62f, 0.66f, 0.30f * A);
            odv::rect(o, c, ax.R, ax.U, -0.5f * w * g, -0.5f * w * g, 0.5f * w * g, 0.5f * w * g, k);
        }
        const float left = r.on ? r.left_now : (r.end == E_SUCCESS ? r.left_now : 0.f);
        const float frac = clampf(r.left0 > 0.f ? left / r.left0 : 0.f, 0.f, 1.f), W = 0.24f * S, H = 0.004f * S;
        const V3 bc = C + ax.U * (-(below + 0.024f * S));
        odv::rect(o, bc, ax.R, ax.U, -0.5f * W, -0.5f * H, 0.5f * W, 0.5f * H, col(0.f, 0.f, 0.f, 0.30f * A));
        const Col bk = left < 0.75f ? col(1.f, 0.32f, 0.25f, 0.85f * A) : left < 1.5f ? col(1.f, 0.72f, 0.22f, 0.85f * A) : col(0.95f, 0.95f, 0.95f, 0.7f * A);
        if (frac > 0.f) odv::rect(o, bc, ax.R, ax.U, -0.5f * W * frac, -0.5f * H, 0.5f * W * frac, 0.5f * H, bk);
        if (success) {   // success: a ring expands from the last plate's spot
            const float u = static_cast<float>(sa / 0.25); const V3 Cl = spot_i(r.n - 1); const Axes3 al = plate_axes(r.fr, Cl);
            odv::arc(o, Cl, al.R, al.U, r.g.rad * (1.f + 1.5f * u), 0.012f * S, 0.f, odg::k_two_pi, col(1.f, 0.95f, 0.8f, (1.f - u) * 0.9f), 24);
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
    /// This Present's triangles (the XR space of the poses; in: t, eye, head, hands): the preview, the current target, the cut's
    /// pieces, the pips and bar; the blades and trails last, with their reserve.
    int build(const In& in, Out& o) {
        const int n0 = o.n; const double t = in.t;
        const bool show = r.on || (r.end != E_NONE && t - r.t_end < k_end_s && t >= r.t_end);
        const float A = r.on ? 1.f : static_cast<float>(1.0 - (t - r.t_end) / k_end_s);
        const int keep = o.max; const int room = o.max - o.n;
        if (room > 2 * k_blade_reserve) o.max = keep - k_blade_reserve;
        const int d0 = o.dropped;
        if (show) {
            const int f = front();
            if (cfg.preview && f < r.n) { const int nx = next_after(f); if (nx >= 0) draw_preview(nx, A, o); }
            if (f < r.n) draw_current(f, t, A, in, o);
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
