// SPDX-License-Identifier: MIT
// ffxvr_menuchord.h -- vr-menu S4: open and close the F8 window from the controllers with the left grip held and a press of the left
// menu button (the one the mod sends to the game as Start). It replaces S4's first try, a long hold of the menu button alone: Virtual
// Desktop reads that button on the headset itself and takes long presses and double taps for its own menu, which the mod cannot
// mask. Pure logic: no Windows, no D3D, no OpenXR, no allocation. The native side runs chord_step in vrpad_take (every game frame and
// every stickrep extra frame, on the raw input) and counts the toggles; managed toggles the window on a new count exactly as F8 does
// (module.cs toggle_window). A unit test (vrmenu_chk/s4/menuchord_test.cpp in the session scratchpad) drives it with pad::grip_step.
//
//   the chord     the left grip squeezed for at least k_grip_min_s, then a press edge of the left menu button: one toggle. A squeeze
//                 and a press together (within k_grip_min_s), or the button first, are a plain Start as before.
//   the button    the chord's press never reaches the game or ImGui's pad (no pause), nor the button until it is let go
//   the grip      the chord's grip belongs to it until it is let go: no quick-squeeze LB (pad::GripTap's chord, as a stick click
//                 makes it), no world grab from it (and a grab nudge since the squeeze began is undone, as a quick squeeze's is)
//   the soft reset Fahrenheit's both grips + Start: with the right grip down the menu button is Start as before (no chord)
//   the gate      the headset on, the controllers the gamepad, the left controller active; a press without it is Start
#pragma once
#define FFXVR_MENUCHORD_REV 1
#include <cmath>

namespace vmc {

constexpr float k_grip_min_s = 0.15f;                    // the grip held at least this long before the press
constexpr float k_dt_max = 0.25f;                        // one call never counts more than this (a stall)

struct Chord {
    bool grip = false;     // the left grip down at the last call
    float grip_t = 0.f;    // ... for this long
    bool menu = false;     // the menu button down at the last call
    bool masked = false;   // the menu button is the chord's (hidden from the game and ImGui) until it is let go
    bool owned = false;    // the left grip is the chord's until it is let go
};
struct Out {
    bool toggle = false;   // toggle the window now
    bool mask = false;     // hide the menu button (Start) from the game and ImGui's pad
    bool fired = false;    // the chord fired this call (veto the grip's quick squeeze, undo its grab nudge)
    bool owned = false;    // the left grip is the chord's (no world grab from it)
};
/// Once a call. enabled: the gate; grip: the left grip squeezed (raw); other: the right grip squeezed; menu: the left menu button
/// (raw); dt: seconds since the last call.
inline Out chord_step(Chord& c, bool enabled, bool grip, bool other, bool menu, float dt) {
    Out o;
    if (grip) {   // (a squeeze's first call counts no time: a squeeze and a press together are no chord)
        if (c.grip) c.grip_t += (dt > 0.f && std::isfinite(dt)) ? (dt < k_dt_max ? dt : k_dt_max) : 0.f;
        c.grip = true;
    }
    else { c.grip = false; c.grip_t = 0.f; c.owned = false; }
    const bool press = menu && !c.menu;
    c.menu = menu;
    if (!menu) c.masked = false;
    if (press && enabled && !other && c.grip_t >= k_grip_min_s) {   // (grip_t is 0 unless the grip is down)
        c.masked = true; c.owned = true; o.toggle = true; o.fired = true;
    }
    o.mask = c.masked; o.owned = c.owned;
    return o;
}

} // namespace vmc
