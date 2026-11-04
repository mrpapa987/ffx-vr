// SPDX-License-Identifier: MIT
// ffxvr_fpown.h -- the first-person body hide's ownership test (NPCs vanishing in first-person cutscenes, 4 Nov 2026): which
// skinned draws are the player's own parts. Pure logic: no Windows, no D3D, no logging. ffxvr_native.cpp (fp_part_of) feeds it
// the draw's world translation and the frame's roots; a unit test drives it with synthetic positions. Render space throughout.
//
// On foot a draw is the player's when
//   1. it is within radius + margin of the hide centre (g_fp_pos, the smooth anchor's position), margin = min(1.5 x step_hide,
//      k_margin_cap): the old test, with the step margin capped (it was unbounded: a scripted move under the 50-unit warp rule, a
//      re-pick of the followed actor or 30 fps logic widened it to several units and NPCs beside Tidus went with him); and
//   2. it is not nearer another actor's drawn root than the player's, by k_tol: an NPC's parts sit on its own root (distance
//      about 0), so it is never taken for the player's however wide the margin is. The player's distance is the smaller of the
//      distances to the hide centre and to his drawn root (+0x200; the hide centre is drawn - chr_offset +0x6C, or the 30 Hz
//      logic position without the smooth anchor): his parts sit on one of the two.
// No other roots (none sent this frame) = test 1 only, as before. Riding is the chocobo rule, unchanged (rider / mount roots).
#pragma once
#include <cmath>

namespace fpown {

constexpr float k_margin_cap = 4.f;   // units: the step margin's cap (the fixed part, hide_radius, is not included)
constexpr float k_tol        = 0.5f;  // units: another root must be this much nearer than the player's to take the draw
constexpr int   k_others_max = 32;    // other actors' roots per frame (managed sends those within ~30 units, nearest first)

/// The step margin: 1.5 x step_hide, capped; 0 for a negative or NaN step.
inline float margin(float step_hide) {
    const float m = 1.5f * step_hide;
    return m > 0.f ? (m < k_margin_cap ? m : k_margin_cap) : 0.f;
}

inline float d2(const float* a, const float* b) { const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return x*x + y*y + z*z; }

struct Foot {
    const float* centre = nullptr;          // the hide centre (g_fp_pos)
    float radius = 0.75f;                   // g_fp_hide_radius
    float step_hide = 0.f;                  // g_fp_step_hide (SM-2)
    const float* self = nullptr;            // the followed actor's drawn root, or nullptr (then the centre only)
    const float (*others)[3] = nullptr;     // other live actors' drawn roots
    int n = 0;                              // how many; <= 0 = none sent: the old behaviour
};

/// What decided (for the diagnostics; filled only for a draw inside the radius).
struct Why {
    float d_centre = 0.f;   // distance to the hide centre
    float d_player = 0.f;   // the player's distance (min of centre and drawn root)
    float d_other = -1.f;   // the nearest other root's distance (-1: none sent)
    int   other = -1;       // its index in others
    bool  refused = false;  // inside the radius, but nearer another root: not the player's
};

/// On foot: 1 the player's part, 0 not (outside the radius, or another actor's: why->refused).
inline int on_foot(const float* w, const Foot& f, Why* why = nullptr) {
    const float r0 = f.radius + margin(f.step_hide);
    const float dc2 = d2(w, f.centre);
    if (!(dc2 < r0 * r0)) return 0;
    if (f.n <= 0 || f.others == nullptr) {
        if (why) { *why = Why{}; why->d_centre = std::sqrt(dc2); why->d_player = why->d_centre; }
        return 1;
    }
    float dp2 = dc2;
    if (f.self) { const float ds2 = d2(w, f.self); if (ds2 < dp2) dp2 = ds2; }
    const float dp = std::sqrt(dp2);
    float bo2 = 1e30f; int bo = -1;
    for (int i = 0; i < f.n && i < k_others_max; ++i) { const float o2 = d2(w, f.others[i]); if (o2 < bo2) { bo2 = o2; bo = i; } }
    // refused iff d_other + tol < d_player (a tie, or within the tolerance, is the player's: his parts never flicker on)
    const float lim = dp - k_tol;
    const bool refused = bo >= 0 && lim > 0.f && bo2 < lim * lim;
    if (why) { why->d_centre = std::sqrt(dc2); why->d_player = dp; why->d_other = bo >= 0 ? std::sqrt(bo2) : -1.f; why->other = bo; why->refused = refused; }
    return refused ? 0 : 1;
}

/// Riding (the chocobo rule, ffxvr_ride_set): within max(hide_radius, ride_radius) of the rider's drawn root and not nearer the
/// mount's root than the rider's = 1; within that radius of the mount's root = 2 (its parts); else 0. flags 2 = the mount is known.
inline int riding(const float* w, int flags, const float* rider, const float* mount, float hide_radius, float ride_radius) {
    const float r = std::fmax(hide_radius, ride_radius);
    const float dr = d2(w, rider), dm = (flags & 2) ? d2(w, mount) : 1e30f;
    if (dr < r * r && dr <= dm) return 1;
    if (dm < r * r) return 2;
    return 0;
}

}  // namespace fpown
