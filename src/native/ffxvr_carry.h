// SPDX-License-Identifier: MIT
#pragma once
// ffxvr_carry.h -- the carry component: "draw a game model's own draws at a VR hand" (research/weapon-hands-design.md section 6).
// Its first user is the Cloister of Trials sphere Tidus carries (research/sphere-hands-design.md 9.5 / 10.4); the battle
// weapons follow. Pure logic: no Windows, no D3D, no globals; the glue is in ffxvr_native.cpp ("Carry"), managed in carry.cs.
// Unit-tested in scratch (carry_chk/carry_test.cpp).
//
// What it does: a slot holds a model's index-buffer keys (its segments: buffer, byte offset, index count), a transform and
// flags. At a draw of that model the constant-buffer images of both eyes are changed so the model is drawn at the VR hand,
// with the game's own shaders, textures and states: nothing is re-drawn, nothing is written to the game.
//
// The layout it moves (ffxvr_native.cpp write_camera, which rewrites the same blocks for the headset on every character-class
// upload): relative to the view block `base` (64, or 48 in the shifted layout) a buffer of the skinned layout (>= 1872 bytes)
// carries the view at +0, the near-variant projection at +64, the inverse view at +256, and at wb = base + 576 the world
// family: W (+0; row-major 4 x 4 for column vectors, the translation in elements 3, 7, 11), V W (+64), W^-1 (+128), P V W
// (+192) and W^-1 V^-1 (+256); then the bone palette (float4x4[80], affine, in WORLD space) at +1344 / +1360 / +1456. A
// rigid move D of the model is W' = D W, the three products rebuilt from W' with the image's own camera, W^-1' = W^-1 D^-1,
// and every palette slot M' = D M. Each product is rewritten only when the image's block is that product (write_camera's own
// rule), so a variant that does not carry one keeps its bytes.
#include <cmath>
#include <cstdint>
#include <cstring>

namespace carry {

constexpr int k_slots = 4;       // 0 a weapon, 1 an off-hand item, 2 the carried sphere, 3 spare
constexpr int k_keys = 16;       // segments of one model
constexpr int k_avoid = 24;      // other actors of the same model (their roots)
constexpr float k_tol_max = 12.f;
constexpr int k_passes = 4;      // passes of a model a frame, each with uploads of its own (a second scene camera, a reflection, a glow or depth pass)
constexpr int k_pal_slots = 80;  // the skinned layout's bone palette: float4x4[80]

enum : uint32_t {
    F_ON = 1,       // the caller wants the model carried
    F_HAND = 2,     // m is a frame in the hand's palm space (metres): the model's root is put at palm . m (its own scale kept).
                    // Without it m is the move D itself, in render space
    F_HIDE = 4,     // the matched draws are skipped instead of moved (an identification test; a stand-in's fallback)
    F_NEAR = 8,     // the model is shared (every Djose sphere is one mesh): a draw is the carried one only when its world
                    // translation is within tol of root and nearer root than every avoid point
    F_SPHERE = 16,  // the hand and the on / off follow the sphere-hands state (SphereGate)
};

// What managed sends (carry.cs CarryDesc; x86, 4-byte fields; native checks bytes at every push).
struct Desc {
    int32_t  bytes, slot;
    uint32_t flags;
    int32_t  hand;        // 0 left, 1 right (F_HAND without F_SPHERE)
    float    m[12];       // 3 x 4 rows (R | t)
    float    root[3];     // the model's drawn root, render space (F_NEAR)
    float    tol;         // units (F_NEAR)
    int32_t  nkeys, navoid;
};
static_assert(sizeof(Desc) == 88, "the layout shared with carry.cs");

struct Key { uint32_t ib, off, count; };

// One draw's verdict. V_* act; R_* leave the draw as the game issued it.
enum Verdict : int { V_NONE = 0, V_MOVE, V_HIDE, R_NO_IMAGE, R_FAR, R_OTHER, R_KEY, R_COUNT, R_FULL, R_NO_HAND, R_LAYOUT, V_N };
inline const char* verdict_name(int v) {
    static const char* const k[V_N] = { "none", "moved", "hidden", "no constant-buffer image with a world matrix", "no draw near the carried root",
                                        "nearer another actor of the model", "not the model's index buffer", "the index count differs", "more buffers than 4 passes of the model's segments",
                                        "the hand is not drawn", "the buffer's layout or matrix was refused" };
    return v >= 0 && v < V_N ? k[v] : "?";
}
struct Stat { uint32_t n[V_N]; uint32_t translated; float d_min; };   // per frame: draws per verdict; images moved by a translation only; the nearest candidate's distance from root

struct Slot {
    bool have = false; uint32_t frame = 0; uint32_t flags = 0; int hand = -1;
    float m[12] = {}, root[3] = {}, tol = 0.f;
    Key k[k_keys] = {}; int nk = 0;
    float avoid[k_avoid][3] = {}; int na = 0;
    bool act = false; int act_hand = -1;   // the decision for the coming frame (the glue: F_ON, and the gate with F_SPHERE)
    int images = 0;                        // constant-buffer images claimed this frame (at most k_passes * nk)
    Stat st = {}, last = {};
};

inline bool finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }
inline float dist2(const float* a, const float* b) { const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2]; return x * x + y * y + z * z; }

struct Slots {
    Slot s[k_slots];
    /// A push for one slot (keys: 3 per key; avoid: 3 per point). False (the slot off) when the description is malformed.
    bool set(const Desc& d, const uint32_t* keys, const float* avoid, uint32_t frame) {
        if (d.bytes != static_cast<int32_t>(sizeof(Desc)) || d.slot < 0 || d.slot >= k_slots) return false;
        Slot& t = s[d.slot];
        t.have = true; t.frame = frame; t.flags = d.flags; t.hand = d.hand; t.nk = 0; t.na = 0; t.act = false; t.act_hand = -1;
        memcpy(t.m, d.m, sizeof(t.m)); memcpy(t.root, d.root, sizeof(t.root)); t.tol = d.tol;
        bool ok = true;
        for (int i = 0; i < 12; ++i) if (!std::isfinite(t.m[i])) ok = false;
        if ((t.flags & F_NEAR) != 0 && (!finite3(t.root) || !(t.tol > 0.f) || t.tol > k_tol_max)) ok = false;
        if ((t.flags & F_HAND) != 0 && (t.flags & F_SPHERE) == 0 && t.hand != 0 && t.hand != 1) ok = false;
        for (int i = 0; ok && keys != nullptr && i < d.nkeys && t.nk < k_keys; ++i) {
            const uint32_t* e = keys + 3 * i;
            if (e[0] == 0 || e[2] == 0) continue;
            t.k[t.nk].ib = e[0]; t.k[t.nk].off = e[1]; t.k[t.nk].count = e[2]; ++t.nk;
        }
        for (int i = 0; ok && avoid != nullptr && i < d.navoid && t.na < k_avoid; ++i) {
            if (!finite3(avoid + 3 * i)) continue;
            memcpy(t.avoid[t.na++], avoid + 3 * i, sizeof(float) * 3);
        }
        if (!ok) { t.flags = 0; t.nk = 0; }
        return ok;
    }
    /// The frame's counts become the last frame's (at the push, before the new decision).
    void roll(int i) { Slot& t = s[i]; t.last = t.st; t.st = Stat(); t.st.d_min = -1.f; t.images = 0; }
    /// The slot acts on this frame's draws: pushed this frame or the last, wanted, decided on, with keys.
    bool live(int i, uint32_t frame) const {
        const Slot& t = s[i];
        return t.have && t.act && (t.flags & F_ON) != 0 && t.nk > 0 && frame - t.frame <= 1u;
    }
    bool any_live(uint32_t frame) const { for (int i = 0; i < k_slots; ++i) if (live(i, frame)) return true; return false; }
};

/// Step 1 (no D3D call needed): can this draw's constant buffer be the slot's model at all? V_NONE = yes, go on to the key.
/// image: the bound slot-0 buffer has this frame's eye images with a world matrix, and is not a light camera's.
/// w: that world matrix's translation (render space).
inline int prefilter(const Slot& t, bool image, const float* w) {
    if (!image) return R_NO_IMAGE;
    if ((t.flags & F_NEAR) == 0) return V_NONE;
    if (!finite3(w)) return R_NO_IMAGE;
    const float d = dist2(w, t.root);
    if (!(d <= t.tol * t.tol)) return R_FAR;
    for (int i = 0; i < t.na; ++i) if (dist2(w, t.avoid[i]) <= d) return R_OTHER;   // a tie goes to the other actor: never moved
    return V_NONE;
}
/// Step 2: the bound index buffer. The buffer, the byte offset and the index count must all be a segment of the model.
/// claimed: the constant-buffer images this slot already took this frame (a model has one per segment at most in a pass, and
/// the game may draw it in several passes with fresh uploads: k_passes of them are taken, so a second pass's copy does not
/// stay at the game's place; the bound only stops a runaway, the keys and F_NEAR's position are the identification);
/// same_image: this draw's image is one of them (another segment or pass of that upload).
inline int key_verdict(const Slot& t, uint32_t ib, uint32_t off, uint32_t count, int claimed, bool same_image) {
    bool key = false, cnt = false;
    for (int i = 0; i < t.nk; ++i) {
        if (t.k[i].ib != ib || t.k[i].off != off) continue;
        key = true; if (t.k[i].count == count) cnt = true;
    }
    if (!key) return R_KEY;
    if (!cnt) return R_COUNT;
    if (!same_image && claimed >= k_passes * t.nk) return R_FULL;
    return (t.flags & F_HIDE) != 0 ? V_HIDE : V_MOVE;
}

// -- 4 x 4 matrices as the buffers hold them: row-major, column vectors (x' = M x), an affine one's translation in 3, 7, 11 ---
inline void m4_mul(const float* a, const float* b, float* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out[r*4+c] = a[r*4+0]*b[0*4+c] + a[r*4+1]*b[1*4+c] + a[r*4+2]*b[2*4+c] + a[r*4+3]*b[3*4+c];
}
/// ffxvr_native.cpp's same4: within 1 % of b's largest element (at least 0.01).
inline bool m4_same(const float* a, const float* b) {
    float scale = 1.f;
    for (int i = 0; i < 16; ++i) { if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return false; const float v = std::fabs(b[i]); if (v > scale) scale = v; }
    const float tol = 0.01f * scale;
    for (int i = 0; i < 16; ++i) if (std::fabs(a[i] - b[i]) > tol) return false;
    return true;
}
inline bool m4_affine(const float* m) {
    for (int i = 0; i < 12; ++i) if (!std::isfinite(m[i])) return false;
    return m[12] == 0.f && m[13] == 0.f && m[14] == 0.f && m[15] == 1.f;
}
inline void m4_identity(float* m) { for (int i = 0; i < 16; ++i) m[i] = (i % 5 == 0) ? 1.f : 0.f; }
/// The uniform scale of an affine matrix's 3 x 3 (the mean column length); ok = a rotation times one scale within tol.
inline float m4_scale(const float* m, bool* ok, float tol = 0.02f) {
    float len[3];
    for (int c = 0; c < 3; ++c) len[c] = std::sqrt(m[c]*m[c] + m[4+c]*m[4+c] + m[8+c]*m[8+c]);
    const float s = (len[0] + len[1] + len[2]) / 3.f;
    bool good = m4_affine(m) && std::isfinite(s) && s > 1e-6f;
    for (int c = 0; good && c < 3; ++c) {
        const int d = (c + 1) % 3;
        const float dot = m[c]*m[d] + m[4+c]*m[4+d] + m[8+c]*m[8+d];
        good = std::fabs(len[c] - s) <= tol * s && std::fabs(dot) <= tol * s * s;
    }
    if (ok) *ok = good;
    return s;
}
/// The inverse of a rotation-times-uniform-scale affine matrix. False (the identity) when it is not one.
inline bool m4_inverse(const float* m, float* o) {
    bool ok = false; const float s = m4_scale(m, &ok);
    m4_identity(o);
    if (!ok) return false;
    const float k = 1.f / (s * s);
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) o[r*4+c] = m[c*4+r] * k;
    for (int r = 0; r < 3; ++r) o[r*4+3] = -(o[r*4+0]*m[3] + o[r*4+1]*m[7] + o[r*4+2]*m[11]);
    return true;
}

/// The target frame of a carried model in the world: the hand's palm (R 3 x 3 with the axes as columns, origin o, s world
/// units per metre: p' = o + s R p, ffxvr_hands.h HvBone) with the slot's m (3 x 4, palm space, metres) applied first. The
/// result is a rotation and an origin (render space). False when an input is not finite or m's 3 x 3 is not a rotation.
inline bool hand_target(const float* palm_R, const float* palm_o, float palm_s, const float* m, float* out_R, float* out_o) {
    float M[16]; memcpy(M, m, sizeof(float) * 12); M[12] = M[13] = M[14] = 0.f; M[15] = 1.f;
    bool ok = false; const float ms = m4_scale(M, &ok, 0.01f);
    for (int i = 0; i < 9; ++i) if (!std::isfinite(palm_R[i])) ok = false;
    if (!ok || std::fabs(ms - 1.f) > 0.01f || !finite3(palm_o) || !std::isfinite(palm_s) || !(palm_s > 1e-3f)) return false;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) out_R[r*3+c] = palm_R[r*3+0]*m[0*4+c] + palm_R[r*3+1]*m[1*4+c] + palm_R[r*3+2]*m[2*4+c];
        out_o[r] = palm_o[r] + palm_s * (palm_R[r*3+0]*m[3] + palm_R[r*3+1]*m[7] + palm_R[r*3+2]*m[11]);
    }
    return true;
}
/// The move that puts a model's root frame W on the target (rotation R, origin o), keeping W's own scale:
/// D = [R | o] . unit(W)^-1, so D W = [s R | o]. D is a rotation and a translation: it moves every bone of the model alike.
/// When W's 3 x 3 is not a rotation times one scale the move is the translation alone (*translated = true): the model keeps
/// the game's orientation and still lands on o. False (the identity) when W is not affine or the target is not finite.
inline bool anchor(const float* R, const float* o, const float* W, float* D, bool* translated) {
    m4_identity(D);
    if (translated) *translated = false;
    if (!m4_affine(W) || !finite3(o)) return false;
    for (int i = 0; i < 9; ++i) if (!std::isfinite(R[i])) return false;
    bool rigid = false; const float s = m4_scale(W, &rigid);
    if (!rigid) {
        D[3] = o[0] - W[3]; D[7] = o[1] - W[7]; D[11] = o[2] - W[11];
        if (translated) *translated = true;
        return true;
    }
    // Q = W's rotation (its 3 x 3 over s); D's rotation = R Q^T; D's translation = o - R Q^T t_W
    float RQt[9];
    for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) RQt[r*3+c] = (R[r*3+0]*W[c*4+0] + R[r*3+1]*W[c*4+1] + R[r*3+2]*W[c*4+2]) / s;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) D[r*4+c] = RQt[r*3+c];
        D[r*4+3] = o[r] - (RQt[r*3+0]*W[3] + RQt[r*3+1]*W[7] + RQt[r*3+2]*W[11]);
    }
    return true;
}

/// The palette slots move_image may multiply: the affine blocks found by content (ffxvr_native.cpp palette_slots) count as a
/// bone palette only when the buffer can hold a whole one from pal_off on. A smaller buffer (a rigid variant) has other
/// matrices there (a texture or light matrix, identities): none is touched, the world family alone moves the model.
inline int palette_count(uint32_t size, uint32_t pal_off, int found) {
    return found > 0 && static_cast<uint64_t>(pal_off) + static_cast<uint64_t>(k_pal_slots) * 64u <= size ? found : 0;
}
// Which blocks move_image rewrote.
enum : unsigned { B_W = 1, B_VW = 2, B_WI = 4, B_PVW = 8, B_IVW = 16 };
constexpr uint32_t k_world_off = 576, k_min_size = 1872;
/// One eye's image of a skinned-layout constant buffer (the layout at the top), moved by D (affine, rotation times one scale).
/// pal_off / pal_n: the palette's place and its affine slots (ffxvr_native.cpp palette_start / palette_slots through
/// palette_count; 0 slots: none).
/// Returns the blocks rewritten (0: refused, nothing written): refused when the buffer is too small for the world family,
/// W is not affine, D cannot be inverted, or the palette does not fit.
inline unsigned move_image(uint8_t* img, uint32_t size, uint32_t base, uint32_t pal_off, int pal_n, const float* D) {
    const uint32_t wb = base + k_world_off;
    if (img == nullptr || size < k_min_size || base < 16 || wb + 320 > size) return 0;
    if (pal_n < 0 || (pal_n > 0 && (pal_off < wb + 320 || static_cast<uint64_t>(pal_off) + static_cast<uint64_t>(pal_n) * 64u > size))) return 0;
    float Dinv[16];
    if (!m4_inverse(D, Dinv)) return 0;
    float V[16], Pj[16], I[16], W[16], VW[16], Wi[16], PVW[16], IVW[16];
    memcpy(V, img + base, 64); memcpy(Pj, img + base + 64, 64); memcpy(I, img + base + 256, 64);
    memcpy(W, img + wb, 64); memcpy(VW, img + wb + 64, 64); memcpy(Wi, img + wb + 128, 64); memcpy(PVW, img + wb + 192, 64); memcpy(IVW, img + wb + 256, 64);
    if (!m4_affine(W)) return 0;
    float PV[16], t[16], Wn[16], Win[16];
    m4_mul(Pj, V, PV);
    // which blocks are this image's products (write_camera's rule: only those are rebuilt)
    m4_mul(V, W, t);   const bool has_vw = m4_same(VW, t);
    m4_mul(PV, W, t);  const bool has_pvw = m4_same(PVW, t);
    float id[16], eye[16]; m4_identity(eye); m4_mul(Wi, W, id);
    const bool has_wi = m4_affine(Wi) && m4_same(id, eye);
    m4_mul(Wi, I, t);  const bool has_ivw = has_wi && m4_same(IVW, t);
    unsigned got = B_W;
    m4_mul(D, W, Wn); memcpy(img + wb, Wn, 64);
    if (has_vw)  { m4_mul(V, Wn, t);  memcpy(img + wb + 64, t, 64);  got |= B_VW; }
    if (has_wi)  { m4_mul(Wi, Dinv, Win); memcpy(img + wb + 128, Win, 64); got |= B_WI; }
    if (has_pvw) { m4_mul(PV, Wn, t); memcpy(img + wb + 192, t, 64); got |= B_PVW; }
    if (has_ivw) { m4_mul(Win, I, t); memcpy(img + wb + 256, t, 64); got |= B_IVW; }
    for (int s = 0; s < pal_n; ++s) {
        float M[16]; memcpy(M, img + pal_off + s * 64, 64);
        if (!m4_affine(M)) continue;
        m4_mul(D, M, t); memcpy(img + pal_off + s * 64, t, 64);
    }
    return got;
}

// -- the carried sphere's rule (F_SPHERE) ---------------------------------------------------------------------------------
// In the hand: the setting on, the sphere-hands gates open (first person, the VR hands, the field, the headset), a sphere
// carried (the trial's variable), the player in control in free play with no text window open and no grab sequence running,
// for k_quiet frames in a row, and the hand that took it drawn. Anything else: the game's own draw, at once (the take and
// place animations, an event, a menu, hidden hands). The hand: the one whose grip took it, the right hand for the A button
// and for a sphere already carried when the rule starts.
constexpr int k_quiet = 4;
enum Event : int { EV_NONE = 0, EV_UP, EV_DOWN, EV_SEEN };
struct SphereIn {
    bool enabled, fresh, gate, control, free_play, window, seq_running;
    int  carried;        // the trial's carried variable: 0 none, > 0 a sphere, < 0 not read
    int  take_hand;      // the hand of a take sequence running now or just ended (0 / 1), else -1
    bool hand_ok[2];     // the hand was drawn on the last frame
    // branch sphere-fast (sphere_carry_seq): a grab sequence the mod itself started is under way (SeqHold)
    bool seq_hold;       // SeqHold's answer this frame
    bool seq_take;       // that sequence is a take (else a place)
    int  seq_hand;       // the hand whose grip started it (0 / 1)
};
// During a grab sequence of the mod's own (a grip squeeze: never the A button's interaction), in the palm at once
// (sphere_carry_seq, branch sphere-fast):
//   a take   from the first frame the carried actor is shown (managed finds it riding Tidus: `enabled`), which may be before
//            the variable moves, in the hand that squeezed; then on through the rest of the sequence and, with no frame at the
//            game's place in between, into the quiet rule above;
//   a place  it stays in the palm it is in while the variable is not 0, whatever the game's animation does with it; from the
//            variable's 0 the socket's own sphere shows it.
// Everything else is as before: the gates, the hand drawn, the identification of the draw, and the game's own draw on any
// doubt (the hold ends with the sequence's event: SeqHold).
struct SphereGate {
    int carried = -1, hand = -1, quiet = 0; bool held = false;
    bool by_seq = false;   // held (or would be, with the hand drawn) by the sequence's rule on the last step
    int step(const SphereIn& in) {
        if (!in.fresh || in.carried < 0) { held = false; quiet = 0; by_seq = false; return EV_NONE; }   // not read this frame: the value and the hand are kept
        if (in.carried == 0) {
            const int ev = carried > 0 ? EV_DOWN : EV_NONE;
            carried = 0; hand = -1; held = false; quiet = 0; by_seq = false;
            // our own take: the sphere the game has put on Tidus's hand, before the variable says so
            if (in.seq_hold && in.seq_take && in.enabled && in.gate && (in.seq_hand == 0 || in.seq_hand == 1)) {
                hand = in.seq_hand; by_seq = true; held = in.hand_ok[hand];
            }
            return ev;
        }
        int ev = EV_NONE;
        if (carried <= 0) { hand = (in.take_hand == 0 || in.take_hand == 1) ? in.take_hand : 1; ev = carried == 0 ? EV_UP : EV_SEEN; }
        carried = in.carried;
        const bool q = in.control && in.free_play && !in.window && !in.seq_running;
        if (q && by_seq) quiet = k_quiet;   // straight on from the sequence's hold
        else quiet = q ? (quiet < 1000000 ? quiet + 1 : quiet) : 0;
        by_seq = in.seq_hold && (hand == 0 || hand == 1);
        held = in.enabled && in.gate && (quiet >= k_quiet || by_seq) && (hand == 0 || hand == 1) && in.hand_ok[hand];
        return ev;
    }
};

// A grab sequence of the mod's own, as the carry sees it: from its start (`started`: the glue passes the squeeze itself, so a
// carried sphere does not leave the palm for the frames before the game takes the interaction) to the end of the event it
// started: the sequence's own end with control back, or, when the sequence ended first (a stop with the prompt shown, "done"
// before the game's last wait), the first frame the player has control in free play with no text window. Bounded: a scene
// change, the setting or the gates going, or k_seq_tail_s after the sequence ended, end it at once.
constexpr double k_seq_tail_s = 8.0;
struct SeqHoldIn {
    double   t;          // seconds
    bool     on;         // ini sphere_carry_seq, the sphere-hands gates open, the state fresh
    bool     running;    // a sequence runs (sph::Seq::running)
    bool     started;    // ... and it counts from now (the glue: from its first frame)
    bool     quiet;      // the player has control in free play and no text window is open
    uint32_t gen;        // the scene
};
struct SeqHold {
    bool active = false, tail = false; uint32_t gen = 0; double t_end = 0.0;
    bool step(const SeqHoldIn& in) {
        if (!in.on) { active = false; tail = false; return false; }
        if (in.running && in.started) {
            if (active && !tail && in.gen != gen) { active = false; return false; }   // (the sequence itself stops on a scene change)
            active = true; tail = false; gen = in.gen; return true;
        }
        if (!active) return false;
        if (in.running) { active = false; tail = false; return false; }   // a new sequence not yet taken by the game: the old event is over
        if (!tail) { tail = true; t_end = in.t; }
        if (in.quiet || in.gen != gen || !(in.t - t_end <= k_seq_tail_s) || in.t < t_end) { active = false; tail = false; return false; }
        return true;
    }
};
// While the sequence's rule holds the sphere, the game's animation moves it on Tidus's hand between the read of its root (at
// Present) and its draw, and faster with the fast sequence: the draw is looked for within a wider radius of the root, still
// under the cap, and still only when it is nearer that root than every other shown actor of the model.
constexpr float k_seq_tol_mul = 2.5f;
inline float seq_tol(float tol) { const float t = tol * k_seq_tol_mul; return t < k_tol_max ? t : k_tol_max; }

}  // namespace carry
