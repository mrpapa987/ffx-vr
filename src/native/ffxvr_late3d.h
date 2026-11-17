// SPDX-License-Identifier: MIT
// ffxvr_late3d.h -- "a late 3D layer is not the UI pass" (branch late3d, ABI 70; design-notes "Late 3D layers: Holy").
// Pure logic: no Windows, no D3D, no file. The native glue (ffxvr_native.cpp: ffxvr_on_draw right before the UI
// classification, the scene resolve, ffxvr_begin_frame's ui: line, ffxvr_late3d_set) feeds it; a unit test drives it and the
// glue's own lines against a model of the frame.
//
// Why: the owner's report on the spell Holy (Nov 2026): "we still get the squished/flatted enemy at a weird angle and we
// really don't see the spell effects". The mod recognises the game's 2D UI pass by its frame structure: a resolve of the
// scene target, a depth-only clear on it, draws, a resolve. In Holy's second phase the game does exactly that for 3D: after
// its post-processing it clears the depth, draws the spell's target alone (3 or 4 ordinary skinned draws with the scene
// camera), resolves, then draws the effects, and only then the real HUD behind its own depth clear. The mod took the first
// clear for the UI's: the target went flat onto the UI panel, the eye images were frozen at that clear, and the effects and
// the HUD never reached the headset.
//
// The rule: while the UI pass is open and has taken no draw yet, a draw into the scene target whose slot-0 buffer has an eye
// image of a real scene camera means the depth clear was not the UI's. The pass is given back (the caller sets its state to
// idle) and the draw goes the normal stereo way. The game's next resolve of the scene target is then the new base and the
// HUD's own depth clear opens the pass again: both are the code that was already there.
// A 2D draw can never match: its buffer is far too small to hold a camera (a view, its inverse and a projection need 368
// bytes; the HUD's buffers are 224 and 240). NOT excluded (review S1): a real UI pass in a battle whose first draw is a true
// 3D draw with the scene camera. None is known, and only one ordinary UI pass has been censused. Such a pass would be given
// back and that frame's HUD would be drawn into the eye images; two log-only counters show it ("SUSPECT" in the ui: line
// and one late3d: line a session): frames in which a pass was given back and no UI pass completed, and draws with no eye
// image inside a late layer (a HUD in the eyes is dozens of those a frame; Holy's layer has none).
// It acts only in a battle with late_3d_layer 1; everything else it only counts.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace l3d {

/// What the draw's slot-0 constant buffer is to the mod this frame.
enum { IMG_NONE = 0,      // no eye image: the mod did not find a camera in it (every 2D draw)
       IMG_CAMERA = 1,    // an eye image of a real scene camera (a view, its inverse, a perspective projection)
       IMG_OTHER = 2 };   // any other eye image: a product-only buffer (the PS2 effect family), the 2D layers' dummy camera

/// What the rule says about one draw into the scene target while the UI pass is open.
enum { V_NONE = 0,             // nothing: a 2D draw, the Blitzball early pass, or the pass already has draws
       V_LATE = 1,             // a late 3D layer: the caller gives the pass back; the draw goes the normal stereo way
       V_WOULD_NOBATTLE = 2,   // the same shape outside a battle: counted, nothing changes
       V_WOULD_OFF = 3,        // the same shape in a battle with late_3d_layer 0: counted, nothing changes
       V_OTHER = 4,            // the pass's first draw has another eye image: counted, nothing changes
       V_CAM_LATER = 5,        // a scene-camera draw after the pass's first draw: counted, nothing changes
       V_SUSPECT_NOPASS = 6,   // (an Event only) a pass was given back and the frame ended without a completed UI pass
       V_SUSPECT_NOIMG = 7 };  // (an Event only) draws with no eye image inside a late layer

/// The rule. early = the open pass is the Blitzball early one; pass_draws = the draws the pass has taken; img = IMG_*;
/// mode = late_3d_layer (1 acts); battle = managed says a battle is running.
inline int verdict(bool early, uint32_t pass_draws, int img, int mode, bool battle) {
    if (early) return V_NONE;
    if (pass_draws != 0) return img == IMG_CAMERA ? V_CAM_LATER : V_NONE;
    if (img == IMG_NONE) return V_NONE;
    if (img != IMG_CAMERA) return V_OTHER;
    if (!battle) return V_WOULD_NOBATTLE;
    if (mode != 1) return V_WOULD_OFF;
    return V_LATE;
}

constexpr int k_said = 16;   // the magic ids one battle gets a line for (more: no further lines that battle)
constexpr uint32_t k_stale = 2;   // frames after managed's last push that its battle state still counts (review N1)

/// One match, kept for the log line and the frame census the glue writes at the next Present. For a V_SUSPECT_* event
/// size = the frame's draws with no eye image inside a late layer and n = the passes it gave back.
struct Event { int verdict; uint32_t frame; int32_t magic; uint32_t size, n; };

struct Tracker {
    int mode = 1; bool battle = false; int32_t magic = -1; uint32_t set_frame = 0;   // from managed, once a frame (set)
    // the frame being drawn
    bool layer_open = false; uint32_t f_layers = 0, f_would = 0, f_noimg = 0;
    // the 10 s window (the ui: line)
    uint32_t w_frames = 0, w_layers = 0, w_draws = 0, w_nobattle = 0, w_off = 0, w_other = 0, w_cam_later = 0;
    uint32_t w_nopass = 0, w_noimg = 0;                // review S1: the two signs of a real UI pass taken for a late layer
    // the session
    uint32_t s_frames = 0, s_layers = 0, s_would = 0, s_nopass = 0, s_noimg = 0;
    int32_t said[k_said] = {}; int said_n = 0;         // this battle's magic ids that have their line
    bool would_said = false, census_late = false, census_would = false, suspect_said = false;   // once a session each
    bool line_on = false, census_on = false, suspect_on = false; Event line = {}, census = {}, suspect = {};

    bool said_has(int32_t m) const {
        if (said_n >= k_said) return true;
        for (int i = 0; i < said_n; ++i) if (said[i] == m) return true;
        return false;
    }
    /// Once a frame, from managed: the setting, whether a battle is running, the magic file's id in play (-1 none);
    /// frame = the frame counter now (the battle state counts for k_stale frames: a push that stops is "no battle").
    void set(int mode_, bool battle_, int32_t magic_, uint32_t frame) {
        if (battle_ != battle) said_n = 0;   // a new battle's lines start over
        mode = mode_ == 0 ? 0 : 1; battle = battle_; magic = battle_ ? magic_ : -1; set_frame = frame;
    }
    /// A battle is running as far as the last push says, and that push is recent.
    bool battle_live(uint32_t frame) const { return battle && frame - set_frame <= k_stale; }
    /// Would a scene-camera draw be given back now? (ffxvr_on_draw's first-person head hide, which runs before the rule:
    /// a late drawing's parts are counted on their own, review S2.) True also while a late layer is open.
    bool late_now(bool pass_open, bool early, uint32_t pass_draws, uint32_t frame) const {
        return layer_open || (pass_open && verdict(early, pass_draws, IMG_CAMERA, mode, battle_live(frame)) == V_LATE);
    }
    /// ffxvr_on_draw: a draw into the scene target while the UI pass is open. frame = the frame being drawn, size = the
    /// slot-0 buffer's bytes (0 without an image), n = the draw's index or vertex count. Returns V_*: only on V_LATE does
    /// the caller change anything (the UI pass's state back to idle).
    int first_draw(bool early, uint32_t pass_draws, int img, uint32_t frame, uint32_t size, uint32_t n) {
        const int v = verdict(early, pass_draws, img, mode, battle_live(frame));
        if (v == V_NONE) return v;
        if (v == V_CAM_LATER) { w_cam_later++; return v; }
        if (v == V_OTHER) { w_other++; return v; }
        const Event e = { v, frame, magic, size, n };
        if (v == V_LATE) {
            if (f_layers == 0) { w_frames++; s_frames++; }
            f_layers++; w_layers++; s_layers++; layer_open = true;
            if (!census_late) { census_late = true; if (!census_on) { census = e; census_on = true; } }
        }
        else {
            if (v == V_WOULD_NOBATTLE) w_nobattle++; else w_off++;
            if (f_would == 0) s_would++;
            f_would++;
            if (v == V_WOULD_NOBATTLE) {   // outside a battle: one line and one census a session
                if (!would_said && !line_on) { would_said = true; line = e; line_on = true; }
                if (!census_would) { census_would = true; if (!census_on) { census = e; census_on = true; } }
                return v;
            }
        }
        // in a battle (acted on, or late_3d_layer 0): one line per magic id
        if (!line_on && !said_has(magic)) { said[said_n++] = magic; line = e; line_on = true; }
        return v;
    }
    /// ffxvr_on_draw: every draw into the scene target (after first_draw). Counts the draws of a late layer, and those of
    /// them with no eye image (review S1: a HUD drawn into the eyes).
    void scene_draw(bool has_image) {
        if (!layer_open) return;
        w_draws++;
        if (!has_image) { f_noimg++; w_noimg++; s_noimg++; }
    }
    /// The game resolved the scene target: the late layer's draws are over.
    void scene_resolve() { layer_open = false; }
    /// ffxvr_begin_frame: the frame `frame` is over; had_ui_pass = it was shown with a completed UI pass. Review S1: a
    /// frame that gave a pass back and then had no UI pass, or had draws with no eye image inside a late layer, is
    /// counted; the session's first one leaves a line and asks for a census.
    void frame_end(bool had_ui_pass, uint32_t frame) {
        const bool nopass = f_layers > 0 && !had_ui_pass;
        if (nopass) { w_nopass++; s_nopass++; }
        if ((nopass || f_noimg > 0) && !suspect_said) {
            suspect_said = true; suspect_on = true;
            suspect = Event{ nopass ? V_SUSPECT_NOPASS : V_SUSPECT_NOIMG, frame, magic, f_noimg, f_layers };
            if (!census_on) { census = suspect; census_on = true; }
        }
        layer_open = false; f_layers = 0; f_would = 0; f_noimg = 0;
    }
    /// The glue, once a frame: the line to write, the suspect line to write (each true once per request). The census
    /// request (census_on, census) stays until the glue could arm it (review S3): the glue clears census_on itself.
    bool take_line(Event* out) { if (!line_on) return false; *out = line; line_on = false; return true; }
    bool take_suspect(Event* out) { if (!suspect_on) return false; *out = suspect; suspect_on = false; return true; }

    /// The ui: line's part for the window just ended (every 10 s); the window starts again. Returns the characters written.
    int hb_text(char* dst, int cap) {
        if (dst == nullptr || cap <= 0) return 0;
        const int w = snprintf(dst, static_cast<size_t>(cap),
            " | late 3D layers (late_3d_layer %d, %s): %u frames, %u layers, %u draws left in the 3D view; the same shape not acted on: %u outside a battle, %u with the setting off; "
            "UI passes whose first draw had another eye image %u; scene-camera draws later in a UI pass %u; this session %u frames, %u layers",
            mode, battle ? "in a battle" : "no battle", w_frames, w_layers, w_draws, w_nobattle, w_off, w_other, w_cam_later, s_frames, s_layers);
        int n = w < 0 ? 0 : w < cap ? w : cap - 1;
        if ((w_nopass != 0 || w_noimg != 0) && n < cap - 1) {   // review S1: said only when there is something to say
            const int w2 = snprintf(dst + n, static_cast<size_t>(cap - n),
                "; SUSPECT (a real UI pass may have been taken for a late layer: was the HUD off its panel?): %u frames gave a pass back and ended with no UI pass, %u draws with no eye image inside a late layer (this session %u, %u)",
                w_nopass, w_noimg, s_nopass, s_noimg);
            n = w2 < 0 ? n : w2 < cap - n ? n + w2 : cap - 1;
        }
        w_frames = w_layers = w_draws = w_nobattle = w_off = w_other = w_cam_later = w_nopass = w_noimg = 0;
        return n;
    }
};

/// The magic id as the lines name it ("magic 0x0092", or "no magic file" for -1).
inline void magic_text(char* dst, int cap, int32_t magic) {
    if (dst == nullptr || cap <= 0) return;
    if (magic == -1) snprintf(dst, static_cast<size_t>(cap), "no magic file loaded");
    else snprintf(dst, static_cast<size_t>(cap), "magic 0x%04X", static_cast<unsigned>(magic) & 0xFFFFu);
}
/// What one match's line says after its head (the glue writes "fx: battle N frame F | magic 0x.... | " or "late3d: " first).
inline void event_text(char* dst, int cap, const Event& e) {
    if (dst == nullptr || cap <= 0) return;
    const char* what =
        e.verdict == V_LATE ? "left in the 3D view (late_3d_layer 1): the draws from here to the game's next resolve go the normal stereo way, that resolve is the new base, and the UI pass reopens at the game's next depth clear" :
        e.verdict == V_WOULD_OFF ? "NOT acted on (late_3d_layer 0, the old behaviour): these draws go onto the UI panel, the eye images stay as they were at that depth clear, and what the game draws after them does not reach the headset" :
        "NOT acted on (no battle is running): counted only; the UI pass takes these draws as before";
    snprintf(dst, static_cast<size_t>(cap),
             "late 3D layer at frame %u: after a depth-only clear on the scene target that opened the UI pass, the first draw has a scene camera's eye image (a %u B buffer, %u indices): %s",
             e.frame, e.size, e.n, what);
}

/// Review S1: the session's first suspect frame, after "late3d: <magic> | ".
inline void suspect_text(char* dst, int cap, const Event& e) {
    if (dst == nullptr || cap <= 0) return;
    snprintf(dst, static_cast<size_t>(cap),
             "SUSPECT late 3D layer at frame %u: %s (%u passes given back that frame, %u draws with no eye image inside a late layer). If the battle menus left their panel in that frame, "
             "a real UI pass was taken for a late layer: set late_3d_layer=0 and send this log (a frame census was asked for). The 10 s ui: line counts further ones",
             e.frame, e.verdict == V_SUSPECT_NOPASS ? "a UI pass was given back and the frame ended with no UI pass at all" : "draws with no eye image were drawn inside a late layer", e.n, e.size);
}
/// The census's reason for an event (the glue adds the magic id's text).
inline const char* census_text(const Event& e) {
    return e.verdict == V_LATE ? "the session's first" : e.verdict == V_WOULD_NOBATTLE ? "the first not acted on, outside a battle" : "SUSPECT, the session's first";
}

}   // namespace l3d
