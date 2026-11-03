// SPDX-License-Identifier: MIT
//
// VR hands v2, the CPU half (docs: research hands-v2-spec.md, stage S1). Plain structs in, arrays out: no D3D, no
// context, no globals of ffxvr_native.cpp, so everything here can be reasoned about (and later tested) on its own.
//
// Frames and units:
//   reference space  the recentred OpenXR space (ffxvr_native.cpp hand_to_ref): metres, x right, y up, z back.
//   grip frame       the controller's grip pose (OpenXR: -Z along the handle, little finger -> thumb; +X normal to the palm,
//                    away from it in the left hand, into it in the right hand).
//   hand frame       the OpenXR hand-joint convention at the palm: +Y out of the back of the hand, -Z toward the fingers
//                    (distal), +X = Y x Z. The same convention for both hands (so +X is the little-finger side of a right
//                    hand held palm down, and the thumb side of a left one).
//   world            the game's render space (y up), game units: w = P + Rb^T (sc r + mv'), see ffxvr_native.cpp.
// Matrices are row-major 3x3 with the frame's axes as COLUMNS (x' = R x), as quat_to_mat in ffxvr_native.cpp.
#pragma once
#include <cstdint>

struct HvVertex    { float p[3]; float n[3]; float uv[2]; uint32_t rgba; };   // the GPU vertex: world space, game units (36 B)
static_assert(sizeof(HvVertex) == 36, "HvVertex is the input layout's 36-byte vertex");
struct HvSrcVertex { float p[3]; float n[3]; uint32_t rgba; int bone; };      // mesh space (metres); bone 0 the hand frame, 1 the grip
struct HvXform     { float R[9]; float t[3]; };                               // rigid: x' = R x + t
struct HvBone      { float R[9]; float o[3]; float s; };                      // to the world: p' = o + s R p, n' = R n

/// The S1 box hand into `out` (at most `max` vertices): a 9 x 3 x 9 cm palm box centred on the palm, a 2 x 2 x 8 cm finger
/// slab distal of it (bone 0), a 1 x 1 x 7 cm stub along the grip's -Z from the grip's origin (bone 1), and with `axes` the
/// hand frame's axes, 6 cm each: +X red, +Y green, +Z blue (bone 0). Triangle list, counter-clockwise seen from outside.
/// Returns the vertex count (0 if `max` is too small).
int  hv_box_mesh(HvSrcVertex* out, int max, bool axes);
/// Rigid skinning: every source vertex through its bone (an out-of-range bone index gives the vertex bone 0).
void hv_skin_rigid(const HvSrcVertex* src, int n, const HvBone* bones, int nb, HvVertex* dst);

/// Default_h, the grip -> hand transform before any calibration (h: 0 left, 1 right). Meta's palm pose as VDXR records it
/// for the Touch controller (research hands-input.md 2.6: grip . Rx(-70 deg) at (-1.0, +1.3, +0.75) cm for the left hand, the
/// X mirror for the right), turned into the hand-joint axes: Meta's palm pose keeps the grip's X (the palm normal) and has
/// +Y toward the thumb, so the joint frame is that pose . Rz(+90 deg) for the left hand and . Rz(-90 deg) for the right.
void hv_default_grip_to_hand(int h, HvXform* out);
/// Trim_h: the user's fine tune in the hand frame, yaw (about +Y), pitch (+X), roll (+Z) as Ry Rx Rz, and an offset
/// (out, up, back) along (+X, +Y, +Z). Yaw, roll and the out offset are mirrored for the left hand (h 0).
void hv_trim(int h, const float rad[3], const float off_m[3], HvXform* out);
void hv_xform_mul(const HvXform& a, const HvXform& b, HvXform* o);   // o = a . b (b first)

/// Calibration capture (spec 2 row 1b), all in reference space: the arms straight out in front, palms down, fingers forward.
/// The fingers point from an estimated shoulder (head + yaw . (-+0.17, -0.13, +0.04) m) through the grip; the thumb side
/// points at the other hand (or along +-x with one controller). Writes the grip -> hand rotation (Rg^T Rt) and returns false
/// on a degenerate pose (the hand at the shoulder, both hands in one spot).
bool hv_cal_capture(int h, const float grip_R[9], const float grip_p[3], bool other_valid, const float other_p[3],
                    const float head_p[3], float head_yaw, float out_R[9]);
/// The angle in degrees between two rotations (of A^T B).
float hv_rot_angle_deg(const float A[9], const float B[9]);
/// Gram-Schmidt on the columns (x, then y, z = x cross y); false when degenerate or not finite.
bool hv_orthonormalize(float R[9]);
/// 9 floats that are a rotation within `tol` (columns unit and orthogonal, det +1).
bool hv_is_rotation(const float R[9], float tol);

void hv_quat_to_mat(const float q[4], float M[9]);
void hv_mat3_mul(const float* A, const float* B, float* O);   // O = A B
void hv_mat3_T(const float* A, float* O);
void hv_mat3_vec(const float* A, const float* v, float* o);
