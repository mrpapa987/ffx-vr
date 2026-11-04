// SPDX-License-Identifier: MIT

namespace FFXVR;

// Direct game-state gates (gates, 4 Nov 2026; design-notes "Direct game-state gates and overdrive minigame state"). The tap
// gate (hands.cs hands2_tap_game) and the hands' game hide rule read the game's own flags, next to the older gates (kept,
// ORed: any shut reason shuts). Facts from Debug Revival, FFX_TAS_Python and the FFX screen reader, each re-checked in our
// exe's disassembly; no code from any of them. Module offsets (VA - 0x400000), as FhUtil.ptr_at takes them:
//   0xF00788  dword  the player actor (Sg_GetPlayer 0x82D860 returns [0x1300788]); 0 = the player has no control
//   0xF40824  dword  a field (TkMenu) menu is up (1 at 0x8A9D8A, 0 at 0x8A9FCB; the post-battle results too)
//   0xD2A008  dword  a skippable movie runs (1 at 0x76EB62 / 0x76EBC0, 0 at 0x76E968 / 0x76EA9C / 0x76ECE9)
//   0x840E28  dword  the movie id (-1 none; the image's value is -1), F8 only
//   0x85A03C  byte   a message window reached state 2 (0x8B6B70); its image value is 0xFF, so it counts only with a window
//                    that is open and shown in the block below
//   0x14663F0 .. 0x1468AB0 (VA 0x18663F0 ..), one guarded read: the 8 message windows (two layouts, 0x130 bytes each; +0x00
//                    u16 1 = open (0x8B8560), +0x2C u16 the state: 2 shown, 3 closing (0x8B5810)), their pointers at VA
//                    0x18676F0; the 8 choice cursors (0x138 bytes each; +0x01 1 = a choice cursor up, set only at window
//                    state 2 (0x8B6004), 2 = confirmed (0x8B6483); +0x02 s8 the highlighted row), their pointers at VA 0x1868A90.
public sealed unsafe partial class FFXVRModule {

    private uint   _gs_player, _gs_menu, _gs_movie;   // the last direct reads (F8)
    private byte   _gs_text_byte;
    private GameGates.Text _gs_text;                  // the window / choice scan last read
    private bool   _gs_have;                          // the direct reads ran at least once
    private ulong  _gs_player_frame = ulong.MaxValue; private bool _gs_player_ok;   // the player read this frame (push_player and the gate share it)
    private PickCheck _gs_pick;                       // the follow pick against the game's player actor (diagnostic only)
    private string _gs_pick_last = "";                // the last mismatch line (F8)
    private byte[]? _gs_blk;                          // the window / cursor block (k_blk_len bytes), allocated at the first full read

    /// <summary>[0x1300788] once per frame (push_player's pick check and the tap gate share it).</summary>
    private bool gs_player(out uint player) {
        if (_gs_player_frame != _frames) { _gs_player_frame = _frames; _gs_player_ok = read(FhUtil.ptr_at<uint>(GameGates.k_player), out _gs_player); }
        player = _gs_player; return _gs_player_ok;
    }

    /// <summary>The view bits (battle, a field menu, a movie): read on every frame that can draw the hands (TapPlan k_battle
    /// and k_full); native hides the hands on any of them. 0x100: a read failed.</summary>
    private int hands2_view_bits() {
        if (_fn_safe_read == null) return 0x100;
        int g = hands2_battle_bit();
        if (!read(FhUtil.ptr_at<uint>(GameGates.k_menu), out uint menu)) g |= 0x100; else { _gs_menu = menu; if (menu != 0) g |= 0x80; }
        if (!read(FhUtil.ptr_at<uint>(GameGates.k_movie_skip), out uint mv)) g |= 0x100; else { _gs_movie = mv; if (mv != 0) g |= 0x2000; }
        return g;
    }

    /// <summary>The tap gate's direct bits on top of the view bits: no player control (0x40), a choice box (0x800), a text box
    /// waiting by the render side (0x1000). Three guarded reads (the player, the byte, the 9.7 KB window / cursor block).</summary>
    private int gates_direct_bits() {
        int g = 0;
        if (!gs_player(out uint player)) g |= 0x100;
        if (!read(FhUtil.ptr_at<byte>(GameGates.k_text_wait), out byte tw)) g |= 0x100; else _gs_text_byte = tw;
        _gs_blk ??= new byte[GameGates.k_blk_len];
        GameGates.Text t;
        fixed (byte* b = _gs_blk) {
            if (_fn_safe_read(FhUtil.ptr_at<byte>(GameGates.k_blk), b, (uint)GameGates.k_blk_len) == 0) { g |= 0x100; t = GameGates.Text.None; }
            else t = GameGates.Scan(new ReadOnlySpan<byte>(b, GameGates.k_blk_len), (uint)(nint)FhUtil.ptr_at<byte>(GameGates.k_blk));
        }
        _gs_text = t; _gs_have = true;
        return g | GameGates.Bits(player, _gs_player_ok, 0, 0, _gs_text_byte, t);
    }

    /// <summary>push_player, the field, first person, every 10th scan tick: is the follow pick the game's player actor
    /// ([0x1300788])? A mismatch is logged (two checks in a row, one line per 10 s, the same pair again after 2 min, 50 a
    /// session); the pick is not changed (diagnostic only, for now).</summary>
    private void gates_pick_check(Actor* actors, int pick) {
        if (!gs_player(out uint player)) return;
        nint pa = pick >= 0 ? (nint)(actors + pick) : 0;
        if (!_gs_pick.Step(pa, (nint)player, _now)) return;
        long d = (long)player - (long)(nint)actors; int gi = d >= 0 && d % sizeof(Actor) == 0 && d / sizeof(Actor) < 48 ? (int)(d / sizeof(Actor)) : -1;
        string gn = gi >= 0 && read(actors + gi, out Actor ga) ? I($"actor {gi} '{actor_name(ga.chr_name)}'") : I($"0x{player:X8} (not in the actor table)");
        string m = I($"first person: the follow pick ({(pick >= 0 ? _fp_how : "none")}) is not the game's player actor ({gn}, [0x1300788]) on map {map_name()}: the pick is kept (a diagnostic; checks {_gs_pick.checks}, agree {_gs_pick.agree}, differ {_gs_pick.mismatch}, no control {_gs_pick.no_control}){(_gs_pick.lines == PickCheck.k_lines_max ? " (no more of these lines this session)" : "")}");
        _gs_pick_last = m;
        _logger.Info(m); d7_log(m);
    }

    /// <summary>F8 (the gestures readout): the direct gates as last read, and the movie id read now (UI only).</summary>
    private string gates_readout() {
        if (!_gs_have) return "direct gates: not read yet (they are read with the tap on, in first person, on frames that can draw the hands)";
        int movie_id = 0; bool mid = read(FhUtil.ptr_at<int>(GameGates.k_movie_id), out movie_id);
        var sb = new System.Text.StringBuilder();
        sb.Append(I($"direct gates: player actor {(_gs_player_ok ? (_gs_player != 0 ? I($"0x{_gs_player:X8} (control)") : "0 (no control)") : "not readable")}"));
        sb.Append(I($" | field menu {_gs_menu} | movie skippable {_gs_movie}, id {(mid ? movie_id.ToString(CI) : "?")}"));
        sb.Append(I($" | text byte 0x{_gs_text_byte:X2}, windows open 0x{_gs_text.open:X2} shown 0x{_gs_text.shown:X2}"));
        sb.Append(_gs_text.choice_slot >= 0 ? I($" | CHOICE BOX: slot {_gs_text.choice_slot}, row {_gs_text.choice_row}") : " | no choice box");
        if (_gs_text.odd != 0) sb.Append(I($" | pointers outside the pools 0x{_gs_text.odd:X4}"));
        sb.Append(I($" | follow pick check: agree {_gs_pick.agree}, differ {_gs_pick.mismatch}, no control {_gs_pick.no_control} of {_gs_pick.checks}"));
        return sb.ToString();
    }
}

/// <summary>The direct gates' pure part (offsets, the window / cursor scan, the bits). Kept free of game types for the scratch test.</summary>
internal static class GameGates {
    public const int k_player = 0xF00788, k_menu = 0xF40824, k_movie_skip = 0xD2A008, k_movie_id = 0x840E28, k_text_wait = 0x85A03C;
    public const int k_blk = 0x14663F0, k_blk_len = 0x26C0;           // VA 0x18663F0 .. 0x1868AB0
    public const int k_win_tab = 0x1300, k_cur_lo = 0x1320, k_cur_tab = 0x26A0;   // in the block: the window pointers, the first cursor, the cursor pointers
    public const int k_win_len = 0x130, k_cur_len = 0x138;
    public struct Text {
        public int open, shown;          // bit per slot: window open (+0x00 == 1); open and shown (+0x2C == 2)
        public int choice;               // bit per slot: a choice cursor up (+0x01 == 1) on a window in state 1 or 2
        public int choice_slot, choice_row;   // the first such slot (-1: none) and its highlighted row
        public int odd;                  // bit per slot (windows 0..7, cursors 8..15): a non-null pointer outside its pool (ignored)
        public static Text None => new Text { choice_slot = -1 };
    }
    static uint u32(ReadOnlySpan<byte> b, int o) => (uint)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24);
    static int u16(ReadOnlySpan<byte> b, int o) => b[o] | b[o + 1] << 8;
    /// <summary>The block as read (k_blk_len bytes from blk_va): which windows are open / shown, and a choice cursor.</summary>
    public static Text Scan(ReadOnlySpan<byte> b, uint blk_va) {
        var t = Text.None;
        if (b.Length < k_blk_len) return t;
        Span<int> st = stackalloc int[8];
        for (int i = 0; i < 8; i++) {
            st[i] = -1;
            uint p = u32(b, k_win_tab + i * 4);
            long o = (long)p - blk_va;
            if (o < 0 || o + k_win_len > k_win_tab) { if (p != 0) t.odd |= 1 << i; continue; }
            int open = u16(b, (int)o), s = u16(b, (int)o + 0x2C);
            st[i] = s;
            if (open == 1) { t.open |= 1 << i; if (s == 2) t.shown |= 1 << i; }
        }
        for (int i = 0; i < 8; i++) {
            uint p = u32(b, k_cur_tab + i * 4);
            long o = (long)p - blk_va;
            if (o < k_cur_lo || o + k_cur_len > k_cur_tab) { if (p != 0) t.odd |= 1 << (8 + i); continue; }
            if (b[(int)o + 1] != 1 || (st[i] != 1 && st[i] != 2)) continue;
            t.choice |= 1 << i;
            if (t.choice_slot < 0) { t.choice_slot = i; t.choice_row = (sbyte)b[(int)o + 2]; }
        }
        return t;
    }
    /// <summary>The direct bits: 0x40 no player control (read and 0), 0x80 a field menu, 0x2000 a movie, 0x800 a choice box,
    /// 0x1000 a text box waiting (the byte set and a window open and shown; the byte alone is 0xFF from the start).</summary>
    public static int Bits(uint player, bool player_ok, uint menu, uint movie, byte text_byte, in Text t) {
        int g = 0;
        if (player_ok && player == 0) g |= 0x40;
        if (menu != 0) g |= 0x80;
        if (movie != 0) g |= 0x2000;
        if (t.choice != 0) g |= 0x800;
        if (text_byte != 0 && t.shown != 0) g |= 0x1000;
        return g;
    }
}

/// <summary>The follow pick against the game's player actor (pure; gates_pick_check). Step returns true when a mismatch line
/// is due: the second mismatching check in a row, 10 s after the last line (2 min for the same pair), 50 lines a session.</summary>
internal struct PickCheck {
    public const int k_lines_max = 50; public const double k_gap = 10.0, k_same_gap = 120.0;
    public int checks, agree, mismatch, no_control, lines;
    int _streak; nint _lp, _lpl; double _last_t;
    public bool Step(nint pick, nint player, double now) {
        checks++;
        if (player == 0) { no_control++; _streak = 0; return false; }
        if (pick == player) { agree++; _streak = 0; return false; }
        mismatch++; _streak++;
        if (_streak < 2 || lines >= k_lines_max) return false;
        bool same = pick == _lp && player == _lpl;
        if (lines > 0 && now - _last_t < (same ? k_same_gap : k_gap)) return false;
        _lp = pick; _lpl = player; _last_t = now; lines++;
        return true;
    }
}
