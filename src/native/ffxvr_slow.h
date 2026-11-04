// SPDX-License-Identifier: MIT
// ffxvr_slow.h -- the slow-GPU guard's decisions (the overdrive freeze, 3 Nov 2026): the trigger and its hysteresis, the
// per-mode GPU-span baselines and the step sequencer. Pure logic: no Windows, no D3D, no logging, no allocation. The
// native side (ffxvr_native.cpp, "slow-GPU guard") feeds it one In per Present on the render thread, applies `off` (the
// items switched off now) and writes the log lines for the events it returns. A unit test drives it with synthetic frames.
//
// The episode: WATCH -> (120 slow frames) ONSET -> CENSUS (1 s: the frame census is written) -> ITEM for each optional
// item that is on (off for about 2 s each; back on when it did not help) -> RESTART_WAIT / RESTART_MEASURE (the session
// restart, at most once per 5 min and 3 a session) -> COOL. An item that helped stays off for 10 s, comes back on, and
// the frames are watched for 2.5 s (VERIFY): still fast = the cause had passed; slow again = the item stays off for 5 min
// (LATCHED). Every path ends in COOL (30 s, or 5 min after giving up), at most 6 episodes a session: nothing can loop.
#pragma once
#include <cstdint>

namespace slow {

enum Item { IT_UI = 0, IT_UIDEPTH, IT_HANDS, IT_IMGUI, IT_SCREEN, IT_N };
inline const char* item_name(int i) {
    static const char* const n[IT_N] = { "ui_capture", "ui_depth", "vr_hands", "imgui_layer", "screen_mono" };
    return i >= 0 && i < IT_N ? n[i] : "?";
}

struct Cfg {   // milliseconds unless named otherwise
    int    run_frames = 120;      // slow frames for the trigger (non-slow frames up to miss_max are tolerated inside a run)
    int    miss_max = 6;
    double span_floor = 33.0, span_mult = 3.0, end_min = 20.0;      // slow: span > max(3 x baseline, 33) and xrEndFrame > 20
    double gap_stall = 250.0, quiet_stall = 2000.0, quiet_excl = 1000.0;   // a Present gap over 250 ms (a stall #N, a load hitch)
    double fast_mult = 2.0, fast_floor = 25.0, fast_end = 10.0;     // fast: span <= max(2 x baseline, 25) and xrEndFrame <= 10
    double fast_frac = 0.75; int min_samples = 8;                   // a window "helped" with 75 % fast frames, 8 at least
    double census_wait = 1000.0, settle = 500.0, measure = 2000.0, step_wall = 8000.0;
    double hold = 10000.0, verify_measure = 2500.0, latch = 300000.0;
    double restart_gap = 300000.0; int restarts_max = 3; double restart_wait = 15000.0, restart_up = 1000.0, restart_settle = 1500.0;
    double cool_ok = 30000.0, cool_fail = 300000.0, cool_abort = 30000.0;
    int    episodes_max = 6; double spent_gap = 60000.0;
    double base_cap = 25.0, base_keep = 2.0; int base_n0 = 64; double base_alpha = 1.0 / 256.0;   // the baseline: spans up to 25 ms
                                                  // and, once learnt (64 frames), up to 2 x itself; a running mean, then an EMA
};

struct In {
    double   now = 0;           // ms, monotonic
    double   frame_ms = 0;      // Present to Present (the frame just finished)
    bool     span_ok = false;   // a GPU span was read at this Present
    double   span = 0;          // ... its length (ms)
    double   end = 0;           // xrEndFrame of the last submit (ms)
    bool     screen = false;    // the mono virtual screen (baseline 1); else 3D or the stereo screen (baseline 0)
    bool     running = false;   // the OpenXR session runs (the headset is on)
    bool     excluded = false;  // a battle transition window, a load phase, the pick tool's slow motion
    unsigned avail = 0;         // the items that are on now and may be switched off (bit per Item)
    bool     enabled = true;    // ini slow_guard
    bool     restart_ok = true; // ini slow_guard_restart
};

enum Ev { EV_NONE = 0, EV_ONSET, EV_ITEM_OFF, EV_ITEM_RESULT, EV_HOLD_END, EV_RECOVERED, EV_LATCHED, EV_UNLATCHED, EV_RESTART, EV_GAVE_UP, EV_ABORT, EV_SPENT };
enum St { ST_OFF = 0, ST_WATCH, ST_CENSUS, ST_ITEM, ST_HOLD, ST_VERIFY, ST_RESTART_WAIT, ST_RESTART_MEASURE, ST_COOL };
enum Fix { FIX_NONE = -1, FIX_RESTART = 100, FIX_PASSED = 101, FIX_NOTHING = 102, FIX_ABORTED = 103 };   // or an Item: it helped
enum Why { WHY_NONE = 0, WHY_HEADSET_OFF, WHY_DISABLED, WHY_NO_RESTART, WHY_RESTARTS_MAX, WHY_RESTART_GAP, WHY_NOT_BACK, WHY_STILL_SLOW };
inline const char* why_text(int w) {
    switch (w) {
        case WHY_HEADSET_OFF:  return "the headset is off";
        case WHY_DISABLED:     return "slow_guard=0";
        case WHY_NO_RESTART:   return "nothing helped and slow_guard_restart=0";
        case WHY_RESTARTS_MAX: return "nothing helped and the session was already restarted 3 times this session";
        case WHY_RESTART_GAP:  return "nothing helped and the last session restart was less than 5 min ago";
        case WHY_NOT_BACK:     return "the session did not come back within 15 s of the restart";
        case WHY_STILL_SLOW:   return "still slow after the session restart";
        default:               return "";
    }
}

struct Out { Ev ev = EV_NONE; int item = -1; bool helped = false; int fast = 0, n = 0; double span_avg = 0, end_avg = 0; int why = WHY_NONE; };

struct Window {
    double t0 = 0; int n = 0, fast = 0; double span_sum = 0, end_sum = 0;
    void start(double t) { t0 = t; n = fast = 0; span_sum = end_sum = 0; }
};

class Guard {
public:
    Cfg c;
    St  st = ST_WATCH;
    unsigned off = 0;                       // the items switched off now (the native side applies it)
    unsigned latched = 0; double latch_until = 0;
    double base[2] = { 0, 0 }; int base_n[2] = { 0, 0 };
    int    run = 0, miss = 0; double run_span = 0, run_end = 0, run_frame = 0, run_t0 = 0, quiet_until = -1e300;
    int    episodes = 0, restarts = 0; double last_restart = -1e300, spent_at = -1e300;
    int    item = -1; Window w; double t_state = 0, t_onset = 0, cool_until = 0;
    // the last episode, for the overlay and the log: its trigger and how it ended
    double last_span = 0, last_end = 0, last_frame = 0, last_base = 0, last_dur = 0; bool last_screen = false; int last_frames = 0;
    int    last_fix = FIX_NONE; bool last_ended = false;

    bool   active() const { return st == ST_CENSUS || st == ST_ITEM || st == ST_HOLD || st == ST_VERIFY || st == ST_RESTART_WAIT || st == ST_RESTART_MEASURE; }
    double thr_slow(bool screen) const { const double t = c.span_mult * base[screen ? 1 : 0]; return t > c.span_floor ? t : c.span_floor; }
    double thr_fast(bool screen) const { const double t = c.fast_mult * base[screen ? 1 : 0]; return t > c.fast_floor ? t : c.fast_floor; }
    bool   is_fast(const In& in) const { return in.span_ok && in.span <= thr_fast(in.screen) && in.end <= c.fast_end; }

    /// One Present. Writes up to `max_out` events (4 is always enough) and returns how many.
    int tick(const In& in, Out* out, int max_out) {
        n_out = 0; o = out; o_max = max_out;
        if (!in.enabled) {
            if (st != ST_OFF) {
                const bool was = active() || off != 0;
                off = 0; latched = 0; reset_run();
                if (active()) end_episode(in.now, 0, FIX_ABORTED);
                st = ST_OFF;
                if (was) emit(EV_ABORT).why = WHY_DISABLED;
            }
            return n_out;
        }
        if (st == ST_OFF) { st = ST_WATCH; reset_run(); }
        if (latched != 0 && !active() && in.now >= latch_until) {
            const int it = first_bit(latched); off &= ~latched; latched = 0; emit(EV_UNLATCHED).item = it;
        }
        if (active() && !in.running && st != ST_RESTART_WAIT) {
            off = latched; emit(EV_ABORT).why = WHY_HEADSET_OFF; end_episode(in.now, c.cool_abort, FIX_ABORTED);
            return n_out;
        }
        switch (st) {
            case ST_COOL:  if (in.now >= cool_until) { st = ST_WATCH; reset_run(); } break;
            case ST_WATCH: watch(in); break;
            case ST_CENSUS: if (in.now - t_state >= c.census_wait) next_item(in); break;
            case ST_ITEM: {
                if (!sample_done(in, c.settle, c.measure)) break;
                Out& r = emit(EV_ITEM_RESULT); fill(r, item);
                if (r.helped) { st = ST_HOLD; t_state = in.now; }
                else { off &= ~(1u << item); next_item(in); }
            } break;
            case ST_HOLD:
                if (in.now - t_state >= c.hold) { off &= ~(1u << item); emit(EV_HOLD_END).item = item; st = ST_VERIFY; w.start(in.now); }
                break;
            case ST_VERIFY: {
                if (!sample_done(in, c.settle, c.verify_measure)) break;
                const int it = item; Out r; fill(r, it);
                if (r.helped) { Out& e = emit(EV_RECOVERED); e = r; e.ev = EV_RECOVERED; end_episode(in.now, c.cool_ok, FIX_PASSED); last_fix_item = it; }
                else {
                    latched = 1u << it; off |= latched; latch_until = in.now + c.latch;
                    Out& e = emit(EV_LATCHED); e = r; e.ev = EV_LATCHED; end_episode(in.now, c.cool_ok, it);
                }
            } break;
            case ST_RESTART_WAIT:
                if (in.running && in.now - t_state >= c.restart_up) { st = ST_RESTART_MEASURE; w.start(in.now); }
                else if (in.now - t_state >= c.restart_wait) { emit(EV_GAVE_UP).why = WHY_NOT_BACK; end_episode(in.now, c.cool_fail, FIX_NOTHING); }
                break;
            case ST_RESTART_MEASURE: {
                if (!sample_done(in, c.restart_settle, c.measure)) break;
                Out r; fill(r, -1);
                if (r.helped) { Out& e = emit(EV_RECOVERED); e = r; e.ev = EV_RECOVERED; end_episode(in.now, c.cool_ok, FIX_RESTART); }
                else { Out& e = emit(EV_GAVE_UP); e = r; e.ev = EV_GAVE_UP; e.why = WHY_STILL_SLOW; end_episode(in.now, c.cool_fail, FIX_NOTHING); }
            } break;
            default: break;
        }
        return n_out;
    }
    int last_fix_item = -1;   // FIX_PASSED: the item that was off when the frames came back

private:
    Out* o = nullptr; int n_out = 0, o_max = 0; Out spare;
    Out& emit(Ev e) {
        if (n_out < o_max && o != nullptr) { o[n_out] = Out(); o[n_out].ev = e; return o[n_out++]; }
        spare = Out(); spare.ev = e; return spare;
    }
    static int first_bit(unsigned m) { for (int i = 0; i < 32; ++i) if (m & (1u << i)) return i; return -1; }
    void reset_run() { run = miss = 0; run_span = run_end = run_frame = 0; }
    void base_update(int m, double span) {
        if (span > c.base_cap || span <= 0) return;                                 // a slow-ish frame never raises the baseline
        if (base_n[m] >= c.base_n0 && span > c.base_keep * base[m]) return;
        if (base_n[m] < c.base_n0) { base_n[m]++; base[m] += (span - base[m]) / base_n[m]; }
        else base[m] += (span - base[m]) * c.base_alpha;
    }
    void watch(const In& in) {
        if (!in.running) { reset_run(); return; }
        if (in.excluded) { reset_run(); if (in.now + c.quiet_excl > quiet_until) quiet_until = in.now + c.quiet_excl; return; }
        if (in.frame_ms > c.gap_stall) { reset_run(); quiet_until = in.now + c.quiet_stall; return; }
        if (in.now < quiet_until || !in.span_ok) return;
        const int m = in.screen ? 1 : 0;
        const bool cand = in.span > thr_slow(in.screen) && in.end > c.end_min;
        if (cand) { if (run == 0) run_t0 = in.now; run++; run_span += in.span; run_end += in.end; run_frame += in.frame_ms; }
        else { base_update(m, in.span); if (run > 0 && ++miss > c.miss_max) reset_run(); }
        if (run < c.run_frames) return;
        last_span = run_span / run; last_end = run_end / run; last_frame = run_frame / run; last_base = base[m]; last_screen = in.screen; last_frames = run;
        last_dur = in.now - run_t0;
        reset_run();
        if (episodes >= c.episodes_max) {
            if (in.now - spent_at >= c.spent_gap) { spent_at = in.now; Out& e = emit(EV_SPENT); e.span_avg = last_span; e.end_avg = last_end; }
            return;
        }
        episodes++; t_onset = in.now; item = -1; st = ST_CENSUS; t_state = in.now; last_fix = FIX_NONE; last_ended = false; last_fix_item = -1;
        Out& e = emit(EV_ONSET); e.span_avg = last_span; e.end_avg = last_end; e.n = last_frames;
    }
    void next_item(const In& in) {
        for (int i = item + 1; i < IT_N; ++i) {
            if (!(in.avail & (1u << i)) || (latched & (1u << i))) continue;
            item = i; off |= 1u << i; st = ST_ITEM; w.start(in.now); emit(EV_ITEM_OFF).item = i;
            return;
        }
        item = IT_N;
        int why = WHY_NONE;
        if (!in.restart_ok) why = WHY_NO_RESTART;
        else if (restarts >= c.restarts_max) why = WHY_RESTARTS_MAX;
        else if (in.now - last_restart < c.restart_gap) why = WHY_RESTART_GAP;
        if (why != WHY_NONE || !in.running) { emit(EV_GAVE_UP).why = why != WHY_NONE ? why : WHY_HEADSET_OFF; end_episode(in.now, c.cool_fail, FIX_NOTHING); return; }
        restarts++; last_restart = in.now; st = ST_RESTART_WAIT; t_state = in.now; emit(EV_RESTART);
    }
    /// Samples a frame into the window (after `settle`, frames with a span and not excluded); true when it is complete.
    bool sample_done(const In& in, double settle, double measure) {
        const double dt = in.now - w.t0;
        if (dt >= settle && !in.excluded && in.span_ok && in.running) {
            w.n++; if (is_fast(in)) w.fast++; w.span_sum += in.span; w.end_sum += in.end;
        }
        return (dt >= settle + measure && w.n >= c.min_samples) || dt >= c.step_wall;
    }
    void fill(Out& r, int it) const {
        r.item = it; r.n = w.n; r.fast = w.fast;
        r.helped = w.n >= c.min_samples && w.fast >= c.fast_frac * w.n;
        r.span_avg = w.n ? w.span_sum / w.n : 0; r.end_avg = w.n ? w.end_sum / w.n : 0;
    }
    void end_episode(double now, double cool, int fix) {
        last_fix = fix; last_ended = true; last_dur = now - t_onset;
        st = ST_COOL; cool_until = now + cool; item = -1; reset_run();
        off = latched;   // anything the episode switched off comes back on (an item latched just now stays off)
    }
};

}  // namespace slow
