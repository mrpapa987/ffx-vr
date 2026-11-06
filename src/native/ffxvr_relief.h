// SPDX-License-Identifier: MIT
// ffxvr_relief.h -- the FMV relief screen (S3 of the FMV depth plan, 4 Nov 2026; design-notes "FMV relief screen (S3)").
// Pure logic: no Windows, no D3D, no logging, no globals. ffxvr_native.cpp runs it: the .fvd reader on the FMV worker
// thread (file I/O and the codec come in through callbacks, so a unit test drives it with files fvd.py wrote with the zlib
// or stored codecs), the frame selection, the hold / cut ramp and the relief's shape on the render thread at Present.
//
// The relief: the video frame (the eye-1 backbuffer copy, cropped to the video rect) on a grid with a vertex per depth
// texel (round 7: 321 x 181 cells on the 320 x 180 map; round 10: 641 x 361 on the guided upsample's 640 x 360, below) at
// the virtual screen's pose and size; the vertex shader samples the depth (R16) and moves each vertex along the ray
// from the nominal eye through its pixel to a distance given by the "3D movie" mapping (map_z below: the shot's nearest content slightly in front of the screen, its farthest a few metres behind,
// linear in disparity); only the part in front of the screen is feathered to the screen plane at the edges (the window
// rule), and as an option the bottom 15 % lies flat (the subtitles).
// map_point / weights / edge_fill below are the CPU copy of the shader's maths (the test checks them).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <new>
#include <vector>
#include "ffxvr_fmv.h"

namespace relief {

constexpr uint32_t k_h = 180, k_w169 = 320, k_w43 = 240, k_max_px = k_w169 * k_h;   // the sizes the converter writes
constexpr uint32_t k_max_frames = 32768;     // 16 B an entry: at most 512 KB of index (END_us, the longest, has 24,828 frames: 397 KB)
constexpr int      k_ring = 6;               // round 7: the worker's output ring: the frame on screen and the 5 after it (16-bit n')
constexpr int      k_raw = 12;               // round 7: the decoded (8-bit) ring: the output frames' temporal neighbours (-3 .. +3)
constexpr int      k_trad = 3;               // the temporal filter's radius (frames)
constexpr int      k_stale_tol = 2;          // the file's frame count may differ from the game's duration by this many frames
static_assert(k_max_px <= 0xFFFF, "a payload size fits 16 bits");

// -- the file -----------------------------------------------------------------------------------------------------------
typedef bool (*ReadAtFn)(void* ctx, uint64_t off, void* buf, uint32_t n);   // exactly n bytes at off, or false
typedef bool (*DecompressFn)(void* ctx, const uint8_t* in, uint32_t n, uint8_t* out, uint32_t cap, uint32_t* got);
struct Io { void* ctx; ReadAtFn read_at; uint64_t size; };
/// fn[codec]: the decompressor for codec 1 (zlib) / 2 (XPRESS_HUFF); null = this build cannot read it. Codec 0 needs none.
struct Codecs { void* ctx; DecompressFn fn[3]; };

enum : uint8_t { EF_CUT = 1, EF_STORED = 2, EF_FLAT = 4 };
constexpr uint8_t k_hf_segment_range = 0x04;   // header byte 27 bit 2 (converter v2): lo / hi are already one range per segment (see shot_norm)
/// The index as kept (the checks are made at open). a, b (round 7, per-shot normalisation): n' = a n + b maps the frame's
/// 8-bit n (normalised to the frame's own lo / hi) to the shot's fixed L / H; during the read they hold lo / hi.
struct Entry { uint32_t off; uint16_t size; uint8_t flags; uint8_t pad; float a, b; };
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
    uint32_t w = 0, h = 0, n = 0, header_size = 0, cuts = 0, flats = 0, bad_entry = 0, game_frames = 0, shots = 0;
    uint8_t codec = 0, predictor = 0;
    bool seg_range = false;   // header flag HF_SEGMENT_RANGE: the converter wrote one fixed lo / hi per segment (no re-normalisation)
    uint64_t index_offset = 0;
    std::vector<Entry> idx;
    int status = OP_NONE;
    void reset() { std::vector<Entry>().swap(idx); info = fmv::FvdInfo{}; w = h = n = header_size = cuts = flats = bad_entry = game_frames = shots = 0; codec = predictor = 0; index_offset = 0; status = OP_NONE; }
};

inline uint32_t rd32(const uint8_t* b) { uint32_t v; memcpy(&v, b, 4); return v; }
struct File;
inline uint32_t shot_norm(File& f);
inline float smooth01(float x) { x = x < 0.f ? 0.f : x > 1.f ? 1.f : x; return x * x * (3.f - 2.f * x); }
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
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
            const bool ok6 = off >= f.header_size && size <= f.index_offset && off <= f.index_offset - size   // offset + size <= index_offset, no overflow
                          && off <= 0xFFFFFFFFull - size;                                                       // round 7: kept as 32 bits (the files are < 0.5 GB)
            const bool ok7 = size > 0 && size <= px && (!stored || size == px);
            if (!ok6 || !ok7) { f.bad_entry = i; std::vector<Entry>().swap(f.idx); return f.status = OP_ENTRY; }
            Entry& x = f.idx[i];
            x.off = static_cast<uint32_t>(off); x.size = static_cast<uint16_t>(size); x.flags = 0; x.pad = 0; x.a = rdf(e + 24); x.b = rdf(e + 28);
            if (stored) x.flags |= EF_STORED;
            if ((fl & 1) || i == 0) { x.flags |= EF_CUT; f.cuts++; if (bits) set_bit(bits->cut, i); }   // frame 0 is a shot start whatever its flag says
            if (range_flat(rdf(e + 24), rdf(e + 28))) { x.flags |= EF_FLAT; f.flats++; if (bits) set_bit(bits->flat, i); }
        }
    }
    f.seg_range = (f.info.flags & k_hf_segment_range) != 0;
    if (f.seg_range) { for (Entry& x : f.idx) { x.a = 1.f; x.b = 0.f; } f.shots = 0; }   // converter v2: its ranges are final
    else f.shots = shot_norm(f);
    return f.status = OP_OK;
}

// -- round 7 (audit plan 2): per-shot re-normalisation from the index ------------------------------------------------------
// The converter normalised each frame to its own p2 / p98 (EMA-smoothed within the shot): n = (d - lo) / (hi - lo), so when
// something nearer enters, hi rises and everything else sinks back (the subject "jumps"), and the nearest 2 % of each frame
// is clipped flat. The index keeps lo / hi per frame and n is exact against them, so one fixed range per shot (L, H) gives
// n' = (d - L) / (H - L) = a n + b, a = (hi - lo) / (H - L), b = (lo - L) / (H - L): no reconversion. L / H are robust (the
// 10th percentile of the shot's lo, the 90th of its hi: one odd frame cannot flatten the shot); flat frames are left out.
// A "shot" shorter than k_min_shot frames (a flash that tripped the converter's cut detector twice) is folded into the one
// before it, so it does not get a range of its own. n' is clamped to 0..1.
constexpr uint32_t k_min_shot = 6;
// Converter v2 (branch fmv-conv2, review fmv-conv2-review.md B-1) writes one fixed lo / hi per segment itself: it restarts
// the model at every cut (so a 1-5 frame shot has its own scale, not a flash) and splits shots at dissolves without a cut
// flag. On such a file the fold and the per-shot percentiles would map a short shot through another shot's range and
// squash or clip the segments of a dissolve, so with the header flag bit 2 (HF_SEGMENT_RANGE, 0x04 in byte 27) every
// frame keeps its own range (a = 1, b = 0; k_hf_segment_range). Files without it (v1) are re-normalised as above.
inline float pct(std::vector<float>& v, float p) {   // nearest rank (v is reordered)
    const size_t k = static_cast<size_t>(p * static_cast<float>(v.size() - 1) + 0.5f);
    std::nth_element(v.begin(), v.begin() + static_cast<ptrdiff_t>(k), v.end());
    return v[k];
}
/// Sets every entry's (a, b) from its lo / hi (held in a / b by open_file). Returns the number of shots (segments) used;
/// on an allocation failure every frame keeps its own range (a = 1, b = 0).
inline uint32_t shot_norm(File& f) {
    const uint32_t n = static_cast<uint32_t>(f.idx.size());
    std::vector<float> lo, hi;
    try { lo.reserve(n); hi.reserve(n); }
    catch (const std::bad_alloc&) { for (Entry& e : f.idx) { e.a = 1.f; e.b = 0.f; } return 0; }
    uint32_t shots = 0;
    for (uint32_t s = 0; s < n;) {
        uint32_t e = s + 1;
        for (; e < n; ++e) {   // the segment ends at the next cut that starts a shot of at least k_min_shot frames (or the end)
            if (!(f.idx[e].flags & EF_CUT)) continue;
            uint32_t nx = e + 1; while (nx < n && !(f.idx[nx].flags & EF_CUT)) ++nx;
            if (nx - e >= k_min_shot || nx >= n) break;
        }
        lo.clear(); hi.clear();
        for (uint32_t i = s; i < e; ++i) if (!(f.idx[i].flags & EF_FLAT)) { lo.push_back(f.idx[i].a); hi.push_back(f.idx[i].b); }
        float L = 0.f, H = 0.f; bool ok = !lo.empty();
        if (ok) { L = pct(lo, 0.10f); H = pct(hi, 0.90f); ok = std::isfinite(L) && std::isfinite(H) && !range_flat(L, H); }
        for (uint32_t i = s; i < e; ++i) {
            Entry& x = f.idx[i];
            if (!ok || (x.flags & EF_FLAT)) { x.a = 1.f; x.b = 0.f; continue; }
            const float r = H - L; const float l0 = x.a, h0 = x.b;
            x.a = (h0 - l0) / r; x.b = (l0 - L) / r;
        }
        shots++; s = e;
    }
    return shots;
}
/// The worker's processing settings as one word (an output frame made with another word is made again): bit 0 the 3x3
/// median, bit 1 each frame to its own range (Debug), bit 2 the centred temporal filter (round 7.4), bit 3 the edge snap
/// (round 10, with the guided upsample), bits 8-15 the filter's smoothing (x 100).
inline int cfg_word(bool median, bool per_frame, bool centred, float smooth, bool snap = false) {
    return (median ? 1 : 0) | (per_frame ? 2 : 0) | (snap ? 8 : 0) | (centred ? 4 | (static_cast<int>(clampf(smooth, 0.f, 0.95f) * 100.f + 0.5f) << 8) : 0);
}
/// The decoded frames an output frame needs on each side (0: itself only).
inline int filter_radius(int cfg) { return (cfg & 4) ? k_trad : 0; }
/// One frame's n' as 16 bits: out = round(clamp(a v / 255 + b, 0, 1) x 65535). per_frame: the frame's own range (a 1, b 0).
inline void norm16(const uint8_t* v, uint32_t px, float a, float b, uint16_t* out) {
    for (uint32_t i = 0; i < px; ++i) {
        const float x = a * (static_cast<float>(v[i]) * (1.f / 255.f)) + b;
        out[i] = static_cast<uint16_t>(clampf(x, 0.f, 1.f) * 65535.f + 0.5f);
    }
}

// -- round 7.4 (audit plan 4): the centred temporal filter on the worker ---------------------------------------------------
// The causal EMA trades lag for smoothing (1.6 video frames at 0.6) and ghosts below its gate. The worker decodes ahead
// anyway, so each output frame i is a depth-only temporal bilateral of the decoded frames i-3 .. i+3 (zero phase, no lag):
// per pixel, weights exp(-dt^2 / 2 sigma_t^2) x exp(-dv^2 / 2 sigma_d^2), dv = the n' difference to frame i in 8-bit levels;
// a neighbour across a cut (or a flat or undecodable frame, or past the ends) has weight 0. sigma_t = 3 x smooth (the
// "smoothing" slider; 0.6 -> 1.8 frames; below 0.05: no filter), sigma_d = 7 levels (the audit's e4: flicker 0.88 vs the
// EMA's 1.23, lag 0.13 vs 1.62 frames, half the error at moving edges). Each neighbour's n' comes from a 256-entry table
// (raw byte -> 16-bit n' with that frame's own a / b), so the filter sees the per-shot n' of every frame.
constexpr float k_tsig_d = 7.f;
struct TFilter { float wt[2 * k_trad + 1]; float wd[1024]; bool on; };   // wd: by |dv| in 1/4 levels (256 levels)
inline void tfilter_make(TFilter& f, float smooth) {
    const float st = 3.f * clampf(smooth, 0.f, 0.95f);
    f.on = st >= 0.15f;
    for (int k = -k_trad; k <= k_trad; ++k) f.wt[k + k_trad] = f.on ? expf(-0.5f * (k / st) * (k / st)) : (k == 0 ? 1.f : 0.f);
    for (int j = 0; j < 1024; ++j) { const float d = j * 0.25f / k_tsig_d; f.wd[j] = expf(-0.5f * d * d); }
}
/// Which neighbours of frame i may be averaged into it (use[k] for i + k - k_trad): inside the file, not flat, and no cut
/// between them (a cut at frame c starts a shot: c and later are not averaged with c - 1 and earlier).
inline void tneighbours(const File& f, int32_t i, bool use[2 * k_trad + 1]) {
    const int32_t n = static_cast<int32_t>(f.idx.size());
    for (int k = 0; k < 2 * k_trad + 1; ++k) use[k] = false;
    use[k_trad] = true;
    for (int dir = -1; dir <= 1; dir += 2)
        for (int d = 1; d <= k_trad; ++d) {
            const int32_t j = i + dir * d;
            if (j < 0 || j >= n) break;
            if ((f.idx[static_cast<uint32_t>(dir > 0 ? j : j + 1)].flags & EF_CUT)) break;   // a cut between them
            if (f.idx[static_cast<uint32_t>(j)].flags & EF_FLAT) continue;
            use[k_trad + dir * d] = true;
        }
}
/// raw byte -> 16-bit n' for a frame's (a, b): norm16's mapping as a table.
inline void norm_lut(float a, float b, uint16_t lut[256]) { uint8_t v[256]; for (int k = 0; k < 256; ++k) v[k] = static_cast<uint8_t>(k); norm16(v, 256, a, b, lut); }
/// The filter: fr[k] the raw frames i + k - k_trad (null: not used), lut[k] their tables; out the 16-bit n' of frame i.
inline void tfilter(const TFilter& f, const uint8_t* const fr[2 * k_trad + 1], const uint16_t* const lut[2 * k_trad + 1], uint32_t px, uint16_t* out) {
    const uint8_t* c = fr[k_trad]; const uint16_t* lc = lut[k_trad];
    if (!f.on) { for (uint32_t p = 0; p < px; ++p) out[p] = lc[c[p]]; return; }
    for (uint32_t p = 0; p < px; ++p) {
        const int v0 = lc[c[p]];
        float acc = static_cast<float>(v0) * f.wt[k_trad], ws = f.wt[k_trad];
        for (int k = 0; k < 2 * k_trad + 1; ++k) {
            if (k == k_trad || fr[k] == nullptr) continue;
            const int v = lut[k][fr[k][p]];
            int j = (v > v0 ? v - v0 : v0 - v) >> 6;   // 1/4 level (65535 / 255 / 4 = 64.25)
            if (j > 1023) continue;                      // over 256 levels: weight ~0
            const float w = f.wt[k] * f.wd[j];
            acc += w * static_cast<float>(v); ws += w;
        }
        out[p] = static_cast<uint16_t>(acc / ws + 0.5f);
    }
}
// The worker's schedule (relw_step), pure so the test can run a playback: for the output window [want, want + k_ring),
// the first output frame that is missing (or made with other settings), and the first of its decoded neighbours (+-rad)
// that is missing. Returns 0 nothing to do, 1 decode *which, 2 make output *which.
inline int next_unit(int32_t want, int32_t n, int rad, const int32_t raw_idx[k_raw], const int32_t out_idx[k_ring], const bool out_ok[k_ring], int32_t* which) {
    const int32_t w0 = want < 0 ? 0 : want;
    for (int32_t i = w0; i < w0 + k_ring && i < n; ++i) {
        if (out_idx[i % k_ring] == i && out_ok[i % k_ring]) continue;
        for (int32_t s = (i - rad < 0 ? 0 : i - rad); s <= (i + rad < n - 1 ? i + rad : n - 1); ++s)
            if (raw_idx[s % k_raw] != s) { *which = s; return 1; }
        *which = i; return 2;
    }
    return 0;
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

// -- per Present: hold, the depth level (ramps in and out), strength ----------------------------------------------------
/// Why a Present shows depth or not (counted per FMV; F8 names the current one). With depth on while the reason is not
/// W_DEPTH / W_HELD, the relief is fading out on the last depth uploaded (review N1).
enum Why : int { W_DEPTH = 0, W_HELD, W_LAG, W_FLAT_FRAME, W_NO_FRAME, W_PAST, W_DECODE, W_N };
static const char* const k_why[W_N] = { "depth ok", "depth ok (one frame late: decode lag)", "flat (decode lag)", "flat (a flat frame in the file)",
    "flat (no video frame yet)", "flat (past the file's last frame)", "flat (decode error)" };
inline const char* why_text(int w) { return w >= 0 && w < W_N ? k_why[w] : "?"; }

struct Tcfg {
    float ramp_ms = 150.f;     // the depth level moves between 0 (flat) and 1 (full) over this, both ways
    float frame_ms = 1001.f / 30.f;   // a video frame (29.97 fps): the level is capped to reach 0 at the file's last frame
    float s_rate = 0.5f;       // the strength setting is followed at most this fast (per second): a slider never pops
    bool old_cut_ramp = false; // Debug (A/B): every cut starts flat and ramps in over ramp_ms, as before round 7.3
};
struct Tstate {
    int32_t bound = -1;        // the frame whose depth is in the texture (-1: none)
    float lvl = 0.f;           // the depth level 0..1 (0: flat; the output uses smoothstep(lvl)); set to 0 by a cut or a fallback
    bool shown = false;        // the last Present drew depth (false: flat, or a fallback to the flat screen: the next depth ramps in)
    float s_cur = 0.f; bool have_t = false; double t_prev = 0.0;
};
/// A Present that did not reach step (a fallback to the flat screen): the next relief frame ramps in from 0.
inline void to_flat(Tstate& t) { t.lvl = 0.f; t.shown = false; }
/// The ring's answer for `want`: ready (made, idx == want), error (its decode failed). (Round 7.5: the median that fed the
/// unused pivot is gone.)
struct Avail { bool ready; bool error; };
/// depth: draw the relief with s (false: flat, s = 0); upload: the ring's frame goes into the texture first; ramp_start: the
/// level was reset to 0 this Present (the first depth after flat; with old_cut_ramp every cut too); fading: depth on the
/// last upload, going to flat; cut: a cut crossed with its depth ready, switched at once; reset: a cut or a return from
/// flat (the causal filter starts over).
struct Tout { bool depth; bool upload; bool ramp_start; bool fading; int why; float s; int32_t bound; float level; bool cut, reset; };
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
/// in: nothing uploaded yet, or the wanted frame starts a new shot (a cut) that is not decoded. Round 7.3 (audit 5a): a cut
/// whose depth is ready switches at once at the level it was at (the picture changes completely there, so a depth change
/// is hidden; the old "start flat and inflate over 150 ms" pulsed at every cut, and at a flash that tripped the cut flag
/// mid-shot); the 150 ms ramp stays for returns from flat (the movie's start, a lag, a fallback).
inline Tout step(Tstate& t, const Tcfg& c, double now, int32_t want, int sel, const Avail& a, const Bits& b, float s_target, uint32_t frames = 0) {
    const double gap = t.have_t ? (now - t.t_prev < 0 ? 0 : now - t.t_prev) : 0.0;   // the level moves by the real time ...
    const double dt = gap > 100 ? 100 : gap;                                             // ... the strength by at most 100 ms a step
    if (!t.have_t) t.s_cur = s_target;
    t.have_t = true; t.t_prev = now;
    const float ds = static_cast<float>(c.s_rate * dt / 1000.0);
    t.s_cur += clampf(s_target - t.s_cur, -ds, ds);
    const float dl = static_cast<float>(gap / c.ramp_ms);
    Tout o = { false, false, false, false, W_DEPTH, 0.f, t.bound, 0.f, false, false };
    enum { OK, FADE, FLAT } act = OK;
    if (want < 0) { o.why = sel == SEL_PAST ? W_PAST : W_NO_FRAME; act = (sel == SEL_PAST && t.bound >= 0) ? FADE : FLAT; }
    else if (want == t.bound) o.why = W_DEPTH;                          // held between video frames: the texture has it
    else if (bit(b.flat, static_cast<uint32_t>(want))) { o.why = W_FLAT_FRAME; act = (t.bound >= 0 && !cut_between(b, t.bound, want)) ? FADE : FLAT; }   // review7 SF-1: fade only within the shot; a cut onto a flat frame is flat at once
    else if (a.ready && !a.error) {
        const bool cut = t.bound < 0 || want < t.bound || cut_between(b, t.bound, want);
        const bool from_flat = !t.shown;
        if (from_flat || (cut && c.old_cut_ramp)) { t.lvl = 0.f; o.ramp_start = true; }
        o.cut = cut && !from_flat && !c.old_cut_ramp; o.reset = cut || from_flat;
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
    o.level = lv; o.bound = t.bound;
    o.depth = act != FLAT && t.lvl > 0.f;
    if (act == OK && o.ramp_start) o.depth = true;                      // the first Present of a ramp: drawn (flat) as the relief
    o.fading = o.depth && act == FADE;
    t.shown = o.depth;
    o.s = o.depth ? t.s_cur * smooth01(lv) : 0.f;
    return o;
}

// -- the mapping: what the vertex shader does, on the CPU (the "3D movie" mapping, owner test 4 Nov 2026) ------------------
// VDA's output is relative inverse depth, known only up to an affine map (scale and shift). The 8-bit value n = q / 255 is
// that disparity normalised per shot between lo and hi (FORMAT.md), so the stereographer's choice is the affine map itself:
// n = 1 (the shot's nearest, hi) -> the near distance Zn = Ds - pop, n = 0 (the farthest, lo) -> Zf = Ds + range, linear in
// disparity in between (depth = 1 / disparity): 1/Z(n) = 1/Zf + n (1/Zn - 1/Zf). Ds is the viewer's distance to the screen
// (the screen sits Ds ahead of the reference: screen_distance). The amount k (strength x the ramp level) blends toward the
// flat screen in disparity: 1/Z = 1/Ds + k D, D = 1/Z(n) - 1/Ds, so k = 0 is the flat screen. The window rule (owner test
// round 3: the top edge made a "shelf"): content behind the screen (D < 0) is never feathered: along its ray it stays inside
// the screen's outline, as through a window, and the black around the frame is the window's frame. Only content in front
// of the screen (D > 0) is scaled by the edge weight, to 0 at the border (the floating window). The subtitle band, when on,
// scales both (the bottom 15 % lies on the screen plane, readable).
// A point goes along the ray from the nominal eye E (Ds in front of the screen's centre, on its normal) through its pixel,
// to distance Z: from E the picture is exactly the flat screen's (no pixel moves, nothing is uncovered); the depth comes
// from the two eyes' parallax. In the screen's frame (x right, y up, +z toward the viewer, E = (0, 0, Ds)), with t = Z / Ds:
// P = (x t, y t, Ds (1 - t)).
/// ex: the nominal eye's x in the screen's frame (review3 N-1: the midpoint between the eyes; the screen is placed from the
/// left eye at the recentre, so ex = ipd / 2). pop / range: what was applied (after the comfort limits); *_limited: capped.
struct Map { float Ds, inv_n, inv_f, k, ex, pop, range; bool pop_limited, range_limited; };
constexpr float k_min_near = 0.5f;   // the near distance is at least this (m): a pop-out setting larger than Ds - 0.5 is capped
constexpr float k_ipd = 0.063f;      // the eye separation the comfort limits are reckoned with (m)
constexpr float k_max_crossed_deg = 1.0f, k_max_uncrossed_deg = 1.5f;   // review3 SF-2: at any screen distance (see the design note)
/// Ds: the screen's distance (m), range: how far behind it the farthest content goes (m), pop: how far in front the nearest
/// goes (m), k: the amount (0..1), ex: the nominal eye's x (m). The pop-out is capped so the nearest content's crossed
/// disparity against the screen plane is at most k_max_crossed_deg (and the nearest at least k_min_near away), the range so
/// the farthest's uncrossed disparity is at most k_max_uncrossed_deg (at Ds >= 2.41 m even infinity is within it).
inline Map make_map(float Ds, float range, float pop, float k, float ex = 0.f) {
    Ds = fmaxf(Ds, 0.6f); range = clampf(range, 0.f, 50.f); pop = clampf(pop, 0.f, Ds - k_min_near);
    Map m = {}; m.Ds = Ds; m.k = clampf(k, 0.f, 1.f); m.ex = ex;
    const float c = k_max_crossed_deg / 57.29578f / k_ipd, u = k_max_uncrossed_deg / 57.29578f / k_ipd;   // in 1/m
    const float zn_min = 1.f / (1.f / Ds + c);
    if (Ds - pop < zn_min) { pop = Ds - zn_min; m.pop_limited = true; }
    if (1.f / Ds - u > 0.f) { const float zf_max = 1.f / (1.f / Ds - u); if (Ds + range > zf_max) { range = zf_max - Ds; m.range_limited = true; } }
    m.pop = pop; m.range = range; m.inv_n = 1.f / (Ds - pop); m.inv_f = 1.f / (Ds + range);
    return m;
}
/// The distance from E (m) of disparity n (0..1); w_pop (0..1) scales only the part in front of the screen (the edge band),
/// w_all (0..1) all of it (the subtitle band). Continuous in n, w_pop and w_all (D * w_pop and D agree at D = 0).
inline float map_z(const Map& m, float n, float w_pop, float w_all = 1.f) {
    const float inv0 = 1.f / m.Ds, inv = m.inv_f + clampf(n, 0.f, 1.f) * (m.inv_n - m.inv_f);
    float d = inv - inv0;
    d *= (d > 0.f ? w_pop : 1.f) * w_all;
    return 1.f / (inv0 + m.k * d);
}
/// The point in the screen's frame for screen position (x, y) (m) at disparity n and the weights: on the ray from the
/// nominal eye E = (ex, 0, Ds) through (x, y, 0), at depth Z behind E: P = E + t (S - E), t = Z / Ds.
inline void map_point(const Map& m, float x, float y, float n, float w_pop, float P[3], float w_all = 1.f) {
    const float t = map_z(m, n, w_pop, w_all) / m.Ds;
    P[0] = m.ex + (x - m.ex) * t; P[1] = y * t; P[2] = m.Ds * (1.f - t);
}
/// The angular disparity (degrees) of a point at distance Z against the screen plane at Ds, for an eye separation ipd:
/// + crossed (in front of the screen), - uncrossed (behind). The comfort figure of the design note.
inline float disparity_deg(float Z, float Ds, float ipd = 0.063f) { return ipd * (1.f / Z - 1.f / Ds) * 57.29578f; }

constexpr float k_feather = 0.04f, k_feather_min = 0.01f, k_feather_max = 0.15f, k_band = 0.15f, k_band_ramp = 0.05f;   // k_feather: the edge band's default
struct Weights { float pop, all; };
/// The weights at grid point (a, b) (0..1 across the video rect, b down). pop (for content in front of the screen only):
/// 0 at the border, 1 from `band` in (smoothstep; band 0: 1 everywhere). all (everything; flat_bottom only): 0 in the bottom
/// 15 % (b >= 0.85), ramping from 1 at b = 0.80.
inline Weights weights(float a, float b, float band, bool flat_bottom) {
    const float e = fminf(fminf(a, 1.f - a), fminf(b, 1.f - b));
    Weights w = { band > 0.f ? smooth01(e / band) : 1.f, 1.f };
    if (flat_bottom) w.all = 1.f - smooth01((b - (1.f - k_band - k_band_ramp)) / k_band_ramp);
    return w;
}
/// The edge fill (Debug, off by default; round 6). The first version measured each triangle's disparity slope in the pixel
/// shader (ddx / ddy: constant over a triangle) and recoloured the whole triangle from two cells toward the far side: but
/// seen from the nominal eye every triangle covers exactly its own cell of the picture (each vertex is on its pixel's ray),
/// so that recoloured the picture itself in cell-sized triangles along every silhouette, switching triangle by triangle:
/// the owner's "tetris" blocks. Now the weight is per vertex and interpolated (continuous across every triangle edge): the
/// slope is a central difference of 1/Z over the vertex's neighbours one cell away (the same in x and y, any range), ramped
/// softly, and gated by how far the vertex lies toward the far side of its neighbourhood (0 on the near side of a depth
/// edge, so the near object is never recoloured; 1 on the far side); the fill sample is offset from the vertex toward
/// the far side by up to two cells, the offset shrinking with the slope below the ramp. It still changes the picture seen
/// from the nominal eye in the cell next to a silhouette, so it stays an experiment (review5 SF-1: it still recolours the
/// near object's outer ~1.5 cells by up to 0.64). Planned replacement, not built: a per-pixel gate on the stretch this eye
/// sees (from ddx / ddy of the uv in that eye: 1 everywhere from the nominal eye) with a far-side sample from a blurred mip.
/// Thresholds in 1/m per m of |grad(1/Z)|: a central difference sees a sharp step over two cells, so these are half the
/// one-cell 0.738 / 2.305 (round 5): a step of about 12 levels between neighbours starts the fill at range 3 m.
constexpr float k_edge_lo = 0.369f, k_edge_hi = 1.153f, k_edge_cells = 2.f;
inline float edge_fill(float slope) { return smooth01((slope - k_edge_lo) / (k_edge_hi - k_edge_lo)); }
/// One vertex's fill (the VS's copy): n and its neighbours left / right / up / down one cell away (disparity 0..1), the
/// vertex's weights, the cell in metres (cu, cv) and in the colour texture's uv (tu, tv). f: the fill amount 0..1; du, dv:
/// the sample's uv offset.
struct FillV { float f, du, dv, slope; };
inline FillV fill_vertex(const Map& m, float n, float nl, float nr, float nu, float nd, float w_pop, float w_all, float cu, float cv, float tu, float tv) {
    auto q = [&](float x) { return 1.f / map_z(m, x, w_pop, w_all); };
    const float q0 = q(n), ql = q(nl), qr = q(nr), qu = q(nu), qd = q(nd);
    const float gu = (qr - ql) / (2.f * cu), gv = (qd - qu) / (2.f * cv);   // v down, as the uv
    const float sl = sqrtf(gu * gu + gv * gv);
    const float qmax = fmaxf(fmaxf(fmaxf(ql, qr), fmaxf(qu, qd)), q0), qmin = fminf(fminf(fminf(ql, qr), fminf(qu, qd)), q0);
    const float farside = qmax - qmin > 1e-9f ? clampf((qmax - q0) / (qmax - qmin), 0.f, 1.f) : 0.f;
    const float sc = k_edge_cells / fmaxf(sl, k_edge_lo);
    return { edge_fill(sl) * farside, -gu * sc * tu, -gv * sc * tv, sl };
}
/// The fill a sharp step from n0 to n1 between neighbouring vertices a cell of `cell_m` apart gives (its slope as the
/// central difference sees it: the step over two cells).
inline float edge_fill_between(const Map& m, float n0, float n1, float cell_m) {
    return edge_fill(fabsf(1.f / map_z(m, n0, 1.f) - 1.f / map_z(m, n1, 1.f)) / (2.f * cell_m));
}

// -- noise ------------------------------------------------------------------------------------------------------------
/// A 3 x 3 median (edges: the nearest pixel repeated), src -> dst (w x h, not in place). The worker runs it after the decode:
/// it removes the model's speckle and keeps the depth edges.
inline void median3(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h) {
    auto srt = [](uint8_t& a, uint8_t& b) { if (a > b) { const uint8_t t = a; a = b; b = t; } };
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* r0 = src + static_cast<size_t>(y > 0 ? y - 1 : 0) * w;
        const uint8_t* r1 = src + static_cast<size_t>(y) * w;
        const uint8_t* r2 = src + static_cast<size_t>(y + 1 < h ? y + 1 : y) * w;
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t xl = x > 0 ? x - 1 : 0, xr = x + 1 < w ? x + 1 : x;
            uint8_t p[9] = { r0[xl], r0[x], r0[xr], r1[xl], r1[x], r1[xr], r2[xl], r2[x], r2[xr] };
            // the 19-exchange median network (Paeth / Devillard)
            srt(p[1], p[2]); srt(p[4], p[5]); srt(p[7], p[8]); srt(p[0], p[1]); srt(p[3], p[4]); srt(p[6], p[7]);
            srt(p[1], p[2]); srt(p[4], p[5]); srt(p[7], p[8]); srt(p[0], p[3]); srt(p[5], p[8]); srt(p[4], p[7]);
            srt(p[3], p[6]); srt(p[1], p[4]); srt(p[2], p[5]); srt(p[4], p[7]); srt(p[4], p[2]); srt(p[6], p[4]);
            srt(p[4], p[2]);
            dst[static_cast<size_t>(y) * w + x] = p[4];
        }
    }
}
constexpr int k_ema_gate = 24;   // a change larger than this (of 255 levels) is motion, not noise: the pixel follows at once
/// The causal filter (Debug since round 7; the default is the worker's centred one), per pixel, at each upload (render
/// thread, display order), on the 16-bit n' (round 7: the R16 upload; it used to be 8.8 on 8 bits): acc follows the new
/// frame by (1 - smooth) of the gap when the gap is below k_ema_gate levels, else jumps to it (no ghost behind a moving edge);
/// reset (a cut, or depth after flat): acc = the frame. io: the new frame in, the filtered frame out (in place).
inline void ema(uint16_t* acc, uint16_t* io, uint32_t n, float smooth, bool reset) {
    const int a = static_cast<int>(256.f * (1.f - clampf(smooth, 0.f, 0.95f)) + 0.5f);   // the share of the gap taken, /256
    const int gate = k_ema_gate * 257;
    for (uint32_t i = 0; i < n; ++i) {
        const int v = io[i];
        int s = acc[i];
        if (reset || a >= 256) s = v;
        else {
            const int d = v - s;
            if (d > gate || d < -gate) s = v;
            else s += (d * a + (d >= 0 ? 128 : -128)) / 256;
        }
        s = s < 0 ? 0 : s > 65535 ? 65535 : s;
        acc[i] = static_cast<uint16_t>(s); io[i] = static_cast<uint16_t>(s);
    }
}

/// Round 7 (audit plan 1): the grid has a vertex at every depth texel's centre, plus one at each edge of the rect: in
/// each axis `texels + 2` vertices, `texels + 1` cells (321 x 181 for 320 x 180; 241 x 181 for 4:3), the inner cells one
/// texel, the border cells half a texel. So each vertex reads one texel of the depth (the 160 x 90 grid sat at texel
/// boundaries: every vertex was the mean of 2 x 2 texels, a quarter of the stored resolution). c: the vertex column / row
/// (0 .. texels + 1). The VS copy: saturate((c - 0.5) / texels).
inline float grid_coord(uint32_t c, uint32_t texels) { return clampf((static_cast<float>(c) - 0.5f) / static_cast<float>(texels), 0.f, 1.f); }
inline uint32_t grid_cells(uint32_t texels) { return texels + 1; }

// -- round 10 (audit plan 5): the edge snap, the guided upsample, the fine grid ----------------------------------------------
// The model's edges are soft, and the area downscale to 320 x 180 leaves mixed texels between a silhouette and what is
// behind it: the rubber sheet's material (7-11 video px wide on round 7's grid). Three parts:
// (1) The worker snaps each texel on a depth edge to the nearer of its 5 x 5 neighbourhood's min and max (a tie to the far
// side: the snap never grows the near side, so a converter v2 file, sharp and grown 2 model px on the near side, keeps its
// edges where they are: on average +-0 texels, at most half a texel). A texel is on an edge when its 3 x 3 range is over
// k_snap_levels and its 5 x 5 range at most 15/8 of that: a ramp up to ~3 texels is a step; any linear slope (5 x 5 range
// exactly twice the 3 x 3 one), a parabola or an exponential (>= 2) is a surface and stays, but for the texel or two next
// to a kink (where a steep slope meets a plateau: a 5-texel ramp between plateaus comes out about 2 texels wide, a longer
// one keeps its middle). The outer 2 texels of the map are never snapped. The snapped map is a second output (snap_out);
// the original stays for round 7's grid and for the fallback below.
// (2) At each depth upload a GPU pass upsamples to twice the size (640 x 360, 480 x 360 for 4:3) guided by the video frame
// on screen (k_jbu_hlsl in ffxvr_native.cpp; guide_ref / jbu_ref below are its CPU copy, and the unit test runs the
// shader's own text against them). A first pass makes the guide: each depth texel's footprint luma (4 bilinear samples at
// its quarter points: a 4 x 4 video px mean at 720p). Then, per output texel, the 5 x 5 snapped texels around it split
// at the middle of their range into a near and a far layer; each layer's depth is a spatially weighted mean (sigma
// k_jbu_sigma_s texels) and its luma the plain mean of its texels, and the output takes the layers by how close its own
// luma (its 2 x 2 video px) is to each layer's (sigma k_jbu_sigma_r), times the layer's spatial weight. A two-layer
// joint bilateral filter: where a per-texel filter gives a texel of the grown near side that shows the background's
// colour full weight (a 3-4 px halo on v2 files), the near layer's mean colour stays the foreground's (unweighted: the
// texel next to the output, the one most likely mislabelled, does not dominate it). Where the depth range is small (< k_jbu_edge) or the layers' lumas are close (< k_jbu_c0 .. c1)
// it fades to the original map, bilinear (round 7's value): no snap crawl where the picture has no edge to follow.
// (3) A vertex per output texel (641 x 361 cells), drawn as indexed triangle strips (strip_indices).
constexpr int k_snap_levels = 12;            // the 3 x 3 range (8-bit levels) over which a texel may be on an edge
constexpr int k_snap_r5 = 15, k_snap_r3 = 8; // ... and its 5 x 5 range at most 15/8 of that (a 3-texel ramp: <= 5/3; a slope: 2)
constexpr uint32_t k_up = 2;                 // the upsample's factor (both ways)
constexpr int k_jbu_runs = 2;                // runs a depth upload: that Present and the next (the copy may show the video a Present late)
constexpr int k_jbu_r = 2;                   // the filter's radius (low-res texels): 5 x 5
constexpr float k_jbu_sigma_s = 1.0f, k_jbu_sigma_r = 0.05f;
constexpr float k_jbu_edge = 0.05f;          // the 5 x 5 depth range (n') where the layers start (full at twice that)
constexpr float k_jbu_c0 = 0.05f, k_jbu_c1 = 0.15f;   // the layers' luma difference over which the guide takes over
constexpr float k_luma_r = 0.2126f, k_luma_g = 0.7152f, k_luma_b = 0.0722f;
/// The snap: out = in with each edge texel snapped (16-bit n', w x h; scratch: 4 w h values, the separable min / max).
inline void snap_out(const uint16_t* in, uint16_t* out, uint32_t w, uint32_t h, std::vector<uint16_t>& scratch, int thr_levels = k_snap_levels) {
    if (w == 0 || h == 0) return;
    const size_t n = static_cast<size_t>(w) * h;
    scratch.resize(4 * n);
    uint16_t *mn3 = scratch.data(), *mx3 = mn3 + n, *mn5 = mx3 + n, *mx5 = mn5 + n;
    for (uint32_t y = 0; y < h; ++y) {   // across: 3 and 5 wide (clamped at the borders)
        const uint16_t* r = in + static_cast<size_t>(y) * w;
        for (uint32_t x = 0; x < w; ++x) {
            int a3 = 65535, b3 = 0, a5 = 65535, b5 = 0;
            for (int d = -2; d <= 2; ++d) {
                const int xx = (std::min)((std::max)(static_cast<int>(x) + d, 0), static_cast<int>(w) - 1); const int v = r[xx];
                a5 = (std::min)(a5, v); b5 = (std::max)(b5, v);
                if (d >= -1 && d <= 1) { a3 = (std::min)(a3, v); b3 = (std::max)(b3, v); }
            }
            const size_t k = static_cast<size_t>(y) * w + x;
            mn3[k] = static_cast<uint16_t>(a3); mx3[k] = static_cast<uint16_t>(b3); mn5[k] = static_cast<uint16_t>(a5); mx5[k] = static_cast<uint16_t>(b5);
        }
    }
    const int thr = thr_levels * 257;
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) {   // down, then the rule
        int a3 = 65535, b3 = 0, a5 = 65535, b5 = 0;
        for (int d = -2; d <= 2; ++d) {
            const size_t k = static_cast<size_t>((std::min)((std::max)(static_cast<int>(y) + d, 0), static_cast<int>(h) - 1)) * w + x;
            a5 = (std::min)(a5, static_cast<int>(mn5[k])); b5 = (std::max)(b5, static_cast<int>(mx5[k]));
            if (d >= -1 && d <= 1) { a3 = (std::min)(a3, static_cast<int>(mn3[k])); b3 = (std::max)(b3, static_cast<int>(mx3[k])); }
        }
        const size_t k = static_cast<size_t>(y) * w + x;
        const int v = in[k], r3 = b3 - a3, r5 = b5 - a5;
        const bool border = x < 2 || y < 2 || x + 2 >= w || y + 2 >= h;   // the clamped window looks like a kink there: kept
        if (border || r3 <= thr || r5 * k_snap_r3 > r3 * k_snap_r5) { out[k] = static_cast<uint16_t>(v); continue; }
        out[k] = static_cast<uint16_t>(v - a5 <= b5 - v ? a5 : b5);   // the nearer; a tie to the far side (0 is far)
    }
}
inline float smooth01(float a, float b, float x) { const float t = clampf((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
/// The guide (the first pass): depth texel (x, y)'s footprint luma. luma(u, v): the picture's luma, bilinear, at a point of
/// the rect (0..1 both ways; the shader samples the backbuffer copy at R.xy + (u, v) R.zw).
template <class L> inline float guide_ref(L luma, uint32_t w, uint32_t h, uint32_t x, uint32_t y) {
    const float u0 = (x + 0.25f) / w, u1 = (x + 0.75f) / w, v0 = (y + 0.25f) / h, v1 = (y + 0.75f) / h;
    return 0.25f * (luma(u0, v0) + luma(u1, v0) + luma(u0, v1) + luma(u1, v1));
}
/// The upsample (the second pass) at output texel (i, j) of (k_up w) x (k_up h): sn the snapped map, orig the original,
/// guide guide_ref's (all w x h, 0..1).
template <class L> inline float jbu_ref(const float* sn, const float* orig, const float* guide, uint32_t w, uint32_t h, L luma, uint32_t i, uint32_t j) {
    const float ox = (static_cast<float>(i) + 0.5f) / static_cast<float>(k_up * w), oy = (static_cast<float>(j) + 0.5f) / static_cast<float>(k_up * h);
    const float px = ox * w - 0.5f, py = oy * h - 0.5f;
    const int bx = static_cast<int>(floorf(px + 0.5f)), by = static_cast<int>(floorf(py + 0.5f));
    auto at = [&](const float* a, int x, int y) { return a[static_cast<size_t>((std::min)((std::max)(y, 0), static_cast<int>(h) - 1)) * w + (std::min)((std::max)(x, 0), static_cast<int>(w) - 1)]; };
    float mn = 1.f, mx = 0.f;
    for (int dj = -k_jbu_r; dj <= k_jbu_r; ++dj) for (int di = -k_jbu_r; di <= k_jbu_r; ++di) { const float d = at(sn, bx + di, by + dj); mn = (std::min)(mn, d); mx = (std::max)(mx, d); }
    const float mid = 0.5f * (mn + mx);
    double S[2] = { 0, 0 }, D[2] = { 0, 0 }, G[2] = { 0, 0 }, C[2] = { 0, 0 };   // [1] the near layer
    for (int dj = -k_jbu_r; dj <= k_jbu_r; ++dj) for (int di = -k_jbu_r; di <= k_jbu_r; ++di) {
        const int qx = (std::min)((std::max)(bx + di, 0), static_cast<int>(w) - 1), qy = (std::min)((std::max)(by + dj, 0), static_cast<int>(h) - 1);
        const float d = at(sn, qx, qy), dx = qx - px, dy = qy - py;
        const double s = exp(-(dx * dx + dy * dy) / (2.0 * k_jbu_sigma_s * k_jbu_sigma_s));
        const int l = d > mid ? 1 : 0;
        S[l] += s; D[l] += s * d; G[l] += at(guide, qx, qy); C[l] += 1.0;   // the luma unweighted (above)
    }
    // the original map, bilinear (D3D's sampler: texel centres at (k + 0.5) / w, clamped)
    const float tx = ox * w - 0.5f, ty = oy * h - 0.5f; const int x0 = static_cast<int>(floorf(tx)), y0 = static_cast<int>(floorf(ty)); const float fx = tx - x0, fy = ty - y0;
    const float lin = (at(orig, x0, y0) * (1 - fx) + at(orig, x0 + 1, y0) * fx) * (1 - fy) + (at(orig, x0, y0 + 1) * (1 - fx) + at(orig, x0 + 1, y0 + 1) * fx) * fy;
    if (S[1] <= 0.0 || S[0] <= 0.0) return lin;
    for (int l = 0; l < 2; ++l) { D[l] /= S[l]; G[l] /= C[l]; }
    const double lp = luma(ox, oy), e0 = (lp - G[0]) * (lp - G[0]), e1 = (lp - G[1]) * (lp - G[1]), m = (std::min)(e0, e1);
    const double k = 1.0 / (2.0 * k_jbu_sigma_r * k_jbu_sigma_r);
    const double w0 = S[0] * exp(-(e0 - m) * k), w1 = S[1] * exp(-(e1 - m) * k);   // relative to the nearer layer: never both 0
    const double lay = (w0 * D[0] + w1 * D[1]) / (w0 + w1);
    const float b = smooth01(k_jbu_edge, 2.f * k_jbu_edge, mx - mn) * smooth01(k_jbu_c0, k_jbu_c1, static_cast<float>(fabs(G[1] - G[0])));
    return static_cast<float>(lin + b * (lay - lin));
}
// -- round 10b: the grid as a Debug choice, and the edge softening ---------------------------------------------------------
// The owner's first look (v2 files, amount 1, range 4.7 m): the guided grid streaks and distorts; round 7's grid shows v2's
// sharp edges as a texel staircase. The grid is now a three-way choice (ffxvr_fmv3d_set flags bits 8-9): round 7's 320
// grid (the default), the 640 grid on the plain depth map (bilinear at each vertex: no snap, no luma pick), and the guided
// upsample. The edge softening (bits 10-13, tenths of a depth texel, 0..1.5) averages 4 bilinear taps at +-half its width
// in the vertex shader's depth fetch (depth_at): a box of that width on the bilinear hat, separable.
enum GridMode : int { GRID_320 = 0, GRID_SMOOTH = 1, GRID_GUIDED = 2 };
inline int grid_mode(int flags) { const int m = (flags >> 8) & 3; return m == GRID_SMOOTH || m == GRID_GUIDED ? m : GRID_320; }
inline float soft_texels(int flags) { return static_cast<float>((flags >> 10) & 15) / 10.f; }
inline int soft_flags(float texels) { return (static_cast<int>(clampf(texels, 0.f, 1.5f) * 10.f + 0.5f) & 15) << 10; }
/// The vertex shader's depth fetch (dsamp): map d (w x h, 0..1) at (u, v) with the softening (texels; 0: one bilinear tap).
inline float depth_at(const float* d, uint32_t w, uint32_t h, float u, float v, float soft) {
    auto bil = [&](float a, float b) {
        const float x = a * w - 0.5f, y = b * h - 0.5f; const int x0 = static_cast<int>(floorf(x)), y0 = static_cast<int>(floorf(y)); const float fx = x - x0, fy = y - y0;
        auto at = [&](int xx, int yy) { xx = (std::min)((std::max)(xx, 0), static_cast<int>(w) - 1); yy = (std::min)((std::max)(yy, 0), static_cast<int>(h) - 1); return d[static_cast<size_t>(yy) * w + xx]; };
        return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
    };
    if (soft <= 0.f) return bil(u, v);
    const float ou = 0.5f * soft / w, ov = 0.5f * soft / h;
    return 0.25f * (bil(u - ou, v - ov) + bil(u + ou, v + ov) + bil(u + ou, v - ov) + bil(u - ou, v + ov));
}
/// The fine grid's indices: one triangle strip per row of cells, the rows cut by 0xFFFFFFFF (D3D11's strip cut for 32-bit
/// indices). Vertex id = row x (cx + 1) + column (cx / cy: cells across / down); the VS takes the id (SV_VertexID is the
/// index) apart. Each cell: (0,0) (0,1) (1,0), then (0,1) (1,0) (1,1): the same diagonal as the round 7 list.
inline void strip_indices(uint32_t cx, uint32_t cy, std::vector<uint32_t>& out) {
    out.clear(); out.reserve(static_cast<size_t>(cy) * (2 * (cx + 1) + 1));
    for (uint32_t j = 0; j < cy; ++j) {
        for (uint32_t i = 0; i <= cx; ++i) { out.push_back(j * (cx + 1) + i); out.push_back((j + 1) * (cx + 1) + i); }
        out.push_back(0xFFFFFFFFu);
    }
}
inline uint32_t strip_index_count(uint32_t cx, uint32_t cy) { return cy * (2 * (cx + 1) + 1); }

/// The video's rect in the backbuffer (uv, the game letterboxes / pillarboxes it centred): vw x vh into bw x bh.
struct Rect { float u0, v0, du, dv; };
/// Round 7.5 (audit 6): the colour's uv kept half a backbuffer texel inside the rect, so the edge vertices of a pillarboxed
/// (4:3) video do not sample the boundary between the picture and the black bar (a 50 % blend: a dark 1-texel rim). The VS
/// copy: clamp(uv, R.xy + Y.xy, R.xy + R.zw - Y.xy), Y = 0.5 / the backbuffer's size.
inline void inset_uv(const Rect& r, uint32_t bw, uint32_t bh, float& u, float& v) {
    const float hx = 0.5f / static_cast<float>(bw ? bw : 1), hy = 0.5f / static_cast<float>(bh ? bh : 1);
    u = clampf(u, r.u0 + hx, r.u0 + r.du - hx); v = clampf(v, r.v0 + hy, r.v0 + r.dv - hy);
}
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
/// Review SF-1: the field of view that just covers the screen's video rect seen from an eye at (eq, ep): the frustum from the
/// nominal eye (ex, 0, Ds) through the rect's four corners, cut at t = t_near and t_far (map_point's t: the relief lies inside
/// it, and a projection of a convex body is bounded by its corners'), widened by `margin` of the span on each side plus
/// 0.004, clamped to `full`. The same eye image then covers less of the view, so the video gets more of its pixels. False
/// (out = full): a corner is not in front of the eye, or the rect is (nearly) out of view.
inline bool tight_fov(const float eq[4], const float ep[3], const Fov& full, const float sq[4], const float sp[3], float W, float H,
                      const Rect& rc, float Ds, float t_near, float t_far, float margin, Fov* out, float ex = 0.f) {
    *out = full;
    float V[16], M[16], VM[16]; view_mat(eq, ep, V); pose_mat(sq, sp, M); mat4_mul(V, M, VM);
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (int k = 0; k < 8; ++k) {
        const float u = rc.u0 + ((k & 1) ? rc.du : 0.f), v = rc.v0 + ((k & 2) ? rc.dv : 0.f), t = (k & 4) ? t_near : t_far;
        const float lx = ex + ((u - 0.5f) * W - ex) * t, ly = (0.5f - v) * H * t, z = Ds * (1.f - t);
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
    if (video_h == 0 || !(fov.u - fov.d > 0.f) || !tight_fov(eq, ep, wide, sq, sp, W, H, rc, 1.f, 1.f, 1.f, 0.f, &v)) return 0.f;
    return static_cast<float>(image_h) * ((v.u - 0.004f) - (v.d + 0.004f)) / (fov.u - fov.d) / static_cast<float>(video_h);
}
inline bool fov_ok(float l, float r, float u, float d) {
    return std::isfinite(l) && std::isfinite(r) && std::isfinite(u) && std::isfinite(d) && r - l > 0.1f && u - d > 0.1f && r - l < 20.f && u - d < 20.f;
}

}  // namespace relief
