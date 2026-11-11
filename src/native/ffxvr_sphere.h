// SPDX-License-Identifier: MIT
// ffxvr_sphere.h -- sphere hands, stages 1 + 2 (research/sphere-hands-design.md sections 9 and 10): in a Cloister of Trials, a
// grip of the VR hand at a sphere or socket takes or places the sphere, with no prompt. Pure logic: no Windows, no D3D, no
// OpenXR, no logging, no allocation. ffxvr_native.cpp (the sph_* glue) feeds it what managed read from the game (sphere_log.cs:
// the talk-enabled workers, the sockets, the trial's variables, the text windows) and the drawn hands, and acts on its answers
// (the labels, the grips held back from the game, one A press at a time). A unit test drives all of it (scratch sph_chk).
//
// Game facts (the capture of 10 Nov, logs9; our exe's 0x866CC0 / 0x866980 / 0x867950):
//   - The game picks the talk target per press: every talk-enabled worker's test, in worker order; a box zone (type 3) takes
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
    char    carried_name[k_name];
};

// -- the game's choice, mirrored ----------------------------------------------------------------------------------------
enum Cand { C_NO = 0, C_YES = 1, C_UNSURE = 2 };
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
        in = d2 <= rr; *metric = d2;
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
constexpr int k_pred_none = -1, k_pred_unsure = -2;
/// The worker the game would choose for a confirm now (its widx), k_pred_none, or k_pred_unsure (a candidate with a facing
/// rule). zones in the game's order (ascending widx).
inline int predict(const Zone* z, int n, const float p[3], float radius) {
    int best = k_pred_none; float bestv = 0.f;
    for (int i = 0; i < n; ++i) {
        float m = 0.f;
        const int c = candidate(z[i], p, radius, &m);
        if (c == C_UNSURE) return k_pred_unsure;
        if (c == C_YES && (best == k_pred_none || m < bestv)) { best = z[i].widx; bestv = m; }
    }
    return best;
}

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
inline float dist3(const float a[3], const float b[3]) { const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return sqrtf(x * x + y * y + z * z); }
struct Pick { int target = -1, hand = -1, act = A_NONE; float d = 1e9f; bool step_closer = false; };
/// The nearest target to either drawn palm (game space) within k_label_m, its action, and whether the game would choose it.
inline Pick pick(const Head& h, const Target* t, int nt, const float palm[2][3], const bool palm_ok[2], float units_per_m, int predicted) {
    Pick p;
    if (!(units_per_m > 0.f)) return p;
    for (int i = 0; i < nt; ++i)
        for (int k = 0; k < 2; ++k) {
            if (!palm_ok[k]) continue;
            const float d = dist3(palm[k], t[i].anchor) / units_per_m;
            if (d < p.d) { p.d = d; p.target = i; p.hand = k; }
        }
    if (p.target < 0 || !(p.d < k_label_m)) return Pick();
    p.act = action(h.carried, t[p.target]);
    p.step_closer = (p.act == A_TAKE || p.act == A_PLACE) && predicted != t[p.target].widx;
    return p;
}
/// The label's text (ASCII). A name the table does not know reads "Sphere".
inline void label_text(const Pick& p, const Target* t, const char* carried_name, char* out, int cap) {
    if (cap <= 0) return;
    out[0] = 0;
    if (p.target < 0 || p.act == A_NONE) return;
    const char* nm = t[p.target].name[0] ? t[p.target].name : "Sphere";
    const char* cn = carried_name && carried_name[0] ? carried_name : "sphere";
    char b[96];
    if (p.step_closer) snprintf(b, sizeof(b), "Step closer");
    else if (p.act == A_TAKE) snprintf(b, sizeof(b), "Take %s", nm);
    else if (p.act == A_PLACE) snprintf(b, sizeof(b), "Insert %s", cn);
    else if (p.act == A_FULL) snprintf(b, sizeof(b), "Hands full");
    else snprintf(b, sizeof(b), "Empty recess");
    copy_str(out, b, cap);
}

// -- the grips ------------------------------------------------------------------------------------------------------------
struct Grip { bool down = false, owned = false; };
/// The press edge (above 0.7 after below 0.5; a quick squeeze counts). Returns 1 when this edge is a grab: the hand was in
/// reach of a target. The grip stays owned until it is let go.
inline int grip_step(Grip& g, float squeeze, bool in_reach) {
    const bool was = g.down;
    if (!g.down && squeeze >= 0.70f) g.down = true;
    else if (g.down && squeeze < 0.50f) g.down = false;
    if (!g.down) { g.owned = false; return 0; }
    if (!was && in_reach) { g.owned = true; return 1; }
    return 0;
}
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

// -- the press sequence ---------------------------------------------------------------------------------------------------
enum Phase { S_IDLE = 0, S_KICK, S_WAIT_OFF, S_WAIT_RECESS, S_WAIT_CHOICE, S_WAIT_RESULT, S_WAIT_CONTROL, S_DONE, S_ABORT, S_N };
static const char* const k_phase_name[S_N] = { "idle", "pressing A (the interaction)", "waiting for the event", "waiting for \"You see a sphere-shaped recess.\"",
    "waiting for the choice", "waiting for \"Removed / Inserted\"", "waiting for control", "done", "stopped" };
enum Why { W_NONE = 0, W_OK, W_TIMEOUT, W_TEXT, W_ROW, W_PLAYER, W_CONTROL, W_TARGET, W_GEN, W_GATE, W_RESULT, W_N };
static const char* const k_why_name[W_N] = { "-", "done", "a step timed out", "unexpected text (another wording or language)", "the choice's cursor was not on row 0",
    "the player pressed a button", "control came back early", "the game chose another target", "the scene changed", "the feature's gates closed", "the variable did not change" };
struct ObsWin { int32_t open, state, mode, row, page; char name[k_name]; };
struct Obs {            // one frame for the sequence
    double  t;          // seconds
    bool    control, free_play, gate;   // gate: Head.on
    int32_t carried, talk_target; uint32_t gen;
    bool    player;     // a physical button pressed (new) or a stick pushed since the last frame
    bool    press_idle; // our press channel is free (the last press went out and was let go)
    ObsWin  w[k_wins];
};
constexpr double k_t_off = 1.0, k_t_page = 3.0, k_t_result = 6.0, k_t_control = 5.0, k_press_gap = 0.15;
struct Seq {
    int phase = S_IDLE, action = A_NONE, why = W_NONE, hand = -1, widx = -1, presses = 0, carried0 = -1, last_page = P_NONE;
    uint32_t gen = 0;
    double t0 = 0, t_phase = 0, t_press = -1e9;
    char name[k_name] = {};   // the sphere's name from the game's text (the note's)
    int steps = 0; int step_page[8] = {}; double step_t[8] = {};   // the pages pressed through (the per-grab log line)

    bool running() const { return phase >= S_KICK && phase <= S_WAIT_CONTROL; }
    /// While it runs the prompt is hidden in the headset; never after a stop.
    bool hide_ui() const { return running(); }
    void start(int act, int h, int w, int carried, uint32_t g, double t) {
        *this = Seq(); phase = S_KICK; action = act; hand = h; widx = w; carried0 = carried; gen = g; t0 = t; t_phase = t;
    }
    void go(int p, double t) { phase = p; t_phase = t; }
    void stop(int w) { phase = S_ABORT; why = w; }
    void note_press(int page, double t) { ++presses; t_press = t; if (steps < 8) { step_page[steps] = page; step_t[steps] = t - t0; ++steps; } }
    /// One frame. Returns 1 when it wants one A pressed now (the caller presses it at once).
    int step(const Obs& o) {
        if (!running()) return 0;
        if (o.gen != gen) { stop(W_GEN); return 0; }
        if (o.player) { stop(W_PLAYER); return 0; }
        const double dt = o.t - t_phase;
        // the pages open now: anything not of this sequence stops it
        int seen[P_N] = {}; int choice_row = -1, choice_mode = 0; const ObsWin* named = nullptr;
        for (int k = 0; k < k_wins; ++k) {
            const ObsWin& w = o.w[k];
            if (!w.open || w.page == P_NONE) continue;
            const bool ours = action == A_TAKE ? (w.page == P_TAKE_INFO || w.page == P_TAKE_CHOICE || w.page == P_REMOVED)
                                               : (w.page == P_RECESS || w.page == P_HOLDING || w.page == P_INSERT_CHOICE || w.page == P_INSERTED);
            if (!ours) { last_page = w.page; stop(W_TEXT); return 0; }
            if (w.state == 2) seen[w.page] = 1;
            if ((w.page == P_TAKE_CHOICE || w.page == P_INSERT_CHOICE) && w.state == 2) { choice_row = w.row; choice_mode = w.mode; }
            if ((w.page == P_REMOVED || w.page == P_INSERTED) && w.name[0]) named = &w;
        }
        const bool can_press = o.press_idle && !o.control && o.t - t_press >= k_press_gap;
        switch (phase) {
        case S_KICK:   // the interaction itself: only from free play, nothing open
            if (!o.gate || !o.control || !o.free_play) { stop(W_GATE); return 0; }
            if (!o.press_idle) { if (dt > k_t_off) stop(W_TIMEOUT); return 0; }
            ++presses; t_press = o.t; go(S_WAIT_OFF, o.t); return 1;
        case S_WAIT_OFF:
            if (!o.control) {
                if (o.talk_target != widx) { stop(W_TARGET); return 0; }
                go(action == A_TAKE ? S_WAIT_CHOICE : S_WAIT_RECESS, o.t); return 0;
            }
            if (dt > k_t_off) stop(W_TIMEOUT);
            return 0;
        case S_WAIT_RECESS:
            if (o.control) { stop(W_CONTROL); return 0; }
            if (seen[P_INSERT_CHOICE]) { go(S_WAIT_CHOICE, o.t); return step_choice(o, choice_mode, choice_row, can_press); }   // the line already answered
            if (seen[P_RECESS] && can_press) { note_press(P_RECESS, o.t); go(S_WAIT_CHOICE, o.t); return 1; }
            if (dt > k_t_page) stop(W_TIMEOUT);
            return 0;
        case S_WAIT_CHOICE:
            if (o.control) { stop(W_CONTROL); return 0; }
            return step_choice(o, choice_mode, choice_row, can_press);
        case S_WAIT_RESULT: {
            if (o.control) { stop(W_CONTROL); return 0; }
            const int want = action == A_TAKE ? P_REMOVED : P_INSERTED;
            if (seen[want] && can_press) {
                if (named) copy_str(name, named->name, k_name);
                note_press(want, o.t); go(S_WAIT_CONTROL, o.t); return 1;
            }
            if (dt > k_t_result) stop(W_TIMEOUT);
            return 0;
        }
        case S_WAIT_CONTROL:   // never a press here
            if (o.control) {
                const bool moved = o.carried >= 0 && carried0 >= 0 && (action == A_TAKE ? o.carried > 0 && carried0 == 0 : o.carried == 0 && carried0 > 0);
                if (moved) { phase = S_DONE; why = W_OK; } else stop(W_RESULT);
                return 0;
            }
            if (dt > k_t_control) stop(W_TIMEOUT);
            return 0;
        default: return 0;
        }
    }
    int step_choice(const Obs& o, int mode, int row, bool can_press) {
        const int want = action == A_TAKE ? P_TAKE_CHOICE : P_INSERT_CHOICE;
        int seen_choice = 0;
        for (int k = 0; k < k_wins; ++k) if (o.w[k].open && o.w[k].page == want && o.w[k].state == 2) seen_choice = 1;
        if (seen_choice && mode == 1) {
            if (row != 0) { stop(W_ROW); return 0; }
            if (!can_press) return 0;
            note_press(want, o.t); go(S_WAIT_RESULT, o.t); return 1;
        }
        if (o.t - t_phase > k_t_page) stop(W_TIMEOUT);
        return 0;
    }
};

/// The note after a sequence: "Picked up <name>" / "Inserted <name>" (the game's name, else the fallback), "" for no note.
inline void note_text(const Seq& s, const char* fallback, char* out, int cap) {
    if (cap <= 0) return;
    out[0] = 0;
    if (s.phase != S_DONE) return;
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

}  // namespace sph
