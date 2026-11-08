// SPDX-License-Identifier: MIT
// ffxvr_eyejump.h -- eye jumps (roadmap 4.1, 5 Nov 2026): the blink when the first-person eye moves to another place in one
// step, and the battle heading that faces the enemies. Pure logic: no Windows, no D3D, no logging, no allocation. The native
// side (ffxvr_native.cpp, build_override_view's first-person branch) calls ej::step once per frame on the primary eye's
// advance call with the eye base (fp_eye_base), and ej::face_yaw when managed asks to face a point (ffxvr_eye_event 1).
// A unit test drives all of it with synthetic values and a replay of a motion log.
//
// Conventions (the native file's own): render space, yaw = atan2(-f.x, -f.z), forward = (-sin yaw, 0, -cos yaw); the view's
// gaze yaw is g_fp_yaw + the head's yaw against the recentre reference (Rn = Mq^T Rb).
// Units are game units (about 9 cm); frames are native frames (g_frames, one per Present). Running moves the eye about 1
// unit a frame at 60 fps (2 at 30), the chocobo about 2.5; a jump is a move of more than Cfg::units (default 8) in a frame.
#pragma once
// fix-eye (7 Nov 2026), revision 2: a grab no longer mutes a jump; 3: a big jump after a blink has ended blinks again; 4: the
// event hold (EvHold); 5: a no-control episode's summary (Episode); 6 (review): min_gap 6, the hold's no_start (a cut at the
// edge), EV_PULL / EV_BLOCKED; 7 (review 2): min_start 20 (the shared scratch test keys its expectations on this).
#define FFXVR_EJ_REV 7
#include <cmath>
#include <cstdint>

namespace ej {

struct Cfg {
    int   blink  = 6;     // ini eye_jump_blink: black frames for a jump (0 = no blink; a jump still resets the head smoothing)
    float units  = 8.f;   // ini eye_jump_units: a jump is more than this many units in one frame
    int   window = 30;    // at most one blink start per this many frames (a scripted glide in 8-20 unit steps would strobe)
    int   ext_max = 2;    // a jump inside the window extends a running blink by at most this many frames in all
    // fix-eye (finding 6, s1 16:41:46: an 87-unit move back 22 frames after a blink, held back by the window, a 7.6 m teleport in the
    // clear): inside the window, once the blink is over, a jump of more than `big` units blinks again: a fresh blink when at least
    // min_gap frames of picture were shown since the last black frame, the dark period restarted when none was (a jump right at
    // the blink's end), and held back when fewer than min_gap were. Jumps up to `big` stay held back in the window: a dash or a
    // scripted glide in 8-20 unit steps would strobe. Review S5: min_gap 6 (was 2): a big jump every 8 frames gave 6 black / 2
    // picture, a 7.5 Hz strobe; now blinks are at least 6 picture frames apart, and a big jump 1-5 picture frames after a blink is
    // held back (a teleport seen once, right after a dark period, rather than a flicker). A run of big jumps stays one dark period.
    float big = 20.f;     // units
    int   min_gap = 6;    // frames of picture between two blinks
    // Review R2-S3: min_gap alone still allowed 5 dark periods a second (a jump every 12 frames); flash guidelines warn from 3. A
    // fresh blink inside the window also needs min_start frames since the last blink began; a big jump before that is held back
    // (seen once) as under min_gap. A restart of the dark period (no picture shown yet) is not a new onset.
    int   min_start = 20; // frames between two blink onsets (60 fps: 3 a second)
};

// This frame's context (native globals at the call).
struct Ctx {
    uint32_t frame = 0;       // g_frames
    int      mode = 0;        // g_fp_mode: 1 first person, 3 third person blink; 0 / 2 (seat, diorama) never
    bool     dragging = false;// g_gest_active (fix-eye: no longer mutes a jump, see step)
    bool     latched = false; // g_fp_yaw_latched was set before this call (false on the latch frame itself)
    uint32_t latch_seq = 0;   // bumped by the native side at every heading latch: a change since the last step = a latch frame
    int      blink_left = 0;  // g_blink_frames now (a running blink can be extended)
};

enum Kind : int { K_NONE = 0, K_START, K_EXTEND, K_HELD, K_OFF };          // K_HELD: inside the window, nothing to extend
enum Why  : int { W_MOVE = 0, W_FIRST, W_GAP, W_LATCH, W_NAN, W_MODE, W_DRAG };   // why a frame was (not) compared; W_MOVE = compared
static const char* const k_kind[] = { "none", "blink", "blink extended", "held back (one blink per window)", "no blink (eye_jump_blink=0)" };

struct Result {
    int   kind = K_NONE;
    int   why = W_FIRST;
    int   frames = 0;     // the blink the caller asks for: g_blink_frames = max(g_blink_frames, frames) (0: none)
    float dist = 0.f;     // the move this frame (0 when not compared)
    float prev_dist = 0.f;// the previous frame's move (0 when that frame was not compared): a run of big moves is a dash, one
                          // alone a teleport (S5: the followed character's battle actions, measured from the owner's eye: lines)
    bool  jump() const { return kind != K_NONE; }
};

struct State {
    float    prev[3] = {};
    bool     valid = false;      // prev holds a usable eye base
    uint32_t prev_frame = 0;
    uint32_t latch_seq = 0;
    bool     started = false;    // a blink has started this session (start_frame is meaningful)
    uint32_t start_frame = 0;
    int      ext = 0;            // frames added to the blink that started at start_frame
    float    last_d = 0.f;       // the last frame's move if it was compared, else 0 (Result::prev_dist)
    bool     black_valid = false;// fix-eye: black_last is meaningful
    uint32_t black_last = 0;     // fix-eye: the last frame whose Present is (or was planned) black, as far as step has seen
    uint32_t n_jumps = 0, n_start = 0, n_ext = 0, n_held = 0;   // since the last clear (the view: line)
};

inline bool finite3(const float p[3]) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); }

/// One frame: P = this frame's eye base. Blink when the eye base moved more than cfg.units since the last frame, and the
/// last frame was also a first-person 3D frame (at most 2 frames ago), the heading was latched before (not the latch
/// frame, nor a re-latch since the last step), the mode is 1 or 3, and P is finite.
/// fix-eye (finding 4): a grab no longer mutes the blink. The grips never grab in first person (gesture_tick grabs in the seat,
/// the diorama and third person only), and in third person the grab moves the camera's offset (g_gest_move), applied after the
/// eye base this tests: a grab cannot move P, so a jump during one is a re-pick or a scripted move like any other. (The s1 hops
/// without a blink, 16:43:21-55, were in the diorama, which never blinks: W_MODE.)
inline void note_black(State& s, uint32_t f) {   // fix-eye: Present f is black (a blink set at frame g with n frames: g .. g + n - 1)
    if (!s.black_valid || static_cast<int32_t>(f - s.black_last) > 0) { s.black_last = f; s.black_valid = true; }
}
inline Result step(State& s, const float P[3], const Ctx& c, const Cfg& cfg) {
    Result r; r.prev_dist = s.last_d; s.last_d = 0.f;
    if (c.blink_left > 0) note_black(s, c.frame + static_cast<uint32_t>(c.blink_left) - 1u);   // any source's blink (cut, eye_disp, a turn)
    if (!finite3(P)) { s.valid = false; r.why = W_NAN; return r; }
    const bool had = s.valid;
    const uint32_t gap = c.frame - s.prev_frame;
    const bool relatched = s.latch_seq != c.latch_seq;
    float d = 0.f;
    if (had) { const float dx = P[0] - s.prev[0], dy = P[1] - s.prev[1], dz = P[2] - s.prev[2]; d = std::sqrt(dx * dx + dy * dy + dz * dz); }
    s.prev[0] = P[0]; s.prev[1] = P[1]; s.prev[2] = P[2]; s.valid = true; s.prev_frame = c.frame; s.latch_seq = c.latch_seq;
    if (!had)                         { r.why = W_FIRST; return r; }
    if (gap > 2)                      { r.why = W_GAP;   return r; }
    if (!c.latched || relatched)      { r.why = W_LATCH; return r; }
    if (c.mode != 1 && c.mode != 3)   { r.why = W_MODE;  return r; }
    r.why = W_MOVE; r.dist = d; s.last_d = d;
    if (!(d > cfg.units)) return r;
    s.n_jumps++;
    if (cfg.blink <= 0) { r.kind = K_OFF; return r; }
    const bool in_window = s.started && c.frame - s.start_frame < static_cast<uint32_t>(cfg.window > 0 ? cfg.window : 0);
    if (!in_window) {
        s.started = true; s.start_frame = c.frame; s.ext = 0; s.n_start++;
        r.kind = K_START; r.frames = cfg.blink; note_black(s, c.frame + static_cast<uint32_t>(r.frames) - 1u);
        return r;
    }
    const int room = cfg.ext_max - s.ext;
    if (c.blink_left > 0 && room > 0) {
        s.ext += room; s.n_ext++;
        r.kind = K_EXTEND; r.frames = c.blink_left + room; note_black(s, c.frame + static_cast<uint32_t>(r.frames) - 1u);
        return r;
    }
    if (c.blink_left <= 0 && d > cfg.big) {   // fix-eye (finding 6): the blink is over and this is a big jump
        // frames of picture shown since the last black frame (a blink cut short, e.g. by the screen, counts as none)
        const int32_t pic = s.black_valid ? static_cast<int32_t>(c.frame - s.black_last) - 1 : 0x7FFFFFFF;
        // review R2-S3: a fresh blink also needs min_start frames since the last one began (3 a second at most at 60 fps)
        const bool fresh_ok = pic >= cfg.min_gap && static_cast<int32_t>(c.frame - s.start_frame) >= cfg.min_start;
        if (pic <= 0 || fresh_ok) {
            s.started = true; s.start_frame = c.frame; s.ext = 0;
            if (pic <= 0) { s.n_ext++; r.kind = K_EXTEND; } else { s.n_start++; r.kind = K_START; }   // none shown: the dark period goes on
            r.frames = cfg.blink; note_black(s, c.frame + static_cast<uint32_t>(r.frames) - 1u);
            return r;
        }
    }
    s.n_held++; r.kind = K_HELD;
    return r;
}

inline float wrap_pi(float a) {   // to [-pi, pi)
    const float tp = 6.2831853f;
    if (!std::isfinite(a)) return 0.f;
    a = std::fmod(a + 3.14159265f, tp);
    if (a < 0.f) a += tp;
    return a - 3.14159265f;
}

struct Face {
    bool  turn = false;   // the heading changes (false: under the dead band, no target, or not a facing mode)
    float yaw = 0.f;      // the new g_fp_yaw (cur + delta: stays continuous with the unwrapped heading)
    float target = 0.f;   // the target's yaw from pos (radians, wrapped)
    float delta = 0.f;    // the change, wrapped to [-pi, pi)
};

/// The heading that puts tgt ahead. mode 1 ("body forward"): straight ahead of the recentred forward (head_rel ignored);
/// mode 2 ("where you look"): ahead of the gaze, g_fp_yaw = yaw_t - head_rel. No change under dead_deg degrees, nor for a
/// target within half a unit horizontally, nor for non-finite input.
inline Face face_yaw(const float pos[3], const float tgt[3], float head_rel, int mode, float cur_yaw, float dead_deg = 15.f) {
    Face f; f.yaw = cur_yaw;
    if (mode != 1 && mode != 2) return f;
    if (!finite3(pos) || !finite3(tgt) || !std::isfinite(head_rel) || !std::isfinite(cur_yaw)) return f;
    const float fx = tgt[0] - pos[0], fz = tgt[2] - pos[2];
    if (fx * fx + fz * fz < 0.25f) return f;
    f.target = std::atan2(-fx, -fz);
    const float want = mode == 2 ? f.target - head_rel : f.target;
    f.delta = wrap_pi(want - cur_yaw);
    if (std::fabs(f.delta) < dead_deg * 0.0174533f) return f;
    f.turn = true; f.yaw = cur_yaw + f.delta;
    return f;
}

/// The field heading kept over a battle (only with battle_face on: it exists to undo the face's turn): saved at the battle's
/// start with the game camera's yaw, taken at the first first-person frame after its end in the same view mode (a mode switch
/// back from battle_view has re-latched from the game's camera by then).
struct Hold {
    bool     valid = false;    // a heading was saved
    bool     pending = false;  // the battle ended: take at the next frame in `mode`
    float    yaw = 0.f;        // g_fp_yaw at the start
    float    cam = 0.f;        // the game camera's yaw (g_yaw_cam) at the start
    int      mode = 0;
    uint32_t frame = 0;        // the battle's end
    int      miss = 0;         // R2-SF1: consecutive same-mode first-person frames under another camera since the arm
    bool     fp_seen = false;  // last_fp is meaningful
    uint32_t last_fp = 0;      // the last first-person frame hold_take saw (every call, armed or not)
};
/// The battle starts (battle_face on; off clears the Hold). N1: an armed restore not yet taken (battle B right after A with no first-person
/// field frame between) keeps A's field heading: B's start heading is A's faced one, not the field's.
inline void hold_save(Hold& h, bool latched, int mode, float yaw, float cam, uint32_t frame, uint32_t max_age) {
    if (h.pending && frame - h.frame <= max_age) { h.pending = false; return; }
    h.valid = latched && mode != 0 && std::isfinite(yaw) && std::isfinite(cam);
    h.pending = false; h.yaw = yaw; h.cam = cam; h.mode = mode;
}
inline void hold_arm(Hold& h, uint32_t frame) { if (h.valid) { h.pending = true; h.frame = frame; h.miss = 0; } }
enum Take : int { H_NONE = 0, H_RESTORE, H_RELATCH, H_DROP };
static const char* const k_take[] = { "none", "restored", "re-latched from the game camera", "dropped" };
/// At EVERY first-person advance frame (cam = the game camera's yaw now). H_RESTORE (*yaw = the saved heading): same mode and
/// the same field camera (within tol rad of the saved yaw). Another camera in the same mode waits (R2-SF1: the battle's end
/// can still be drawn with the battle camera for a few frames after battle_state 0) and gives H_RELATCH (the caller latches
/// from the game camera) only after miss_max such frames in a row, or at once after a first-person gap of 3+ Presents (a load
/// has passed: this is the new scene, e.g. a battle that ends into a cutscene). Given up after max_age frames: H_RELATCH in
/// the saved mode (the battle's faced heading must not stay), H_DROP in another (its own switch re-latched already).
/// H_NONE: nothing armed, another mode within max_age, or a mismatch still waiting.
inline int hold_take(Hold& h, int mode, uint32_t frame, float cam, uint32_t max_age, float tol, float* yaw, int miss_max = 20) {
    const bool gap = !h.fp_seen || frame - h.last_fp >= 3;
    h.fp_seen = true; h.last_fp = frame;
    if (!h.pending) return H_NONE;
    if (frame - h.frame > max_age) { h.pending = false; h.valid = false; return mode == h.mode ? H_RELATCH : H_DROP; }
    if (mode != h.mode) return H_NONE;
    if (!std::isfinite(cam) || std::fabs(wrap_pi(cam - h.cam)) > tol) {
        if (!gap && ++h.miss < miss_max) return H_NONE;
        h.pending = false; h.valid = false;
        return H_RELATCH;
    }
    h.pending = false; h.valid = false;
    *yaw = h.yaw;
    return H_RESTORE;
}

/// B1: the face request (ffxvr_eye_event 1) waits for first-person frames: it is aged in first-person advance frames, not in
/// Presents, so a request made while nothing is drawn in 3D (the load, the shards) waits for the first battle frame.
struct FaceReq {
    bool     on = false;
    float    t[3] = {};
    uint32_t frame = 0;     // g_frames at the request
    int      fp = 0;        // first-person advance frames seen since (in a mode that does not face)
};
enum FaceStep : int { F_NONE = 0, F_WAIT, F_APPLY, F_DROP };
inline void face_req_set(FaceReq& q, const float t[3], uint32_t frame) { q.on = true; q.t[0] = t[0]; q.t[1] = t[1]; q.t[2] = t[2]; q.frame = frame; q.fp = 0; }
/// At each first-person advance frame: F_APPLY in a facing view (mode 1 / 3), F_WAIT in another (the diorama), F_DROP after
/// max_fp such frames, after max_presents Presents, or with battle_face off.
inline int face_req_tick(FaceReq& q, uint32_t frame, int mode, int face, int max_fp = 30, uint32_t max_presents = 600) {
    if (!q.on) return F_NONE;
    if (face == 0 || frame - q.frame > max_presents) { q.on = false; return F_DROP; }
    if (mode == 1 || mode == 3) { q.on = false; return F_APPLY; }
    if (++q.fp >= max_fp) { q.on = false; return F_DROP; }
    return F_WAIT;
}

/// B1: Btl.battle_state for managed's face request (eye_jump.cs ej_face_tick mirrors this: keep the two equal). The native
/// battle transition's own reading (bt_running_state, BT-B-1): 4..0x15 and 0x22 the load chain (the 5 Nov log: 4 at frame
/// 2396, 6, 7, 0x22 for 57 frames, 0x0C..0x14, 0x16 at 2512, the intro drawn at 2513), 0x16 the intro (drawn in 3D), 1 / 2
/// the battle, 0x17..0x21 its end. BS_LOAD: wait (no countdown, no send); BS_LIVE: count down and send; BS_OTHER: count down.
enum BattleState : int { BS_OTHER = 0, BS_LOAD, BS_LIVE };
inline int face_bst(int bst) {
    if ((bst >= 4 && bst <= 0x15) || bst == 0x22) return BS_LOAD;
    if (bst == 0x16 || bst == 1 || bst == 2) return BS_LIVE;
    return BS_OTHER;
}

/// S4: one blink start per window for the seat view's cut blinks (cut_blink), as for the eye jumps.
struct Limit { bool started = false; uint32_t start = 0; uint32_t n_held = 0; };
inline bool limit_start(Limit& l, uint32_t frame, int window) {
    if (l.started && frame - l.start < static_cast<uint32_t>(window > 0 ? window : 0)) { l.n_held++; return false; }
    l.started = true; l.start = frame;
    return true;
}

// ---- fix-eye: the event hold (the sphere grab, research/sphere-grab-eye.md) ------------------------------------------------
// In the Djose cloister (7 Nov) the game moved Tidus's root when player control went off for an interaction (9.1 and 10.5 units
// in one frame on dj/djyt0600; a scripted run of a second on dj/djyt0000), and the first-person eye, which stands on the root,
// went with it into the wall. While control is off in first person on foot, the eye's horizontal position is held where it was
// when control went off, on a leash: when the game moves the root further than `leash`, the eye trails it at that distance. A
// single root step over 2 x leash is a scene staging: the hold starts again at the new root (the eye-jump blink covers it). The
// height keeps the head rule over the root (stairs, a kneel). When control returns the eye glides back over glide_s if it is
// under leash / 2 off, else it snaps with a blink. A camera cut or max_s seconds end the hold the same way; a map change, a load
// or a re-pick (managed) and the setting off end it with a snap. One hold per no-control episode: it starts on the edge only.
// Pure: the native glue (build_override_view's primary advance call) passes the eye base and the frame's inputs.
struct EvCfg {
    float leash  = 12.f;   // ini fp_event_leash: units (12 = about 1.05 m)
    float glide_s = 0.3f;  // the glide back, seconds
    float max_s  = 45.f;   // the hold ends after this long (a cutscene without a cut; 0 = no cap)
    float reset() const { return 2.f * leash; }       // a single root step over this restarts the hold at the root
    float glide_max() const { return 0.5f * leash; }  // an offset under this glides back, else a blink and a snap
};
// EV_GLIDE / EV_SNAP: the hold ended this frame. EV_PULL (review N2): a single root step over the leash pulled the eye along
// (a 12-24 unit staging under the restart; the caller blinks). EV_BLOCKED (review S1): control went off with a camera cut on that
// frame or just before (a cutscene's start): no hold this episode.
enum EvEvent : int { EV_NONE = 0, EV_START, EV_RESET, EV_GLIDE, EV_SNAP, EV_PULL, EV_BLOCKED };
enum EvWhy : int { EVW_NONE = 0, EVW_CONTROL, EVW_RELEASE, EVW_CUT, EVW_CAP, EVW_OFF };
static const char* const k_ev_why[] = { "-", "control back", "a map change, a load, a re-pick or a gap in first person", "a camera cut", "the time cap", "fp_event_hold off, the eye on the drawn head, or first person left" };
struct EvHold {
    bool     on = false;
    bool     want_prev = false;   // the last step's want (the start is on its rising edge)
    float    H[2] = {};           // the held horizontal eye position (render x, z)
    float    prev[2] = {};        // the last step's eye base (x, z)
    bool     prev_ok = false;
    float    t = 0.f;             // seconds held
    uint32_t start_frame = 0;
    bool     glide = false;       // gliding back after the hold
    float    G[2] = {};           // the glide's offset from the eye base (decays to 0)
    float    glide_left = 0.f;    // seconds
    // the hold's statistics (the start / end lines)
    float    start[2] = {};       // the eye base at the start
    float    off_max = 0.f;       // the largest eye offset from the base while held
    float    step_max = 0.f;      // the largest single-frame base step while held
    int      resets = 0;
    float    end_off = 0.f;       // the offset when it ended
    int      why = EVW_NONE;      // why it ended
};
inline float hlen(float x, float z) { return std::sqrt(x * x + z * z); }
/// One advance frame. P = the eye base (x and z replaced while held or gliding). want = an eligible frame with control off
/// (managed); enabled = fp_event_hold (the caller also clears it while the eye is on the drawn head, review S3, and outside first
/// person); release = managed's map change / load / re-pick (or a gap in the steps); cut = a camera cut since the last step;
/// no_start = a camera cut on this frame or within the 2 before (review S1: the cut that starts a cutscene can come a frame before
/// the control flag, or in the same game frame, and the hold must not swallow it). dt = seconds since the last frame.
inline int ev_step(EvHold& h, float P[3], bool want, bool enabled, bool release, bool cut, bool no_start, float dt, const EvCfg& c, uint32_t frame) {
    const bool rising = want && !h.want_prev;
    h.want_prev = want;
    if (!finite3(P) || !std::isfinite(dt)) { h.on = false; h.glide = false; h.prev_ok = false; return EV_NONE; }
    const float bx = P[0], bz = P[2];
    const float step = h.prev_ok ? hlen(bx - h.prev[0], bz - h.prev[1]) : 0.f;
    h.prev[0] = bx; h.prev[1] = bz; h.prev_ok = true;
    const float L = c.leash > 0.5f ? c.leash : 0.5f;
    int ev = EV_NONE;
    if (h.on) {
        const int why = !enabled ? EVW_OFF : !want ? EVW_CONTROL : release ? EVW_RELEASE : cut ? EVW_CUT : (c.max_s > 0.f && h.t >= c.max_s) ? EVW_CAP : EVW_NONE;
        if (why == EVW_NONE) {
            if (step > c.reset()) { h.H[0] = bx; h.H[1] = bz; h.resets++; ev = EV_RESET; }   // a staging: the usual blink (the jump)
            else {
                const float vx = bx - h.H[0], vz = bz - h.H[1], d = hlen(vx, vz), hx = h.H[0], hz = h.H[1];
                if (d > L) { const float k = L / d; h.H[0] = bx - vx * k; h.H[1] = bz - vz * k; }   // the leash: trail at L
                if (step > L && hlen(h.H[0] - hx, h.H[1] - hz) > 0.5f) ev = EV_PULL;   // review N2: a staging step the leash passes on
            }
            if (step > h.step_max) h.step_max = step;
            h.t += dt > 0.f ? dt : 0.f;
            const float off = hlen(h.H[0] - bx, h.H[1] - bz);
            if (off > h.off_max) h.off_max = off;
            P[0] = h.H[0]; P[2] = h.H[1];
            return ev;
        }
        h.on = false; h.why = why;
        h.end_off = hlen(h.H[0] - bx, h.H[1] - bz);
        // a glide back for control, a cut or the cap; a release (another map, a load, a re-pick) or the setting off: snap
        if (h.end_off < c.glide_max() && c.glide_s > 0.f && why != EVW_RELEASE && why != EVW_OFF) { h.glide = true; h.G[0] = h.H[0] - bx; h.G[1] = h.H[1] - bz; h.glide_left = c.glide_s; ev = EV_GLIDE; }
        else { h.glide = false; ev = EV_SNAP; }
    }
    else if (rising && enabled && !release && no_start) ev = EV_BLOCKED;   // (a glide under way goes on below)
    else if (rising && enabled && !release) {
        // from where the eye is now (a glide still under way included)
        const float ox = h.glide ? h.G[0] : 0.f, oz = h.glide ? h.G[1] : 0.f;
        h.on = true; h.glide = false; h.H[0] = bx + ox; h.H[1] = bz + oz; h.t = 0.f; h.start_frame = frame;
        h.start[0] = bx; h.start[1] = bz; h.off_max = hlen(ox, oz); h.step_max = 0.f; h.resets = 0; h.end_off = 0.f; h.why = EVW_NONE;
        P[0] = h.H[0]; P[2] = h.H[1];
        return EV_START;
    }
    if (h.glide) {
        const float k = dt > 0.f && h.glide_left > 0.f ? (dt < h.glide_left ? dt / h.glide_left : 1.f) : 0.f;   // linear: what is left over the time left
        h.G[0] -= h.G[0] * k; h.G[1] -= h.G[1] * k; h.glide_left -= dt > 0.f ? dt : 0.f;
        if (h.glide_left <= 0.f || hlen(h.G[0], h.G[1]) < 0.01f) h.glide = false;
        else { P[0] = bx + h.G[0]; P[2] = bz + h.G[1]; }
    }
    return ev;
}
/// The eye's horizontal offset from the base this frame applies (held or gliding), for the other callers of the frame.
inline bool ev_active(const EvHold& h) { return h.on || h.glide; }

// ---- fix-eye: a no-control episode's summary (the automatic capture, research/sphere-grab-eye.md section 4) -----------------
// From the frame control goes off in first person on foot to the frame it is back (or first person is left): how far and which
// way the game moved the root (against the view's heading at the start), its largest single-frame step, how far the eye got from
// where it was, the black frames and the camera cuts. Whether or not the hold is on: with it off this is what the eye went through.
struct Episode {
    bool     on = false;
    uint32_t start_frame = 0, frames = 0;
    float    root0[2] = {}, eye0[2] = {}, prev[2] = {};   // horizontal (render x, z): the root and the eye at the start; the last root
    float    fwd[2] = {};                                  // the view's horizontal forward at the start
    float    step_max = 0.f; uint32_t step_at = 0;         // the largest single-frame root step, frames after the start
    float    eye_far = 0.f;                                // the eye's largest distance from where it was at the start
    uint32_t black = 0, cuts = 0;
    float    disp = 0.f, along = 0.f, across = 0.f;        // at the end: the root's displacement, its parts along / across fwd
    int      dir = 0;                                      // 0 none (under half a unit), 1 forward, 2 sideways, 3 back
};
enum EpEvent : int { EP_NONE = 0, EP_START, EP_END };
static const char* const k_ep_dir[] = { "nowhere (under half a unit)", "forward", "sideways", "back" };
/// One advance frame: want = an eligible frame with control off (ev_step's), root = the eye's root (g_fp_pos), eye = the eye base
/// after the hold, yaw = the view's yaw (forward (-sin, -cos)), black = this frame's Present is black, cut = a camera cut.
inline int ep_step(Episode& e, bool want, const float root[3], const float eye[3], float yaw, bool black, bool cut, uint32_t frame) {
    if (!e.on) {
        if (!want || !finite3(root) || !finite3(eye)) return EP_NONE;
        e = Episode{};
        e.on = true; e.start_frame = frame;
        e.root0[0] = e.prev[0] = root[0]; e.root0[1] = e.prev[1] = root[2]; e.eye0[0] = eye[0]; e.eye0[1] = eye[2];
        if (std::isfinite(yaw)) { e.fwd[0] = -std::sin(yaw); e.fwd[1] = -std::cos(yaw); }
        return EP_START;
    }
    if (!want) {
        const float dx = e.prev[0] - e.root0[0], dz = e.prev[1] - e.root0[1];
        e.disp = hlen(dx, dz); e.along = dx * e.fwd[0] + dz * e.fwd[1]; e.across = dx * e.fwd[1] - dz * e.fwd[0];
        const float c = e.disp > 1e-6f ? e.along / e.disp : 0.f;
        e.dir = e.disp < 0.5f || (e.fwd[0] == 0.f && e.fwd[1] == 0.f) ? 0 : c > 0.5f ? 1 : c < -0.5f ? 3 : 2;
        e.on = false;
        return EP_END;
    }
    e.frames++;
    if (black) e.black++;
    if (cut) e.cuts++;
    if (finite3(root)) {
        const float st = hlen(root[0] - e.prev[0], root[2] - e.prev[1]);
        if (st > e.step_max) { e.step_max = st; e.step_at = frame - e.start_frame; }
        e.prev[0] = root[0]; e.prev[1] = root[2];
    }
    if (finite3(eye)) { const float d = hlen(eye[0] - e.eye0[0], eye[2] - e.eye0[1]); if (d > e.eye_far) e.eye_far = d; }
    return EP_NONE;
}

}  // namespace ej
