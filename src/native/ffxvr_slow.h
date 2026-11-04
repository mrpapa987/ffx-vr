// SPDX-License-Identifier: MIT
// ffxvr_slow.h -- the slow-GPU guard's decisions (the overdrive freeze, 3 Nov 2026): the trigger and its hysteresis, the
// per-view GPU-span baselines and the step sequencer. Pure logic: no Windows, no D3D, no logging, no allocation. The
// native side (ffxvr_native.cpp, "slow-GPU guard") feeds it one In per Present on the render thread, applies `off` (the
// items switched off now) and writes the log lines for the events it returns. A unit test drives it with synthetic frames.
//
// The episode: WATCH -> (120 slow frames against a LEARNT baseline) ONSET -> CENSUS (1 s) -> ITEM for each optional item
// that is on (off for about 2.5 s; back on when it did not help) -> PRE_RESTART (everything back on, the slow state measured
// once more) -> RESTART_ARM (a FOCUSED frame outside a transition window / load phase) -> RESTART_WAIT -> RESTART_MEASURE
// -> COOL. An item that helped stays off for 10 s, comes back on, and the frames are checked for 2.5 s (VERIFY): still fast
// = the cause had passed; slow again = the item stays off for 5 min (LATCHED).
// Only measured windows decide (review B1-B3): a window that ends with fewer than min_samples measured frames (all frames
// excluded: the Quest menu, a load phase, a transition window) is repeated once, then skipped (a step) or ends the episode
// with everything back on (a verify); the restart needs 2 measured steps and the slow state measured right before it.
// Without a learnt baseline for the view (B2: a GPU-bound setup, the first seconds of a view) a slow run is only logged.
// Every path ends in COOL (30 s, or 5 min after giving up), at most 6 episodes a session: nothing can loop.
#pragma once
#include <cstdint>

namespace slow {

enum Item { IT_UI = 0, IT_UIDEPTH, IT_HANDS, IT_SCREEN, IT_N };   // (review SF1: the ImGui layer is never switched off: F8 and
inline const char* item_name(int i) {                             //  Fahrenheit's save screen are on it)
    static const char* const n[IT_N] = { "ui_capture", "ui_depth", "vr_hands", "screen_mono" };
    return i >= 0 && i < IT_N ? n[i] : "?";
}

struct Cfg {   // milliseconds unless named otherwise
    int    run_frames = 120;      // slow frames for the trigger (non-slow frames up to miss_max are tolerated inside a run)
    int    miss_max = 6;
    double span_floor = 33.0, span_mult = 3.0, end_min = 20.0;      // slow: span > max(3 x baseline, 33) and xrEndFrame > 20
    double gap_stall = 250.0, quiet_stall = 2000.0, quiet_excl = 1000.0;   // a Present gap over 250 ms (a stall #N, a load hitch)
    double fast_mult = 2.0, fast_floor = 25.0, fast_end = 10.0;     // fast: span <= max(2 x baseline, 25) and xrEndFrame <= 10
    double fast_frac = 0.75, slow_frac = 0.75; int min_samples = 8; // a window "helped" with 75 % fast frames; "still slow" with
                                                                    // 75 % slow ones; a window needs 8 measured frames to decide
    double census_wait = 1000.0, settle = 500.0, measure = 2000.0, step_wall = 8000.0;
    double hold = 10000.0, verify_measure = 2500.0, latch = 300000.0, pre_measure = 1500.0, arm_wait = 5000.0;
    int    restart_min_steps = 2;
    double restart_gap = 300000.0; int restarts_max = 3; double restart_wait = 15000.0, restart_up = 1000.0, restart_settle = 1500.0;
    double cool_ok = 30000.0, cool_fail = 300000.0, cool_abort = 30000.0;
    int    episodes_max = 6; double spent_gap = 60000.0, unlearnt_gap = 600000.0;
    // The baseline (review B2): the median of the measured spans outside episodes, from a histogram (0.5 ms bins up to
    // 120 ms) that halves once it holds 8192 frames (so it reflects the last ~1-2 min at 120 Hz). No 25 ms cap: a setup
    // that always runs at 36 ms learns 36 and is never "slow" against itself. Learnt once it holds base_min frames.
    double bin_ms = 0.5; int bins = 240; int base_halve = 8192, base_min = 600;
};

struct In {
    double   now = 0;           // ms, monotonic
    double   frame_ms = 0;      // Present to Present (the frame just finished)
    bool     span_ok = false;   // a GPU span was read at this Present
    double   span = 0;          // ... its length (ms)
    double   end = 0;           // xrEndFrame of the last submit (ms)
    bool     screen = false;    // the span's frame went to the mono virtual screen (baseline 1); else 3D / the stereo screen (0)
    bool     running = false;   // the OpenXR session runs
    bool     focused = false;   // ... and is FOCUSED (the restart needs it)
    bool     wanted = true;     // the user wants the headset on (false: an abort, also while a restart is pending)
    bool     excluded = false;  // a battle transition window, a load phase, slow motion, a session not FOCUSED
    unsigned avail = 0;         // the items that are on now and may be switched off (bit per Item)
    bool     enabled = true;    // ini slow_guard
    bool     restart_ok = true; // ini slow_guard_restart
};

enum Ev { EV_NONE = 0, EV_ONSET, EV_ITEM_OFF, EV_ITEM_RESULT, EV_HOLD_END, EV_RECOVERED, EV_LATCHED, EV_UNLATCHED, EV_RESTART, EV_GAVE_UP,
          EV_ABORT, EV_SPENT, EV_UNMEASURED, EV_PRE_CHECK, EV_LOG_ONLY };
enum St { ST_OFF = 0, ST_WATCH, ST_CENSUS, ST_ITEM, ST_HOLD, ST_VERIFY, ST_PRE_RESTART, ST_RESTART_ARM, ST_RESTART_WAIT, ST_RESTART_MEASURE, ST_COOL };
enum Fix { FIX_NONE = -1, FIX_RESTART = 100, FIX_PASSED = 101, FIX_NOTHING = 102, FIX_ABORTED = 103 };   // or an Item: it helped
enum Why { WHY_NONE = 0, WHY_HEADSET_OFF, WHY_DISABLED, WHY_NO_RESTART, WHY_RESTARTS_MAX, WHY_RESTART_GAP, WHY_NOT_BACK, WHY_STILL_SLOW,
           WHY_FEW_STEPS, WHY_PRE_UNMEASURED, WHY_NOT_SLOW_NOW, WHY_NO_FOCUS, WHY_VERIFY_UNMEASURED, WHY_AFTER_UNMEASURED,
           WHY_REPEAT, WHY_SKIPPED, WHY_UNLEARNT };
inline const char* why_text(int w) {
    switch (w) {
        case WHY_HEADSET_OFF:       return "the headset is off";
        case WHY_DISABLED:          return "slow_guard=0";
        case WHY_NO_RESTART:        return "nothing helped and slow_guard_restart=0";
        case WHY_RESTARTS_MAX:      return "nothing helped and the session was already restarted 3 times this session";
        case WHY_RESTART_GAP:       return "nothing helped and the last session restart was less than 5 min ago";
        case WHY_NOT_BACK:          return "the session did not come back within 15 s of the restart";
        case WHY_STILL_SLOW:        return "still slow after the session restart";
        case WHY_FEW_STEPS:         return "fewer than 2 steps could be measured (frames excluded: a menu, a load, a transition): no restart on that";
        case WHY_PRE_UNMEASURED:    return "the slow state could not be measured right before the restart: no restart";
        case WHY_NOT_SLOW_NOW:      return "with everything back on it is no longer clearly slow: no restart";
        case WHY_NO_FOCUS:          return "no FOCUSED frame outside a transition window or load phase within 5 s: no restart";
        case WHY_VERIFY_UNMEASURED: return "the re-check with the item back on could not be measured twice: it stays on";
        case WHY_AFTER_UNMEASURED:  return "the frames after the restart could not be measured";
        default:                    return "";
    }
}

struct Out { Ev ev = EV_NONE; int item = -1; bool helped = false; int fast = 0, slow = 0, n = 0; double span_avg = 0, end_avg = 0; int why = WHY_NONE; };

struct Window {
    double t0 = 0; int n = 0, fast = 0, slow = 0; double span_sum = 0, end_sum = 0;
    void start(double t) { t0 = t; n = fast = slow = 0; span_sum = end_sum = 0; }
};

class Guard {
public:
    Cfg c;
    St  st = ST_WATCH;
    unsigned off = 0;                       // the items switched off now (the native side applies it)
    unsigned latched = 0; double latch_until = 0;
    double base[2] = { 0, 0 }; bool learnt[2] = { false, false };
    int    run = 0, miss = 0; double run_span = 0, run_end = 0, run_frame = 0, run_t0 = 0, quiet_until = -1e300;
    int    episodes = 0, restarts = 0; double last_restart = -1e300, spent_at = -1e300, unlearnt_at = -1e300;
    int    item = -1; Window w; double t_state = 0, t_onset = 0, cool_until = 0; bool repeated = false; int measured_steps = 0;
    // the last episode, for the overlay and the log: its trigger and how it ended
    double last_span = 0, last_end = 0, last_frame = 0, last_base = 0, last_dur = 0; bool last_screen = false; int last_frames = 0;
    int    last_fix = FIX_NONE; bool last_ended = false; int last_fix_item = -1;   // FIX_PASSED: the item that was off then (-1: none)

    bool   active() const { return st >= ST_CENSUS && st <= ST_RESTART_MEASURE; }
    double thr_slow(bool screen) const { const double t = c.span_mult * base[screen ? 1 : 0]; return t > c.span_floor ? t : c.span_floor; }
    double thr_fast(bool screen) const { const double t = c.fast_mult * base[screen ? 1 : 0]; return t > c.fast_floor ? t : c.fast_floor; }
    bool   is_fast(const In& in) const { return in.span_ok && in.span <= thr_fast(in.screen) && in.end <= c.fast_end; }
    bool   is_slow(const In& in) const { return in.span_ok && in.span > thr_slow(in.screen) && in.end > c.end_min; }
    int    hist_n(int m) const { return hist_total[m]; }

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
        const bool gone = st == ST_RESTART_WAIT ? !in.wanted : !in.running;   // (review N2) a pending restart: only the user's untick
        if (active() && gone) {
            latched = 0; off = 0; emit(EV_ABORT).why = WHY_HEADSET_OFF; end_episode(in.now, c.cool_abort, FIX_ABORTED);   // (N3) all on
            return n_out;
        }
        if (!active() && in.span_ok && !in.excluded && in.running) learn(in.screen ? 1 : 0, in.span);
        switch (st) {
            case ST_COOL:  if (in.now >= cool_until) { st = ST_WATCH; reset_run(); } break;
            case ST_WATCH: watch(in); break;
            case ST_CENSUS: if (in.now - t_state >= c.census_wait) next_item(in); break;
            case ST_ITEM: {
                if (!sample_done(in, c.settle, c.measure)) break;
                if (w.n < c.min_samples) {   // B1: not measured: once more, then skipped (not "no change")
                    Out& u = emit(EV_UNMEASURED); fill(u, item);
                    if (!repeated) { repeated = true; u.why = WHY_REPEAT; w.start(in.now); }
                    else { u.why = WHY_SKIPPED; off &= ~(1u << item); next_item(in); }
                    break;
                }
                measured_steps++;
                Out& r = emit(EV_ITEM_RESULT); fill(r, item);
                if (r.helped) { st = ST_HOLD; t_state = in.now; }
                else { off &= ~(1u << item); next_item(in); }
            } break;
            case ST_HOLD:
                if (in.now - t_state >= c.hold) { off &= ~(1u << item); emit(EV_HOLD_END).item = item; st = ST_VERIFY; w.start(in.now); repeated = false; }
                break;
            case ST_VERIFY: {
                if (!sample_done(in, c.settle, c.verify_measure)) break;
                const int it = item; Out r; fill(r, it);
                if (w.n < c.min_samples) {   // B3: not measured: once more, then the item simply stays on (no latch)
                    if (!repeated) { repeated = true; Out& u = emit(EV_UNMEASURED); u = r; u.ev = EV_UNMEASURED; u.why = WHY_REPEAT; w.start(in.now); }
                    else { Out& e = emit(EV_GAVE_UP); e = r; e.ev = EV_GAVE_UP; e.why = WHY_VERIFY_UNMEASURED; end_episode(in.now, c.cool_ok, FIX_NOTHING); }
                    break;
                }
                if (r.helped) { Out& e = emit(EV_RECOVERED); e = r; e.ev = EV_RECOVERED; end_episode(in.now, c.cool_ok, FIX_PASSED); last_fix_item = it; }
                else {
                    latched = 1u << it; off |= latched; latch_until = in.now + c.latch;
                    Out& e = emit(EV_LATCHED); e = r; e.ev = EV_LATCHED; end_episode(in.now, c.cool_ok, it);
                }
            } break;
            case ST_PRE_RESTART: {   // B1 (d): everything is back on; is it measurably still slow?
                if (!sample_done(in, c.settle, c.pre_measure)) break;
                Out r; fill(r, -1);
                if (w.n < c.min_samples) { Out& e = emit(EV_GAVE_UP); e = r; e.ev = EV_GAVE_UP; e.why = WHY_PRE_UNMEASURED; end_episode(in.now, c.cool_fail, FIX_NOTHING); break; }
                if (r.helped) { Out& e = emit(EV_RECOVERED); e = r; e.ev = EV_RECOVERED; end_episode(in.now, c.cool_ok, FIX_PASSED); last_fix_item = -1; break; }
                if (w.slow < c.slow_frac * w.n) { Out& e = emit(EV_GAVE_UP); e = r; e.ev = EV_GAVE_UP; e.why = WHY_NOT_SLOW_NOW; end_episode(in.now, c.cool_fail, FIX_NOTHING); break; }
                st = ST_RESTART_ARM; t_state = in.now;
            } // fall through: the restart may go out on this very frame
            [[fallthrough]];
            case ST_RESTART_ARM:   // B1 (b) (c): only from a FOCUSED frame outside a transition window or load phase
                if (in.focused && in.running && !in.excluded) {
                    restarts++; last_restart = in.now; st = ST_RESTART_WAIT; t_state = in.now; emit(EV_RESTART);
                }
                else if (in.now - t_state >= c.arm_wait) { emit(EV_GAVE_UP).why = WHY_NO_FOCUS; end_episode(in.now, c.cool_fail, FIX_NOTHING); }
                break;
            case ST_RESTART_WAIT:
                if (in.running && in.now - t_state >= c.restart_up) { st = ST_RESTART_MEASURE; w.start(in.now); }
                else if (in.now - t_state >= c.restart_wait) { emit(EV_GAVE_UP).why = WHY_NOT_BACK; end_episode(in.now, c.cool_fail, FIX_NOTHING); }
                break;
            case ST_RESTART_MEASURE: {
                if (!sample_done(in, c.restart_settle, c.measure)) break;
                Out r; fill(r, -1);
                if (w.n < c.min_samples) { Out& e = emit(EV_GAVE_UP); e = r; e.ev = EV_GAVE_UP; e.why = WHY_AFTER_UNMEASURED; end_episode(in.now, c.cool_fail, FIX_NOTHING); }
                else if (r.helped) { Out& e = emit(EV_RECOVERED); e = r; e.ev = EV_RECOVERED; end_episode(in.now, c.cool_ok, FIX_RESTART); }
                else { Out& e = emit(EV_GAVE_UP); e = r; e.ev = EV_GAVE_UP; e.why = WHY_STILL_SLOW; end_episode(in.now, c.cool_fail, FIX_NOTHING); }
            } break;
            default: break;
        }
        return n_out;
    }

private:
    Out* o = nullptr; int n_out = 0, o_max = 0; Out spare;
    uint32_t hist[2][240] = {}; int hist_total[2] = { 0, 0 }, hist_since[2] = { 0, 0 };
    Out& emit(Ev e) {
        if (n_out < o_max && o != nullptr) { o[n_out] = Out(); o[n_out].ev = e; return o[n_out++]; }
        spare = Out(); spare.ev = e; return spare;
    }
    static int first_bit(unsigned m) { for (int i = 0; i < 32; ++i) if (m & (1u << i)) return i; return -1; }
    void reset_run() { run = miss = 0; run_span = run_end = run_frame = 0; }
    /// B2: the median of the measured spans outside episodes (all of them: a setup that is always slow learns its own speed).
    void learn(int m, double span) {
        if (!(span >= 0)) return;
        int b = static_cast<int>(span / c.bin_ms); if (b >= c.bins) b = c.bins - 1; if (b > 239) b = 239;
        hist[m][b]++; hist_total[m]++;
        if (hist_total[m] >= c.base_halve) { hist_total[m] = 0; for (int i = 0; i < 240; ++i) { hist[m][i] >>= 1; hist_total[m] += (int)hist[m][i]; } }
        if (++hist_since[m] < 16 && learnt[m]) return;   // the median every 16 frames once learnt
        hist_since[m] = 0;
        if (hist_total[m] < c.base_min) { learnt[m] = false; return; }
        const int half = (hist_total[m] + 1) / 2; int acc = 0;
        for (int i = 0; i < 240; ++i) { acc += (int)hist[m][i]; if (acc >= half) { base[m] = (i + 0.5) * c.bin_ms; break; } }
        learnt[m] = true;
    }
    void watch(const In& in) {
        if (!in.running) { reset_run(); return; }
        if (in.excluded) { reset_run(); if (in.now + c.quiet_excl > quiet_until) quiet_until = in.now + c.quiet_excl; return; }
        if (in.frame_ms > c.gap_stall) { reset_run(); quiet_until = in.now + c.quiet_stall; return; }
        if (in.now < quiet_until || !in.span_ok) return;
        const int m = in.screen ? 1 : 0;
        if (is_slow(in)) { if (run == 0) run_t0 = in.now; run++; run_span += in.span; run_end += in.end; run_frame += in.frame_ms; }
        else if (run > 0 && ++miss > c.miss_max) reset_run();
        if (run < c.run_frames) return;
        last_span = run_span / run; last_end = run_end / run; last_frame = run_frame / run; last_base = base[m]; last_screen = in.screen; last_frames = run;
        last_dur = in.now - run_t0;
        reset_run();
        if (!learnt[m]) {   // B2: no "it changed" without a learnt baseline: logged (rate-limited), nothing switched off
            if (in.now - unlearnt_at >= c.unlearnt_gap) { unlearnt_at = in.now; Out& e = emit(EV_LOG_ONLY); e.span_avg = last_span; e.end_avg = last_end; e.n = last_frames; e.why = WHY_UNLEARNT; }
            return;
        }
        if (episodes >= c.episodes_max) {
            if (in.now - spent_at >= c.spent_gap) { spent_at = in.now; Out& e = emit(EV_SPENT); e.span_avg = last_span; e.end_avg = last_end; }
            return;
        }
        episodes++; t_onset = in.now; item = -1; st = ST_CENSUS; t_state = in.now; last_fix = FIX_NONE; last_ended = false; last_fix_item = -1;
        measured_steps = 0; repeated = false;
        Out& e = emit(EV_ONSET); e.span_avg = last_span; e.end_avg = last_end; e.n = last_frames;
    }
    void next_item(const In& in) {
        repeated = false;
        for (int i = item + 1; i < IT_N; ++i) {
            if (!(in.avail & (1u << i)) || (latched & (1u << i))) continue;
            item = i; off |= 1u << i; st = ST_ITEM; w.start(in.now); emit(EV_ITEM_OFF).item = i;
            return;
        }
        item = IT_N;
        int why = WHY_NONE;
        if (!in.restart_ok) why = WHY_NO_RESTART;
        else if (measured_steps < c.restart_min_steps) why = WHY_FEW_STEPS;
        else if (restarts >= c.restarts_max) why = WHY_RESTARTS_MAX;
        else if (in.now - last_restart < c.restart_gap) why = WHY_RESTART_GAP;
        if (why != WHY_NONE) { emit(EV_GAVE_UP).why = why; end_episode(in.now, c.cool_fail, FIX_NOTHING); return; }
        st = ST_PRE_RESTART; w.start(in.now); emit(EV_PRE_CHECK);
    }
    /// Samples a frame into the window (after `settle`, frames with a span and not excluded); true when it is complete.
    bool sample_done(const In& in, double settle, double measure) {
        const double dt = in.now - w.t0;
        if (dt >= settle && !in.excluded && in.span_ok && in.running) {
            w.n++; if (is_fast(in)) w.fast++; if (is_slow(in)) w.slow++; w.span_sum += in.span; w.end_sum += in.end;
        }
        return (dt >= settle + measure && w.n >= c.min_samples) || dt >= c.step_wall;
    }
    void fill(Out& r, int it) const {
        r.item = it; r.n = w.n; r.fast = w.fast; r.slow = w.slow;
        r.helped = w.n >= c.min_samples && w.fast >= c.fast_frac * w.n;
        r.span_avg = w.n ? w.span_sum / w.n : 0; r.end_avg = w.n ? w.end_sum / w.n : 0;
    }
    void end_episode(double now, double cool, int fix) {
        last_fix = fix; last_ended = true; last_dur = now - t_onset;
        st = ST_COOL; cool_until = now + cool; item = -1; reset_run(); repeated = false;
        off = latched;   // anything the episode switched off comes back on (an item latched just now stays off)
    }
};

}  // namespace slow
