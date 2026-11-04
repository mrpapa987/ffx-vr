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
//      about 0), so it is never taken for the player's however wide the margin is. Nor is a draw within k_own of the player
//      (review N3): while his root is known to within 0.75 his body does not flash on because another root is close. (With
//      both his roots a tick off what is drawn, no snapshot, it still can: all his parts share one translation, so they are
//      refused together and his whole body shows for that frame. The safer failure.)
//      The player's parts sit on his drawn root, +0x200 = +0x0C + chr_offset (+0x6C). The hide centre is that root minus
//      chr_offset (the smooth anchor's snapshot) or the 30 Hz +0x0C: chr_offset moves the centre off his parts, never the root.
//      So when the root is the snapshot's (self_exact: what this frame draws) the player's distance is to it alone (review N2);
//      when it is a Present-time read (the lerp the last frame drew or the canonical tick, up to a tick off) it is the smaller
//      of the distances to it and to the centre, whichever is nearer what is drawn.
// No other roots (none sent this frame) = test 1 only, as before. Riding is the chocobo rule, unchanged (rider / mount roots).
// A draw that test 2 leaves tied (another root as near as his: a shared scene anchor) is resolved by model identity (below).
#pragma once
#include <cmath>
#include <cstdint>

namespace fpown {

constexpr float k_margin_cap = 4.f;   // units: the step margin's cap (the fixed part, hide_radius, is not included)
constexpr float k_tol        = 0.5f;  // units: another root must be this much nearer than the player's to take the draw
constexpr float k_own        = 0.75f; // units: a draw this near the player's (his distance) is never refused (review N3)
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
    bool self_exact = false;                // self is the smooth anchor's post-join snapshot (used alone), not a Present-time read
    const float (*others)[3] = nullptr;     // other live actors' drawn roots
    int n = 0;                              // how many; <= 0 = none sent: the old behaviour
};

/// What decided (for the diagnostics; filled only for a draw inside the radius).
struct Why {
    float d_centre = 0.f;   // distance to the hide centre
    float d_player = 0.f;   // the player's distance (to the exact drawn root; else the min of centre and drawn root)
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
    if (f.self) { const float ds2 = d2(w, f.self); if (f.self_exact || ds2 < dp2) dp2 = ds2; }
    const float dp = std::sqrt(dp2);
    float bo2 = 1e30f; int bo = -1;
    for (int i = 0; i < f.n && i < k_others_max; ++i) { const float o2 = d2(w, f.others[i]); if (o2 < bo2) { bo2 = o2; bo = i; } }
    // refused iff d_player > k_own and d_other + tol < d_player (a tie, within the tolerance or within k_own of him is his)
    const float lim = dp - k_tol;
    const bool refused = bo >= 0 && dp > k_own && lim > 0.f && bo2 < lim * lim;
    if (why) { why->d_centre = std::sqrt(dc2); why->d_player = dp; why->d_other = bo >= 0 ? std::sqrt(bo2) : -1.f; why->other = bo; why->refused = refused; }
    return refused ? 0 : 1;
}

// ---- Shared-anchor ties: model identity (4 Nov 2026, mi/mihn0100) --------------------------------------------------------
// In an event scene two actors can share one root exactly: the event Tidus (c101) and the event Yuna (c102) both stand on the
// scene anchor and their animations place the bodies away from it through the bone palette. Every draw of either then has the
// same world translation, ties with the player's root, and the tie goes to the player: Yuna was hidden with him. Geometry of
// the world translation cannot tell them apart, so a tied draw is resolved by the model it belongs to:
//   1. the model key is the draw's pixel-shader slot-0 texture (a character's atlas: every part of one model samples it, two
//      models never share one; identical NPC models share theirs, which is harmless: they are one kind of NPC);
//   2. a tied draw's bone palette (world space, M = W_joint x Bind^-1, the bind pose on the model's axis, y down, the head at
//      about y = -14.5 .. -18.2) puts its head points somewhere; the distance from them to the player's head sub-position and to
//      the tied actor's head sub-position (each built by the game from that actor's own skeleton) says whose head it is: the
//      nearest model points per frame and texture, compared with a margin, step a per-texture score;
//   3. a texture matched as the player's with no other root within k_clear (normal play) is the player's at once.
// A tied draw whose texture scores as another actor's is kept visible; one that scores as the player's, or not at all (no
// palette, no heads, no evidence yet), is hidden as before: the identity only ever turns a hide into a show, never the reverse.
// The table is cleared on a re-pick (a new key) and entries expire unseen. Pure: the native glue feeds it, a test drives it.

constexpr float    k_clear       = 3.f;    // units: a matched draw with no other root within d_player + this is unambiguously his
constexpr float    k_head_match  = 3.f;    // units: a head this near the model's head points is a match
constexpr float    k_head_margin = 1.5f;   // units: ... and nearer it than the other head by this much
constexpr float    k_head_far    = k_head_match + k_head_margin;   // units: another actor's only when the player's head is this far from the model
constexpr int      k_models      = 8;      // textures remembered
constexpr int      k_s_max       = 8;      // the score's clamp (player side)
constexpr int      k_s_min       = -4;     // ... and the other side (review SF1: a wrong "other" must not need many frames to undo)
constexpr int      k_s_player    = 2;      // score >= this: the player's model
constexpr int      k_s_other     = -3;     // score <= this: another actor's (three frames of evidence from an unknown start)
constexpr uint32_t k_model_ttl   = 600;    // frames unseen before an entry is dropped
constexpr float    k_head_y[4]   = { -14.5f, -15.8f, -17.0f, -18.2f };   // bind-space points on the model's axis: neck .. crown

/// A matched draw is tied when another root is within the tolerance of the player's (not refused, and no nearer than that).
inline bool tied(const Why& y) { return y.other >= 0 && !y.refused && y.d_other >= 0.f && y.d_other <= y.d_player + k_tol; }
/// A matched draw is unambiguous when no other root is within d_player + k_clear (none sent: other < 0).
inline bool clear_of_others(const Why& y) { return y.other < 0 || y.d_other > y.d_player + k_clear; }

/// The nearest of a palette's head points to h: min over slots j and the k_head_y points b = (0, y, 0) of |M_j b - h|.
/// pal = n row-major 4x4 matrices (rows 0..2 read, translation in column 3). -1 when there is nothing to test.
inline float head_dist(const float* pal, int n, const float* h) {
    if (pal == nullptr || h == nullptr || n <= 0) return -1.f;
    float best2 = 1e30f;
    for (int j = 0; j < n && j < 80; ++j) {
        const float* M = pal + 16 * j;
        for (float by : k_head_y) {
            const float x = M[1] * by + M[3] - h[0], y = M[5] * by + M[7] - h[1], z = M[9] * by + M[11] - h[2];
            const float e2 = x * x + y * y + z * z;
            if (e2 < best2) best2 = e2;
        }
    }
    return best2 < 1e29f ? std::sqrt(best2) : -1.f;
}
/// One frame's evidence for a texture: +1 its head points match the player's head, -1 the tied actor's, 0 neither or both.
/// "Another actor's" also needs the player's head well away from every head point of the model (k_head_far): the safer side,
/// since a wrong -1 shows the player's body. The test (anchor_test) keeps Tidus hidden with the head sub-position up to 6 units
/// off the bind axis points (the game's is the neck joint, at the second point); without k_head_far, from about 4.
inline int head_evidence(float dp, float dq) {
    if (!(dp >= 0.f) || !(dq >= 0.f)) return 0;
    if (dp < k_head_match && dp + k_head_margin < dq) return 1;
    if (dq < k_head_match && dq + k_head_margin < dp && dp > k_head_far) return -1;
    return 0;
}

struct Model {
    uintptr_t tex = 0;          // the key (0 = a free entry)
    int       s = 0;            // the score: >= k_s_player the player's, <= k_s_other another actor's
    uint32_t  seen = 0;         // the last frame it was noted, learned or looked up in a tie
    float     dp = 1e30f, dq = 1e30f;   // this frame's nearest head distances (player, other) over its tied draws
    bool      voted = false;    // this frame has head distances
    float     last_dp = -1.f, last_dq = -1.f;   // the last frame's (the diagnostics)
};
struct Models {
    Model e[k_models];
    void clear() { for (Model& m : e) m = Model{}; }
    Model* find(uintptr_t t) { if (t == 0) return nullptr; for (Model& m : e) if (m.tex == t) return &m; return nullptr; }
    const Model* find(uintptr_t t) const { if (t == 0) return nullptr; for (const Model& m : e) if (m.tex == t) return &m; return nullptr; }
    Model* take(uintptr_t t, uint32_t frame) {
        if (t == 0) return nullptr;
        if (Model* m = find(t)) return m;
        Model* v = &e[0];
        for (Model& m : e) { if (m.tex == 0) { v = &m; break; } if (frame - m.seen > frame - v->seen) v = &m; }   // a free one, else the oldest
        *v = Model{}; v->tex = t; v->seen = frame;
        return v;
    }
    /// A tied draw's head distances (either < 0: not known, nothing noted).
    void note(uintptr_t t, float dp, float dq, uint32_t frame) {
        if (!(dp >= 0.f) || !(dq >= 0.f)) return;
        Model* m = take(t, frame); if (m == nullptr) return;
        m->seen = frame; m->voted = true; if (dp < m->dp) m->dp = dp; if (dq < m->dq) m->dq = dq;
    }
    /// A draw matched as the player's with no other root near (clear_of_others): his model.
    void learn(uintptr_t t, uint32_t frame) { Model* m = take(t, frame); if (m == nullptr) return; m->seen = frame; if (m->s < k_s_player) m->s = k_s_player; }
    /// Frame end: this frame's head distances step each score; unseen entries expire.
    void frame_end(uint32_t frame) {
        for (Model& m : e) {
            if (m.tex == 0) continue;
            if (m.voted) {
                int ev = head_evidence(m.dp, m.dq);
                // review SF1: the recovery term. A frame with the player's head on this model undoes an "other" score step by
                // step even while the other head is as near (no margin): a 3-frame transient cannot latch his body visible.
                if (ev == 0 && m.s < 0 && m.dp < k_head_match) ev = 1;
                m.s += ev; if (m.s > k_s_max) m.s = k_s_max; if (m.s < k_s_min) m.s = k_s_min;
                m.last_dp = m.dp; m.last_dq = m.dq; m.dp = m.dq = 1e30f; m.voted = false;
            }
            else if (frame - m.seen > k_model_ttl) m = Model{};
        }
    }
    /// 1 the player's model, -1 another actor's, 0 not known.
    int verdict(uintptr_t t) const { const Model* m = find(t); if (m == nullptr) return 0; return m->s >= k_s_player ? 1 : m->s <= k_s_other ? -1 : 0; }
};

// ---- Exact identity: the index buffer (npc-anchor round 2, research/fh-structs.md 1a, checked in the disassembly) ---------------
// Phyre draws every character segment with 0x592C60: IASetIndexBuffer(block->buffers[k].+0, R16/R32, block->+0x2C byte offset)
// then DrawIndexed(count, 0, 0). The block is the segment's index data (PMeshSegmentD3D11 +0x30), reached from an actor through
// +0x830 (model object: +0 count, +4 PMeshInstance*[]) -> instance +0 PMesh -> +0 count, +4 segments (0x6C each). Managed walks
// the followed actor's chain and those of the actors near him and sends (buffer, byte offset, index count, owner) entries: a
// matched draw whose bound index buffer and offset are in the table belongs to that owner exactly. Two actors of one model share
// the mesh (the same keys): such a key has both owners and decides nothing (the other rules then apply). Pure: the native glue
// queries IAGetIndexBuffer for matched draws only; a test drives this with fake keys.
constexpr int k_ib_max = 192;   // entries kept (a character has about 13-20 segments)
struct IbEntry { uint32_t ib = 0, off = 0, count = 0; int32_t owner = -1; };   // owner 0 the player's, 1 another actor's
struct IbTable {
    IbEntry e[k_ib_max]; int n = 0;
    /// Replaces the table from (ib, off, count, owner) quadruples; malformed owners are dropped. Returns the count kept.
    int set(const uint32_t* q, int m) {
        n = 0;
        for (int i = 0; q != nullptr && i < m && n < k_ib_max; ++i) {
            const uint32_t* r = q + 4 * i;
            if (r[0] == 0 || r[3] > 1u) continue;
            e[n].ib = r[0]; e[n].off = r[1]; e[n].count = r[2]; e[n].owner = static_cast<int32_t>(r[3]); ++n;
        }
        return n;
    }
    /// The owner of a draw: 0 the player's, 1 another actor's, -1 not in the table, -2 both (identical models: undecided).
    /// count_ok (if not null) says whether an entry with this key also has the draw's index count (diagnostics only: the key
    /// is the buffer and the byte offset, which the segment owns whatever count the caller draws).
    int owner(uint32_t ib, uint32_t off, uint32_t count, bool* count_ok = nullptr) const {
        bool p = false, o = false, c = false;
        for (int i = 0; i < n; ++i) {
            if (e[i].ib != ib || e[i].off != off) continue;
            (e[i].owner == 0 ? p : o) = true; if (e[i].count == count) c = true;
        }
        if (count_ok) *count_ok = c;
        return p && o ? -2 : p ? 0 : o ? 1 : -1;
    }
};

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
