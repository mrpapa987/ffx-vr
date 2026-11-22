// SPDX-License-Identifier: MIT
// ffxvr_odbar.h -- the overdrive bar art (19 Nov 2026; the owner: "updating tidus's overdrive bar ... to use this kind of art",
// "let's only use it for tidus and lulu for now"). Pure: no Windows, no D3D, no allocation, no game data. The art is a local
// pack, <mod>\art\odbar\*.png (tools/target-art/bar_art.py cuts it from the owner's AI-generated sheet; never in the repository
// or the mod's zip; docs/design-notes.md "Overdrive bar art (local pack)"). The native glue decodes the seven pieces (WIC),
// checks each size, packs them into one 1024 x 1024 atlas with its full mip chain (the target art's packing and mips,
// odt::art::put / mip_down) and makes its texture; ffxvr_odvis.h draws Tidus's Swordplay bar and Lulu's Fury countdown from it with
// the layout below, in the targets' own draw (their shader, the atlas in slot 1: Vtx t.z 2, + 4 additive). Without the pack,
// or with F8 "overdrive bar art" off, the visuals are exactly what they were. A unit test (outside the repository, as the
// other overdrive suites) checks the placement from the game's values, the slices, the countdown and the guard.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>

namespace odb {

// -- the pack and its atlas ---------------------------------------------------------------------------------------------
enum Piece { P_FRAME = 0, P_FILL, P_EMPTY, P_PLATE, P_ZONE, P_DOWN, P_UP, P_N };
inline const char* file(int p) {
    static const char* const k[P_N] = { "frame.png", "track_fill.png", "track_empty.png", "plate.png", "zone.png", "cursor_down.png", "cursor_up.png" };
    return p >= 0 && p < P_N ? k[p] : "";
}
constexpr int k_side = 1024;   // the atlas: 1024 x 1024 RGBA8 with its chain (5.3 MB)
/// A piece's cell (texels): each PNG must be exactly this size (bar_art.py writes them so). 16 texels between neighbours,
/// so mips 0-4 never mix two pieces (the art's sampler stops at mip 6; the bar is about 600 texels of a 1024 piece wide in
/// the headset, mip 0-1).
struct Cell { int x = 0, y = 0, w = 0, h = 0; };
inline Cell cell(int p) {
    Cell c;
    switch (p) {
        case P_FRAME: c.x = 0;   c.y = 0;   c.w = 1024; c.h = 112; break;
        case P_FILL:  c.x = 0;   c.y = 128; c.w = 1024; c.h = 96;  break;
        case P_EMPTY: c.x = 0;   c.y = 240; c.w = 1024; c.h = 96;  break;
        case P_PLATE: c.x = 0;   c.y = 352; c.w = 1024; c.h = 160; break;
        case P_ZONE:  c.x = 0;   c.y = 528; c.w = 512;  c.h = 224; break;
        case P_DOWN:  c.x = 528; c.y = 528; c.w = 256;  c.h = 208; break;
        case P_UP:    c.x = 528; c.y = 752; c.w = 256;  c.h = 208; break;
        default: break;
    }
    return c;
}
/// A decoded piece's size check: true when it is its cell's size.
inline bool size_ok(int p, int w, int h) { const Cell c = cell(p); return c.w > 0 && w == c.w && h == c.h; }
/// The atlas u of a column px of a piece (0..its width), half a texel in from the piece's own edges (bilinear never reads the
/// gutter at mip 0) and exact inside, so two slices of a piece meet without a seam; v of a row likewise.
inline float ux(const Cell& c, float px) { const float x = px < 0.5f ? 0.5f : px > static_cast<float>(c.w) - 0.5f ? static_cast<float>(c.w) - 0.5f : px; return (static_cast<float>(c.x) + x) / static_cast<float>(k_side); }
inline float vy(const Cell& c, float py) { const float y = py < 0.5f ? 0.5f : py > static_cast<float>(c.h) - 0.5f ? static_cast<float>(c.h) - 0.5f : py; return (static_cast<float>(c.y) + y) / static_cast<float>(k_side); }

// -- the pieces' proportions (bar_art.py composes the cells so and prints what it measured on the owner's sheet) ---------------
constexpr float k_open_x0 = 0.0488f, k_open_x1 = 0.9512f, k_open_y0 = 0.2857f, k_open_y1 = 0.7143f;   // the frame's opening, of its cell (y down)
constexpr float k_track_pad = 0.10f;   // the track pieces drawn this much (of the opening's height) past its top and bottom, under the frame
constexpr int k_cap_px = 48, k_edge_px = 40;   // the track pieces' round cap, the fill's bright leading edge (cell columns)
constexpr int k_zone_m_px = 24, k_zone_cap_px = 120;   // the zone: its glow outside each end bar, each end bar with its scrolls (cell columns)
constexpr float k_zone_h = 1.78f;      // the zone's quad height, of the opening's (its golden body, 0.223..0.786 of the cell, fills the opening)
constexpr float k_tip_down = 0.933f, k_tip_up = 0.029f;   // the cursors' tips, of their cell's height from its top
constexpr float k_cursor_h = 1.45f;    // a cursor's height, of the opening's
constexpr float k_tip_in = 0.15f;      // the two tips reach this far (of the opening's height) into the opening: they pinch the track
constexpr float k_plate_w = 1.10f, k_plate_dy = -0.20f;   // the plate: its width of the frame's; its centre below the bar's centre (of its own height)

// -- the layout ---------------------------------------------------------------------------------------------------------
/// A bar whose opening (the track: the game's bar, the countdown's length) is W wide, centred at the local origin (x right,
/// y up, metres). Every other size follows from W and the pieces' proportions.
struct Layout {
    float W = 0.f, hi = 0.f;                          // the opening: width, height
    float fw = 0.f, fh = 0.f, fcx = 0.f, fcy = 0.f;   // the frame's quad: size, and its centre from the opening's
    float th = 0.f;                                   // the track pieces' height (the opening's and the pads)
    float pw = 0.f, ph = 0.f, pcy = 0.f;              // the plate's quad: size, centre height
    float cw = 0.f, ch = 0.f;                         // a cursor's quad
    float zh = 0.f;                                   // the zone's quad height
};
inline Layout layout(float W) {
    Layout L;
    if (!(W > 0.f) || !std::isfinite(W)) return L;
    const Cell f = cell(P_FRAME), p = cell(P_PLATE), d = cell(P_DOWN);
    L.W = W;
    L.fw = W / (k_open_x1 - k_open_x0); L.fh = L.fw * static_cast<float>(f.h) / static_cast<float>(f.w);
    L.hi = L.fh * (k_open_y1 - k_open_y0);
    L.fcx = (0.5f - 0.5f * (k_open_x0 + k_open_x1)) * L.fw;   // (the cell's y runs down: an opening below the cell's middle puts the frame's centre above it)
    L.fcy = (0.5f * (k_open_y0 + k_open_y1) - 0.5f) * L.fh;
    L.th = L.hi * (1.f + 2.f * k_track_pad);
    L.pw = L.fw * k_plate_w; L.ph = L.pw * static_cast<float>(p.h) / static_cast<float>(p.w); L.pcy = k_plate_dy * L.ph;
    L.ch = L.hi * k_cursor_h; L.cw = L.ch * static_cast<float>(d.w) / static_cast<float>(d.h);
    L.zh = L.hi * k_zone_h;
    return L;
}
/// A slice: the cell's columns p0..p1 drawn over x0..x1 (metres, local).
struct Seg { float x0 = 0.f, x1 = 0.f, p0 = 0.f, p1 = 0.f; };
/// A three-slice of the cell columns a..b over x0..x1 at height h (the cell's height cell_h maps to h): the left cap cl and
/// the right cap cr (columns) at their natural width (h / cell_h a column), the middle stretched; when x0..x1 is narrower than
/// both caps they share it in their proportion and the middle is left out. Returns the slices written (0-3), none empty.
inline int slice3(float x0, float x1, float h, int cell_h, float a, float b, float cl, float cr, Seg* out) {
    if (!(x1 > x0) || !(h > 0.f) || cell_h <= 0 || !(b > a) || !(cl >= 0.f) || !(cr >= 0.f) || cl + cr > b - a) return 0;
    const float k = h / static_cast<float>(cell_h), w = x1 - x0;
    float lw = cl * k, rw = cr * k;
    if (lw + rw > w) { const float s = w / (lw + rw); lw *= s; rw *= s; }
    const float eps = 1e-6f * (w > 1.f ? w : 1.f);
    int n = 0;
    if (lw > eps) { out[n].x0 = x0; out[n].x1 = x0 + lw; out[n].p0 = a; out[n].p1 = a + cl; ++n; }
    if (w - lw - rw > eps) { out[n].x0 = x0 + lw; out[n].x1 = x1 - rw; out[n].p0 = a + cl; out[n].p1 = b - cr; ++n; }
    if (rw > eps) { out[n].x0 = x1 - rw; out[n].x1 = x1; out[n].p0 = b - cr; out[n].p1 = b; ++n; }
    return n;
}
/// The opening's local x of a 0..1 bar position (odv::bar_x of a game value).
inline float bar_lx(const Layout& L, float x01) { return (x01 - 0.5f) * L.W; }
/// The golden zone over the game's zone x0..x1 (local x): its end bars exactly at x0 and x1 (the cell's columns k_zone_m_px
/// and w - k_zone_m_px), each bar's scrolls a cap (squeezed for a narrow zone), the glow outside the bars at its natural width.
/// out: room for 5. Returns the count (0 for an empty zone).
inline int zone_segs(const Layout& L, float x0, float x1, Seg* out) {
    const Cell c = cell(P_ZONE);
    if (!(x1 > x0) || !(L.zh > 0.f)) return 0;
    const float m = L.zh * static_cast<float>(k_zone_m_px) / static_cast<float>(c.h);
    const int k = slice3(x0, x1, L.zh, c.h, static_cast<float>(k_zone_m_px), static_cast<float>(c.w - k_zone_m_px), static_cast<float>(k_zone_cap_px), static_cast<float>(k_zone_cap_px), out + 1);
    if (k == 0) return 0;
    out[0].x0 = x0 - m; out[0].x1 = x0; out[0].p0 = 0.f; out[0].p1 = static_cast<float>(k_zone_m_px);
    Seg& r = out[k + 1]; r.x0 = x1; r.x1 = x1 + m; r.p0 = static_cast<float>(c.w - k_zone_m_px); r.p1 = static_cast<float>(c.w);
    return k + 2;
}
/// The empty track over the whole opening (its round caps at both ends). out: room for 3.
inline int empty_segs(const Layout& L, Seg* out) {
    const Cell c = cell(P_EMPTY);
    return slice3(-0.5f * L.W, 0.5f * L.W, L.th, c.h, 0.f, static_cast<float>(c.w), static_cast<float>(k_cap_px), static_cast<float>(k_cap_px), out);
}
/// The countdown's fill: the lit track from the opening's left end over the fraction f of its width (clamped to 0..1; NaN: none),
/// its round cap and bright leading edge at their natural width (squeezed together when shorter), the body stretched. out:
/// room for 3. Returns the count.
inline int fill_segs(const Layout& L, float f, Seg* out) {
    const float k = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    if (!(k > 0.f)) return 0;
    const Cell c = cell(P_FILL);
    return slice3(-0.5f * L.W, -0.5f * L.W + L.W * k, L.th, c.h, 0.f, static_cast<float>(c.w), static_cast<float>(k_cap_px), static_cast<float>(k_edge_px), out);
}
/// A local rectangle (y0 its bottom, y1 its top).
struct Rect { float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f; };
/// A cursor's quad at the bar position x01: centred on it; the down cursor's tip k_tip_in of the opening's height below the
/// opening's top, the up cursor's as far above its bottom (the classic pinch pair).
inline Rect cursor_rect(const Layout& L, float x01, bool down) {
    const float cx = bar_lx(L, x01);
    const float tip = down ? (0.5f - k_tip_in) * L.hi : (k_tip_in - 0.5f) * L.hi;
    const float top = tip + (down ? k_tip_down : k_tip_up) * L.ch;
    Rect r; r.x0 = cx - 0.5f * L.cw; r.x1 = cx + 0.5f * L.cw; r.y1 = top; r.y0 = top - L.ch;
    return r;
}
/// Lulu's countdown: the time left over the window's total (the game's shared timer, f32 0x133F790 / 0x133F78C, as od::Raw
/// reads them), 0..1; 0 for a total or a time left that is not positive (or NaN).
inline float countdown(float left, float total) {
    if (!(total > 0.f) || !(left > 0.f)) return 0.f;
    const float f = left / total;
    return f > 1.f ? 1.f : f;
}

// -- the readable-size guard --------------------------------------------------------------------------------------------
/// The opening never shows smaller than this angle at arm's length (k_guard_dist): about 18 px at the Quest 3's ~20 px a degree,
/// the cursors (1.45x) about 26 px. At the defaults (Tidus: a 22 cm opening 16 cm above the right hand, about 0.43 m from the eyes) the opening is
/// 1.14 cm, about 1.5 degrees (30 px; the frame 24.4 x 2.7 cm, about 630 px wide); Lulu's 16 cm countdown's is 0.83 cm, about
/// 1.1 degrees (at 0.6 m 0.8: the guard draws it x1.13); the guard grows a bar only at a small size setting.
constexpr float k_min_deg = 0.9f, k_max_grow = 3.f;
/// The distance the guard is judged at (review N8): a fixed arm's length, not the hand's distance now, so the bar's size never
/// follows the hand (Tidus's 22 cm needs nothing there; Lulu's 16 cm countdown is drawn x1.13, as if 18 cm).
constexpr float k_guard_dist = 0.6f;
/// The bar's place (round 4, the owner: "it should really appear straight out in front of us"): ini od_bar_place 0 in front of
/// you (the default), 1 above your hand (as before). In front: od_bar_dist metres out along the head's yaw (the default 0.6 m,
/// the guard's own arm's length), its pitch followed only within k_front_down below and k_front_up above the horizon, raised by
/// od_bar_height; the guard is then judged at that distance.
constexpr float k_front_dmin = 0.35f, k_front_dmax = 1.2f, k_front_hmax = 0.4f;
constexpr float k_front_down = 40.f * 0.0174532925f, k_front_up = 10.f * 0.0174532925f;   // (round 5, review N-T5: 40 down, so the bar stays in view looking down at the hands)
inline float front_dist(float d) { return std::isfinite(d) ? (d < k_front_dmin ? k_front_dmin : d > k_front_dmax ? k_front_dmax : d) : k_guard_dist; }
inline float front_height(float h) { return std::isfinite(h) ? (h < -k_front_hmax ? -k_front_hmax : h > k_front_hmax ? k_front_hmax : h) : 0.f; }
/// The head's pitch (radians, up positive) from its forward's y, clamped to the front band.
inline float front_pitch(float fw_y) {
    const float y = std::isfinite(fw_y) ? (fw_y < -1.f ? -1.f : fw_y > 1.f ? 1.f : fw_y) : 0.f;
    const float p = std::asin(y);
    return p < -k_front_down ? -k_front_down : p > k_front_up ? k_front_up : p;
}
/// A factor >= 1 on the bar's width so its opening (hi metres tall at d metres from the eye) is at least k_min_deg tall
/// (at most k_max_grow); 1 when it already is, or for odd inputs.
inline float readable_scale(float hi, float d) {
    if (!(hi > 0.f) || !(d > 0.f) || !std::isfinite(hi) || !std::isfinite(d)) return 1.f;
    const float need = 2.f * d * std::tan(0.5f * k_min_deg * 0.0174532925f);
    if (hi >= need) return 1.f;
    const float s = need / hi;
    return s > k_max_grow ? k_max_grow : s;
}

// -- the game's own limit UI (round 6: a state-based rule) ----------------------------------------------------------------
/// The game's Swordplay bar (0x897240, called at 0x89B416), the limit plates (0x8A1CE0 at 0x89B3B8) and Auron's Bushido row
/// (0x892740 at 0x89B411) are skipped by our call-site stubs, and the held window scenes ("btidu"; "bauro", "bauro2") are left
/// out of the scene draw (0x8AE2F0 at 0x89B3C3 and 0x89B513), whenever our replacement for that character is enabled. The
/// "enabled" flag is published each Present from settled facts (Gate). The character's own state word decides the rest, and each
/// stub reads it itself on the game's thread, so there is no frame of lag.
/// Round 6 (the root-cause report's fix 1; the owner: no hide ever worked): the earlier rule needed our art built in the last 3
/// Presents, a 0.25 s grace and the gestures injecting now, and any of those failing showed the game's bar. Now only settings and
/// standing facts count; the one fact about our bar being placeable is a robust, current one (the right grip tracked, when the
/// bar sits above the hand).
struct Gate {
    bool on = false;      // the setting (od_bar_hide2d; the row: od_targets_hide2d, its patch made at launch)
    bool battle = false;  // a battle is running
    bool view = false;    // the headset shows the 3D view (not a screen, not suspended)
    bool live = false;    // the headset session runs and the VR pad is merged (our bar and the targets are drawn only then)
    bool cfg = false;     // our replacement is on: Tidus's swing gesture (gestures on), or Auron's "strike the targets"
    bool vis = false;     // Tidus: the overdrive visuals on (our bar is one of them); Auron: not a Practice round
    bool anchor = false;  // Tidus: our bar can be placed (in front, or above a tracked right grip); Auron: always
};
/// Why the game's UI showed: bits, named in k_why_name.
enum : uint32_t { W_OFF = 1u, W_BATTLE = 2u, W_VIEW = 4u, W_LIVE = 8u, W_CFG = 16u, W_VIS = 32u, W_ANCHOR = 64u, W_FRESH = 128u, W_OTHER = 256u };
constexpr int k_why_n = 9;
constexpr const char* k_why_name[k_why_n] = { "hide off", "no battle", "not the 3D view", "headset session or VR pad not live",
    "our replacement off (Tidus's swing / Auron's targets mode)", "Tidus: the visuals off / Auron: a Practice round",
    "right grip untracked (bar above the hand)", "the state not read fresh", "another overdrive" };
inline uint32_t gate_miss(const Gate& g) {
    uint32_t w = 0u;
    if (!g.on) w |= W_OFF;
    if (!g.battle) w |= W_BATTLE;
    if (!g.view) w |= W_VIEW;
    if (!g.live) w |= W_LIVE;
    if (!g.cfg) w |= W_CFG;
    if (!g.vis) w |= W_VIS;
    if (!g.anchor) w |= W_ANCHOR;
    return w;
}
inline bool enabled(const Gate& g) { return gate_miss(g) == 0u; }
/// A limit window's states: 1-10 (0 none).
inline bool active(int s) { return s >= 1 && s <= 10; }
/// The stubs' own rule (the game's thread): the flag and the word the stub reads (the bar, the plates, the row).
inline bool stub_hides(bool enabled_now, int word) { return enabled_now && active(word); }
/// The held scenes: from state 3 on only. A scene's draw also steps its clock, and the window's state 2 waits for the intro's
/// end, so skipping it in states 1-2 would stall the window. From state 3 on the intro has ended (btidu, bauro), or nothing
/// waits for the scene (bauro2), until states 6 / 8 stop them.
inline bool scene_hides(bool enabled_now, int word) { return enabled_now && word >= 3 && word <= 10; }
/// The Present's decision (the window line's; the stubs decide by their own read): enabled, the state read fresh and active, no
/// other overdrive.
inline bool hide_now(const Gate& g, bool fresh, int own, bool other) { return enabled(g) && fresh && active(own) && !other; }
/// Why a Present in state 3 or 4 showed the game's UI (0 when hidden or outside 3 and 4).
inline uint32_t why_shown(const Gate& g, bool fresh, int own, bool other) {
    if (hide_now(g, fresh, own, other) || (own != 3 && own != 4)) return 0u;
    return gate_miss(g) | (fresh ? 0u : W_FRESH) | (other ? W_OTHER : 0u);
}
/// A scene slot's name (char[20] at slot +8, as TOSeqStartScene copies it) equals want, terminator included.
inline bool name_is(const volatile char* nm, const char* want) {
    for (int i = 0; i < 20; ++i) { if (nm[i] != want[i]) return false; if (want[i] == 0) return true; }
    return false;
}
/// Frames a drawn bar's image (the layer) stays valid without a new build (a 3D frame without the game's UI pass builds nothing).
constexpr uint32_t k_built_presents = 3;

// -- the window lines (round 4: the owner saw the game's Swordplay UI exactly as without the mod) -----------------------------
/// One limit window as the hide saw it, from the character's state leaving 0 to its return (render thread, one step a Present).
/// Presents by state: [0] 1-2, [1] 3, [2] 4, [3] 5-10, each [shown, hidden]; the stubs' counts are the glue's snapshots.
struct Win {
    bool open = false; double t0 = 0.0;
    uint32_t pres[4][2] = {}, unread = 0, presents = 0, ui_pass = 0, why_n[k_why_n] = {}, lay = 0;   // lay: Presents with the bar's own layer up (round 5)
    uint32_t snap[12] = {};   // the glue's counters at the open
    /// Opens or closes: 1 opened (snapshot now), 2 closed (write the line), 0 neither. A stale read neither opens nor closes.
    int step(bool battle, bool fresh, int state, double t) {
        if (open && !battle) { open = false; return 2; }
        if (!fresh) { if (open) { unread++; presents++; } return 0; }
        const bool in = active(state);
        if (!open && battle && in) { *this = Win{}; open = true; t0 = t; return 1; }
        if (open && !in) { open = false; return 2; }
        return 0;
    }
    /// This Present's decision (after step, while open and fresh).
    void note(int state, bool hidden, uint32_t why, bool ui, bool lay_up = false) {
        if (!open) return;
        presents++; if (ui) ui_pass++; if (lay_up) lay++;
        const int b = state <= 2 ? 0 : state == 3 ? 1 : state == 4 ? 2 : 3;
        pres[b][hidden ? 1 : 0]++;
        for (int i = 0; i < k_why_n; ++i) if (why & (1u << i)) why_n[i]++;
    }
};
/// The window's verdict (the questions of the owner's test, in order), into out (n bytes). site: 1 the call still points at our
/// stub, 0 rewritten by something else, -1 never patched. Round 5 (review S-T1): proportions, not "any". forced: the Developer
/// switch made the stub skip every call in 3 / 4 whatever the decision. row: Auron's Bushido row (0x892740 at 0x89B411) instead
/// of Tidus's bar (0x897240 at 0x89B416). Returns out.
constexpr uint32_t k_most_num = 8, k_most_den = 10;   // "most": at least 80 %
inline bool most(uint32_t part, uint32_t whole) { return whole > 0 && static_cast<uint64_t>(part) * k_most_den >= static_cast<uint64_t>(whole) * k_most_num; }
inline const char* verdict(char* out, size_t n, int site, uint32_t calls34, uint32_t skipped34, uint32_t hidden34, uint32_t read34, bool forced = false, bool row = false) {
    if (out == nullptr || n == 0) return "";
    const unsigned c = calls34, k = skipped34, h = hidden34, r = read34;
    const char* who = row ? "Auron's" : "Tidus's"; const char* what = row ? "row" : "bar"; const char* at = row ? "0x89B411" : "0x89B416"; const char* fn = row ? "0x892740" : "0x897240";
    if (site < 0) snprintf(out, n, "the call site was never patched: the game's %s stays (see the patch line at launch)", what);
    else if (site == 0) snprintf(out, n, "the call site NO LONGER calls our stub (something rewrote it after launch): nothing of ours runs there");
    else if (c == 0) snprintf(out, n, "the stub was NEVER called in %s states 3 and 4: the battle UI did not reach %s while the game's %s was up (it is drawn some other way)", who, at, what);
    else if (forced && most(k, c)) snprintf(out, n, "FORCED: the stub skipped %u of %u calls in states 3 and 4: if the game's %s still showed, it is not drawn by %s", k, c, what, fn);
    else if (forced) snprintf(out, n, "FORCED, yet the stub skipped only %u of %u calls in states 3 and 4 (its own read of %s state)", k, c, who);
    else if (r == 0) snprintf(out, n, "%s states 3 and 4 were never read fresh at a Present: no decision was made", who);
    else if (h == 0) snprintf(out, n, "the decision said SHOW at every Present of states 3 and 4 (the reasons above)");
    else if (!most(h, r)) snprintf(out, n, "the decision said SHOW at most Presents of states 3 and 4 (hidden %u of %u; the reasons above)", h, r);
    else if (k == 0) snprintf(out, n, "the decision said hide (%u of %u Presents) but the stub never skipped (the flag did not reach the game's thread in time)", h, r);
    else if (!most(k, c)) snprintf(out, n, "the decision hid %u of %u Presents but the stub skipped only %u of %u calls (the flag late on the game's thread)", h, r, k, c);
    else snprintf(out, n, "the stub skipped %u of %u calls: if the game's %s still showed, it is drawn by something other than %s (or the panel showed an older UI image)", k, c, what, fn);
    return out;
}

}  // namespace odb
