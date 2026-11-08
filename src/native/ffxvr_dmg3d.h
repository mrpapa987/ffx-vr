// SPDX-License-Identifier: MIT
// ffxvr_dmg3d.h -- 3D damage numbers (roadmap 4.2, 6 Nov 2026). Pure logic: no Windows, no D3D, no logging, no allocation.
// A unit test drives it (node parsing, the chain walk, the game's placement and fade, the queue check, the digit layout and
// UVs, the anchor maths, the lifetime rule, the number grouping for the log).
//
// The game (our exe, CHECKED 5-6 Nov): MsNumberRegist 0x79FA00 makes one node per digit (0x79FC60) in a pool of 128 x 0x14 B at
// [0x11360B8] (a singly linked list from [0x11360BC], link at +0x10). Every logic tick 0x79F5E0 counts each node's start delay
// down and its frame counter up and frees it after `len` (30) frames. Once per drawn frame, in the battle UI draw 0x89B360,
// 0x79F380 projects each Chr's damage anchor (optpos[Chr+0x442], or 0x794AB0(chr) when Chr+0x442 is 0xFF: anchor_794ab0
// below; then lifted by Chr+0x4E8) to PS2 px (Chr+0xF44/F48, clamped copy F4C/F50), 0x79F660 turns every node whose delay is 0 into a popup-queue entry at that anchor plus the node's offset and a
// hop from the table at 0xC424B0, and 0x893290 draws the queue (digits from battle.dds at 34 x 45 UI px) and empties it.
// We read the same nodes at the same point of the frame (managed: the job join right after that draw, or the queue draw's
// own detour when dmg3d_hide2d is on) and draw each digit as a head-facing quad from battle.dds at its target, with the
// game's own hop and fade, in both eyes (native: the marker pass at Present).
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dmg {

constexpr uint32_t k_node_size = 0x14, k_pool_n = 128, k_pool_bytes = k_node_size * k_pool_n;   // 0xA00 (0x79F9B0's alloc)
constexpr uint32_t k_link_off = 0x10;                 // node +0x10: the next node (absolute address; 0 ends the list)
constexpr int k_tab_n = 32;                           // the hop table: 32 frames of (dx, dy, alpha) at 0xC424B0 (len 30 fits)
constexpr int k_q_cap = 63;                           // the popup queue (0x899A40 refuses a 64th)
constexpr int k_max = 64;                             // drawn glyphs at most (ffxvr_dmg_set); the pool holds 128 nodes
constexpr int k_chr_max = 16;                         // distinct targets per snapshot at most
constexpr float k_ui_per_ps2_x = 1920.f / 512.f;      // the PS2 space (512 x 416) covers the whole 1920 x 1080 UI space
constexpr float k_ui_per_ps2_y = 1080.f / 416.f;      // (0x79F380: F3C = F34 / (w / 512) + 256, F40 = F38 / (h / 416) + 208)
constexpr float k_glyph_w = 34.f, k_glyph_h = 45.f;   // UI px (0x8FB840: 0x6447E0(34) = 34 * 512 / 1920, 0x644820(45) = 45 * 416 / 1080)
constexpr float k_px_per_unit = 14.f;                 // C3's measured scale (the game's UI px per world unit at its default battle view)
constexpr uint8_t k_self = 8;                        // Chr::aflags: the actor first person looks out of
constexpr float k_min_rad = 1.0f * 3.14159265f / 180.f, k_max_rad = 4.0f * 3.14159265f / 180.f;   // the digit height is kept between these at size 1

#pragma pack(push, 1)
/// One pool node, as 0x79FC60 writes it (mirrored in dmg3d.cs, 16 B). slot = its index in the pool (k of base + k * 0x14).
struct Node { uint8_t flags, kind, chr, idx, glyph, delay, counter, len; int16_t ox, oy; uint8_t ex, ey, slot, pad; };
/// One target, as managed reads it on the main thread (mirrored in dmg3d.cs, 72 B). Game space (y down).
struct Chr {
    float opt[3], tl[3], td[3];   // the sub-position the game anchors at (d7_anchor's rule), the logic root, the drawn root
    float h4e8;                   // Chr+0x4E8: the damage-number height (the game subtracts it from y: up)
    int32_t sx, sy;               // Chr+0xF34 / F38: the cursor anchor on screen, UI units (log only)
    int32_t f44, f48, f4c, f50;   // Chr+0xF44 / F48: the damage anchor on screen, PS2 px; F4C / F50 its clamped copy (flag 0x80)
    uint8_t chr, idx442;          // the chr index; Chr+0x442 (0xFF: the game anchors at 0x794AB0 instead)
    uint8_t src;                  // 0 the game's own rule (optpos[Chr+0x442], or the 0x794AB0 port for 0xFF: how != 0); 1 a stand-in
                                  // (optpos[1] for 0xFF when the port's reads failed, an index above 25, d7_anchor's fallback); 2 no anchor
    uint8_t aflags;               // d7_anchor's flags: 1 attached, 2 override matrix (no 30 Hz correction then), 4 index outside 0..25;
                                  // k_self (8, ours): first person looks out of this actor (C3's rule): its digits go to the self spot
    uint8_t ok;                   // ok: the read succeeded (src 0 or 1)
    uint8_t how;                  // Chr+0x442 == 0xFF: anchor_794ab0's result (AB_*; 0 = not ported, opt is optpos[1]). Log only
    uint8_t pad[2];
};
/// A popup-queue entry (0x899A40; 16 B at 0x133F110 + 16 * i).
struct Entry { int16_t x, y; int32_t kind, glyph; uint8_t r, g, b, a; };
#pragma pack(pop)
static_assert(sizeof(Node) == 16, "Node is mirrored in dmg3d.cs");
static_assert(sizeof(Chr) == 72, "Chr is mirrored in dmg3d.cs");
static_assert(sizeof(Entry) == 16, "Entry is the game's layout");

/// The hop table as read from our exe on 6 Nov (VA 0xC424B0, 32 x {dx, dy, alpha}): a 12 px hop with a fade-in, a 3 px
/// bounce, then rest at alpha 0x80. The live bytes are read at launch; a difference is logged (another exe).
static const uint8_t k_table_checked[96] = {
    0, 0, 32,   0, 3, 60,   0, 6, 84,   0, 8, 104,  0, 10, 120, 0, 11, 128, 0, 12, 128, 0, 11, 128,
    0, 10, 128, 0, 8, 128,  0, 6, 128,  0, 3, 128,  0, 0, 128,  0, 2, 128,  0, 3, 128,  0, 2, 128,
    0, 0, 128,  0, 1, 128,  0, 0, 128,  0, 0, 128,  0, 0, 128,  0, 0, 128,  0, 0, 128,  0, 0, 128,
    0, 0, 128,  0, 0, 128,  0, 0, 128,  0, 0, 128,  0, 0, 128,  0, 0, 128,  0, 0, 128,  0, 0, 128 };

struct Table { int8_t dx[k_tab_n], dy[k_tab_n]; uint8_t a[k_tab_n]; };
inline Table table_from(const uint8_t* t) {
    Table T; if (!t) t = k_table_checked;
    for (int j = 0; j < k_tab_n; ++j) { T.dx[j] = static_cast<int8_t>(t[3 * j]); T.dy[j] = static_cast<int8_t>(t[3 * j + 1]); T.a[j] = t[3 * j + 2]; }
    return T;
}
inline bool table_is_checked(const uint8_t* t) { return t && memcmp(t, k_table_checked, sizeof(k_table_checked)) == 0; }

// ---- the pool --------------------------------------------------------------------------------------------------------
inline Node parse(const uint8_t* p, int slot) {
    Node n;
    n.flags = p[0]; n.kind = p[1]; n.chr = p[2]; n.idx = p[3]; n.glyph = p[4]; n.delay = p[5]; n.counter = p[6]; n.len = p[7];
    n.ox = static_cast<int16_t>(p[8] | (p[9] << 8)); n.oy = static_cast<int16_t>(p[10] | (p[11] << 8));
    n.ex = p[12]; n.ey = p[13]; n.slot = static_cast<uint8_t>(slot); n.pad = 0;
    return n;
}
inline uint32_t rd32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }

enum WalkWhy : uint32_t { W_LOOP = 1, W_OUTSIDE = 2, W_MISALIGNED = 4, W_CAP = 8, W_FREE = 16, W_FULL = 32, W_NOPOOL = 64 };
/// Walks the list in a copy of the pool (k_pool_bytes from `base`), from `head`, in list order. Every link must be base + k *
/// 0x14 with k < 128, every node live (byte +0 != 0) and seen once; at most 128 steps. A torn, freed or looped chain ends the
/// walk there (why says how). Returns the nodes written to out (at most max).
inline int walk(const uint8_t* pool, uint32_t base, uint32_t head, Node* out, int max, uint32_t* why) {
    uint32_t w = 0; int n = 0;
    if (!pool || base == 0) { if (why) *why = head ? static_cast<uint32_t>(W_NOPOOL) : 0u; return 0; }
    uint8_t seen[k_pool_n] = {};
    uint32_t cur = head;
    for (uint32_t step = 0; cur != 0; ++step) {
        if (step >= k_pool_n) { w |= W_CAP; break; }
        const uint32_t off = cur - base;
        if (off >= k_pool_bytes) { w |= W_OUTSIDE; break; }
        if (off % k_node_size != 0) { w |= W_MISALIGNED; break; }
        const uint32_t k = off / k_node_size;
        if (seen[k]) { w |= W_LOOP; break; }
        seen[k] = 1;
        const uint8_t* p = pool + off;
        if (p[0] == 0) { w |= W_FREE; break; }
        if (n < max) out[n++] = parse(p, static_cast<int>(k)); else w |= W_FULL;
        cur = rd32(p + k_link_off);
    }
    if (why) *why = w;
    return n;
}

/// 0x79F660's second loop draws a node once its start delay is 0. The frame counter is >= 1 by then (the update counts it up
/// first); a node with counter 0 would read before the table, so we never draw or predict one.
inline bool visible(const Node& n) { return n.flags != 0 && n.delay == 0 && n.counter >= 1 && n.counter <= 0x7F; }
/// What we draw in 3D (v1): the digits 0-9 of kinds 0-2. The four words (0x3A-0x3D, texture 600) stay on the panel.
inline bool drawable(const Node& n) { return visible(n) && n.glyph <= 9 && n.kind <= 2; }

// ---- the game's placement (0x79F8A1..0x79F93B), in PS2 px -----------------------------------------------------------
struct Ps2 { int dx, dy; uint8_t r, g, b, a; bool ok; };
/// The node's offset from its Chr's screen anchor (PS2 px, y down), its colour and alpha bytes, exactly as the game computes
/// them: step = counter - 1, j = step - idx; kind 0: (T.dx[j] + ox, oy - T.dy[j]); kind 1 (heal): (ox, oy - step / 2); kind 2:
/// (ox, oy - step); alpha T.a[j], or (len - step) << 5 (a byte) in the last frames (step > len - 5). ok = false for a j
/// outside the table or a kind the game has no colour for (its colours then carry over from the last node: not predictable).
inline Ps2 ps2(const Node& n, const Table& T) {
    Ps2 r = { 0, 0, 0x80, 0x80, 0x80, 0, false };
    const int step = static_cast<int8_t>(n.counter) - 1;
    const int j = step - static_cast<int8_t>(n.idx);
    if (j < 0 || j >= k_tab_n || n.kind > 2) return r;
    uint8_t a = T.a[j];
    if (step > static_cast<int8_t>(n.len) - 5) a = static_cast<uint8_t>((n.len - static_cast<uint8_t>(step)) << 5);
    r.a = a;
    if (n.kind == 1) { r.r = 0x4A; r.g = 0xA0; r.b = 0x58; }
    if (n.kind == 0) { r.dx = T.dx[j] + n.ox; r.dy = n.oy - T.dy[j]; }
    else if (n.kind == 1) { r.dx = n.ox; r.dy = n.oy - step / 2; }
    else { r.dx = n.ox; r.dy = n.oy - step; }
    r.ok = true;
    return r;
}

/// The queue entry the game pushes for this node (x, y = the anchor + the offset; the clamped anchor when flags & 0x80), and
/// whether it is pushed at all (the game's screen bounds). False when the placement is not predictable.
inline bool predict(const Node& n, const Table& T, const Chr& c, Entry* e, bool* in_bounds) {
    const Ps2 p = ps2(n, T); if (!p.ok) return false;
    const bool clamp = (n.flags & 0x80) != 0;
    const int x = (clamp ? c.f4c : c.f44) + p.dx, y = (clamp ? c.f50 : c.f48) + p.dy;
    if (in_bounds) *in_bounds = x + n.ex >= 0 && x <= 0x200 && y + n.ey >= 0 && y <= 0x1A0;
    if (e) { e->x = static_cast<int16_t>(x); e->y = static_cast<int16_t>(y); e->kind = n.kind; e->glyph = static_cast<int8_t>(n.glyph); e->r = p.r; e->g = p.g; e->b = p.b; e->a = p.a; }
    return true;
}
inline bool same(const Entry& a, const Entry& b) { return a.x == b.x && a.y == b.y && a.kind == b.kind && a.glyph == b.glyph && a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }

inline const Chr* chr_of(const Chr* c, int nc, int chr) { for (int i = 0; i < nc; ++i) if (c[i].chr == chr) return &c[i]; return nullptr; }
inline bool finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
/// Drawn in 3D (native's rule, dmg_build): a drawable node whose target was read (the game's rule or the optpos[1] stand-in)
/// with a finite anchor. The first k_max of them in list order are drawn.
inline bool drawn3d(const Node& n, const Chr* c) { return drawable(n) && c && c->ok && c->src <= 1 && finite3(c->opt) && finite3(c->tl) && finite3(c->td) && std::isfinite(c->h4e8); }
/// Taken off the panel by the hide (with the 64 cap, queue_check): drawn in 3D at the game's own anchor (a stand-in anchor
/// keeps the game's number too). The self spot counts: those digits are drawn, just in front of you.
inline bool exact(const Node& n, const Chr* c) { return drawn3d(n, c) && c->src == 0; }

enum QueueWhy : uint32_t { Q_OK = 0, Q_MISMATCH = 1, Q_MISSING = 2, Q_UNPREDICTABLE = 3, Q_NOCHR = 4, Q_BADCOUNT = 5 };
struct QueueResult { int count = -1, matched = 0, dropped = 0, expected = 0, at = -1; uint32_t why = Q_OK; };
/// The queue check (and, with compact, the hide): the node entries of this frame, as predicted from the nodes in list order,
/// must appear in the queue in that order, each as the next entry of kind 0-2 (the Doom countdown's kind-3 entries come first and
/// are skipped; nothing else pushes). count = the live count (the hide, before the queue is drawn) or k_q_cap (the stale entries
/// after the draw, which only zeroes the count: then the check stops at the last predicted entry). On a full match, compact removes
/// the entries of nodes drawn in 3D (exact()) and keeps everything else in order; on any doubt nothing is changed.
inline QueueResult queue_check(Entry* q, int count, bool compact, const Node* n, int nn, const Chr* c, int nc, const Table& T) {
    QueueResult r;
    if (!q || count < 0 || count > k_q_cap) { r.why = Q_BADCOUNT; return r; }
    Entry pe[128]; bool drop[128]; int np = 0, rank = 0;   // rank: the node's place among those drawn in 3D (only the first k_max are)
    for (int i = 0; i < nn && i < 128; ++i) {
        if (!visible(n[i])) continue;
        const Chr* ci = chr_of(c, nc, n[i].chr);
        if (!ci) { r.why = Q_NOCHR; r.at = i; return r; }
        bool inb = false;
        const bool d3 = drawn3d(n[i], ci), mine = exact(n[i], ci) && rank < k_max; if (d3) ++rank;
        if (!predict(n[i], T, *ci, &pe[np], &inb)) { r.why = Q_UNPREDICTABLE; r.at = i; return r; }
        if (!inb) continue;
        drop[np++] = mine;
    }
    r.expected = np;
    int j = 0, last = -1;
    for (int i = 0; i < count && j < np; ++i) {
        if (q[i].kind == 3) continue;
        if (!same(q[i], pe[j])) { r.why = Q_MISMATCH; r.at = i; r.matched = j; return r; }
        last = i; ++j;
    }
    r.matched = j;
    if (j < np) { r.why = Q_MISSING; r.at = last; return r; }
    if (compact)   // the live queue: nothing but the Doom countdown may follow the node entries (else the mapping is in doubt)
        for (int i = last + 1; i < count; ++i) if (q[i].kind != 3) { r.why = Q_MISMATCH; r.at = i; return r; }
    int out = count;
    if (compact) {
        out = 0; j = 0;
        for (int i = 0; i < count; ++i) {
            const bool node_entry = q[i].kind != 3 && j < np;
            if (node_entry) { const bool d = drop[j++]; if (d) { r.dropped++; continue; } }
            q[out++] = q[i];
        }
    }
    r.count = out;
    return r;
}

// ---- 3D -------------------------------------------------------------------------------------------------------------
/// The node's quad in UI px relative to its anchor (x right, y down: top-left x0, y0, bottom-right x1, y1), and its colour
/// (the game's bytes / 128: 0x80 = 1; the heal's green 0xA0 is 1.25, saturated by the target as the game's sprite is) and alpha.
struct Quad { float x0, y0, x1, y1; float c[4]; bool ok; };
inline Quad place(const Node& n, const Table& T) {
    Quad q = {}; const Ps2 p = ps2(n, T); if (!p.ok || !drawable(n)) return q;
    q.x0 = p.dx * k_ui_per_ps2_x; q.y0 = p.dy * k_ui_per_ps2_y; q.x1 = q.x0 + k_glyph_w; q.y1 = q.y0 + k_glyph_h;
    q.c[0] = p.r / 128.f; q.c[1] = p.g / 128.f; q.c[2] = p.b / 128.f; q.c[3] = p.a >= 128 ? 1.f : p.a / 128.f;
    q.ok = true;
    return q;
}
/// battle.dds texels of digit d (0x8FB840: (444 + 49 (d % 5), 254 + 65 (d / 5)), 49 x 65 of 1024), as u0 v0 u1 v1, with
/// v flipped (1 - v) when the stream held it so (C3's rule).
inline void uv(int d, bool flip, float out[4]) {
    d = d < 0 ? 0 : d > 9 ? 9 : d;
    const float k = 1.f / 1024.f;
    const float u0 = (444.f + 49.f * (d % 5)) * k, v0 = (254.f + 65.f * (d / 5)) * k;
    out[0] = u0; out[2] = u0 + 49.f * k;
    out[1] = flip ? 1.f - v0 : v0; out[3] = flip ? 1.f - (v0 + 65.f * k) : v0 + 65.f * k;
}

/// The target's anchor in render space (x, y negated; y up): optpos with the 30 Hz correction (- logic root + drawn root, not
/// for an attached actor or an override matrix: C2's rule), lifted by Chr+0x4E8 as the game does, then `lift` units up.
inline bool anchor(const Chr& c, float lift, float out[3]) {
    if (!c.ok || c.src > 1) return false;
    float g[3];
    for (int k = 0; k < 3; ++k) g[k] = (c.aflags & 3) ? c.opt[k] : c.opt[k] - c.tl[k] + c.td[k];
    g[1] -= c.h4e8;
    out[0] = -g[0]; out[1] = -g[1] + lift; out[2] = g[2];
    return finite3(out) && std::isfinite(c.h4e8);
}
/// C3-13's rule: the anchor pulled toward the head by a tenth of the distance, at most 30 units, along the line of sight (the
/// number stays where it is on screen but no longer sinks into a big body: there is no depth test).
inline void pull(float a[3], const float head[3]) {
    const float dx = head[0] - a[0], dy = head[1] - a[1], dz = head[2] - a[2], dl = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(dl > 1e-3f) || !std::isfinite(dl)) return;
    const float p = (0.1f * dl < 30.f ? 0.1f * dl : 30.f) / dl;
    a[0] += dx * p; a[1] += dy * p; a[2] += dz * p;
}
/// Where a target's digits are drawn: its anchor (anchor(), `lift` up) pulled toward the head, or, for the actor first person
/// looks out of (aflags & k_self), C3's self spot (0.5 m ahead at chest height, from eye 1's view: bb[6..8]) as it is: the
/// anchor would be at your own eyes (behind them, or a cross-eyed number a few cm away). No spot given: the anchor.
inline bool site(const Chr& c, float lift, const float head[3], const float* self_spot, float out[3]) {
    if ((c.aflags & k_self) && self_spot) {
        if (!c.ok || c.src > 1 || !finite3(self_spot)) return false;
        out[0] = self_spot[0]; out[1] = self_spot[1]; out[2] = self_spot[2];
        return true;
    }
    if (!anchor(c, lift, out)) return false;
    pull(out, head);
    return finite3(out);
}
/// No digit nearer the eye than min_units (0.6 m: three of the 80 numbers logged on 6 / 7 Nov were 0.43-0.53 m away, 3-4 cm
/// digits in the face): a site nearer than that goes out along the ray from the head through it, so it stays where it is on
/// screen. A site at the head itself (no ray) is left. Returns whether it moved.
constexpr float k_min_eye_m = 0.6f;
inline bool keep_out(float a[3], const float head[3], float min_units) {
    if (!(min_units > 0.f) || !std::isfinite(min_units) || !finite3(a) || !finite3(head)) return false;
    const float dx = a[0] - head[0], dy = a[1] - head[1], dz = a[2] - head[2], dl = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(dl > 1e-4f) || dl >= min_units) return false;
    const float k = min_units / dl;
    a[0] = head[0] + dx * k; a[1] = head[1] + dy * k; a[2] = head[2] + dz * k;
    return true;
}

// ---- 0x794AB0: the damage anchor of a chr whose Chr+0x442 is 0xFF ---------------------------------------------------
/// What managed reads for it (main thread, guarded reads; mirrored in dmg3d.cs, 56 B). Game space (y down).
struct AnchorIn {
    float ovr_t[3];       // the override matrix's translation: *(actor+0x10C) + 0x30..0x38 (ovr_ok)
    float parent_opt[3];  // optpos[Chr+0x441] of chr Chr+0x43F's actor, by 0x8354F0's rule (parent_ok 1)
    float pos[3];         // actor +0x0C..0x14 (chr_pos_vec: the logic root on the ground, without +0x6C)
    float f70, c3a4, a4f0;          // Chr+0xF70 (the dead height), Chr+0x3A4 (a scale, 1.0 at init), actor+0x4F0 (a height)
    uint8_t ovr, ovr_ok;            // actor+0x10C != 0; its translation read
    uint8_t parent, parent_ok;      // Chr+0x43F (0xFF none); 0 no parent actor (id 0xFF, >= 0x1F or no actor), 1 read, 2 read failed
    uint8_t stat_death, flying;     // Chr+0xDCC, Chr+0x505
    uint8_t btl_9dd, btl_9ce;       // [0x112C9DD], [0x112C9CE] (a battle-wide flag from the btl file: 0x783E20)
};
static_assert(sizeof(AnchorIn) == 56, "AnchorIn is mirrored in dmg3d.cs");
static_assert(offsetof(AnchorIn, ovr_t) == 0 && offsetof(AnchorIn, parent_opt) == 12 && offsetof(AnchorIn, pos) == 24 &&
              offsetof(AnchorIn, f70) == 36 && offsetof(AnchorIn, c3a4) == 40 && offsetof(AnchorIn, a4f0) == 44 &&
              offsetof(AnchorIn, ovr) == 48 && offsetof(AnchorIn, ovr_ok) == 49 && offsetof(AnchorIn, parent) == 50 &&
              offsetof(AnchorIn, parent_ok) == 51 && offsetof(AnchorIn, stat_death) == 52 && offsetof(AnchorIn, flying) == 53 &&
              offsetof(AnchorIn, btl_9dd) == 54 && offsetof(AnchorIn, btl_9ce) == 55, "AnchorIn's field order is dmg3d.cs DmgAnchorIn's");
enum AnchorHow : int { AB_FAIL = 0, AB_OVR = 1, AB_PARENT = 2, AB_POS = 3, AB_DEAD_Y = 0x10, AB_FLY = 0x20 };
/// The game's 0x794AB0(chr, out) for a chr with an actor (our exe, read 7 Nov): (1) actor+0x10C != 0: the override matrix's
/// translation, and nothing else; else 0x796EA0: (2) Chr+0x43F != 0xFF and that chr has an actor (0x795080): optpos[Chr+0x441]
/// of that actor through 0x8354F0; (3) else the actor's position (0x82AC90: +0x0C..0x14). Then (0x794AB0 again, not for 1):
/// [0x112C9DD] != 0 and stat_death == 2: y = Chr+0xF70; Chr+0x505 (flying) or [0x112C9CE]: y += actor+0x4F0 * Chr+0x3A4 * 0.5
/// (0x7950F0, [0xB92220] = 0.5). 0x79F380 then subtracts Chr+0x4E8 from y for both its branches (anchor() does that).
/// Returns AB_* (branch | adjustments) with out set, or AB_FAIL (a read the branch needs failed: the caller keeps its stand-in).
inline int anchor_794ab0(const AnchorIn& in, float out[3]) {
    int how;
    if (in.ovr) {
        if (!in.ovr_ok || !finite3(in.ovr_t)) return AB_FAIL;
        out[0] = in.ovr_t[0]; out[1] = in.ovr_t[1]; out[2] = in.ovr_t[2];
        return AB_OVR;
    }
    if (in.parent != 0xFF && in.parent_ok != 0) {
        if (in.parent_ok != 1 || !finite3(in.parent_opt)) return AB_FAIL;
        out[0] = in.parent_opt[0]; out[1] = in.parent_opt[1]; out[2] = in.parent_opt[2]; how = AB_PARENT;
    }
    else { out[0] = in.pos[0]; out[1] = in.pos[1]; out[2] = in.pos[2]; how = AB_POS; }
    if (in.btl_9dd != 0 && in.stat_death == 2) { out[1] = in.f70; how |= AB_DEAD_Y; }
    if (in.flying != 0 || in.btl_9ce != 0) { const float k = in.a4f0 * in.c3a4; out[1] = out[1] + k * 0.5f; how |= AB_FLY; }
    return finite3(out) ? how : AB_FAIL;
}

/// World units per UI px at distance `dist`: the game's size (size / 14 units per px), with the digit's height kept between
/// k_min_rad and k_max_rad (times size) so it stays readable from the diorama to first person.
inline float units_per_px(float size, float dist) {
    if (!(size > 0.f) || !std::isfinite(size)) size = 1.f;
    float s = size / k_px_per_unit;
    if (dist > 1e-3f && std::isfinite(dist)) {
        const float h = k_glyph_h * s, lo = k_min_rad * size * dist, hi = k_max_rad * size * dist;   // small angles: height = angle * distance
        if (h < lo) s = lo / k_glyph_h; else if (h > hi) s = hi / k_glyph_h;
    }
    return s;
}
/// The quad's corners in render space: TL TR BL BR = a + right * x * s + up * (-y * s).
inline void corners(const float a[3], const float right[3], const float up[3], float s, const Quad& q, float P[4][3]) {
    const float lx[4] = { q.x0, q.x1, q.x0, q.x1 }, ly[4] = { q.y0, q.y0, q.y1, q.y1 };
    for (int i = 0; i < 4; ++i) for (int k = 0; k < 3; ++k) P[i][k] = a[k] + right[k] * lx[i] * s - up[k] * ly[i] * s;
}

/// A render-space point through the game's camera (cam: g_cam's view rows 0..11 and its projection at +16, row-major) to PS2 px,
/// the space of Chr+0xF44 / F48: UI units (x * 960, -y * 540 of NDC, centred), then / 3.75 + 256 and / 2.596 + 208. Log only.
inline bool project_ps2(const float* cam, const float p[3], float* px, float* py) {
    if (!cam || !px || !py) return false;
    const float e0 = cam[0] * p[0] + cam[1] * p[1] + cam[2] * p[2] + cam[3];
    const float e1 = cam[4] * p[0] + cam[5] * p[1] + cam[6] * p[2] + cam[7];
    const float e2 = cam[8] * p[0] + cam[9] * p[1] + cam[10] * p[2] + cam[11];
    const float* P = cam + 16;
    const float c0 = P[0] * e0 + P[1] * e1 + P[2] * e2 + P[3], c1 = P[4] * e0 + P[5] * e1 + P[6] * e2 + P[7];
    const float c3 = P[12] * e0 + P[13] * e1 + P[14] * e2 + P[15];
    if (!(c3 > 1e-4f)) return false;
    *px = c0 / c3 * 960.f / k_ui_per_ps2_x + 256.f; *py = -c1 / c3 * 540.f / k_ui_per_ps2_y + 208.f;
    return std::isfinite(*px) && std::isfinite(*py);
}

// ---- lifetime -------------------------------------------------------------------------------------------------------
/// A snapshot counts while its game-frame stamp is within 3 frames of the counter (a counter behind the stamp is not live),
/// not older than the floor (the frame the feature or the battle began) and not stuck for 180 Presents: C2's rule, so the
/// numbers come and go with the game's nodes, a late frame never blinks them, and a stopped main thread clears them in time.
constexpr int k_live = 3, k_backstop = 180;
inline bool live(uint32_t stamp, uint32_t frame, uint32_t floor, int since) {
    const int lag = static_cast<int>(frame - stamp);
    return lag >= 0 && lag <= k_live && since >= 0 && since <= k_backstop && static_cast<int>(stamp - floor) >= 0;
}

// ---- numbers (for the log) ------------------------------------------------------------------------------------------
/// One MsNumberRegist call: its digit nodes (idx 0, 1, 2 ... in list order, the same chr), value = sum glyph * 10^idx (idx 0
/// is the ones digit), and whether a word node follows (idx = the digit count).
struct Group { int chr, kind, digits; uint32_t value; bool word; int first; uint8_t slot0, counter0; };
inline int groups(const Node* n, int nn, Group* out, int max) {
    int g = 0;
    for (int i = 0; i < nn && g < max; ) {
        if (n[i].idx != 0 || n[i].glyph > 9) { ++i; continue; }
        Group G = { n[i].chr, n[i].kind, 1, n[i].glyph, false, i, n[i].slot, n[i].counter };
        uint32_t p10 = 10; int k = i + 1;
        while (k < nn && n[k].chr == n[i].chr && n[k].idx == G.digits && n[k].glyph <= 9 && G.digits < 9) { G.value += n[k].glyph * p10; p10 *= 10; G.digits++; ++k; }
        if (k < nn && n[k].chr == n[i].chr && n[k].idx == G.digits && n[k].glyph >= 0x3A) { G.word = true; ++k; }
        out[g++] = G; i = k;
    }
    return g;
}
/// New numbers between snapshots: a group is new when its first digit's pool slot was not in use in the last snapshot, or
/// its frame counter went down (the slot was freed and reused).
struct Tracker {
    uint8_t live[k_pool_n] = {}, counter[k_pool_n] = {};
    bool fresh(const Group& g) const { return !live[g.slot0] || g.counter0 < counter[g.slot0]; }
    void update(const Node* n, int nn) {
        memset(live, 0, sizeof(live));
        for (int i = 0; i < nn; ++i) { live[n[i].slot] = 1; counter[n[i].slot] = n[i].counter; }
    }
};

}   // namespace dmg
