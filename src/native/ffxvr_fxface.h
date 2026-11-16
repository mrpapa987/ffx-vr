// SPDX-License-Identifier: MIT
// ffxvr_fxface.h -- battle-vis build 2 (branch battle-vis2, ABI 69; design-notes "Battle visibility, build 2"): aiming the
// game's effect sprites at the eyes (ini fx_face_eyes, default 0). Pure logic: no Windows, no D3D, no file. The native glue
// (ffxvr_native.cpp, "fx_face_eyes") gives it the head's pose and the game's six effect matrices; a unit test checks the rule.
//
// What the game does (ffx-rust's symbols/ffx.sym and docs/formats/vfx.md, render.md, static research of the same FFX.exe; the
// names are theirs or Fahrenheit's; UNCONFIRMED by us unless the owner's recording is named):
//   The PS2 effect code ("ppp") runs one effect part at a time: its host sets the part's world matrix (pppSetMatrix), then
//     g_pppViewMatrix   (0x230FF20) = the part's world matrix x g_pppCameraMatrix (0x230FF60)   (the recording's fx: samples
//     g_pppViewMatrixCopy (0x230FE20) = a copy of it                                             agree: view = world x camera)
//   and calls pppRunPart (0x712250, cdecl (part, flags); Fahrenheit's own call table has it). The part's programs place
//   their objects in the game camera's view space through those two, build each sprite as a quad in that space (so it faces
//   the game's camera), and hand the result to the renderer with g_pppFillMatrix (0x230FCA0) = the inverse of the game's
//   camera, which brings it back to the world (the mod's "product-only" draws: the quads are already in the world when
//   Direct3D gets them, so nothing can re-aim them at draw time). g_pppFillMatrix2 / 3 (0x230FC20 / 0x230FBE0) take a
//   view-space vertex to clip space and back (the z-matrix shapes' clip test).
//   Matrices are 4 x 4 floats, row vectors: a point p goes to p x M. View space: +Z forward, +Y down, X = Y cross Z. Game
//   space has +Y down too (the mod's render space is game space with x and y negated).
//
// The rule (substitute): around one pppRunPart call, with F = g_pppFillMatrix as it is at that moment and S = a level camera
// on the line from the head (the centre of the two eyes) to the part, looking at the part from as far away as the part is
// deep in the game's own view:
//     G = F x S
//     camera, view, view copy  ->  each x G        (what the programs read)
//     fill                     ->  the inverse of S
//     fill2                    ->  the inverse of G x fill2,   fill3 -> fill3 x G
// and all six are put back after the call. Everything the programs compute linearly through the view comes out where the game
// puts it (view x G x inverse(S) = view x F, whatever F is: no assumption that F is exactly the inverse of the camera in
// use), so nothing moves; only what is built in view space (a sprite's corners, a ribbon's width) is now built in the
// substituted view space, and so faces the head. Nothing outside the call sees a substituted matrix: the game's other
// effect systems, which share g_pppFillMatrix but keep their own copies of the camera, are not touched. Inside the call
// one thing is not covered (review S2): code of the part that takes the game's camera by another route (MsCopyCameraMatrix,
// which the magic files import) and builds a view-space position from it hands that to a fill request that now carries
// inverse(S), and lands displaced. The glue counts those calls; they are not answered.
// S looks from the head's side at the part (not along the head's own forward): the sprites then do not turn when the head
// turns, only when it moves, and both eyes get the same quads. Its roll is removed (its Y axis is the world's down as far
// as its forward allows): a vertical beam stays vertical when the head tilts. Its distance to the part is the part's
// depth in the game's own view (the view's translation z, which is what the programs see), not the head's (review S1): what a program derives from view depth (a sprite sized on the screen, a depth
// fade, a lens flare) then keeps the value the game computed; only the direction is the head's.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>

namespace fxf {

constexpr int k_save = 6 * 16;      // floats: the six matrices as they were (camera, view, view copy, fill, fill2, fill3)
constexpr float k_close = 2.f;      // game units: a part nearer the head than this has no direction to look along
constexpr float k_depth = 0.01f;    // game units: a view depth of the part below this is not used as S's distance (the part is at or behind the camera's plane)
constexpr float k_level = 0.1f;     // the sine of the smallest angle between the forward and the vertical that still gives a level basis

/// out = a x b (row vectors: a point goes through a, then b). out may be a or b.
inline void mul(float* out, const float* a, const float* b) {
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
    memcpy(out, r, sizeof(r));
}
inline bool finite16(const float* m) { for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return false; return true; }
/// The fourth column is (0, 0, 0, 1): rotation, scale and shear with a translation in row 3.
inline bool affine(const float* m) { return std::fabs(m[3]) < 1e-4f && std::fabs(m[7]) < 1e-4f && std::fabs(m[11]) < 1e-4f && std::fabs(m[15] - 1.f) < 1e-4f; }
/// The inverse of an affine matrix (affine(m), finite, its 3 x 3 part not flat). out must not be m.
inline bool inverse_affine(float* out, const float* m) {
    if (!finite16(m) || !affine(m)) return false;
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], k = m[10];
    const double det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
    const double size = std::sqrt(a * a + b * b + c * c) * std::sqrt(d * d + e * e + f * f) * std::sqrt(g * g + h * h + k * k);
    if (!(size > 1e-18) || !(std::fabs(det) > 1e-6 * size)) return false;
    const double q = 1.0 / det;
    const double i00 = (e * k - f * h) * q, i01 = (c * h - b * k) * q, i02 = (b * f - c * e) * q;
    const double i10 = (f * g - d * k) * q, i11 = (a * k - c * g) * q, i12 = (c * d - a * f) * q;
    const double i20 = (d * h - e * g) * q, i21 = (b * g - a * h) * q, i22 = (a * e - b * d) * q;
    const double tx = m[12], ty = m[13], tz = m[14];
    out[0] = static_cast<float>(i00); out[1] = static_cast<float>(i01); out[2]  = static_cast<float>(i02); out[3]  = 0.f;
    out[4] = static_cast<float>(i10); out[5] = static_cast<float>(i11); out[6]  = static_cast<float>(i12); out[7]  = 0.f;
    out[8] = static_cast<float>(i20); out[9] = static_cast<float>(i21); out[10] = static_cast<float>(i22); out[11] = 0.f;
    out[12] = static_cast<float>(-(tx * i00 + ty * i10 + tz * i20));
    out[13] = static_cast<float>(-(tx * i01 + ty * i11 + tz * i21));
    out[14] = static_cast<float>(-(tx * i02 + ty * i12 + tz * i22));
    out[15] = 1.f;
    return finite16(out);
}
inline float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline void cross3(float* o, const float* a, const float* b) {
    const float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
/// Normalises v in place; false (v left) when it has no length.
inline bool norm3(float* v) {
    const float l = std::sqrt(dot3(v, v));
    if (!(l > 1e-6f) || !std::isfinite(l)) return false;
    v[0] /= l; v[1] /= l; v[2] /= l;
    return true;
}
inline float angle_deg3(const float* a, const float* b) {
    float x[3] = { a[0], a[1], a[2] }, y[3] = { b[0], b[1], b[2] };
    if (!norm3(x) || !norm3(y)) return -1.f;
    float c = dot3(x, y);
    c = c > 1.f ? 1.f : c < -1.f ? -1.f : c;
    return std::acos(c) * 57.29578f;
}

/// The head in game space: where it is, where it looks, its up.
struct Pose { float pos[3] = {}, fwd[3] = {}, up[3] = {}; };
/// The centre eye, from the two views the mod draws the eyes with (each 12 floats, the mod's own form: three rows of a
/// rotation with the translation in column 3, the camera looks along -Z, render space = game space with x and y negated).
/// One pose for both eyes: the quads the effect code builds are the same for both, as a flat sprite's have to be.
inline bool centre_eye(const float* e1, const float* e2, Pose* o) {
    if (e1 == nullptr || e2 == nullptr || o == nullptr) return false;
    const float* e[2] = { e1, e2 };
    float p[3] = {}, f[3] = {}, u[3] = {};
    for (int k = 0; k < 2; ++k) {
        const float* v = e[k];
        for (int i = 0; i < 12; ++i) if (!std::isfinite(v[i])) return false;
        for (int i = 0; i < 3; ++i) {
            p[i] += 0.5f * -(v[0 + i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
            f[i] += -v[8 + i];
            u[i] += v[4 + i];
        }
    }
    if (!norm3(f) || !norm3(u)) return false;
    const float sgn[3] = { -1.f, -1.f, 1.f };   // render space to game space
    for (int i = 0; i < 3; ++i) { o->pos[i] = sgn[i] * p[i]; o->fwd[i] = sgn[i] * f[i]; o->up[i] = sgn[i] * u[i]; }
    return true;
}

/// The substituted camera's axes and a first place: S (the game's form: row vectors, columns X, Y = down, Z = forward; a
/// point p is p x S in view space) at the head, looking at c (game space), level. substitute() then moves it along its
/// forward to the game camera's distance (place_behind). *how: 0 at the part, level; 1 the part is at the head (within
/// k_close): the head's own forward, level; 2 the forward is within about 6 degrees of the vertical: the head's own up
/// decides the roll. false: no basis (the forward and the head's up are parallel, or a value is not finite).
inline bool camera_for(const Pose& h, const float* c, float* S, int* how) {
    float f[3] = { c[0] - h.pos[0], c[1] - h.pos[1], c[2] - h.pos[2] };
    int w = 0;
    if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) return false;
    if (!(dot3(f, f) > k_close * k_close)) { f[0] = h.fwd[0]; f[1] = h.fwd[1]; f[2] = h.fwd[2]; w = 1; }
    if (!norm3(f)) return false;
    const float down[3] = { 0.f, 1.f, 0.f };   // game space: +Y is down
    float x[3]; cross3(x, down, f);
    if (!(dot3(x, x) > k_level * k_level)) {
        const float hd[3] = { -h.up[0], -h.up[1], -h.up[2] };   // the head's own down
        cross3(x, hd, f); w = 2;
        if (!(dot3(x, x) > k_level * k_level)) return false;
    }
    if (!norm3(x)) return false;
    float y[3]; cross3(y, f, x);
    if (!norm3(y)) return false;
    for (int i = 0; i < 3; ++i) { S[i * 4 + 0] = x[i]; S[i * 4 + 1] = y[i]; S[i * 4 + 2] = f[i]; S[i * 4 + 3] = 0.f; }
    S[12] = -dot3(h.pos, x); S[13] = -dot3(h.pos, y); S[14] = -dot3(h.pos, f); S[15] = 1.f;
    if (how != nullptr) *how = w;
    return finite16(S);
}

/// S moved along its own forward so that c (game space) is straight ahead of it, `back` units away; its axes stay.
/// false: back is negative or not finite.
inline bool place_behind(float* S, const float* c, float back) {
    if (!std::isfinite(back) || !(back >= 0.f)) return false;
    const float x[3] = { S[0], S[4], S[8] }, y[3] = { S[1], S[5], S[9] }, f[3] = { S[2], S[6], S[10] };
    const float p[3] = { c[0] - f[0] * back, c[1] - f[1] * back, c[2] - f[2] * back };
    S[12] = -dot3(p, x); S[13] = -dot3(p, y); S[14] = -dot3(p, f);
    return finite16(S);
}

enum { R_OK = 1,
       R_NOPOSE = 0,      // no head pose is published (not a battle drawn in VR, the setting is off)
       R_COPY = -1,       // the view's copy is not a copy of the view: this host uses it for something else, left alone
       R_SINGULAR = -2,   // a matrix is not finite, not affine, or does not invert
       R_BASIS = -3,      // no camera could be built (camera_for)
       R_NESTED = -4,     // inside a re-aimed run already (the outer run's matrices are in place)
       R_NULL = -5 };
/// What one substitution did, for the log (game space).
struct Last { float part[3] = {}, cam_pos[3] = {}, cam_fwd[3] = {}, head[3] = {}, look[3] = {}, eye[3] = {}; float off_deg = 0.f, dist = 0.f; int how = 0; };   // eye: where S sits; dist: its distance to the part = the part's depth in the game's view

/// Before one pppRunPart call (the game's thread): the six matrices in place (the game's own statics), as the rule above.
/// save gets them as they were (k_save floats) when R_OK is returned; any other result changed nothing.
inline int substitute(float* cam, float* view, float* copy, float* fill, float* fill2, float* fill3, const Pose& h, float* save, Last* last) {
    if (cam == nullptr || view == nullptr || copy == nullptr || fill == nullptr || fill2 == nullptr || fill3 == nullptr || save == nullptr) return R_NULL;
    if (memcmp(view, copy, 16 * sizeof(float)) != 0) return R_COPY;
    if (!finite16(cam) || !finite16(view) || !affine(view)) return R_SINGULAR;
    float Finv[16];
    if (!inverse_affine(Finv, fill)) return R_SINGULAR;
    // the part's place in the world: the view's origin, brought back as the game's renderer will bring it back
    const float c[3] = { view[12] * fill[0] + view[13] * fill[4] + view[14] * fill[8] + fill[12],
                         view[12] * fill[1] + view[13] * fill[5] + view[14] * fill[9] + fill[13],
                         view[12] * fill[2] + view[13] * fill[6] + view[14] * fill[10] + fill[14] };
    float S[16], Sinv[16]; int how = 0;
    if (!camera_for(h, c, S, &how)) return R_BASIS;
    // S1: the head's direction, the game's depth. S looks straight at the part, so the part's depth under S is S's distance
    // to it: that distance is the part's depth in the game's own view (view[14]: what the programs see, also with a stale
    // fill matrix or a parent folded into the camera), so every value derived from view depth stays the game's. A part at
    // or behind the game camera's plane has no such depth: the straight-line distance to the game camera then (the fill
    // matrix's row 3 is its place).
    const float gd[3] = { c[0] - fill[12], c[1] - fill[13], c[2] - fill[14] };
    const float back = view[14] > k_depth ? view[14] : std::sqrt(dot3(gd, gd));
    if (!place_behind(S, c, back)) return R_BASIS;
    if (!inverse_affine(Sinv, S)) return R_BASIS;
    float G[16], Ginv[16];
    mul(G, fill, S); mul(Ginv, Sinv, Finv);
    if (!finite16(G) || !finite16(Ginv)) return R_SINGULAR;
    memcpy(save, cam, 16 * sizeof(float)); memcpy(save + 16, view, 16 * sizeof(float)); memcpy(save + 32, copy, 16 * sizeof(float));
    memcpy(save + 48, fill, 16 * sizeof(float)); memcpy(save + 64, fill2, 16 * sizeof(float)); memcpy(save + 80, fill3, 16 * sizeof(float));
    if (last != nullptr) {
        for (int i = 0; i < 3; ++i) { last->part[i] = c[i]; last->cam_pos[i] = fill[12 + i]; last->cam_fwd[i] = fill[8 + i]; last->head[i] = h.pos[i]; last->look[i] = S[i * 4 + 2]; }
        norm3(last->cam_fwd);
        for (int i = 0; i < 3; ++i) last->eye[i] = Sinv[12 + i];
        last->off_deg = angle_deg3(last->cam_fwd, last->look); last->how = how; last->dist = back;
    }
    mul(cam, cam, G);
    mul(view, view, G);
    memcpy(copy, view, 16 * sizeof(float));
    memcpy(fill, Sinv, 16 * sizeof(float));
    if (finite16(fill2)) mul(fill2, Ginv, fill2);
    if (finite16(fill3)) mul(fill3, fill3, G);
    return R_OK;
}
/// After that call: the six matrices as they were.
inline void restore(float* cam, float* view, float* copy, float* fill, float* fill2, float* fill3, const float* save) {
    if (cam == nullptr || view == nullptr || copy == nullptr || fill == nullptr || fill2 == nullptr || fill3 == nullptr || save == nullptr) return;
    memcpy(cam, save, 16 * sizeof(float)); memcpy(view, save + 16, 16 * sizeof(float)); memcpy(copy, save + 32, 16 * sizeof(float));
    memcpy(fill, save + 48, 16 * sizeof(float)); memcpy(fill2, save + 64, 16 * sizeof(float)); memcpy(fill3, save + 80, 16 * sizeof(float));
}

/// The counters (the game's thread writes, the render thread reads for the log and F8).
struct Stats { uint32_t runs = 0, applied = 0, no_pose = 0, copy_differs = 0, singular = 0, no_basis = 0, nested = 0; };
inline void count(Stats& s, int r) {
    s.runs++;
    switch (r) {
        case R_OK:       s.applied++; break;
        case R_NOPOSE:   s.no_pose++; break;
        case R_COPY:     s.copy_differs++; break;
        case R_SINGULAR: s.singular++; break;
        case R_BASIS:    s.no_basis++; break;
        case R_NESTED:   s.nested++; break;
        default:         s.singular++; break;
    }
}

}  // namespace fxf
