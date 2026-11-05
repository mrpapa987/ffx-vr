// SPDX-License-Identifier: MIT
// ffxvr_fmv.h -- the FMV sync probe (S2 of the FMV depth plan, 4 Nov 2026; design-notes "FMV sync probe"). Read-only
// diagnostics: which video plays and which of its frames is on screen. Pure logic: no Windows, no D3D, no logging, no
// allocation. ffxvr_native.cpp (fmv_frame, every Present) reads the blocks below with ffxvr_safe_read, feeds the tracker and
// writes its lines; the .fvd header is read there with ReadFile and checked here. A unit test drives all of it.
//
// The movie player (our exe, VA with the image at 0x400000; each fact re-checked in scratchpad/ffxexe/text.txt):
//   [0xCDED3C]      sFMVPlayerManager* (made at 0x6D717E, 0 at 0x6D738B), 0x750 bytes (push 0x750 at 0x6D715C)
//   mgr+0x6D0 u8    playing: 1 at the end of start 0x6D8FC0 (0x6D9159) and in 0x6DA9B0 (0x6DAB51: opens a video by path,
//                   +0x6D8 = 0, sets neither the name nor the id nor 0xCDED31; no call, jump or data reference to it was
//                   found: probably dead; if it ever runs, the probe names the previous video); 0 in the constructor
//                   0x6D6A89, at the natural end 0x6D7884 (with +0x6D8 = 0xFFFE at 0x6D787A), at a skip 0x6D9559, in stop
//                   0x6D71C7 (word 0x100: +0x6D1 = 1 "destroying"), at load 0x6D9D83 (word 0), and at 0x6DAC40 (from the
//                   event command 0x76EA60 via 0x643E50: playing alone cleared, the probe's "stopped before its end").
//                   The update 0x6D7680 uploads a frame only while it is set (0x6D77A2).
//   mgr+0x6D2 u8    open: the update and draw run only with it (0x6D768A, 0x6D7576); 1 at 0x6D8F6C, 0 in close 0x6D91A3
//   mgr+0x4F0       char[0x80] the file name: load 0x6D9C80 empties it (strcpy "" 0x6D9CC3), 0x6D93E0 fills it with
//                   snprintf(buf, 0x80, "%s", map[id]) (0x6D9436..0x6D943E); unknown id: stays "" ("didn't find FMV")
//   mgr+0x6DC u32   the video id (load's first argument, 0x6D9D4B)
//   mgr+0x6D8 u32   the game's frame number, 0 at load / start (0x6D9D41, 0x6D9016); after each upload (0x6D77C9 inline,
//                   0x6DAE3E in the render-thread job 0x6DAE00) = ftol(float(pos_ms) * 29.97 * 0.001), pos_ms =
//                   0xA27BD0 = trunc((count + 1) * ms_per_frame); 0xFFFE = ended (0x6D787A, skip 0x6D9528); +0x6E0 the
//                   previous value
//   mgr+0x6E4 u8    a frame was uploaded (1 at 0x6D7828 / 0x6DAE9D; 0 at load)
//   mgr+0x6FC u8    paused (the toggle 0x6DA4B0: player pause 0xA27E90 and +0x6FC = !+0x6FC; also 1 from the update itself
//                   at 0x6D785D when +0x6FD is set and the skippable flag is 0; the update skips while set)
//   mgr+0x570 u8    the upload runs on the render thread (0x6D9DC8: ids 0x20 / 0x2E / 0x48 or +0x6FE)
//   mgr+0x74C u8    the skip prompt is up (pad 0x800 at 0x6D94A9 while +0x74D; a second press 0x80 skips, 0x6D94E3)
//   mgr+0x74D u8    skipping is allowed for this id (0x6D9D6A: 0 for the 16 ids of the table 0xB49B08: 2 4 6 18 24 32 38
//                   42 44 46 50 60 62 70 72 84)
//   mgr+0x38        the player object, embedded (`lea ecx, [edi+0x38]` at 0x6D76B4 / 0x6D9012); its fields here as
//                   manager offsets:
//     +0x130 u64    player+0xF8 the duration in ms (0xA28148: the WebM duration in ns * 0.001 * 0.001, 0xB921B0 = 0.001)
//     +0x138 u32    player+0x100 frames uploaded: 0 at open 0xA27F4B, `inc` after the Y/U/V upload 0xA28826 (in 0xA284A0);
//                   no other writer. The frame now in the planes is index count - 1 = the .fvd frame index.
//     +0x140 f64    player+0x108 ms per frame = 1000 / fps (0xA280C9)
//     +0x184 u32    player+0x14C / +0x150 the video's width / height (0xA280DA / 0xA280EB)
//   [0xCDED31] u8   the user skipped: 1 at the skip 0x6D9548 only, 0 at every load 0x6D9D27
//   [0x112A008] u32 a skippable movie was started (1 at 0x76EB62 / 0x76EBC0, 0 at 0x76E968 / 0x76E9D2 / 0x76EA9C / 0x76ECE9);
//                   it lingers (gates review B1)
//   [0xC40E28] s32  the event's movie id (-1 none; set 0x76E937 / 0x76EBA4, -1 at 0x76EA1F / 0x76EA92). 0x76EFC0 returns 1
//                   for ids 0x20 / 0x2E / 0x46 / 0x48: the hybrid (movie + 3D) ids, read by ten callers
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace fmv {

constexpr uint32_t k_mgr_va = 0xCDED3C, k_skippable_va = 0x112A008, k_skipped_va = 0xCDED31, k_movie_id_va = 0xC40E28;
constexpr uint32_t k_mgr_size = 0x750;
// the three blocks read while a movie may be up (all inside the 0x750-byte manager)
constexpr uint32_t k_p_lo = 0x130, k_p_len = 0x5C;   // the player's fields
constexpr uint32_t k_n_lo = 0x4F0, k_n_len = 0x84;   // the name and the render-thread byte
constexpr uint32_t k_s_lo = 0x6D0, k_s_len = 0x80;   // the state
constexpr uint32_t k_dur = 0x130, k_count = 0x138, k_msf = 0x140, k_vw = 0x184, k_vh = 0x188;
constexpr uint32_t k_name_len = 0x80, k_rt = 0x570;
constexpr uint32_t k_playing = 0x6D0, k_open = 0x6D2, k_frame = 0x6D8, k_id = 0x6DC, k_started = 0x6E4, k_paused = 0x6FC,
                   k_prompt = 0x74C, k_skip_ok = 0x74D;
constexpr uint32_t k_frame_end = 0xFFFE;
static_assert(k_p_lo + k_p_len <= k_n_lo && k_n_lo + k_n_len <= k_s_lo && k_s_lo + k_s_len == k_mgr_size, "blocks");

// The view of the frame just presented: slow_frame's numbers (0 no headset, 1 3D, 2 the virtual screen, 3 the stereo screen).
enum Mode : int { M_MONITOR = 0, M_3D = 1, M_SCREEN = 2, M_STEREO = 3 };
static const char* const k_mode[4] = { "monitor (no headset)", "3D", "screen", "stereo screen" };
inline const char* mode_name(int m) { return k_mode[m & 3]; }

inline uint32_t rd32(const uint8_t* b) { uint32_t v; memcpy(&v, b, 4); return v; }
inline uint16_t rd16(const uint8_t* b) { uint16_t v; memcpy(&v, b, 2); return v; }
inline uint64_t rd64(const uint8_t* b) { uint64_t v; memcpy(&v, b, 8); return v; }

/// One Present's read. mgr: the manager pointer was non-null and its playing word read; s_ok / p_ok / n_ok: the blocks.
struct Snap {
    bool mgr = false, s_ok = false, p_ok = false, n_ok = false;
    uint8_t playing = 0, open = 0, started = 0, paused = 0, prompt = 0, skip_ok = 0, rt = 0, skipped = 0;
    uint32_t frame = 0, id = 0, count = 0, vw = 0, vh = 0, skippable = 0;
    int32_t movie_id = -1;
    uint64_t dur_ms = 0; double msf = 0.0;
    char name[k_name_len + 1] = {};
};
inline void parse_state(Snap& s, const uint8_t* b) {   // k_s_len bytes from mgr + k_s_lo
    s.s_ok = true;
    s.playing = b[k_playing - k_s_lo]; s.open = b[k_open - k_s_lo]; s.started = b[k_started - k_s_lo]; s.paused = b[k_paused - k_s_lo];
    s.prompt = b[k_prompt - k_s_lo]; s.skip_ok = b[k_skip_ok - k_s_lo];
    s.frame = rd32(b + (k_frame - k_s_lo)); s.id = rd32(b + (k_id - k_s_lo));
}
inline void parse_player(Snap& s, const uint8_t* b) {   // k_p_len bytes from mgr + k_p_lo
    s.p_ok = true;
    s.dur_ms = rd64(b + (k_dur - k_p_lo)); s.count = rd32(b + (k_count - k_p_lo));
    memcpy(&s.msf, b + (k_msf - k_p_lo), 8);
    s.vw = rd32(b + (k_vw - k_p_lo)); s.vh = rd32(b + (k_vh - k_p_lo));
}
inline void parse_name(Snap& s, const uint8_t* b) {   // k_n_len bytes from mgr + k_n_lo
    s.n_ok = true;
    memcpy(s.name, b, k_name_len); s.name[k_name_len] = 0;
    for (uint32_t i = 0; i < k_name_len && s.name[i]; ++i) { const unsigned char c = static_cast<unsigned char>(s.name[i]); if (c < 0x20 || c > 0x7E) s.name[i] = '?'; }
    s.rt = b[k_rt - k_n_lo];
}

/// The video's frame count by the game's duration (29.97 fps = 30000/1001), rounded.
inline uint32_t frames_from_ms(uint64_t ms) { return static_cast<uint32_t>((static_cast<double>(ms) * 30.0 / 1001.0) + 0.5); }
inline double ms_from_frames(double n) { return n * 1001.0 / 30.0; }

// -- the .fvd header (tools/fmvdepth/FORMAT.md, "Reader validation" 1-5) ------------------------------------------------
enum FvdStatus : int { FVD_UNCHECKED = 0, FVD_OK, FVD_ABSENT, FVD_NO_NAME, FVD_READ, FVD_SHORT, FVD_MAGIC, FVD_VERSION, FVD_INCOMPLETE,
                       FVD_HEADER, FVD_CODEC, FVD_INDEX, FVD_THUMBS, FVD_CHECKING, FVD_CLOUD, FVD_N };
static const char* const k_fvd[FVD_N] = { "not checked", "present", "absent", "absent (the name has no depth file form)", "unreadable",
    "invalid (shorter than a header)", "invalid (not an .fvd file)", "invalid (a version this build does not know)", "invalid (unfinished)",
    "invalid (header size or frame size)", "invalid (codec or predictor)", "invalid (index out of the file)", "invalid (thumbnails out of the file)",
    "checking...", "not checked (cloud-only)" };
inline const char* fvd_text(int st) { return st >= 0 && st < FVD_N ? k_fvd[st] : "?"; }
struct FvdInfo {
    int status = FVD_UNCHECKED;
    uint32_t frames = 0, w = 0, h = 0, fps_num = 0, fps_den = 0;
    uint8_t codec = 0, predictor = 0, model = 0, flags = 0;
    uint64_t size = 0;
    char src[48] = {};
    bool name_match = false;   // src_name equals the game's name, ignoring case
    char file[48] = {};        // the file name looked for (fvd_file_name)
};
/// "OPN_us.webm" -> "opn_us.fvd": the converter's rule (FORMAT.md: the name without .webm, lower-cased) and its filter
/// ([A-Za-z0-9_]+.webm, no Windows device name); false for any other name (no file is looked for).
/// The file part of the game's movie name: the game stores the full archive path ("/FFX_Data/GameData/PS3Data/Video/
/// OPN_us.webm", seen on the owner's PC); the depth file and the .fvd's src_name use the file name alone.
inline const char* base_name(const char* p) {
    if (p == nullptr) return nullptr;
    const char* b = p;
    for (const char* q = p; *q; ++q) if (*q == '/' || *q == '\\') b = q + 1;
    return b;
}
inline bool fvd_file_name(const char* game, char* out, size_t cap) {
    if (cap) out[0] = 0;
    game = base_name(game);
    if (game == nullptr || cap < 8) return false;
    const size_t n = strnlen(game, k_name_len);
    if (n <= 5 || n - 5 + 5 > cap) return false;   // stem + ".fvd" + NUL
    const char* ext = game + n - 5;
    const char e[] = ".webm";
    for (int i = 0; i < 5; ++i) { char c = ext[i]; if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32); if (c != e[i]) return false; }
    const size_t stem = n - 5;
    for (size_t i = 0; i < stem; ++i) {
        char c = game[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) { out[0] = 0; return false; }
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        out[i] = c;
    }
    out[stem] = 0;
    static const char* const dev[] = { "con", "prn", "aux", "nul", "conin$", "conout$" };
    for (const char* d : dev) if (strcmp(out, d) == 0) { out[0] = 0; return false; }
    if (stem == 4 && (memcmp(out, "com", 3) == 0 || memcmp(out, "lpt", 3) == 0) && out[3] >= '0' && out[3] <= '9') { out[0] = 0; return false; }
    memcpy(out + stem, ".fvd", 5);
    return true;
}
inline bool name_ieq(const char* a, const char* b) {
    for (;; ++a, ++b) {
        char x = *a, y = *b; if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32); if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
        if (x == 0) return true;
    }
}
/// The header's checks 1-5 (64-bit arithmetic). b: the first n bytes of the file (n >= 128 for a complete header),
/// file_size: the whole file's. game: the game's name for the video (name_match), or null.
inline int fvd_parse(const uint8_t* b, size_t n, uint64_t file_size, const char* game, FvdInfo& o) {
    o.size = file_size;
    if (n < 128 || file_size < 128) return o.status = FVD_SHORT;
    if (memcmp(b, "FVD1", 4) != 0) return o.status = FVD_MAGIC;
    if (rd16(b + 4) != 1) return o.status = FVD_VERSION;
    o.flags = b[27];
    if (!(o.flags & 1)) return o.status = FVD_INCOMPLETE;
    const uint64_t hs = rd16(b + 6);
    o.w = rd16(b + 8); o.h = rd16(b + 10); o.frames = rd32(b + 12); o.fps_num = rd32(b + 16); o.fps_den = rd32(b + 20);
    o.codec = b[24]; o.predictor = b[25]; o.model = b[26];
    memcpy(o.src, b + 80, 47); o.src[47] = 0;
    o.name_match = game != nullptr && name_ieq(o.src, base_name(game));
    if (hs < 128 || o.w == 0 || o.h == 0) return o.status = FVD_HEADER;
    if (o.codec > 2 || o.predictor > 1) return o.status = FVD_CODEC;
    const uint64_t io = rd64(b + 40), n_idx = 32ull * o.frames;
    if (io < hs || io > file_size || n_idx > file_size - io) return o.status = FVD_INDEX;
    if (o.flags & 2) {
        const uint64_t to = rd64(b + 48), tw = rd16(b + 56), th = rd16(b + 58), tb = static_cast<uint64_t>(o.frames) * tw * th;
        if (tw == 0 || th == 0 || to < io + n_idx || to > file_size || tb > file_size - to) return o.status = FVD_THUMBS;
    }
    return o.status = FVD_OK;
}

// -- one FMV, from the first Present with the playing byte to the first without it --------------------------------------
enum End : int { E_NONE = 0, E_ENDED, E_SKIPPED, E_STOPPED, E_REPLACED, E_GONE, E_N };
static const char* const k_end[E_N] = { "-", "ended (the player reached the end: frame number 0xFFFE)", "SKIPPED by the user",
    "stopped before its end (playing cleared without the end mark)", "replaced by another video (a new id, name or a count reset)", "the movie player went away" };
inline const char* end_text(int e) { return e >= 0 && e < E_N ? k_end[e] : "?"; }
struct Fmv {
    bool on = false;
    char name[k_name_len + 1] = {};
    uint32_t id = 0, vw = 0, vh = 0, skippable = 0; int32_t movie_id = -1;
    uint8_t rt = 0, skip_ok = 0; int mode0 = 0;
    uint64_t dur_ms = 0; double msf = 0.0;
    double t0 = 0, t_end = 0;            // ms: the first Present playing, the first one not (or the replacement)
    double t_first = 0, t_last = 0;      // ms: the first count >= 1 seen, the last count change
    uint32_t presents = 0, by_mode[4] = {}, cam3d = 0, paused = 0, prompt = 0;
    bool have_u = false; uint32_t u_first = 0, u_last = 0, u_changes = 0, u_jump_max = 0, u_missed = 0, u_start = 0;
    double gap_max = 0;                  // ms between two count changes
    bool have_f = false; uint32_t f_first = 0, f_last = 0, f_jump_max = 0, f_back = 0;
    bool have_fu = false; int32_t fu_min = 0, fu_max = 0;   // frame number - count, while both are known
    int end = E_NONE; uint32_t f_end = 0; uint8_t skipped_end = 0;
};
enum : int { EV_START = 1, EV_END = 2 };
inline void fmv_start(Fmv& f, const Snap& s, double now, int mode) {
    f = Fmv{};
    f.on = true; memcpy(f.name, s.name, sizeof(f.name)); f.id = s.id; f.vw = s.vw; f.vh = s.vh; f.skippable = s.skippable; f.movie_id = s.movie_id;
    f.rt = s.rt; f.skip_ok = s.skip_ok; f.mode0 = mode; f.dur_ms = s.dur_ms; f.msf = s.msf; f.t0 = now; f.u_start = s.count;
}
inline void fmv_update(Fmv& f, const Snap& s, double now, int mode, bool cam3d) {
    f.presents++; f.by_mode[mode & 3]++;
    if (cam3d) f.cam3d++;
    if (s.paused) f.paused++;
    if (s.prompt) f.prompt++;
    if (s.p_ok) {
        const uint32_t u = s.count;
        if (!f.have_u) { if (u >= 1) { f.have_u = true; f.u_first = f.u_last = u; f.t_first = f.t_last = now; } }
        else if (u > f.u_last) {
            const uint32_t j = u - f.u_last;
            if (j > f.u_jump_max) f.u_jump_max = j;
            f.u_missed += j - 1;
            if (now - f.t_last > f.gap_max) f.gap_max = now - f.t_last;
            f.u_changes++; f.u_last = u; f.t_last = now;
        }
        if (s.dur_ms != 0) f.dur_ms = s.dur_ms;
    }
    if (s.s_ok && s.frame < k_frame_end) {
        if (!f.have_f) { f.have_f = true; f.f_first = f.f_last = s.frame; }
        else if (s.frame > f.f_last) { const uint32_t j = s.frame - f.f_last; if (j > f.f_jump_max) f.f_jump_max = j; f.f_last = s.frame; }
        else if (s.frame < f.f_last) f.f_back++;
        if (s.p_ok) {
            const int32_t d = static_cast<int32_t>(s.frame) - static_cast<int32_t>(s.count);
            if (!f.have_fu) { f.have_fu = true; f.fu_min = f.fu_max = d; }
            else { if (d < f.fu_min) f.fu_min = d; if (d > f.fu_max) f.fu_max = d; }
        }
    }
}
/// One Present. Returns EV_START / EV_END bits; at an end the finished FMV is copied to `done` first (both bits: one video
/// replaced another). The playing byte decides (a lingering skippable flag alone starts nothing).
inline int step(Fmv& cur, const Snap& s, double now, int mode, bool cam3d, Fmv& done) {
    int ev = 0;
    const bool active = s.mgr && s.playing != 0;
    if (cur.on) {
        int why = E_NONE;
        if (!s.mgr) why = E_GONE;
        else if (!s.playing) why = s.skipped ? E_SKIPPED : (s.s_ok && s.frame == k_frame_end) ? E_ENDED : E_STOPPED;
        else if ((s.s_ok && s.id != cur.id) || (s.n_ok && strcmp(s.name, cur.name) != 0) || (s.p_ok && cur.have_u && s.count < cur.u_last)) why = E_REPLACED;
        if (why != E_NONE) {
            cur.end = why; cur.t_end = now; cur.f_end = s.s_ok ? s.frame : 0; cur.skipped_end = s.skipped; cur.on = false;
            done = cur; ev |= EV_END;
        }
    }
    if (!cur.on && active && s.s_ok) { fmv_start(cur, s, now, mode); ev |= EV_START; }
    if (cur.on) fmv_update(cur, s, now, mode, cam3d);
    return ev;
}

// -- the log's limit ---------------------------------------------------------------------------------------------------
/// At most per_session lines (a start keeps room for its end); a start of the same video within 1 s of the last line is
/// not logged (a flapping byte), nor its end. Suppressed starts are counted and named on the next logged start.
struct Log {
    uint32_t per_session = 240, lines = 0, suppressed = 0, since = 0, reported = 0;   // suppressed: this session; since: since the last logged start
    double last_t = -1e300; uint32_t last_id = 0xFFFFFFFFu; char last_name[k_name_len + 1] = {};
    bool cur_logged = false, cap_said = false;
    /// true: log this start (reported = the starts not logged before it).
    bool want_start(const char* name, uint32_t id, double now) {
        const bool same = id == last_id && strcmp(name, last_name) == 0;
        if (lines + 2 > per_session || (same && now - last_t < 1000.0)) { cur_logged = false; suppressed++; since++; return false; }
        lines++; last_t = now; last_id = id; memcpy(last_name, name, k_name_len); last_name[k_name_len] = 0; cur_logged = true;
        reported = since; since = 0;
        return true;
    }
    bool want_end(double now) { if (!cur_logged) return false; cur_logged = false; lines++; last_t = now; return true; }
    bool cap_line() { if (cap_said || lines + 2 <= per_session) return false; cap_said = true; return true; }   // once, when full
};

// -- the lines -----------------------------------------------------------------------------------------------------------
inline size_t add(char* b, size_t cap, size_t len, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 4, 5)))
#endif
    ;
inline size_t add(char* b, size_t cap, size_t len, const char* fmt, ...) {
    if (cap == 0 || len + 1 >= cap) return len;
    va_list ap; va_start(ap, fmt);
    const int k = vsnprintf(b + len, cap - len, fmt, ap);
    va_end(ap);
    if (k <= 0) return len;
    return len + static_cast<size_t>(k) < cap ? len + static_cast<size_t>(k) : cap - 1;
}
inline const char* model_name(uint8_t m) { return m == 0 ? "fake model: a pipeline test, not depth" : m == 1 ? "VDA-Small" : "model ?"; }
inline size_t add_fvd(char* b, size_t cap, size_t len, const FvdInfo& v) {
    if (v.file[0] == 0) return add(b, cap, len, "%s", fvd_text(v.status));
    len = add(b, cap, len, "%s (fmvdepth\\%s", fvd_text(v.status), v.file);
    if (v.status == FVD_OK || (v.status >= FVD_HEADER && v.status <= FVD_THUMBS))
        len = add(b, cap, len, ": %u frames %ux%u, %s%s", v.frames, v.w, v.h, model_name(v.model), v.name_match ? "" : ", its source name differs");
    return add(b, cap, len, ")");
}
/// fmv: start ... (one line).
inline void start_line(char* b, size_t cap, const Fmv& f, const FvdInfo& v, uint32_t suppressed) {
    size_t n = add(b, cap, 0, "fmv: start '%s' id %u (event movie id %d, skippable flag %u, skip allowed %u, %s upload), view %s | video %ux%u, %.3f ms a frame (%.3f fps), duration %llu ms (%u frames at 29.97) | count at start %u | depth file: ",
                   f.name, f.id, f.movie_id, f.skippable, f.skip_ok, f.rt ? "render-thread" : "inline", mode_name(f.mode0), f.vw, f.vh, f.msf, f.msf > 0 ? 1000.0 / f.msf : 0.0,
                   static_cast<unsigned long long>(f.dur_ms), frames_from_ms(f.dur_ms), f.u_start);
    n = add_fvd(b, cap, n, v);
    if (suppressed) add(b, cap, n, " | %u fmv starts not logged before this one (the limit)", suppressed);
}
/// fmv: depth file ... (the worker's result for a logged start, one line).
inline void fvd_line(char* b, size_t cap, const char* name, uint32_t id, const FvdInfo& v, double ms) {
    size_t n = add(b, cap, 0, "fmv: depth file for '%s' id %u: ", name, id);
    n = add_fvd(b, cap, n, v);
    add(b, cap, n, " (checked off the render thread in %.0f ms)", ms);
}
/// fmv: end ... (one line): the counts, the timing, the comparison with the file, the views.
inline void end_line(char* b, size_t cap, const Fmv& f, const FvdInfo& v) {
    size_t n = add(b, cap, 0, "fmv: end '%s' id %u: %s | %.2f s, %u Presents (", f.name, f.id, end_text(f.end), (f.t_end - f.t0) / 1000.0, f.presents);
    bool first = true;
    for (int m = 0; m < 4; ++m) if (f.by_mode[m]) { n = add(b, cap, n, "%s%s %u", first ? "" : ", ", mode_name(m), f.by_mode[m]); first = false; }
    n = add(b, cap, n, "), a 3D camera on %u, paused on %u, skip prompt on %u", f.cam3d, f.paused, f.prompt);
    if (!f.have_u) n = add(b, cap, n, " | no frame uploaded");
    else {
        const double span = f.t_last - f.t_first, adv = static_cast<double>(f.u_last - f.u_first);
        n = add(b, cap, n, " | uploaded %u..%u (%u changes seen, largest step %u, %u frames never presented), first upload after %.0f ms, longest gap %.0f ms | %.2f s from the first to the last upload vs %.2f s at 29.97 (%.3f uploads/s)",
                f.u_first, f.u_last, f.u_changes, f.u_jump_max, f.u_missed, f.t_first - f.t0, f.gap_max, span / 1000.0, ms_from_frames(adv) / 1000.0, span > 0 ? adv * 1000.0 / span : 0.0);
    }
    if (f.have_f) n = add(b, cap, n, " | frame number %u..%u (largest step %u, back %u), number - count %d..%d, at the end %s%u",
                          f.f_first, f.f_last, f.f_jump_max, f.f_back, f.have_fu ? f.fu_min : 0, f.have_fu ? f.fu_max : 0, f.f_end == k_frame_end ? "0xFFFE = " : "", f.f_end);
    const uint32_t gf = frames_from_ms(f.dur_ms);
    n = add(b, cap, n, " | expected: the game's duration %llu ms = %u frames", static_cast<unsigned long long>(f.dur_ms), gf);
    if (v.status == FVD_OK) {
        n = add(b, cap, n, ", the file %u frames = %.2f s", v.frames, ms_from_frames(v.frames) / 1000.0);
        if (f.have_u) {
            const int64_t d = static_cast<int64_t>(f.u_last) - static_cast<int64_t>(v.frames);
            if (d == 0) n = add(b, cap, n, " | final count EQUALS the file's frame count (%u)", v.frames);
            else n = add(b, cap, n, " | final count %u vs the file's %u: %s by %lld", f.u_last, v.frames, d < 0 ? "SHORT" : "OVER", static_cast<long long>(d < 0 ? -d : d));
        }
    }
    else {
        n = add(b, cap, n, " | depth file: ");
        n = add_fvd(b, cap, n, v);
        if (f.have_u) n = add(b, cap, n, " | final count %u vs the game's %u frames (%+lld)", f.u_last, gf, static_cast<long long>(static_cast<int64_t>(f.u_last) - gf));
    }
}

}  // namespace fmv
