// SPDX-License-Identifier: MIT
//
// VR hands v2, the CPU half (docs: research hands-v2-spec.md, stages S1 and S2). Plain structs in, arrays out: no D3D, no
// context, no globals of ffxvr_native.cpp, so everything here can be reasoned about and tested on its own (the S2 test
// builds this file with g++ on Linux; the Windows-only parts, the file read and the WIC decode, are under _WIN32).
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
#include <cstddef>
#include <cstdint>
#include <vector>

struct HvVertex    { float p[3]; float n[3]; float uv[2]; uint32_t rgba; };   // the GPU vertex: world space, game units (36 B)
static_assert(sizeof(HvVertex) == 36, "HvVertex is the input layout's 36-byte vertex");
struct HvSrcVertex { float p[3]; float n[3]; uint32_t rgba; int bone; };      // mesh space (metres); bone 0 the hand frame, 1 the grip
struct HvXform     { float R[9]; float t[3]; };                               // rigid: x' = R x + t
struct HvBone      { float R[9]; float o[3]; float s; };                      // to the world: p' = o + s R p, n' = R n

/// The S1 box hand into `out` (at most `max` vertices): a 9 x 3 x 9 cm palm box centred on the palm, a 2 x 2 x 8 cm finger
/// slab distal of it (bone 0), a 1 x 1 x 7 cm stub along the grip's -Z from the grip's origin (bone 1), and with `axes` the
/// hand frame's axes, 6 cm each: +X red, +Y green, +Z blue (bone 0). Triangle list, counter-clockwise seen from outside.
/// Returns the vertex count (0 if `max` is too small).
int  hv_box_mesh(HvSrcVertex* out, int max, bool axes, bool boxes = true);   // boxes false: the axes alone (with the mesh)
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
/// points at the other hand (or along the head's yaw-turned +-x with one controller). Writes the grip -> hand rotation (Rg^T Rt) and returns false
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

// ---------------------------------------------------------------------------------------------------------------------
// S2: hand packs (glb), the controller-driven finger pose, forward kinematics and the skinning palette.
//
// Skeleton: the 26 OpenXR XR_EXT_hand_tracking joints in XrHandJointEXT order (palm 0, wrist 1, thumb 2-5, index 6-10,
// middle 11-15, ring 16-20, little 21-25). Mesh space: metres, the hand's OpenXR wrist-joint space at the bind pose
// (origin at the wrist, -Z toward the fingertips, +Y out of the back of the hand); a left hand is the X mirror of a right.
// Joint frames follow OpenXR (+Y dorsal, +Z along the bone away from the tip, X = Y x Z). The pack contract is the one
// tools/hands/handlib.py writes (research hands-assets.md 1).

enum { HV_J = 26, HV_PALM = 0, HV_WRIST = 1, HV_MAX_VERTS = 65535, HV_MAX_INDICES = 393216, HV_MAX_TEX = 2048, HV_MAX_IMAGES = 4 };   // per hand; a hand needs ~6 k indices
extern const int  hv_parent[HV_J];
extern const char* const hv_joint_name[HV_J];

/// The skinned GPU vertex (44 B): mesh space; joints 0..25 (canonical), weights unorm8 summing to 255.
struct HvSkinVertex { float p[3]; float n[3]; float uv[2]; uint32_t rgba; uint8_t j[4]; uint8_t w[4]; };
static_assert(sizeof(HvSkinVertex) == 44, "HvSkinVertex is the skinned input layout's 44-byte vertex");
struct HvBatch { uint32_t first, count; int tex; };   // an index range drawn with one texture (-1: none, the vertex colour alone)
struct HvImage {                                      // decoded RGBA8 (R in the low byte)
    int w = 0, h = 0; std::vector<uint32_t> rgba;     // mip 0
    std::vector<std::vector<uint32_t>> mips;          // S3: levels 1.. (each side halved, at least 1), built on the loader thread
    int src_w = 0, src_h = 0;                         // the size as decoded (larger than w x h when hv_image_prepare shrank it)
};
/// S3 (on the loader thread, not at Present): halves the image (box filter) until both sides are <= max_side, then builds the
/// box-filtered mip chain down to 1 x 1 (at most 12 levels in all). An image with a bad size is emptied (the base colour).
void hv_image_prepare(HvImage* im, int max_side);
/// One hand of a pack, on the CPU.
struct HvMesh {
    std::vector<HvSkinVertex> v; std::vector<uint32_t> idx; std::vector<HvBatch> batch;
    std::vector<std::vector<uint8_t>> tex_file;           // the encoded images (PNG / JPEG bytes from the glb), by HvBatch::tex
    std::vector<HvImage> tex;                             // ... decoded (hv_wic_decode); an empty one draws as the base colour
    HvXform bind[HV_J], local[HV_J], inv_bind[HV_J];      // the bind frames in mesh space, their parent-relative locals, the inverses
    float thumb_axis[3] = { 0.f, 1.f, 0.f };              // the opposition axis in the thumb metacarpal's frame (in right-hand terms)
    float thumb_up[3] = { 0.f, 1.f, 0.f };                // the wrist's +Y (dorsal) in that frame: the thumbs-up swing toward the radial side
    float hand_len = 0.f;                                 // wrist -> middle tip at the bind pose (m), for the log
    int   prims = 0, tris = 0, unknown_joints = 0;        // glTF primitives read, triangles, skin joints not in the 26 (weights moved to the wrist)
    int   hand = 1;                                       // 0 left, 1 right
    int   tex_from = -1;                                  // >= 0: this hand uses that hand's textures (a mirror: nothing decoded or uploaded twice)
    int   images_dropped = 0;                             // images past HV_MAX_IMAGES (those batches draw the base colour)
    bool  palm_rebuilt = false, mirrored = false;
};

/// Parses a glb (glTF 2.0 binary: one skinned mesh, embedded buffer and images only) into `out`. `h` 0 left, 1 right: the
/// hand the file is for (only used for the thumb's opposition axis). `tint` multiplies the base colour (null: none). On
/// failure returns false with the reason in err (e.g. "joint 'thumb_tip' missing"). Every index is checked before use.
/// Caps (a 32-bit process; a bad file must not reach an out-of-memory): HV_MAX_VERTS vertices and HV_MAX_INDICES indices per
/// hand, HV_MAX_IMAGES images (the rest draw their base colour); compressed glbs (Draco, meshopt) and other required
/// extensions are refused. Can throw std::bad_alloc (the caller catches).
bool hv_glb_parse(const void* data, size_t size, int h, const float* tint, HvMesh* out, char* err, int errn);
/// S3: both hands of a pack from its files' bytes (null / 0: that file is not given; [0] left, [1] right, as HvMesh::hand).
/// right + mirror (or right only): the left is the right mirrored; left only: the right is the left mirrored; both and no
/// mirror: both parsed (a two-handed pack). The error names the file (names[h], e.g. "left.glb"). Can throw std::bad_alloc.
bool hv_pack_parse(const uint8_t* const data[2], const size_t size[2], const char* const names[2], bool mirror, const float* tint,
                   HvMesh hand[2], char* err, int errn);
/// The other hand from this one: x negated in positions and normals, the winding reversed, every bind frame conjugated
/// with S = diag(-1, 1, 1) (spec 2 row 7). The textures are not copied: dst->tex_from names the hand whose textures it uses.
void hv_mesh_mirror(const HvMesh& src, HvMesh* dst);
/// The bind's parent-relative locals and inverses from `bind`, and the thumb's opposition axis (`h` 0 left, 1 right).
void hv_mesh_finish(HvMesh* m, int h);
#if defined(_WIN32)
/// Reads a whole file (wide path, at most max_bytes); false with err on failure.
bool hv_read_file(const wchar_t* path, size_t max_bytes, std::vector<uint8_t>* out, char* err, int errn);
/// Decodes PNG / JPEG bytes with WIC into RGBA8 (the calling thread must have COM initialised); larger than HV_MAX_TEX
/// in either side, or a decode failure: false.
bool hv_wic_decode(const uint8_t* data, size_t size, HvImage* out, char* err, int errn);
#endif

/// The controller inputs one hand's fingers follow (research hands-input.md 4.1).
struct HvCtrl { float trigger, squeeze; bool trigger_touch, thumb_stick, thumb_rest, thumb_face; };
/// Per hand, across frames: the smoothed inputs (4.3) and the "put down" detector.
struct HvFingerState {
    bool  init = false;
    float trig = 0.f, trig_t = 0.f, sq = 0.f, thumb_t = 0.f, face_t = 0.f, rest_t = 0.f;   // smoothed: trigger, trigger touch, squeeze, thumb touch, on the face buttons, on the thumbrest
    float off_s[4] = { 1e9f, 1e9f, 1e9f, 1e9f };          // S3: seconds since each touch (trigger, thumb, face, rest) was last on: a release holds 50 ms
    float curl[5] = {};                                   // the last solve: thumb (its touch blend), index, middle, ring, little
    float still_s = 0.f; float anchor_p[3] = {}; float anchor_R[9] = {}; bool anchor = false; bool put_down = false;
};
/// One step of the smoothing (dt in seconds from the display times; the first call, dt < 0 or > 0.25 s snaps; 0 holds).
/// S3: a touch must be off for 50 ms before its finger starts to open (a sensor flicker no longer twitches it), then opens
/// with a 50 ms time constant (S2: at once, 70 ms; the 10 % point is ~165 ms either way); the grip has a 0.04 dead zone.
void hv_fingers_update(HvFingerState* s, const HvCtrl& in, float dt);
/// The "controller put down" rule (spec S2): the grip moved < 5 mm and < 2 deg for 10 s and nothing is touched. Returns
/// the state (true: hide this hand). grip_R / grip_p in reference space.
bool hv_put_down_update(HvFingerState* s, const HvCtrl& in, const float grip_R[9], const float grip_p[3], float dt);
/// The local rotation deltas (applied after each joint's bind local) for hand h from the smoothed state: the curls and
/// maxima of hands-input.md 4.2, the thumb's 2x2 key-pose blend. rest_curl: the user's idle curl of the middle, ring and
/// little fingers; pack_rest (5, may be null): the pack's per-finger offsets. Left-hand deltas are the mirror (S D S).
void hv_pose_deltas(HvFingerState* s, const HvMesh& m, int h, float rest_curl, const float* pack_rest, float D[HV_J][9]);
/// Forward kinematics on the pack's own bones and the skinning matrices skin_j = posed_j . inv_bind_j (mesh space).
void hv_fk_skin(const HvMesh& m, const float D[HV_J][9], HvXform skin[HV_J], HvXform* posed /* HV_J, may be null */);
/// The bone palette of one hand for the vertex shader (HV_J x 3 rows of world = row . (p, 1)): the pack's palm joint (Bp) is
/// put on the hand frame H (reference space, metres; spec 2 row 1: the palm is the anchor) and scaled about it by pack_scale,
/// then mapped to the world as the S1 boxes are: w = P + Rb^T (sc r + mv') + (0, dy, 0). So world = o + k Rw (skin_j p) with
/// Rw = Rb^T H.R Bp.R^T, k = sc pack_scale, o = P + Rb^T (sc H.t + mv') - k Rw Bp.t + (0, dy, 0).
void hv_palette(const HvXform skin[HV_J], const HvXform& H, const HvXform& Bp, float pack_scale, const float P[3], const float Rb[9], float sc,
                const float mvp[3], float dy, float out[HV_J * 3][4]);
/// The open / point / fist test poses for the previews and the unit test (the same code path as the runtime).
void hv_test_inputs(int which, HvCtrl* c);   // 0 open (nothing touched), 1 rest on the trigger, 2 trigger pulled, 3 point (grip, thumb on),
                                             // 4 thumbs-up (grip + trigger, thumb off), 5 fist (everything), 6 finger-gun (grip only)
