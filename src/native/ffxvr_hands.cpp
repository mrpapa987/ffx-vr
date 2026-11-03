// SPDX-License-Identifier: MIT
//
// VR hands v2, the CPU half: the S1 box mesh, rigid skinning, the grip -> hand placement and the calibration capture; S2's
// pack loader (cgltf, WIC), mirroring, the controller-driven finger pose and the forward kinematics for GPU skinning.
// See ffxvr_hands.h for the frames and conventions. No D3D, no globals of ffxvr_native.cpp.
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS   // cgltf's implementation names fopen / strncpy / strcpy (its file callbacks are never used: we parse from memory); /sdl would make C4996 an error
#endif
#include "ffxvr_hands.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>   // WIC: PNG / JPEG decode for pack textures (part of Windows: windowscodecs.lib, ole32.lib)
#endif

// cgltf 1.15 (MIT, Copyright (c) 2018-2021 Johannes Kuhlmann; third_party/cgltf/LICENSE, THIRD-PARTY-NOTICES), vendored
// unmodified. The implementation is compiled here only; third-party code, so its warnings are not ours.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996 4244 4267 4018 4100 4456 4706)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#define CGLTF_IMPLEMENTATION
#include "third_party/cgltf/cgltf.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

void hv_quat_to_mat(const float q[4], float M[9]) {   // the same as ffxvr_native.cpp's quat_to_mat: columns = the rotated axes
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    M[0] = 1 - 2*(y*y + z*z); M[1] = 2*(x*y - w*z);     M[2] = 2*(x*z + w*y);
    M[3] = 2*(x*y + w*z);     M[4] = 1 - 2*(x*x + z*z); M[5] = 2*(y*z - w*x);
    M[6] = 2*(x*z - w*y);     M[7] = 2*(y*z + w*x);     M[8] = 1 - 2*(x*x + y*y);
}
void hv_mat3_mul(const float* A, const float* B, float* O) {
    float t[9];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) t[i*3+j] = A[i*3+0]*B[0*3+j] + A[i*3+1]*B[1*3+j] + A[i*3+2]*B[2*3+j];
    memcpy(O, t, sizeof(t));
}
void hv_mat3_T(const float* A, float* O) {
    float t[9]; for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) t[i*3+j] = A[j*3+i];
    memcpy(O, t, sizeof(t));
}
void hv_mat3_vec(const float* A, const float* v, float* o) {
    const float x = v[0], y = v[1], z = v[2];
    for (int i = 0; i < 3; ++i) o[i] = A[i*3+0]*x + A[i*3+1]*y + A[i*3+2]*z;
}
static float v3dot(const float* a, const float* b) { return a[0]*b[0] + a[1]*b[1] + a[2]*b[2]; }
static void v3cross(const float* a, const float* b, float* o) { const float x = a[1]*b[2] - a[2]*b[1], y = a[2]*b[0] - a[0]*b[2], z = a[0]*b[1] - a[1]*b[0]; o[0] = x; o[1] = y; o[2] = z; }
static bool v3norm(float* v) {
    const float l = sqrtf(v3dot(v, v));
    if (!(l > 1e-6f) || !std::isfinite(l)) return false;
    v[0] /= l; v[1] /= l; v[2] /= l; return true;
}
static void rot_x(float a, float* R) { const float c = cosf(a), s = sinf(a); const float M[9] = { 1, 0, 0,  0, c, -s,  0, s, c }; memcpy(R, M, sizeof(M)); }
static void rot_y(float a, float* R) { const float c = cosf(a), s = sinf(a); const float M[9] = { c, 0, s,  0, 1, 0,  -s, 0, c }; memcpy(R, M, sizeof(M)); }
static void rot_z(float a, float* R) { const float c = cosf(a), s = sinf(a); const float M[9] = { c, -s, 0,  s, c, 0,  0, 0, 1 }; memcpy(R, M, sizeof(M)); }
static const float k_deg = 0.0174532925f;

bool hv_orthonormalize(float R[9]) {
    float x[3] = { R[0], R[3], R[6] }, y[3] = { R[1], R[4], R[7] };
    if (!v3norm(x)) return false;
    const float d = v3dot(x, y); for (int i = 0; i < 3; ++i) y[i] -= d * x[i];
    if (!v3norm(y)) return false;
    float z[3]; v3cross(x, y, z);
    for (int i = 0; i < 3; ++i) { R[i*3+0] = x[i]; R[i*3+1] = y[i]; R[i*3+2] = z[i]; }
    return true;
}
bool hv_is_rotation(const float R[9], float tol) {
    for (int i = 0; i < 9; ++i) if (!std::isfinite(R[i])) return false;
    float Rt[9], I[9]; hv_mat3_T(R, Rt); hv_mat3_mul(Rt, R, I);
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) if (fabsf(I[i*3+j] - (i == j ? 1.f : 0.f)) > tol) return false;
    const float det = R[0]*(R[4]*R[8] - R[5]*R[7]) - R[1]*(R[3]*R[8] - R[5]*R[6]) + R[2]*(R[3]*R[7] - R[4]*R[6]);
    return det > 0.f;
}
float hv_rot_angle_deg(const float A[9], const float B[9]) {
    float At[9], D[9]; hv_mat3_T(A, At); hv_mat3_mul(At, B, D);
    float c = (D[0] + D[4] + D[8] - 1.f) * 0.5f; c = c > 1.f ? 1.f : c < -1.f ? -1.f : c;
    return acosf(c) / k_deg;
}

void hv_xform_mul(const HvXform& a, const HvXform& b, HvXform* o) {
    HvXform r;
    hv_mat3_mul(a.R, b.R, r.R);
    hv_mat3_vec(a.R, b.t, r.t); for (int i = 0; i < 3; ++i) r.t[i] += a.t[i];
    *o = r;
}

void hv_default_grip_to_hand(int h, HvXform* out) {
    float Rx[9], Rz[9];
    rot_x(-70.f * k_deg, Rx);                          // Meta's palm pose in the grip frame (both hands: a rotation about X is its own mirror)
    rot_z((h == 0 ? 90.f : -90.f) * k_deg, Rz);        // its +Y (thumb side) and X (palm normal) -> the joint frame's +Y dorsal
    hv_mat3_mul(Rx, Rz, out->R);
    out->t[0] = (h == 0 ? -0.010f : 0.010f); out->t[1] = 0.013f; out->t[2] = 0.0075f;
}

void hv_trim(int h, const float rad[3], const float off_m[3], HvXform* out) {
    const float m = (h == 0) ? -1.f : 1.f;              // the left hand is the X mirror: rotations about Y and Z, and the X offset, flip
    float Ry[9], Rx[9], Rz[9], t[9];
    rot_y(rad[0] * m, Ry); rot_x(rad[1], Rx); rot_z(rad[2] * m, Rz);
    hv_mat3_mul(Rx, Rz, t); hv_mat3_mul(Ry, t, out->R);
    out->t[0] = off_m[0] * m; out->t[1] = off_m[1]; out->t[2] = off_m[2];
}

bool hv_cal_capture(int h, const float grip_R[9], const float grip_p[3], bool other_valid, const float other_p[3],
                    const float head_p[3], float head_yaw, float out_R[9]) {
    const float sx = (h == 0) ? -0.17f : 0.17f, sy = -0.13f, sz = 0.04f;   // the old virtual shoulder (beside and below the eyes)
    const float c = cosf(head_yaw), s = sinf(head_yaw);
    const float S[3] = { head_p[0] + c * sx + s * sz, head_p[1] + sy, head_p[2] - s * sx + c * sz };   // Ry(yaw) . (sx, sy, sz)
    float f[3] = { grip_p[0] - S[0], grip_p[1] - S[1], grip_p[2] - S[2] };
    if (!v3norm(f)) return false;
    float t[3];
    if (other_valid) { for (int i = 0; i < 3; ++i) t[i] = other_p[i] - grip_p[i]; }
    else { const float sg = (h == 0) ? 1.f : -1.f; t[0] = sg * c; t[1] = 0.f; t[2] = -sg * s; }   // one controller: Ry(yaw) (+-1, 0, 0), the head's right (left hand) / left
    { const float d = v3dot(t, f); for (int i = 0; i < 3; ++i) t[i] -= d * f[i]; }
    if (!v3norm(t)) return false;
    // The target hand frame: Z = -f (proximal), X = -t for the right hand (the thumb, toward the other hand, is -X there) and
    // +t for the left, Y = Z x X (dorsal: up for palms down).
    const float Z[3] = { -f[0], -f[1], -f[2] };
    float X[3]; for (int i = 0; i < 3; ++i) X[i] = (h == 0) ? t[i] : -t[i];
    float Y[3]; v3cross(Z, X, Y);
    float Rt[9]; for (int i = 0; i < 3; ++i) { Rt[i*3+0] = X[i]; Rt[i*3+1] = Y[i]; Rt[i*3+2] = Z[i]; }
    float GT[9]; hv_mat3_T(grip_R, GT);
    hv_mat3_mul(GT, Rt, out_R);                       // Cal = Rg^T Rt: Rg Cal is the target
    return hv_orthonormalize(out_R);
}

// -- the box mesh ------------------------------------------------------------------------------------------------------

static uint32_t rgba(float r, float g, float b) {
    auto q = [](float x) { const int v = (int)(x * 255.f + 0.5f); return (uint32_t)(v < 0 ? 0 : v > 255 ? 255 : v); };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (255u << 24);   // R8G8B8A8_UNORM: r in the low byte
}
/// One axis-aligned box (centre c, half extents e), 36 vertices, counter-clockwise from outside: for the face with normal
/// s e_a, the tangents (u, v) are chosen with u x v = s e_a, and the quad's corners go round (-,-) (+,-) (+,+) (-,+).
static int add_box(HvSrcVertex* out, int at, int max, const float c[3], const float e[3], uint32_t col, int bone) {
    if (at + 36 > max) return at;
    for (int a = 0; a < 3; ++a) for (int si = 0; si < 2; ++si) {
        const float s = si == 0 ? 1.f : -1.f;
        int u = (a + 1) % 3, v = (a + 2) % 3; if (s < 0.f) { const int k = u; u = v; v = k; }
        float n[3] = { 0.f, 0.f, 0.f }; n[a] = s;
        float q[4][3];
        const float su[4] = { -1.f, 1.f, 1.f, -1.f }, sv[4] = { -1.f, -1.f, 1.f, 1.f };
        for (int k = 0; k < 4; ++k) {
            for (int i = 0; i < 3; ++i) q[k][i] = c[i];
            q[k][a] += s * e[a]; q[k][u] += su[k] * e[u]; q[k][v] += sv[k] * e[v];
        }
        const int tri[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; ++k) {
            HvSrcVertex& o = out[at++];
            memcpy(o.p, q[tri[k]], sizeof(o.p)); memcpy(o.n, n, sizeof(o.n)); o.rgba = col; o.bone = bone;
        }
    }
    return at;
}

int hv_box_mesh(HvSrcVertex* out, int max, bool axes, bool boxes) {
    if (out == nullptr || max < 36 * 6) return 0;
    int n = 0;
    if (boxes) {
    { const float c[3] = { 0.f, 0.f, 0.f },     e[3] = { 0.045f, 0.015f, 0.045f }; n = add_box(out, n, max, c, e, rgba(0.62f, 0.62f, 0.66f), 0); }   // palm
    { const float c[3] = { 0.f, 0.f, -0.085f }, e[3] = { 0.010f, 0.010f, 0.040f }; n = add_box(out, n, max, c, e, rgba(0.80f, 0.80f, 0.84f), 0); }   // finger slab: z -0.045 .. -0.125
    { const float c[3] = { 0.f, 0.f, -0.035f }, e[3] = { 0.005f, 0.005f, 0.035f }; n = add_box(out, n, max, c, e, rgba(0.95f, 0.70f, 0.20f), 1); }   // stub: the grip's -Z, 0 .. -7 cm
    }
    if (axes) {
        { const float c[3] = { 0.030f, 0.f, 0.f }, e[3] = { 0.030f, 0.003f, 0.003f }; n = add_box(out, n, max, c, e, rgba(0.95f, 0.15f, 0.15f), 0); }   // +X red
        { const float c[3] = { 0.f, 0.030f, 0.f }, e[3] = { 0.003f, 0.030f, 0.003f }; n = add_box(out, n, max, c, e, rgba(0.15f, 0.90f, 0.15f), 0); }   // +Y green (out of the back of the hand)
        { const float c[3] = { 0.f, 0.f, 0.030f }, e[3] = { 0.003f, 0.003f, 0.030f }; n = add_box(out, n, max, c, e, rgba(0.20f, 0.35f, 1.00f), 0); }   // +Z blue (toward the elbow)
    }
    return n;
}

void hv_skin_rigid(const HvSrcVertex* src, int n, const HvBone* bones, int nb, HvVertex* dst) {
    for (int i = 0; i < n; ++i) {
        const HvSrcVertex& s = src[i];
        const HvBone& b = bones[(s.bone >= 0 && s.bone < nb) ? s.bone : 0];
        float p[3], nn[3];
        hv_mat3_vec(b.R, s.p, p); hv_mat3_vec(b.R, s.n, nn);
        HvVertex& d = dst[i];
        for (int k = 0; k < 3; ++k) { d.p[k] = b.o[k] + b.s * p[k]; d.n[k] = nn[k]; }
        d.uv[0] = d.uv[1] = 0.f; d.rgba = s.rgba;
    }
}

// =====================================================================================================================
// S2: packs, the finger pose, forward kinematics
// =====================================================================================================================

const int hv_parent[HV_J] = { 1, -1,  1, 2, 3, 4,  1, 6, 7, 8, 9,  1, 11, 12, 13, 14,  1, 16, 17, 18, 19,  1, 21, 22, 23, 24 };
const char* const hv_joint_name[HV_J] = {
    "palm", "wrist", "thumb_metacarpal", "thumb_proximal", "thumb_distal", "thumb_tip",
    "index_metacarpal", "index_proximal", "index_intermediate", "index_distal", "index_tip",
    "middle_metacarpal", "middle_proximal", "middle_intermediate", "middle_distal", "middle_tip",
    "ring_metacarpal", "ring_proximal", "ring_intermediate", "ring_distal", "ring_tip",
    "little_metacarpal", "little_proximal", "little_intermediate", "little_distal", "little_tip" };

static bool hv_fail(char* err, int errn, const char* fmt, ...) {
    if (err != nullptr && errn > 0) { va_list ap; va_start(ap, fmt); vsnprintf(err, static_cast<size_t>(errn), fmt, ap); va_end(ap); }
    return false;
}
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static void mat_ident(float* R) { const float I[9] = { 1, 0, 0,  0, 1, 0,  0, 0, 1 }; memcpy(R, I, sizeof(I)); }
static void xf_inv(const HvXform& a, HvXform* o) {   // rigid: (R, t)^-1 = (R^T, -R^T t)
    HvXform r; hv_mat3_T(a.R, r.R); hv_mat3_vec(r.R, a.t, r.t); for (float& v : r.t) v = -v; *o = r;
}
static void mirror_mat(const float* R, float* O) {   // S R S, S = diag(-1, 1, 1): negate the entries that mix x with y or z
    float t[9]; memcpy(t, R, sizeof(t));
    t[1] = -t[1]; t[2] = -t[2]; t[3] = -t[3]; t[6] = -t[6];
    memcpy(O, t, sizeof(t));
}
/// Rotation by `ang` radians about the unit axis a (Rodrigues), axes as columns like every matrix here.
static void rot_axis(const float a[3], float ang, float* R) {
    const float c = cosf(ang), s = sinf(ang), k = 1.f - c, x = a[0], y = a[1], z = a[2];
    R[0] = c + x*x*k;     R[1] = x*y*k - z*s;   R[2] = x*z*k + y*s;
    R[3] = y*x*k + z*s;   R[4] = c + y*y*k;     R[5] = y*z*k - x*s;
    R[6] = z*x*k - y*s;   R[7] = z*y*k + x*s;   R[8] = c + z*z*k;
}

/// The canonical index of a joint name, or -1: "index_proximal", also "XR_HAND_JOINT_INDEX_PROXIMAL_EXT", any case, '-' for '_'.
static int hv_canon_joint(const char* name) {
    if (name == nullptr) return -1;
    char b[96]; size_t n = 0;
    for (const char* p = name; *p != 0 && n + 1 < sizeof(b); ++p) { char c = *p; if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a'); if (c == '-' || c == ' ') c = '_'; b[n++] = c; }
    b[n] = 0;
    const char* s = b;
    if (strncmp(s, "xr_hand_joint_", 14) == 0) s += 14;
    size_t l = strlen(s);
    char t[96]; memcpy(t, s, l + 1);
    if (l > 4 && strcmp(t + l - 4, "_ext") == 0) t[l - 4] = 0;
    for (int j = 0; j < HV_J; ++j) if (strcmp(t, hv_joint_name[j]) == 0) return j;
    return -1;
}
/// A glTF column-major 4x4 into a rigid transform; false when the 3x3 is not a rotation (scaled or sheared).
static bool hv_from_gltf(const float m[16], HvXform* x) {
    for (int r = 0; r < 3; ++r) { for (int c = 0; c < 3; ++c) x->R[r*3+c] = m[c*4+r]; x->t[r] = m[12+r]; }
    for (int i = 0; i < 16; ++i) if (!std::isfinite(m[i])) return false;
    if (!hv_is_rotation(x->R, 0.03f)) return false;
    return hv_orthonormalize(x->R);
}

void hv_mesh_finish(HvMesh* m, int h) {
    m->hand = h;
    for (int j = 0; j < HV_J; ++j) {
        xf_inv(m->bind[j], &m->inv_bind[j]);
        if (hv_parent[j] < 0) m->local[j] = m->bind[j];
        else { HvXform ip; xf_inv(m->bind[hv_parent[j]], &ip); hv_xform_mul(ip, m->bind[j], &m->local[j]); }
    }
    // The thumb's opposition axis (handlib.py pose_world): the metacarpal swings from its bind direction toward a fixed
    // direction across the palm. Worked out in right-hand terms (a left hand mirrored first) and kept in the metacarpal's frame.
    float WT[9], Rm[9]; hv_mat3_T(m->bind[HV_WRIST].R, WT); hv_mat3_mul(WT, m->bind[2].R, Rm);
    if (h == 0) mirror_mat(Rm, Rm);
    const float d0[3] = { -Rm[2], -Rm[5], -Rm[8] };
    float t[3] = { -0.20f, -0.05f, -0.80f }; v3norm(t);
    float a[3]; v3cross(d0, t, a);
    if (!v3norm(a)) { m->thumb_axis[0] = 0.f; m->thumb_axis[1] = 1.f; m->thumb_axis[2] = 0.f; }
    else { float RmT[9]; hv_mat3_T(Rm, RmT); hv_mat3_vec(RmT, a, m->thumb_axis); v3norm(m->thumb_axis); }
    { float RmT[9]; hv_mat3_T(Rm, RmT); const float y[3] = { 0.f, 1.f, 0.f }; hv_mat3_vec(RmT, y, m->thumb_up); if (!v3norm(m->thumb_up)) { m->thumb_up[0] = 0.f; m->thumb_up[1] = 1.f; m->thumb_up[2] = 0.f; } }
    float d[3]; for (int i = 0; i < 3; ++i) d[i] = m->bind[15].t[i] - m->bind[HV_WRIST].t[i];
    m->hand_len = sqrtf(v3dot(d, d));
    // Tap build (the fist fix): how far each finger bone is turned sideways at the bind pose, measured about the wrist's +Y
    // (dorsal) in the wrist's X-Z plane: abd[f][0] the proximal bone from the hand's long axis (the wrist's -Z), [1] / [2] the
    // intermediate / distal bone from its parent. A finger that flexes about its own X axis while fanned out curls away from
    // its neighbour (the owner: "abnormal separation between ring and middle fingers when making a fist"); a real hand closes
    // its spread as it curls, the curled fingers side by side, so hv_pose_deltas turns each bone back by this as the finger
    // curls. The wrist, not the middle finger, is the reference, and every bone is straightened, because an imported rig can
    // zig-zag (the Tidus pack's right middle finger: proximal 26 deg toward the ring, intermediate 19 deg back). In right-hand
    // terms (a left hand's angles are the mirror: negated); each limited to +-30 deg.
    // (WT: the wrist's bind rotation transposed, from the thumb's block above)
    for (int f = 0; f < 4; ++f) {
        float prev = 0.f;
        for (int k = 0; k < 3; ++k) {
            const float* R = m->bind[7 + 5 * f + k].R; const float d[3] = { -R[2], -R[5], -R[8] }; float w[3]; hv_mat3_vec(WT, d, w);
            float yaw = (w[0] * w[0] + w[2] * w[2] > 1e-8f) ? atan2f(w[0], -w[2]) : prev;
            if (!std::isfinite(yaw)) yaw = prev;
            const float a = clampf(yaw - prev, -30.f * k_deg, 30.f * k_deg);
            m->abd[f][k] = h == 0 ? -a : a;
            prev = yaw;
        }
    }
}

void hv_mesh_mirror(const HvMesh& src, HvMesh* dst) {
    HvMesh& d = *dst;
    d = HvMesh();   // field by field: the encoded and decoded images stay with src (SF-1: never two copies)
    d.v = src.v; d.idx = src.idx; d.batch = src.batch;
    memcpy(d.bind, src.bind, sizeof(d.bind));
    d.prims = src.prims; d.tris = src.tris; d.unknown_joints = src.unknown_joints; d.images_dropped = src.images_dropped; d.palm_rebuilt = src.palm_rebuilt;
    d.tex_from = src.tex_from >= 0 ? src.tex_from : src.hand;
    for (HvSkinVertex& v : dst->v) { v.p[0] = -v.p[0]; v.n[0] = -v.n[0]; }
    for (size_t i = 0; i + 2 < dst->idx.size(); i += 3) { const uint32_t k = dst->idx[i + 1]; dst->idx[i + 1] = dst->idx[i + 2]; dst->idx[i + 2] = k; }
    for (int j = 0; j < HV_J; ++j) { mirror_mat(dst->bind[j].R, dst->bind[j].R); dst->bind[j].t[0] = -dst->bind[j].t[0]; }
    dst->mirrored = true;
    hv_mesh_finish(dst, 1 - src.hand);
}

static uint32_t rgba4(const float c[4]) {
    auto q = [](float x) { const int v = static_cast<int>(clampf(x, 0.f, 1.f) * 255.f + 0.5f); return static_cast<uint32_t>(v); };
    return q(c[0]) | (q(c[1]) << 8) | (q(c[2]) << 16) | (q(c[3]) << 24);
}

/// glTF requires every accessor to be aligned to its component size (and its stride a multiple of it); cgltf reads through a
/// typed pointer, so a corrupted offset would be a misaligned load. False for a misaligned one (null or without a view: fine).
static bool hv_acc_aligned(const cgltf_accessor* a) {
    if (a == nullptr || a->buffer_view == nullptr) return true;
    const cgltf_size c = cgltf_component_size(a->component_type);
    if (c <= 1) return true;
    return (a->buffer_view->offset + a->offset) % c == 0 && a->stride % c == 0;
}

bool hv_glb_parse(const void* data, size_t size, int h, const float* tint, HvMesh* out, char* err, int errn) {
    *out = HvMesh();
    if (data == nullptr || size < 20) return hv_fail(err, errn, "the file is empty or too small");
    cgltf_options opt; memset(&opt, 0, sizeof(opt)); opt.type = cgltf_file_type_glb;
    cgltf_data* d = nullptr;
    cgltf_result r = cgltf_parse(&opt, data, size, &d);
    if (r != cgltf_result_success || d == nullptr) return hv_fail(err, errn, "not a glTF 2.0 binary (.glb) (cgltf_parse %d)", static_cast<int>(r));
    struct Free { cgltf_data* d; ~Free() { cgltf_free(d); } } fr = { d };
    for (cgltf_size i = 0; i < d->buffers_count; ++i)
        if (d->buffers[i].uri != nullptr) return hv_fail(err, errn, "buffer %u is an external file or a data: URI (only a glb with its embedded buffer is read)", static_cast<unsigned>(i));
    r = cgltf_load_buffers(&opt, d, nullptr);
    if (r != cgltf_result_success) return hv_fail(err, errn, "the embedded buffer could not be read (cgltf_load_buffers %d)", static_cast<int>(r));
    for (cgltf_size i = 0; i < d->buffers_count; ++i)
        if (d->buffers[i].size > 0 && d->buffers[i].data == nullptr) return hv_fail(err, errn, "buffer %u has no data", static_cast<unsigned>(i));
    for (cgltf_size i = 0; i < d->extensions_required_count; ++i) {   // N8: a compressed or otherwise extended glb would read as zeros or garbage
        const char* x = d->extensions_required[i] ? d->extensions_required[i] : "?";
        if (strcmp(x, "KHR_mesh_quantization") == 0 || strcmp(x, "KHR_texture_transform") == 0 || strncmp(x, "KHR_materials_", 14) == 0) continue;
        return hv_fail(err, errn, "the glb requires the glTF extension %s (export without compression)", x);
    }
    for (cgltf_size i = 0; i < d->buffer_views_count; ++i)
        if (d->buffer_views[i].has_meshopt_compression) return hv_fail(err, errn, "the glb is meshopt-compressed (export without compression)");
    for (cgltf_size i = 0; i < d->meshes_count; ++i) for (cgltf_size k = 0; k < d->meshes[i].primitives_count; ++k)
        if (d->meshes[i].primitives[k].has_draco_mesh_compression) return hv_fail(err, errn, "the glb is Draco-compressed (export without compression)");
    // Before cgltf_validate: it reads every index accessor (and sparse indices) through typed pointers to find their bound,
    // so a misaligned one must be refused first (found by the S3 fuzz under UBSan on an importer pack's uint16 indices).
    for (cgltf_size i = 0; i < d->buffers_count; ++i)
        if (d->buffers[i].data != nullptr && reinterpret_cast<uintptr_t>(d->buffers[i].data) % 4 != 0) return hv_fail(err, errn, "the glb's binary chunk is misaligned (not a valid glb)");
    for (cgltf_size i = 0; i < d->accessors_count; ++i) {
        if (d->accessors[i].is_sparse) return hv_fail(err, errn, "accessor %u is sparse (not supported: export without sparse accessors)", static_cast<unsigned>(i));
        if (!hv_acc_aligned(&d->accessors[i])) return hv_fail(err, errn, "accessor %u is misaligned (not a valid glTF)", static_cast<unsigned>(i));
    }
    r = cgltf_validate(d);
    if (r != cgltf_result_success) return hv_fail(err, errn, "the glTF does not validate (cgltf_validate %d: an accessor or view out of range?)", static_cast<int>(r));
    const cgltf_node* mn = nullptr;
    for (cgltf_size i = 0; i < d->nodes_count; ++i) if (d->nodes[i].mesh != nullptr && d->nodes[i].skin != nullptr) { mn = &d->nodes[i]; break; }
    if (mn == nullptr) return hv_fail(err, errn, "no skinned mesh (a node with a mesh and a skin)");
    const cgltf_skin* sk = mn->skin; const cgltf_mesh* me = mn->mesh;
    if (sk->joints_count == 0 || sk->joints_count > 256) return hv_fail(err, errn, "the skin has %u joints (26 expected)", static_cast<unsigned>(sk->joints_count));
    // The joints by name (spec S2: also the XR_HAND_JOINT_*_EXT names); a skin joint outside the 26 gives its weights to the wrist.
    std::vector<int> jmap(sk->joints_count, -1); int skin_of[HV_J]; for (int& k : skin_of) k = -1;
    for (cgltf_size k = 0; k < sk->joints_count; ++k) {
        const int c = sk->joints[k] != nullptr ? hv_canon_joint(sk->joints[k]->name) : -1;
        if (c >= 0 && skin_of[c] < 0) { skin_of[c] = static_cast<int>(k); jmap[k] = c; }
        else { jmap[k] = HV_WRIST; out->unknown_joints++; }
    }
    for (int c = 1; c < HV_J; ++c) if (skin_of[c] < 0) return hv_fail(err, errn, "joint '%s' missing", hv_joint_name[c]);
    // Bind frames: the inverse bind matrices inverted (else the joint nodes' world transforms).
    const cgltf_accessor* ibm = sk->inverse_bind_matrices;
    if (ibm != nullptr && (ibm->is_sparse || ibm->type != cgltf_type_mat4 || ibm->count < sk->joints_count)) return hv_fail(err, errn, "the inverse bind matrices are not one 4x4 per joint");
    if (!hv_acc_aligned(ibm)) return hv_fail(err, errn, "the inverse bind matrices are misaligned");
    for (int c = 0; c < HV_J; ++c) {
        if (skin_of[c] < 0) continue;
        float m[16];
        if (ibm != nullptr) {
            if (!cgltf_accessor_read_float(ibm, static_cast<cgltf_size>(skin_of[c]), m, 16)) return hv_fail(err, errn, "the inverse bind matrix of '%s' could not be read", hv_joint_name[c]);
            HvXform inv; if (!hv_from_gltf(m, &inv)) return hv_fail(err, errn, "the bind matrix of '%s' is not a rotation + translation (scaled? export in metres at scale 1)", hv_joint_name[c]);
            xf_inv(inv, &out->bind[c]);
        }
        else {
            cgltf_node_transform_world(sk->joints[skin_of[c]], m);
            if (!hv_from_gltf(m, &out->bind[c])) return hv_fail(err, errn, "the joint node '%s' is scaled or sheared (export in metres at scale 1)", hv_joint_name[c]);
        }
    }
    if (skin_of[HV_PALM] < 0) {   // no palm: the middle of the middle metacarpal, oriented like it (hands-assets 1)
        memcpy(out->bind[HV_PALM].R, out->bind[11].R, sizeof(out->bind[HV_PALM].R));
        for (int i = 0; i < 3; ++i) out->bind[HV_PALM].t[i] = 0.5f * (out->bind[11].t[i] + out->bind[12].t[i]);
        out->palm_rebuilt = true;
    }
    // Primitives: triangles, POSITION + JOINTS_0 + WEIGHTS_0 (NORMAL, TEXCOORD_0, COLOR_0 optional), one material each.
    const float tn[3] = { tint ? clampf(tint[0], 0.f, 4.f) : 1.f, tint ? clampf(tint[1], 0.f, 4.f) : 1.f, tint ? clampf(tint[2], 0.f, 4.f) : 1.f };
    std::vector<const cgltf_image*> images;               // tex index -> image
    std::vector<std::vector<uint32_t>> lists; std::vector<int> list_tex;   // the indices, grouped by texture
    cgltf_size idx_total = 0;                                               // SF-1: <= HV_MAX_INDICES for the hand
    for (cgltf_size pi = 0; pi < me->primitives_count; ++pi) {
        const cgltf_primitive& p = me->primitives[pi];
        if (p.type != cgltf_primitive_type_triangles) return hv_fail(err, errn, "primitive %u is not a triangle list", static_cast<unsigned>(pi));
        const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *jnt = nullptr, *wgt = nullptr, *col = nullptr;
        for (cgltf_size a = 0; a < p.attributes_count; ++a) {
            const cgltf_attribute& at = p.attributes[a];
            if (at.index != 0) continue;
            switch (at.type) {
                case cgltf_attribute_type_position: pos = at.data; break;
                case cgltf_attribute_type_normal:   nrm = at.data; break;
                case cgltf_attribute_type_texcoord: uv  = at.data; break;
                case cgltf_attribute_type_joints:   jnt = at.data; break;
                case cgltf_attribute_type_weights:  wgt = at.data; break;
                case cgltf_attribute_type_color:    col = at.data; break;
                default: break;
            }
        }
        if (pos == nullptr || pos->type != cgltf_type_vec3) return hv_fail(err, errn, "primitive %u has no POSITION", static_cast<unsigned>(pi));
        if (jnt == nullptr || wgt == nullptr || jnt->type != cgltf_type_vec4 || wgt->type != cgltf_type_vec4) return hv_fail(err, errn, "primitive %u is not skinned (JOINTS_0 / WEIGHTS_0)", static_cast<unsigned>(pi));
        const cgltf_size n = pos->count;
        auto same = [&](const cgltf_accessor* a) { return a == nullptr || (a->count == n && !a->is_sparse); };
        if (pos->is_sparse || !same(nrm) || !same(uv) || !same(jnt) || !same(wgt) || !same(col)) return hv_fail(err, errn, "primitive %u: the attributes differ in length (or are sparse)", static_cast<unsigned>(pi));
        if (!hv_acc_aligned(pos) || !hv_acc_aligned(nrm) || !hv_acc_aligned(uv) || !hv_acc_aligned(jnt) || !hv_acc_aligned(wgt) || !hv_acc_aligned(col) || !hv_acc_aligned(p.indices))
            return hv_fail(err, errn, "primitive %u: an accessor is misaligned (not a valid glTF)", static_cast<unsigned>(pi));
        if (nrm != nullptr && nrm->type != cgltf_type_vec3) nrm = nullptr;
        if (uv != nullptr && uv->type != cgltf_type_vec2) uv = nullptr;
        if (col != nullptr && col->type != cgltf_type_vec3 && col->type != cgltf_type_vec4) col = nullptr;
        if (n == 0) continue;
        if (out->v.size() + n > static_cast<size_t>(HV_MAX_VERTS)) return hv_fail(err, errn, "more than %d vertices", HV_MAX_VERTS);
        // material: the base colour factor, and its texture if the image is embedded (a bufferView)
        float base[4] = { 1.f, 1.f, 1.f, 1.f }; int tex = -1;
        if (p.material != nullptr && p.material->has_pbr_metallic_roughness) {
            const cgltf_pbr_metallic_roughness& pm = p.material->pbr_metallic_roughness;
            for (int i = 0; i < 4; ++i) base[i] = std::isfinite(pm.base_color_factor[i]) ? pm.base_color_factor[i] : 1.f;
            const cgltf_texture* t = pm.base_color_texture.texture;
            const cgltf_image* im = (t != nullptr) ? t->image : nullptr;
            if (im != nullptr && im->uri == nullptr && im->buffer_view != nullptr && uv != nullptr && pm.base_color_texture.texcoord == 0) {
                for (size_t k = 0; k < images.size(); ++k) if (images[k] == im) tex = static_cast<int>(k);
                if (tex < 0 && images.size() >= static_cast<size_t>(HV_MAX_IMAGES)) out->images_dropped++;   // SF-1: at most 4 images (64 MB decoded)
                else if (tex < 0) {
                    const uint8_t* b = cgltf_buffer_view_data(im->buffer_view); const size_t bs = im->buffer_view->size;
                    if (b != nullptr && bs > 8 && bs <= (32u << 20)) { images.push_back(im); out->tex_file.emplace_back(b, b + bs); tex = static_cast<int>(images.size() - 1); }
                }
            }
        }
        for (int i = 0; i < 3; ++i) base[i] *= tn[i];
        const uint32_t vb = static_cast<uint32_t>(out->v.size());
        out->v.resize(vb + n);
        for (cgltf_size i = 0; i < n; ++i) {
            HvSkinVertex& v = out->v[vb + i];
            float f4[4] = { 0.f, 0.f, 0.f, 1.f };
            if (!cgltf_accessor_read_float(pos, i, v.p, 3)) return hv_fail(err, errn, "primitive %u: POSITION could not be read", static_cast<unsigned>(pi));
            for (float x : v.p) if (!std::isfinite(x) || fabsf(x) > 10.f) return hv_fail(err, errn, "primitive %u: a position is not finite or over 10 m from the wrist", static_cast<unsigned>(pi));
            v.n[0] = v.n[1] = v.n[2] = 0.f; if (nrm != nullptr) cgltf_accessor_read_float(nrm, i, v.n, 3);
            v.uv[0] = v.uv[1] = 0.f; if (uv != nullptr) cgltf_accessor_read_float(uv, i, v.uv, 2);
            for (float& x : v.uv) if (!std::isfinite(x)) x = 0.f;
            float c[4] = { base[0], base[1], base[2], base[3] };
            if (col != nullptr && cgltf_accessor_read_float(col, i, f4, col->type == cgltf_type_vec4 ? 4 : 3)) for (int k = 0; k < 4; ++k) c[k] *= std::isfinite(f4[k]) ? f4[k] : 1.f;
            v.rgba = rgba4(c);
            cgltf_uint jj[4] = { 0, 0, 0, 0 }; float ww[4] = { 0.f, 0.f, 0.f, 0.f };
            if (!cgltf_accessor_read_uint(jnt, i, jj, 4) || !cgltf_accessor_read_float(wgt, i, ww, 4)) return hv_fail(err, errn, "primitive %u: JOINTS_0 / WEIGHTS_0 could not be read", static_cast<unsigned>(pi));
            // merge to canonical joints, renormalise, quantise to unorm8 summing to 255 (weights renormalised at load: spec S2)
            int cj[4]; float cw[4]; int m = 0;
            for (int k = 0; k < 4; ++k) {
                float w = std::isfinite(ww[k]) ? ww[k] : 0.f; if (w <= 0.f) continue;
                if (jj[k] >= sk->joints_count) return hv_fail(err, errn, "primitive %u: a vertex names skin joint %u of %u", static_cast<unsigned>(pi), static_cast<unsigned>(jj[k]), static_cast<unsigned>(sk->joints_count));
                const int c2 = jmap[jj[k]];
                int at = -1; for (int q = 0; q < m; ++q) if (cj[q] == c2) at = q;
                if (at >= 0) cw[at] += w; else { cj[m] = c2; cw[m] = w; m++; }
            }
            float sum = 0.f; for (int k = 0; k < m; ++k) sum += cw[k];
            if (m == 0 || !(sum > 1e-6f)) { m = 1; cj[0] = HV_WRIST; cw[0] = 1.f; sum = 1.f; }
            int q8[4] = { 0, 0, 0, 0 }, tot = 0, big = 0;
            for (int k = 0; k < m; ++k) { q8[k] = static_cast<int>(cw[k] / sum * 255.f + 0.5f); tot += q8[k]; if (cw[k] > cw[big]) big = k; }
            q8[big] += 255 - tot; if (q8[big] < 0) q8[big] = 0;
            for (int k = 0; k < 4; ++k) { v.j[k] = static_cast<uint8_t>(k < m ? cj[k] : 0); v.w[k] = static_cast<uint8_t>(k < m ? q8[k] : 0); }
        }
        std::vector<uint32_t> ix;
        if (p.indices != nullptr) {
            if (p.indices->is_sparse || p.indices->count % 3 != 0) return hv_fail(err, errn, "primitive %u: the index count is not a multiple of 3", static_cast<unsigned>(pi));
            if (idx_total + p.indices->count > static_cast<cgltf_size>(HV_MAX_INDICES))
                return hv_fail(err, errn, "primitive %u: %u indices, more than %d for the hand", static_cast<unsigned>(pi), static_cast<unsigned>(p.indices->count), HV_MAX_INDICES);
            ix.resize(p.indices->count);
            for (cgltf_size i = 0; i < p.indices->count; ++i) {
                const cgltf_size k = cgltf_accessor_read_index(p.indices, i);
                if (k >= n) return hv_fail(err, errn, "primitive %u: index %u out of range (%u vertices)", static_cast<unsigned>(pi), static_cast<unsigned>(k), static_cast<unsigned>(n));
                ix[i] = vb + static_cast<uint32_t>(k);
            }
        }
        else {
            if (n % 3 != 0) return hv_fail(err, errn, "primitive %u: not indexed and the vertex count is not a multiple of 3", static_cast<unsigned>(pi));
            if (idx_total + n > static_cast<cgltf_size>(HV_MAX_INDICES)) return hv_fail(err, errn, "primitive %u: more than %d indices for the hand", static_cast<unsigned>(pi), HV_MAX_INDICES);
            ix.resize(n); for (cgltf_size i = 0; i < n; ++i) ix[i] = vb + static_cast<uint32_t>(i);
        }
        if (nrm == nullptr) {   // no normals: area-weighted vertex normals of this primitive
            for (size_t t = 0; t + 2 < ix.size(); t += 3) {
                HvSkinVertex &a = out->v[ix[t]], &b = out->v[ix[t+1]], &c = out->v[ix[t+2]];
                const float e1[3] = { b.p[0]-a.p[0], b.p[1]-a.p[1], b.p[2]-a.p[2] }, e2[3] = { c.p[0]-a.p[0], c.p[1]-a.p[1], c.p[2]-a.p[2] };
                float fnm[3]; v3cross(e1, e2, fnm);
                for (int k = 0; k < 3; ++k) { a.n[k] += fnm[k]; b.n[k] += fnm[k]; c.n[k] += fnm[k]; }
            }
        }
        for (cgltf_size i = 0; i < n; ++i) { float* nn = out->v[vb + i].n; if (!v3norm(nn)) { nn[0] = 0.f; nn[1] = 1.f; nn[2] = 0.f; } }
        int li = -1; for (size_t k = 0; k < list_tex.size(); ++k) if (list_tex[k] == tex) li = static_cast<int>(k);
        if (li < 0) { lists.emplace_back(); list_tex.push_back(tex); li = static_cast<int>(lists.size() - 1); }
        lists[static_cast<size_t>(li)].insert(lists[static_cast<size_t>(li)].end(), ix.begin(), ix.end());
        idx_total += ix.size();
        out->tris += static_cast<int>(ix.size() / 3); out->prims++;
    }
    for (size_t k = 0; k < lists.size(); ++k) {
        if (lists[k].empty()) continue;
        HvBatch b; b.first = static_cast<uint32_t>(out->idx.size()); b.count = static_cast<uint32_t>(lists[k].size()); b.tex = list_tex[k];
        out->idx.insert(out->idx.end(), lists[k].begin(), lists[k].end()); out->batch.push_back(b);
    }
    if (out->idx.empty()) return hv_fail(err, errn, "the mesh has no triangles");
    out->tex.resize(out->tex_file.size());
    hv_mesh_finish(out, h);
    if (!(out->hand_len > 0.05f && out->hand_len < 1.f)) return hv_fail(err, errn, "the wrist -> middle tip length is %.3f m (a hand in metres is about 0.19)", out->hand_len);
    return true;
}

bool hv_pack_parse(const bool have[2], bool mirror, const float* tint, HvPackRead read, void* ctx, const char* const names[2],
                   HvMesh hand[2], char* err, int errn) {
    hand[0] = HvMesh(); hand[1] = HvMesh();
    if (!have[0] && !have[1]) return hv_fail(err, errn, "no glb named for either hand");
    const int src = have[1] ? 1 : 0;                     // the hand read first; the other is its mirror unless both are read
    const bool both = have[0] && have[1] && !mirror;
    char e[256] = "";
    for (int k = 0; k < (both ? 2 : 1); ++k) {
        const int h = k == 0 ? src : 1 - src;
        const char* nm = names[h] ? names[h] : "?";
        std::vector<uint8_t> b;
        if (!read(ctx, h, &b, e, sizeof(e))) return hv_fail(err, errn, "%s: %s", nm, e);
        if (!hv_glb_parse(b.data(), b.size(), h, tint, &hand[h], e, sizeof(e))) return hv_fail(err, errn, "%s: %s", nm, e);
    }                                                    // b freed here, before the next file is read
    if (!both) hv_mesh_mirror(hand[src], &hand[1 - src]);
    return true;
}

void hv_image_prepare(HvImage* im, int max_side) {
    im->mips.clear(); im->src_w = im->w; im->src_h = im->h;
    if (im->w <= 0 || im->h <= 0 || im->w > HV_MAX_TEX || im->h > HV_MAX_TEX || im->rgba.size() != static_cast<size_t>(im->w) * static_cast<size_t>(im->h)) { *im = HvImage(); return; }
    auto half = [](const std::vector<uint32_t>& s, int w, int h, std::vector<uint32_t>* d, int* nw_, int* nh_) {
        const int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        d->assign(static_cast<size_t>(nw) * static_cast<size_t>(nh), 0u);
        for (int y = 0; y < nh; ++y) for (int x = 0; x < nw; ++x) {
            uint32_t acc[4] = {};
            for (int k = 0; k < 4; ++k) {
                const int sx = (x * 2 + (k & 1)) < w ? x * 2 + (k & 1) : w - 1, sy = (y * 2 + (k >> 1)) < h ? y * 2 + (k >> 1) : h - 1;
                const uint32_t p = s[static_cast<size_t>(sy) * static_cast<size_t>(w) + static_cast<size_t>(sx)];
                for (int c = 0; c < 4; ++c) acc[c] += (p >> (8 * c)) & 255u;
            }
            (*d)[static_cast<size_t>(y) * static_cast<size_t>(nw) + static_cast<size_t>(x)] = ((acc[0] + 2) / 4) | (((acc[1] + 2) / 4) << 8) | (((acc[2] + 2) / 4) << 16) | (((acc[3] + 2) / 4) << 24);
        }
        *nw_ = nw; *nh_ = nh;
    };
    if (max_side < 1) max_side = 1;
    while (im->w > max_side || im->h > max_side) {       // too big for a hand: smaller on the GPU (and in a 32-bit process)
        std::vector<uint32_t> d; int nw = 0, nh = 0; half(im->rgba, im->w, im->h, &d, &nw, &nh);
        im->rgba.swap(d); im->w = nw; im->h = nh;
    }
    im->mips.reserve(11);                                 // no reallocation below: s points into it
    int w = im->w, h = im->h; const std::vector<uint32_t>* s = &im->rgba;
    while ((w > 1 || h > 1) && im->mips.size() < 11) {
        std::vector<uint32_t> d; int nw = 0, nh = 0; half(*s, w, h, &d, &nw, &nh);
        im->mips.push_back(std::move(d)); s = &im->mips.back(); w = nw; h = nh;
    }
}

#if defined(_WIN32)
bool hv_read_file(const wchar_t* path, size_t max_bytes, std::vector<uint8_t>* out, char* err, int errn) {
    out->clear();
    FILE* f = nullptr;
    if (path == nullptr || _wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return hv_fail(err, errn, "cannot open the file");
    bool ok = false;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long n = ftell(f);
        if (n <= 0) hv_fail(err, errn, "the file is empty");
        else if (static_cast<unsigned long>(n) > max_bytes) hv_fail(err, errn, "the file is %ld bytes (more than %u MB)", n, static_cast<unsigned>(max_bytes >> 20));
        else if (fseek(f, 0, SEEK_SET) == 0) {
            out->resize(static_cast<size_t>(n));
            ok = fread(out->data(), 1, out->size(), f) == out->size();
            if (!ok) hv_fail(err, errn, "the file could not be read");
        }
    }
    fclose(f);
    if (!ok) out->clear();
    return ok;
}

bool hv_wic_decode(const uint8_t* data, size_t size, HvImage* out, char* err, int errn) {
    *out = HvImage();
    if (data == nullptr || size == 0 || size > 0x7FFFFFFF) return hv_fail(err, errn, "no image data");
    IWICImagingFactory* fac = nullptr; IWICStream* st = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    bool ok = false;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fac));
    if (FAILED(hr)) hv_fail(err, errn, "no WIC imaging factory (hr 0x%08lX)", static_cast<unsigned long>(hr));
    else if (FAILED(hr = fac->CreateStream(&st)) || FAILED(hr = st->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size)))) hv_fail(err, errn, "WIC stream (hr 0x%08lX)", static_cast<unsigned long>(hr));
    else if (FAILED(hr = fac->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) || FAILED(hr = dec->GetFrame(0, &fr))) hv_fail(err, errn, "not a PNG or JPEG WIC can decode (hr 0x%08lX)", static_cast<unsigned long>(hr));
    else {
        UINT w = 0, h = 0;
        if (FAILED(fr->GetSize(&w, &h)) || w == 0 || h == 0 || w > HV_MAX_TEX || h > HV_MAX_TEX) hv_fail(err, errn, "the image is %ux%u (at most %dx%d)", w, h, HV_MAX_TEX, HV_MAX_TEX);
        else if (FAILED(hr = fac->CreateFormatConverter(&cv)) || FAILED(hr = cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) hv_fail(err, errn, "WIC convert (hr 0x%08lX)", static_cast<unsigned long>(hr));
        else {
            out->rgba.resize(static_cast<size_t>(w) * h);
            if (FAILED(hr = cv->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(out->rgba.data())))) { hv_fail(err, errn, "WIC copy (hr 0x%08lX)", static_cast<unsigned long>(hr)); out->rgba.clear(); }
            else { out->w = static_cast<int>(w); out->h = static_cast<int>(h); ok = true; }
        }
    }
    if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release(); if (st) st->Release(); if (fac) fac->Release();
    return ok;
}
#endif

// -- the controller-driven pose (research hands-input.md 4.1 - 4.3) -------------------------------------------------

/// Every angle of the controller pose in one place (degrees; first values from hands-input 4.2, the thumb's key poses tuned
/// on the generic hand with tools/hands/preview-pack.py). A later pack format may override the maxima.
static const struct HvAngles {
    float off[3], max[3];          // MCP (proximal), PIP (intermediate), DIP (distal): flexion off + c (max - off)
    float splay[4];                // at the proximal joint about local Y, index middle ring little, scaled by (1 - 0.7 c)
    float cup[4];                  // metacarpal flexion x c (the palm cups): index middle ring little
    float thumb[4][5];             // key poses FREE, UP, ON, WRAP: opposition, CMC flexion, MCP flexion, IP flexion, radial swing (about the wrist's +Y)
    float face_flex, rest_flex;    // the ON pose's CMC flexion offset: thumb on the face buttons / on the thumbrest
} k_ang = {
    { 8.f, 10.f, 5.f }, { 80.f, 100.f, 70.f },
    { 6.f, 0.f, -4.f, -9.f },
    { 0.f, 0.f, 6.f, 12.f },
    { {  4.f,  0.f,  8.f,  8.f,  0.f },    // FREE: relaxed beside the open hand
      { -6.f, -8.f,  0.f, -6.f, 24.f },    // UP: thumbs-up (lifted off the face, straight, swung to the radial side)
      { 16.f,  8.f, 22.f, 24.f,  0.f },    // ON: on the controller's face (stick / buttons / thumbrest)
      { 23.f,  9.f, 32.f, 52.f,  0.f } },  // WRAP: over the curled fingers (the fist)
    -8.f, 6.f,
};
static const float k_d2r = 0.0174532925f;

static const float k_release_hold = 0.050f;   // S3: seconds a touch must stay off before its finger opens
static const float k_sq_dead = 0.04f;         // S3: the grip's dead zone (a finger resting on the grip button)
static float hv_approach(float cur, float tgt, float dt, float tau) { return cur + (tgt - cur) * (1.f - expf(-dt / tau)); }

void hv_fingers_update(HvFingerState* s, const HvCtrl& in, float dt) {
    const float trig = std::isfinite(in.trigger) ? clampf(in.trigger, 0.f, 1.f) : 0.f;
    const float sq = std::isfinite(in.squeeze) ? clampf((in.squeeze - k_sq_dead) / (1.f - k_sq_dead), 0.f, 1.f) : 0.f;   // S3: a resting finger's grip noise stays 0
    const float tt = (in.trigger_touch || trig > 0.05f) ? 1.f : 0.f;            // trigger > 0.05 counts as a touch (a dropped bit)
    const float th = (in.thumb_stick || in.thumb_rest || in.thumb_face) ? 1.f : 0.f;
    const float fa = in.thumb_face ? 1.f : 0.f, re = (in.thumb_rest && !in.thumb_face) ? 1.f : 0.f;
    const float raw[4] = { tt, th, fa, re };
    if (!s->init || dt < 0.f || dt > 0.25f || !std::isfinite(dt)) {   // the first frame, or after a pause: snap
        s->trig = trig; s->sq = sq; s->trig_t = tt; s->thumb_t = th; s->face_t = fa; s->rest_t = re; s->init = true;
        for (int k = 0; k < 4; ++k) s->off_s[k] = raw[k] > 0.5f ? 0.f : 1e9f;
        return;
    }
    if (dt == 0.f) return;                         // the same poses as last time (no new display time): no step
    float tgt[4];
    for (int k = 0; k < 4; ++k) {                  // S3: a release is held for k_release_hold (a flicker of the sensor is ignored)
        if (raw[k] > 0.5f) s->off_s[k] = 0.f; else if (s->off_s[k] < 1e8f) s->off_s[k] += dt;
        tgt[k] = (raw[k] > 0.5f || s->off_s[k] < k_release_hold) ? 1.f : 0.f;
    }
    if (raw[2] > 0.5f) { tgt[3] = 0.f; s->off_s[3] = 1e9f; }   // review N5: face buttons and thumbrest exclude each other: no hold of the one left
    if (raw[3] > 0.5f) { tgt[2] = 0.f; s->off_s[2] = 1e9f; }
    auto boolean = [&](float cur, float t) { return hv_approach(cur, t, dt, t > cur ? 0.035f : 0.050f); };   // on 35 ms; off 50 ms after the hold
    s->trig = hv_approach(s->trig, trig, dt, 0.015f); s->sq = hv_approach(s->sq, sq, dt, 0.015f);   // floats 15 ms
    s->trig_t = boolean(s->trig_t, tgt[0]); s->thumb_t = boolean(s->thumb_t, tgt[1]); s->face_t = boolean(s->face_t, tgt[2]); s->rest_t = boolean(s->rest_t, tgt[3]);
}

bool hv_put_down_update(HvFingerState* s, const HvCtrl& in, const float grip_R[9], const float grip_p[3], float dt) {
    const bool touched = in.trigger_touch || in.trigger > 0.05f || in.squeeze > 0.05f || in.thumb_stick || in.thumb_rest || in.thumb_face;
    bool moved = !s->anchor;
    if (!moved) {
        const float d[3] = { grip_p[0] - s->anchor_p[0], grip_p[1] - s->anchor_p[1], grip_p[2] - s->anchor_p[2] };
        moved = v3dot(d, d) > 0.005f * 0.005f || hv_rot_angle_deg(s->anchor_R, grip_R) > 2.f;
    }
    if (moved || touched) {
        memcpy(s->anchor_p, grip_p, sizeof(s->anchor_p)); memcpy(s->anchor_R, grip_R, sizeof(s->anchor_R)); s->anchor = true;
        s->still_s = 0.f; s->put_down = false;
        return false;
    }
    if (dt > 0.f && dt < 0.25f) s->still_s += dt;   // a pause (a menu, a load) does not count as stillness
    if (s->still_s >= 10.f) s->put_down = true;
    return s->put_down;
}

void hv_pose_deltas(HvFingerState* s, const HvMesh& m, int h, float rest_curl, const float* pack_rest, float D[HV_J][9]) {
    for (int j = 0; j < HV_J; ++j) mat_ident(D[j]);
    const float g = clampf(s->sq, 0.f, 1.f), tr = clampf(s->trig, 0.f, 1.f), tt = clampf(s->trig_t, 0.f, 1.f);
    const float rc = clampf(std::isfinite(rest_curl) ? rest_curl : 0.2f, 0.f, 1.f);
    float c[4];
    c[0] = tt * (0.30f + 0.70f * tr);                    // index: untouched straight (point), resting 0.30, pulled 1
    c[1] = rc + (1.f - rc) * g;                          // middle, ring, little round the handle (ring and little a little ahead)
    c[2] = rc + (1.f - rc) * powf(g, 0.85f);
    c[3] = rc + (1.f - rc) * powf(g, 0.70f);
    for (int f = 0; f < 4; ++f) {
        const float cf = clampf(c[f] + (pack_rest ? clampf(pack_rest[f + 1], -1.f, 1.f) : 0.f), -0.2f, 1.f);
        s->curl[f + 1] = cf;
        const int b = 6 + 5 * f;                          // metacarpal of this finger
        float A[9], B[9];
        rot_x(-k_ang.cup[f] * cf * k_d2r, D[b]);
        // Tap build (the fist fix): the spread closes as the finger curls. Below a curl of 0.25 (open, the idle 0.20, a point's
        // index) nothing changes; at a full curl every bone of the finger lies along the hand's long axis (its own bind-pose fan
        // and any zig-zag of the rig taken out: mesh.abd), the four side by side as in a real fist, smoothstepped in between.
        const float c01 = clampf(cf, 0.f, 1.f), u = clampf((c01 - 0.25f) / 0.70f, 0.f, 1.f), close = u * u * (3.f - 2.f * u);
        rot_y(k_ang.splay[f] * (1.f - 0.7f * c01) * (1.f - close) * k_d2r + m.abd[f][0] * close, A);
        rot_x(-(k_ang.off[0] + cf * (k_ang.max[0] - k_ang.off[0])) * k_d2r, B);
        hv_mat3_mul(A, B, D[b + 1]);
        for (int k = 1; k < 3; ++k) {   // the intermediate and distal bones: straightened by their own sideways turn, then flexed
            rot_y(m.abd[f][k] * close, A);
            rot_x(-(k_ang.off[k] + cf * (k_ang.max[k] - k_ang.off[k])) * k_d2r, B);
            hv_mat3_mul(A, B, D[b + 1 + k]);
        }
    }
    // the thumb: T = lerp(lerp(FREE, UP, g), lerp(ON', WRAP, g), t), ON' with the surface the thumb is on
    const float t = clampf(s->thumb_t, 0.f, 1.f);
    s->curl[0] = t;
    float on[5]; memcpy(on, k_ang.thumb[2], sizeof(on));
    on[1] += k_ang.face_flex * clampf(s->face_t, 0.f, 1.f) + k_ang.rest_flex * clampf(s->rest_t, 0.f, 1.f);
    float k[5];
    for (int i = 0; i < 5; ++i) {
        const float top = k_ang.thumb[0][i] + (k_ang.thumb[1][i] - k_ang.thumb[0][i]) * g;
        const float bot = on[i] + (k_ang.thumb[3][i] - on[i]) * g;
        k[i] = top + (bot - top) * t;
    }
    if (pack_rest != nullptr) { const float pr = clampf(pack_rest[0], -1.f, 1.f) * 40.f; k[2] += pr; k[3] += pr; }
    {
        float U[9], O[9], F[9], UO[9];
        rot_axis(m.thumb_up, k[4] * k_d2r, U);
        rot_axis(m.thumb_axis, k[0] * k_d2r, O);
        rot_x(-k[1] * k_d2r, F);
        hv_mat3_mul(U, O, UO); hv_mat3_mul(UO, F, D[2]);  // CMC: the radial swing, opposition, then flexion in the metacarpal's frame
        rot_x(-k[2] * k_d2r, D[3]);                       // MCP
        rot_x(-k[3] * k_d2r, D[4]);                       // IP
    }
    if (h == 0) for (int j = 0; j < HV_J; ++j) mirror_mat(D[j], D[j]);   // the left hand: the mirror of the right's deltas (spec 2 row 7)
}

void hv_fk_skin(const HvMesh& m, const float D[HV_J][9], HvXform skin[HV_J], HvXform* posed) {
    HvXform W[HV_J];
    static const int order[HV_J] = { 1, 0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25 };
    for (int oi = 0; oi < HV_J; ++oi) {
        const int j = order[oi];
        HvXform L = m.local[j];
        hv_mat3_mul(m.local[j].R, D[j], L.R);              // the delta in the joint's own frame, after its offset from the parent
        if (hv_parent[j] < 0) W[j] = L; else hv_xform_mul(W[hv_parent[j]], L, &W[j]);
    }
    for (int j = 0; j < HV_J; ++j) hv_xform_mul(W[j], m.inv_bind[j], &skin[j]);
    if (posed != nullptr) for (int j = 0; j < HV_J; ++j) posed[j] = W[j];
}

void hv_palette(const HvXform skin[HV_J], const HvXform& H, const HvXform& Bp, float pack_scale, const float P[3], const float Rb[9], float sc,
                const float mvp[3], float dy, float out[HV_J * 3][4]) {
    float RbT[9], BpT[9], A[9], Rw[9]; hv_mat3_T(Rb, RbT); hv_mat3_T(Bp.R, BpT);
    hv_mat3_mul(RbT, H.R, A); hv_mat3_mul(A, BpT, Rw);
    const float k = sc * pack_scale;
    const float r[3] = { sc * H.t[0] + mvp[0], sc * H.t[1] + mvp[1], sc * H.t[2] + mvp[2] };
    float o[3], w[3]; hv_mat3_vec(RbT, r, o); hv_mat3_vec(Rw, Bp.t, w);
    for (int i = 0; i < 3; ++i) o[i] = P[i] + o[i] - k * w[i];
    o[1] += dy;
    for (int j = 0; j < HV_J; ++j) {
        float M[9], t[3]; hv_mat3_mul(Rw, skin[j].R, M); hv_mat3_vec(Rw, skin[j].t, t);
        for (int i = 0; i < 3; ++i) { float* row = out[j * 3 + i]; row[0] = k * M[i*3+0]; row[1] = k * M[i*3+1]; row[2] = k * M[i*3+2]; row[3] = k * t[i] + o[i]; }
    }
}

void hv_test_inputs(int which, HvCtrl* c) {
    *c = HvCtrl{ 0.f, 0.f, false, false, false, false };
    switch (which) {
        case 1: c->trigger_touch = true; break;                                                    // resting on the trigger
        case 2: c->trigger_touch = true; c->trigger = 1.f; break;                                  // trigger pulled
        case 3: c->squeeze = 1.f; c->thumb_face = true; break;                                     // point: grip, thumb on, trigger off
        case 4: c->squeeze = 1.f; c->trigger_touch = true; c->trigger = 1.f; break;                // thumbs-up
        case 5: c->squeeze = 1.f; c->trigger_touch = true; c->trigger = 1.f; c->thumb_stick = true; break;   // fist
        case 6: c->squeeze = 1.f; break;                                                           // finger-gun
        default: break;                                                                            // open
    }
}

// -- the tap gesture (branch hands-tap; ffxvr_hands.h) ----------------------------------------------------------------

const char* const hv_tap_why[HV_TAP_N] = { "", "tap", "head back", "short", "long", "reach", "sideways", "grip", "head moving", "turn", "gate", "glitch", "gap", "cooldown", "shake" };

int hv_tap_update(HvTapState* s, const HvTapCfg& c, const HvTapIn& in) {
    s->blocked = HV_TAP_NONE;
    auto fin3 = [](const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); };
    auto end = [&](int why, const float* rel, const float* tip, double t) {   // a push ended: remember its numbers; wait for the hand to slow
        float d[3] = { rel[0] - s->r0[0], rel[1] - s->r0[1], rel[2] - s->r0[2] }, dw[3] = { tip[0] - s->tip0[0], tip[1] - s->tip0[1], tip[2] - s->tip0[2] };
        const float tr = v3dot(d, s->f), dl = sqrtf(v3dot(d, d));
        s->last_why = why; s->last_peak = s->peak_s; s->last_travel = tr; s->last_world = v3dot(dw, s->f); s->last_dur = static_cast<float>(t - s->t0);
        s->last_net_deg = dl > 1e-6f ? acosf(clampf(tr / dl, -1.f, 1.f)) / k_deg : 0.f;
        float fh[3] = { s->f[0], 0.f, s->f[2] }; float rt[3] = { 1.f, 0.f, 0.f };
        if (v3norm(fh)) { rt[0] = -fh[2]; rt[2] = fh[0]; }
        const float dh = v3dot(d, fh), dr = v3dot(d, rt);
        s->last_net_yaw = atan2f(dr, dh) / k_deg;
        s->last_net_elev = dl > 1e-6f ? asinf(clampf(d[1] / dl, -1.f, 1.f)) / k_deg : 0.f;
        s->last_net_pitch = s->last_net_elev - asinf(clampf(s->f[1], -1.f, 1.f)) / k_deg;
        s->phase = 2; s->push = 0.f;
        return why;
    };
    if (!in.valid || !std::isfinite(in.t) || !fin3(in.tip) || !fin3(in.head_p) || !fin3(in.head_fwd)) {
        int r = HV_TAP_NONE;
        if (s->phase == 1) r = end(HV_TAP_GAP, s->rel_prev, s->tip_prev, s->t_prev);
        s->have = false; s->have_v = false; s->phase = 0; s->v_f = s->v_abs = 0.f; s->push = 0.f;
        return r;
    }
    const float rel[3] = { in.tip[0] - in.head_p[0], in.tip[1] - in.head_p[1], in.tip[2] - in.head_p[2] };
    auto store = [&]() {
        s->t_prev = in.t; memcpy(s->tip_prev, in.tip, sizeof(s->tip_prev)); memcpy(s->rel_prev, rel, sizeof(rel)); memcpy(s->head_prev, in.head_p, sizeof(s->head_prev)); memcpy(s->fwd_prev, in.head_fwd, sizeof(s->fwd_prev));
        s->have = true;
    };
    if (!s->have) { store(); s->have_v = false; s->phase = 0; return HV_TAP_NONE; }
    const double dtd = in.t - s->t_prev;
    if (dtd == 0.0) return HV_TAP_NONE;                 // the same poses again (no new display time)
    if (!(dtd > 0.0) || dtd > 0.1) {                    // time went back, or a gap (a menu, a load, the hands hidden): start over
        int r = HV_TAP_NONE;
        if (s->phase == 1) r = end(HV_TAP_GAP, s->rel_prev, s->tip_prev, s->t_prev);
        s->phase = 0; s->have_v = false; s->push = 0.f; store();
        return r;
    }
    const float dt = static_cast<float>(dtd);
    const float v[3] = { (rel[0] - s->rel_prev[0]) / dt, (rel[1] - s->rel_prev[1]) / dt, (rel[2] - s->rel_prev[2]) / dt };   // raw: a tracking jump is one frame's
    // the speeds the start, the slowing, and the head use: the newest sample against the latest one >= v_span older (2 frames back at
    // 60-72 Hz, 3 at 90-120 Hz), so a sample's noise counts the same at any rate
    float vs[3], vhs[3], head_w = 0.f, dt_mean = 0.f;
    {
        if (!s->have_v) { s->have_v = true; s->ring_t[0] = s->t_prev; memcpy(s->ring_rel[0], s->rel_prev, 12); memcpy(s->ring_head[0], s->head_prev, 12); memcpy(s->ring_fwd[0], s->fwd_prev, 12); s->ring_n = 1; s->ring_i = 1; }
        if (s->ring_n >= 3) {   // the mean recent frame interval (review 3 N2: tells a frame hitch from a tracking step)
            const int q = (s->ring_i - 1 + 8) % 8, p = (s->ring_i - s->ring_n + 8) % 8;
            dt_mean = static_cast<float>(s->ring_t[q] - s->ring_t[p]) / static_cast<float>(s->ring_n - 1);
        }
        int k = -1;
        for (int j = 1; j <= s->ring_n; ++j) { const int q = (s->ring_i - j + 8) % 8; k = q; if (in.t - s->ring_t[q] >= c.v_span) break; }
        const float dk = static_cast<float>(in.t - s->ring_t[k]);
        for (int i = 0; i < 3; ++i) { vs[i] = (rel[i] - s->ring_rel[k][i]) / dk; vhs[i] = (in.head_p[i] - s->ring_head[k][i]) / dk; }
        const float* f0 = s->ring_fwd[k];
        head_w = acosf(clampf(v3dot(f0, in.head_fwd) / fmaxf(1e-6f, sqrtf(v3dot(f0, f0) * v3dot(in.head_fwd, in.head_fwd))), -1.f, 1.f)) / dk;
        s->ring_t[s->ring_i] = in.t; memcpy(s->ring_rel[s->ring_i], rel, 12); memcpy(s->ring_head[s->ring_i], in.head_p, 12); memcpy(s->ring_fwd[s->ring_i], in.head_fwd, 12);
        s->ring_i = (s->ring_i + 1) % 8; if (s->ring_n < 8) s->ring_n++;
    }
    const float va = sqrtf(v3dot(v, v));
    s->v_abs = sqrtf(v3dot(vs, vs)); s->head_v = sqrtf(v3dot(vhs, vhs)); s->head_w = head_w;
    float fw[3] = { in.head_fwd[0], in.head_fwd[1], in.head_fwd[2] };   // the head's 3-D forward
    if (!v3norm(fw)) { fw[0] = 0.f; fw[1] = 0.f; fw[2] = -1.f; }
    const bool head_bad = s->head_v > c.head_v || s->head_w > c.head_w;
    // review 3 N2: a sample after a frame hitch (over 50 ms and over 2.5 x the mean recent interval; the display clock's own
    // uneven steps with a repeated time reach ~42 ms) carries several frames' motion: its step counts for the tracking-step
    // check as spread over those frames (a tracking step comes in one ordinary frame)
    const float step_w = (dt > 0.050f && dt_mean > 0.f && dt > 2.5f * dt_mean) ? dt_mean / dt : 1.f;
    int r = HV_TAP_NONE; bool started = false;
    if (s->phase == 0) {
        const float vf = v3dot(vs, fw); s->v_f = vf;
        if (vf > c.v_start) {
            int why = HV_TAP_NONE;
            if (va > c.v_glitch) why = HV_TAP_GLITCH;
            else if (!in.pose) why = HV_TAP_POSE;      // not armed (the grip)
            else if (!in.gate) why = HV_TAP_GATE;
            else if (in.turning) why = HV_TAP_TURN;
            else if (head_bad) why = HV_TAP_HEAD;
            else if (in.t < s->cool_until) why = HV_TAP_COOLDOWN;
            if (why != HV_TAP_NONE) { s->blocked = why; s->phase = 2; }   // not later in this stroke either
            else {
                s->phase = 1; s->t0 = s->t_prev; memcpy(s->r0, s->rel_prev, sizeof(s->r0)); memcpy(s->tip0, s->tip_prev, sizeof(s->tip0)); memcpy(s->f, fw, sizeof(s->f));
                s->peak_s = vf; s->step_max = fmaxf(0.f, v3dot(v, fw) * dt * step_w); s->n = 1; s->fail = HV_TAP_NONE; started = true;
            }
        }
    }
    if (s->phase == 1 && !started) {
        const float vf = v3dot(vs, s->f); s->v_f = vf;
        s->n++;
        int why = HV_TAP_NONE;
        if (va > c.v_glitch) why = HV_TAP_GLITCH;
        else if (!in.gate) why = HV_TAP_GATE;
        else if (!in.pose) why = HV_TAP_POSE;          // the grip released
        else if (in.turning) why = HV_TAP_TURN;
        else if (head_bad) why = HV_TAP_HEAD;
        if (why != HV_TAP_NONE) r = end(why, rel, in.tip, in.t);
        else {
            if (vf > s->peak_s) s->peak_s = vf;
            { const float st = v3dot(v, s->f) * dt * step_w; if (st > s->step_max) s->step_max = st; }
            const float d[3] = { rel[0] - s->r0[0], rel[1] - s->r0[1], rel[2] - s->r0[2] };
            const float dw[3] = { in.tip[0] - s->tip0[0], in.tip[1] - s->tip0[1], in.tip[2] - s->tip0[2] };
            const float travel = v3dot(d, s->f), dl = sqrtf(v3dot(d, d)), world = v3dot(dw, s->f);
            s->push = fmaxf(0.f, travel);
            if (travel >= c.tap_dist) {
                if (dl > 1e-6f && travel < cosf(c.dir_max_deg * k_deg) * dl) s->fail = HV_TAP_SIDEWAYS;   // the net displacement off the forward
                else if (world < c.world_frac * c.tap_dist) s->fail = HV_TAP_SLOW;                     // the head moved back, not the hand
                else if (s->step_max > c.step_frac * travel) s->fail = HV_TAP_GLITCH;                  // one sample did most of it: a tracking step
                else if (s->n < c.min_samples) s->fail = HV_TAP_GLITCH;
                else { s->cool_until = in.t + c.cooldown; r = end(HV_TAP_FIRED, rel, in.tip, in.t); }
            }
            if (r == HV_TAP_NONE) {
                if (vf < 0.5f * c.v_start) r = end(s->fail != HV_TAP_NONE ? s->fail : HV_TAP_SHORT, rel, in.tip, in.t);        // slowed or came back
                else if (in.t - s->t0 > c.max_time) r = end(s->fail != HV_TAP_NONE ? s->fail : HV_TAP_REACH, rel, in.tip, in.t);   // not there in time
            }
        }
    }
    else if (s->phase == 2) {   // after a push: the next may start once the hand has slowed or come back
        const float vf = v3dot(vs, fw); s->v_f = vf;
        if (vf < 0.5f * c.v_start) s->phase = 0;
    }
    store();
    return r;
}

int hv_grip_strip(bool held_back[2], const bool down[2], bool hold) {
    int pass = 0;
    for (int h = 0; h < 2; ++h) {
        if (!down[h]) held_back[h] = false; else if (hold) held_back[h] = true;
        if (down[h] && !held_back[h]) pass |= 1 << h;
    }
    return pass;
}

int hv_tap_hook(HvTapHook* h, bool pressed, uint32_t pkt, int32_t el_ms, int32_t press_ms, int32_t polls) {
    const bool new_pkt = pkt != h->pkt; h->pkt = pkt;
    if (!pressed) { if (new_pkt) h->need_clear = false; return 0; }   // a state without the button reached the game
    if (el_ms < 0 || el_ms >= k_hv_tap_cap_ms) { h->need_clear = !new_pkt; return HV_HOOK_RELEASE; }   // the cap, or a clock that went back
    if (h->need_clear) { if (new_pkt) h->need_clear = false; return 0; }   // the last press's release first (this new packet carries it)
    const int count = new_pkt ? HV_HOOK_COUNT : 0;
    if (el_ms >= press_ms && polls >= 3) { h->need_clear = !new_pkt; return HV_HOOK_RELEASE; }   // 3 new packets delivered: this poll goes out without it
    return HV_HOOK_HOLD | count;
}
