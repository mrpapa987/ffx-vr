// SPDX-License-Identifier: MIT
// ffxvr_tag.h -- the battle swap "tag" (5 Nov 2026, the owner: "make it so the view switches when they kind of cross paths,
// almost like they are tagging in / out"). Pure logic: no Windows, no D3D, no logging, no allocation. A unit test drives it.
//
// A swap-in (a party member runs in from the bench to the slot of the one who runs out): managed used to keep the eyes on
// the outgoing character until the incoming one had run in and stood still (about 2 s in the 5 Nov log, watching him run
// off). Now managed feeds both characters' positions every frame of the swap (ffxvr_bt_tag_step) and switches the follow
// when the two cross: their horizontal distance is under Cfg::dist, or it has come down and grown again past its minimum
// by Cfg::hyst (they passed without coming that close). The first Cfg::warm frames, and any frame before both have moved
// Cfg::moved units from where they stood at the start, are ignored (a character standing still has not started).
// Then, in "tag and wait on the spot", the eye is held at the outgoing character's standing spot (Hold, ffxvr_bt_tag_hold)
// until the incoming one arrives: the follow (and the body hide) is already his, only the eye base is moved.
#pragma once
#include <cmath>
#include <cstdint>

namespace tag {

struct Cfg {
    float dist  = 6.f;   // ini bt_tag_dist: tag when the two are closer than this (horizontal units; party models are 15-22 tall)
    float hyst  = 2.f;   // "passed": closed in by at least this much, then grew this much past the minimum
    int   warm  = 4;     // frames ignored at the start of a swap
    float moved = 1.f;   // a character is moving once he is this far (horizontally) from where he stood at the start
};

enum Verdict : int { T_WAIT = 0, T_DIST, T_PASSED, T_BAD };
static const char* const k_verdict[] = { "waiting", "distance", "passed", "not readable" };

struct State {
    bool  on = false;        // a swap is being watched (begin had finite positions)
    int   frames = 0;        // steps since begin
    float o0[2] = {}, i0[2] = {};   // where each stood at the start (x, z)
    bool  o_moved = false, i_moved = false;
    bool  armed = false;     // past the warm-up with both moving: distances count
    float first = 0.f;       // the distance when armed
    float min = 0.f;         // the smallest distance since armed
    int   min_frame = 0;
    float d = 0.f;           // this step's distance
};

inline bool fin2(const float p[2]) { return std::isfinite(p[0]) && std::isfinite(p[1]); }
inline float hdist(const float a[2], const float b[2]) { const float dx = a[0] - b[0], dz = a[1] - b[1]; return std::sqrt(dx * dx + dz * dz); }

/// A swap begins: o = the outgoing character (the one followed), i = the incoming one, horizontal (x, z), any one space.
/// False (and nothing watched) when either is not finite.
inline bool begin(State& s, const float o[2], const float i[2]) {
    s = State();
    if (!fin2(o) || !fin2(i)) return false;
    s.on = true; s.o0[0] = o[0]; s.o0[1] = o[1]; s.i0[0] = i[0]; s.i0[1] = i[1];
    return true;
}

/// One frame of the swap. T_DIST / T_PASSED: tag now. T_WAIT: not yet. T_BAD: nothing watched, or a position not finite (the
/// caller gives up on the tag: it waits for the arrival as before).
inline int step(State& s, const float o[2], const float i[2], const Cfg& c) {
    if (!s.on || !fin2(o) || !fin2(i)) return T_BAD;
    s.frames++;
    s.d = hdist(o, i);
    if (hdist(o, s.o0) > c.moved) s.o_moved = true;
    if (hdist(i, s.i0) > c.moved) s.i_moved = true;
    if (!s.armed) {
        if (s.frames <= c.warm || !s.o_moved || !s.i_moved) return T_WAIT;
        s.armed = true; s.first = s.d; s.min = s.d; s.min_frame = s.frames;
    }
    if (s.d < s.min) { s.min = s.d; s.min_frame = s.frames; }
    if (s.d < c.dist) return T_DIST;
    if (s.first - s.min >= c.hyst && s.d > s.min + c.hyst) return T_PASSED;
    return T_WAIT;
}

/// ini bt_tag_dist as native takes it: 1..30 units, not finite -> the default.
inline float clamp_dist(float d) { return !std::isfinite(d) ? Cfg().dist : d < 1.f ? 1.f : d > 30.f ? 30.f : d; }

/// The eye held at a spot (render space) while the incoming character runs in. Managed refreshes it every frame; a hold not
/// refreshed for more than k_stale frames lapses on its own (the battle ended, the follow stopped, managed went away).
struct Hold {
    bool     on = false;
    uint32_t frame = 0;      // the last refresh (g_frames)
    float    base[3] = {};   // where the followed actor's position would have to be for the eye to stand on the spot
    uint32_t n_tags = 0;     // tags this session (rising edges)
};
const uint32_t k_stale = 2;
inline bool live(const Hold& h, uint32_t frame) { return h.on && frame - h.frame <= k_stale; }
/// on 1 = hold at b this frame; 2 = a tag without a hold (the eyes go straight to the incoming character); anything else =
/// release. Returns true on a tag, where the caller blinks and snaps the head offset: every on 2, and a hold that starts (it
/// was off or had lapsed). A hold with a base that is not finite releases instead (no tag).
inline bool set(Hold& h, int on, const float b[3], uint32_t frame) {
    const bool was = live(h, frame);
    if (on == 2) { h.on = false; h.n_tags++; return true; }
    if (on != 1 || b == nullptr || !(std::isfinite(b[0]) && std::isfinite(b[1]) && std::isfinite(b[2]))) { h.on = false; return false; }
    h.on = true; h.frame = frame; h.base[0] = b[0]; h.base[1] = b[1]; h.base[2] = b[2];
    if (was) return false;
    h.n_tags++;
    return true;
}
/// The eye base on the spot: P (the eye from the followed actor at pos, his own eye height and head offset) moved by
/// base - pos. First person only (fp_mode 1); the caller checks live() and the mode.
inline void apply(const Hold& h, const float pos[3], float P[3]) {
    for (int k = 0; k < 3; ++k) P[k] += h.base[k] - pos[k];
}

/// While the eye is held, the incoming character is hidden only near the spot (review of 1442290, SF: he was hidden all the
/// way in, so nobody seemed to arrive). k_near_extra: units beyond the usual hide radius + step margin, for where he stops
/// against the recorded spot (it is the outgoing one's position on the swap's first frame) and his body's width, so his head is
/// never drawn through the held eye on the last steps or where he stands.
const float k_near_extra = 2.f;
/// The battle body hide's centre (render space). Not live: pos (the followed actor's position, as always). Live: the spot,
/// where nothing stands (the outgoing one has left), so the runner shows; and pos again once he is within r_near of the spot
/// horizontally (r_near = the usual hide radius + step margin + k_near_extra), so the usual hide takes him in full.
inline const float* hide_centre(const Hold& h, bool live, const float pos[3], float r_near) {
    if (!live) return pos;
    const float dx = pos[0] - h.base[0], dz = pos[2] - h.base[2];
    return dx * dx + dz * dz < r_near * r_near ? pos : h.base;
}

}  // namespace tag
