// SPDX-License-Identifier: MIT
//
// VR hands v2, the CPU half: the S1 box mesh, rigid skinning, the grip -> hand placement and the calibration capture.
// See ffxvr_hands.h for the frames and conventions. No D3D, no globals of ffxvr_native.cpp.
#include "ffxvr_hands.h"

#include <cmath>
#include <cstring>

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
    else { t[0] = (h == 0) ? 1.f : -1.f; t[1] = 0.f; t[2] = 0.f; }   // one controller: toward the heading's right (left hand) / left
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

int hv_box_mesh(HvSrcVertex* out, int max, bool axes) {
    if (out == nullptr || max < 36 * 6) return 0;
    int n = 0;
    { const float c[3] = { 0.f, 0.f, 0.f },     e[3] = { 0.045f, 0.015f, 0.045f }; n = add_box(out, n, max, c, e, rgba(0.62f, 0.62f, 0.66f), 0); }   // palm
    { const float c[3] = { 0.f, 0.f, -0.085f }, e[3] = { 0.010f, 0.010f, 0.040f }; n = add_box(out, n, max, c, e, rgba(0.80f, 0.80f, 0.84f), 0); }   // finger slab: z -0.045 .. -0.125
    { const float c[3] = { 0.f, 0.f, -0.035f }, e[3] = { 0.005f, 0.005f, 0.035f }; n = add_box(out, n, max, c, e, rgba(0.95f, 0.70f, 0.20f), 1); }   // stub: the grip's -Z, 0 .. -7 cm
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
