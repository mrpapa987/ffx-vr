// SPDX-License-Identifier: MIT
// ffxvr_gates.h -- direct game-state gates and the overdrive minigame state (gates, 4 Nov 2026). Pure logic: no Windows, no
// D3D, no logging, no allocation. The native side (ffxvr_native.cpp) maps managed's game bits (ffxvr_hands2_tap_set's
// VrTapCfg.game, hands.cs hands2_tap_game) to the tap gate's reason and the hands' game hide rule, and feeds the overdrive
// tracker one read per Present in battle (slow_frame). A unit test drives all of it with synthetic values.
//
// Addresses (our exe, VA with the image at 0x400000; the code reads them as module offsets = VA - 0x400000). Facts from
// Debug Revival, FFX_TAS_Python and the FFX screen reader, each re-checked in our own disassembly (design-notes.md "Direct
// game-state gates and overdrive minigame state"); no code from any of them.
//   player actor     dword 0x1300788 (Sg_GetPlayer 0x82D860; 0 = the player has no control)
//   field menu       dword 0x1340824 (1 at 0x8A9D8A when the TkMenu loop opens, cleared at 0x8A9FCB)
//   window shown     byte  0xC5A03C (0x8B6B70; its image value is 0xFF, so it counts only with a window shown, below)
//   message windows  [0x18676F0 + i*4], i < 8: +0x00 u16 1 = open (0x8B8560), +0x2C u16 state (2 = shown, 3 = closing)
//   choice cursors   [0x1868A90 + i*4]: +0x01 1 = a choice cursor up (0x8B6004, only at window state 2; 2 = confirmed),
//                    +0x02 s8 the highlighted row
//   movie            dword 0x112A008 1 = a skippable movie was started (0x76EB62 / 0x76EBC0; it can linger after the movie:
//                    review B1), so only with the movie player playing: byte [[0xCDED3C] + 0x6D0] (1 at 0x6D9159, 0 at
//                    0x6D6A89 / 0x6D7884 / 0x6D9559), as the game's own readers pair them (0x6D947D, 0x6DA4B3, 0x6DAC06);
//                    dword 0xC40E28 the movie id (-1 none, F8 only)
//   overdrives       s16 0x133D734 Tidus, 0x133D6F4 Auron, 0x133D708 Lulu, u8 0x133C93F Wakka (the HUD's own test 0x89B38D);
//                    the shared timer dword 0x133F77C (0 off, 1 armed, 2 running) and f32 0x133F790 (seconds left);
//                    Tidus: cursor s16 0x133D742, hit zone s16 0x133D73C .. 0x133D73E (inclusive, 0x8923CD..0x8923E8)
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>

// The tap gate's reasons (TG_OPEN = a push may press). Appended values keep the old numbers (ffxvr_hands2_tap_status [4]).
enum TapGate { TG_OPEN = 0, TG_OFF, TG_VIEW, TG_NO_TOUCH, TG_FOCUS, TG_PAD, TG_CHORD, TG_STALE, TG_BATTLE, TG_SPHERE, TG_SAVE, TG_TEXT, TG_BLITZ, TG_READ, TG_HIDDEN, TG_RIDING,
               TG_MOVIE, TG_MENU, TG_CHOICE, TG_NO_CONTROL, TG_N };
static const char* const k_tap_gate[TG_N] = { "open", "off", "not first person", "no touch sensors", "headset not focused", "VR pad off", "a chord or grab",
    "no game state", "battle", "sphere grid", "save / load screen", "a text box", "blitzball", "game state not readable", "hands not drawn", "riding",
    "a movie", "a field menu", "a choice box", "no player control" };

namespace gates {

// managed's game bits (hands.cs hands2_tap_game / hands2_view_bits)
enum : int {
    GB_BATTLE = 1, GB_SPHERE = 2, GB_SAVE = 4, GB_TEXT = 8, GB_BLITZ = 16, GB_RIDING = 32,
    GB_NO_CONTROL = 0x40,    // [0x1300788] == 0
    GB_MENU = 0x80,          // [0x1340824] != 0
    GB_UNREADABLE = 0x100, GB_NONE_READ = 0x200, GB_VIEW_ONLY = 0x400,   // a read failed / nothing read / only the view bits read
    GB_CHOICE = 0x800,       // a choice cursor is up on a shown window
    GB_TEXT_WAIT = 0x1000,   // a render-side message window open and shown (state 2: the typewriter done), with [0xC5A03C] != 0
    GB_MOVIE = 0x2000,       // [0x112A008] != 0 and the movie player playing ([[0xCDED3C] + 0x6D0] != 0)
};

/// The game part of the tap gate (after the view, sensors, focus, pad, chord and staleness checks). First match wins; every
/// shut reason shuts (the older gates and the direct ones are ORed). A choice box always shuts: a confirm there takes the
/// highlighted answer. No player control shuts too. A text box shuts unless "also advance dialogue" is on and the line is
/// fully shown: the Atel window bank has a window (bit 8) and a render-side window is in state 2, the typewriter done (0x1000;
/// the byte 0xC5A03C adds little: left at 0xFF it reduces to "some window is in state 2", review N2), with or without control
/// (review SF1); that is also the one way past "no control".
inline int tap_game_gate(int gm, bool dialogue) {
    if (gm & (GB_UNREADABLE | GB_NONE_READ | GB_VIEW_ONLY)) return TG_READ;
    if (gm & GB_BATTLE) return TG_BATTLE;
    if (gm & GB_SPHERE) return TG_SPHERE;
    if (gm & GB_SAVE) return TG_SAVE;
    if (gm & GB_BLITZ) return TG_BLITZ;
    if (gm & GB_RIDING) return TG_RIDING;
    if (gm & GB_MOVIE) return TG_MOVIE;
    if (gm & GB_MENU) return TG_MENU;
    if (gm & GB_CHOICE) return TG_CHOICE;
    const bool text = (gm & (GB_TEXT | GB_TEXT_WAIT)) != 0;
    const bool waits = (gm & GB_TEXT) != 0 && (gm & GB_TEXT_WAIT) != 0;
    if ((gm & GB_NO_CONTROL) && !(dialogue && waits)) return text ? TG_TEXT : TG_NO_CONTROL;
    if (text && !(dialogue && waits)) return TG_TEXT;   // SF1: a line still typing shuts with control too
    return TG_OPEN;
}

/// The hands' game hide rule (hands_hide_why, with a fresh gate): 1 battle (or nothing read: hidden as in battle, as before),
/// 2 a field menu, 0 none. The owner's rule: never in battle, menus or FMVs. A movie is not a reason here (review N6): a
/// frame that is the movie itself is a screen frame (hidden earlier) or has no 3D camera (no tick / no projection), and
/// the movie-camera scenes that draw 3D over a playing movie (0x76EFC0: ids 0x20, 0x2E, 0x46, 0x48) keep the hands.
inline int hands_game_hide(int gm) {
    if (gm & (GB_BATTLE | GB_NONE_READ)) return 1;
    if (gm & GB_MENU) return 2;
    return 0;
}

}  // namespace gates

namespace od {

// The block slow_frame reads: 0x133D6F2 .. 0x133D745 (0x54 bytes), then the Wakka byte, then (only while a minigame is up)
// the timer block 0x133F77C .. 0x133F793 (0x18 bytes). Offsets into the first block:
constexpr uint32_t k_blk_va = 0x133D6F2, k_blk_len = 0x54;
constexpr int k_res_auron = 0x00, k_res_tidus = 0x01, k_auron = 0x02, k_lulu = 0x16, k_tidus = 0x42, k_zone_lo = 0x4A, k_zone_hi = 0x4C, k_cursor = 0x50;
constexpr uint32_t k_wakka_va = 0x133C93F, k_timer_va = 0x133F77C, k_timer_len = 0x18;
constexpr int k_timer_left = 0x14;   // f32 0x133F790
constexpr int k_timer_count = 0x0C;  // f32 0x133F788: the countdown's frame count (+1 a logic frame while running; left = total - count / 30)

struct Raw {
    bool    ok = false;        // the state block was read
    int16_t auron = 0, lulu = 0, tidus = 0; uint8_t wakka = 0;
    uint8_t res_tidus = 0, res_auron = 0;
    int16_t cursor = 0, zone_lo = 0, zone_hi = 0;
    bool    timer_ok = false; int32_t timer = 0; float left = 0.f; float count = 0.f;
};
inline int16_t s16_at(const uint8_t* b, int o) { return static_cast<int16_t>(static_cast<uint16_t>(b[o]) | static_cast<uint16_t>(b[o + 1]) << 8); }
inline int32_t s32_at(const uint8_t* b, int o) { return static_cast<int32_t>(static_cast<uint32_t>(b[o]) | static_cast<uint32_t>(b[o + 1]) << 8 | static_cast<uint32_t>(b[o + 2]) << 16 | static_cast<uint32_t>(b[o + 3]) << 24); }
/// Fills r from the state block (k_blk_len bytes) and the Wakka byte.
inline void parse_state(Raw& r, const uint8_t* blk, uint8_t wakka) {
    r.ok = true;
    r.res_auron = blk[k_res_auron]; r.res_tidus = blk[k_res_tidus];
    r.auron = s16_at(blk, k_auron); r.lulu = s16_at(blk, k_lulu); r.tidus = s16_at(blk, k_tidus); r.wakka = wakka;
    r.zone_lo = s16_at(blk, k_zone_lo); r.zone_hi = s16_at(blk, k_zone_hi); r.cursor = s16_at(blk, k_cursor);
}
inline void parse_timer(Raw& r, const uint8_t* t) {
    r.timer_ok = true; r.timer = s32_at(t, 0);
    uint32_t u = static_cast<uint32_t>(s32_at(t, k_timer_left)); float f; static_assert(sizeof(f) == 4, "f32"); std::memcpy(&f, &u, 4); r.left = f;
    u = static_cast<uint32_t>(s32_at(t, k_timer_count)); std::memcpy(&f, &u, 4); r.count = f;
}
/// The game's own "an overdrive minigame is up" test (the battle HUD, 0x89B38D..0x89B3B2).
inline bool active(const Raw& r) { return r.ok && (r.auron != 0 || r.lulu != 0 || r.tidus != 0 || r.wakka != 0); }
/// Which one: 'T' Tidus, 'A' Auron, 'L' Lulu, 'W' Wakka, 0 none (the first non-zero, in that order).
inline char who(const Raw& r) { return !r.ok ? 0 : r.tidus ? 'T' : r.auron ? 'A' : r.lulu ? 'L' : r.wakka ? 'W' : 0; }
inline int who_state(const Raw& r) { const char w = who(r); return w == 'T' ? r.tidus : w == 'A' ? r.auron : w == 'L' ? r.lulu : w == 'W' ? r.wakka : 0; }
/// Tidus's Swordplay machine (handler 0x892320, table 0x89255C).
inline const char* tidus_state(int s) {
    switch (s) {
        case 0: return "none"; case 1: return "loading"; case 2: return "waiting for the asset"; case 3: return "arming the timer";
        case 4: return "gauge running"; case 5: return "hit"; case 6: return "hit, unloading"; case 7: return "timed out";
        case 8: return "timed out, unloading"; case 9: return "done"; case 10: return "result to the effect"; default: return "?";
    }
}
inline const char* timer_state(int t) { return t == 0 ? "off" : t == 1 ? "armed" : t == 2 ? "running" : "?"; }
inline bool in_zone(const Raw& r) { return r.cursor >= r.zone_lo && r.cursor <= r.zone_hi; }
/// The ring's byte: who in the top 3 bits (1 T, 2 A, 3 L, 4 W), the state (clamped to 31) in the low 5; 0 = none.
inline uint8_t ring_code(const Raw& r) {
    const char w = who(r); if (!w) return 0;
    const int id = w == 'T' ? 1 : w == 'A' ? 2 : w == 'L' ? 3 : 4;
    int s = who_state(r); if (s < 0) s = 31; if (s > 31) s = 31;
    return static_cast<uint8_t>(id << 5 | s);
}
inline char ring_who(uint8_t c) { static const char k[8] = { 0, 'T', 'A', 'L', 'W', '?', '?', '?' }; return k[c >> 5]; }
/// A short text for the slow: onset line and the F8 readout ("none" or "Tidus 4 (gauge running), cursor 120 in 100..140,
/// timer 2 (running), 1.25 s left").
inline int brief(char* s, size_t n, const Raw& r, long bst) {
    if (!r.ok) return snprintf(s, n, bst > 0 ? "not readable" : "not read (no battle)");
    const char w = who(r);
    if (!w) return snprintf(s, n, "none");
    const char* nm = w == 'T' ? "Tidus" : w == 'A' ? "Auron" : w == 'L' ? "Lulu" : "Wakka";
    int k = snprintf(s, n, "%s %d%s%s%s", nm, who_state(r), w == 'T' ? " (" : "", w == 'T' ? tidus_state(r.tidus) : "", w == 'T' ? ")" : "");
    if (k < 0 || static_cast<size_t>(k) >= n) return k;
    if (w == 'T') { const int j = snprintf(s + k, n - k, ", cursor %d %s %d..%d", r.cursor, in_zone(r) ? "in" : "outside", r.zone_lo, r.zone_hi); if (j > 0) k += j; }
    if (k < 0 || static_cast<size_t>(k) >= n) return k;
    if (r.timer_ok) { const int j = snprintf(s + k, n - k, ", timer %d (%s), %.2f s left", r.timer, timer_state(r.timer), r.left); if (j > 0) k += j; }
    return k;
}

/// The od: lines: one per change of (Tidus, Auron, Lulu, Wakka, the timer's state), at most per_window in window_ms and
/// per_session a session; the changes not logged are counted and named in the next line. The cursor and the time left move
/// every frame and are not changes (they go into the line).
struct Log {
    int    per_window = 24; double window_ms = 5000.0; uint32_t per_session = 400;
    bool   have = false; int16_t a = 0, l = 0, t = 0; uint8_t w = 0; int32_t tm = 0;
    double win_t0 = -1e300; int win_n = 0; uint32_t lines = 0, dropped = 0, changes = 0;
    enum { NOTHING = 0, LOG = 1, DROPPED = 2 };
    /// One read (a battle frame). LOG: write the line now (dropped_before = changes not logged since the last line).
    int step(const Raw& r, double now, uint32_t& dropped_before) {
        dropped_before = 0;
        if (!r.ok) return NOTHING;
        const int32_t tmn = r.timer_ok ? r.timer : 0;
        const bool changed = !have ? (active(r) || tmn != 0) : (r.auron != a || r.lulu != l || r.tidus != t || r.wakka != w || tmn != tm);
        have = true; a = r.auron; l = r.lulu; t = r.tidus; w = r.wakka; tm = tmn;
        if (!changed) return NOTHING;
        changes++;
        if (now - win_t0 >= window_ms) { win_t0 = now; win_n = 0; }
        if (win_n >= per_window || lines >= per_session) { dropped++; return DROPPED; }
        win_n++; lines++; dropped_before = dropped; dropped = 0;
        return LOG;
    }
    /// The battle ended (or the reads stopped): true when the last state seen was a minigame (one "over" line).
    bool reset() {
        const bool was = have && (a != 0 || l != 0 || t != 0 || w != 0 || tm != 0);
        have = false; a = l = t = 0; w = 0; tm = 0;
        return was;
    }
};

}  // namespace od
