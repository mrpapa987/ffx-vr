// SPDX-License-Identifier: MIT
// ffxvr_battlevis.h -- the battle research log (cmd: / vis: / fx: lines) and the first-person battle body hide's identity rule
// (branch battle-vis, ABI 68; design-notes "Battle visibility, build 1"). Pure logic: no Windows, no D3D, no file. The native
// glue (ffxvr_native.cpp, "Battle research log" and bh_draw in ffxvr_on_draw) feeds it what managed read from the game
// (battle_vis.cs) and hands every line to the session log; a unit test drives it with synthetic battles.
//
// Why: the owner's reports (Nov 2026). Aeons are often hidden during the summoning animation; an ability with a "charging"
// animation (Armor Break) can hide other characters; Holy "renders very very poorly in VR, like it's squished vertically". This
// build only records what the game does (who acts, with what; every battle actor's visibility votes; the effect's matrices next
// to the eyes'), and changes one thing: in a first-person battle the followed character's body is hidden by the identity of
// its draws (its model's index-buffer keys, as the field path does), no longer by distance alone.
//
// Game facts used (addresses are absolute, image base 0x400000; ffx-rust's symbols/ffx.sym and docs/formats, static research of
// the same FFX.exe; Fahrenheit's names where it has them; no code taken). UNCONFIRMED = not seen in the running game by us:
//   battle actor record (MsGetChr 0x794020, 0xF90 bytes, 0..0x1E at [0x11334D4]):
//     +0x00 the character (the field actor table's entry), +0x15 visible, +0x16..+0x19 four visibility requests ("votes"):
//     msSetChrVisibleReq(i, k, v) (0x797080) writes vote k and the actor is visible only while all four are set.
//       k 0 (+0x16) MsSetChrVisibleMain (0x796860; Fahrenheit: stat_visible, script property STAT_VISIBLE)
//       k 1 (+0x17) MsSetChrVisibleAppear (0x7966A0; Fahrenheit: stat_hide)
//       k 2 (+0x18) MsSetChrVisible (0x796660; Fahrenheit: stat_visible_eff; the effect scripts' opu_en_on and the magic files)
//       k 3 (+0x19) Fahrenheit: stat_visible_cam (STAT_VISIBLE_CAM); its setter is taken to be MsSetChrVisibleCamera
//                   (0x7967A0, a BSim name match: UNCONFIRMED)
//     +0x6DF the count of commands the actor has executed (MsParseCommand adds 1, up to 255), +0xF5C the command id it
//     parsed last, +0xDC0 that command's target bits (one per actor).
//   character (field actor) +0x180 hide byte: 0x01 event, 0x02 effect (Ch_SetHideEffect 0x82B2A0: opu_en_on's nibbles 2 / 3,
//     and 0 / 1 / 4 / 5 for a character that is no battle actor, set it without the battle setter), 0x10 battle
//     (Ch_SetHideBattle 0x82B260; the votes' own hide goes through these two); 0x08 / 0x20 the frustum / distance culls
//     (our exe-analysis.md). +0x354 the alpha (0 = not drawn), +0x198 bit 0x20 the model released.
//   MsParseCommand(cue) (0x7AE370, cdecl, one argument): cue +0 the actor, +2 the group in play, +3 the group count, groups
//     of 0x10 bytes from +8: two u16 command ids (the second replaces the first unless its record says otherwise; for a swap
//     or a summon it is the argument) and at +8 the target bits. Kind 0 commands act by their id: 0x3002 a swap, 0x3003 /
//     0x3018 escapes, 0x3004 / 0x3005 weapon / armour, 0x3117 / 0x3118 / 0x30FF an aeon's calling (ffx-rust's port of the
//     function: UNCONFIRMED in play).
//   command ids: the top nibble names the kernel table (2 items, 3 player commands, 4 / 6 monster commands); record +0x15
//     the animation kind MsParseCommand switches on (1 / 0x1E: the actor moves in and hits).
//
// Build 2 (branch battle-vis2, ABI 69; design-notes "Battle visibility, build 2"): the battle camera's hide is skipped while
// the mod draws a battle in VR ("the battle camera's hide" below: the rule, the events' ring, the Log's lines), and the Log
// reports what fx_face_eyes did per magic file (FxFace; the matrix rule itself is ffxvr_fxface.h).
//   msSetChrVisibleReq(i, k, v) (0x797080, cdecl, three arguments, per ffx-rust's port: UNCONFIRMED by us): k 3 is the
//     camera's request; its callers are MsSetChrVisibleCamera (0x7967A0: sets g_BtlCameraHides 0x112C9FE, then the request;
//     called for the scripts' property 0xD8 STAT_VISIBLE_CAM), MsSetChrVisibleCameraAll (0x7967C0) and, through it,
//     MsReturnCameraVisible (0x795C70: when g_BtlCameraHides is set, clears it and shows all 31 again).
//   0x11363B8 & 0x1136B58 (g_ActorPropertyLatch & g_BtlHideMask): while they meet, the game queues a hide in 0x2311060
//     instead of hiding (one battle scene, 0xD40000); a camera hide is then left to the game.
#pragma once
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace bvis {

constexpr int k_actors = 31;      // battle actor records 0..0x1E
constexpr int k_name   = 32;      // an actor's name in a line (ASCII, NUL-terminated)
constexpr int k_cname  = 40;      // a command's name

// ---- text ----------------------------------------------------------------------------------------------------------------
/// A bounded line builder: add() appends printf-style and never writes past cap; a line that does not fit ends there.
struct Buf {
    char* p; int cap; int n;
    Buf(char* p_, int cap_) : p(p_), cap(cap_), n(0) { if (cap > 0) p[0] = 0; }
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))   // (the scratch tests' compiler checks every format string against its arguments)
#endif
    void add(const char* fmt, ...) {
        if (cap <= 0 || n >= cap - 1) return;
        va_list ap; va_start(ap, fmt);
        const int w = vsnprintf(p + n, static_cast<size_t>(cap - n), fmt, ap);
        va_end(ap);
        if (w > 0) n = (w < cap - n) ? n + w : cap - 1;
    }
    bool full() const { return n >= cap - 1; }
};
/// Copies a name for a log line: printable ASCII only (anything else '?'), no quote characters, always terminated.
inline void copy_name(char* dst, int cap, const char* src) {
    int k = 0;
    for (; src != nullptr && k < cap - 1 && src[k] != 0; ++k) {
        const unsigned char c = static_cast<unsigned char>(src[k]);
        dst[k] = (c < 32 || c > 126 || c == '\'' || c == '|') ? '?' : static_cast<char>(c);
    }
    if (cap > 0) dst[k] = 0;
}

// ---- what managed sends per frame ----------------------------------------------------------------------------------------
enum : uint8_t { F_ALPHA0 = 1, F_RELEASED = 2, F_NOCHAR = 4, F_DRAWFX = 8 };
/// One battle actor record as managed read it (battle_vis.cs writes these bytes by offset: keep the layout).
struct ActorIn {
    uint8_t  live;        //  0: the record has a character (+0x00 not null)
    uint8_t  vis[5];      //  1: +0x15 visible, then +0x16..+0x19 the four votes
    uint8_t  hide;        //  6: the character's +0x180, raw
    uint8_t  flags;       //  7: F_ALPHA0 its alpha word is 0, F_RELEASED +0x198 bit 0x20, F_NOCHAR the character is not in the actor table,
                          //     F_DRAWFX +0x194 bit 13 (Ch_DrawEnEffect 0x82EB60, opu_en_on's nibbles 6 / 7; what it does to the draw is unconfirmed)
    uint8_t  exe_count;   //  8: +0x6DF
    uint8_t  slot;        //  9: the character's actor-table slot (0xFF: none)
    uint16_t cmd;         // 10: +0xF5C
    uint32_t targets;     // 12: +0xDC0
    char     name[k_name];// 16
};
static_assert(sizeof(ActorIn) == 48, "ActorIn is 48 bytes (battle_vis.cs k_bv_rec)");
static_assert(offsetof(ActorIn, cmd) == 10 && offsetof(ActorIn, targets) == 12 && offsetof(ActorIn, name) == 16, "ActorIn offsets (battle_vis.cs)");

/// The globals managed reads per frame (int32 each, in this order: battle_vis.cs k_bvg_*).
enum { G_STATE = 0,        // g_BtlStatus (0x112A8E0, i8): not 0 in battle
       G_MAGIC,            // g_MagicFile_MagicId (0xC64CA0): -1 none
       G_MAGIC_HANDLE,     // g_MagicFile_Handle (0xC64CAC): -1 not loaded
       G_MAGIC_HIDDEN,     // g_MagicHiddenBtlChr (0xC64CDC): -1 none
       G_CAM_HIDES,        // g_BtlCameraHides (0x112C9FE, u8)
       G_CMD_ACTOR,        // g_BtlCommandActor (0x112C9E0, u8): 0xFF none
       G_CUE_COUNT,        // g_BtlCommandCueCount (0x112BDE1, i8)
       G_ROSTER,           // the field roster's three bytes (0x112C895), packed b0 | b1 << 8 | b2 << 16
       G_FOLLOW,           // the chr id the eyes follow (-1 none)
       G_VIEW,             // the view: 0 seat, 1 first person, 2 diorama, 3 third person
       G_FLAGS,            // GF_*
       G_COUNT };
enum { GF_HOOK = 1,        // the MsParseCommand detour is in
       GF_HOOK_BUSY = 2,   // ... and is inside the game's function right now (its event is not in the ring yet)
       GF_MON_OK = 4,      // the monsters' pointer ([0x1134468]) is the party's + 0x14 records: one array
       GF_MON_BAD = 8,     // ... it is not: records 0x14.. were not read from where the game keeps them
       GF_SHORT = 16 };    // only the first 0x14 records read

// ---- the votes -----------------------------------------------------------------------------------------------------------
constexpr int k_votes = 4;
inline const char* vote_name(int k) {
    switch (k) { case 0: return "main"; case 1: return "appear"; case 2: return "effect"; case 3: return "camera"; default: return "?"; }
}
/// The legend, once per battle (what each vote is, and what is not confirmed).
inline const char* votes_legend() {
    return "votes (the actor is visible only while all four are 1): main = +0x16 MsSetChrVisibleMain (scripts: STAT_VISIBLE), "
           "appear = +0x17 MsSetChrVisibleAppear, effect = +0x18 MsSetChrVisible (effect scripts' opu_en_on, magic files), "
           "camera = +0x19 STAT_VISIBLE_CAM (the battle cameras; its setter's name is unconfirmed) | chr hide = the character's "
           "+0x180: event 0x01, effect 0x02 (effect scripts can set it without a vote), battle 0x10, "
           "frustum cull 0x08, distance cull 0x20";
}
/// v5 = the visible flag then the four votes. The votes that hide (bit k: vote k is 0).
inline unsigned hiders(const uint8_t* v5) {
    unsigned m = 0;
    for (int k = 0; k < k_votes; ++k) if (v5[1 + k] == 0) m |= 1u << k;
    return m;
}
/// The flag is what the votes say (all set = visible).
inline bool vis_consistent(const uint8_t* v5) { return (v5[0] != 0) == (hiders(v5) == 0); }
/// "visible 1 -> 0 | votes: main 1, appear 1, effect 1 -> 0 (HIDES), camera 1"; was5 null = no previous state.
inline void votes_text(Buf& b, const uint8_t* now5, const uint8_t* was5) {
    if (was5 != nullptr && was5[0] != now5[0]) b.add("visible %u -> %u", was5[0], now5[0]); else b.add("visible %u", now5[0]);
    b.add(" | votes:");
    for (int k = 0; k < k_votes; ++k) {
        const unsigned v = now5[1 + k];
        if (was5 != nullptr && was5[1 + k] != v) b.add("%s %s %u -> %u", k ? "," : "", vote_name(k), was5[1 + k], v);
        else b.add("%s %s %u", k ? "," : "", vote_name(k), v);
        if (v == 0) b.add(" (HIDES)");
    }
    if (!vis_consistent(now5)) b.add(" | the flag and the votes DISAGREE (a hide may be queued by the game: unconfirmed)");
}
constexpr uint8_t k_hide_stable = 0x13;   // the script bits; 0x08 / 0x20 are per-frame culls (they flap with the game's camera)
/// "0x12 (effect, battle)".
inline void hide_text(Buf& b, uint8_t h) {
    b.add("0x%02X", h);
    if (h == 0) return;
    static const struct { uint8_t bit; const char* name; } bits[] = {
        { 0x01, "event" }, { 0x02, "effect" }, { 0x10, "battle" }, { 0x08, "frustum cull" }, { 0x20, "distance cull" } };
    bool first = true; uint8_t known = 0;
    for (const auto& x : bits) { known |= x.bit; if (h & x.bit) { b.add("%s%s", first ? " (" : ", ", x.name); first = false; } }
    if (h & static_cast<uint8_t>(~known)) { b.add("%sother 0x%02X", first ? " (" : ", ", h & static_cast<uint8_t>(~known)); first = false; }
    if (!first) b.add(")");
}

/// What a change is detected on: the flag, the votes, the hide byte's script bits, the flags.
struct ActorState { uint8_t b[7]; };
inline ActorState state_of(const ActorIn& a) {
    ActorState s;
    for (int k = 0; k < 5; ++k) s.b[k] = a.vis[k];
    s.b[5] = a.hide & k_hide_stable; s.b[6] = a.flags & (F_ALPHA0 | F_RELEASED | F_NOCHAR | F_DRAWFX);
    return s;
}
inline bool same(const ActorState& x, const ActorState& y) { return memcmp(x.b, y.b, sizeof(x.b)) == 0; }
/// Shown by everything this log reads (the flag, the votes, the character's script bits, its alpha, its model; F_DRAWFX is
/// not counted: its effect on the draw is not known).
inline bool shown(const ActorState& s) { return s.b[0] != 0 && s.b[1] && s.b[2] && s.b[3] && s.b[4] && s.b[5] == 0 && (s.b[6] & (F_ALPHA0 | F_RELEASED | F_NOCHAR)) == 0; }

enum { VS_NONE = 0, VS_FIRST, VS_CHANGED, VS_GONE };   // (not V_*: oleauto.h has function-like macros of those names)
struct VisTrack {
    bool have[k_actors]; ActorState st[k_actors];
    VisTrack() { reset(); }
    void reset() { for (int i = 0; i < k_actors; ++i) { have[i] = false; memset(st[i].b, 0, sizeof(st[i].b)); } }
    /// One actor, once a frame. baseline = the battle's first frame (states are taken, nothing is a change). *was gets the
    /// state before (VS_CHANGED, VS_GONE).
    int step(int i, const ActorIn& a, bool baseline, ActorState* was) {
        if (i < 0 || i >= k_actors) return VS_NONE;
        if (!a.live) {
            if (!have[i]) return VS_NONE;
            have[i] = false; if (was) *was = st[i];
            return baseline ? VS_NONE : VS_GONE;
        }
        const ActorState s = state_of(a);
        if (!have[i]) { have[i] = true; st[i] = s; return baseline ? VS_NONE : VS_FIRST; }
        if (same(st[i], s)) return VS_NONE;
        if (was) *was = st[i];
        st[i] = s;
        return VS_CHANGED;
    }
};

// ---- build 2: the battle camera's hide ------------------------------------------------------------------------------------
// The owner's recording (16 Nov 2026): the game's battle camera hides one character per action through the camera vote
// (Armor Break its target for 0.9 s, Tidus's Attack hid Yuna for 1.2 s, an enemy's Blizzard another enemy for 2.3 s). In VR
// the eyes are not the game's camera, so the hide protects nothing and takes a character away for a second. battle_cam_hide 0
// (the default): while the mod draws a battle in first person or the diorama, a call msSetChrVisibleReq(i, 3, 0) is not
// passed on to the game (managed's detour, battle_vis2.cs). Nothing else is touched: every other vote, every show (v not 0),
// the game's own g_BtlCameraHides bookkeeping (set before the request, cleared by MsReturnCameraVisible, whose "show all"
// calls pass through as always) and the game's hide queue (a hide the game would queue is the game's).
constexpr uint32_t k_vote_camera = 3;   // msSetChrVisibleReq's k of the camera's request (+0x19)
enum { CHS_GAME = 0,     // battle_cam_hide 1: the game's own behaviour
       CHS_ON,           // battle_cam_hide 0, a VR battle in first person or the diorama, the detour is in: hides are skipped
       CHS_NOHOOK,       // ... the same, but the detour is not in (not inserted yet, or refused): nothing is skipped
       CHS_IDLE };       // battle_cam_hide 0, not in effect now (no battle, not in the headset, another view, or the headset shows the game's own camera on a screen)
/// The views in which the mod replaces the game's camera for a battle: 1 first person, 2 the diorama.
inline bool cam_hide_view(int view) { return view == 1 || view == 2; }
/// setting = ini battle_cam_hide; xr = the headset shows the mod's own camera this frame (the session is on and the frame is
/// not going to a screen: review B1, on a screen the player looks through the game's camera, whose hides then matter);
/// hook = managed's detour state (1 = inserted).
inline int cam_hide_state(int setting, bool battle, bool xr, int view, int hook) {
    if (setting != 0) return CHS_GAME;
    if (!battle || !xr || !cam_hide_view(view)) return CHS_IDLE;
    return hook == 1 ? CHS_ON : CHS_NOHOOK;
}
/// The detour's decision for one call msSetChrVisibleReq(i, k, v) (the game's thread). active = the state is CHS_ON;
/// queue_bits = [0x11363B8] & [0x1136B58] (not 0: the game would queue this hide, so it is the game's). Only a hide (the
/// byte the game stores is 0) of the camera's request is ever skipped: never another vote, never a show.
inline bool cam_hide_skip(bool active, uint32_t k, uint32_t v, uint32_t queue_bits) {
    return active && k == k_vote_camera && (v & 0xFFu) == 0u && queue_bits == 0u;
}
inline const char* cam_hide_text(int state) {
    switch (state) {
        case CHS_GAME:   return "1: the game's own behaviour (its battle camera hides characters)";
        case CHS_ON:     return "0: the mod skips the battle camera's hides while it draws this battle in VR (first person or the diorama); the detour on msSetChrVisibleReq is in";
        case CHS_NOHOOK: return "0, but the detour on msSetChrVisibleReq is NOT in (not inserted yet, or refused): the camera's hides happen as in the game";
        case CHS_IDLE:   return "0, not in effect now (not a battle drawn in VR in first person or the diorama, or the headset shows the game's own camera on a screen): the camera's hides happen as in the game";
        default:         return "?";
    }
}
/// The skipped hides, from the detour (the game's thread, the one writer) to the log (the render thread, the one reader):
/// the actor of each in the low byte, the game's effect-camera mode byte at that moment in the high one (review S3).
/// Full: the event is counted as lost, the hide is skipped all the same.
struct SkipRing {
    static constexpr uint32_t k_cap = 64;   // a power of two (the counters wrap)
    std::atomic<uint32_t> w{ 0 }, r{ 0 }, lost{ 0 };
    uint16_t e[k_cap] = {};
    void push(uint16_t actor) {
        const uint32_t ww = w.load(std::memory_order_relaxed);
        if (ww - r.load(std::memory_order_acquire) >= k_cap) { lost.fetch_add(1u, std::memory_order_relaxed); return; }
        e[ww % k_cap] = actor;
        w.store(ww + 1u, std::memory_order_release);
    }
    bool pop(uint16_t* actor) {
        const uint32_t rr = r.load(std::memory_order_relaxed);
        if (rr == w.load(std::memory_order_acquire)) return false;
        if (actor != nullptr) *actor = e[rr % k_cap];
        r.store(rr + 1u, std::memory_order_release);
        return true;
    }
};

// ---- the line budget -----------------------------------------------------------------------------------------------------
// Per battle and per session, shared fairly between the sources (the actors and three globals): every source has `guarantee`
// lines of its own per battle; past them it draws on a common pool (the battle's budget less every source's guarantee), of
// which one source may take at most `share_cap`. So one flapping actor ends with guarantee + share_cap lines and the others
// still have theirs. What is not written is counted per source and reported at the battle's end.
constexpr int k_src = k_actors + 3;
constexpr int k_src_magic_hidden = k_actors, k_src_cam_hides = k_actors + 1, k_src_roster = k_actors + 2;
struct Budget {
    int per_battle = 0, per_session = 0, guarantee = 0, share_cap = 0;
    int used_session = 0, used_battle = 0, pool_used = 0, skipped_battle = 0, skipped_session = 0;
    int used[k_src] = {}, skipped[k_src] = {};
    void config(int battle, int session, int guar, int share) { per_battle = battle; per_session = session; guarantee = guar; share_cap = share; }
    int pool() const { const int p = per_battle - k_src * guarantee; return p > 0 ? p : 0; }
    void battle_start() {
        used_battle = pool_used = skipped_battle = 0;
        for (int i = 0; i < k_src; ++i) { used[i] = 0; skipped[i] = 0; }
    }
    /// 0: not written (counted); 1: write it; 2: write it, and it is the session's last line of this kind (say so).
    int take(int src) {
        if (src < 0 || src >= k_src) return 0;
        bool ok = false, from_pool = false;
        if (used_session < per_session && used_battle < per_battle) {
            if (used[src] < guarantee) ok = true;
            else if (used[src] - guarantee < share_cap && pool_used < pool()) { ok = true; from_pool = true; }
        }
        if (!ok) { skipped[src]++; skipped_battle++; skipped_session++; return 0; }
        if (from_pool) pool_used++;
        used[src]++; used_battle++; used_session++;
        return used_session >= per_session ? 2 : 1;
    }
};

// ---- commands: the hook's lines and the polled fallback ------------------------------------------------------------------
/// One executed command, from the MsParseCommand detour (src 0) or asked for by the poll (src 1). battle_vis.cs writes these
/// bytes by offset: keep the layout.
struct CmdEv {
    uint8_t  src;          //   0: 0 the hook, 1 polled
    uint8_t  actor;        //   1
    uint8_t  exe_count;    //   2: the actor's +0x6DF after the parse
    uint8_t  sub;          //   3: the cue's group in play (0xFF: no cue entry found)
    uint8_t  groups;       //   4: the cue's group count
    uint8_t  kind;         //   5: the record's +0x15
    uint8_t  rec_ok;       //   6: 1 the record was read, 2 the game's own lookup (MsGetComData) gave it
    uint8_t  sub_menu;     //   7: the record's +0x18
    uint16_t id;           //   8: the command executed (the game's parsed-command word after the call)
    uint16_t list0;        //  10: the group's first id
    uint16_t list1;        //  12: ... and second (0xFF none; a swap's or a summon's argument)
    uint16_t target_flags; //  14: the record's u16 +0x1A
    uint32_t targets;      //  16: the target bits
    uint32_t misc;         //  20: the record's u32 +0x1C
    char     name[k_cname];    //  24: the executed command's name
    char     name0[k_cname];   //  64: the first id's name when it is another command (the menu it was chosen from)
};
static_assert(sizeof(CmdEv) == 104, "CmdEv is 104 bytes (battle_vis.cs k_bv_ev)");
static_assert(offsetof(CmdEv, id) == 8 && offsetof(CmdEv, targets) == 16 && offsetof(CmdEv, misc) == 20 && offsetof(CmdEv, name) == 24 && offsetof(CmdEv, name0) == 64, "CmdEv offsets (battle_vis.cs)");

/// The kind as far as the id and the record tell (ffx-rust's port of MsParseCommand; unconfirmed in play). The game's own
/// names (the command and the menu it came from) say the rest: Skill, Black Magic, Overdrive, Defend...
inline const char* cmd_kind(uint16_t id, int rec_kind) {
    switch (id) {
        case 0x3002: return "swap";
        case 0x3003: return "escape (one character)";
        case 0x3018: return "escape (the party flees)";
        case 0x3004: return "weapon change";
        case 0x3005: return "armour change";
        case 0x3117: case 0x3118: return "summon";
        case 0x30FF: return "summon (the Magus Sisters)";
        case 0x3129: case 0x3056: case 0x3057: return "Magus Sisters command";
        default: break;
    }
    switch (id >> 12) {
        case 2: return "item";
        case 3: return (rec_kind == 1 || rec_kind == 0x1E) ? "attack (the actor moves in and hits)" : "command (skill / spell / overdrive / other: see the names)";
        case 4: case 6: return "monster command";
        default: return "unknown";
    }
}
inline bool cmd_is_swap(uint16_t id) { return id == 0x3002; }
inline bool cmd_is_summon(uint16_t id) { return id == 0x3117 || id == 0x3118 || id == 0x30FF; }

/// "0x00100001: 0 'Tidus', 20 'Sand Wolf'" (at most 8 named).
inline void targets_text(Buf& b, uint32_t t, const char (*names)[k_name]) {
    b.add("0x%08X", t);
    if (t == 0) { b.add(" (none)"); return; }
    int shown_n = 0, total = 0;
    for (int i = 0; i < 32; ++i) {
        if (!((t >> i) & 1u)) continue;
        total++;
        if (shown_n >= 8) continue;
        b.add("%s %d", shown_n ? "," : ":", i);
        if (i < k_actors && names != nullptr && names[i][0]) b.add(" '%s'", names[i]);
        shown_n++;
    }
    if (total > shown_n) b.add(", and %d more", total - shown_n);
}
inline void actor_text(Buf& b, int i, const char (*names)[k_name]) {
    b.add("actor %d", i);
    if (i >= 0 && i < k_actors && names != nullptr && names[i][0]) b.add(" '%s'", names[i]);
}

struct PolledReq { uint8_t actor, count; uint16_t id; uint32_t targets; };
constexpr uint32_t k_poll_hold = 2;   // frames a polled change waits for the hook's event before it is reported as missed
/// The polled fallback reports only what the hook missed. Per actor: the last (exe count, command id) seen in the record, the
/// last one the hook reported, and a change waiting k_poll_hold frames for the hook's event (the detour pushes its event when
/// the game's function returns, which is after the function wrote the record; while the detour is inside it nothing expires).
struct Dedup {
    struct Key { uint8_t count; uint16_t id; bool set; };
    Key last[k_actors], hook[k_actors]; struct Pend { Key k; uint32_t frame, targets; } pend[k_actors];
    Dedup() { reset(); }
    void reset() { for (int i = 0; i < k_actors; ++i) { last[i] = Key{ 0, 0, false }; hook[i] = Key{ 0, 0, false }; pend[i].k = Key{ 0, 0, false }; pend[i].frame = 0; pend[i].targets = 0; } }
    static bool eq(const Key& a, const Key& b) { return a.set && b.set && a.count == b.count && a.id == b.id; }
    void on_hook(int actor, uint8_t count, uint16_t id) {
        if (actor < 0 || actor >= k_actors) return;
        hook[actor] = Key{ count, id, true };
        if (eq(pend[actor].k, hook[actor])) pend[actor].k.set = false;
    }
    /// One actor, once a frame. true: *out is a command the hook did not report (a polled line is due).
    bool poll(int actor, bool live, uint8_t count, uint16_t id, uint32_t targets, uint32_t frame, bool hook_busy, PolledReq* out) {
        if (actor < 0 || actor >= k_actors) return false;
        if (!live) { last[actor].set = false; pend[actor].k.set = false; hook[actor].set = false; return false; }
        const Key now{ count, id, true };
        if (!last[actor].set) last[actor] = now;   // the baseline: nothing to report
        else if (!eq(last[actor], now)) {
            last[actor] = now;
            // a record just set up or cleared (count 0, no command) is a new baseline, not a command
            if (count == 0 || id == 0 || id == 0xFFFF || eq(hook[actor], now)) pend[actor].k.set = false;
            else { pend[actor].k = now; pend[actor].frame = frame; pend[actor].targets = targets; }
        }
        if (!pend[actor].k.set || hook_busy || frame - pend[actor].frame < k_poll_hold) return false;
        const Pend p = pend[actor]; pend[actor].k.set = false;
        if (eq(hook[actor], p.k)) return false;
        if (out) { out->actor = static_cast<uint8_t>(actor); out->count = p.k.count; out->id = p.k.id; out->targets = p.targets; }
        return true;
    }
};

// ---- the first-person battle body hide: identity ---------------------------------------------------------------------------
// Before: in a first-person battle the followed character's body was hidden by distance alone (a skinned draw within the hide
// radius of the eye's anchor), which can take an aeon appearing on the summoner's spot or a neighbour running through it.
// Now (fp_battle_hide 1): a draw is his when its bound index buffer and byte offset are one of his model's segment keys
// (managed walks actor +0x830, as the field path does: body_hide.cs IbChain). The rules the review of an earlier version asked for:
//   - another live actor with the same keys (one model, two actors): the keys are kept, and a draw with a shared key is his
//     when his drawn root is the nearest of the sharing actors' roots (a tie goes to him);
//   - no keys for his model (not built yet, a read failed): the distance rule meanwhile, and managed retries every k_retry frames;
//   - the keys are trusted only once a draw has carried one (M_PROBE: the distance rule until then), so a battle model whose
//     draws do not use its chain's keys leaves the old rule in place instead of showing his body;
//   - nothing is pushed outside first person (the diorama, third person, the virtual screen): M_OFF, nothing judged.
constexpr int k_keys = 768, k_owners = 48;      // owner 0 the followed actor, 1.. the other live actors (one each)
constexpr uint32_t k_rebuild = 60, k_rebuild_partial = 30, k_retry = 4;   // frames between managed's rebuilds: all read / another actor's chain failed / his own gave no keys

struct KeyEntry { uint32_t ib, off, count; uint32_t owner; };
struct KeyTable {
    KeyEntry e[k_keys]; int n = 0, nf = 0;   // nf: the followed actor's entries, kept first
    bool any_shared = false; uint32_t hash = 0;
    void clear() { n = nf = 0; any_shared = false; hash = 0; }
    /// From (ib, off, count, owner) quadruples in any order. Entries with a null buffer or an owner out of range are dropped.
    int set(const uint32_t* q, int m) {
        clear();
        for (int pass = 0; pass < 2; ++pass) {
            for (int i = 0; q != nullptr && i < m && n < k_keys; ++i) {
                const uint32_t* r = q + 4 * i;
                if (r[0] == 0 || r[3] >= static_cast<uint32_t>(k_owners)) continue;
                if ((r[3] == 0) != (pass == 0)) continue;
                e[n].ib = r[0]; e[n].off = r[1]; e[n].count = r[2]; e[n].owner = r[3]; ++n;
            }
            if (pass == 0) nf = n;
        }
        uint32_t h = 2166136261u;
        for (int i = 0; i < nf; ++i) { h = (h ^ e[i].ib) * 16777619u; h = (h ^ e[i].off) * 16777619u; }
        hash = nf > 0 ? (h | 1u) : 0u;
        for (int i = 0; i < nf && !any_shared; ++i) for (int j = nf; j < n; ++j) if (e[j].ib == e[i].ib && e[j].off == e[i].off) { any_shared = true; break; }
        return n;
    }
    bool self(uint32_t ib, uint32_t off) const { for (int i = 0; i < nf; ++i) if (e[i].ib == ib && e[i].off == off) return true; return false; }
    /// The other owners of a key, one bit each (owner k = bit k).
    uint64_t others(uint32_t ib, uint32_t off) const {
        uint64_t m = 0;
        for (int i = nf; i < n; ++i) if (e[i].ib == ib && e[i].off == off) m |= 1ull << e[i].owner;
        return m;
    }
};

enum { M_OFF = 0,       // no push for this frame (not a first-person battle): nothing judged
       M_SETTING,       // fp_battle_hide 0: the distance rule, by choice
       M_NOKEYS,        // no keys for the followed model: the distance rule meanwhile
       M_PROBE,         // keys built, none seen on a draw yet: the distance rule meanwhile
       M_IDENT };       // the identity rule
enum { C_NONE = 0, C_SELF, C_OTHER, C_SHARED, C_UNREAD,
       C_HIS };         // another actor's key, and every such actor is attached to the followed one (his weapon, something he holds)
inline const char* mode_name(int m) {
    switch (m) {
        case M_SETTING: return "DISTANCE (fp_battle_hide 0)";
        case M_NOKEYS:  return "DISTANCE meanwhile (no keys for the followed model yet: not built, or a read failed; retried every few frames)";
        case M_PROBE:   return "DISTANCE meanwhile (his keys are built, but no draw has carried one yet)";
        case M_IDENT:   return "IDENTITY (his own model's index-buffer keys)";
        default:        return "off";
    }
}
/// A draw's class from its key. owners (may be null) gets the other owners of the key. want_others false: a key that is not
/// his is not looked up among the other actors' (the whole table is not walked for a draw nobody asks about) and is C_NONE.
inline int classify(const KeyTable& t, bool read, uint32_t ib, uint32_t off, bool want_others, uint64_t* owners) {
    if (owners) *owners = 0;
    if (!read) return C_UNREAD;
    const bool s = t.self(ib, off);
    if (s && !t.any_shared) return C_SELF;
    if (!s && !want_others) return C_NONE;
    const uint64_t o = t.others(ib, off);
    if (owners) *owners = o;
    return s ? (o ? C_SHARED : C_SELF) : (o ? C_OTHER : C_NONE);
}
/// The shared-model tie-break: is the followed actor's root (roots[0..2]) the nearest of the sharing owners' roots to the
/// draw's world translation w? roots = x y z per owner (render space); a root that is not finite is not known and is left out;
/// his own not known: true (the keys are his). A tie is his.
inline bool self_nearest(const float* w, const float* roots, int n_roots, uint64_t owners) {
    if (w == nullptr || roots == nullptr || n_roots <= 0) return true;
    const auto fin = [](const float* p) { return std::isfinite(p[0]) && std::isfinite(p[1]) && std::isfinite(p[2]); };
    if (!fin(roots) || !fin(w)) return true;
    const auto d2 = [&](const float* p) { const float x = w[0] - p[0], y = w[1] - p[1], z = w[2] - p[2]; return x * x + y * y + z * z; };
    const float ds = d2(roots);
    for (int k = 1; k < n_roots && k < k_owners; ++k) {
        if (!((owners >> k) & 1ull)) continue;
        const float* p = roots + 3 * k;
        if (!fin(p)) continue;
        if (d2(p) < ds) return false;
    }
    return true;
}
/// 1: hide it as the followed character's part; 0: it is not his; -1: the distance rule's answer stands.
/// C_HIS (his weapon, something attached to him): the distance rule, exactly as before this rule existed (the weapon is drawn
/// at the hand, outside the radius: shown; a draw of it inside the radius is hidden, as it always was).
/// partial (another actor's chain did not read, or the table is full: a twin of his model may be missing from it): his keys
/// hide only a draw that is also near; one that is not near is left to the distance rule (which shows it).
inline int verdict(int mode, int cls, bool nearest, bool nearby = true, bool partial = false) {
    if (mode != M_IDENT || cls == C_UNREAD || cls == C_HIS) return -1;
    if (cls == C_SELF) return (partial && !nearby) ? -1 : 1;
    if (cls == C_SHARED) return nearest ? 1 : 0;
    return 0;
}
/// Are all the owners in the mask attached to the followed actor (flags bit 0, from managed)? An empty mask: no.
inline bool all_his(uint64_t owners, const uint8_t* owner_flags, int n_owners) {
    if (owners == 0 || owner_flags == nullptr) return false;
    for (int k = 1; k < k_owners; ++k) if (((owners >> k) & 1ull) && (k >= n_owners || !(owner_flags[k] & 1))) return false;
    return true;
}

struct HideStats {
    uint32_t frames = 0;
    uint32_t hidden = 0;        // draws the identity rule hid (his keys)
    uint32_t seen_self = 0;     // draws that carried one of his keys (any mode that reads keys)
    uint32_t near_total = 0;    // draws the old distance rule matched ("near the camera")
    uint32_t near_self = 0, near_other = 0, near_none = 0, near_unread = 0;
    uint32_t near_his = 0;      // near draws of an actor attached to him (his weapon): left to the distance rule
    uint32_t od[k_owners] = {}; // draws by owner (0 his own, hidden or not; k another actor's), only while attribution is on
    uint32_t judged = 0;        // draws looked up in the key table at all (not the shadow pass): 0 = nothing was judged this frame
    uint32_t kept = 0;          // near draws the identity rule left visible (another actor's, or in no table)
    uint32_t tie_hidden = 0, tie_shown = 0;   // draws with a key he shares with another actor, by the nearest root
    uint32_t caster = 0;        // his own shadow's draws judged by his keys (the light-camera pass)
    void add(const HideStats& o) {
        frames += o.frames; hidden += o.hidden; seen_self += o.seen_self; near_total += o.near_total; near_self += o.near_self; near_other += o.near_other;
        near_none += o.near_none; near_unread += o.near_unread; kept += o.kept; tie_hidden += o.tie_hidden; tie_shown += o.tie_shown; caster += o.caster;
        near_his += o.near_his; judged += o.judged;
        for (int k = 0; k < k_owners; ++k) od[k] += o.od[k];
    }
};
struct Ident {
    int mode = M_OFF; bool proven = false; uint32_t proven_hash = 0;
    bool lost = false;   // the last frame_end dropped the latch: his keys matched nothing while unknown draws sat on his root
    bool partial = false, attrib = false;   // from the push: the table may miss an actor; count the draws per owner (the log is on)
    const uint8_t* owner_flags = nullptr; int n_owners = 0;   // from the push: bit 0 = the owner is attached to the followed actor
    HideStats f;   // the frame being drawn
    /// The mode for the draws to come. live: managed pushed for this frame; wanted: fp_battle_hide 1.
    void decide(bool live, bool wanted, const KeyTable& t) {
        if (!live) { mode = M_OFF; proven = false; return; }
        if (!wanted) { mode = M_SETTING; proven = false; return; }
        if (t.nf <= 0) { mode = M_NOKEYS; proven = false; return; }
        if (proven && proven_hash != t.hash) proven = false;
        mode = proven ? M_IDENT : M_PROBE;
    }
    /// One draw (render thread). nearby: the distance rule matched it; read / ib / off: its bound index buffer; w: its world
    /// translation. Returns verdict() and counts. caster: the light-camera pass (counted apart).
    int draw(const KeyTable& t, bool nearby, bool read, uint32_t ib, uint32_t off, const float* w, const float* roots, int n_roots, bool caster) {
        if (mode != M_PROBE && mode != M_IDENT) { if (nearby && !caster) f.near_total++; return -1; }
        uint64_t owners = 0;
        int cls = classify(t, read, ib, off, nearby || (attrib && !caster), &owners);
        if (cls == C_OTHER && all_his(owners, owner_flags, n_owners)) cls = C_HIS;
        const bool nearest = cls == C_SHARED ? self_nearest(w, roots, n_roots, owners) : true;
        const int v = verdict(mode, cls, nearest, nearby, partial);
        if (caster) { if (v == 1) f.caster++; return v; }
        f.judged++;
        if (cls == C_SELF || cls == C_SHARED) f.seen_self++;
        if (attrib) {   // S3: who drew (his own count includes the draws this rule hides)
            if (cls == C_SELF || cls == C_SHARED) f.od[0]++;
            for (int k = 1; owners != 0 && k < k_owners; ++k) if ((owners >> k) & 1ull) f.od[k]++;
        }
        if (nearby) {
            f.near_total++;
            if (cls == C_SELF || cls == C_SHARED) f.near_self++; else if (cls == C_OTHER) f.near_other++; else if (cls == C_HIS) f.near_his++; else if (cls == C_NONE) f.near_none++; else f.near_unread++;
            if (v == 0) f.kept++;
        }
        if (cls == C_SHARED && mode == M_IDENT) { if (v == 1) f.tie_hidden++; else f.tie_shown++; }
        if (v == 1) f.hidden++;
        return v;
    }
    /// Present: the frame's counts are returned (and cleared); a frame in which a draw carried one of his keys proves them.
    /// S1: in identity, a frame in which none of his keys was seen while draws of no actor's table sat on his root drops the
    /// latch (lost: the caller asks for a rebuild at once): the distance rule is back for the next frame, so a table that
    /// has gone stale (his buffers replaced under the same model handle) cannot leave his body in his face.
    HideStats frame_end(bool live, bool wanted, const KeyTable& t) {
        lost = mode == M_IDENT && f.seen_self == 0 && f.near_none > 0;
        if (lost) proven = false;
        else if (live && wanted && t.nf > 0 && f.seen_self > 0) { proven = true; proven_hash = t.hash; }
        HideStats r = f; r.frames = 1; f = HideStats{};
        decide(live, wanted, t);
        return r;
    }
};

/// Managed asks once a frame, before walking the model chains: rebuild the key table now? Yes on the first ask, on a new
/// signature (the followed actor, any actor's model, the scene), after a gap in the asks (a menu, another view: buffers may
/// have been freed and reused), at once after the rule lost its keys (lost()), and periodically: every k_retry frames while the followed model gave no keys, k_rebuild_partial
/// while another actor's chain did not read whole, else k_rebuild.
struct Rebuild {
    uint32_t sig = 0, built = 0, last_ask = 0; bool any = false, have = false, complete = false, force = false;
    bool due(uint32_t s, uint32_t frame) {
        const bool gap = !any || frame - last_ask > 1u;
        last_ask = frame;
        const uint32_t every = !have ? k_retry : !complete ? k_rebuild_partial : k_rebuild;
        const bool d = gap || force || s != sig || frame - built >= every;
        if (d) { sig = s; built = frame; any = true; force = false; }
        return d;
    }
    /// S1: the identity rule lost its keys (Ident::lost): the next ask rebuilds, whatever the cadence.
    void lost() { force = true; }
    /// What the build gave (from the push that follows it). fixed (N-R2-1): there is no table because his model is larger
    /// than the walk reads, which a retry 4 frames on cannot change: the slow cadence.
    void result(bool have_self, bool all_read, bool fixed = false) { have = have_self || fixed; complete = all_read; }
};

/// The "body hide: BATTLE" lines: one per run of first-person battle frames (k_hl_delay frames in, so its counts mean
/// something), one k_hl_delay frames after each change of the rule, the keys or the followed actor, and one at the run's end.
constexpr uint32_t k_hl_delay = 30; constexpr int k_hl_per_run = 12, k_hl_per_session = 300;
struct HideLog {
    bool running = false; int run_lines = 0, session_lines = 0;
    int mode = -1, pick = -2; uint32_t hash = 0; bool shared = false;
    bool pending = false; uint32_t due = 0; const char* why = "";
    HideStats acc; uint32_t max_hidden = 0, lost_n = 0;
    /// Once a frame at Present, after Ident::frame_end. live: managed pushed for the frame just drawn. Returns 1 when a line is
    /// due (the caller writes line() and calls wrote()), 2 for the run's last line; 0 none.
    int step(bool live, bool log_on, int mode_now, int pick_now, const KeyTable& t, const HideStats& fs, uint32_t frame, bool lost_now = false) {
        if (lost_now) lost_n++;
        if (!live) {
            const bool was = running; running = false; pending = false;
            if (was && acc.frames > 0 && log_on && can()) { why = "the first-person battle frames ended"; return 2; }
            acc = HideStats{}; max_hidden = 0; lost_n = 0; mode = -1; pick = -2; hash = 0;
            return 0;
        }
        if (!running) { running = true; run_lines = 0; acc = HideStats{}; max_hidden = 0; mode = mode_now; pick = pick_now; hash = t.hash; shared = t.any_shared; pending = true; due = frame + k_hl_delay; why = "a first-person battle"; }
        acc.add(fs); if (fs.hidden > max_hidden) max_hidden = fs.hidden;
        if (mode_now != mode || pick_now != pick || t.hash != hash || t.any_shared != shared) {
            const char* w = pick_now != pick ? "the followed actor changed" : mode_now != mode ? "the rule changed" : t.hash != hash ? "his keys changed" : "the shared-model state changed";
            mode = mode_now; pick = pick_now; hash = t.hash; shared = t.any_shared;
            if (!pending) { pending = true; due = frame + k_hl_delay; why = w; acc = HideStats{}; max_hidden = 0; }
        }
        if (pending && static_cast<int32_t>(frame - due) >= 0) { pending = false; if (log_on && can()) return 1; acc = HideStats{}; max_hidden = 0; }
        return 0;
    }
    bool can() const { return run_lines < k_hl_per_run && session_lines < k_hl_per_session; }
    void wrote() { run_lines++; session_lines++; acc = HideStats{}; max_hidden = 0; lost_n = 0; if (!running) { mode = -1; pick = -2; hash = 0; } }
    /// The line (after step() returned 1 or 2). who = the followed actor's model name; mode_now = the rule in effect.
    /// owner_slot (may be null) = each owner's actor-table slot, for the per-actor draw counts; partial = the push's flag 8.
    void line(Buf& b, int mode_now, const KeyTable& t, const char* who, uint32_t gen, int n_roots, const uint8_t* owner_slot = nullptr, bool partial = false) const {
        const float fr = acc.frames > 0 ? static_cast<float>(acc.frames) : 1.f;
        b.add("body hide: BATTLE (%s): rule %s | following actor slot %d '%s' | his keys %d", why, mode_name(mode_now), pick, who != nullptr ? who : "?", t.nf);
        for (int i = 0; i < t.nf && i < 4; ++i) b.add("%s%08X+%u/%u", i ? " " : " (", t.e[i].ib, t.e[i].off, t.e[i].count);
        if (t.nf > 0) b.add("%s)", t.nf > 4 ? " ..." : "");
        b.add(", %d keys of %d other actors, build %u", t.n - t.nf, n_roots > 0 ? n_roots - 1 : 0, gen);
        b.add(" | the last %u frames: draws hidden by his keys %u (%.1f a frame, at most %u), draws carrying his keys %u", acc.frames, acc.hidden, acc.hidden / fr, max_hidden, acc.seen_self);
        b.add(" | near the camera (the old distance rule's matches) %u (%.1f a frame): his own %u, his weapon's or another model's attached to him %u (left to the distance rule, as before), another actor's %u, in no actor's table %u, index buffer not read %u; kept visible by the identity rule %u",
              acc.near_total, acc.near_total / fr, acc.near_self, acc.near_his, acc.near_other, acc.near_none, acc.near_unread, acc.kept);
        if (acc.caster > 0) b.add(" | his shadow's draws hidden by his keys %u", acc.caster);
        if (t.any_shared) b.add(" | SAME MODEL as another live actor: a draw with a shared key is his when his root is the nearest: hidden %u, shown %u", acc.tie_hidden, acc.tie_shown);
        else b.add(" | same model as another live actor: no");
        if (partial) b.add(" | the table is PARTIAL (another actor's chain did not read whole, or it is full): his keys hide only draws near the camera");
        if (lost_n > 0) b.add(" | his keys were LOST %u times (none matched while draws of no actor's table sat on his spot: the distance rule again, and a rebuild at once)", lost_n);
        if (mode_now == M_IDENT && acc.near_none > 0)
            b.add(" | NOTE: %u draws on his spot with keys in no actor's table were left visible: if a part of your own body shows, his model has draws its chain does not list", acc.near_none);
        bool any_od = false;
        for (int k = 0; k < k_owners && !any_od; ++k) any_od = acc.od[k] != 0;
        if (any_od) {   // S3: who drew, by the key table (a frame's average; his own count includes the hidden draws)
            b.add(" | draws by actor (table slot: a frame):"); int shown_n = 0;
            for (int k = 0; k < k_owners && shown_n < 16; ++k) {
                if (acc.od[k] == 0) continue;
                if (owner_slot != nullptr) b.add(" %u%s %.1f,", owner_slot[k], k == 0 ? " (his)" : "", acc.od[k] / fr); else b.add(" owner %d %.1f,", k, acc.od[k] / fr);
                shown_n++;
            }
        }
        if (mode_now == M_PROBE && acc.frames >= k_hl_delay && acc.seen_self == 0 && acc.near_total > 0)
            b.add(" | NOTE: draws near the camera, none with his keys: this battle model's draws do not carry its chain's keys (the identity rule cannot work here; the distance rule stays)");
        if (run_lines + 1 >= k_hl_per_run) b.add(" (no more of these lines for this battle)");
        else if (session_lines + 1 >= k_hl_per_session) b.add(" (no more of these lines this session)");
    }
};

// ---- the log ---------------------------------------------------------------------------------------------------------------
typedef void (*LineFn)(void* ctx, const char* line);
struct DrawCounts { uint32_t cam = 0, prod = 0, noimg = 0; };   // this frame's scene-pass draws: a camera eye image, a product-only one (the PS2 effect family), none
/// The mod's side of an fx: sample (the native glue fills it; render space, the mod's own conventions: a view is three rows of
/// a rotation with the translation in column 3, the camera looks along -Z).
struct Views {
    bool ok = false;
    float game[12] = {};      // the game's main camera as its shaders get it (before the mod's override)
    float eye[2][12] = {};    // the views the mod draws each eye with
    float proj0 = 0.f, proj5 = 0.f;   // the game's main projection [0] and [5]
    float fov[2][4] = {};     // each eye's tangents: left, right, up, down
    bool fov_ok = false;
};
inline void view_pos(const float* v, float* p) { for (int i = 0; i < 3; ++i) p[i] = -(v[0 + i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]); }
inline float angle_deg(const float* a, const float* b) {
    const float la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]), lb = std::sqrt(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    if (!(la > 1e-6f) || !(lb > 1e-6f)) return -1.f;
    float c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / (la * lb);
    c = c > 1.f ? 1.f : c < -1.f ? -1.f : c;
    return std::acos(c) * 57.29578f;
}
inline void view_text(Buf& b, const char* what, const float* v) {
    float p[3]; view_pos(v, p);
    b.add("%s: position (%.2f %.2f %.2f) forward (%.3f %.3f %.3f) up (%.3f %.3f %.3f)", what, p[0], p[1], p[2], -v[8], -v[9], -v[10], v[4], v[5], v[6]);
}
inline void mat_text(Buf& b, const char* name, const float* m) {
    b.add(" | %s [%.4g %.4g %.4g %.4g][%.4g %.4g %.4g %.4g][%.4g %.4g %.4g %.4g][%.4g %.4g %.4g %.4g]", name,
          m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}
constexpr int k_mats = 6;   // 16 floats each, as the game keeps them
inline const char* mat_name(int k) {
    switch (k) {
        case 0: return "g_pppCameraMatrix 0x230FF60";
        case 1: return "g_pppViewMatrix 0x230FF20";
        case 2: return "g_pppScreenMatrix 0x230FE60";
        case 3: return "g_pppFpWoodMatrix 0x230FEA0";
        case 4: return "g_pppWoodMatrixWork 0x230FEE0";
        case 5: return "g_pppWoodMatrix 0x230FDE0";
        default: return "?";
    }
}
// The fx: samples (S2): the first on the first frame the effect is on screen (the frame's product-only or no-image scene draws
// rise k_fx_rise above the frame before the file loaded), or k_fx_s1_late frames after the load if they never do; the second
// k_fx_gap frames after the first. Review round 2 (S2-R2): a rise of 2 is also what a charge glow gives, so after the first
// sample one more is taken whenever the frame's product-only draws exceed the highest count sampled so far by half and by at
// least k_fx_peak_add (the effect's own sprites arriving), up to k_fx_samples samples per magic file in all; the end line
// gives the product-only peak, its frame, and the highest count a sample caught.
constexpr uint32_t k_fx_rise = 2, k_fx_s1_late = 30, k_fx_gap = 20, k_fx_peak_add = 8;
constexpr int k_fx_samples = 6;
constexpr uint32_t k_nodraw = 30;   // S3: frames with no draw of an actor the game shows before the line
constexpr uint8_t k_hide_cull = 0x28;   // the character hide byte's per-frame cull bits (frustum 0x08, distance 0x20)
/// Has the frame's product-only count passed the highest sampled one by half and by k_fx_peak_add?
inline bool fx_peak_due(uint32_t prod, uint32_t sampled) { return prod >= sampled + k_fx_peak_add && 2u * prod >= 3u * sampled; }
/// S3: last frame's draws by actor-table slot, from the key table (first person only; the native glue fills it).
struct Attrib { bool has[k_owners] = {}; uint32_t draws[k_owners] = {}; };
/// Build 2: what fx_face_eyes did so far this session (the native glue fills it from ffxvr_fxface.h's counters; the counts
/// only grow). on = the setting is on and this is a battle drawn in VR in first person or the diorama; hook = managed's
/// detour on the effect code's part runner (pppRunPart) is in. The last re-aimed run, game space: the part's place, the
/// game's effect camera, the head (the centre of the two eyes), where the substituted camera looks.
struct FxFace {
    bool on = false, hook = false, have_last = false;
    uint32_t runs = 0, applied = 0, no_pose = 0, copy_differs = 0, singular = 0, no_basis = 0, nested = 0;
    uint32_t cam_copies = 0;   // MsCopyCameraMatrix calls inside a re-aimed run (review S2: counted, not answered)
    bool cc_hook = false;      // managed's detour on MsCopyCameraMatrix is in (without it cam_copies says nothing)
    bool cc_refused = false;   // ... it was tried and is not in (refused); else it was not wanted: the battle research log was off
    float part[3] = {}, cam_pos[3] = {}, cam_fwd[3] = {}, head[3] = {}, look[3] = {}, eye[3] = {};
    float off_deg = 0.f, dist = 0.f; int how = 0;
};
// Build 2: Holy is the one effect with scene draws that have no eye image (up to 202 a frame in the owner's recording, 0 in
// the others). What they are cannot be told from the counts: the first frame a magic file's no-eye-image scene draws are
// k_fx_noimg_rise above the frame before it loaded, one automatic frame census is asked for (every scene draw of one frame
// with its buffers, blend state and shaders: ffxvr_frame_auto.log), k_fx_census_session a session at most.
constexpr uint32_t k_fx_noimg_rise = 8; constexpr int k_fx_census_session = 2;

struct Log {
    bool active = false; int battle = 0; uint32_t start_frame = 0, frame_now = 0;
    Budget b_cmd, b_vis, b_fx;
    VisTrack vt; Dedup dd;
    int32_t g_prev[G_COUNT] = {}; bool g_have = false;
    char names[k_actors][k_name] = {};
    int cmd_seq = 0;                       // this session's cmd: lines' numbers
    int lines_cmd = 0, lines_vis = 0, lines_fx = 0, polled = 0, hooked = 0;   // this battle
    // the last command (who is acting)
    bool act_on = false; int act_seq = 0, act_actor = -1; uint16_t act_id = 0; uint32_t act_frame = 0; char act_name[k_cname] = {}; int act_vis = 0;
    // the magic file in play
    bool fx_on = false; int32_t fx_id = -1; uint32_t fx_req_frame = 0, fx_load_frame = 0, fx_s1_frame = 0; int fx_samples = 0;
    bool fx_gap_done = false; uint32_t fx_sampled = 0, fx_peak_frame = 0;   // S2-R2: the "20 frames after the first" sample taken; the highest product-only count a sample caught; the peak's frame
    float cam_prev[12] = {}; bool cam_prev_ok = false; uint32_t cam_prev_frame = 0;   // the game's main camera of the frame before (N-R2-2)
    float mats_prev[k_mats * 16] = {}, mats_s1[k_mats * 16] = {}; bool mats_prev_ok = false; uint32_t mats_prev_frame = 0;
    // S3: per actor, the run of attributed frames without a draw (nd), those of them with a cull bit (nd_cull), the run's
    // last frames without one (nd_clear), the lines written for the run (nd_said: 1 the cull line, 2 the NO DRAWS line);
    // at_gap: frames in a row without attribution (the windows wait; a long gap forgets them)
    uint32_t nd[k_actors] = {}, nd_cull[k_actors] = {}, nd_clear[k_actors] = {}; uint8_t nd_said[k_actors] = {}; bool attrib_seen = false; uint32_t at_gap = 0;
    void nd_reset(int i) { nd[i] = 0; nd_cull[i] = 0; nd_clear[i] = 0; nd_said[i] = 0; }
    DrawCounts fx_base, fx_max, dc_prev;
    int32_t magic_prev = -1; uint32_t magic_req_frame = 0;
    // build 2: the battle camera hide's state as last told (CHS_*; -1 never) and as last written, the skips since
    // g_BtlCameraHides was last 0 and this battle; fx_face_eyes (ff: the session's counters as last told, ff_base: at the
    // magic file's load, ff_said: its first line is written); the frame census (asked for this magic file; this session's)
    int cam_state_now = -1, cam_state_said = -1, cam_state_battle = 0; uint32_t cam_skips_flag = 0, cam_skips_battle = 0;
    FxFace ff, ff_base; bool ff_have = false, ff_said = false;
    bool fx_census_done = false, census_req = false; int census_session = 0;
    char buf[4096];

    Log() {
        b_cmd.config(500, 8000, 500, 0);
        b_vis.config(600, 8000, 6, 60);
        b_fx.config(600, 8000, 600, 0);
    }
    void emit(LineFn fn, void* ctx, Buf& b, int take) {
        if (take == 2) b.add(" (no more of these lines this session)");
        if (fn) fn(ctx, b.p);
    }
    void acting_text(Buf& b) const {
        if (!act_on) { b.add(" | acting: nobody yet (no command seen this battle)"); return; }
        b.add(" | acting: #%d ", act_seq); actor_text(b, act_actor, names); b.add(" 0x%04X '%s' (%u frames ago)", act_id, act_name, frame_now - act_frame);
    }
    /// The next frame() with matrices may write an fx: sample (a file about to be seen loaded, or loaded with a sample still to
    /// come): the glue then fills Views. While a magic file is loaded and has samples left, that is every frame (S2-R2: a
    /// peak sample can come at any frame).
    bool wants_views() const { return active && (!fx_on || fx_samples < k_fx_samples); }

    void end_battle(LineFn fn, void* ctx, const char* why) {
        if (!active) return;
        if (fx_on) fx_end(fn, ctx, "the battle ended");
        Buf b(buf, static_cast<int>(sizeof(buf)));
        b.add("vis: === battle %d ended (%s; frame %u, %u frames) | lines: cmd %d (hook %d, polled %d), vis %d, fx %d | not written (the budget): cmd %d, vis %d, fx %d | draw attribution: %s",
              battle, why, frame_now, frame_now - start_frame, lines_cmd, hooked, polled, lines_vis, lines_fx, b_cmd.skipped_battle, b_vis.skipped_battle, b_fx.skipped_battle,
              attrib_seen ? "on for some frames (first person)" : "never on (no first-person frame with a key table)");
        if (cam_state_now >= 0) b.add(" | camera hides skipped by the mod: %u", cam_skips_battle);
        if (b_vis.skipped_battle > 0) {
            b.add(" | vis lines not written, by source:");
            for (int i = 0; i < k_src; ++i) {
                if (b_vis.skipped[i] == 0) continue;
                if (i < k_actors) { b.add(" "); actor_text(b, i, names); b.add(" %d,", b_vis.skipped[i]); }
                else b.add(" %s %d,", i == k_src_magic_hidden ? "g_MagicHiddenBtlChr" : i == k_src_cam_hides ? "g_BtlCameraHides" : "the roster", b_vis.skipped[i]);
            }
        }
        if (fn) fn(ctx, b.p);
        active = false; act_on = false; fx_on = false; g_have = false;
    }
    void fx_end(LineFn fn, void* ctx, const char* why) {
        const int tk = b_fx.take(0);
        if (tk) {
            Buf b(buf, static_cast<int>(sizeof(buf)));
            b.add("fx: battle %d frame %u | magic 0x%04X (%d) ended (%s) after %u frames loaded | scene-pass draws a frame while it was loaded, at most: with a camera eye image %u, product-only (the PS2 effect family) %u, with no eye image %u (the frame before it loaded: %u, %u, %u) | the product-only peak: %u at frame %u (%u frames after the load); the highest product-only count a sample caught: %u%s | samples written %d | visibility changes logged since the command %d",
                  battle, frame_now, static_cast<unsigned>(fx_id) & 0xFFFFu, fx_id, why, frame_now - fx_load_frame, fx_max.cam, fx_max.prod, fx_max.noimg, fx_base.cam, fx_base.prod, fx_base.noimg,
                  fx_max.prod, fx_peak_frame, fx_peak_frame - fx_load_frame, fx_sampled,
                  fx_samples == 0 ? " (NO sample was taken)" : fx_peak_due(fx_max.prod, fx_sampled) ? " (the peak was NOT sampled: no sample left, or no matrices that frame)" : " (a sample is at or near the peak)",
                  fx_samples, act_vis);
            fx_face_total(b);
            acting_text(b);
            emit(fn, ctx, b, tk); lines_fx++;
        }
        fx_on = false;
    }
    /// Build 2: the end line's part. Nothing when the glue never told the state (the tests of build 1, an older caller).
    void fx_face_total(Buf& b) const {
        if (!ff_have) return;
        const uint32_t runs = ff.runs - ff_base.runs, done = ff.applied - ff_base.applied;
        if (runs == 0) { b.add(" | fx_face_eyes: %s", !ff.on ? "off, or not a battle drawn in VR in first person or the diorama (no part run was looked at)" : !ff.hook ? "on, but the detour on pppRunPart is NOT in: nothing was re-aimed" : "on, but the effect code ran no part through pppRunPart while it was loaded: nothing was re-aimed"); return; }
        b.add(" | fx_face_eyes: %u part runs (pppRunPart) while it was loaded, %u re-aimed at the eyes, %u left as the game's (no head pose %u, the view's copy is not a copy of the view %u, a matrix did not invert %u, no level basis %u, nested in a re-aimed run %u)",
              runs, done, runs - done, ff.no_pose - ff_base.no_pose, ff.copy_differs - ff_base.copy_differs, ff.singular - ff_base.singular, ff.no_basis - ff_base.no_basis, ff.nested - ff_base.nested);
        // S2: "re-aimed" says the matrices were substituted, not that everything the part built followed them
        if (!ff.cc_hook) b.add("; MsCopyCameraMatrix calls inside those runs: %s", ff.cc_refused ? "not counted: the counting detour was refused" : "not counted: the battle research log was off (the counting detour is inserted only with this box ticked and the battle research log on)");
        else if (ff.cam_copies == ff_base.cam_copies) b.add("; MsCopyCameraMatrix was not called inside those runs (no part took the game's camera by that route)");
        else b.add("; MsCopyCameraMatrix was called %u times INSIDE those runs: that code took the game's own camera, and what it built from it in view space is DISPLACED (only counted, not answered)", ff.cam_copies - ff_base.cam_copies);
    }
    /// Build 2, once a frame before frame() (the render thread): the session's fx_face_eyes counters. A magic file's first
    /// re-aimed part run gets one fx: line (what was substituted).
    void fx_face(const FxFace& f, LineFn fn, void* ctx) {
        if (!ff_have) ff_base = f;
        ff = f; ff_have = true;
        if (!active || !fx_on || ff_said || !f.have_last || f.applied == ff_base.applied) return;
        ff_said = true;
        const int tk = b_fx.take(0);
        if (!tk) return;
        Buf b(buf, static_cast<int>(sizeof(buf)));
        b.add("fx: battle %d frame %u | magic 0x%04X | fx_face_eyes: this effect's parts are being aimed at the eyes (game space; a re-aimed run of the last frame) | the part at (%.1f %.1f %.1f), the game's effect camera at (%.1f %.1f %.1f) looking (%.3f %.3f %.3f)",
              battle, frame_now, static_cast<unsigned>(fx_id) & 0xFFFFu, f.part[0], f.part[1], f.part[2], f.cam_pos[0], f.cam_pos[1], f.cam_pos[2], f.cam_fwd[0], f.cam_fwd[1], f.cam_fwd[2]);
        b.add(" | substituted for that run: a camera on the line from the head (the centre of the two eyes) (%.1f %.1f %.1f) to the part, as far from it as the part is deep in the game's own view (%.1f): at (%.1f %.1f %.1f) looking (%.3f %.3f %.3f): %s; it looks %.1f deg off the game's",
              f.head[0], f.head[1], f.head[2], f.dist, f.eye[0], f.eye[1], f.eye[2], f.look[0], f.look[1], f.look[2],
              f.how == 0 ? "at the part, level (no roll)" : f.how == 1 ? "along the head's own forward, level (the part is at the head)" : "at the part, from straight above or below (the head's own up for the roll)", f.off_deg);
        b.add(" | so far for this magic file: %u part runs, %u re-aimed", f.runs - ff_base.runs, f.applied - ff_base.applied);
        acting_text(b);
        emit(fn, ctx, b, tk); lines_fx++;
    }
    /// Build 2, once a frame before frame() (the render thread): the battle camera hide's state (CHS_*). One vis: line when
    /// it is first known in a logged battle, and one when it changes.
    void cam_state(int state, LineFn fn, void* ctx) {
        cam_state_now = state;
        if (!active || (cam_state_battle == battle && cam_state_said == state)) return;
        cam_state_battle = battle; cam_state_said = state;
        const int tk = b_vis.take(k_src_cam_hides);
        if (!tk) return;
        Buf b(buf, static_cast<int>(sizeof(buf)));
        b.add("vis: battle %d frame %u | battle_cam_hide %s", battle, frame_now, cam_hide_text(state));
        emit(fn, ctx, b, tk); lines_vis++;
    }
    /// Build 2: the detour skipped one camera hide of this actor (drained from the ring, before frame()). fx_cam = the
    /// game's effect-camera mode byte (0x112C9D7) when the hide was asked; n_frame = how many hides were skipped since the
    /// last frame (review S3: several at once, or an effect camera running, is a staged shot rather than one character in
    /// the camera's way).
    void cam_skip(int actor, int fx_cam, int n_frame, LineFn fn, void* ctx) {
        if (!active) return;
        cam_skips_flag++; cam_skips_battle++;
        const int tk = b_vis.take(actor >= 0 && actor < k_actors ? actor : k_src_cam_hides);
        if (!tk) return;
        Buf b(buf, static_cast<int>(sizeof(buf)));
        b.add("vis: battle %d frame %u | ", battle, frame_now); actor_text(b, actor, names);
        b.add(" | camera hide SKIPPED by the mod (battle_cam_hide 0): the game's battle camera asked to hide it (msSetChrVisibleReq(%d, 3 = camera, 0)); the call was not passed on: no vote changed, it stays drawn", actor);
        b.add(" | %d hide(s) skipped since the last frame%s, effect-camera mode 0x%02X%s", n_frame, n_frame > 1 ? " (several at once: a staged shot?)" : "", static_cast<unsigned>(fx_cam) & 0xFFu, (fx_cam & 0xFF) != 0 ? " (an effect camera is running)" : "");
        acting_text(b);
        if (g_have) b.add(" | command actor now %d", g_prev[G_CMD_ACTOR]);
        emit(fn, ctx, b, tk); lines_vis++; act_vis++;
    }
    /// Build 2: true once per request: the glue arms the automatic frame census (ffxvr_frame_auto.log).
    bool take_census() { const bool r = census_req; census_req = false; return r; }
    /// Branch late3d (ABI 70; ffxvr_late3d.h): a late 3D layer's line as an fx: line of the logged battle. The glue passes
    /// the magic id's text and the line's body (the render thread, once a frame before frame()). False = not written (no
    /// battle is logged, or the fx: budget is spent): the glue then writes its own late3d: line.
    bool late3d(const char* magic, const char* what, LineFn fn, void* ctx) {
        if (!active) return false;
        const int l3_tk = b_fx.take(0);
        if (!l3_tk) return false;
        Buf b(buf, static_cast<int>(sizeof(buf)));
        b.add("fx: battle %d frame %u | %s | %s", battle, frame_now, magic, what);
        acting_text(b);
        emit(fn, ctx, b, l3_tk); lines_fx++;
        return true;
    }

    /// One executed command (from the detour, or the poll's request answered by managed).
    void cmd(const CmdEv& e, uint32_t frame, LineFn fn, void* ctx) {
        if (!active) return;   // (a detour event with no battle frame yet is not a battle's command)
        frame_now = frame;
        if (e.src == 0) { dd.on_hook(e.actor, e.exe_count, e.id); hooked++; } else polled++;
        char nm[k_cname], nm0[k_cname]; copy_name(nm, k_cname, e.name); copy_name(nm0, k_cname, e.name0);
        act_on = true; act_seq = ++cmd_seq; act_actor = e.actor; act_id = e.id; act_frame = frame; act_vis = 0; memcpy(act_name, nm, sizeof(act_name));
        const int tk = b_cmd.take(0);
        if (!tk) return;
        Buf b(buf, static_cast<int>(sizeof(buf)));
        b.add("cmd: #%d battle %d frame %u | ", act_seq, battle, frame); actor_text(b, e.actor, names);
        b.add(" | 0x%04X '%s'", e.id, nm[0] ? nm : "?");
        if (e.list0 != e.id && e.list0 != 0 && e.list0 != 0xFF) b.add(" from 0x%04X '%s'", e.list0, nm0[0] ? nm0 : "?");
        b.add(" | kind: %s", cmd_kind(e.id, e.rec_ok ? e.kind : -1));
        if (cmd_is_swap(e.id) || cmd_is_summon(e.id)) {
            b.add(", argument 0x%04X", e.list1);
            if (e.list1 < k_actors && names[e.list1][0]) b.add(" = actor %u '%s' (%s)", e.list1, names[e.list1], cmd_is_swap(e.id) ? "comes in" : "the aeon");
        }
        b.add(" | targets "); targets_text(b, e.targets, names);
        if (e.sub != 0xFF) b.add(" | cue group %u of %u, ids 0x%04X 0x%04X", e.sub + 1u, e.groups, e.list0, e.list1); else b.add(" | no cue entry read");
        if (e.rec_ok) b.add(" | record: anim kind 0x%02X, sub-menu 0x%02X, target flags 0x%04X, flags 0x%08X%s", e.kind, e.sub_menu, e.target_flags, e.misc, e.rec_ok == 2 ? "" : " (our own table walk)");
        else b.add(" | record: not read");
        b.add(" | exe count %u | source: %s", e.exe_count, e.src == 0 ? "hook" : "polled (the hook did not report it)");
        emit(fn, ctx, b, tk); lines_cmd++;
    }

    /// Once per Present while the log is on. n = 0 (or state 0): not in a battle. Returns the number of polled commands
    /// written to out (managed looks each up and sends it back through cmd()).
    int frame(const ActorIn* a, int n, const int32_t* g, int ng, const float* mats, const Views* vw, const DrawCounts& dc, uint32_t frame,
              PolledReq* out, int out_cap, LineFn fn, void* ctx, const Attrib* at = nullptr) {
        frame_now = frame;
        const bool in_battle = a != nullptr && n > 0 && g != nullptr && ng >= G_COUNT && g[G_STATE] != 0;
        if (!in_battle) { end_battle(fn, ctx, "the game left the battle, or the log was turned off"); dc_prev = dc; return 0; }
        if (n > k_actors) n = k_actors;
        bool baseline = false;
        if (!active) {
            active = true; baseline = true; battle++; start_frame = frame;
            b_cmd.battle_start(); b_vis.battle_start(); b_fx.battle_start(); vt.reset(); dd.reset();
            lines_cmd = lines_vis = lines_fx = polled = hooked = 0; act_on = false; fx_on = false; g_have = false; magic_prev = -1;
            mats_prev_ok = false; cam_prev_ok = false; attrib_seen = false; at_gap = 0;
            cam_skips_flag = 0; cam_skips_battle = 0;
            for (int i = 0; i < k_actors; ++i) nd_reset(i);
        }
        // (a record without a character keeps its name when it has one: a party member on the bench, an aeon not yet called)
        for (int i = 0; i < k_actors; ++i) { if (i < n && a[i].name[0]) copy_name(names[i], k_name, a[i].name); else if (baseline) names[i][0] = 0; }
        if (baseline) start_lines(a, n, g, fn, ctx);

        // S3-R2: a frame without attribution (no draw was judged: a battle tag's hold, the diorama, a frame not drawn) is not a
        // frame without draws: the windows wait; after k_nodraw such frames in a row they are forgotten
        if (at != nullptr) at_gap = 0;
        else if (at_gap < k_nodraw && ++at_gap == k_nodraw) { for (int i = 0; i < k_actors; ++i) nd_reset(i); }
        // the actors
        int nout = 0;
        for (int i = 0; i < n; ++i) {
            ActorState was{};
            const int ch = vt.step(i, a[i], baseline, &was);
            if (ch != VS_NONE) {
                const int tk = b_vis.take(i);
                if (tk) {
                    Buf b(buf, static_cast<int>(sizeof(buf)));
                    b.add("vis: battle %d frame %u | ", battle, frame); actor_text(b, i, names);
                    if (ch == VS_GONE) { b.add(" | its record lost its character (freed, or left the battle); it was: "); state_text(b, was, nullptr, was.b[5]); }
                    else if (ch == VS_FIRST) { b.add(" (slot %u) | first seen: ", a[i].slot); state_text(b, state_of(a[i]), nullptr, a[i].hide); }
                    else { b.add(" (slot %u) | ", a[i].slot); state_text(b, state_of(a[i]), &was, a[i].hide); }
                    if (ch != VS_GONE) b.add(" | now %s", shown(state_of(a[i])) ? "SHOWN" : "HIDDEN");
                    acting_text(b); b.add(" | command actor now %d", g[G_CMD_ACTOR]);
                    emit(fn, ctx, b, tk); lines_vis++; act_vis++;
                }
            }
            nodraw(i, a[i], at, g, fn, ctx);
            PolledReq pr{};
            if (dd.poll(i, a[i].live != 0, a[i].exe_count, a[i].cmd, a[i].targets, frame, (g[G_FLAGS] & GF_HOOK_BUSY) != 0, &pr) && out != nullptr && nout < out_cap) out[nout++] = pr;
        }
        // the globals
        if (g_have) {
            global_line(k_src_magic_hidden, "g_MagicHiddenBtlChr 0xC64CDC", g_prev[G_MAGIC_HIDDEN], g[G_MAGIC_HIDDEN], g, fn, ctx);
            global_line(k_src_cam_hides, "g_BtlCameraHides 0x112C9FE", g_prev[G_CAM_HIDES], g[G_CAM_HIDES], g, fn, ctx);
            if (g_prev[G_CAM_HIDES] != 0 && g[G_CAM_HIDES] == 0) cam_skips_flag = 0;   // build 2: counted since the flag was last 0 (written or not)
            if (g_prev[G_ROSTER] != g[G_ROSTER]) {
                const int tk = b_vis.take(k_src_roster);
                if (tk) {
                    Buf b(buf, static_cast<int>(sizeof(buf)));
                    const uint32_t p = static_cast<uint32_t>(g_prev[G_ROSTER]), q = static_cast<uint32_t>(g[G_ROSTER]);
                    b.add("cmd: battle %d frame %u | the field roster [%u %u %u] -> [%u %u %u] (255 = empty; a swap, a summon or a dismissal; chr ids 8..17 are aeons)", battle, frame,
                          p & 0xFF, (p >> 8) & 0xFF, (p >> 16) & 0xFF, q & 0xFF, (q >> 8) & 0xFF, (q >> 16) & 0xFF);
                    acting_text(b);
                    emit(fn, ctx, b, tk); lines_cmd++;
                }
            }
        }
        // the magic file
        const int32_t mid = g[G_MAGIC], mh = g[G_MAGIC_HANDLE];
        if (mid != magic_prev) { if (mid != -1) magic_req_frame = frame; magic_prev = mid; }
        const bool loaded = mid != -1 && mh != -1;
        if (fx_on && (!loaded || mid != fx_id)) fx_end(fn, ctx, !loaded ? "the game unloaded it" : "another magic id took its place");
        if (!fx_on && loaded) {
            fx_on = true; fx_id = mid; fx_load_frame = frame; fx_req_frame = magic_req_frame; fx_samples = 0; fx_s1_frame = 0; fx_base = dc_prev; fx_max = DrawCounts{};
            fx_gap_done = false; fx_sampled = 0; fx_peak_frame = frame;
            ff_base = ff; ff_said = false; fx_census_done = false;
            const int tk = b_fx.take(0);
            if (tk) {
                Buf b(buf, static_cast<int>(sizeof(buf)));
                b.add("fx: battle %d frame %u | magic 0x%04X (%d) loaded, handle %d, file magic_%04d.dll (the game's name pattern per ffx-rust: unconfirmed), id set %u frames before",
                      battle, frame, static_cast<unsigned>(mid) & 0xFFFFu, mid, mh, mid, frame - fx_req_frame);
                acting_text(b);
                emit(fn, ctx, b, tk); lines_fx++;
            }
        }
        if (fx_on) {
            if (dc.cam > fx_max.cam) fx_max.cam = dc.cam;
            if (dc.prod > fx_max.prod) { fx_max.prod = dc.prod; fx_peak_frame = frame; }
            if (dc.noimg > fx_max.noimg) fx_max.noimg = dc.noimg;
            const uint32_t age = frame - fx_load_frame;
            if (!fx_census_done && dc.noimg >= fx_base.noimg + k_fx_noimg_rise && census_session < k_fx_census_session) {   // build 2: what are its no-eye-image draws?
                fx_census_done = true; census_session++; census_req = true;
                const int tk = b_fx.take(0);
                if (tk) {
                    Buf b(buf, static_cast<int>(sizeof(buf)));
                    b.add("fx: battle %d frame %u | magic 0x%04X: %u scene-pass draws with no eye image this frame (the frame before it loaded: %u): the mod draws those the same in both eyes | a frame census was asked for (ffxvr_frame_auto.log: every scene draw of one frame, two or three frames from now, with its constant buffers, blend state and shaders: the NO IMAGE lines are these draws; %d of %d a session)",
                          battle, frame, static_cast<unsigned>(fx_id) & 0xFFFFu, dc.noimg, fx_base.noimg, census_session, k_fx_census_session);
                    acting_text(b);
                    emit(fn, ctx, b, tk); lines_fx++;
                }
            }
            const bool risen = dc.prod >= fx_base.prod + k_fx_rise || dc.noimg >= fx_base.noimg + k_fx_rise;   // the effect's own draws are on screen
            if (mats != nullptr && fx_samples == 0 && (risen || age >= k_fx_s1_late)) {
                fx_samples = 1; fx_s1_frame = frame;
                fx_sample(mats, vw, age, dc, risen ? "the first rise of the effect draws: this may be a charge glow, not the effect itself" : "its draws never rose: taken anyway", fn, ctx);
            }
            else if (mats != nullptr && fx_samples >= 1 && fx_samples < k_fx_samples && fx_peak_due(dc.prod, fx_sampled)) {   // S2-R2: the effect's own sprites
                fx_samples++;
                fx_sample(mats, vw, age, dc, "a new product-only peak: half again above the highest sampled so far", fn, ctx);
            }
            else if (mats != nullptr && fx_samples >= 1 && fx_samples < k_fx_samples && !fx_gap_done && frame - fx_s1_frame >= k_fx_gap) {
                fx_samples++; fx_gap_done = true;
                fx_sample(mats, vw, age, dc, "20 frames after the first", fn, ctx);
            }
        }
        if (vw != nullptr && vw->ok) { memcpy(cam_prev, vw->game, sizeof(cam_prev)); cam_prev_ok = true; cam_prev_frame = frame; } else cam_prev_ok = false;
        if (mats != nullptr) { memcpy(mats_prev, mats, sizeof(mats_prev)); mats_prev_ok = true; mats_prev_frame = frame; } else mats_prev_ok = false;
        if (at != nullptr) attrib_seen = true;
        for (int k = 0; k < G_COUNT; ++k) g_prev[k] = g[k];
        g_have = true; dc_prev = dc;
        return nout;
    }

    /// hide_raw: the character's whole hide byte now (the state keeps only its script bits).
    void state_text(Buf& b, const ActorState& s, const ActorState* was, uint8_t hide_raw) {
        votes_text(b, s.b, was ? was->b : nullptr);
        b.add(" | chr hide ");
        if (was != nullptr && was->b[5] != s.b[5]) { hide_text(b, was->b[5]); b.add(" -> "); }
        hide_text(b, hide_raw);
        if (was != nullptr && was->b[6] != s.b[6]) b.add(" | flags 0x%02X -> 0x%02X", was->b[6], s.b[6]);
        if (s.b[6] & F_ALPHA0) b.add(" | its alpha is 0 (not drawn)");
        if (s.b[6] & F_RELEASED) b.add(" | its model is released");
        if (s.b[6] & F_NOCHAR) b.add(" | its character is not in the actor table");
        if (s.b[6] & F_DRAWFX) b.add(" | its +0x194 bit 13 is set (Ch_DrawEnEffect; what it does to the draw is unconfirmed)");
    }
    void start_lines(const ActorIn* a, int n, const int32_t* g, LineFn fn, void* ctx) {
        Buf b(buf, static_cast<int>(sizeof(buf)));
        int live = 0; for (int i = 0; i < n; ++i) if (a[i].live) live++;
        b.add("vis: === battle %d start (frame %u, battle state 0x%02X, view %d, following chr %d) | hook %s | records: %s | draw attribution (NO DRAWS lines): first person only | %d actors with a character (v = visible, then the four votes main appear effect camera, h = chr hide):",
              battle, frame_now, static_cast<unsigned>(g[G_STATE]) & 0xFFu, g[G_VIEW], g[G_FOLLOW],
              (g[G_FLAGS] & GF_HOOK) ? "in (cmd: lines from MsParseCommand)" : "NOT in (cmd: lines polled only)",
              (g[G_FLAGS] & GF_SHORT) ? "only 0..0x13 read (the monsters' block did not read)" : (g[G_FLAGS] & GF_MON_BAD) ? "0..0x1E read as one array, but the monsters' pointer is NOT the party's + 0x14 records (actors 0x14.. may be wrong: unconfirmed layout)"
                  : (g[G_FLAGS] & GF_MON_OK) ? "0..0x1E, one array (the monsters' pointer agrees)" : "0..0x1E read as one array (the monsters' pointer did not read)", live);
        for (int i = 0; i < n; ++i) {
            if (!a[i].live) continue;
            b.add(" %d '%s' v%u %u%u%u%u h%02X%s;", i, names[i], a[i].vis[0], a[i].vis[1], a[i].vis[2], a[i].vis[3], a[i].vis[4], a[i].hide, (a[i].flags & F_ALPHA0) ? " alpha0" : "");
        }
        if (fn) fn(ctx, b.p);
        Buf c(buf, static_cast<int>(sizeof(buf)));
        c.add("vis: %s", votes_legend());
        if (battle == 1 && fn) fn(ctx, c.p);
    }
    void global_line(int src, const char* name, int32_t was, int32_t now, const int32_t* g, LineFn fn, void* ctx) {
        if (was == now) return;
        const int tk = b_vis.take(src);
        if (!tk) return;
        Buf b(buf, static_cast<int>(sizeof(buf)));
        b.add("vis: battle %d frame %u | %s %d -> %d", battle, frame_now, name, was, now);
        if (src == k_src_magic_hidden) {
            const int32_t who = now != -1 ? now : was;
            if (who >= 0 && who < k_actors) { b.add(" ("); actor_text(b, who, names); b.add(now != -1 ? ": hidden by the magic's effect script without a vote of its own" : ": shown again by MagicFile_Update"); b.add("; unconfirmed)"); }
        }
        else if (now != 0 && cam_state_now == CHS_ON) b.add(" (a battle camera asked to hide characters: MsSetChrVisibleCamera; battle_cam_hide 0: the mod skipped %u hide(s) so far this battle, so no actor's camera vote changes; the flag stays 1 from here on, as in the game)", cam_skips_battle);
        else b.add(now != 0 ? " (a battle camera hid characters: MsSetChrVisibleCamera; unconfirmed)" : " (the camera's hides were taken back)");
        if (src == k_src_cam_hides && now == 0 && cam_skips_flag > 0) b.add(" (the mod had skipped %u hide(s) meanwhile: the camera hid nothing)", cam_skips_flag);   // build 2
        b.add(" | magic 0x%04X", static_cast<unsigned>(g[G_MAGIC]) & 0xFFFFu);
        acting_text(b); b.add(" | command actor now %d", g[G_CMD_ACTOR]);
        emit(fn, ctx, b, tk); lines_vis++; act_vis++;
    }
    /// S3: an actor the game shows whose model's keys no draw has carried for k_nodraw attributed frames: one line; one more
    /// when its draws are back. Only while the key table attributes draws (a first-person battle with the identity rule's
    /// table in use, and a frame in which draws were judged: at == nullptr leaves the windows as they are, S3-R2).
    /// S3-R2: the game's own camera culls characters (hide byte 0x08 / 0x20: common in first person, behind the game's
    /// camera). A run with a cull bit in it gets the "culled by the game's camera" line; the NO DRAWS line is kept for
    /// k_nodraw frames in a row with no draw and no cull bit.
    void nodraw(int i, const ActorIn& a, const Attrib* at, const int32_t* g, LineFn fn, void* ctx) {
        if (at == nullptr) return;
        const bool can = a.live && a.slot < k_owners && at->has[a.slot] && shown(state_of(a));
        if (!can) { nd_reset(i); return; }
        const uint32_t d = at->draws[a.slot];
        if (d != 0) {
            const uint32_t gone = nd[i], culled = nd_cull[i]; const uint8_t said = nd_said[i];
            nd_reset(i);
            if (said == 0) return;
            const int tk = b_vis.take(i);
            if (!tk) return;
            Buf b(buf, static_cast<int>(sizeof(buf)));
            b.add("vis: battle %d frame %u | ", battle, frame_now); actor_text(b, i, names);
            b.add(" (slot %u) | DRAWS AGAIN: %u draws with its model's keys this frame, after %u frames with none (the game's camera culled it in %u of them)", a.slot, d, gone, culled);
            acting_text(b); emit(fn, ctx, b, tk); lines_vis++;
            return;
        }
        nd[i]++;
        if ((a.hide & k_hide_cull) != 0) { nd_cull[i]++; nd_clear[i] = 0; } else nd_clear[i]++;
        const bool alarm = nd_clear[i] == k_nodraw && (nd_said[i] & 2) == 0;
        const bool cull = !alarm && nd[i] == k_nodraw && nd_cull[i] > 0 && nd_said[i] == 0;
        if (!alarm && !cull) return;
        nd_said[i] |= alarm ? 2 : 1;
        const int tk = b_vis.take(i);
        if (!tk) return;
        Buf b(buf, static_cast<int>(sizeof(buf)));
        b.add("vis: battle %d frame %u | ", battle, frame_now); actor_text(b, i, names);
        if (cull) {
            b.add(" (slot %u) | NOT DRAWN, culled by the game's camera: no draw has carried its model's keys for %u frames, and in %u of them the game's own cull had it (chr hide now ", a.slot, k_nodraw, nd_cull[i]);
            hide_text(b, a.hide);
            b.add("): the game's camera does not see it; not a hide by a script and not the mod (first person; a NO DRAWS line follows if it stays undrawn %u frames with no cull bit)", k_nodraw);
        }
        else {
            b.add(" (slot %u) | NO DRAWS: the game shows it (", a.slot); votes_text(b, a.vis, nullptr); b.add(" | chr hide "); hide_text(b, a.hide);
            b.add("), with no cull bit, but no draw has carried its model's keys for %u frames", k_nodraw);
            if (nd[i] > k_nodraw) b.add(" (%u frames in all, %u of them culled by the game's camera before)", nd[i], nd_cull[i]);
            b.add(" (first person; only draws with an eye image and a world position are counted: not the game's hide and not its cull, so the draws are missing or have no eye image)");
        }
        acting_text(b); b.add(" | command actor now %d", g[G_CMD_ACTOR]);
        emit(fn, ctx, b, tk); lines_vis++; act_vis++;
    }
    /// One sample (fx_samples is already its number): its lines, then what the next sample compares with.
    void fx_sample(const float* mats, const Views* vw, uint32_t age, const DrawCounts& dc, const char* trigger, LineFn fn, void* ctx) {
        fx_sample_lines(mats, vw, age, dc, trigger, fn, ctx);
        memcpy(mats_s1, mats, sizeof(mats_s1));
        if (dc.prod > fx_sampled) fx_sampled = dc.prod;
    }
    void fx_sample_lines(const float* mats, const Views* vw, uint32_t age, const DrawCounts& dc, const char* trigger, LineFn fn, void* ctx) {
        int tk = b_fx.take(0);
        if (tk) {
            Buf b(buf, static_cast<int>(sizeof(buf)));
            b.add("fx: battle %d frame %u | magic 0x%04X sample %d (%u frames after it loaded; %s) | this frame's scene-pass draws: camera eye image %u, product-only %u, no eye image %u (before the load: %u, %u, %u; the highest product-only count sampled before: %u)",
                  battle, frame_now, static_cast<unsigned>(fx_id) & 0xFFFFu, fx_samples, age, trigger, dc.cam, dc.prod, dc.noimg, fx_base.cam, fx_base.prod, fx_base.noimg, fx_sampled);
            if (mats_prev_ok && mats_prev_frame + 1 == frame_now) b.add(" | g_pppCameraMatrix changed since the frame before: %s", memcmp(mats_prev, mats, 16 * sizeof(float)) != 0 ? "yes (the effect code is copying the camera)" : "NO (it may be stale: not written this frame)");
            else b.add(" | g_pppCameraMatrix changed since the frame before: not known (no copy of the frame before)");
            // N-R2-2: a camera standing still gives "NO" too: say whether the game's main camera moved in the same frame
            if (vw != nullptr && vw->ok && cam_prev_ok && cam_prev_frame + 1 == frame_now) b.add("; the game's main camera moved since the frame before: %s", memcmp(cam_prev, vw->game, sizeof(cam_prev)) != 0 ? "yes" : "no (a still camera: \"NO\" above then says nothing)");
            else b.add("; the game's main camera moved since the frame before: not known");
            if (fx_samples >= 2) {
                b.add(" | changed since sample %d:", fx_samples - 1);
                bool any = false;
                for (int k = 0; k < k_mats; ++k) if (memcmp(mats_s1 + 16 * k, mats + 16 * k, 16 * sizeof(float)) != 0) { b.add(" %s,", mat_name(k)); any = true; }
                if (!any) b.add(" none of the six (they are not being written)");
            }
            b.add(" | the game's effect matrices, 4 rows of 4 as stored (game space)");
            for (int k = 0; k < k_mats; ++k) mat_text(b, mat_name(k), mats + 16 * k);
            if (ff_have && ff.on && ff.applied != ff_base.applied) b.add(" | fx_face_eyes is re-aiming this effect's parts: g_pppCameraMatrix and g_pppViewMatrix above may be the substituted ones (they are read here on another thread, and are substituted only during a part run)");
            emit(fn, ctx, b, tk); lines_fx++;
        }
        if (vw == nullptr || !vw->ok) return;
        tk = b_fx.take(0);
        if (!tk) return;
        Buf b(buf, static_cast<int>(sizeof(buf)));
        b.add("fx: battle %d frame %u | magic 0x%04X sample %d: the views of the same frame (render space: game space with x and y negated) | ", battle, frame_now, static_cast<unsigned>(fx_id) & 0xFFFFu, fx_samples);
        view_text(b, "the game's main camera (what its effect code aims sprites at)", vw->game);
        b.add(" | "); view_text(b, "eye 1", vw->eye[0]); b.add(" | "); view_text(b, "eye 2", vw->eye[1]);
        const float gf[3] = { -vw->game[8], -vw->game[9], -vw->game[10] }, ef[3] = { -vw->eye[0][8], -vw->eye[0][9], -vw->eye[0][10] };
        b.add(" | eye 1 looks %.1f deg off the game's camera, its up is %.1f deg off", angle_deg(gf, ef), angle_deg(vw->game + 4, vw->eye[0] + 4));
        if (vw->proj0 != 0.f && vw->proj5 != 0.f) b.add(" | the game's projection: P0 %.4f P5 %.4f (width : height %.3f)", vw->proj0, vw->proj5, std::fabs(vw->proj5 / vw->proj0));
        if (vw->fov_ok) for (int e = 0; e < 2; ++e) {
            const float* f = vw->fov[e]; const float w = f[1] - f[0], h = f[2] - f[3];
            if (w > 1e-3f && h > 1e-3f) b.add(" | eye %d tangents l %.3f r %.3f u %.3f d %.3f (P0 %.4f P5 %.4f, width : height %.3f)", e + 1, f[0], f[1], f[2], f[3], 2.f / w, 2.f / h, w / h);
        }
        emit(fn, ctx, b, tk); lines_fx++;
    }
};

}  // namespace bvis
