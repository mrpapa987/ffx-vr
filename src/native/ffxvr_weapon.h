// SPDX-License-Identifier: MIT
#pragma once
// Weapon hands (the party's battle weapons in the VR hand): the pure parts, unit-tested in scratch (wpn_chk/wpn_test.cpp).
// This round is the LOG-ONLY capture (the wpn: lines, weapon_log.cs): the draw probe and the motion classes are used by it; the
// grip maths is the prototype of the next stage (nothing calls it in the game yet).
//
// Game facts (our exe, module offsets = VA - 0x400000; Fahrenheit's names where it has them, facts only):
//   Chr (MsGetChr 0x794020: [0xD334D4] + id * 0xF90, ids 0..0x1E):
//     +0x3F0 u8  the standard battle motion playing (written by the battle's motion setters 0x7A8FC0 and 0x7882C0 after
//                0x837D00(actor, Chr+0x20, motion) -> Ch_SetMotion 0x837AC0; 0xFF = none yet)
//     +0x3F1 u8  the idle motion in effect: what the request "motion 1" (the system idle) becomes (0x7A8C80 case 1; 0x10 at
//                set-up, the weak variants when the character is weak)
//   The numbering is the game's battle motion table (btlmot), as Fahrenheit's ids list it (mot_btl.cs).
#include <cmath>
#include <cstdint>
#include <cstring>

namespace wpn {

// -- the standard battle motions ----------------------------------------------------------------------------------------
enum Phase : int {
    PH_NONE = 0,    // 0xFF / 0: nothing set yet
    PH_IDLE,        // 0x01 the system idle, 0x10..0x12 idle 1-3
    PH_WEAK,        // 0x18..0x1A, 0x20..0x24: the weak (low HP / status) stances: still standing in place
    PH_GUARD,       // 0x28..0x2A (Defend) and 0x30..0x32 (provoked)
    PH_ATTACK,      // 0x02..0x06, 0x58..0x5D, 0x52 (steal)
    PH_MAGIC,       // 0x60..0x72
    PH_SKILL,       // 0x78..0x7A
    PH_ITEM,        // 0x88..0x8B
    PH_MOVE,        // 0x08 (appear), 0x0C..0x0D (return), 0x50..0x51 (run), 0x98..0x9C (escape, side steps, turns)
    PH_HURT,        // 0x38..0x43 (damage), 0x80..0x82 (avoid), 0xA8 / 0xAB (a nulled hit)
    PH_DEAD,        // 0x48..0x4C
    PH_WIN,         // 0x90..0x91
    PH_SLEEP,       // 0xA0..0xA3
    PH_SPECIAL,     // 0xB0 and up: the per-character sets (special attacks, overdrives)
    PH_OTHER,       // anything else
    PH_N
};
inline const char* phase_name(int p) {
    static const char* const k[PH_N] = { "none", "idle", "weak", "guard", "attack", "magic", "skill", "item", "move", "hurt", "dead", "win", "sleep", "special", "other" };
    return p >= 0 && p < PH_N ? k[p] : "?";
}
/// The class of a standard battle motion (Chr+0x3F0).
inline int phase(int m) {
    if (m <= 0 || m >= 0xFF) return PH_NONE;
    if (m == 0x01 || (m >= 0x10 && m <= 0x12)) return PH_IDLE;
    if ((m >= 0x02 && m <= 0x06) || (m >= 0x58 && m <= 0x5D) || m == 0x52) return PH_ATTACK;
    if (m == 0x08 || m == 0x0C || m == 0x0D || m == 0x50 || m == 0x51 || (m >= 0x98 && m <= 0x9C)) return PH_MOVE;
    if ((m >= 0x18 && m <= 0x1A) || (m >= 0x20 && m <= 0x24)) return PH_WEAK;
    if ((m >= 0x28 && m <= 0x2A) || (m >= 0x30 && m <= 0x32)) return PH_GUARD;
    if ((m >= 0x38 && m <= 0x43) || (m >= 0x80 && m <= 0x82) || m == 0xA8 || m == 0xAB) return PH_HURT;
    if (m >= 0x48 && m <= 0x4C) return PH_DEAD;
    if (m >= 0x60 && m <= 0x72) return PH_MAGIC;
    if (m >= 0x78 && m <= 0x7A) return PH_SKILL;
    if (m >= 0x88 && m <= 0x8B) return PH_ITEM;
    if (m == 0x90 || m == 0x91) return PH_WIN;
    if (m >= 0xA0 && m <= 0xA3) return PH_SLEEP;
    if (m >= 0xB0) return PH_SPECIAL;
    return PH_OTHER;
}
/// The candidate rule "the weapon may be in the VR hand": the character stands in an idle stance. motion = Chr+0x3F0,
/// idle = Chr+0x3F1 (the idle in effect; it counts only when it is itself an idle or weak stance, so a stale byte cannot hold
/// the weapon through an attack).
inline bool idle_like(int motion, int idle) {
    const int p = phase(motion);
    if (p == PH_IDLE || p == PH_WEAK) return true;
    if (motion != idle) return false;
    const int pi = phase(idle);
    return pi == PH_IDLE || pi == PH_WEAK;
}

// -- the draw probe (the capture: which draws belong to a weapon model, and what their constant buffer carries) ------------
constexpr int k_keys_max = 64;      // index-buffer keys (a weapon's segments) from managed
constexpr int k_recs_max = 40;      // draws kept for one frame
constexpr int k_rec_floats = 32;    // one record as floats (report)
struct Key { uint32_t ib, off, count; int32_t tag; };
enum : int { F_IMAGE = 1,        // the bound slot-0 buffer has this frame's eye image (the mod saw its upload)
             F_WORLD = 2,        // ... with a world matrix (the skinned layout: >= 1872 bytes)
             F_COUNT_DIFF = 4,   // the draw's index count is not the key's
             F_DEPTH = 8,        // a depth target was bound
             F_LIGHT = 16,       // the buffer's latest upload was a light camera (a shadow caster)
             F_PALETTE = 32 };   // the palette's first slot was read
struct Rec { int32_t tag, key, draw; uint32_t count, cb_size; int32_t flags, pal_n, pal_off; float w[12], p0[12]; };
struct Probe {
    Key k[k_keys_max]; int n = 0; uint32_t arm_frame = 0; bool armed = false;
    Rec r[k_recs_max]; int rn = 0; uint32_t r_frame = 0, r_total = 0; bool r_any = false;
    /// The keys, (buffer, byte offset, index count, tag) each; armed for frame `frame` and the one after. m <= 0 disarms.
    int set(const uint32_t* q, int m, uint32_t frame) {
        n = 0; armed = false;
        for (int i = 0; q != nullptr && i < m && n < k_keys_max; ++i) {
            const uint32_t* e = q + 4 * i;
            if (e[0] == 0) continue;
            k[n].ib = e[0]; k[n].off = e[1]; k[n].count = e[2]; k[n].tag = static_cast<int32_t>(e[3]); ++n;
        }
        if (n > 0) { armed = true; arm_frame = frame; }
        return n;
    }
    bool live(uint32_t frame) const { return armed && n > 0 && frame - arm_frame <= 1u; }
    /// The key of a draw (the buffer and the byte offset decide; of two entries the one with the draw's count), -1 none.
    int find(uint32_t ib, uint32_t off, uint32_t count, bool* count_ok) const {
        int hit = -1; bool c = false;
        for (int i = 0; i < n; ++i) {
            if (k[i].ib != ib || k[i].off != off) continue;
            if (k[i].count == count) { hit = i; c = true; break; }
            if (hit < 0) hit = i;
        }
        if (count_ok) *count_ok = c;
        return hit;
    }
    /// A matched draw of frame `frame`: the record to fill (zeroed but for key, tag, count, draw and the count flag), or null
    /// when the frame's list is full (it is still counted). The first draw of a new frame starts a new list.
    Rec* note(uint32_t frame, int key, uint32_t count, int draw, bool count_ok) {
        if (key < 0 || key >= n) return nullptr;
        if (!r_any || frame != r_frame) { r_any = true; r_frame = frame; rn = 0; r_total = 0; }
        ++r_total;
        if (rn >= k_recs_max) return nullptr;
        Rec* e = &r[rn++]; memset(e, 0, sizeof(*e));
        e->tag = k[key].tag; e->key = key; e->count = count; e->draw = draw; e->flags = count_ok ? 0 : F_COUNT_DIFF;
        return e;
    }
    /// The latest list as floats, k_rec_floats a record: tag, key, draw, count, buffer size, flags, palette slots, palette
    /// offset, the world matrix (3 x 4, rows, translation in column 3), the palette's first slot (3 x 4). Returns the
    /// records written; frame = the list's frame, total = its matched draws (may exceed the list).
    int report(float* out, int max_recs, uint32_t* frame, uint32_t* total) const {
        if (frame) *frame = r_any ? r_frame : 0u;
        if (total) *total = r_any ? r_total : 0u;
        if (!r_any || out == nullptr) return 0;
        int m = rn < max_recs ? rn : max_recs; if (m < 0) m = 0;
        for (int i = 0; i < m; ++i) {
            const Rec& e = r[i]; float* o = out + i * k_rec_floats;
            o[0] = static_cast<float>(e.tag); o[1] = static_cast<float>(e.key); o[2] = static_cast<float>(e.draw); o[3] = static_cast<float>(e.count);
            o[4] = static_cast<float>(e.cb_size); o[5] = static_cast<float>(e.flags); o[6] = static_cast<float>(e.pal_n); o[7] = static_cast<float>(e.pal_off);
            memcpy(o + 8, e.w, sizeof(e.w)); memcpy(o + 20, e.p0, sizeof(e.p0));
        }
        return m;
    }
};

// -- the grip maths (prototype) -----------------------------------------------------------------------------------------
// A frame "to the world": p' = R p + t, m = the 3 x 4 rows (R | t). R may carry one uniform scale (the game's root matrices
// do: model scale x actor scale).
struct Xf { float m[12]; };
inline Xf xf_identity() { Xf a; for (int i = 0; i < 12; ++i) a.m[i] = (i % 5 == 0) ? 1.f : 0.f; return a; }
/// a . b: b first, then a.
inline Xf xf_mul(const Xf& a, const Xf& b) {
    Xf o;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) o.m[r*4+c] = a.m[r*4+0]*b.m[0*4+c] + a.m[r*4+1]*b.m[1*4+c] + a.m[r*4+2]*b.m[2*4+c];
        o.m[r*4+3] = a.m[r*4+0]*b.m[3] + a.m[r*4+1]*b.m[7] + a.m[r*4+2]*b.m[11] + a.m[r*4+3];
    }
    return o;
}
inline void xf_point(const Xf& a, const float* p, float* o) { for (int r = 0; r < 3; ++r) o[r] = a.m[r*4+0]*p[0] + a.m[r*4+1]*p[1] + a.m[r*4+2]*p[2] + a.m[r*4+3]; }
/// The uniform scale of a frame (the mean length of R's columns); ok = finite, the columns of one length and square to each
/// other within tol (a rotation times one scale: the only kind this code moves).
inline float xf_scale(const Xf& a, bool* ok, float tol = 0.02f) {
    float len[3], dot[3];
    for (int c = 0; c < 3; ++c) len[c] = std::sqrt(a.m[c]*a.m[c] + a.m[4+c]*a.m[4+c] + a.m[8+c]*a.m[8+c]);
    const float s = (len[0] + len[1] + len[2]) / 3.f;
    for (int c = 0; c < 3; ++c) { const int d = (c + 1) % 3; dot[c] = a.m[c]*a.m[d] + a.m[4+c]*a.m[4+d] + a.m[8+c]*a.m[8+d]; }
    bool good = std::isfinite(s) && s > 1e-6f;
    for (int i = 0; good && i < 12; ++i) good = std::isfinite(a.m[i]);
    for (int c = 0; good && c < 3; ++c) good = std::fabs(len[c] - s) <= tol * s && std::fabs(dot[c]) <= tol * s * s;
    if (ok) *ok = good;
    return s;
}
/// The inverse of a rotation-times-uniform-scale frame. False (and the identity) when it is not one.
inline bool xf_inverse(const Xf& a, Xf* o) {
    bool ok = false; const float s = xf_scale(a, &ok);
    if (!ok) { *o = xf_identity(); return false; }
    const float k = 1.f / (s * s);
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) o->m[r*4+c] = a.m[c*4+r] * k;   // R^-1 = R^T / s^2
    for (int r = 0; r < 3; ++r) o->m[r*4+3] = -(o->m[r*4+0]*a.m[3] + o->m[r*4+1]*a.m[7] + o->m[r*4+2]*a.m[11]);
    return true;
}
/// The game's actor matrices (root +0x1D0, bones) are 4 x 4 for row vectors (v' = v M: the axes in rows 0..2, the translation
/// in floats 12..14): the same frame as a column-convention Xf. t_scale multiplies the translation (1000 for a bone record's
/// matrix: 0x8350F0 builds the sub-positions that way).
inline Xf xf_from_rows(const float* g16, float t_scale = 1.f) {
    Xf o;
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) o.m[r*4+c] = g16[c*4+r]; o.m[r*4+3] = g16[12+r] * t_scale; }
    return o;
}
/// Game space -> render space (x and y negated): S A S for the rotation, S t for the translation.
inline Xf xf_to_render(const Xf& a) {
    Xf o = a; static const float s[3] = { -1.f, -1.f, 1.f };
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) o.m[r*4+c] = s[r] * a.m[r*4+c] * s[c]; o.m[r*4+3] = s[r] * a.m[r*4+3]; }
    return o;
}
/// The grip: the weapon's frame seen from the hand's (G = hand^-1 . weapon). Constant while the weapon is carried rigidly.
inline bool grip_of(const Xf& hand, const Xf& weapon, Xf* g) {
    Xf hi; if (!xf_inverse(hand, &hi)) { *g = xf_identity(); return false; }
    *g = xf_mul(hi, weapon); return true;
}
/// The move that takes everything drawn with the game's hand to the VR hand: D = vr_hand . game_hand^-1, applied on the left
/// of every world-space matrix of the weapon's draws (its world matrix and each palette slot). D . weapon = vr_hand . G, so no
/// per-weapon number is needed. size (1 = as the game draws it) scales the weapon about the VR hand's origin.
inline bool redirect(const Xf& vr_hand, const Xf& game_hand, float size, Xf* d) {
    Xf gi; if (!xf_inverse(game_hand, &gi) || !(size > 0.f) || !std::isfinite(size)) { *d = xf_identity(); return false; }
    bool ok = false; xf_scale(vr_hand, &ok); if (!ok) { *d = xf_identity(); return false; }
    Xf v = vr_hand;
    if (size != 1.f) for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) v.m[r*4+c] *= size;
    *d = xf_mul(v, gi); return true;
}
/// A hand's frame from three points of it (any space): the wrist, the base of the index finger and the base of the little
/// finger (or of the joined fingers). Origin = the wrist; -Z toward the fingers (the mean of the two bases), X from the little
/// finger's side to the index side (squared to Z), Y = Z x X. The same rule on the game's hand joints and on the VR hand
/// gives two frames that mean the same thing, whatever the bones' own axes are. False when the points do not span a hand.
inline bool hand_frame(const float* wrist, const float* index_base, const float* little_base, Xf* o) {
    float f[3], x[3];
    for (int i = 0; i < 3; ++i) { f[i] = 0.5f * (index_base[i] + little_base[i]) - wrist[i]; x[i] = index_base[i] - little_base[i]; }
    const float fl = std::sqrt(f[0]*f[0] + f[1]*f[1] + f[2]*f[2]);
    if (!(fl > 1e-5f) || !std::isfinite(fl)) { *o = xf_identity(); return false; }
    for (float& v : f) v /= fl;
    const float d = x[0]*f[0] + x[1]*f[1] + x[2]*f[2];
    for (int i = 0; i < 3; ++i) x[i] -= d * f[i];
    const float xl = std::sqrt(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);
    if (!(xl > 1e-5f) || !std::isfinite(xl)) { *o = xf_identity(); return false; }
    for (float& v : x) v /= xl;
    const float z[3] = { -f[0], -f[1], -f[2] };
    const float y[3] = { z[1]*x[2] - z[2]*x[1], z[2]*x[0] - z[0]*x[2], z[0]*x[1] - z[1]*x[0] };
    for (int r = 0; r < 3; ++r) { o->m[r*4+0] = x[r]; o->m[r*4+1] = y[r]; o->m[r*4+2] = z[r]; o->m[r*4+3] = wrist[r]; }
    return true;
}

}  // namespace wpn
