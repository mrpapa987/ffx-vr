// SPDX-License-Identifier: MIT
// ffxvr_odgest.h -- overdrive gestures (roadmap 4.3, 6 Nov 2026), pure logic: no Windows, no D3D, no logging, no allocation.
// ffxvr_native.cpp runs the classifiers once a game frame on the render thread (od_gest_tick, from xr_tick) and the stick
// limiter in the game's XInputGetState (vrpad_apply, the input thread); a unit test drives all of it with synthetic hand paths
// and a transcription of the game's spin counter.
//
//   window / inject_gate  which overdrive's input window is open (exactly one state word at 4, the shared timer running) and
//                         whether its gestures may inject now (the settings, the headset, the VR pad)
//   Circle / CircleMix    Lulu's Fury: a hand's circles in any plane -> an unwrapped angle (the turns the hand made)
//   Stick                 the input thread's right-stick vector: the game's quadrant centres, one quadrant at a time toward
//                         that angle, each held for 2 new packets and 60 ms (never a skipped quadrant); backlog under a turn
//   Stroke / classify     Auron's slashes (left / right / up / down on the head's yaw axes, diagonals refused) and punches
//                         (thrust, mapped to the hand's shoulder); Tidus's swing (any fast stroke, fired at its onset)
//   PressQ                the recognised presses waiting for the press slot (one button at a time: Auron's compare is an
//                         equality, so two buttons in one poll would be a wrong input)
//   Sess / summary        the odg: line, one per overdrive window
//
// Game facts (our exe, CHECKED in re/dis_full.txt unless marked; roadmap-design.md 4.3, tas.md; facts only, no code from any
// project): Lulu's state 4 handler 0x891CCF reads two analog bytes (0x888C20 axis 0 = x, 1 = y; 0x80 centre) and records one of
// four DIAGONAL quadrants (x >= 0xA0 & y < 0x60 -> 4, x >= 0xA0 & y >= 0xA0 -> 5, x < 0x60 & y >= 0xA0 -> 6, x < 0x60 &
// y < 0x60 -> 7, else none) when it differs from the last recorded; five recorded quadrants equal to s, s+1, s+2, s+3, s
// (either turning order, 7 wraps to 4) count one spin (sub-counter 0x133D71E + 1; counter 0x133D71F + 1 when the sub-counter
// reaches table[counter] at 0x133F798, capped at 16); a mismatch drops the turn. A full-length vector is in a quadrant for 61
// degrees of every 90. Auron's state 4 (0x890FE5): the new-press word u16 0x25D09D4 equal to seq[progress] -> progress + 1,
// another non-zero press -> progress 0. Tidus's state 4 (0x892386): a confirm press inside the zone -> 5, outside -> a sound
// and the gauge keeps running.
#pragma once
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ffxvr_gates.h"   // od::Raw, the overdrive block's layout

namespace odg {

struct V3 { float x = 0.f, y = 0.f, z = 0.f; };
inline V3 v3(float x, float y, float z) { V3 v; v.x = x; v.y = y; v.z = z; return v; }
inline V3 operator+(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
inline V3 operator-(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
inline V3 operator*(V3 a, float s) { return v3(a.x * s, a.y * s, a.z * s); }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float len(V3 a) { return std::sqrt(dot(a, a)); }

constexpr float k_two_pi = 6.28318531f;
constexpr int32_t k_turn = 65536;   // the published angle's units per turn (an int32 wraps after 32768 turns; differences stay right)

// -- the gate ------------------------------------------------------------------------------------------------------------
enum Od { OD_NONE = 0, OD_LULU = 1, OD_AURON = 2, OD_TIDUS = 3, OD_N };
static const char* const k_od_name[OD_N] = { "none", "Lulu (Fury)", "Auron (Bushido)", "Tidus (Swordplay)" };

/// The overdrive whose input window is open now: in battle, a read from this frame or the last (fresh), exactly one of the
/// three words non-zero and at 4 (Lulu spinning, Auron's input, Tidus's gauge), Wakka's byte 0, the shared timer running (2).
inline int window(const od::Raw& r, bool battle, bool fresh) {
    if (!battle || !fresh || !r.ok || !r.timer_ok || r.timer != 2 || r.wakka != 0) return OD_NONE;
    if (r.lulu == 4 && r.auron == 0 && r.tidus == 0) return OD_LULU;
    if (r.auron == 4 && r.lulu == 0 && r.tidus == 0) return OD_AURON;
    if (r.tidus == 4 && r.lulu == 0 && r.auron == 0) return OD_TIDUS;
    return OD_NONE;
}

/// The settings (ini od_gestures, od_gest_lulu / _auron / _tidus, od_gest_haptics, od_lulu_stick, od_circle_cm, od_slash_speed).
struct Cfg {
    bool on = true, lulu = true, auron = false, tidus = true, haptics = true;
    int lulu_stick = 0;           // 0 the right stick (the PS2 byte order: GUESS-strong), 1 the left, 2 both
    float circle_r = 0.04f;       // m: a circle smaller than this is jitter
    float slash_speed = 1.6f;     // m/s: a stroke starts above this
};
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/// The overdrive whose gestures may inject now (OD_NONE: nothing): its window, the master and its own setting, the headset
/// showing (headset) and the VR pad merged (vrpad: on, the session running, synced).
inline int inject_gate(int win, const Cfg& c, bool headset, bool vrpad) {
    if (!c.on || !headset || !vrpad) return OD_NONE;
    if (win == OD_LULU && c.lulu) return OD_LULU;
    if (win == OD_AURON && c.auron) return OD_AURON;
    if (win == OD_TIDUS && c.tidus) return OD_TIDUS;
    return OD_NONE;
}

/// More of the od block (od::k_blk_va, k_blk_len bytes) than od::Raw parses.
struct Extra {
    int16_t auron_prog = 0; int32_t auron_len = 0; uint32_t auron_ptr = 0;   // 0x133D6FC, 0x133D700, 0x133D704 (-> u16[len])
    uint8_t lulu_sub = 0, lulu_cnt = 0, lulu_hist_n = 0;                    // 0x133D71E spins, 0x133D71F the counter, 0x133D731 recorded quadrants
};
constexpr int k_x_prog = 0x0A, k_x_len = 0x0E, k_x_ptr = 0x12, k_x_sub = 0x2C, k_x_cnt = 0x2D, k_x_hist_n = 0x3F;
static_assert(k_x_hist_n < static_cast<int>(od::k_blk_len), "inside the od block");
inline Extra parse_extra(const uint8_t* b) {
    Extra e;
    e.auron_prog = od::s16_at(b, k_x_prog); e.auron_len = od::s32_at(b, k_x_len); e.auron_ptr = static_cast<uint32_t>(od::s32_at(b, k_x_ptr));
    e.lulu_sub = b[k_x_sub]; e.lulu_cnt = b[k_x_cnt]; e.lulu_hist_n = b[k_x_hist_n];
    return e;
}
constexpr int k_seq_max = 32;   // Auron's sequence is read only when 1 <= length <= this
inline bool seq_len_ok(int32_t n) { return n >= 1 && n <= k_seq_max; }

// -- Lulu: circles -----------------------------------------------------------------------------------------------------
// Any plane: the hand minus a running centre (tau 0.35 s) is the radius r; the plane's normal is the running mean of
// r_prev x r (tau 0.25 s), so it follows whatever plane the hand circles in (facing you, flat like stirring, tilted) and its
// sign follows the turning order: the angle advances (positive) for a clean circle either way round, which the game accepts
// (both orders count). Gates: |r| >= circle_r (jitter and drifts give a small r), coherence = |mean of r_prev x r| / mean
// of |r_prev x r| >= 0.6 (random motion has no common axis), a step under 100 degrees a sample and a hand under 8 m/s (a
// tracking jump starts again), and at least 1 rad/s of turning (a hand that stopped is not circling).
struct CircleCfg { float r_min = 0.04f, centre_tau = 0.35f, normal_tau = 0.25f, coherence = 0.6f, max_step = 1.745f, max_speed = 8.f, min_rate = 1.f; };
struct Circle {
    bool have_c = false, have_prev = false, circling = false; V3 c, prev, n_acc, last_p; float mag_acc = 0.f; double t = 0.0;
    float turns = 0.f;   // the turns this hand gave the output (kept by lose(); a window's reset clears it)
    void lose() { have_c = have_prev = circling = false; n_acc = V3{}; mag_acc = 0.f; }
    /// One sample (p: the grip position, m; t_s: seconds). Returns the signed angle step to add (radians; 0 when not circling).
    float step(V3 p, bool valid, double t_s, const CircleCfg& k) {
        if (!valid) { lose(); return 0.f; }
        const double dt = have_c ? t_s - t : 0.0;
        if (have_c && (dt <= 0.0 || dt > 0.25 || len(p - last_p) > k.max_speed * static_cast<float>(dt))) lose();   // a gap, time going back, a tracking jump: start again
        t = t_s; last_p = p;
        if (!have_c) { have_c = true; c = p; have_prev = circling = false; n_acc = V3{}; mag_acc = 0.f; return 0.f; }
        c = c + (p - c) * static_cast<float>(1.0 - std::exp(-dt / k.centre_tau));
        const V3 r = p - c;
        if (len(r) < k.r_min) { have_prev = circling = false; return 0.f; }
        if (!have_prev) { prev = r; have_prev = true; return 0.f; }
        const V3 cr = cross(prev, r);
        const float b = static_cast<float>(1.0 - std::exp(-dt / k.normal_tau));
        n_acc = n_acc + (cr - n_acc) * b; mag_acc += (len(cr) - mag_acc) * b;
        const V3 r0 = prev; prev = r;
        const float nl = len(n_acc);
        if (!(nl > 1e-9f) || !(mag_acc > 1e-9f) || nl < k.coherence * mag_acc) { circling = false; return 0.f; }
        const V3 n = n_acc * (1.f / nl);
        const V3 a0 = r0 - n * dot(r0, n), a1 = r - n * dot(r, n);
        const float d = std::atan2(dot(cross(a0, a1), n), dot(a0, a1));
        if (!(std::fabs(d) < k.max_step)) { circling = false; have_prev = false; return 0.f; }
        if (std::fabs(d) < k.min_rate * static_cast<float>(dt)) { circling = false; return 0.f; }   // a hand at rest (its r shrinking with no turn)
        circling = true; turns += std::fabs(d) / k_two_pi;
        return d;
    }
};
/// Either hand: the hand circling leads; the lead changes only when it stops and the other circles (so two hands never add up).
struct CircleMix {
    int lead = -1; double angle = 0.0; uint32_t lead_changes = 0;
    void reset() { lead = -1; angle = 0.0; lead_changes = 0; }
    void step(const float d[2], const bool circling[2]) {
        if (lead < 0 || !circling[lead]) {
            const int o = lead < 0 ? (circling[1] ? 1 : circling[0] ? 0 : -1) : (circling[lead ^ 1] ? lead ^ 1 : lead);
            if (o != lead && o >= 0) { if (lead >= 0) lead_changes++; lead = o; }
        }
        if (lead >= 0 && circling[lead]) angle += d[lead];
    }
    /// The angle for the input thread (k_turn units a turn, wrapping).
    int32_t published() const {
        const double u = angle / static_cast<double>(k_two_pi) * static_cast<double>(k_turn);
        const long long q = std::llround(u);
        return static_cast<int32_t>(static_cast<uint32_t>(static_cast<unsigned long long>(q) & 0xFFFFFFFFull));
    }
};

// -- the stick (input thread) ----------------------------------------------------------------------------------------------
// The game counts quadrants, so the stick shows quadrant centres (full-length diagonals, far past the 25 % thresholds; never a
// dead band) and follows the hand's angle one quadrant at a time: a step only on a new packet (the game ignores an unchanged
// one), only after the quadrant was in at least k_hold_polls new packets and k_hold_ms (two 30 Hz logic reads see it), never
// two quadrants at once (no skip: a skip is a mismatch and loses the turn). Holding each quadrant for two reads keeps the
// game's chain: after a counted spin its history restarts at the next quadrant read, so a stick that already left the end
// quadrant loses a quarter turn per spin (the 40-degree sweep the design proposed loses that much at speed: the unit test's
// transcription showed it). A quarter-quadrant hysteresis keeps a hand resting on a boundary from flickering. A backlog of a
// whole turn or more is dropped in whole turns (the game could not count them in time; the angle is unchanged): after the
// hand stops the stick finishes at most one turn. Cap: 4 polls a turn and 240 ms, 3.75 turns a second at 30 polls a second.
constexpr int32_t k_quad = k_turn / 4, k_hyst = k_quad / 4;
constexpr int k_hold_polls = 2; constexpr uint32_t k_hold_ms = 60;
inline int32_t wrap_sub(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); }
inline int32_t wrap_add(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
/// The start of the quadrant holding angle a (units).
inline int32_t quad_floor(int32_t a) { return static_cast<int32_t>(static_cast<uint32_t>(a) & ~static_cast<uint32_t>(k_quad - 1)); }
struct Stick {
    bool have = false; int32_t q = 0; uint32_t pkt = 0, t_in = 0; int seen = 0;   // q: the quadrant shown (its start, units)
    long long sent = 0, dropped = 0;   // units stepped / dropped (whole turns), cumulative
    /// on: the render thread's channel is live and fresh. Returns true with (x, y) = the unit vector to merge; false: nothing.
    bool step(bool on, int32_t target, uint32_t pkt_now, uint32_t now_ms, float& x, float& y) {
        if (!on) { have = false; return false; }
        if (!have) { have = true; q = quad_floor(target); pkt = pkt_now; t_in = now_ms; seen = 1; }
        else if (pkt_now != pkt) {
            pkt = pkt_now;
            int32_t d = wrap_sub(target, q);   // the target's angle from the shown quadrant's start
            if (d >= k_turn || d <= -k_turn) {
                const int32_t whole = d / k_turn * k_turn;
                q = wrap_add(q, whole); d -= whole; dropped += whole < 0 ? -static_cast<long long>(whole) : whole;
            }
            const int dir = d >= k_quad + k_hyst ? 1 : d < -k_hyst ? -1 : 0;
            if (dir != 0 && seen >= k_hold_polls && now_ms - t_in >= k_hold_ms) { q = wrap_add(q, dir * k_quad); t_in = now_ms; seen = 1; sent += k_quad; }
            else if (seen < 1000) seen++;
        }
        const float a = static_cast<float>(static_cast<double>(static_cast<uint16_t>(static_cast<uint32_t>(wrap_add(q, k_quad / 2)) & 0xFFFFu)) * (k_two_pi / k_turn));
        x = std::cos(a); y = std::sin(a);
        return true;
    }
};

// -- Auron and Tidus: strokes ------------------------------------------------------------------------------------------
// Per hand. Speed is the two-frame difference; a stroke starts above `start` (its start point is the sample two frames back)
// and ends below `end` or after max_s; one that starts inside the refractory after the last is ignored whole. Its path
// (start -> end) on the head's yaw axes (right, world up, forward; the head's pitch is ignored, so looking down at the hands
// does not tilt "up") classifies it at the end: a thrust when forward >=
// min_len and >= thrust_dom x the larger of right / up; a slash when the larger of right / up >= min_len, >= dom x the other
// and >= fwd_dom x |forward|; else nothing (diagonal, short, mixed, a pull back). A refractory follows every stroke.
// The swing (Tidus) fires at the onset: once the path of a stroke reaches swing_len in any direction (the bar's timing).
enum Cls { C_NONE = 0, C_LEFT, C_RIGHT, C_UP, C_DOWN, C_THRUST, C_SWING, C_N };
static const char* const k_cls[C_N] = { "none", "left", "right", "up", "down", "thrust", "swing" };
enum Why { R_NONE = 0, R_SENT, R_DIAGONAL, R_SHORT, R_MIXED, R_BACK, R_LOST, R_N };
static const char* const k_why[R_N] = { "none", "sent", "diagonal", "short", "mixed (forward and sideways)", "pulled back", "tracking lost" };
struct StrokeCfg {
    float start = 1.6f, end = 0.6f, max_s = 0.5f, min_len = 0.15f, dom = 1.8f, thrust_dom = 1.5f, fwd_dom = 1.2f, refractory = 0.2f, swing_len = 0.10f;
};
struct Axes { V3 r, u, f; };
/// The head's yaw axes from its forward vector (any length; y up): false when the head looks straight up or down.
inline bool yaw_axes(V3 fwd, Axes& a) {
    const float h = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    if (!(h > 1e-3f)) return false;
    a.f = v3(fwd.x / h, 0.f, fwd.z / h); a.u = v3(0.f, 1.f, 0.f); a.r = v3(-a.f.z, 0.f, a.f.x);
    return true;
}
/// The class of a finished stroke's path d (m) and why when none.
inline int classify(V3 d, const Axes& a, const StrokeCfg& c, int* why) {
    const float x = dot(d, a.r), y = dot(d, a.u), z = dot(d, a.f);
    const float ax = std::fabs(x), ay = std::fabs(y), az = std::fabs(z);
    const float planar = ax > ay ? ax : ay, other = ax > ay ? ay : ax;
    if (z >= c.min_len && z >= c.thrust_dom * planar) { if (why) *why = R_SENT; return C_THRUST; }
    if (planar >= c.min_len && planar >= c.dom * other && planar >= c.fwd_dom * az) {
        if (why) *why = R_SENT;
        return ax >= ay ? (x > 0.f ? C_RIGHT : C_LEFT) : (y > 0.f ? C_UP : C_DOWN);
    }
    if (why) {
        if (planar < c.min_len && az < c.min_len) *why = R_SHORT;
        else if (-z >= c.min_len && -z >= c.thrust_dom * planar) *why = R_BACK;
        else if (planar >= c.min_len && planar < c.dom * other && planar >= c.fwd_dom * az) *why = R_DIAGONAL;
        else *why = R_MIXED;
    }
    return C_NONE;
}
enum { ST_DIR = 0, ST_SWING = 1 };       // Stroke::step modes: Auron's classes at the end / Tidus's swing at the onset
enum { EV_FIRE = 1, EV_END = 2 };        // Stroke::step results
struct Stroke {
    int nh = 0; V3 h1, h2; double t1 = 0.0, t2 = 0.0;   // the last two samples
    bool in = false, fired = false, blocked = false; V3 p0; double t0 = 0.0, quiet_until = -1e300; float peak = 0.f; Axes ax0;
    int last_cls = C_NONE, last_why = R_NONE; float last_len = 0.f, last_peak = 0.f;
    void reset() { nh = 0; in = fired = blocked = false; quiet_until = -1e300; peak = 0.f; }
    /// One sample. Returns EV_* bits; cls = the class when EV_FIRE.
    int step(V3 p, bool valid, double t, const Axes& ax, const StrokeCfg& c, int mode, int& cls) {
        cls = C_NONE; int ev = 0;
        if (!valid) {
            if (in) { if (!fired) { last_cls = C_NONE; last_why = R_LOST; last_len = len(h1 - p0); last_peak = peak; } ev |= EV_END; }
            in = false; nh = 0; return ev;
        }
        if (nh >= 1 && (t - t1 <= 0.0 || t - t1 > 0.25)) {   // a gap: the stroke and the history start again
            if (in) { if (!fired) { last_cls = C_NONE; last_why = R_LOST; last_len = len(h1 - p0); last_peak = peak; } ev |= EV_END; }
            in = false; nh = 0;
        }
        float speed = 0.f;
        if (nh >= 2 && t > t2) speed = len(p - h2) / static_cast<float>(t - t2);
        // a stroke that starts in the refractory is refused whole: blocked until its speed falls under `end`
        if (!in && nh >= 2 && speed > c.start && t < quiet_until) blocked = true;
        if (blocked && speed < c.end) blocked = false;
        if (!in && !blocked && nh >= 2 && speed > c.start) { in = true; fired = false; p0 = h2; t0 = t2; peak = speed; ax0 = ax; }
        if (in) {
            if (speed > peak) peak = speed;
            const V3 d = p - p0;
            if (mode == ST_SWING && !fired && len(d) >= c.swing_len) {
                fired = true; cls = C_SWING; ev |= EV_FIRE; last_cls = C_SWING; last_why = R_SENT; last_len = len(d); last_peak = peak;
            }
            if (speed < c.end || t - t0 > c.max_s) {
                if (mode == ST_DIR) {
                    int why = R_NONE; const int k = classify(d, ax0, c, &why);
                    if (k != C_NONE) { cls = k; ev |= EV_FIRE; }
                    last_cls = k; last_why = why; last_len = len(d); last_peak = peak;
                }
                else if (!fired) { last_cls = C_NONE; last_why = R_SHORT; last_len = len(d); last_peak = peak; }
                in = false; quiet_until = t + c.refractory; ev |= EV_END;
            }
        }
        h2 = h1; t2 = t1; h1 = p; t1 = t; if (nh < 2) nh++;
        return ev;
    }
};

// -- the buttons ---------------------------------------------------------------------------------------------------------
enum : uint16_t { B_DPAD_UP = 0x0001, B_DPAD_DOWN = 0x0002, B_DPAD_LEFT = 0x0004, B_DPAD_RIGHT = 0x0008, B_LSHOULDER = 0x0100, B_RSHOULDER = 0x0200, B_A = 0x1000 };
/// The XInput button for a recognised gesture (0: none): Auron's slashes the d-pad, a punch the shoulder of its hand (L1 / R1);
/// Tidus's swing A (GUESS: the PC port's confirm; the game tests its confirm bit 0x20 of 0x888E80).
inline uint16_t button_for(int od, int cls, int hand) {
    if (od == OD_AURON) {
        switch (cls) {
            case C_LEFT: return B_DPAD_LEFT; case C_RIGHT: return B_DPAD_RIGHT; case C_UP: return B_DPAD_UP; case C_DOWN: return B_DPAD_DOWN;
            case C_THRUST: return hand == 0 ? B_LSHOULDER : B_RSHOULDER;
            default: return 0;
        }
    }
    if (od == OD_TIDUS) return cls == C_SWING ? B_A : 0;
    return 0;
}
inline const char* button_name(uint16_t b) {
    switch (b) {
        case B_DPAD_UP: return "DPAD_UP"; case B_DPAD_DOWN: return "DPAD_DOWN"; case B_DPAD_LEFT: return "DPAD_LEFT"; case B_DPAD_RIGHT: return "DPAD_RIGHT";
        case B_LSHOULDER: return "LB"; case B_RSHOULDER: return "RB"; case B_A: return "A"; case 0: return "none"; default: return "?";
    }
}
/// The presses waiting for the press slot (render thread): at most 3, oldest first.
struct PressQ {
    uint16_t b[3] = { 0, 0, 0 }; uint8_t h[3] = { 0, 0, 0 }; int n = 0; uint32_t full = 0;
    bool push(uint16_t btn, int hand) { if (!btn) return false; if (n >= 3) { full++; return false; } b[n] = btn; h[n] = static_cast<uint8_t>(hand & 1); n++; return true; }
    uint16_t pop(int* hand) {
        if (n <= 0) return 0;
        const uint16_t r = b[0]; if (hand) *hand = h[0];
        for (int i = 1; i < n; ++i) { b[i - 1] = b[i]; h[i - 1] = h[i]; }
        n--; return r;
    }
    void clear() { n = 0; }
};
/// The press slot's rule (pad::press_hook): held for k_press_polls new packets and at least k_press_ms, then one clear packet.
/// 3 packets / 80 ms is the tap's proven length (a new-press edge in every poll pattern, ffxvr_hands.cpp hv_tap_hook) and
/// under the game's repeat (0.2333 s); about 4 polls a press, 6 a second at 24 polls a second.
constexpr int32_t k_press_ms = 80, k_press_polls = 3;

// -- the odg: line ---------------------------------------------------------------------------------------------------------
struct Sess {
    int od = OD_NONE; bool inject = false; double t0 = 0.0, t1 = 0.0;
    float turns[2] = { 0.f, 0.f }; double out_turns = 0.0, sent_turns = 0.0, dropped_turns = 0.0; uint32_t lead_changes = 0;   // Lulu
    int sub0 = -1, sub1 = -1, cnt0 = -1, cnt1 = -1;                                                                         // Lulu's spins / counter at the open and the close
    uint32_t cls_n[C_N] = {}, why_n[R_N] = {}; uint32_t sent = 0, queue_full = 0;                                         // strokes, presses sent
    int len = 0, prog_max = 0; uint32_t resets = 0, game_presses = 0;                                                        // Auron
    int end_state = 0, end_timer = 0;
};
inline int fmt_append(char* s, size_t n, int k, const char* fmt, ...) {
    if (k < 0 || static_cast<size_t>(k) >= n) return k;
    va_list ap; va_start(ap, fmt); const int j = vsnprintf(s + k, n - static_cast<size_t>(k), fmt, ap); va_end(ap);
    return j < 0 ? k : k + j;
}
/// The overdrive's end state in words.
inline const char* end_text(int od, int st) {
    if (od == OD_TIDUS) return od::tidus_state(st);
    if (od == OD_AURON) return st == 5 ? "success" : st == 7 ? "timed out" : st == 4 ? "still at input" : "?";
    if (od == OD_LULU) return st == 6 ? "timed out (the spins go to the spell)" : st == 4 ? "still spinning" : "?";
    return "?";
}
/// The line after "odg: ": one per window, written when it closes.
inline int summary(char* s, size_t n, const Sess& x) {
    if (n == 0) return 0;
    s[0] = 0; int k = 0;
    k = fmt_append(s, n, k, "%s: window %.1f s, gestures %s", k_od_name[x.od < 0 || x.od >= OD_N ? 0 : x.od], x.t1 - x.t0, x.inject ? "on" : "off (logged only)");
    if (x.od == OD_LULU) {
        k = fmt_append(s, n, k, "; circles: left %.1f turns, right %.1f; the stick turned %.1f (asked %.1f, dropped %.1f by the rate cap), hand changes %u",
                       x.turns[0], x.turns[1], x.sent_turns, x.out_turns, x.dropped_turns, x.lead_changes);
        if (x.sub0 >= 0 && x.sub1 >= 0) k = fmt_append(s, n, k, "; the game counted %d spins, counter %d -> %d", (x.sub1 - x.sub0) & 0xFF, x.cnt0, x.cnt1);
    }
    else {
        k = fmt_append(s, n, k, "; strokes:");
        for (int c = C_LEFT; c < C_N; ++c) if (x.cls_n[c]) k = fmt_append(s, n, k, " %s %u", k_cls[c], x.cls_n[c]);
        uint32_t refused = 0; for (int w = R_DIAGONAL; w < R_N; ++w) refused += x.why_n[w];
        k = fmt_append(s, n, k, "%s; refused %u", (x.cls_n[C_LEFT] | x.cls_n[C_RIGHT] | x.cls_n[C_UP] | x.cls_n[C_DOWN] | x.cls_n[C_THRUST] | x.cls_n[C_SWING]) ? "" : " none", refused);
        if (refused) { k = fmt_append(s, n, k, " ("); bool first = true; for (int w = R_DIAGONAL; w < R_N; ++w) if (x.why_n[w]) { k = fmt_append(s, n, k, "%s%s %u", first ? "" : ", ", k_why[w], x.why_n[w]); first = false; } k = fmt_append(s, n, k, ")"); }
        k = fmt_append(s, n, k, "; presses sent %u", x.sent);
        if (x.queue_full) k = fmt_append(s, n, k, " (%u dropped: queue full)", x.queue_full);
        if (x.od == OD_AURON) k = fmt_append(s, n, k, "; progress best %d of %d, wrong-press resets %u, game presses seen %u", x.prog_max, x.len, x.resets, x.game_presses);
    }
    k = fmt_append(s, n, k, "; closed at state %d (%s), timer %d", x.end_state, end_text(x.od, x.end_state), x.end_timer);
    return k;
}
/// Auron's expected sequence as raw values ("0x0040 0x0080 ...").
inline int seq_text(char* s, size_t n, const uint16_t* seq, int len) {
    if (n == 0) return 0;
    s[0] = 0; int k = 0;
    for (int i = 0; i < len && i < k_seq_max; ++i) k = fmt_append(s, n, k, "%s0x%04X", i ? " " : "", static_cast<unsigned>(seq[i]));
    return k;
}

// -- the driver: one render-thread tick a game frame ------------------------------------------------------------------------
struct FrameIn {
    od::Raw r; Extra x; bool battle = false, fresh = false;   // the od block (r.ok in battle only) and its freshness
    bool headset = false, vrpad = false;                      // the headset shows the game; the VR pad is merged
    V3 p[2]; bool valid[2] = { false, false };                // the grip positions (m, the reference space; y up)
    V3 head_fwd = v3(0.f, 0.f, -1.f);                         // the head's forward vector (same space)
    double t = 0.0;                                           // seconds (the poses' time)
    bool sample_new = true;                                   // the poses changed since the last tick (false: the classifiers wait)
    bool press_free = true;                                   // the press slot is empty (the input thread let the last one go)
    uint32_t stick_sent = 0, stick_dropped = 0;               // the input thread's Stick counters (units, cumulative, wrapping)
    bool game_press_ok = false; uint16_t game_press = 0;      // Auron's window: the game's new-press word u16 0x25D09D4 this frame
};
struct Haptic { int hand = 0; float amp = 0.f, sec = 0.f; };
struct FrameOut {
    int win = OD_NONE, inject = OD_NONE;                // the window open; the one whose gestures inject (OD_NONE: release everything)
    bool stick_on = false; int32_t target = 0;          // the stick channel: only while a hand circles (the physical stick works otherwise)
    uint16_t press = 0; int press_hand = 0;             // a press for the slot now (0: none)
    bool opened = false, closed = false;                // a window opened / closed this tick (closed: Driver::last holds its Sess)
    int nh = 0; Haptic hap[6];                          // pulses to send
    int fired[2] = { C_NONE, C_NONE };                  // a gesture recognised this tick, per hand (logged by the caller)
    uint16_t fired_btn[2] = { 0, 0 }; bool queued[2] = { false, false };
    bool game_press_new = false;                        // Auron: a new non-zero game press this tick (logged)
    void pulse(int hand, float amp, float sec) { if (nh < 6) { hap[nh].hand = hand; hap[nh].amp = amp; hap[nh].sec = sec; nh++; } }
};
struct Driver {
    Cfg cfg; CircleCfg cc; StrokeCfg sc;
    Circle circ[2]; CircleMix mix; Stroke st[2]; PressQ q; Sess sess, last; bool open = false;
    int last_hand = 1; uint16_t last_btn = 0; double last_btn_t = -1e300;   // the last press sent (Auron's capture line)
    int16_t prog = 0; uint8_t sub = 0, cnt = 0; uint16_t gp = 0; double second_at = -1.0, circ_t = -1e300;
    static constexpr double k_stick_linger = 0.25;   // s: the stick channel stays on this long after the circling stops
    uint32_t stick_sent0 = 0, stick_dropped0 = 0; uint32_t windows = 0;
    void apply_cfg() { cc.r_min = clampf(cfg.circle_r, 0.01f, 0.20f); sc.start = clampf(cfg.slash_speed, 0.8f, 5.f); sc.end = sc.start * 0.375f; }
    static int word(const od::Raw& r, int od) { return od == OD_LULU ? r.lulu : od == OD_AURON ? r.auron : od == OD_TIDUS ? r.tidus : 0; }
    void close_now(const FrameIn& in) {
        sess.t1 = in.t; sess.end_state = in.r.ok ? word(in.r, sess.od) : -1; sess.end_timer = in.r.timer_ok ? in.r.timer : 0;
        sess.turns[0] = circ[0].turns; sess.turns[1] = circ[1].turns; sess.out_turns = std::fabs(mix.angle) / k_two_pi; sess.lead_changes = mix.lead_changes;
        sess.sent_turns = static_cast<double>(in.stick_sent - stick_sent0) / k_turn; sess.dropped_turns = static_cast<double>(in.stick_dropped - stick_dropped0) / k_turn;
        sess.queue_full = q.full;
        last = sess; open = false; q.clear();
    }
    void tick(const FrameIn& in, FrameOut& o) {
        o = FrameOut{};
        apply_cfg();
        const int w = window(in.r, in.battle, in.fresh);
        if (open && w != sess.od) { close_now(in); o.closed = true; }
        if (w != OD_NONE && !open) {
            open = true; windows++; sess = Sess{}; sess.od = w; sess.t0 = in.t;
            for (int h = 0; h < 2; ++h) { circ[h] = Circle{}; st[h].reset(); }
            mix.reset(); q.clear(); q.full = 0; second_at = -1.0; last_btn = 0; circ_t = -1e300;
            prog = in.x.auron_prog; sub = in.x.lulu_sub; cnt = in.x.lulu_cnt; gp = in.game_press_ok ? in.game_press : 0;
            stick_sent0 = in.stick_sent; stick_dropped0 = in.stick_dropped;
            if (w == OD_LULU) { sess.sub0 = sess.sub1 = sub; sess.cnt0 = sess.cnt1 = cnt; }
            if (w == OD_AURON) sess.len = in.x.auron_len;
            o.opened = true;
        }
        const int inj = inject_gate(w, cfg, in.headset, in.vrpad);
        o.win = w; o.inject = inj;
        if (!open) return;
        if (inj != OD_NONE) sess.inject = true;
        const bool hap = cfg.haptics && inj != OD_NONE;
        if (o.opened && hap) { o.pulse(0, 0.5f, 0.04f); o.pulse(1, 0.5f, 0.04f); second_at = in.t + 0.12; }
        if (second_at > 0.0 && in.t >= second_at) { if (hap) { o.pulse(0, 0.5f, 0.04f); o.pulse(1, 0.5f, 0.04f); } second_at = -1.0; }
        if (w == OD_LULU) {
            if (in.sample_new) {
                float d[2]; bool c[2];
                for (int h = 0; h < 2; ++h) { d[h] = circ[h].step(in.p[h], in.valid[h], in.t, cc); c[h] = circ[h].circling; }
                mix.step(d, c);
                if (c[0] || c[1]) circ_t = in.t;
            }
            if (inj == OD_LULU && in.t - circ_t < k_stick_linger) { o.stick_on = true; o.target = mix.published(); }
            const int lh = mix.lead < 0 ? 1 : mix.lead;
            if (in.x.lulu_sub != sub) { sub = in.x.lulu_sub; if (hap) o.pulse(lh, 0.3f, 0.015f); }
            if (in.x.lulu_cnt != cnt) { cnt = in.x.lulu_cnt; if (hap) o.pulse(lh, 0.6f, 0.04f); }
            sess.sub1 = sub; sess.cnt1 = cnt;
        }
        else {
            Axes ax; const bool ax_ok = yaw_axes(in.head_fwd, ax);
            for (int h = 0; h < 2; ++h) {
                if (w == OD_TIDUS && h == 0) continue;   // Tidus: the right hand's swing
                int cls = C_NONE;
                if (!in.sample_new) continue;
                const int ev = st[h].step(in.p[h], in.valid[h] && ax_ok, in.t, ax, sc, w == OD_TIDUS ? ST_SWING : ST_DIR, cls);
                if (ev & EV_FIRE) {
                    sess.cls_n[cls]++; o.fired[h] = cls;
                    const uint16_t b = button_for(w, cls, h); o.fired_btn[h] = b;
                    if (inj == w && q.push(b, h)) { o.queued[h] = true; if (hap) o.pulse(h, 0.5f, 0.025f); }
                }
                else if ((ev & EV_END) && st[h].last_why > R_SENT && st[h].last_why < R_N) sess.why_n[st[h].last_why]++;
            }
            if (w == OD_AURON) {
                sess.len = in.x.auron_len;
                const int16_t pr = in.x.auron_prog;
                if (pr > prog && hap) o.pulse(last_hand, 0.6f, 0.03f);
                if (pr == 0 && prog > 0) { sess.resets++; if (hap) { o.pulse(0, 0.9f, 0.12f); o.pulse(1, 0.9f, 0.12f); } }
                prog = pr; if (pr > sess.prog_max) sess.prog_max = pr;
                const uint16_t g = in.game_press_ok ? in.game_press : 0;
                if (g != 0 && g != gp) { sess.game_presses++; o.game_press_new = true; }
                gp = g;
            }
        }
        if (inj == OD_NONE) q.clear();
        else if (in.press_free && q.n > 0) {
            o.press = q.pop(&o.press_hand); sess.sent++; last_hand = o.press_hand; last_btn = o.press; last_btn_t = in.t;
        }
    }
};

}  // namespace odg
