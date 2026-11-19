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
/// The opening never shows smaller than this angle at the eye: about 18 px at the Quest 3's ~20 px a degree, the cursors (1.45x)
/// about 26 px. At the defaults (Tidus: a 22 cm opening 16 cm above the right hand, about 0.43 m from the eyes) the opening is
/// 1.14 cm, about 1.5 degrees (30 px; the frame 24.4 x 2.7 cm, about 630 px wide); Lulu's 16 cm countdown's is 0.83 cm, about
/// 1.1 degrees: the guard acts only at a small size or with the bar far out (Lulu's at 0.6 m: x1.13).
constexpr float k_min_deg = 0.9f, k_max_grow = 3.f;
/// A factor >= 1 on the bar's width so its opening (hi metres tall at d metres from the eye) is at least k_min_deg tall
/// (at most k_max_grow); 1 when it already is, or for odd inputs.
inline float readable_scale(float hi, float d) {
    if (!(hi > 0.f) || !(d > 0.f) || !std::isfinite(hi) || !std::isfinite(d)) return 1.f;
    const float need = 2.f * d * std::tan(0.5f * k_min_deg * 0.0174532925f);
    if (hi >= need) return 1.f;
    const float s = need / hi;
    return s > k_max_grow ? k_max_grow : s;
}

// -- the game's Swordplay window ----------------------------------------------------------------------------------------
/// The game's own Swordplay bar (0x897240, drawn by the battle UI at 0x89B416 while Tidus's state 0x133D734 is 3 or 4) hidden:
/// with F8 "hide the game's Swordplay bar" (ini od_bar_hide2d, the call-site patch made at launch) while our art bar is drawn
/// in the headset, and from state 3 (the window pending) when it is coming: the visuals and the bar art on and loaded, Tidus's
/// swing gesture able to inject (the gestures on, the headset, the VR pad), a battle, the 3D view, no other overdrive.
struct HideIn {
    bool on = false, drawn = false, coming = false;   // the setting; our art bar drawn this Present; it will be drawn when the window opens
    bool battle = false, fresh = false, gate = false, view = false;
    int auron = 0, lulu = 0, tidus = 0, wakka = 0;
};
inline bool hide_tidus(const HideIn& x) {
    if (!x.on) return false;
    if (x.drawn) return true;
    return x.coming && x.battle && x.fresh && x.gate && x.view && x.tidus == 3 && x.auron == 0 && x.lulu == 0 && x.wakka == 0;
}

}  // namespace odb
