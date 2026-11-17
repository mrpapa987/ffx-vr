// SPDX-License-Identifier: MIT
// ffxvr_dioanchor.h -- the battle diorama's view at the start and the end of a battle (branch dio-anchor, ABI 70;
// design-notes "Battle: only the combatants in the fight count"; managed battle_anchor.cs).
// Pure logic: no Windows, no D3D, no file. The native glue (ffxvr_native.cpp: dio_battle_view on the view tick,
// ffxvr_anchor_override, ffxvr_eye_event 4, ffxvr_fp_set) feeds it; dio_chk's test drives it and the glue's own lines.
//
// In battle the diorama is anchored to the fight's table (ffxvr_anchor_override, from the managed side, which also numbers
// the battles). The first frame the diorama uses a battle's table, the view goes to its defaults: the offset, tilt and
// zoom (view_defaults(2): behind and above) and, when the managed side has sent the direction from the party to the
// enemies (ffxvr_eye_event 4), the heading, so "behind" is behind the party. Once a battle: what is moved after that
// stays. The field's own view (offset, zoom, tilt, heading) is kept at that moment and put back when the table is taken
// away again in the diorama (the battle ended, or battle_anchor was unticked); a view-mode change in between drops it
// (the mode change starts the new mode from its defaults, as always). A grab in progress on the first frame skips that
// battle's reset (the hand is already moving the view). None of this is a user's view action (no bt_user).
#pragma once
#include <cmath>

namespace dio {

/// ffxvr_anchor_override's "valid" argument: bit 0 a table is given, bit 1 this battle's view starts from the defaults,
/// the battle's serial from bit 2 up (1 .. 0xFFFFF; 0: none). 0 and 1 mean what they did before the bits were added.
struct Arg { bool valid; bool reset; int serial; };
inline Arg arg_decode(int v) { Arg a; a.valid = v != 0; a.reset = (v & 2) != 0; a.serial = v >> 2; return a; }

/// The diorama's view: the grab offset (right, up, forward), the zoom, the tilt, the heading.
struct View { float move[3]; float scale; float pitch; float yaw; };

struct Battle {
    bool valid = false, reset = false;   // the last ffxvr_anchor_override
    int  epoch = 0, epoch_done = 0;      // its serial; the serial whose first frame in the diorama was handled
    bool face = false; float yaw = 0.f;  // ffxvr_eye_event 4: the heading from the party toward the enemies
    bool kept = false; View field = {};  // the field's view, kept at a reset until the table is taken away
};

enum { ACT_NONE = 0, ACT_RESET = 1, ACT_RESTORE = 2 };

/// Once a frame, on the view tick. diorama = the view mode is the diorama; grabbing = a grab is in progress; cur = the view
/// now. ACT_RESET: the caller sets the defaults (and b.yaw when b.face); cur was kept unless a view is already kept (a
/// battle right after another, with no field frame between: the field's stays). ACT_RESTORE: *back is the kept view.
inline int battle_view(Battle& b, bool diorama, bool grabbing, const View& cur, View* back) {
    if (!diorama) return ACT_NONE;
    if (b.valid) {
        if (b.epoch == b.epoch_done) return ACT_NONE;
        b.epoch_done = b.epoch;
        if (!b.reset || grabbing) return ACT_NONE;
        if (!b.kept) { b.kept = true; b.field = cur; }
        return ACT_RESET;
    }
    if (b.kept) { b.kept = false; *back = b.field; return ACT_RESTORE; }
    return ACT_NONE;
}
/// ffxvr_fp_set saw another view mode: the kept view belongs to the mode that was left.
inline void mode_changed(Battle& b) { b.kept = false; }

/// The heading (yaw about +y, as g_fp_yaw: forward = (-sin, 0, -cos)) that looks along (dx, dz) in render space. False:
/// no direction (not finite, or shorter than one unit: the two groups stand on each other).
inline bool face_yaw(float dx, float dz, float* yaw) {
    const float l2 = dx * dx + dz * dz;
    if (!std::isfinite(l2) || !(l2 >= 1.f)) return false;   // (NaN or infinity in either makes l2 not finite)
    *yaw = std::atan2(-dx, -dz);
    return true;
}

}   // namespace dio
