// SPDX-License-Identifier: MIT
// ffxvr_sphere.h -- sphere hands, stages 1 + 2 (research/sphere-hands-design.md sections 9 and 10): in a Cloister of Trials, a
// grip of the VR hand at a sphere or socket takes or places the sphere, with no prompt. Pure logic: no Windows, no D3D, no
// OpenXR, no logging, no allocation. ffxvr_native.cpp (the sph_* glue) feeds it what managed read from the game (sphere_log.cs:
// the talk-enabled workers, the sockets, the trial's variables, the text windows) and the drawn hands, and acts on its answers
// (the labels, the grips held back from the game, one A press at a time). A unit test drives all of it (scratch sph_chk).
//
// Game facts (the capture of 10 Nov, logs9; our exe's 0x866CC0 / 0x866980 / 0x867950):
//   - The game picks the talk target per press: every talk-enabled worker's test, in worker order, but only the workers it
//     tests at all (tested(): the dispatch 0x8666E0 needs +0x34 bit 0x80; the object test 0x866980 also +0xAA == 1, +0x34
//     bits 0x20 / 0x200 clear and +0x36 bit 0x08 clear: the player's own worker and the door's and pedestal's parts fail
//     these, review B1). A box zone (type 3) takes
//     the player's root inside its rotated box (half-extents +0x30 / +0x38, height +0x1C) or within range + radius of it, and
//     stores (range + radius)^2 as its "distance"; an object (type 1) takes its distance squared; the first strictly smaller
//     wins. Then the facing test with the worker's tolerance (Djose's sphere interactions: 4.0 rad, so any facing).
//   - The prompts (English): take = "A <name> is inserted here." + a choice "Take sphere." / "Do nothing." (row 0 at open),
//     then "Removed <name>."; place = "You see a sphere-shaped recess.", then "You are holding a <name>." + "Insert sphere." /
//     "Do nothing.", then "Inserted <name>."; a full socket while carrying = "... is inserted here." then "You can only hold
//     one sphere at a time." Each line waits for confirm. Any other text (another language, another trial's wording) is
//     P_UNKNOWN: the automation stops and the prompt shows (the player answers it).
// Conventions: game space (y down), units; distances to the hands in metres through units_per_m (11.4 in first person).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sph {

constexpr int   k_zones = 24, k_targets = 16, k_wins = 3, k_page = 96, k_name = 32;
constexpr float k_reach_m = 0.30f;    // a grip within this of a target's sphere grabs (the capture: a deliberate touch 0.18 m)
constexpr float k_label_m = 0.60f;    // the label fades in from here and is clear at k_reach_m
constexpr float k_pi = 3.14159265f;

// -- what managed sends (layouts shared with sphere_hands.cs; x86, 4-byte fields) --------------------------------------
struct Zone {           // a talk-enabled worker near Tidus (the game's own candidates)
    int32_t widx;       // its global index (+0x2E): the game's loop order
    int32_t kind;       // its type: 3 a box zone, 1 an object (radius), 2 a line zone
    float   c[3];       // the talk block's position (+0x558), game space
    float   hx, hz;     // type 3: the half-extents (+0x30 / +0x38); type 2: the segment's other end x / z
    float   rot;        // type 3: the box's rotation (+0x34)
    float   h;          // the height (+0x1C; <= 0: none)
    float   range;      // +0x40
    float   htol;       // +0x2C (type 1: >= 0 a horizontal distance and |dy| < htol; < 0 a 3D distance)
    float   atol;       // +0x3C, the facing tolerance (radians)
    int32_t target;     // the index of its sphere target (-1: not a sphere socket)
    uint32_t flags;     // its +0x34 (low 16 bits) and +0x36 (high 16): the game's own gates (tested())
    int32_t aa;         // its +0xAA (an object is tested only with 1)
};
struct Target {         // a socket or pedestal of the trial (sphere_log.cs SphTrial's socket table)
    int32_t widx;       // its worker (global index)
    int32_t kind;       // 0 a socket zone, 1 a pedestal (slots)
    int32_t value;      // its content: the sphere's value (0 empty; a pedestal: any of its slots); -1 not read
    int32_t free_slots; // empty slots (a socket: 1 when value == 0)
    float   anchor[3];  // where its sphere sits (game space): the reach and the label
    char    name[k_name];   // the content's name ("" unknown)
};
struct Win {            // a render-side message window (slots 0..2), read this frame
    int32_t open;       // +0x00 == 1
    int32_t state;      // +0x2C: 1 typing, 2 shown, 3 / 4 closing
    int32_t choice_mode;    // its cursor's +0x01: 0 none, 1 up, 2 confirmed
    int32_t choice_row;     // the cursor's +0x02
    int32_t n;          // page bytes read (0: none)
    int32_t page_end;   // +0x2E: set with state 2 (0x8DE970 / 0x8DED60): 3 = the text's last page is out (the script's waits test it), 2 = more pages
    uint8_t page[k_page];   // the page text (+0x94), the game's encoding
};
struct Head {           // the rest of this frame's state
    int32_t bytes;
    int32_t on;         // managed's gates: the setting, a trial map with its table, first person, VR hands, the field (no
                        // battle, menu, movie, save screen, blitz), the F8 panel not shown
    int32_t control;    // the player actor [0x1300788] != 0
    int32_t free_play;  // the controller's byte 0 has 0x02 and byte 3 is 0 (no event running)
    int32_t carried;    // the trial's carried variable (-1 not read)
    int32_t talk_target;    // the controller's +0x1E8 (the game's last chosen worker)
    uint32_t gen;       // the scene generation (map, event, load): a change ends a sequence
    float   tidus[3];   // Tidus's root, game space
    float   player_radius;  // the controller's +0x1F0
    int32_t player_widx;    // the player's own worker (the controller's +0x0A; -1 none): never a candidate
    char    carried_name[k_name];
    int32_t sizes;      // k_sizes: the other three layouts' sizes (review round 2 N12: native refuses and logs any other)
    // the script side's 8 message windows of the current bank (TOMesWinWork, FhUtil 0xF26D70 + bank * 8 * 0x2C; the bank at
    // 0xF26B82): what the game's own confirm test reads (msg_kind, line_ready)
    int32_t mw_ok;      // 1: read this frame
    uint8_t mw_state[8];    // +0x14: 0 none, 1 typing, 2 shown, 3 closing (the render window's state, copied each frame by 0x871F30)
    uint8_t mw_flags[8];    // +0x1D: 0x02 / 0x20 a window the player answers (a choice), 0x10 the script waits for a confirm on it
    uint8_t mw_w16[8];      // +0x16 != 0
};
constexpr int32_t k_sizes = static_cast<int32_t>(sizeof(Zone) | (sizeof(Target) << 8) | (sizeof(Win) << 16));
// sphere_hands.cs's SphZone / SphTarget / SphWin / SphHead (review N4: only Head carries its size; native refuses and logs
// a Head of another size. Round 2 N12: Head.sizes carries the other three sizes, checked on every push)
static_assert(sizeof(Zone) == 60 && sizeof(Target) == 60 && sizeof(Win) == 120 && sizeof(Head) == 112, "the layouts shared with sphere_hands.cs");

// -- the game's choice, mirrored ----------------------------------------------------------------------------------------
enum Cand { C_NO = 0, C_YES = 1, C_UNSURE = 2, C_EDGE = 3 };
/// Is the player at p (game space) a talk candidate of zone z, and its "distance" (what the game compares)?
inline int candidate(const Zone& z, const float p[3], float radius, float* metric) {
    const float dx = p[0] - z.c[0], dy = p[1] - z.c[1], dz = p[2] - z.c[2];
    const float rr = (z.range + radius) * (z.range + radius);
    bool in = false;
    if (z.kind == 3) {
        const float c = cosf(z.rot), s = sinf(z.rot);
        const float x = dx * c - dz * s, w = dx * s + dz * c;      // 0x866CC0: x' = dx cos - dz sin, z' = dx sin + dz cos
        const bool hin = !(z.h > 0.f) || (dy < z.h && dy > -z.h);
        const bool box = x > -z.hx && x <= z.hx && w > -z.hz && w <= z.hz;
        if (box && hin) in = true;
        else if (hin) {
            const float cx = x < -z.hx ? -z.hx : x > z.hx ? z.hx : x, cw = w < -z.hz ? -z.hz : w > z.hz ? z.hz : w;
            in = (x - cx) * (x - cx) + (w - cw) * (w - cw) <= rr;
        }
        *metric = rr;
    }
    else if (z.kind == 1) {
        float d2 = dx * dx + dz * dz;
        if (z.htol < 0.f) d2 += dy * dy; else if (!(fabsf(dy) < z.htol)) return C_NO;
        in = d2 < rr; *metric = d2;   // 0x866980: out when (range + radius)^2 <= d2
    }
    else if (z.kind == 2) {   // the segment's nearest point, horizontally
        const float sx = z.hx - z.c[0], sz = z.hz - z.c[2], l2 = sx * sx + sz * sz;
        float u = l2 > 1e-6f ? (dx * sx + dz * sz) / l2 : 0.f; u = u < 0.f ? 0.f : u > 1.f ? 1.f : u;
        const float ex = dx - u * sx, ez = dz - u * sz;
        in = ex * ex + ez * ez <= rr; *metric = rr;
    }
    else return C_NO;
    if (!in) return C_NO;
    return z.atol >= k_pi ? C_YES : C_UNSURE;   // a real facing rule: its heading convention is not mirrored (no guess)
}
/// Does the game run its talk test on this worker at all (review B1; the decompile of 0x8666E0 and 0x866980)? Managed lists
/// every talk-enabled worker of type 1..3 near Tidus with its flags; this is the game's own filter on that list.
inline bool tested(const Zone& z, int player_widx) {
    const uint32_t f34 = z.flags & 0xFFFFu, f36 = z.flags >> 16;
    if (z.widx == player_widx) return false;                // the player's own worker
    if (!(f34 & 0x80u)) return false;                       // 0x8666E0: no test without it
    if (z.kind == 1) return z.aa == 1 && !(f34 & 0x220u) && !(f36 & 0x08u);   // 0x866980
    return z.kind == 2 || z.kind == 3;
}
/// Round 3 S6: how far inside the zone's candidate region the player is, in game units (negative: outside). A box zone
/// only (kind 3; the others: 1e9, no margin is asked of them). The least of the distances to the box's four sides (the
/// range's ring counts as inside) and, with a height rule, to its two height limits.
inline float box_depth(const Zone& z, const float p[3], float radius) {
    if (z.kind != 3) return 1e9f;
    const float dx = p[0] - z.c[0], dy = p[1] - z.c[1], dz = p[2] - z.c[2];
    const float c = cosf(z.rot), s = sinf(z.rot);
    const float u = dx * c - dz * s, v = dx * s + dz * c;   // as candidate()
    const float ex = z.hx - fabsf(u), ew = z.hz - fabsf(v), r = z.range + radius;
    float d;
    if (ex >= 0.f && ew >= 0.f) d = (ex < ew ? ex : ew) + r;                    // in the box: the nearer side
    else { const float ox = ex < 0.f ? -ex : 0.f, ow = ew < 0.f ? -ew : 0.f; d = r - sqrtf(ox * ox + ow * ow); }
    if (z.h > 0.f) { const float eh = z.h - fabsf(dy); if (eh < d) d = eh; }
    return d;
}
constexpr float k_edge_margin = 0.5f;   // a box zone is a sure candidate from this far inside every side, and surely none from this far outside
/// candidate() with the margin (round 3 S6; the owner's log, 22:18:20: Tidus at x = -1.0, on worker 50's side, read as
/// -1.02 by the game on the press's frame, which chose worker 51): a box zone within `margin` of its region's border,
/// inside or outside, is C_EDGE: the game may or may not count it a few frames later. margin 0: candidate() itself.
inline int candidate_m(const Zone& z, const float p[3], float radius, float margin, float* metric) {
    const int c = candidate(z, p, radius, metric);
    if (z.kind != 3 || !(margin > 0.f)) return c;
    const float d = box_depth(z, p, radius);
    if (d >= margin) return c;
    if (d <= -margin) return C_NO;
    return C_EDGE;   // (its metric: the box's, set by candidate())
}
constexpr int k_pred_none = -1, k_pred_unsure = -2, k_pred_edge = -3;
/// The worker the game would choose for a confirm now (its widx), k_pred_none, k_pred_unsure or k_pred_edge.
/// k_pred_unsure: a candidate with a facing rule (not mirrored) that could win, i.e. nearer than the best sure one, or
/// equal and earlier, or with no sure one. A box zone's "distance" is (range + radius)^2, 0 in Djose, which no object can
/// beat. zones in the game's order (ascending widx).
/// k_pred_edge (round 3 S6; no grab starts on it, the label says "Step closer"): the answer could change within a few
/// frames. Either a box zone within `margin` of its border could win by the same rule (so a zone's edge matters only where
/// it changes the choice: in the door pair's overlap the earlier worker wins whatever the later one's edge says), or
/// there is a choice and the player has not stopped (`still` false; see Still).
inline int predict(const Zone* z, int n, const float p[3], float radius, int player_widx = -1, float margin = 0.f, bool still = true) {
    int best = k_pred_none, bi = -1; float bestv = 0.f;
    for (int i = 0; i < n; ++i) {
        float m = 0.f;
        if (!tested(z[i], player_widx)) continue;
        if (candidate_m(z[i], p, radius, margin, &m) == C_YES && (best == k_pred_none || m < bestv)) { best = z[i].widx; bestv = m; bi = i; }
    }
    for (int i = 0; i < n; ++i) {
        float m = 0.f;
        if (!tested(z[i], player_widx)) continue;
        if (candidate_m(z[i], p, radius, margin, &m) == C_UNSURE && (best == k_pred_none || m < bestv || (m == bestv && i < bi))) return k_pred_unsure;
    }
    for (int i = 0; i < n; ++i) {
        float m = 0.f;
        if (!tested(z[i], player_widx)) continue;
        if (candidate_m(z[i], p, radius, margin, &m) == C_EDGE && (best == k_pred_none || m < bestv || (m == bestv && i < bi))) return k_pred_edge;
    }
    if (best != k_pred_none && !still) return k_pred_edge;
    return best;
}
inline float dist3(const float a[3], const float b[3]) { const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return sqrtf(x * x + y * y + z * z); }
/// Has the player stopped (round 3 S6)? His root has stayed within k_still_eps of one place for k_still_s. Timed, not
/// counted in frames: the render thread reads the root more often than the game moves it. feed() once per fresh read;
/// reset() when there was none.
constexpr float  k_still_eps = 0.02f;   // game units (11.4 = 1 m); a run is about 0.7 a game frame, the log's settling 0.02 in 70 ms
constexpr double k_still_s = 0.10;      // three game frames
struct Still {
    float a[3] = { 0.f, 0.f, 0.f }; double t0 = 0.0; bool have = false;
    void reset() { have = false; }
    void feed(const float p[3], double t) {
        if (have && dist3(p, a) <= k_still_eps) return;
        a[0] = p[0]; a[1] = p[1]; a[2] = p[2]; t0 = t; have = true;
    }
    bool stopped(double t) const { return have && t - t0 >= k_still_s; }
};

// -- the hand's target, the action and the label --------------------------------------------------------------------------
enum Act { A_NONE = 0, A_TAKE, A_PLACE, A_FULL, A_EMPTY };
inline int action(int carried, const Target& t) {
    if (carried < 0 || t.value < 0) return A_NONE;
    if (carried == 0) return t.value > 0 ? A_TAKE : A_EMPTY;
    return t.free_slots > 0 ? A_PLACE : A_FULL;
}
/// The label's opacity at the nearer palm's distance d (metres): none from k_label_m, clear at k_reach_m and nearer.
inline float fade(float d) {
    if (!(d < k_label_m)) return 0.f;
    if (d <= k_reach_m) return 1.f;
    return (k_label_m - d) / (k_label_m - k_reach_m);
}
/// strncpy without its warnings: at most cap - 1 characters, always terminated.
inline void copy_str(char* out, const char* in, int cap) { if (cap <= 0) return; int i = 0; for (; in && in[i] && i < cap - 1; ++i) out[i] = in[i]; out[i] = 0; }
/// One hand's nearest target with a known action (its index, -1 none) and its distance in metres.
inline int nearest(const Head& h, const Target* t, int nt, const float palm[3], float units_per_m, float* d_m) {
    int best = -1; float bd = 1e9f;
    if (units_per_m > 0.f)
        for (int i = 0; i < nt; ++i) {
            if (action(h.carried, t[i]) == A_NONE) continue;
            const float d = dist3(palm, t[i].anchor) / units_per_m;
            if (d < bd) { bd = d; best = i; }
        }
    *d_m = bd;
    return best;
}
constexpr float k_keep_m = 0.03f;     // the label stays on its target until another is this much nearer (review N1: the door pair)
struct Pick { int target = -1, hand = -1, act = A_NONE; float d = 1e9f; bool step_closer = false, unsure = false; };
/// The nearest target to either drawn palm (game space) within k_label_m, its action, and whether the game would choose it.
/// keep: the last frame's target (-1 none), kept while it is within k_keep_m of the nearest.
inline Pick pick(const Head& h, const Target* t, int nt, const float palm[2][3], const bool palm_ok[2], float units_per_m, int predicted, int keep = -1) {
    Pick p;
    if (!(units_per_m > 0.f)) return p;
    for (int i = 0; i < nt; ++i)
        for (int k = 0; k < 2; ++k) {
            if (!palm_ok[k]) continue;
            const float d = dist3(palm[k], t[i].anchor) / units_per_m;
            if (d < p.d) { p.d = d; p.target = i; p.hand = k; }
        }
    if (keep >= 0 && keep < nt && keep != p.target) {
        float dk = 1e9f; int hk = -1;
        for (int k = 0; k < 2; ++k) { if (!palm_ok[k]) continue; const float d = dist3(palm[k], t[keep].anchor) / units_per_m; if (d < dk) { dk = d; hk = k; } }
        if (hk >= 0 && dk <= p.d + k_keep_m) { p.d = dk; p.target = keep; p.hand = hk; }
    }
    if (p.target < 0 || !(p.d < k_label_m)) return Pick();
    p.act = action(h.carried, t[p.target]);
    const bool doable = p.act == A_TAKE || p.act == A_PLACE;
    p.unsure = doable && predicted == k_pred_unsure;
    p.step_closer = doable && !p.unsure && predicted != t[p.target].widx;
    return p;
}
/// The label's text (ASCII). A name the table does not know reads "Sphere". resqueeze: the hand's grip was squeezed away from
/// the target and carried in (no grab): the label says to squeeze again.
inline void label_text(const Pick& p, const Target* t, const char* carried_name, char* out, int cap, bool resqueeze = false) {
    if (cap <= 0) return;
    out[0] = 0;
    if (p.target < 0 || p.act == A_NONE) return;
    const char* nm = t[p.target].name[0] ? t[p.target].name : "Sphere";
    const char* cn = carried_name && carried_name[0] ? carried_name : "sphere";
    char b[96];
    if (p.unsure) snprintf(b, sizeof(b), "Use the A button here");   // the game's choice is not known here (a facing rule)
    else if (p.step_closer) snprintf(b, sizeof(b), "Step closer");
    else if (resqueeze && (p.act == A_TAKE || p.act == A_PLACE)) snprintf(b, sizeof(b), "Squeeze again to %s", p.act == A_TAKE ? "take" : "insert");
    else if (p.act == A_TAKE) snprintf(b, sizeof(b), "Take %s", nm);
    else if (p.act == A_PLACE) snprintf(b, sizeof(b), "Insert %s", cn);
    else if (p.act == A_FULL) snprintf(b, sizeof(b), "Hands full");
    else snprintf(b, sizeof(b), "Empty recess");
    copy_str(out, b, cap);
}

// -- the grips ------------------------------------------------------------------------------------------------------------
struct Grip { bool down = false, owned = false, armed = false; };
/// The grip (down above 0.7 after below 0.5; a quick squeeze counts). Returns 1 when the grab happens now: the press edge in
/// reach of a target, or (review S2, "squeeze while reaching") a grip pressed inside the label range and still held when
/// the hand enters reach. A grip pressed outside the label range and carried in never grabs (resqueeze()). The grip stays
/// owned from the grab until it is let go.
inline int grip_step(Grip& g, float squeeze, bool in_label, bool in_reach) {
    const bool was = g.down;
    if (!g.down && squeeze >= 0.70f) g.down = true;
    else if (g.down && squeeze < 0.50f) g.down = false;
    if (!g.down) { g.owned = false; g.armed = false; return 0; }
    if (!was) g.armed = in_label || in_reach;
    if (g.armed && !g.owned && in_reach) { g.owned = true; return 1; }
    return 0;
}
/// The grips held back from the game (the input thread's strip() latches each until it is let go): a hand in reach of a
/// target (reach_mask), a grip a grab owns, and both hands for as long as a sequence runs (round 2 S5: a second squeeze
/// "to be sure" during the grab must not reach the game as LB / RB, which would count as the player's button and stop it).
inline int hold_mask(int reach_mask, const Grip g[2], bool seq_running) {
    int m = reach_mask & 3;
    for (int h = 0; h < 2; ++h) if (g[h].owned) m |= 1 << h;
    if (seq_running) m = 3;
    return m;
}
/// The hand is in reach with a grip that cannot grab (squeezed away from the target): the label's hint.
inline bool resqueeze(const Grip& g, bool in_reach) { return g.down && !g.armed && in_reach; }   // (an owned grip is armed)
/// The input thread's strip (hv_grip_strip's rule per hand): a grip down (> 0.5) while its hand's hold bit is set is held back
/// from the game until it is let go. Returns the grips the game may see.
inline int strip(bool back[2], const bool down[2], int hold_mask) {
    int pass = 0;
    for (int h = 0; h < 2; ++h) {
        if (!down[h]) back[h] = false; else if ((hold_mask >> h) & 1) back[h] = true;
        if (down[h] && !back[h]) pass |= 1 << h;
    }
    return pass;
}

// -- the prompt's pages ---------------------------------------------------------------------------------------------------
enum Page { P_NONE = 0, P_UNKNOWN, P_TAKE_INFO, P_TAKE_CHOICE, P_RECESS, P_HOLDING, P_INSERT_CHOICE, P_REMOVED, P_INSERTED, P_ONE_ONLY, P_N };
static const char* const k_page_name[P_N] = { "-", "unknown text", "\"A ... is inserted here.\"", "the take choice", "\"You see a sphere-shaped recess.\"",
    "\"You are holding a ...\"", "the insert choice", "\"Removed ...\"", "\"Inserted ...\"", "\"You can only hold one sphere at a time.\"" };
/// The game's text as plain ASCII (its index = byte - 0x30 for the ASCII range); the control codes dropped (two-byte ones with
/// their argument), a line break as a space, spaces collapsed, trimmed. Stops at an end code (0..2).
inline int decode(const uint8_t* b, int n, char* out, int cap) {
    static const char k_cs[] = "0123456789 !\"#$%&'()*+,-./:;<=>?ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~";
    int o = 0;
    auto put = [&](char c) { if (c == ' ' && (o == 0 || out[o - 1] == ' ')) return; if (o < cap - 1) out[o++] = c; };
    for (int i = 0; i < n; ++i) {
        const uint8_t c = b[i];
        if (c <= 2) break;
        if (c == 3) { put(' '); continue; }
        if (c >= 0x30) { const int k = c - 0x30; put(k < static_cast<int>(sizeof(k_cs)) - 1 ? k_cs[k] : '?'); continue; }
        if (c >= 0x2B) { put('?'); ++i; continue; }                  // a glyph outside ASCII (two bytes)
        if (c == 4) { i += (i + 1 < n && b[i + 1] < 0x30) ? 2 : 1; continue; }
        if (c == 0x0B) { put('?'); ++i; continue; }                   // a button glyph
        if ((c >= 6 && c <= 14) || c == 16 || (c >= 18 && c <= 42)) { if (c >= 0x12 && c <= 0x23 && c != 0x14 && c != 0x15 && c != 0x18) put('?'); ++i; continue; }
    }
    while (o > 0 && out[o - 1] == ' ') --o;
    if (cap > 0) out[o] = 0;
    return o;
}
inline bool has_prefix(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }
inline bool name_between(const char* s, const char* pre, const char* suf, char* name, int cap) {
    const size_t ls = strlen(s), lp = strlen(pre), lf = strlen(suf);
    if (ls <= lp + lf || strncmp(s, pre, lp) != 0 || strcmp(s + ls - lf, suf) != 0) return false;
    const size_t n = ls - lp - lf;
    if (n == 0 || n >= static_cast<size_t>(cap)) return false;
    for (size_t i = 0; i < n; ++i) { const char c = s[lp + i]; if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == ' ' || c == '-' || c == '\'')) return false; }
    memcpy(name, s + lp, n); name[n] = 0;
    return true;
}
/// A page's class (English only) and the sphere's name in it ("" none).
inline int classify(const uint8_t* b, int n, char* name, int cap) {
    if (cap > 0) name[0] = 0;
    if (n <= 0) return P_NONE;
    char s[160];
    if (decode(b, n, s, sizeof(s)) == 0) return P_UNKNOWN;
    if (strcmp(s, "Take sphere. Do nothing.") == 0) return P_TAKE_CHOICE;
    if (strcmp(s, "Insert sphere. Do nothing.") == 0) return P_INSERT_CHOICE;
    if (strcmp(s, "You see a sphere-shaped recess.") == 0) return P_RECESS;
    if (strcmp(s, "You can only hold one sphere at a time.") == 0) return P_ONE_ONLY;
    if (name_between(s, "A ", " is inserted here.", name, cap)) return P_TAKE_INFO;
    if (name_between(s, "You are holding a ", ".", name, cap) || name_between(s, "You are holding an ", ".", name, cap)) return P_HOLDING;
    if (name_between(s, "Removed ", ".", name, cap)) return P_REMOVED;
    if (name_between(s, "Inserted ", ".", name, cap)) return P_INSERTED;
    if (cap > 0) name[0] = 0;
    return P_UNKNOWN;
}

// -- when the game takes a confirm (the owner's first test, 13 Nov: presses on a line the moment it showed were lost) -------
// The field's input (0x867950) reads the NEW confirm presses of a frame and asks 0x86C8B0 what a confirm would do: 2 when a
// window of the bank in state 1 or 2 has +0x1D & 0x22 (a choice: the cursor's own input 0x8B62B0 takes the confirm while the
// cursor is up, mode 1, which 0x8B5FB0 sets only at window state 2), 1 when one has +0x16 != 0 or +0x1D & 0x10 (a line: it
// sets the controller's confirm bit, byte 0 bit 2), 0 otherwise (the press is lost). The script's wait for a line (0x860AA0)
// then closes its window when the window is in state 2 with the page ended (render +0x2E == 3) and that bit is set. +0x1D
// bit 0x10 is set by the script's wait call itself (0x85ABD0), which in the log starts about 2 frames after the line reached
// state 2: a press whose first frame is earlier than that is lost (it is not "new" any more afterwards). So a line is ready
// only once its own +0x1D has 0x10; we read it after the game's frame, so a press requested then is new in a later frame.
/// 0x86C8B0, mirrored: what a new confirm would do now (-1: the script windows were not read).
inline int msg_kind(const Head& h) {
    if (!h.mw_ok) return -1;
    for (int i = 0; i < 8; ++i) {
        if (h.mw_state[i] != 1 && h.mw_state[i] != 2) continue;
        if (h.mw_flags[i] & 0x22) return 2;
        if (h.mw_w16[i] || (h.mw_flags[i] & 0x10)) return 1;
    }
    return 0;
}
/// The plain line in window k takes a new confirm now.
inline bool line_ready(const Head& h, const Win& w, int k) {
    return k >= 0 && k < 8 && w.open && w.state == 2 && w.page_end == 3 && h.mw_ok && h.mw_state[k] == 2 && (h.mw_flags[k] & 0x10) != 0 && msg_kind(h) == 1;
}
/// The choice in window k takes a new confirm now (its cursor is up).
inline bool choice_ready(const Head& h, const Win& w, int k) {
    return k >= 0 && k < 8 && w.open && w.state == 2 && w.page_end == 3 && w.choice_mode == 1 && h.mw_ok && h.mw_state[k] == 2;
}

// -- the press sequence ---------------------------------------------------------------------------------------------------
enum Phase { S_IDLE = 0, S_KICK, S_WAIT_OFF, S_WAIT_RECESS, S_WAIT_CHOICE, S_WAIT_RESULT, S_WAIT_CONTROL, S_DONE, S_ABORT, S_N };
static const char* const k_phase_name[S_N] = { "idle", "pressing A (the interaction)", "waiting for the event", "waiting for \"You see a sphere-shaped recess.\"",
    "waiting for the choice", "waiting for \"Removed / Inserted\"", "waiting for control", "done", "stopped" };
enum Why { W_NONE = 0, W_OK, W_TIMEOUT, W_TEXT, W_ROW, W_PLAYER, W_CONTROL, W_TARGET, W_GEN, W_GATE, W_RESULT, W_STUCK, W_NOSTART, W_NOLINE, W_N };
static const char* const k_why_name[W_N] = { "-", "control back", "a step timed out", "unexpected text (another wording or language)", "the choice's cursor was not on row 0",
    "the player pressed a button", "control came back early", "the game chose another target", "the scene changed", "the feature's gates closed", "the variable did not change",
    "the game did not take our presses (the page stayed)", "the game did not start the interaction", "no result line came (the game went on by itself)" };
struct ObsWin { int32_t open, state, mode, row, page, ready; char name[k_name]; };
inline bool is_choice(int page) { return page == P_TAKE_CHOICE || page == P_INSERT_CHOICE; }
/// Window k as the sequence sees it. An open, fully shown window whose text did not read is unknown text (review S4), not
/// "no page": the sequence stops on it. ready: the game takes a new confirm on it now (line_ready / choice_ready).
inline void obs_win(const Head& h, const Win& w, int k, ObsWin& o) {
    o.open = w.open; o.state = w.state; o.mode = w.choice_mode; o.row = w.choice_row; o.name[0] = 0; o.ready = 0;
    if (!w.open) o.page = P_NONE;
    else if (w.n <= 0) o.page = w.state == 2 ? P_UNKNOWN : P_NONE;
    else o.page = classify(w.page, w.n, o.name, k_name);
    if (o.page > P_UNKNOWN) o.ready = (is_choice(o.page) ? choice_ready(h, w, k) : line_ready(h, w, k)) ? 1 : 0;
}
struct Obs {            // one frame for the sequence
    double  t;          // seconds
    bool    control, free_play, gate;   // gate: Head.on
    int32_t carried, talk_target; uint32_t gen;
    bool    player;     // a physical button newly pressed since the last frame (not a stick: phys_step)
    bool    press_idle; // our press channel is free (the last press went out and was let go, or was dropped)
    ObsWin  w[k_wins];
};
constexpr double k_t_off = 1.0, k_t_page = 3.0, k_t_result = 6.0, k_t_control = 5.0, k_press_gap = 0.15;
constexpr double k_t_settle = 0.06;  // a page must have been ready this long before a press (a few game frames of margin)
constexpr double k_t_again = 0.30;   // a page still ready this long after our press on it was let go: the game did not take it; once more
constexpr int    k_presses_page = 3; // at most this many presses on one page
constexpr double k_t_stuck = 0.40;   // ... and still up this long after the last one was let go: stopped, the prompt shown
constexpr double k_t_blind = 1.5;    // a page fully shown this long that never read as ready (a misread of the game's flags): one press as before, no repeat
constexpr double k_t_noline = 1.0;   // the variable moved and no result line opened within this (it opens 2 frames after): none comes (the door pair's second sphere)
/// The outcome is the trial's carried variable (review S1): once it has moved the right way (moved; the caller shows the note
/// at once) every end is "done" (why: how it ended), whatever the pages did after. Before that an end is "stopped".
/// Every press but the first needs its page READY (the game's own test, above) for k_t_settle, and control off. A page still
/// ready k_t_again after our press on it was let go was not answered: it is pressed again, at most k_presses_page times in
/// all, the result line included. That repeat cannot be a press too many: it is requested only on a frame where the line's
/// window is still in state 2 with the script's wait flag set, so the script is still waiting on that very line and control
/// cannot have returned; the first frame of an earlier press is long past (it was let go k_t_again ago), so no earlier press
/// can still answer it; and when the line does take a press the window leaves state 2 in that same game frame, so the next
/// frame we read is not ready and nothing more is requested. (The caller also withdraws a press the game has not polled in
/// time, and one still unpolled when the sequence ends.)
struct Seq {
    int phase = S_IDLE, action = A_NONE, why = W_NONE, hand = -1, widx = -1, presses = 0, carried0 = -1, last_page = P_NONE;
    int pressed_page = P_NONE, page_presses = 0, retries = 0, blind_presses = 0;   // the page our last press answered, our presses on it; all repeats; presses without the ready signal
    bool blind = false;                       // the last press went out on a page that never read as ready
    bool moved = false;                       // the carried variable changed as the action wants
    uint32_t gen = 0;
    double t0 = 0, t_phase = 0, t_press = -1e9, t_rel = -1e9, t_moved = 0;
    double t_rdy[P_N], t_shn[P_N];            // since when each page has been ready / fully shown without a break (< 0: not)
    char name[k_name] = {};   // the sphere's name from the game's text (the note's)
    int steps = 0; int step_page[8] = {}; double step_t[8] = {};   // the pages pressed through (the per-grab log line)

    Seq() { for (int i = 0; i < P_N; ++i) { t_rdy[i] = -1.0; t_shn[i] = -1.0; } }
    bool running() const { return phase >= S_KICK && phase <= S_WAIT_CONTROL; }
    /// While it runs the prompt is hidden in the headset (through the result line: the note at the hand replaces it); never
    /// after an end.
    bool hide_ui() const { return running(); }
    void start(int act, int h, int w, int carried, uint32_t g, double t) {
        *this = Seq(); phase = S_KICK; action = act; hand = h; widx = w; carried0 = carried; gen = g; t0 = t; t_phase = t;
    }
    void go(int p, double t) { phase = p; t_phase = t; }
    void stop(int w) { phase = moved ? S_DONE : S_ABORT; why = w; }
    /// Control is back: done when the sphere moved.
    void back() { if (moved) { phase = S_DONE; why = W_OK; } else stop(phase == S_WAIT_CONTROL ? W_RESULT : W_CONTROL); }
    void note_press(int page, double t) {
        page_presses = page == pressed_page ? page_presses + 1 : 1;
        ++presses; t_press = t; t_rel = -1e9; pressed_page = page;
        if (steps < 8) { step_page[steps] = page; step_t[steps] = t - t0; ++steps; }
    }
    /// The outcome: the carried variable moved as the action wants (a take: from nothing to a sphere; a place: to nothing).
    /// Only once the game started our interaction (control off with our worker chosen: past S_WAIT_OFF), and the caller checks
    /// the scene first (round 2 N11: a value read across a map change or a reload is not an outcome).
    void watch(int carried, double t) {
        if (moved || phase < S_WAIT_RECESS || phase > S_WAIT_CONTROL) return;
        if (carried >= 0 && carried0 >= 0 && (action == A_TAKE ? (carried0 == 0 && carried > 0) : (carried0 > 0 && carried == 0))) { moved = true; t_moved = t; }
    }
    /// The feature's gates closed while it ran (the caller's test): the variable still counts in the same scene, then the end.
    void gate_closed(const Obs& o) {
        if (!running()) return;
        if (o.gen == gen) watch(o.carried, o.t);
        stop(W_GATE);
    }
    /// One frame. Returns 1 when it wants one A pressed now (the caller presses it at once).
    int step(const Obs& o) {
        if (!running()) return 0;
        if (o.gen != gen) { stop(W_GEN); return 0; }   // the scene first: nothing read after a map change or a reload counts
        watch(o.carried, o.t);
        if (o.player && phase != S_WAIT_CONTROL) { stop(W_PLAYER); return 0; }   // waiting for control there is nothing left to press: the player may walk on
        if (t_rel < 0 && o.press_idle && o.t > t_press) t_rel = o.t;           // our last press was let go (t_rel < 0: one is out or held)
        const double dt = o.t - t_phase;
        // the pages open now: anything not of this sequence stops it. shown: fully out and waiting for an answer (a choice:
        // its cursor up); ready: the game takes a confirm on it now
        int open[P_N] = {}, shown[P_N] = {}, rdy[P_N] = {}; int choice_row = -1;
        for (int k = 0; k < k_wins; ++k) {
            const ObsWin& w = o.w[k];
            if (!w.open || w.page == P_NONE) continue;
            const bool ours = action == A_TAKE ? (w.page == P_TAKE_INFO || w.page == P_TAKE_CHOICE || w.page == P_REMOVED)
                                               : (w.page == P_RECESS || w.page == P_HOLDING || w.page == P_INSERT_CHOICE || w.page == P_INSERTED);
            if (!ours) { last_page = w.page; stop(W_TEXT); return 0; }
            open[w.page] = 1;
            if (w.state == 2 && (!is_choice(w.page) || w.mode == 1)) { shown[w.page] = 1; if (w.ready) rdy[w.page] = 1; if (is_choice(w.page)) choice_row = w.row; }
            if (w.name[0]) copy_str(name, w.name, k_name);   // "A <name> is inserted here." / "You are holding a <name>." / the result line
        }
        for (int p = 0; p < P_N; ++p) {
            t_shn[p] = shown[p] ? (t_shn[p] < 0 ? o.t : t_shn[p]) : -1.0;
            t_rdy[p] = rdy[p] ? (t_rdy[p] < 0 ? o.t : t_rdy[p]) : -1.0;
        }
        const int choice = action == A_TAKE ? P_TAKE_CHOICE : P_INSERT_CHOICE, result = action == A_TAKE ? P_REMOVED : P_INSERTED;
        if (shown[choice] && choice_row != 0) { stop(W_ROW); return 0; }   // never an answer on another row
        // the page we answered is still up and waiting: the game did not take the press
        if (pressed_page != P_NONE && !o.control && t_rel >= 0 && shown[pressed_page]) {
            const double since = o.t - t_rel;
            if (blind || page_presses >= k_presses_page) { if (since >= k_t_stuck) { stop(W_STUCK); return 0; } }
            else if (since >= k_t_again && rdy[pressed_page] && o.t - t_rdy[pressed_page] >= k_t_settle) { ++retries; note_press(pressed_page, o.t); return 1; }
        }
        switch (phase) {
        case S_KICK:   // the interaction itself: only from free play, nothing open
            if (!o.gate || !o.control || !o.free_play) { stop(W_GATE); return 0; }
            if (!o.press_idle) { if (dt > k_t_off) stop(W_TIMEOUT); return 0; }
            ++presses; t_press = o.t; t_rel = -1e9; go(S_WAIT_OFF, o.t); return 1;
        case S_WAIT_OFF:
            if (!o.control) {
                if (o.talk_target != widx) { stop(W_TARGET); return 0; }
                go(action == A_TAKE ? S_WAIT_CHOICE : S_WAIT_RECESS, o.t); return 0;
            }
            if (dt > k_t_off) stop(W_NOSTART);
            return 0;
        case S_WAIT_RECESS:
            if (o.control) { back(); return 0; }
            if (open[P_INSERT_CHOICE] || open[P_HOLDING]) { go(S_WAIT_CHOICE, o.t); return first(choice, S_WAIT_RESULT, o, shown, rdy); }   // the line already answered
            if (first(P_RECESS, S_WAIT_CHOICE, o, shown, rdy)) return 1;
            if (dt > k_t_page) stop(W_TIMEOUT);
            return 0;
        case S_WAIT_CHOICE:
            if (o.control) { back(); return 0; }
            if (first(choice, S_WAIT_RESULT, o, shown, rdy)) return 1;
            if (dt > k_t_page) stop(W_TIMEOUT);
            return 0;
        case S_WAIT_RESULT:
            if (o.control) { back(); return 0; }   // moved and no result line: done
            if (first(result, S_WAIT_CONTROL, o, shown, rdy)) return 1;
            if (moved && !open[result] && o.t - t_moved > k_t_noline) { stop(W_NOLINE); return 0; }   // done: nothing is left to answer
            if (dt > k_t_result) stop(W_TIMEOUT);   // moved: done
            return 0;
        case S_WAIT_CONTROL:   // only the result line's repeats (above): never a press on anything else
            if (o.control) { back(); return 0; }
            if (dt > k_t_control) stop(W_TIMEOUT);   // moved: done (a long cutscene)
            return 0;
        default: return 0;
        }
    }
    /// The first press on a page: when it has been ready for k_t_settle; or, never ready but fully shown for k_t_blind, one
    /// press without a repeat. Returns 1 when it pressed (and moved on to phase next).
    int first(int page, int next, const Obs& o, const int* shown, const int* rdy) {
        const bool ready = rdy[page] && o.t - t_rdy[page] >= k_t_settle;
        const bool late = !rdy[page] && shown[page] && o.t - t_shn[page] >= k_t_blind;
        if (!ready && !late) return 0;
        if (!o.press_idle || o.control || o.t - t_press < k_press_gap) return 0;
        note_press(page, o.t); blind = late; if (late) ++blind_presses;
        go(next, o.t); return 1;
    }
};

/// The player's own input for the sequence's stop rule (the input thread): the buttons newly down since the last poll. A
/// stick is not a button: it answers nothing (the choice's row is checked before every press), and in the owner's first test
/// the three "the player pressed a button" stops had no button in the game's own input: a stick pushed past 0.6 counted.
/// sticks: pushes past 0.6 are counted apart, for the log only.
struct Phys { uint16_t prev = 0; bool stick_prev = false; };
inline uint16_t phys_step(Phys& p, uint16_t buttons, bool stick_out, int* stick_pushes) {
    const uint16_t fresh = static_cast<uint16_t>(buttons & ~p.prev);
    if (stick_out && !p.stick_prev && stick_pushes) ++*stick_pushes;
    p.prev = buttons; p.stick_prev = stick_out;
    return fresh;
}

/// The note once the sphere moved: "Picked up <name>" / "Inserted <name>" (the game's name, else the fallback), "" before.
inline void note_text(const Seq& s, const char* fallback, char* out, int cap) {
    if (cap <= 0) return;
    out[0] = 0;
    if (!s.moved) return;
    const char* nm = s.name[0] ? s.name : (fallback && fallback[0] ? fallback : "sphere");
    char b[96]; snprintf(b, sizeof(b), "%s %s", s.action == A_TAKE ? "Picked up" : "Inserted", nm);
    copy_str(out, b, cap);
}
constexpr double k_note_s = 2.0, k_note_fade_s = 0.4;
/// The note's opacity age seconds after it was shown.
inline float note_alpha(double age) {
    if (!(age >= 0.0) || age >= k_note_s) return 0.f;
    if (age <= k_note_s - k_note_fade_s) return 1.f;
    return static_cast<float>((k_note_s - age) / k_note_fade_s);
}

// -- the fast sequence (branch sphere-fast; ABI 67) -------------------------------------------------------------------------
// While a grab sequence of ours runs with control off, the game's world runs `factor` ticks per main-loop pass instead of one
// (its own "exec frames" count: Sg_MainLoop's update loop asks Sg_GetCurExecFrames 0x82D7E0 before every tick, and builds one
// draw whatever the count; ffx-rust docs/formats/frame-timing.md). Nothing is written to the game: managed's detour on that
// getter (sphere_fast.cs) asks ffxvr_sph_fast_exec, which multiplies an answer of exactly 1 and passes every other value
// through (0 is a draw-only pass of FFX-60fps, which sets the game's byte to 1 or 0 before every pass and would overwrite a
// written value). So there is no game value to restore, and "never left sped up" needs no restore to succeed.
// Two places decide, and the game gets the factor only when both agree:
//   - the render thread, once a frame (Fast::step from ffxvr_native.cpp sfast_frame, at every Present whatever the headset
//     does): is a sequence of ours past the interaction's start, control off, the gates open, the same scene, the state
//     fresh, under the cap? It publishes the factor or 1, with a time stamp; a published answer is good for k_fast_lease_ms
//     only (fast_answer): if the Presents stop, the game is back at its own speed 0.1 s later with nobody doing anything.
//   - the game's own thread, at every ask (fast_ask from ffxvr_sph_fast_exec; review round 1: everything that is a race
//     between the render, input and game threads is decided here, from what the input thread stamped at the instant it
//     handed the game an input, and from what the game thread itself reads):
//       our press     the pad's press edges are made once per pass, so with several ticks in a pass every tick would see the
//                     same new confirm. From the instant a press of ours is set (the word the input thread reads) until it
//                     is let go, and through the pass under way then and the next (k_fast_guard_passes; by time,
//                     k_fast_guard_ms, only if the pass counter did not read), the answer is the game's own: the pass that
//                     carries our press's edge runs one tick, as in normal play, and every timing rule of the press sequence
//                     holds as it was written. A button let go is no edge, so nothing more is needed after. While a page
//                     waits for our press nothing else in the script moves; what this costs is the hold's own 0.1 s a page.
//       the player's  any button or stick push of the player's own (stamped by the input thread before the game gets the
//                     state): the game's own answer for k_fast_player_ms or k_fast_player_passes (the later).
//       control       the player actor is set (the game's own word, read at the ask): free play never runs at the factor,
//                     from the very tick that hands control back.
//       a slow pass   once a pass's ticks have taken k_fast_pass_ms (from its first ask to the ask after a tick: the draw
//                     that follows is not counted) the answer is the game's own for the rest of that pass: N ticks never
//                     push a pass far enough to feed the game's catch-up (a second Sg_MainLoop per frame from 66.7 ms of
//                     frame time) or FFX-60fps' backlog. One such pass ends nothing (a hitch). After k_fast_slow_passes of
//                     them the render thread halves the factor (never under 2) and counts anew; at 2 it ends the run for
//                     that sequence (FE_SLOW). (Review round 2, S1: the first version ended the run at the first such pass
//                     and timed the draw too.)
// The stamps carry the game's main-loop pass counter as well as the time, so a stall between the input and the pass that
// reads it (a hitch, a slow GPU frame) cannot age a guard away before its pass ran: the player's guard holds while EITHER
// is young, our release's by passes.
constexpr int      k_fast_min = 1, k_fast_max = 12, k_fast_def = 6;   // ini sphere_fast_factor (1 = off)
constexpr double   k_fast_cap_s = 3.0;      // one sequence is sped up for at most this long (real time), then never again
constexpr uint32_t k_fast_lease_ms = 100;   // a published answer's life
constexpr uint32_t k_fast_guard_ms = 70, k_fast_guard_passes = 1;      // after our press was let go: the pass under way and the next; 70 ms (two 30 Hz passes) only without a pass counter
constexpr uint32_t k_fast_player_ms = 150, k_fast_player_passes = 8;   // after the player's own input: 8 passes is one logic tick of FFX-60fps at 240 fps
// The pass budget. A pass at the game's speed is one tick and the frame's draw inside 33.3 ms (16.7 ms with FFX-60fps at
// 60). 25 ms of ticks leaves the draw 8 ms of a 30 Hz frame and keeps a frame far under the 66.7 ms at which Sg_FrameSync
// adds a second pass (and FFX-60fps a backlog pass); at 60 fps it makes at most one late frame per pass, which neither
// counts as a backlog. Past it the pass's remaining ticks are not run; three such passes halve the factor.
constexpr double   k_fast_pass_ms = 25.0;
constexpr int      k_fast_slow_passes = 3;  // passes over the budget before the factor is halved (at 2: before the run ends)
constexpr int      k_fast_floor = 2;        // the factor is never stepped down under this
inline int fast_clamp(int f) { return f < k_fast_min ? k_fast_min : f > k_fast_max ? k_fast_max : f; }
enum FastEnd { FE_NONE = 0, FE_SEQ, FE_CONTROL, FE_CAP, FE_GATE, FE_GEN, FE_STALE, FE_SETTING, FE_SHUTDOWN, FE_SLOW, FE_N };
static const char* const k_fast_end_name[FE_N] = { "-", "the sequence ended", "control came back", "the 3 s cap", "the feature's gates closed (a menu, a battle, the view, the headset)",
    "the scene changed", "no fresh state (the headset's frame or managed's push stopped)", "the setting is off or the factor is 1", "the module is shutting down",
    "the ticks of a main-loop pass took longer than 25 ms three times at factor x2 (the scene is too heavy to speed up)" };
struct FastIn {
    double t;            // seconds
    bool   setting;      // ini sphere_fast, from a settings push that is not stale
    int    factor;       // ini sphere_fast_factor
    bool   fresh;        // managed's state is fresh AND the sequence was stepped this very frame (sph_tick ran)
    bool   gate;         // sph_tick's `on`: a trial with a socket table, first person, the VR hands, the field, the headset
    bool   running;      // Seq::running()
    int    phase;        // Seq::phase
    double seq_t0;       // Seq::t0: which sequence this is
    bool   gen_same;     // the scene is the one the sequence started in
    bool   control;      // the player has control
    bool   press_out;    // a press of ours is out (asked for and not yet let go)
    bool   player;       // the player's own input is recent (the input thread's stamp)
    int    slow_passes;  // the passes whose ticks went over the budget during this run (counted by the game's thread; 0 at a run's start)
};
/// The factor after a step down: half, never under k_fast_floor (12 -> 6 -> 3 -> 2).
inline int fast_half(int f) { const int h = f / 2; return h < k_fast_floor ? k_fast_floor : h; }
struct Fast {
    bool   active = false; int why = FE_NONE;
    double t0 = 0.0, run_seq = -1.0, spent_t0 = -1.0;   // the run's start and its sequence; the sequence whose run hit the cap or the budget
    int    factor = 1, factor0 = 1, frames = 0, frames_fast = 0;   // of the run: the factor now and the one it started with, its frames, those answered with a factor
    int    slow_base = 0, downs = 0;                   // the over-budget passes already answered by a step down; the step downs
    /// Why the game must run at its own speed now (FE_NONE: it may be sped up). The cap and the step down are step()'s.
    static int blocked(const FastIn& in) {
        if (!in.setting || fast_clamp(in.factor) <= 1) return FE_SETTING;
        if (!in.fresh) return FE_STALE;
        if (!in.running || in.phase <= S_WAIT_OFF || in.phase > S_WAIT_CONTROL) return FE_SEQ;   // only once the game took our interaction
        if (!in.gate) return FE_GATE;
        if (!in.gen_same) return FE_GEN;
        if (in.control) return FE_CONTROL;
        return FE_NONE;
    }
    /// One frame. Returns the multiplier in force until the next call: 1 (the game's own speed) or the factor.
    /// The guard here is per press, not per channel (review B1): it holds on every frame a press is out, whichever press
    /// it is. (The game's thread has the same rule and the release's tail from the input thread's own stamps, fast_ask:
    /// this one keeps the published value and the log honest.)
    int step(const FastIn& in) {
        const int b = blocked(in);
        if (active) {
            if (in.seq_t0 != run_seq) { active = false; why = FE_SEQ; }   // another sequence: a run of its own, with its own cap, from the next frame
            else if (b != FE_NONE) { active = false; why = b; }
            else if (in.slow_passes - slow_base >= k_fast_slow_passes) {   // too heavy at this factor: half of it, or at the floor the end
                if (factor > k_fast_floor) { factor = fast_half(factor); slow_base = in.slow_passes; ++downs; }
                else { active = false; why = FE_SLOW; spent_t0 = in.seq_t0; }
            }
            else if (!(in.t - t0 <= k_fast_cap_s) || in.t < t0) { active = false; why = FE_CAP; spent_t0 = in.seq_t0; }
        }
        else if (b == FE_NONE && in.seq_t0 != spent_t0) {
            active = true; why = FE_NONE; t0 = in.t; run_seq = in.seq_t0; factor = factor0 = fast_clamp(in.factor); frames = 0; frames_fast = 0;
            slow_base = 0; downs = 0;   // (the glue starts the game thread's count at 0 with the run)
        }
        if (!active) return 1;
        ++frames;
        if (in.press_out || in.player) return 1;   // the press guard, the player's input
        ++frames_fast;
        return factor;
    }
    void stop(int w) { if (active) { active = false; why = w; } }
};
/// The published answer as the game's main thread may use it now: 1 unless it is a factor, younger than the lease (the tick
/// counts are unsigned milliseconds: the difference is right across a wrap), and the module is not going away.
inline int fast_answer(int mul, uint32_t now_ms, uint32_t stamp_ms, bool dead) {
    if (dead || mul <= 1 || mul > k_fast_max) return 1;
    if (static_cast<uint32_t>(now_ms - stamp_ms) > k_fast_lease_ms) return 1;
    return mul;
}
/// What the game's exec-frames getter returns: its own answer, multiplied only when that answer is exactly 1 (0: a draw-only
/// pass or the debug pause; 2 and more: somebody else's speed-up; a negative: never).
inline int fast_exec(int game, int mul) { return (game == 1 && mul > 1 && mul <= k_fast_max) ? mul : game; }

/// An input the input thread handed the game: when (its tick count), and the game's main-loop pass counter then.
struct EdgeStamp { bool have; uint32_t ms; bool pass_ok; uint32_t pass; };
/// Is the stamp still young: within ms_max of now, or within pass_max passes of the pass under way (either: a stall cannot
/// age it away before its pass ran)? A pass counter that did not read counts as young by time only.
inline bool edge_young(const EdgeStamp& e, uint32_t now_ms, bool pass_ok, uint32_t pass, uint32_t ms_max, uint32_t pass_max) {
    if (!e.have) return false;
    if (static_cast<uint32_t>(now_ms - e.ms) < ms_max) return true;
    return e.pass_ok && pass_ok && static_cast<uint32_t>(pass - e.pass) <= pass_max;
}
/// Our press was just let go: the pass under way at the release and the next one (the pass that carried its edge began
/// while it was set, and is that pass or an earlier one). By time only when a pass counter did not read.
inline bool release_young(const EdgeStamp& e, uint32_t now_ms, bool pass_ok, uint32_t pass) {
    if (!e.have) return false;
    if (e.pass_ok && pass_ok) return static_cast<uint32_t>(pass - e.pass) <= k_fast_guard_passes;
    return static_cast<uint32_t>(now_ms - e.ms) < k_fast_guard_ms;
}
/// The pass budget, on the game's thread (its only user). It times a pass's TICKS: from the pass's first ask (the tick
/// loop's entry check) to each ask that follows a tick. The loop asks once before the first tick and once after every tick,
/// and goes on while the answer is above the ticks run; so the ask with index j (0 the entry) is still the tick loop's as
/// long as every answer before it kept the loop going, and the first answer that is not above its own index ends the loop
/// (answered()). Every later ask of the pass is the draw's (the effects' loop) or another caller's: not timed, and it gets
/// the verdict the ticks left (review round 2, S1). The pass counter names the pass.
struct PassClock {
    bool have = false, over = false, went_over = false, ticking = false;
    uint32_t pass = 0; int idx = 0; double t0 = 0.0, worst_ms = 0.0;
    /// One ask at time now (seconds). Returns true while the pass under way is over the budget; went_over: this ask found it.
    bool ask(bool pass_ok, uint32_t p, double now) {
        went_over = false;
        if (!pass_ok) { have = false; over = false; ticking = false; return false; }   // no counter: no budget (the lease and the cap still bound the run)
        if (!have || p != pass) { have = true; pass = p; t0 = now; over = false; ticking = true; idx = 0; return false; }
        if (idx < 1000000) ++idx;
        if (!ticking) return over;
        const double ms = (now - t0) * 1e3;
        if (ms > worst_ms) worst_ms = ms;
        if (!over && (!(ms <= k_fast_pass_ms) || now < t0)) { over = true; went_over = true; }
        return over;
    }
    /// The answer the ask just made got: one that is not above the ask's index ends the tick loop.
    void answered(int r) { if (r <= idx) ticking = false; }
};
/// Is the 4-byte word at va (image base 0x400000) inside an image of image_size bytes (review round 2, N1: the game
/// thread's two words are read plainly; this is checked once)?
inline bool image_has(uint32_t va, uint32_t image_size) { return va >= 0x400000u && image_size >= 4u && va - 0x400000u <= image_size - 4u; }
enum FastNo { FN_NONE = 0, FN_OFF, FN_GAME, FN_PRESS, FN_RELEASE, FN_PLAYER, FN_CONTROL, FN_SLOW, FN_N };   // why an ask got the game's own answer
struct FastAsk {
    int      game;        // the game's own answer
    int      mul;         // the published multiplier
    uint32_t now_ms, stamp_ms; bool dead;   // fast_answer's
    bool     press_out;   // a press of ours is set now (the very word the input thread reads)
    EdgeStamp rel;        // our last press was let go (the input thread, before it clears the word; the render thread's withdrawals too)
    EdgeStamp phys;       // the player's own button or stick push (the input thread, before the game gets the state)
    bool     pass_ok; uint32_t pass;   // the game's main-loop pass counter now
    bool     control;     // the game's player actor is set: the player has control
    bool     slow;        // PassClock: this pass is over the budget
};
/// The answer the game's exec-frames getter gives (the game's main thread, every ask). The factor only when the game's own
/// answer is exactly 1, a fresh factor is published, and none of the game thread's own reasons holds; *why says which did.
inline int fast_ask(const FastAsk& a, int* why = nullptr) {
    int w = FN_NONE;
    const int m = fast_answer(a.mul, a.now_ms, a.stamp_ms, a.dead);
    if (m <= 1) w = FN_OFF;
    else if (a.game != 1) w = FN_GAME;
    else if (a.press_out) w = FN_PRESS;
    else if (release_young(a.rel, a.now_ms, a.pass_ok, a.pass)) w = FN_RELEASE;
    else if (edge_young(a.phys, a.now_ms, a.pass_ok, a.pass, k_fast_player_ms, k_fast_player_passes)) w = FN_PLAYER;
    else if (a.control) w = FN_CONTROL;
    else if (a.slow) w = FN_SLOW;
    if (why) *why = w;
    return w == FN_NONE ? fast_exec(a.game, m) : a.game;
}

}  // namespace sph
