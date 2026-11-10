// SPDX-License-Identifier: MIT
// ffxvr_menuhold.h -- vr-menu S4: open and close the F8 window by holding the left controller's menu button (the one the mod sends
// to the game as Start). Pure logic: no Windows, no D3D, no OpenXR, no allocation. The native side runs hold_step in vrpad_take (every
// game frame and every stickrep extra frame, on the raw input, beside the panel's grab and pointer masks) and counts the toggles;
// managed toggles the window on a new count exactly as F8 does (module.cs toggle_window). A unit test (vrmenu_chk/s4/menuhold_test.cpp
// in the session scratchpad) drives it.
//
//   a tap         unchanged: the press reaches the game as Start (it pauses) and the ImGui pad feed, as before
//   a hold        k_hold_default_s (vr_menu_hold_ms) of the button held: one toggle, then the button is hidden (Start released for
//                 the game and ImGui) until it is let go, so a held Start does nothing more and never makes a second press edge
//                 (Fahrenheit's soft reset wants L1 + R1 down and a Start press: the hold cannot complete it)
//   the gate      only with the headset on and the controllers as the gamepad (enabled); a hold begun without it never toggles
#pragma once
#define FFXVR_MENUHOLD_REV 1
#include <cmath>

namespace vmh {

constexpr float k_hold_default_s = 1.0f;                 // vr_menu_hold_ms 1000
constexpr float k_hold_min_s = 0.4f, k_hold_max_s = 3.0f; // vr_menu_hold_ms 400..3000
constexpr float k_dt_max = 0.25f;                        // one call never counts more than this (a stall is not a hold)

struct Hold {
    bool down = false;     // the button was down at the last call
    float t = 0.f;         // seconds held, while it may still toggle
    bool fired = false;    // this hold toggled (once)
    bool barred = false;   // this hold began, or went on, without the gate: it never toggles
};
struct Out { bool toggle = false; bool mask = false; };   // toggle the window now; hide the button from the game and ImGui
inline float clamp_hold(float s) { return !std::isfinite(s) ? k_hold_default_s : s < k_hold_min_s ? k_hold_min_s : s > k_hold_max_s ? k_hold_max_s : s; }
/// Once a call. enabled: vr_menu_hold on, the headset running, the controllers the gamepad; button: the left menu button (raw);
/// dt: seconds since the last call; hold_s: the hold time (clamped).
inline Out hold_step(Hold& h, bool enabled, bool button, float dt, float hold_s) {
    Out o;
    if (!button) { h = Hold{}; return o; }                  // released: everything ends (a mask with it: the button is up anyway)
    if (!h.down) { h.down = true; h.t = 0.f; h.fired = false; h.barred = !enabled; return o; }   // the press: it reaches the game (a tap)
    if (!enabled) h.barred = true;                          // the gate went during the hold: no toggle for it
    if (!h.fired && !h.barred) {
        h.t += (dt > 0.f && std::isfinite(dt)) ? (dt < k_dt_max ? dt : k_dt_max) : 0.f;
        if (h.t >= clamp_hold(hold_s)) { h.fired = true; o.toggle = true; }
    }
    o.mask = h.fired;                                       // from the toggle until the release
    return o;
}

} // namespace vmh
