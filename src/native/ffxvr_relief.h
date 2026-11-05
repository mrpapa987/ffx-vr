// SPDX-License-Identifier: MIT
// ffxvr_relief.h -- the FMV relief screen (S3 of the FMV depth plan, 4 Nov 2026; design-notes "FMV relief screen (S3)").
// Pure logic: no Windows, no D3D, no logging, no globals. ffxvr_native.cpp runs it: the .fvd reader on the FMV worker
// thread (file I/O and the codec come in through callbacks, so a unit test drives it with files fvd.py wrote with the zlib
// or stored codecs), the frame selection, the hold / cut ramp and the relief's shape on the render thread at Present.
//
// The relief: the video frame (the eye-1 backbuffer copy, cropped to the video rect) on a 160 x 90 cell grid (120 x 90 for
// 4:3) at the virtual screen's pose and size; the vertex shader samples the frame's depth map (R8, 320 x 180 / 240 x 180)
// and moves the vertex along the screen normal: z = w * k * (d - p), d the 0..1 disparity (0 far, 1 near), p the pivot
// (the frame's median, smoothed), k = s * W below the pivot (into the screen) and s * W * g above it (out of the screen),
// g = min(1, 0.5 p / (1 - p)) so the pop-out range is at most half the push-in range; w feathers the outer 4 % to flat and,
// as an option, the bottom 15 % (the subtitles). relief_z() below is the CPU copy of the shader's maths (the test checks it).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>
#include "ffxvr_fmv.h"

namespace relief {

constexpr uint32_t k_h = 180, k_w169 = 320, k_w43 = 240, k_max_px = k_w169 * k_h;   // the sizes the converter writes
constexpr uint32_t k_max_frames = 65536;     // 16 B an entry: at most 1 MB of index (END_us, the longest, has 24,828 frames)
constexpr int      k_ring = 8;               // decoded frames: the one on screen and the ones after it
constexpr int      k_stale_tol = 2;          // the file's frame count may differ from the game's duration by this many frames
static_assert(k_max_px <= 0xFFFF, "a payload size fits 16 bits");

// -- the file -----------------------------------------------------------------------------------------------------------
typedef bool (*ReadAtFn)(void* ctx, uint64_t off, void* buf, uint32_t n);   // exactly n bytes at off, or false
typedef bool (*DecompressFn)(void* ctx, const uint8_t* in, uint32_t n, uint8_t* out, uint32_t cap, uint32_t* got);
struct Io { void* ctx; ReadAtFn read_at; uint64_t size; };
/// fn[codec]: the decompressor for codec 1 (zlib) / 2 (XPRESS_HUFF); null = this build cannot read it. Codec 0 needs none.
struct Codecs { void* ctx; DecompressFn fn[3]; };

enum : uint8_t { EF_CUT = 1, EF_STORED = 2, EF_FLAT = 4 };
struct Entry { uint64_t off; uint16_t size; uint8_t flags; uint8_t pad[5]; };   // the index as kept (the checks are made at open)
static_assert(sizeof(Entry) == 16, "16 bytes a frame");

enum Open : int { OP_NONE = 0, OP_OK, OP_HEADER, OP_SIZE, OP_CODEC, OP_FAKE, OP_OTHER, OP_NO_FRAMES, OP_TOO_MANY, OP_STALE, OP_ENTRY,
                  OP_READ, OP_MEMORY, OP_N };
static const char* const k_open[OP_N] = { "not opened", "ok", "invalid header", "not 320x180 / 240x180", "a codec this build cannot read",
    "a pipeline test file (fake model), not depth", "made for another video", "no frames", "too many frames for the memory budget",
    "stale (its frame count differs from the video's)", "a bad index entry", "read error", "out of memory" };
inline const char* open_text(int s) { return s >= 0 && s < OP_N ? k_open[s] : "?"; }

/// The bit maps of cut and flat frames (one bit a frame, k_max_frames bits each): the render thread reads them, the worker
/// writes them at open (never while the render thread may read them: see ffxvr_native.cpp, "the relief worker").
struct Bits { uint8_t cut[k_max_frames / 8]; uint8_t flat[k_max_frames / 8]; };
inline bool bit(const uint8_t* b, uint32_t i) { return i < k_max_frames && (b[i >> 3] >> (i & 7)) & 1; }
inline void set_bit(uint8_t* b, uint32_t i) { if (i < k_max_frames) b[i >> 3] = static_cast<uint8_t>(b[i >> 3] | (1u << (i & 7))); }

struct File {
    fmv::FvdInfo info;                       // the header (ffxvr_fmv.h fvd_parse: FORMAT.md checks 1-5)
    uint32_t w = 0, h = 0, n = 0, header_size = 0, cuts = 0, flats = 0, bad_entry = 0, game_frames = 0;
    uint8_t codec = 0, predictor = 0;
    uint64_t index_offset = 0;
    std::vector<Entry> idx;
    int status = OP_NONE;
    void reset() { std::vector<Entry>().swap(idx); info = fmv::FvdInfo{}; w = h = n = header_size = cuts = flats = bad_entry = game_frames = 0; codec = predictor = 0; index_offset = 0; status = OP_NONE; }
};

inline uint32_t rd32(const uint8_t* b) { uint32_t v; memcpy(&v, b, 4); return v; }
inline uint16_t rd16(const uint8_t* b) { uint16_t v; memcpy(&v, b, 2); return v; }
inline uint64_t rd64(const uint8_t* b) { uint64_t v; memcpy(&v, b, 8); return v; }
inline float    rdf(const uint8_t* b) { float v; memcpy(&v, b, 4); return v; }

/// FORMAT.md "The values": a frame whose range hi - lo is below eps carries no depth (and a non-finite range none either).
inline bool range_flat(float lo, float hi) {
    if (!std::isfinite(lo) || !std::isfinite(hi)) return true;
    const float eps = fmaxf(1e-6f, fabsf(hi) * 1e-4f);
    return hi - lo < eps;
}

/// Opens the file: the header (checks 1-5), this runtime's own requirements, then the whole index (checks 6 and 7 for every
/// entry, so a frame's read needs no further check but 8), the cut / flat bits into `bits` (may be null). game: the game's
/// name for the video (the source name must match it); game_frames: the video's frame count by the game's duration (0 =
/// unknown: no stale check). Returns f.status. Allocation failure: OP_MEMORY (no exception leaves).
inline int open_file(File& f, const Io& io, const char* game, uint32_t game_frames, const Codecs& c, Bits* bits) {
    f.reset();
    f.game_frames = game_frames;
    uint8_t b[128];
    if (io.size < 128 || !io.read_at(io.ctx, 0, b, 128)) {
        if (io.size < 128) { fmv::fvd_parse(b, 0, io.size, game, f.info); return f.status = OP_HEADER; }
        return f.status = OP_READ;
    }
    if (fmv::fvd_parse(b, 128, io.size, game, f.info) != fmv::FVD_OK) return f.status = OP_HEADER;
    f.w = f.info.w; f.h = f.info.h; f.n = f.info.frames; f.codec = f.info.codec; f.predictor = f.info.predictor;
    f.header_size = rd16(b + 6); f.index_offset = rd64(b + 40);
    if (f.h != k_h || (f.w != k_w169 && f.w != k_w43)) return f.status = OP_SIZE;
    if (f.codec != 0 && (f.codec > 2 || c.fn[f.codec] == nullptr)) return f.status = OP_CODEC;
    if (f.info.model == 0) return f.status = OP_FAKE;
    if (!f.info.name_match) return f.status = OP_OTHER;
    if (f.n == 0) return f.status = OP_NO_FRAMES;
    if (f.n > k_max_frames) return f.status = OP_TOO_MANY;
    if (game_frames != 0) {
        const int64_t d = static_cast<int64_t>(f.n) - static_cast<int64_t>(game_frames);
        if (d > k_stale_tol || d < -k_stale_tol) return f.status = OP_STALE;
    }
    try { f.idx.resize(f.n); }
    catch (const std::bad_alloc&) { std::vector<Entry>().swap(f.idx); return f.status = OP_MEMORY; }
    if (bits) memset(bits, 0, sizeof(Bits));
    const uint64_t px = static_cast<uint64_t>(f.w) * f.h;
    uint8_t chunk[256 * 32];
    for (uint32_t i0 = 0; i0 < f.n; i0 += 256) {
        const uint32_t k = f.n - i0 < 256 ? f.n - i0 : 256;
        if (!io.read_at(io.ctx, f.index_offset + 32ull * i0, chunk, 32 * k)) { std::vector<Entry>().swap(f.idx); return f.status = OP_READ; }
        for (uint32_t j = 0; j < k; ++j) {
            const uint8_t* e = chunk + 32 * j; const uint32_t i = i0 + j;
            const uint64_t off = rd64(e); const uint32_t size = rd32(e + 8); const uint16_t fl = rd16(e + 12);
            const bool stored = (fl & 2) != 0 || f.codec == 0;
            const bool ok6 = off >= f.header_size && size <= f.index_offset && off <= f.index_offset - size;   // offset + size <= index_offset, no overflow
            const bool ok7 = size > 0 && size <= px && (!stored || size == px);
            if (!ok6 || !ok7) { f.bad_entry = i; std::vector<Entry>().swap(f.idx); return f.status = OP_ENTRY; }
            Entry& x = f.idx[i];
            x.off = off; x.size = static_cast<uint16_t>(size); x.flags = 0; memset(x.pad, 0, sizeof(x.pad));
            if (stored) x.flags |= EF_STORED;
            if ((fl & 1) || i == 0) { x.flags |= EF_CUT; f.cuts++; if (bits) set_bit(bits->cut, i); }   // frame 0 is a shot start whatever its flag says
            if (range_flat(rdf(e + 24), rdf(e + 28))) { x.flags |= EF_FLAT; f.flats++; if (bits) set_bit(bits->flat, i); }
        }
    }
    return f.status = OP_OK;
}

/// The left predictor's inverse (FORMAT.md), in place.
inline void unpredict(uint8_t* p, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = p + static_cast<size_t>(y) * w;
        if (y > 0) row[0] = static_cast<uint8_t>(row[0] + row[-static_cast<ptrdiff_t>(w)]);
        for (uint32_t x = 1; x < w; ++x) row[x] = static_cast<uint8_t>(row[x] + row[x - 1]);
    }
}
/// The forward predictor (tests only: the converter's left_predict).
inline void predict(const uint8_t* v, uint8_t* r, uint32_t w, uint32_t h) {
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {
        const size_t i = static_cast<size_t>(y) * w + x;
        r[i] = static_cast<uint8_t>(x > 0 ? v[i] - v[i - 1] : y > 0 ? v[i] - v[i - w] : v[i]);
    }
}

enum Dec : int { D_OK = 0, D_RANGE, D_READ, D_CODEC, D_SIZE, D_N };
static const char* const k_dec[D_N] = { "ok", "no such frame", "read error", "decode error", "decoded size wrong" };
inline const char* dec_text(int d) { return d >= 0 && d < D_N ? k_dec[d] : "?"; }
/// Frame i into out (w * h bytes): one read, one decompress (check 8: exactly w * h bytes), the predictor's inverse.
/// payload: a buffer of w * h bytes (any compressed payload fits it: check 7).
inline int decode(const File& f, uint32_t i, const Io& io, const Codecs& c, uint8_t* payload, uint8_t* out) {
    if (f.status != OP_OK || i >= f.n || i >= f.idx.size()) return D_RANGE;
    const Entry& e = f.idx[i]; const uint32_t px = f.w * f.h;
    if (e.flags & EF_STORED) { if (!io.read_at(io.ctx, e.off, out, e.size)) return D_READ; }
    else {
        if (!io.read_at(io.ctx, e.off, payload, e.size)) return D_READ;
        uint32_t got = 0;
        if (f.codec > 2 || c.fn[f.codec] == nullptr || !c.fn[f.codec](c.ctx, payload, e.size, out, px, &got)) return D_CODEC;
        if (got != px) return D_SIZE;
    }
    if (f.predictor == 1) unpredict(out, f.w, f.h);
    return D_OK;
}

/// The median of n bytes (the smallest value with at least half of them at or below it).
inline uint8_t median_u8(const uint8_t* p, uint32_t n) {
    if (n == 0) return 0;
    uint32_t hist[256] = {};
    for (uint32_t i = 0; i < n; ++i) hist[p[i]]++;
    const uint32_t half = (n + 1) / 2; uint32_t cum = 0;
    for (int v = 0; v < 256; ++v) { cum += hist[v]; if (cum >= half) return static_cast<uint8_t>(v); }
    return 255;
}

// -- which depth frame: the frame on screen is the uploaded count - 1 (design-notes "FMV sync probe") --------------------
enum Sel : int { SEL_OK = 0, SEL_NONE_YET, SEL_PAST, SEL_BEFORE };
/// count: player+0x100; offset: ini fmv_depth_offset (frames; + shows a later depth frame); frames: the file's.
inline int32_t frame_for(uint32_t count, int32_t offset, uint32_t frames, int* why) {
    if (count == 0) { if (why) *why = SEL_NONE_YET; return -1; }
    const int64_t i = static_cast<int64_t>(count) - 1 + offset;
    if (i < 0) { if (why) *why = SEL_BEFORE; return -1; }
    if (i >= static_cast<int64_t>(frames)) { if (why) *why = SEL_PAST; return -1; }
    if (why) *why = SEL_OK;
    return static_cast<int32_t>(i);
}

// -- per Present: hold, the depth level (ramps in and out), pivot, strength ----------------------------------------------
/// Why a Present shows depth or not (counted per FMV; F8 names the current one). With depth on while the reason is not
/// W_DEPTH / W_HELD, the relief is fading out on the last depth uploaded (review N1).
enum Why : int { W_DEPTH = 0, W_HELD, W_LAG, W_FLAT_FRAME, W_NO_FRAME, W_PAST, W_DECODE, W_N };
static const char* const k_why[W_N] = { "depth ok", "depth ok (one frame late: decode lag)", "flat (decode lag)", "flat (a flat frame in the file)",
    "flat (no video frame yet)", "flat (past the file's last frame)", "flat (decode error)" };
inline const char* why_text(int w) { return w >= 0 && w < W_N ? k_why[w] : "?"; }

struct Tcfg {
    float ramp_ms = 150.f;     // the depth level moves between 0 (flat) and 1 (full) over this, both ways
    float frame_ms = 1001.f / 30.f;   // a video frame (29.97 fps): the level is capped to reach 0 at the file's last frame
    float p_alpha = 0.15f;     // the pivot follows the frame's median: this share of the gap per video frame ...
    float p_step = 0.02f;      // ... at most this much per video frame (0..1 units)
    float s_rate = 0.5f;       // the strength setting is followed at most this fast (per second): a slider never pops
};
struct Tstate {
    int32_t bound = -1;        // the frame whose depth is in the texture (-1: none)
    float lvl = 0.f;           // the depth level 0..1 (0: flat; the output uses smoothstep(lvl)); set to 0 by a cut or a fallback
    bool shown = false;        // the last Present drew depth (false: flat, or a fallback to the flat screen: the next depth ramps in)
    float pivot = 0.5f;
    float s_cur = 0.f; bool have_t = false; double t_prev = 0.0;
};
/// A Present that did not reach step (a fallback to the flat screen): the next relief frame ramps in from 0.
inline void to_flat(Tstate& t) { t.lvl = 0.f; t.shown = false; }
/// The ring's answer for `want`: ready (decoded, idx == want), error (its decode failed), median (0..255).
struct Avail { bool ready; bool error; uint8_t median; };
/// depth: draw the relief with s (false: flat, s = 0); upload: the ring's frame goes into the texture first; ramp_start: the
/// level was reset to 0 this Present (a cut, or the first depth after flat); fading: depth on the last upload, going to flat.
struct Tout { bool depth; bool upload; bool ramp_start; bool fading; int why; float s; float pivot; int32_t bound; float level; };
inline float smooth01(float x) { x = x < 0.f ? 0.f : x > 1.f ? 1.f : x; return x * x * (3.f - 2.f * x); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
/// A cut in (a, b] (frames), by the bit map; a span longer than 64 frames counts as a cut (a jump).
inline bool cut_between(const Bits& b, int32_t a, int32_t c) {
    if (a < 0 || c <= a) return true;
    if (c - a > 64) return true;
    for (int32_t i = a + 1; i <= c; ++i) if (bit(b.cut, static_cast<uint32_t>(i))) return true;
    return false;
}
/// One Present. now: ms; want: the depth frame for the video frame on screen (frame_for, -1 none; sel its reason); a: the
/// ring's answer for want; s_target: the strength setting; frames: the file's frame count (the level reaches 0 at its last
/// frame, so a movie's end and a short file fade out; 0 = no cap).
/// The level rises toward 1 while the wanted depth is there and falls toward 0 (on the last depth uploaded) while it is
/// not: a lag, a flat frame, a decode error, past the file's end. Instant flat only when there is no depth that could stand
/// in: nothing uploaded yet, or the wanted frame starts a new shot (a cut) that is not decoded.
inline Tout step(Tstate& t, const Tcfg& c, double now, int32_t want, int sel, const Avail& a, const Bits& b, float s_target, uint32_t frames = 0) {
    const double gap = t.have_t ? (now - t.t_prev < 0 ? 0 : now - t.t_prev) : 0.0;   // the level moves by the real time ...
    const double dt = gap > 100 ? 100 : gap;                                             // ... the strength by at most 100 ms a step
    if (!t.have_t) t.s_cur = s_target;
    t.have_t = true; t.t_prev = now;
    const float ds = static_cast<float>(c.s_rate * dt / 1000.0);
    t.s_cur += clampf(s_target - t.s_cur, -ds, ds);
    const float dl = static_cast<float>(gap / c.ramp_ms);
    Tout o = { false, false, false, false, W_DEPTH, 0.f, t.pivot, t.bound, 0.f };
    enum { OK, FADE, FLAT } act = OK;
    if (want < 0) { o.why = sel == SEL_PAST ? W_PAST : W_NO_FRAME; act = (sel == SEL_PAST && t.bound >= 0) ? FADE : FLAT; }
    else if (want == t.bound) o.why = W_DEPTH;                          // held between video frames: the texture has it
    else if (bit(b.flat, static_cast<uint32_t>(want))) { o.why = W_FLAT_FRAME; act = t.bound >= 0 ? FADE : FLAT; }
    else if (a.ready && !a.error) {
        const bool cut = !t.shown || t.bound < 0 || want < t.bound || cut_between(b, t.bound, want);
        const float m = a.median / 255.f;
        if (cut) { t.lvl = 0.f; t.pivot = m; o.ramp_start = true; }
        else t.pivot += clampf(c.p_alpha * (m - t.pivot), -c.p_step, c.p_step);
        t.bound = want; o.upload = true; o.why = W_DEPTH;
    }
    else if (t.bound >= 0 && cut_between(b, t.bound, want)) { o.why = a.ready ? W_DECODE : W_LAG; act = FLAT; }   // a new shot: the old depth must not stand in
    else if (a.ready) { o.why = W_DECODE; act = FADE; }
    else if (want == t.bound + 1 && t.shown) o.why = W_HELD;            // one frame late
    else { o.why = W_LAG; act = FADE; }
    if (act == FLAT) t.lvl = 0.f;
    else if (act == FADE) t.lvl = fmaxf(0.f, t.lvl - dl);
    else if (!o.ramp_start) t.lvl = fminf(1.f, t.lvl + dl);
    float cap = 1.f;
    if (frames > 0 && want >= 0) cap = clampf((static_cast<float>(frames) - 1.f - static_cast<float>(want)) * c.frame_ms / c.ramp_ms, 0.f, 1.f);
    const float lv = fminf(t.lvl, cap);
    o.level = lv; o.pivot = t.pivot; o.bound = t.bound;
    o.depth = act != FLAT && t.lvl > 0.f;
    if (act == OK && o.ramp_start) o.depth = true;                      // the first Present of a ramp: drawn (flat) as the relief
    o.fading = o.depth && act == FADE;
    t.shown = o.depth;
    o.s = o.depth ? t.s_cur * smooth01(lv) : 0.f;
    return o;
}

// -- the shape: what the vertex shader does, on the CPU ------------------------------------------------------------------
struct Shape { float k_push, k_pop, pivot; };
/// s: strength (0..0.25), W: the screen width (m), p: the pivot (0..1).
inline Shape shape(float s, float W, float p) {
    p = clampf(p, 0.02f, 0.98f);
    const float g = fminf(1.f, 0.5f * p / (1.f - p));
    return { s * W, s * W * g, p };
}
constexpr float k_feather = 0.04f, k_band = 0.15f, k_band_ramp = 0.05f;
/// The relief's weight at grid point (a, b) (0..1 across the video rect, b down): flat at the border, full 4 % in; with
/// flat_bottom 0 in the bottom 15 % (b >= 0.85), ramping from 1 at b = 0.80.
inline float weight(float a, float b, bool flat_bottom) {
    const float e = fminf(fminf(a, 1.f - a), fminf(b, 1.f - b));
    float w = smooth01(e / k_feather);
    if (flat_bottom) w *= 1.f - smooth01((b - (1.f - k_band - k_band_ramp)) / k_band_ramp);
    return w;
}
/// The offset along the screen normal (m, + toward the viewer) for disparity d (0..1).
inline float relief_z(const Shape& sh, float d, float w) { const float t = d - sh.pivot; return w * (t < 0.f ? sh.k_push * t : sh.k_pop * t); }

/// The video's rect in the backbuffer (uv, the game letterboxes / pillarboxes it centred): vw x vh into bw x bh.
struct Rect { float u0, v0, du, dv; };
inline Rect video_rect(uint32_t vw, uint32_t vh, uint32_t bw, uint32_t bh) {
    if (vw == 0 || vh == 0 || bw == 0 || bh == 0) return { 0.f, 0.f, 1.f, 1.f };
    const double va = static_cast<double>(vw) / vh, ba = static_cast<double>(bw) / bh;
    if (va < ba) { const float du = static_cast<float>(va / ba); return { 0.5f - du * 0.5f, 0.f, du, 1.f }; }   // pillarbox
    const float dv = static_cast<float>(ba / va); return { 0.f, 0.5f - dv * 0.5f, 1.f, dv };                    // letterbox
}

// -- matrices (row-major, column vectors: clip = M * v; HLSL row_major + mul(M, v)) ---------------------------------------
inline void quat_mat3(const float q[4], float R[9]) {   // x, y, z, w; R's columns are the rotated axes
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    R[0] = 1 - 2 * (y * y + z * z); R[1] = 2 * (x * y - z * w);     R[2] = 2 * (x * z + y * w);
    R[3] = 2 * (x * y + z * w);     R[4] = 1 - 2 * (x * x + z * z); R[5] = 2 * (y * z - x * w);
    R[6] = 2 * (x * z - y * w);     R[7] = 2 * (y * z + x * w);     R[8] = 1 - 2 * (x * x + y * y);
}
inline void mat4_mul(const float* A, const float* B, float* O) { float t[16]; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) { float s = 0; for (int k = 0; k < 4; ++k) s += A[i * 4 + k] * B[k * 4 + j]; t[i * 4 + j] = s; } memcpy(O, t, sizeof(t)); }
/// A pose (orientation q, position p) as the matrix local -> parent.
inline void pose_mat(const float q[4], const float p[3], float M[16]) {
    float R[9]; quat_mat3(q, R);
    const float m[16] = { R[0], R[1], R[2], p[0], R[3], R[4], R[5], p[1], R[6], R[7], R[8], p[2], 0, 0, 0, 1 };
    memcpy(M, m, sizeof(m));
}
/// The view matrix of an eye at pose (q, p): parent -> eye (the inverse pose).
inline void view_mat(const float q[4], const float p[3], float V[16]) {
    float R[9]; quat_mat3(q, R);
    const float t[3] = { -(R[0] * p[0] + R[3] * p[1] + R[6] * p[2]), -(R[1] * p[0] + R[4] * p[1] + R[7] * p[2]), -(R[2] * p[0] + R[5] * p[1] + R[8] * p[2]) };
    const float m[16] = { R[0], R[3], R[6], t[0], R[1], R[4], R[7], t[1], R[2], R[5], R[8], t[2], 0, 0, 0, 1 };
    memcpy(V, m, sizeof(m));
}
/// OpenXR's view space (x right, y up, looking down -z) to D3D clip space (depth 0 at n .. 1 at f), from the tangents of the
/// half angles (l and d negative, as XrFovTan).
inline void proj_mat(float l, float r, float u, float d, float n, float f, float P[16]) {
    const float m[16] = { 2.f / (r - l), 0, (r + l) / (r - l), 0,
                          0, 2.f / (u - d), (u + d) / (u - d), 0,
                          0, 0, f / (n - f), f * n / (n - f),
                          0, 0, -1, 0 };
    memcpy(P, m, sizeof(m));
}
struct Fov { float l, r, u, d; };   // tangents of the half angles (l, d negative), as XrFovTan
/// Review SF-1: the field of view that just covers the screen's video rect seen from an eye at (eq, ep): the rect's four
/// corners at z = +pop and -push along the screen normal (the box holds the whole relief, and a projection of a convex box is
/// bounded by its corners'), widened by `margin` of the span on each side plus 0.004, clamped to `full`. The same eye image
/// then covers less of the view, so the video gets more of its pixels. False (out = full): a corner is not in front of the
/// eye, or the rect is (nearly) out of view.
inline bool tight_fov(const float eq[4], const float ep[3], const Fov& full, const float sq[4], const float sp[3], float W, float H,
                      const Rect& rc, float pop, float push, float margin, Fov* out) {
    *out = full;
    float V[16], M[16], VM[16]; view_mat(eq, ep, V); pose_mat(sq, sp, M); mat4_mul(V, M, VM);
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (int k = 0; k < 8; ++k) {
        const float u = rc.u0 + ((k & 1) ? rc.du : 0.f), v = rc.v0 + ((k & 2) ? rc.dv : 0.f), z = (k & 4) ? pop : -push;
        const float lx = (u - 0.5f) * W, ly = (0.5f - v) * H;
        const float px = VM[0] * lx + VM[1] * ly + VM[2] * z + VM[3], py = VM[4] * lx + VM[5] * ly + VM[6] * z + VM[7], pz = VM[8] * lx + VM[9] * ly + VM[10] * z + VM[11];
        if (!(pz < -0.05f)) return false;
        const float tx = px / -pz, ty = py / -pz;
        x0 = fminf(x0, tx); x1 = fmaxf(x1, tx); y0 = fminf(y0, ty); y1 = fmaxf(y1, ty);
    }
    const float mx = margin * (x1 - x0) + 0.004f, my = margin * (y1 - y0) + 0.004f;
    Fov f = { fmaxf(full.l, x0 - mx), fminf(full.r, x1 + mx), fminf(full.u, y1 + my), fmaxf(full.d, y0 - my) };
    if (!(f.r - f.l > 0.02f) || !(f.u - f.d > 0.02f)) return false;
    *out = f;
    return true;
}
/// The eye image's rows per video row: the video rect's vertical tangent extent (at z = 0) over the field of view's, times
/// the image height, over the video's lines (review SF-1's log figure).
inline float rows_per_line(const float eq[4], const float ep[3], const Fov& fov, const float sq[4], const float sp[3], float W, float H, const Rect& rc,
                           uint32_t image_h, uint32_t video_h) {
    const Fov wide = { -1e6f, 1e6f, 1e6f, -1e6f };
    Fov v;
    if (video_h == 0 || !(fov.u - fov.d > 0.f) || !tight_fov(eq, ep, wide, sq, sp, W, H, rc, 0.f, 0.f, 0.f, &v)) return 0.f;
    return static_cast<float>(image_h) * ((v.u - 0.004f) - (v.d + 0.004f)) / (fov.u - fov.d) / static_cast<float>(video_h);
}
inline bool fov_ok(float l, float r, float u, float d) {
    return std::isfinite(l) && std::isfinite(r) && std::isfinite(u) && std::isfinite(d) && r - l > 0.1f && u - d > 0.1f && r - l < 20.f && u - d < 20.f;
}

}  // namespace relief
