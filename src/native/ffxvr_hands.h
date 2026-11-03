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
    float abd[4][3] = {};                                 // tap build: each finger's (index .. little) sideways turn at the bind pose, radians about
                                                          // the dorsal axis, in right-hand terms: [0] the proximal bone from the hand's long axis,
                                                          // [1] / [2] the intermediate / distal bone from its parent; a curl takes it out
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
/// S3: reads one hand's file into `out` (h 0 left, 1 right); false with err (without the file name) on failure.
typedef bool (*HvPackRead)(void* ctx, int h, std::vector<uint8_t>* out, char* err, int errn);
/// S3: both hands of a pack ([0] left, [1] right, as HvMesh::hand; have[h]: that file is named). right + mirror (or right
/// only): the left is the right mirrored; left only: the right is the left mirrored; both and no mirror: both parsed (a
/// two-handed pack). Each file is read through `read`, parsed and its bytes freed before the next is read (review SF-3: one
/// file buffer at a time in a 32-bit process). The error names the file (names[h], e.g. "left.glb"). Can throw std::bad_alloc.
bool hv_pack_parse(const bool have[2], bool mirror, const float* tint, HvPackRead read, void* ctx, const char* const names[2],
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
// ---------------------------------------------------------------------------------------------------------------------
// The tap gesture (branch hands-tap): a quick forward jab of a pointing hand presses the game's confirm button. Per hand,
// on the frames the hands are drawn (hands_draw_frame), from the fingertip and the head in reference space (metres, the
// display time of the latched poses). Pure: no globals, so the unit test drives it with synthetic pose streams.
//
// The jab (retuned after the owner's first headset round, 3 Nov 2026: his natural poke is a long, fast, slightly arcing jab of
// 15-30 cm, often after a small wind-up): the fingertip's velocity RELATIVE TO THE HEAD (so a lean or a step that carries
// the arm along is no jab), measured over >= 25 ms, along the head's forward with its pitch kept within -60..+30 deg (a
// poke down at a chest you look at counts), rises past v_start, peaks at >= v_on, and falls below stop_frac x the peak (the
// stop, or the start of the retraction) within max_dur. Then the WHOLE jab is judged: its net displacement within dir_max
// of that forward (no single noisy or arcing sample decides), min_travel..max_travel forward, the fingertip's own forward
// peak >= world_frac x v_on (the head moving back alone is no jab), and no one-frame jolt (a raw peak far above the 25 ms
// peak: a tracking step). Throughout: the point pose (held pose_hold before the start), the head slower than head_v /
// head_w, the stick not turning, the caller's gate open, no raw speed above v_glitch. Then a cooldown. A jab that fails is
// "not a tap" (counted by reason); a start that is blocked is not started later in the same stroke; the next may start once
// the forward speed has dropped below v_start / 2. A shake (forward, back, forward with no pause) is blocked; a single
// wind-up before a poke is not.
struct HvTapCfg {
    float v_on = 0.70f;        // m/s: the peak forward speed of a tap (sensitivity 1; the F8 slider divides it)
    float v_start = 0.35f;     // m/s: a jab starts above this forward speed
    float stop_frac = 0.40f;   // the jab ends when the forward speed falls below this x its peak
    float min_travel = 0.035f, max_travel = 0.50f;   // m, forward, from the start to the stop
    float min_dur = 0.05f, max_dur = 0.36f;           // s, from the start to the stop (and at least 3 samples)
    float cooldown = 0.40f;    // s after a tap
    float dir_max_deg = 50.f;  // the jab's net displacement within this of the forward
    float pitch_min_deg = -60.f, pitch_max_deg = 30.f;   // the forward follows the head's pitch within these (a level forward when looking far up or down)
    float head_v = 0.50f;      // m/s: the head moving faster blocks a jab (and aborts one)
    float head_w = 1.75f;      // rad/s (100 deg/s): the head turning faster blocks a jab
    float v_glitch = 5.0f;     // m/s: faster is a tracking jump, not a hand
    float jolt_ratio = 1.6f;   // a raw forward peak above this x the two-frame peak: a one-frame step
    float pose_hold = 0.08f;   // s: the point pose held before a jab may start (the caller's pose is quick: the raw touch bit, debounced 30 ms)
    float back_lock = 0.12f;   // s (review S1): a start this soon after a backward stroke (> 0.6 x v_start toward the head) ...
    float shake_gap = 0.30f;   // s ... that itself followed a forward stroke within this: a shake (forward, back, forward)
    float v_span = 0.025f;     // s: the speeds are measured over at least this (the newest sample against one >= this old)
    float world_frac = 0.50f;  // review S2: the fingertip itself (not only relative to the head) must peak at >= this x v_on forward
};
struct HvTapIn {
    double t;                  // seconds (the display time of the poses)
    float tip[3];              // the index fingertip, reference space (m)
    float head_p[3];           // the head (m)
    float head_fwd[3];         // the head's forward (-Z of its pose), unit
    bool valid;                // the hand is tracked this frame
    bool pose;                 // the point pose (index off the trigger, the other fingers gripped)
    bool gate;                 // the caller's gate: the field, the player in control, ... (false blocks a start and aborts a jab)
    bool turning;              // the stick is turning the view
};
enum HvTapWhy { HV_TAP_NONE = 0, HV_TAP_FIRED, HV_TAP_SLOW, HV_TAP_SHORT, HV_TAP_LONG, HV_TAP_REACH, HV_TAP_SIDEWAYS, HV_TAP_POSE, HV_TAP_HEAD,
                HV_TAP_TURN, HV_TAP_GATE, HV_TAP_GLITCH, HV_TAP_GAP, HV_TAP_COOLDOWN, HV_TAP_SHAKE, HV_TAP_N };
extern const char* const hv_tap_why[HV_TAP_N];   // short names for the log ("slow", "short", ...)
struct HvTapState {
    int    phase = 0;          // 0 idle, 1 in a jab, 2 waiting for the forward speed to drop (after a tap or a non-tap)
    bool   have = false;       // a previous sample
    double t_prev = 0.0, cool_until = -1e9, t0 = 0.0;
    float  rel_prev[3] = {}, head_prev[3] = {}, fwd_prev[3] = {}, r0[3] = {}, f[3] = {}, fw_last[3] = { 0.f, 0.f, -1.f };
    bool   have_v = false; double ring_t[8] = {}; float ring_rel[8][3] = {}, ring_tip[8][3] = {}; int ring_n = 0, ring_i = 0;   // the last samples: the speeds used
                                                                 // span >= 25 ms (2 frames at 60-72 Hz, 3 at 90-120 Hz: the same noise at any rate)
    float  tip_prev[3] = {};
    float  peak_s = 0.f, peak_raw = 0.f, peak_w = 0.f;           // in the jab: the forward peak (two-frame), the raw one, the fingertip's own (S2)
    float  worst_deg = 0.f;                                       // in the jab: the largest angle of the (two-frame) velocity from the forward, for the log
    bool   in_fwd = false, in_back = false;                       // stroke episodes (two-frame forward speed beyond +-0.6 v_start), for the shake rule
    double t_fwd_end = -1e9, t_back_start = -1e9, t_back_end = -1e9;
    float  pose_s = 0.f;
    int    n = 0;
    // the last sample and the last outcome, for the F8 readout and the log
    float  v_f = 0.f, v_abs = 0.f, head_v = 0.f, head_w = 0.f;
    float  last_peak = 0.f, last_travel = 0.f, last_dur = 0.f;   // of the last jab that ended (a tap or not)
    float  last_net_deg = 0.f, last_net_yaw = 0.f, last_net_pitch = 0.f, last_worst_deg = 0.f;   // its net displacement's angle from the forward
                                                                 // (and its yaw / pitch against it, + right / up), its worst sample's angle
    int    last_why = HV_TAP_NONE;                               // how it ended
    int    blocked = HV_TAP_NONE;                                // this sample: why a jab could not start (forward speed above v_start but ...)
};
/// One sample. Returns HV_TAP_FIRED on the sample that completes a tap, else HV_TAP_NONE, or the reason a jab just ended
/// without one (st->last_why holds it too).
int hv_tap_update(HvTapState* st, const HvTapCfg& c, const HvTapIn& in);

/// Review S5: the held button in the game's XInputGetState (the input thread), one call per poll of pad 0. The game's pad poll
/// (0x62DBA0) ignores a state whose packet number did not change, so: a press is ORed in until press_ms has passed AND the
/// game got 3 new packets with it, never at or past k_hv_tap_cap_ms (or with a clock that went back); after a release, one new
/// packet without the button must reach the game before the next press is ORed (so two presses never merge, review N6).
/// polls: the new packets counted so far for this press (the caller keeps it, adding 1 on HV_HOOK_COUNT).
enum { HV_HOOK_HOLD = 1, HV_HOOK_RELEASE = 2, HV_HOOK_COUNT = 4 };
enum { k_hv_tap_cap_ms = 400 };
struct HvTapHook { uint32_t pkt = 0; bool need_clear = false; };
int hv_tap_hook(HvTapHook* h, bool pressed, uint32_t pkt, int32_t el_ms, int32_t press_ms, int32_t polls);

/// The open / point / fist test poses for the previews and the unit test (the same code path as the runtime).
void hv_test_inputs(int which, HvCtrl* c);   // 0 open (nothing touched), 1 rest on the trigger, 2 trigger pulled, 3 point (grip, thumb on),
                                             // 4 thumbs-up (grip + trigger, thumb off), 5 fist (everything), 6 finger-gun (grip only)
