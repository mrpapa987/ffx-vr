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
//                         that angle, each held for 1 new packet and 40 ms (the spin's end 2 and 60 ms): no skip with a
//                         regular 30 Hz logic clock; a logic hitch over 40 ms can lose a turn; backlog under a turn
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
// another non-zero press -> progress 0. Tidus's state 4 (0x892386): a confirm press inside the zone -> 5; outside (0x8923F8..
// 0x89244F) -> sound 3 and THE BAR RESTARTS: the cursor goes back to the bar's start (s16 0x133D736) moving forward
// (velocity s16 0x133D744 = |v|) while the timer keeps running, so every miss costs a whole traversal (review B1). The bar's
// own bounces at its ends are reflections (0x892456..0x892499): the cursor moves continuously except at a restart.
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
/// od_gest_auron 0 / 1 / 2: auron off; on with auron_mode 1 (slashes and punches, this file); on with auron_mode 2 (strike the
/// targets, ffxvr_odtarget.h: the driver then runs no stroke classifier in Auron's window and queues nothing; the targets' Runner
/// sends the presses through the same slot and gate).
struct Cfg {
    bool on = true, lulu = true, auron = false, tidus = false, haptics = true;   // Tidus off until the owner's test: a false A restarts the bar
    int auron_mode = 1;           // 1 slashes and punches, 2 strike the targets (only while auron is on)
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
    int16_t tidus_start = 0, tidus_vel = 0, tidus_len = 0;                  // 0x133D736 the bar's start, 0x133D744 the cursor's velocity, 0x133D73A the bar's length
};
constexpr int k_x_prog = 0x0A, k_x_len = 0x0E, k_x_ptr = 0x12, k_x_sub = 0x2C, k_x_cnt = 0x2D, k_x_hist_n = 0x3F, k_x_tstart = 0x44, k_x_tvel = 0x52, k_x_tlen = 0x48;
static_assert(k_x_tvel + 2 <= static_cast<int>(od::k_blk_len), "inside the od block");
inline Extra parse_extra(const uint8_t* b) {
    Extra e;
    e.auron_prog = od::s16_at(b, k_x_prog); e.auron_len = od::s32_at(b, k_x_len); e.auron_ptr = static_cast<uint32_t>(od::s32_at(b, k_x_ptr));
    e.lulu_sub = b[k_x_sub]; e.lulu_cnt = b[k_x_cnt]; e.lulu_hist_n = b[k_x_hist_n];
    e.tidus_start = od::s16_at(b, k_x_tstart); e.tidus_vel = od::s16_at(b, k_x_tvel); e.tidus_len = od::s16_at(b, k_x_tlen);
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
    float r_len = 0.f;   // the last radius (m): the visuals' ring
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
        r_len = len(r);
        if (r_len < k.r_min) { have_prev = circling = false; return 0.f; }
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
// one), never two quadrants at once (a skip is a mismatch and loses the turn). After a counted spin the game's history
// restarts at the next quadrant it reads, so the quadrant that completes a spin must be read twice; the others once.
// Round 3 (the owner's play2 Fury: 18.8 hand turns, 13.2 stick turns with 5.0 dropped by this cap, 13 spins counted: the
// chain held, the cap was the loss): a quadrant is held for 1 new packet and k_hold_ms (40 ms, a 30 Hz logic frame with
// margin, on a precise clock), and every 4th step since the stick came on (the spin's end, counted from the quadrant it came
// on at, which the game's chain starts at) for 2 packets and 60 ms. With a regular 30 Hz logic clock, 40 ms between any two
// changes keeps one logic read between them even with jittered polls (30 ms let two changes fall between two reads: a skip,
// in the unit test's jittered 30 Hz polls): no skip. A logic hitch over 40 ms (a frame that reads late) can let two changes
// fall between two reads and lose that turn (the od-visual review's probe6: 3-16 lost turns in 80 s of free-running polls
// with a hitching clock, against 10-30 % more spins than round 2's 60 ms; 45 / 50 ms holds lose about as many and cost spins). Polls a spin: 5 at 24 polls a second (4.8 spins a second; it was 8 polls, 3 a second), 8 at 30 (as before),
// 13 at 60 (4.6 a second; it was 3.75); if the game's chain started elsewhere (a pause mid-window) a spin takes one quadrant more, 6 polls: still
// no mismatch, never slower than before. A quarter-quadrant hysteresis keeps a hand resting on a boundary from flickering. A
// backlog of a whole turn or more is dropped in whole turns (the game could not count them in time; the angle is unchanged).
constexpr int32_t k_quad = k_turn / 4, k_hyst = k_quad / 4;
constexpr int k_hold_polls = 1, k_hold_end_polls = 2; constexpr uint32_t k_hold_ms = 40, k_hold_end_ms = 60;
inline int32_t wrap_sub(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b)); }
inline int32_t wrap_add(int32_t a, int32_t b) { return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b)); }
/// The start of the quadrant holding angle a (units).
inline int32_t quad_floor(int32_t a) { return static_cast<int32_t>(static_cast<uint32_t>(a) & ~static_cast<uint32_t>(k_quad - 1)); }
struct Stick {
    bool have = false; int32_t q = 0; uint32_t pkt = 0, t_in = 0; int seen = 0;   // q: the quadrant shown (its start, units)
    uint32_t steps = 0;                       // quadrant steps since the stick came on (every 4th lands on the spin's end)
    long long sent = 0, dropped = 0;   // units stepped / dropped (whole turns), cumulative
    /// on: the render thread's channel is live and fresh. Returns true with (x, y) = the unit vector to merge; false: nothing.
    bool step(bool on, int32_t target, uint32_t pkt_now, uint32_t now_ms, float& x, float& y) {
        if (!on) { have = false; return false; }
        if (!have) { have = true; q = quad_floor(target); pkt = pkt_now; t_in = now_ms; seen = 1; steps = 0; }
        else if (pkt_now != pkt) {
            pkt = pkt_now;
            int32_t d = wrap_sub(target, q);   // the target's angle from the shown quadrant's start
            if (d >= k_turn || d <= -k_turn) {
                const int32_t whole = d / k_turn * k_turn;
                q = wrap_add(q, whole); d -= whole; dropped += whole < 0 ? -static_cast<long long>(whole) : whole;
            }
            const int dir = d >= k_quad + k_hyst ? 1 : d < -k_hyst ? -1 : 0;
            const bool end = steps > 0 && steps % 4 == 0;   // the spin's end quadrant: read twice (2 packets and 60 ms)
            if (dir != 0 && seen >= (end ? k_hold_end_polls : k_hold_polls) && now_ms - t_in >= (end ? k_hold_end_ms : k_hold_ms)) { q = wrap_add(q, dir * k_quad); t_in = now_ms; seen = 1; sent += k_quad; steps++; }
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
// and >= fwd_dom x |forward|; else nothing (diagonal, short, mixed, a pull back). A refractory (0.12 s) follows every stroke
// (0.2 s until round 2: the bring-back rule below now does the longer job, and a learned opposite pair may follow at 0.15 s).
// The swing (Tidus) fires at the onset, once a stroke's path reaches swing_len (20 cm) with a peak of swing_peak (1.25 x the
// start speed, od_slash_speed: 2.0 m/s at 1.6; a 30 cm swing in 250 ms peaks at 2.25 m/s and samples under 2.2 at 30 Hz) and is not mainly upward (an upward share under swing_up of the path: reaching for the headset).
// Jumps (review S1, round 2 S1): the caller passes an untracked or merely inferred pose as not valid, which ends the stroke
// with nothing (the Quest snaps a controller's pose when it re-acquires it). On tracked poses: a one-frame step over
// max_speed (20 m/s; the fastest real stroke in the reviewer's table peaks at 16.4 m/s: 70 cm in 80 ms) ends the stroke; and
// a snap is a step of at least snap_min (4 cm) with rest on both sides: the step before and the step after are each at most
// snap_rest (8 %) of it (a snap stops, a stroke goes on). Deciding that needs the next sample, so while a step could still
// be a snap the stroke's decision (the swing's onset, the slash's end) waits one sample. A real stroke at 30 Hz has its
// neighbours above 10 % down to 60 ms strokes (the centred frame of a 60 ms minimum-jerk stroke holds 83 %, each neighbour
// 8.5 % of the path: 10 % of the big step); only strokes under about 55 ms at 30 Hz look like a snap.
// The bring-back (review S2, round 2 S2): after a stroke that fired, the same hand's next stroke starting within return_s
// (0.3 s) of its end is refused when it points back (cosine under -0.5 with the fired path), unless the caller allows that
// class now (Auron: the game's expected next input is that stroke's button by the map in use (Driver::map): Shooting
// Star's Left, Right and Banishing Blade's Right, Left, from the first Bushido); for the swing any stroke within swing_return_s (0.5 s) is refused (one A per swing).
enum Cls { C_NONE = 0, C_LEFT, C_RIGHT, C_UP, C_DOWN, C_THRUST, C_SWING, C_N };
static const char* const k_cls[C_N] = { "none", "left", "right", "up", "down", "thrust", "swing" };
enum Why { R_NONE = 0, R_SENT, R_DIAGONAL, R_SHORT, R_MIXED, R_BACK, R_LOST, R_JUMP, R_RETURN, R_UP, R_N };
static const char* const k_why[R_N] = { "none", "sent", "diagonal", "short", "mixed (forward and sideways)", "pulled back", "tracking lost",
                                        "a tracking jump", "the bring-back", "upward (a reach)" };
struct StrokeCfg {
    float start = 1.6f, end = 0.6f, max_s = 0.5f, min_len = 0.15f, dom = 1.8f, thrust_dom = 1.5f, fwd_dom = 1.2f, refractory = 0.12f;
    float swing_len = 0.20f, swing_peak = 2.0f, swing_up = 0.8f, max_speed = 20.f, return_s = 0.3f, swing_return_s = 0.5f;
    float snap_min = 0.04f, snap_rest = 0.08f;
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
    bool in = false, fired = false, decided = false, blocked = false; V3 p0; double t0 = 0.0, quiet_until = -1e300; float peak = 0.f; Axes ax0;
    bool cand = false; float cand_step = 0.f;          // the last step may be a snap (rest before it): the next sample decides
    double ret_until = -1e300; V3 ret_dir;              // the bring-back rule: after a fired stroke, its path (unit) and until when
    int last_cls = C_NONE, last_why = R_NONE; float last_len = 0.f, last_peak = 0.f; bool last_rev = false;   // last_rev: sent as an allowed bring-back
    void reset() { nh = 0; in = fired = decided = blocked = cand = false; quiet_until = -1e300; ret_until = -1e300; peak = 0.f; }
    /// The stroke in progress ends with nothing (why); the history starts again.
    int drop(int why) {
        int ev = 0;
        if (in) { if (!fired) { last_cls = C_NONE; last_why = why; last_len = len(h1 - p0); last_peak = peak; } ev |= EV_END; }
        in = false; nh = 0; cand = false; return ev;
    }
    /// True when a stroke starting at t0 with path d is the bring-back of the last fired one (swing: any stroke then).
    bool is_return(V3 d, int mode) const {
        if (t0 >= ret_until) return false;
        if (mode == ST_SWING) return true;
        const float l = len(d);
        return l > 1e-6f && dot(d, ret_dir) < -0.5f * l;
    }
    /// One sample. Returns EV_* bits; cls = the class when EV_FIRE. allow_rev: bit c set = a class-c stroke is allowed even as
    /// the bring-back (the game expects that input next).
    int step(V3 p, bool valid, double t, const Axes& ax, const StrokeCfg& c, int mode, int& cls, int allow_rev = 0) {
        cls = C_NONE; int ev = 0;
        if (!valid) return drop(R_LOST);
        if (nh >= 1 && (t - t1 <= 0.0 || t - t1 > 0.25)) ev |= drop(R_LOST);                              // a gap
        else if (nh >= 1 && len(p - h1) > c.max_speed * static_cast<float>(t - t1)) ev |= drop(R_JUMP);    // a tracking jump
        else if (nh >= 1) {   // the snap test: a big step with rest before it is a candidate; rest after it makes it a snap
            const float sl = len(p - h1);
            if (cand && sl <= c.snap_rest * cand_step) ev |= drop(R_JUMP);
            else { cand = sl >= c.snap_min && (nh < 2 || len(h1 - h2) <= c.snap_rest * sl); cand_step = sl; }
        }
        float speed = 0.f;
        if (nh >= 2 && t > t2) speed = len(p - h2) / static_cast<float>(t - t2);
        // a stroke that starts in the refractory is refused whole: blocked until its speed falls under `end`
        if (!in && nh >= 2 && speed > c.start && t < quiet_until) blocked = true;
        if (blocked && speed < c.end) blocked = false;
        if (!in && !blocked && nh >= 2 && speed > c.start) { in = true; fired = decided = false; p0 = h2; t0 = t2; peak = speed; ax0 = ax; }
        if (in) {
            if (speed > peak) peak = speed;
            const V3 d = p - p0;
            const float l = len(d);
            if (mode == ST_SWING && !decided && !cand && l >= c.swing_len && peak >= c.swing_peak) {
                decided = true;
                const int why = is_return(d, mode) ? R_RETURN : dot(d, ax0.u) >= c.swing_up * l ? R_UP : R_SENT;
                if (why == R_SENT) { fired = true; cls = C_SWING; ev |= EV_FIRE; last_cls = C_SWING; }
                else last_cls = C_NONE;
                last_why = why; last_len = l; last_peak = peak;
            }
            if (!cand && (speed < c.end || t - t0 > c.max_s)) {   // (a candidate snap: the end waits one sample)
                if (mode == ST_DIR) {
                    int why = R_NONE; int k = classify(d, ax0, c, &why); last_rev = false;
                    if (k != C_NONE && is_return(d, mode)) { if (allow_rev & (1 << k)) last_rev = true; else { k = C_NONE; why = R_RETURN; } }
                    if (k != C_NONE) { cls = k; ev |= EV_FIRE; fired = true; }
                    last_cls = k; last_why = why; last_len = l; last_peak = peak;
                }
                else if (!decided) { last_cls = C_NONE; last_why = R_SHORT; last_len = l; last_peak = peak; }
                if (fired && l > 1e-6f) { ret_until = t + (mode == ST_SWING ? c.swing_return_s : c.return_s); ret_dir = d * (1.f / l); }
                in = false; quiet_until = t + c.refractory; ev |= EV_END;
            }
        }
        h2 = h1; t2 = t1; h1 = p; t1 = t; if (nh < 2) nh++;
        return ev;
    }
};

// -- the buttons ---------------------------------------------------------------------------------------------------------
enum : uint16_t { B_DPAD_UP = 0x0001, B_DPAD_DOWN = 0x0002, B_DPAD_LEFT = 0x0004, B_DPAD_RIGHT = 0x0008, B_LSHOULDER = 0x0100, B_RSHOULDER = 0x0200, B_A = 0x1000,
                  B_B = 0x2000, B_X = 0x4000, B_Y = 0x8000 };
/// Auron's sequence values (u16 [[0x133D704]]) -> XInput buttons. The values are the game's raw pad word (the word the
/// pad record keeps at +4, 0x888D70; Bushido's new-press word 0x25D09D4 is built from it with no remap): the owner's play2
/// capture (7 Nov: Dragon Fang u16[8] 0x4000 0x8000 0x1000 0x2000 0x0004 0x0008 0x0040 0x0020 and Shooting Star u16[7]
/// 0x0010 0x0040 0x0080 0x0020 0x8000 0x2000 0x0020, every press matched) fixes the d-pad and shoulder half (Up 0x1000, Right
/// 0x2000, Down 0x4000, Left 0x8000, L1 0x0004, R1 0x0008). The face half: 0x0020 is the confirm bit (Tidus's state 4 tests
/// it at 0x8923C8 and our tap's A confirms), 0x0040 cancel (B); jegged's lists against the capture put Triangle at 0x0010 and
/// Square at 0x0080 (Y and X in their Quest places). The od-visual review (S1) found A and B swapped in round 3's table.
/// AuronMap is what the Driver and the glyph row use: k_auron_map (that built-in fallback) until a window's open reads the
/// game's own bindings (auron_map_from_bindings below), which also follows a player's remap.
struct ValBtn { uint16_t v, b; };
struct AuronMap {
    ValBtn e[10];
    uint16_t value_of(uint16_t b) const { for (const ValBtn& m : e) if (b != 0 && m.b == b) return m.v; return 0; }
    uint16_t button_of(uint16_t v) const { for (const ValBtn& m : e) if (v != 0 && m.v == v) return m.b; return 0; }
};
static constexpr AuronMap k_auron_map = { { { 0x1000, B_DPAD_UP }, { 0x2000, B_DPAD_RIGHT }, { 0x4000, B_DPAD_DOWN }, { 0x8000, B_DPAD_LEFT },
                                            { 0x0004, B_LSHOULDER }, { 0x0008, B_RSHOULDER }, { 0x0010, B_Y }, { 0x0020, B_A }, { 0x0040, B_B }, { 0x0080, B_X } } };
inline uint16_t value_of_button(uint16_t b) { return k_auron_map.value_of(b); }   // the fallback's (tests, the logs' "built-in")
inline uint16_t button_of_value(uint16_t v) { return k_auron_map.button_of(v); }
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
        case B_LSHOULDER: return "LB"; case B_RSHOULDER: return "RB"; case B_A: return "A"; case B_B: return "B"; case B_X: return "X"; case B_Y: return "Y";
        case 0: return "none"; default: return "?";
    }
}
/// The game's gamepad bindings (CHECKED in re/dis_full.txt): the raw pad word (dword [0xCCB170], read by 0x630C80 and 0x8898A0
/// into the pad record) is built each frame by 0x632A30 from a heap table, its pointer at [0xCCB214] (freed at exit,
/// 0xB06718): pairs { u32 id, u32 bits } at +0x70..+0xDF (14 of them) for the pad (0x632DF2: 0x629CA0 tests the pad's button
/// byte id - 0x0B, then bits are ORed in), others for the keyboard. The pad's bytes are the poll's order (0x62DBA0 slot
/// +0x398..+0x3A7): X A B Y LB RB LT RT Back Start LS RS Up Right Down Left, so id 0x0B is X, 0x0C A, ... 0x1A Left. (The
/// per-pad table 0x1330488, which 0x888C40 applies, is not on this path: the pad init sets it to identity, and only some menu
/// code (0x85xxxx, 0x8677EA) maps through it; Bushido does not.)
constexpr uint32_t k_bind_ptr_va = 0xCCB214, k_bind_pad_off = 0x70; constexpr int k_bind_pad_n = 14;
inline int pad_byte_of(uint16_t b) {   // the poll's byte index of an XInput button (-1: not one of Auron's ten)
    switch (b) {
        case B_X: return 0; case B_A: return 1; case B_B: return 2; case B_Y: return 3; case B_LSHOULDER: return 4; case B_RSHOULDER: return 5;
        case B_DPAD_UP: return 12; case B_DPAD_RIGHT: return 13; case B_DPAD_DOWN: return 14; case B_DPAD_LEFT: return 15;
        default: return -1;
    }
}
/// pairs: the n { id, bits } pairs read at the table's pad part (nullptr: not readable). The pad's byte order above is
/// INFERRED (the copy from the poll's slot into the pad device's bytes was not traced), so the d-pad and shoulders, whose
/// values the capture proves, must read exactly those values: that is the check that the reading is right. The four face
/// buttons must get the four face bits (0x10 / 0x20 / 0x40 / 0x80) in any order, one each: a player's remap (confirm and
/// cancel swapped) is followed; a d-pad or shoulder remap is not (the built-in map, with the reason). True: *out = the game's
/// map. False: *out = k_auron_map and why says what failed. changed: how many buttons differ from the built-in map.
inline bool auron_map_from_bindings(const uint32_t (*pairs)[2], int n, AuronMap* out, char* why, size_t why_n, int* changed = nullptr) {
    *out = k_auron_map; if (changed) *changed = 0;
    if (why && why_n) why[0] = 0;
    if (pairs == nullptr || n <= 0) { if (why && why_n) snprintf(why, why_n, "the bindings are not readable"); return false; }
    AuronMap m = k_auron_map; uint16_t seen = 0; int diff = 0;
    for (ValBtn& e : m.e) {
        const int byte = pad_byte_of(e.b);
        uint32_t bits = 0;
        for (int i = 0; i < n; ++i) if (pairs[i][0] == static_cast<uint32_t>(0x0B + byte)) bits |= pairs[i][1];
        const char* bad = nullptr;
        if (bits == 0) bad = "no binding";
        else if (bits > 0xFFFFu || k_auron_map.button_of(static_cast<uint16_t>(bits)) == 0) bad = "not one of the ten game bits";
        else if (seen & bits) bad = "a game bit bound twice";
        else if (e.b != B_A && e.b != B_B && e.b != B_X && e.b != B_Y && bits != e.v) bad = "not the capture's value: a d-pad / shoulder remap is not followed";
        if (bad) { if (why && why_n) snprintf(why, why_n, "%s: 0x%X (%s)", button_name(e.b), static_cast<unsigned>(bits), bad); return false; }
        seen = static_cast<uint16_t>(seen | bits);
        if (e.v != bits) diff++;
        e.v = static_cast<uint16_t>(bits);
    }
    *out = m; if (changed) *changed = diff;
    return true;
}
/// The map as text: "Up 0x1000 Right 0x2000 ... X 0x0080".
inline void map_text(char* s, size_t n, const AuronMap& m) {
    if (!s || !n) return;
    s[0] = 0; size_t k = 0;
    for (const ValBtn& e : m.e) {
        const int w = snprintf(s + k, n - k, "%s%s 0x%04X", k ? " " : "", button_name(e.b), static_cast<unsigned>(e.v));
        if (w < 0 || static_cast<size_t>(w) >= n - k) { s[n - 1] = 0; return; }
        k += static_cast<size_t>(w);
    }
}
/// The presses waiting for the press slot (render thread): at most 3, oldest first; one older than max_age (300 ms) when the
/// slot frees is dropped (review N6: a slow poll must not send a gesture half a second late).
constexpr double k_press_max_age = 0.3;
struct PressQ {
    uint16_t b[3] = { 0, 0, 0 }; uint8_t h[3] = { 0, 0, 0 }; double t[3] = { 0.0, 0.0, 0.0 }; int n = 0; uint32_t full = 0, stale = 0;
    bool push(uint16_t btn, int hand, double now = 0.0) {
        if (!btn) return false;
        if (n >= 3) { full++; return false; }
        b[n] = btn; h[n] = static_cast<uint8_t>(hand & 1); t[n] = now; n++; return true;
    }
    uint16_t pop(int* hand, double now = 0.0, double max_age = 1e300, double* when = nullptr) {
        while (n > 0) {
            const uint16_t r = b[0]; const int hh = h[0]; const double tt = t[0];
            for (int i = 1; i < n; ++i) { b[i - 1] = b[i]; h[i - 1] = h[i]; t[i - 1] = t[i]; }
            n--;
            if (now - tt > max_age) { stale++; continue; }
            if (hand) *hand = hh;
            if (when) *when = tt;
            return r;
        }
        return 0;
    }
    void clear() { n = 0; }
};
/// A quick squeeze and a punch of the same hand within this are one shoulder press, not two (review S3: clenching to punch).
/// Both sides are judged when they are SENT (round 2 N4): a punch is dropped at the slot if that hand's squeeze went within
/// 0.3 s of the punch, and a squeeze is dropped only after a punch that was really sent, so one of the two always goes.
constexpr double k_shoulder_clash = 0.3;
inline bool shoulder_clash(double a, double b) { return a - b < k_shoulder_clash && b - a < k_shoulder_clash; }
/// The press slot's rule (pad::press_hook): held for k_press_polls new packets and at least k_press_ms, then one clear packet.
/// 3 packets / 80 ms is the tap's proven length (a new-press edge in every poll pattern, ffxvr_hands.cpp hv_tap_hook) and
/// under the game's repeat (0.2333 s); about 4 polls a press, 6 a second at 24 polls a second.
constexpr int32_t k_press_ms = 80, k_press_polls = 3;

// -- the odg: line ---------------------------------------------------------------------------------------------------------
struct Sess {
    int od = OD_NONE; bool inject = false; double t0 = 0.0, t1 = 0.0;
    float turns[2] = { 0.f, 0.f }; double out_turns = 0.0, sent_turns = 0.0, dropped_turns = 0.0; uint32_t lead_changes = 0;   // Lulu
    int sub0 = -1, sub1 = -1, cnt0 = -1, cnt1 = -1;                                                                         // Lulu's spins / counter at the open and the close
    uint32_t cls_n[C_N] = {}, why_n[R_N] = {}; uint32_t sent = 0, queue_full = 0, stale = 0, clashes = 0;               // strokes, presses sent
    uint32_t restarts = 0;                                                                                                  // Tidus: the bar restarted
    uint32_t samples[2] = { 0, 0 }, inferred[2] = { 0, 0 };                                                                 // poses seen / valid but untracked
    uint32_t rev_allowed = 0;                                                                                               // Auron: bring-backs sent (the game expected them)
    int len = 0, prog_max = 0; uint32_t resets = 0, game_presses = 0;                                                        // Auron
    bool targets = false;                                                                                                   // Auron in the targets scheme (its own odt: line)
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
    else if (x.od == OD_AURON && x.targets) k = fmt_append(s, n, k, "; strike the targets (the odt: line); presses sent %u; progress best %d of %d, wrong-press resets %u, game presses seen %u", x.sent, x.prog_max, x.len, x.resets, x.game_presses);
    else {
        k = fmt_append(s, n, k, "; strokes:");
        for (int c = C_LEFT; c < C_N; ++c) if (x.cls_n[c]) k = fmt_append(s, n, k, " %s %u", k_cls[c], x.cls_n[c]);
        uint32_t refused = 0; for (int w = R_DIAGONAL; w < R_N; ++w) refused += x.why_n[w];
        k = fmt_append(s, n, k, "%s; refused %u", (x.cls_n[C_LEFT] | x.cls_n[C_RIGHT] | x.cls_n[C_UP] | x.cls_n[C_DOWN] | x.cls_n[C_THRUST] | x.cls_n[C_SWING]) ? "" : " none", refused);
        if (refused) { k = fmt_append(s, n, k, " ("); bool first = true; for (int w = R_DIAGONAL; w < R_N; ++w) if (x.why_n[w]) { k = fmt_append(s, n, k, "%s%s %u", first ? "" : ", ", k_why[w], x.why_n[w]); first = false; } k = fmt_append(s, n, k, ")"); }
        k = fmt_append(s, n, k, "; presses sent %u", x.sent);
        if (x.queue_full) k = fmt_append(s, n, k, " (%u dropped: queue full)", x.queue_full);
        if (x.stale) k = fmt_append(s, n, k, " (%u dropped: older than 300 ms)", x.stale);
        if (x.clashes) k = fmt_append(s, n, k, " (%u punches dropped: the quick squeeze sent that shoulder)", x.clashes);
        if (x.od == OD_AURON) k = fmt_append(s, n, k, "; progress best %d of %d, wrong-press resets %u, game presses seen %u", x.prog_max, x.len, x.resets, x.game_presses);
        if (x.od == OD_TIDUS) k = fmt_append(s, n, k, "; the bar restarted %u times (a press outside the zone)", x.restarts);
    }
    if (x.od == OD_AURON && x.rev_allowed) k = fmt_append(s, n, k, "; opposite pairs sent at once %u (the game expected them)", x.rev_allowed);
    k = fmt_append(s, n, k, "; poses untracked (inferred) L %.0f %% / R %.0f %%", x.samples[0] ? 100.0 * x.inferred[0] / x.samples[0] : 0.0, x.samples[1] ? 100.0 * x.inferred[1] / x.samples[1] : 0.0);
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
    bool grab[2] = { false, false };                          // the hand is grabbing the world (the seat / diorama grip): no gestures
    double squeeze_t[2] = { -1e300, -1e300 };                 // the last quick squeeze (LB / RB) of each hand, the same clock as t
    bool inferred[2] = { false, false };                      // the pose is valid but not tracked (inferred): counted for the odg: line
    const uint16_t* seq = nullptr; int seq_n = 0;             // Auron's expected sequence as read (the bring-back exception)
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
    bool tidus_restart = false;                         // Tidus: the bar restarted this tick (a press outside the zone, ours or a button)
    bool clash[2] = { false, false };                   // a punch dropped: that hand's quick squeeze already sent the shoulder
    bool learned = false;                               // Auron: the learned map changed this tick (logged)
    bool spin = false;                                  // Lulu: the game counted a spin this tick (the visuals' flash)
    bool refused[2] = { false, false };                 // a deliberate stroke was refused this tick (diagonal, mixed, pulled back, the bring-back, a reach up)
    bool disagree = false;                              // ... and the value differs from the map in use (logged as a disagreement)
    void pulse(int hand, float amp, float sec) { if (nh < 6) { hap[nh].hand = hand; hap[nh].amp = amp; hap[nh].sec = sec; nh++; } }
};
struct Driver {
    Cfg cfg; CircleCfg cc; StrokeCfg sc;
    Circle circ[2]; CircleMix mix; Stroke st[2]; PressQ q; Sess sess, last; bool open = false;
    int last_hand = 1; uint16_t last_btn = 0; double last_btn_t = -1e300;   // the last press sent (Auron's capture line)
    int16_t prog = 0; uint8_t sub = 0, cnt = 0; uint16_t gp = 0; double second_at = -1.0, circ_t = -1e300;
    bool have_cur = false; int16_t cur = 0; double cur_t = 0.0;   // Tidus's cursor last tick and when (the restart test)
    uint16_t learned[16] = {};                          // Auron: our button (XInput bit index) -> the game's new-press value seen right after we sent it
    uint32_t learned_n = 0, disagree_n = 0;            // ... a cross-check of map: values that differ from it
    AuronMap map = k_auron_map;                         // Auron's value -> button map in use (the caller sets it at a window's open: the game's bindings or the fallback)
    double thrust_t[2] = { -1e300, -1e300 };            // the last punch queued per hand (the quick squeeze's clash test)
    static constexpr double k_stick_linger = 0.25;   // s: the stick channel stays on this long after the circling stops
    uint32_t stick_sent0 = 0, stick_dropped0 = 0; uint32_t windows = 0;
    void apply_cfg() { cc.r_min = clampf(cfg.circle_r, 0.01f, 0.20f); sc.start = clampf(cfg.slash_speed, 0.8f, 5.f); sc.end = sc.start * 0.375f; sc.swing_peak = sc.start * 1.25f; }
    static int bit_of(uint16_t b) { for (int i = 0; i < 16; ++i) if (b == (1u << i)) return i; return -1; }
    /// The classes whose button the game expects next (seq[progress]) by the map in use: allowed as a bring-back.
    int allow_mask(const FrameIn& in, int hand) const {
        if (in.seq == nullptr || in.x.auron_prog < 0 || in.x.auron_prog >= in.seq_n) return 0;
        const uint16_t want = in.seq[in.x.auron_prog]; if (want == 0) return 0;
        int m = 0;
        for (int c = C_LEFT; c <= C_THRUST; ++c) { const uint16_t b = button_for(OD_AURON, c, hand); if (b && map.value_of(b) == want) m |= 1 << c; }
        return m;
    }
    /// A quick squeeze of hand h now (t: this clock) is the same shoulder as a punch just queued in an injecting Bushido window:
    /// the caller drops the squeeze (review S3). The other order (the squeeze first) is in tick, from FrameIn::squeeze_t.
    bool squeeze_clashes(int h, double t) const { return open && sess.od == OD_AURON && sess.inject && h >= 0 && h < 2 && shoulder_clash(t, thrust_t[h]); }
    /// The grips must not grab the world now (review S4): an injecting Bushido window, where a clenched punch is a punch.
    bool grips_are_gestures() const { return open && sess.od == OD_AURON && inject_now == OD_AURON; }
    /// The targets scheme's press (ffxvr_odtarget.h Runner), recorded as ours: the capture line's "our last press" and the
    /// learned map's cross-check (as a press from the queue below).
    void note_press(uint16_t b, int hand, double t) { if (!b) return; sess.sent++; last_hand = hand & 1; last_btn = b; last_btn_t = t; }
    bool targets_now() const { return cfg.auron && cfg.auron_mode == 2; }
    int inject_now = OD_NONE;
    /// The hand whose poses were mostly inferred in the open window (over 50 % of at least 30 samples), or -1 (round 2 S3: a runtime
    /// that never reports POSITION_TRACKED would make every gesture silently dead).
    int untracked_hand() const {
        if (!open) return -1;
        for (int h = 1; h >= 0; --h) if (sess.samples[h] >= 30 && sess.inferred[h] * 2 > sess.samples[h]) return h;
        return -1;
    }
    static int word(const od::Raw& r, int od) { return od == OD_LULU ? r.lulu : od == OD_AURON ? r.auron : od == OD_TIDUS ? r.tidus : 0; }
    void close_now(const FrameIn& in) {
        sess.t1 = in.t; sess.end_state = in.r.ok ? word(in.r, sess.od) : -1; sess.end_timer = in.r.timer_ok ? in.r.timer : 0;
        sess.turns[0] = circ[0].turns; sess.turns[1] = circ[1].turns; sess.out_turns = std::fabs(mix.angle) / k_two_pi; sess.lead_changes = mix.lead_changes;
        sess.sent_turns = static_cast<double>(in.stick_sent - stick_sent0) / k_turn; sess.dropped_turns = static_cast<double>(in.stick_dropped - stick_dropped0) / k_turn;
        sess.queue_full = q.full; sess.stale = q.stale;
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
            thrust_t[0] = thrust_t[1] = -1e300;
            mix.reset(); q.clear(); q.full = q.stale = 0; second_at = -1.0; last_btn = 0; last_btn_t = -1e300; circ_t = -1e300; have_cur = false;
            prog = in.x.auron_prog; sub = in.x.lulu_sub; cnt = in.x.lulu_cnt; gp = in.game_press_ok ? in.game_press : 0;
            stick_sent0 = in.stick_sent; stick_dropped0 = in.stick_dropped;
            if (w == OD_LULU) { sess.sub0 = sess.sub1 = sub; sess.cnt0 = sess.cnt1 = cnt; }
            if (w == OD_AURON) { sess.len = in.x.auron_len; sess.targets = targets_now(); }
            o.opened = true;
        }
        const int inj = inject_gate(w, cfg, in.headset, in.vrpad);
        o.win = w; o.inject = inj; inject_now = inj;
        if (!open) return;
        if (inj != OD_NONE) sess.inject = true;
        const bool hap = cfg.haptics && inj != OD_NONE;
        if (o.opened && hap) { o.pulse(0, 0.5f, 0.04f); o.pulse(1, 0.5f, 0.04f); second_at = in.t + 0.12; }
        if (second_at > 0.0 && in.t >= second_at) { if (hap) { o.pulse(0, 0.5f, 0.04f); o.pulse(1, 0.5f, 0.04f); } second_at = -1.0; }
        if (w == OD_LULU) {
            if (in.sample_new) {
                float d[2]; bool c[2];
                for (int h = 0; h < 2; ++h) { d[h] = circ[h].step(in.p[h], in.valid[h] && !in.grab[h], in.t, cc); c[h] = circ[h].circling; sess.samples[h]++; if (in.inferred[h]) sess.inferred[h]++; }
                mix.step(d, c);
                if (c[0] || c[1]) circ_t = in.t;
            }
            if (inj == OD_LULU && in.t - circ_t < k_stick_linger) { o.stick_on = true; o.target = mix.published(); }
            const int lh = mix.lead < 0 ? 1 : mix.lead;
            if (in.x.lulu_sub != sub) { sub = in.x.lulu_sub; o.spin = true; if (hap) o.pulse(lh, 0.3f, 0.015f); }
            if (in.x.lulu_cnt != cnt) { cnt = in.x.lulu_cnt; if (hap) o.pulse(lh, 0.6f, 0.04f); }
            sess.sub1 = sub; sess.cnt1 = cnt;
        }
        else {
            Axes ax; const bool ax_ok = yaw_axes(in.head_fwd, ax);
            const bool tgt = w == OD_AURON && sess.targets;   // the targets scheme: no strokes (the Runner strikes), poses still counted
            for (int h = 0; h < 2; ++h) {
                if (w == OD_TIDUS && h == 0) continue;   // Tidus: the right hand's swing
                int cls = C_NONE;
                if (!in.sample_new) continue;
                sess.samples[h]++; if (in.inferred[h]) sess.inferred[h]++;
                if (tgt) continue;
                const int allow = w == OD_AURON ? allow_mask(in, h) : 0;
                const int ev = st[h].step(in.p[h], in.valid[h] && !in.grab[h] && ax_ok, in.t, ax, sc, w == OD_TIDUS ? ST_SWING : ST_DIR, cls, allow);
                if ((ev & EV_FIRE) && st[h].last_rev) sess.rev_allowed++;
                if (ev & EV_FIRE) {
                    sess.cls_n[cls]++; o.fired[h] = cls;
                    const uint16_t b = button_for(w, cls, h); o.fired_btn[h] = b;
                    if (inj == w && cls == C_THRUST && shoulder_clash(in.t, in.squeeze_t[h])) { o.clash[h] = true; sess.clashes++; }
                    else if (inj == w && q.push(b, h, in.t)) { o.queued[h] = true; if (hap) o.pulse(h, 0.5f, 0.025f); }
                }
                else if ((ev & EV_END) && st[h].last_why > R_SENT && st[h].last_why < R_N) {
                    sess.why_n[st[h].last_why]++;
                    const int y = st[h].last_why;
                    o.refused[h] = y == R_DIAGONAL || y == R_MIXED || y == R_BACK || y == R_RETURN || y == R_UP;
                }
            }
            if (w == OD_AURON) {
                sess.len = in.x.auron_len;
                const int16_t pr = in.x.auron_prog;
                if (pr > prog && hap && !tgt && in.t - last_btn_t < 0.5) o.pulse(last_hand, 0.6f, 0.03f);   // ours: tick that hand (review N9; the targets: the hit already ticked)
                if (pr == 0 && prog > 0) { sess.resets++; if (hap) { o.pulse(0, 0.9f, 0.12f); o.pulse(1, 0.9f, 0.12f); } }
                prog = pr; if (pr > sess.prog_max) sess.prog_max = pr;
                const uint16_t g = in.game_press_ok ? in.game_press : 0;
                if (g != 0 && g != gp) {
                    sess.game_presses++; o.game_press_new = true;
                    // the map our bring-back exception needs: the game's value for the button we sent just before
                    const int bi = bit_of(last_btn);
                    if (bi >= 0 && in.t - last_btn_t < 0.3 && learned[bi] != g) {
                        learned[bi] = g; learned_n++; o.learned = true;
                        if (map.value_of(last_btn) != g) { disagree_n++; o.disagree = true; }   // the map in use says otherwise: logged
                    }
                }
                gp = g;
            }
            if (w == OD_TIDUS) {   // the bar's restart: the cursor jumps (its own bounces are reflections, continuous)
                // at most |v| a logic tick: the ticks since the last look (up to 60 a second, one more for the phase, at least 2)
                // bound the travel, so a hitch that spans several ticks is no restart (round 2 N3)
                const int16_t c = in.r.cursor; const int v = in.x.tidus_vel < 0 ? -in.x.tidus_vel : in.x.tidus_vel;
                if (have_cur) {
                    const double el = in.t - cur_t; int ticks = el > 0.0 ? static_cast<int>(std::ceil(el * 60.0 - 1e-6)) + 1 : 2;
                    if (ticks < 3) ticks = 3;
                    if (ticks > 1000) ticks = 1000;
                    const int jump = c > cur ? c - cur : cur - c;
                    if (jump > ticks * v + 4) { o.tidus_restart = true; sess.restarts++; if (hap) o.pulse(1, 0.9f, 0.12f); }
                }
                have_cur = true; cur = c; cur_t = in.t;
            }
        }
        if (inj == OD_NONE) q.clear();
        else if (in.press_free && q.n > 0) {
            int hh = 0; double qt = 0.0; uint16_t b = q.pop(&hh, in.t, k_press_max_age, &qt);
            while (b && (b == B_LSHOULDER || b == B_RSHOULDER) && shoulder_clash(qt, in.squeeze_t[hh])) {   // a punch whose squeeze went since: one shoulder (N4)
                o.clash[hh] = true; sess.clashes++; b = q.pop(&hh, in.t, k_press_max_age, &qt);
            }
            if (b) {
                o.press = b; o.press_hand = hh; sess.sent++; last_hand = hh; last_btn = b; last_btn_t = in.t;
                if (b == B_LSHOULDER || b == B_RSHOULDER) thrust_t[hh] = in.t;   // only a punch really sent drops a later squeeze
            }
        }
    }
};

}  // namespace odg
