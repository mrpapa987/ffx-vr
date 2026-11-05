// SPDX-License-Identifier: MIT
// ffxvr_pad.h -- the Quest controllers as a gamepad, pure logic (Quest-only controls, 5 Nov 2026; design-notes "Quest
// controllers only"). No Windows types beyond a local POD copy of XINPUT_GAMEPAD, no logging, no allocation. Used by
// ffxvr_native.cpp in three places: vrpad_apply (the game's XInputGetState import, the input thread), the ImGui feed
// (Fahrenheit's ImGui XInput DLL, the render thread) and gesture_tick (the quick-squeeze grips, the render thread). A unit
// test drives all of it with synthetic values.
//
//   compose / merge   the VR pad's XInput state, ORed / max-merged into the real pad (the mapping vrpad_apply always had)
//   imgui_*           which buttons the ImGui feed passes (no Start / Back by default), the buttons held when it opens
//   walk_why          the walk gate: the first-person stick remap only while the player can walk (raw in battle and menus)
//   grip_step         quick squeeze of a grip = LB / RB, a held grip stays the grab / the chord modifier
//   ShQueue           the quick squeezes waiting for the input thread's press slot
#pragma once
#include <cmath>
#include <cstdint>

#include "ffxvr_gates.h"   // the game bits (gates::GB_*), the same ones the tap gate reads

namespace pad {

// XInput's button bits (Xinput.h values).
enum : uint16_t {
    DPAD_UP = 0x0001, DPAD_DOWN = 0x0002, DPAD_LEFT = 0x0004, DPAD_RIGHT = 0x0008, START = 0x0010, BACK = 0x0020,
    LTHUMB = 0x0040, RTHUMB = 0x0080, LSHOULDER = 0x0100, RSHOULDER = 0x0200, A = 0x1000, B = 0x2000, X = 0x4000, Y = 0x8000,
};

/// One Touch controller as the pad sees it (the XR layer's XrHand, the fields the mapping uses).
struct Hand { bool primary = false, secondary = false, menu = false, stick_click = false; float trigger = 0.f, squeeze = 0.f; float stick[2] = { 0.f, 0.f }; };
struct In { Hand h[2]; };   // 0 left, 1 right

/// A POD copy of XINPUT_GAMEPAD (the same layout; the native side merges into the real struct through the template).
struct Gamepad { uint16_t wButtons; uint8_t bLeftTrigger, bRightTrigger; int16_t sThumbLX, sThumbLY, sThumbRX, sThumbRY; };

/// What the Quest controllers contribute: buttons, triggers as bytes, the sticks as the raw floats (merge compares the
/// magnitudes in float, exactly as vrpad_apply did before this header).
struct Out { uint16_t buttons = 0; uint8_t lt = 0, rt = 0; float ls[2] = { 0.f, 0.f }, rs[2] = { 0.f, 0.f }; };

inline bool grip_down(float squeeze) { return squeeze > 0.5f; }
/// 0..1 -> 0..255, exactly as vrpad_apply's `(BYTE)(t * 255.f)` did on x86 (MSVC truncates through a 32-bit int, so an
/// out-of-range value wraps: -0.1 -> 231, 1.01 -> 1; NaN and values beyond int32 give 0x80000000, low byte 0). Written
/// without the undefined float-to-byte conversion. Not clamped: the old code did not clamp the triggers (only the sticks).
inline uint8_t trigger_byte(float t) {
    const float f = t * 255.f;
    if (!(f > -2147483648.f && f < 2147483648.f)) return 0;
    return static_cast<uint8_t>(static_cast<uint32_t>(static_cast<int32_t>(f)) & 0xFFu);
}
/// -1..1 -> a stick short (vrpad_apply's stick_s).
inline int16_t stick_s(float v) { v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v); return static_cast<int16_t>(v * 32767.f); }

enum { C_VD_MASK = 1 };   // compose flags: both stick clicks (Virtual Desktop's overlay) send neither Back nor R3
inline bool vd_chord(const In& in) { return in.h[0].stick_click && in.h[1].stick_click; }

/// The mapping (vrpad_apply's, unchanged): R A/B = A/B, L X/Y = X/Y, grips = LB/RB (only the ones in grip_pass: 1 left,
/// 2 right), left menu = Start, left stick click = Back, right stick click = R3, triggers = LT/RT, sticks = sticks.
inline Out compose(const In& in, int grip_pass, int flags) {
    const Hand& L = in.h[0]; const Hand& R = in.h[1];
    Out o; uint16_t b = 0;
    if (R.primary) b |= A;
    if (R.secondary) b |= B;
    if (L.primary) b |= X;
    if (L.secondary) b |= Y;
    if (grip_pass & 1) b |= LSHOULDER;
    if (grip_pass & 2) b |= RSHOULDER;
    if (L.menu) b |= START;
    if (L.stick_click) b |= BACK;
    if (R.stick_click) b |= RTHUMB;
    if ((flags & C_VD_MASK) && vd_chord(in)) b = static_cast<uint16_t>(b & ~(BACK | RTHUMB));
    o.buttons = b;
    o.lt = trigger_byte(L.trigger); o.rt = trigger_byte(R.trigger);
    o.ls[0] = L.stick[0]; o.ls[1] = L.stick[1]; o.rs[0] = R.stick[0]; o.rs[1] = R.stick[1];
    return o;
}
/// vd_overlay_mask for the game (input thread): the masking in compose alone would still let the first click through,
/// because the two clicks of the Virtual Desktop chord seldom land in the same poll (Back toggles the battle help window on
/// its press). So each stick click reaches the game delay_ms late, as it was (a click of any length keeps its length); once
/// both are down together neither reaches the game until both are released. Only Back and R3 are delayed.
constexpr uint32_t k_vd_delay_ms = 80;
struct VdGate {
    struct C { bool down = false, live = false; uint32_t t_on = 0, t_off = 0; } c[2];
    bool chord = false;
    /// The clicks to pass now: 1 left (Back), 2 right (R3).
    int step(bool l, bool r, uint32_t now, uint32_t delay_ms = k_vd_delay_ms) {
        const bool in[2] = { l, r };
        if (l && r) { chord = true; c[0].live = c[1].live = false; }
        for (int h = 0; h < 2; ++h) {
            C& k = c[h];
            if (in[h] && !k.down) { k.down = true; if (!chord) { k.live = true; k.t_on = now; } }
            else if (!in[h] && k.down) { k.down = false; k.t_off = now; }
        }
        if (!l && !r) chord = false;
        int out = 0;
        for (int h = 0; h < 2; ++h) {
            C& k = c[h];
            if (!k.live) continue;
            if (static_cast<int32_t>(now - (k.t_on + delay_ms)) < 0) continue;                       // not yet
            if (!k.down && static_cast<int32_t>(now - (k.t_off + delay_ms)) >= 0) { k.live = false; continue; }   // played out
            out |= 1 << h;
        }
        return out;
    }
};

/// The grips that are down (1 left, 2 right).
inline int grips_down(const In& in) { return (grip_down(in.h[0].squeeze) ? 1 : 0) | (grip_down(in.h[1].squeeze) ? 2 : 0); }

/// The stick with more deflection wins (the VR stick replaces the pad's only when it is pushed further).
template <class S> inline void merge_stick(S& x, S& y, const float* v) {
    const float m_pad = (float)x * (float)x + (float)y * (float)y, m_vr = (v[0] * v[0] + v[1] * v[1]) * 32767.f * 32767.f;
    if (m_vr > m_pad) { x = static_cast<S>(stick_s(v[0])); y = static_cast<S>(stick_s(v[1])); }
}
/// The VR pad into a real pad state (XINPUT_GAMEPAD or Gamepad): buttons OR, triggers max, sticks by magnitude. A
/// disconnected real pad is a zeroed state (the caller zeroes it).
template <class G> inline void merge(const Out& o, G& g) {
    g.wButtons = static_cast<decltype(g.wButtons)>(g.wButtons | o.buttons);
    if (o.lt > g.bLeftTrigger) g.bLeftTrigger = o.lt;
    if (o.rt > g.bRightTrigger) g.bRightTrigger = o.rt;
    merge_stick(g.sThumbLX, g.sThumbLY, o.ls); merge_stick(g.sThumbRX, g.sThumbRY, o.rs);
}

// -- the ImGui feed (Fahrenheit's save screen) -------------------------------------------------------------------------
enum { IM_OFF = 0, IM_SAVE = 1, IM_ALWAYS = 2 };   // ini imgui_vrpad
/// The feed is on: mode 2 always, mode 1 while Fahrenheit's save / load screen is open (save_state != 0, not CLOSED).
inline bool imgui_feed_on(int mode, uint32_t save_state) { return mode == IM_ALWAYS || (mode == IM_SAVE && save_state != 0); }
/// The buttons ImGui may see from the VR pad: all but Start (Fahrenheit's soft reset: L1 + R1 + Start) and Back, unless
/// mode 2 (the owner's option: then everything, the soft reset included).
inline uint16_t imgui_mask(int mode) { return mode == IM_ALWAYS ? static_cast<uint16_t>(0xFFFF) : static_cast<uint16_t>(~(START | BACK) & 0xFFFF); }
constexpr uint8_t k_trigger_threshold = 30;   // XINPUT_GAMEPAD_TRIGGER_THRESHOLD: what ImGui's backend counts as pressed

/// Buttons (and triggers) already down when the feed opens stay masked until released, per button: the A that picked
/// "Save" on the sphere's menu never becomes a press on the save screen's first slot.
struct HeldMask {
    bool was_on = false; uint16_t held = 0; bool lt = false, rt = false;
    void step(bool on, Out& o) {
        if (!on) { was_on = false; held = 0; lt = rt = false; return; }
        if (!was_on) { was_on = true; held = o.buttons; lt = o.lt > k_trigger_threshold; rt = o.rt > k_trigger_threshold; }
        held = static_cast<uint16_t>(held & o.buttons);
        if (o.lt <= k_trigger_threshold) lt = false;
        if (o.rt <= k_trigger_threshold) rt = false;
        o.buttons = static_cast<uint16_t>(o.buttons & ~held);
        if (lt) o.lt = 0;
        if (rt) o.rt = 0;
    }
};
struct ImguiFeed { HeldMask held; uint32_t calls = 0, merged = 0, masked_at_open = 0; uint32_t last_packet = 0; };

/// One ImGui XInputGetState(0) call. real_ok: the real call succeeded (g holds the real pad; otherwise the caller passes a
/// zeroed g). vr_ok: the VR pad is available (on, the session running, synced). suppress: a mod gesture owns the controllers
/// this frame (the VR pad then contributes nothing). Returns whether the VR pad was merged (then g and packet hold the merged
/// state and the call reports success); false: the feed is off, g and packet are untouched (the call returns what it returned).
/// No movement remap, no tap press, no grip hold-back, no quick squeeze: those are the game's. The grips pass as LB / RB
/// (the save list pages with them).
template <class G> inline bool imgui_apply(ImguiFeed& f, int mode, uint32_t save_state, bool vr_ok, bool suppress, const In& in,
                                           uint32_t vr_packet, bool real_ok, G& g, uint32_t& packet) {
    f.calls++;
    const bool on = vr_ok && imgui_feed_on(mode, save_state);
    Out o;
    if (on) o = compose(in, grips_down(in), C_VD_MASK);   // composed even while suppressed: the held mask must see what is down
    o.buttons = static_cast<uint16_t>(o.buttons & imgui_mask(mode));
    const bool opening = on && !f.held.was_on;
    f.held.step(on, o);
    if (opening) f.masked_at_open = f.held.held;
    if (suppress) o = Out{};   // a chord or a seat grip owns the controllers this frame: ImGui sees them all up
    if (!on) return false;
    merge(o, g);
    packet = (real_ok ? packet : 0u) + vr_packet;
    f.merged++; f.last_packet = packet;
    return true;
}

// -- the walk gate ---------------------------------------------------------------------------------------------------
constexpr uint32_t k_walk_stale_ms = 250;
enum WalkWhy { W_OPEN = 0, W_STALE, W_UNREADABLE, W_BATTLE, W_SPHERE, W_SAVE, W_MENU, W_CHOICE, W_NO_CONTROL, W_N };
static const char* const k_walk_why[W_N] = { "walking (rotated)", "stale: no game state for 250 ms", "game state not readable", "battle",
                                             "sphere grid", "save / load screen", "a field menu", "a choice box", "no player control" };
/// The walk gate's reason (W_OPEN: the stick is rotated with the view, as walking needs). bits: managed's game bits (the tap
/// gate's: gates::GB_*), age_ms: since the last push, have: a push ever arrived. Stale or missing shuts (raw stick, a
/// gamepad's, never a rotated menu). Riding keeps the remap (the stick steers the bird). Blitzball keeps today's behaviour
/// (the remap; its pass / shoot menus are a separate owner test).
inline int walk_why(int bits, uint32_t age_ms, bool have) {
    using namespace gates;
    if (!have || age_ms > k_walk_stale_ms) return W_STALE;
    if (bits & GB_UNREADABLE) return W_UNREADABLE;
    if (bits & GB_BLITZ) return W_OPEN;
    if (bits & GB_BATTLE) return W_BATTLE;
    if (bits & GB_SPHERE) return W_SPHERE;
    if (bits & GB_SAVE) return W_SAVE;
    if (bits & GB_MENU) return W_MENU;
    if (bits & GB_CHOICE) return W_CHOICE;
    if (bits & GB_NO_CONTROL) return W_NO_CONTROL;
    return W_OPEN;
}
inline bool move_walk(int bits, uint32_t age_ms, bool have) { return walk_why(bits, age_ms, have) == W_OPEN; }

// -- quick-squeeze grips -----------------------------------------------------------------------------------------------
enum { G_OFF = 0, G_BATTLE_GRAB = 1, G_ALWAYS = 2 };   // ini grip_lbrb
struct GripCfg { int mode = G_BATTLE_GRAB; uint32_t tap_ms = 300; float max_move_m = 0.03f, max_turn_deg = 10.f; };
inline uint32_t clamp_tap_ms(int ms) { return static_cast<uint32_t>(ms < 150 ? 150 : ms > 500 ? 500 : ms); }
/// A squeeze is deferred (decided at its release) in mode 1 in battle (the grips are the chord modifiers) or in a grab view
/// (the seat / the diorama: the grips are the grab), in mode 2 everywhere; otherwise it goes to the game at once (mode 0:
/// always, today's behaviour).
inline bool grip_defers(int mode, bool battle, bool grab_view) { return mode >= G_ALWAYS || (mode == G_BATTLE_GRAB && (battle || grab_view)); }

/// One hand's sample, once a frame (gesture_tick).
struct GripSample {
    bool down = false;                 // the grip squeezed (> 0.5)
    bool pose_ok = false; float p[3] = { 0.f, 0.f, 0.f }, q[4] = { 0.f, 0.f, 0.f, 1.f };   // the grip pose (m; x, y, z, w)
    bool any_click = false;            // either stick click (a chord)
    bool other_down = false;           // the other grip (a two-hand grab)
    bool battle = false, grab_view = false;   // the context now
};
enum GripVerdict { GV_NONE = 0, GV_SENT, GV_HELD, GV_CHORD, GV_MOVED, GV_TURNED, GV_TWO, GV_PASSED, GV_LOST, GV_N };
static const char* const k_grip_verdict[GV_N] = { "none", "sent", "held (grab / chord modifier)", "chord", "moved", "turned", "two grips",
                                                  "passed to the game at once", "controller lost" };
struct GripTap {
    bool down = false, defer = false, grab0 = false, have_pose = false, chord = false, other = false;
    uint32_t t0 = 0; float p0[3] = { 0.f, 0.f, 0.f }, q0[4] = { 0.f, 0.f, 0.f, 1.f }; float max_d = 0.f, max_deg = 0.f;
    int last = GV_NONE; uint32_t last_ms = 0; float last_d = 0.f, last_deg = 0.f; uint32_t fired = 0, squeezes = 0;
};
enum { GT_PASS = 1, GT_FIRE = 2, GT_RESTORE = 4, GT_BEGIN = 8, GT_END = 16 };
/// The angle between two unit quaternions (degrees, 0..180).
inline float quat_angle_deg(const float* a, const float* b) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]; d = d < 0.f ? -d : d; if (d > 1.f) d = 1.f;
    return 2.f * std::acos(d) * 57.2957795f;
}
/// One sample. Returns GT_* bits: PASS this grip goes to the game as LB / RB now (also while up: what a new squeeze would
/// do, so the input thread passes it with no frame of delay); BEGIN a squeeze began (snapshot the grab); END it ended; FIRE
/// it was a quick squeeze: send the shoulder once; RESTORE undo the grab's nudge (the squeeze began in a grab view).
/// A quick squeeze: released within tap_ms of the squeeze, the hand moved less than max_move_m and turned less than
/// max_turn_deg from where it was squeezed (the largest seen), no stick click while it was down (no chord), and the other grip
/// never down meanwhile (no two-hand grab). A squeeze keeps the kind it began with (passed / deferred) until it is released,
/// so a context change never makes an LB / RB edge the user did not make.
inline int grip_step(GripTap& s, const GripCfg& c, const GripSample& in, uint32_t now) {
    int r = 0;
    if (in.down && !s.down) {
        s.down = true; s.defer = grip_defers(c.mode, in.battle, in.grab_view); s.grab0 = in.grab_view; s.t0 = now;
        s.chord = in.any_click; s.other = in.other_down; s.max_d = s.max_deg = 0.f; s.have_pose = in.pose_ok; s.squeezes++;
        if (in.pose_ok) for (int i = 0; i < 4; ++i) { if (i < 3) s.p0[i] = in.p[i]; s.q0[i] = in.q[i]; }
        r |= GT_BEGIN;
    }
    else if (in.down) {
        s.chord = s.chord || in.any_click; s.other = s.other || in.other_down;
        if (in.pose_ok && !s.have_pose) { s.have_pose = true; for (int i = 0; i < 4; ++i) { if (i < 3) s.p0[i] = in.p[i]; s.q0[i] = in.q[i]; } }
        else if (in.pose_ok) {
            const float dx = in.p[0] - s.p0[0], dy = in.p[1] - s.p0[1], dz = in.p[2] - s.p0[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz), a = quat_angle_deg(s.q0, in.q);
            if (d > s.max_d) s.max_d = d;
            if (a > s.max_deg) s.max_deg = a;
        }
    }
    else if (s.down) {
        s.down = false; r |= GT_END;
        const uint32_t el = now - s.t0;
        int v;
        if (!s.defer) v = GV_PASSED;
        else if (s.chord) v = GV_CHORD;
        else if (s.other) v = GV_TWO;
        else if (el >= c.tap_ms) v = GV_HELD;
        else if (s.max_d >= c.max_move_m) v = GV_MOVED;
        else if (s.max_deg >= c.max_turn_deg) v = GV_TURNED;
        else { v = GV_SENT; r |= GT_FIRE; if (s.grab0) r |= GT_RESTORE; s.fired++; }
        s.last = v; s.last_ms = el; s.last_d = s.max_d; s.last_deg = s.max_deg;
    }
    const bool pass = s.down ? !s.defer : !grip_defers(c.mode, in.battle, in.grab_view);
    if (pass) r |= GT_PASS;
    return r;
}
/// The controllers went away (no sync, the VR pad off): a squeeze in progress ends without a verdict of its own.
inline void grip_reset(GripTap& s) { if (s.down) { s.down = false; s.last = GV_LOST; } }

/// The press slot in the game's poll (input thread), hv_tap_hook's rule (ffxvr_hands.cpp) with the minimum of new packets as
/// a parameter. The game's pad poll (0x62DBA0) ignores a state whose packet did not change, so a press is ORed in until
/// press_ms has passed AND the game got min_polls new packets with it, never at or past k_press_cap_ms; after a release one
/// new packet without the button reaches the game before the next press. polls: the caller's count for this press (it adds
/// 1 on HV_HOOK_COUNT's value 4). Returns 1 hold, 2 release, | 4 count (the HV_HOOK_* values).
/// A quick squeeze uses min_polls = 2, press_ms = grip_tap_press_ms (default 0): the battle CTB scroll (0x89F021) steps a
/// row when R1 is held and the list is at rest (target == shown), and the shown offset slides 11 px (half a row) a tick, so
/// R1 seen on 1 or 2 consecutive UI ticks is exactly one row, 3 or 4 ticks two rows. 2 polls is one row when the game polls
/// once or twice per battle UI tick (the poll and the UI are the same logic frame: about 24 polls a second in the owner's
/// 4 Nov log, 30 Hz logic); two rows only if the UI ticked twice per poll. The tap's 3 polls / 100 ms is 3-4 ticks: 2 rows.
constexpr int32_t k_press_cap_ms = 400;
struct PressHook { uint32_t pkt = 0; bool need_clear = false; };
inline int press_hook(PressHook& h, bool pressed, uint32_t pkt, int32_t el_ms, int32_t press_ms, int32_t polls, int32_t min_polls) {
    const bool new_pkt = pkt != h.pkt; h.pkt = pkt;
    if (!pressed) { if (new_pkt) h.need_clear = false; return 0; }
    if (el_ms < 0 || el_ms >= k_press_cap_ms) { h.need_clear = !new_pkt; return 2; }
    if (h.need_clear) { if (new_pkt) h.need_clear = false; return 0; }
    const int count = new_pkt ? 4 : 0;
    if (el_ms >= press_ms && polls >= min_polls) { h.need_clear = !new_pkt; return 2; }
    return 1 | count;
}
constexpr int k_sh_min_polls = 2;
inline int32_t clamp_press_ms(int ms) { return ms < 0 ? 0 : ms > 300 ? 300 : ms; }

/// Quick squeezes waiting for the input thread's press slot (render thread only): at most 3 per hand, oldest first,
/// alternating fairly when both hands queued.
struct ShQueue {
    uint8_t n[2] = { 0, 0 }; int next = 0;
    void push(int h) { if (h >= 0 && h < 2 && n[h] < 3) n[h]++; }
    /// The shoulder bit to press now (0: nothing queued).
    uint16_t pop() {
        for (int k = 0; k < 2; ++k) {
            const int h = (next + k) & 1;
            if (n[h]) { n[h]--; next = h ^ 1; return h == 0 ? LSHOULDER : RSHOULDER; }
        }
        return 0;
    }
    void clear() { n[0] = n[1] = 0; }
};

}  // namespace pad
