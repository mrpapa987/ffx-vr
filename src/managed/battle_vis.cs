// SPDX-License-Identifier: MIT

namespace FFXVR;

// Battle visibility, build 1 (branch battle-vis, ABI 68; design-notes "Battle visibility, build 1"; the rules and every line's
// text are native: ffxvr_battlevis.h, unit-tested). Two parts.
//
// 1. The battle research log (F8 > Developer > Inspect > "Battle research log", ini diag_battle, default 0): ffxvr_diag.log
//    gets, per battle,
//      cmd:  one line per executed command: who, the command's id and name (and the menu it came from), its kind, its targets.
//            From a detour on the game's MsParseCommand; a per-frame poll of every battle actor's "commands executed" count
//            reports what the detour missed, marked "polled". A change of the field roster (a swap, a summon, a dismissal)
//            gets its own cmd: line.
//      vis:  every change of a battle actor's visibility state: the visible flag, the four request votes, the character's hide
//            byte, its alpha, with who is acting; and changes of the two globals effect scripts and cameras hide through.
//      fx:   per magic file: its id, the game's effect camera / billboard matrices next to the eyes' own views, the frame's
//            scene-pass draw counts by eye-image kind.
//    Everything is read (guarded copies, or the exe's own statics); nothing is written to the game. The one hook, the
//    MsParseCommand detour, is inserted only while diag_battle is on (the first frame it is: never by developer mode alone,
//    never by the "Verbose" logging profile). The detour always calls the game's function with its own argument and returns
//    its result; with diag_battle off again it does nothing else (it cannot be taken out before the game closes).
//    "Log the command table" writes every command, item and monster-command id with its name once (cmdtab: lines).
// 2. The first-person battle body hide by identity (View > First person > "Your body in battle", ini fp_battle_hide, default 1):
//    in a first-person battle the followed character's draws are found by his own model's index-buffer keys (IbChain, as the
//    field path does) instead of by distance, so an aeon or a neighbour near the camera is never hidden with him. This file
//    builds the key table (his keys, and every other live actor's: for the same-model tie-break and the log); native decides.
//    Who is followed during a summon: the summoner (the battle follow's rule, unchanged), so the aeon is another actor and its
//    draws are never the followed character's. Nothing is pushed outside first person, so nothing is hidden in the diorama.
//
// Game facts (FhUtil offsets = VA - 0x400000; ffx-rust's symbols/ffx.sym and docs/formats/battle.md / kernel.md, static
// research of the same FFX.exe, and Fahrenheit's names; facts only). Not seen in the running game by us unless said:
//   0x7AE370 MsParseCommand(cue): cdecl, one argument, returns int (Fahrenheit's FhCall table: int d_MsParseCommand(byte*)).
//     cue +0 the actor, +2 the group in play, +3 the group count, groups of 0x10 bytes from +8: two u16 ids, +8 target bits.
//     After it: [0x112C8D8] the command it parsed, actor +0xF5C the same, +0x6DF the actor's count of commands (+1, to 255).
//   0x79A4B0 MsGetComData(id, &strings): cdecl, returns the kernel record of a command / item / monster command id (the top
//     nibble: 3 / 2 / 4, 6); record +0 the name's offset in strings, +0x15 the animation kind, +0x18 the sub-menu, u16 +0x1A
//     target flags, u32 +0x1C flags. Called only inside the detour, on the game's own thread, for the id the game just parsed.
//   The kernel tables ([0x112A92C] command, [0x112A940] item, [0x112A930] / [0x112A934] monster commands): u16 segment count,
//     12-byte segments from +8 (first id, last id, record size, data length, u32 data offset), strings after the data. Walked
//     with guarded reads for the polled lines and the table dump.
//   [0x11334D4] the battle actor records 0..0x1E, 0xF90 each (MsGetChr); [0x1134468] the monsters' pointer (compared, logged).
//   0x112A8E0 g_BtlStatus, 0xC64CA0 / 0xC64CAC the magic file's id / handle (-1 none), 0xC64CDC g_MagicHiddenBtlChr,
//   0x112C9FE g_BtlCameraHides, 0x112C9E0 g_BtlCommandActor, 0x112AC70 the command cue (0x48 each), 0x112BDE1 its count.
//   The effect code's matrices (4 x 4 floats): 0x230FF60 g_pppCameraMatrix, 0x230FF20 g_pppViewMatrix, 0x230FE60
//   g_pppScreenMatrix, 0x230FEA0 g_pppFpWoodMatrix, 0x230FEE0 g_pppWoodMatrixWork, 0x230FDE0 g_pppWoodMatrix.
public sealed unsafe partial class FFXVRModule {

    private bool _diag_battle;            // ini diag_battle (default 0; Developer > Inspect): the battle research log
    private int  _fp_battle_hide = 1;     // ini fp_battle_hide: 1 by the followed model's keys (default), 0 the old distance rule
    private delegate* unmanaged[Cdecl]<uint, int> _fn_fp_battle_due;                                             // ABI 68
    private delegate* unmanaged[Cdecl]<uint*, int, uint, int, float*, byte*, int, int, byte*, int> _fn_fp_battle_set;   // ABI 68
    private delegate* unmanaged[Cdecl]<byte*, int, int*, int, float*, uint*, int, int> _fn_bvis_frame;           // ABI 68
    private delegate* unmanaged[Cdecl]<byte*, int, void> _fn_bvis_cmd;                                           // ABI 68
    private delegate* unmanaged[Cdecl]<int*, int, int> _fn_bvis_status;                                          // ABI 68

    // = native bvis:: k_actors, sizeof(ActorIn), sizeof(CmdEv), G_COUNT (ffxvr_battlevis.h's static_asserts; bvis_chk/check.py)
    private const int k_bv_actors = 31, k_bv_rec = 48, k_bv_ev = 104, k_bvg_n = 11;
    private const int k_bv_chr_size = 0xF90;
    private const int k_bv_parse = 0x3AE370;     // VA 0x7AE370 MsParseCommand
    private const int k_bv_comdata = 0x39A4B0;   // VA 0x79A4B0 MsGetComData
    private const int k_bv_chrs = 0xD334D4, k_bv_mons = 0xD34468;   // VA 0x11334D4, 0x1134468
    private const int k_bv_state = 0xD2A8E0;     // VA 0x112A8E0
    private const int k_bv_magic_id = 0x864CA0, k_bv_magic_handle = 0x864CAC, k_bv_magic_hidden = 0x864CDC;   // VA 0xC64CA0, 0xC64CAC, 0xC64CDC
    private const int k_bv_cam_hides = 0xD2C9FE, k_bv_cmd_actor = 0xD2C9E0, k_bv_cue_count = 0xD2BDE1, k_bv_cue = 0xD2AC70, k_bv_parsed = 0xD2C8D8;   // VA 0x112C9FE, 0x112C9E0, 0x112BDE1, 0x112AC70, 0x112C8D8
    private static readonly int[] k_bv_mats = { 0x1F0FF60, 0x1F0FF20, 0x1F0FE60, 0x1F0FEA0, 0x1F0FEE0, 0x1F0FDE0 };   // = native bvis::mat_name's order
    private static readonly int[] k_bv_tables = { 0xD2A92C, 0xD2A940, 0xD2A930, 0xD2A934 };   // command, item, monster commands 1 / 2
    private static readonly string[] k_bv_table_name = { "command (ids 0x3...)", "item (ids 0x2...)", "monster commands 1 (ids 0x4...)", "monster commands 2 (ids 0x6...)" };
    private static readonly int[] k_bv_table_top = { 3, 2, 4, 6 };

    // the detour's events: a ring the game's thread writes and the render thread reads (bytes; one writer, one reader)
    //   +0 actor, +1 group, +2 groups, +3 exe count, +4 record read (2), +5 exe count read, u16 +8 id, +10 list0, +12 list1,
    //   u32 +16 targets, +20 the record's first 0x30 bytes, +68 the name (game encoding), +108 the first id's name
    private const int k_bv_ring = 16, k_bv_slot = 152;
    private byte* _bv_ring; private int _bv_w, _bv_r, _bv_busy, _bv_calls, _bv_faults, _bv_dropped, _bv_text_faults;
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] private delegate int d_bv_parse(byte* cue);
    private d_bv_parse? _h_bv_parse, _o_bv_parse; private nint _a_bv_parse, _a_bv_comdata;
    private int _bv_hook;   // 0 not tried, 1 inserted, 3 refused / failed, 4 FFX.exe not found, 5 no native helper
    private string _bv_hook_status = "not inserted (it is inserted the first frame diag_battle is on)";

    // the render thread's state
    private bool _bv_active; private byte* _bv_buf, _bv_in; private float* _bv_mats; private int _bv_tick_faults, _bv_snap_miss;
    private readonly long[] _bv_name_key = new long[k_bv_actors]; private readonly byte[] _bv_names = new byte[k_bv_actors * 32];
    private int _bv_dump_t = -1, _bv_dump_id, _bv_dump_lines;
    private readonly int[] _bv_st = new int[14];

    // the battle body hide's key table
    private const int k_bh_max = 768, k_bh_segs = 96, k_bh_owners = 48;   // = native bvis::k_keys, k_owners
    private uint* _bh_q; private byte* _bh_seg, _bh_who, _bh_own; private float* _bh_roots;   // _bh_own: two bytes per owner (its table slot; bit 0 = attached to the followed actor)
    private int _bh_n, _bh_self_n, _bh_owners, _bh_mode, _bh_hidden, _bh_dumps; private uint _bh_gen; private bool _bh_incomplete; private nint _bh_who_key; private ulong _bh_logged;
    private readonly int[] _bh_owner_slot = new int[k_bh_owners];

    private bool bv_parse(string k, string v) {
        switch (k) {
            case "diag_battle":    pb(k, v, ref _diag_battle); return true;
            case "fp_battle_hide": pi(k, v, ref _fp_battle_hide, 0, 1); return true;
            default: return false;
        }
    }
    private string bv_ini_text() => $"diag_battle={(_diag_battle ? 1 : 0)}\nfp_battle_hide={_fp_battle_hide.ToString(CI)}\n";
    private void bv_bind(Func<string, nint> req) {
        _fn_fp_battle_due = (delegate* unmanaged[Cdecl]<uint, int>)req("ffxvr_fp_battle_due");
        _fn_fp_battle_set = (delegate* unmanaged[Cdecl]<uint*, int, uint, int, float*, byte*, int, int, byte*, int>)req("ffxvr_fp_battle_set");
        _fn_bvis_frame = (delegate* unmanaged[Cdecl]<byte*, int, int*, int, float*, uint*, int, int>)req("ffxvr_bvis_frame");
        _fn_bvis_cmd = (delegate* unmanaged[Cdecl]<byte*, int, void>)req("ffxvr_bvis_cmd");
        _fn_bvis_status = (delegate* unmanaged[Cdecl]<int*, int, int>)req("ffxvr_bvis_status");
    }
    private void bv_unbind() { _fn_fp_battle_due = null; _fn_fp_battle_set = null; _fn_bvis_frame = null; _fn_bvis_cmd = null; _fn_bvis_status = null; }

    // -- the MsParseCommand detour -------------------------------------------------------------------------------------------

    /// Render thread, once, the first frame diag_battle is on (from bv_tick; a late insertion, bracketed like sfast_install's):
    /// the detour on the game's MsParseCommand. Never at module init and never for developer mode alone.
    private void bv_install() {
        if (_bv_hook != 0) return;
        if (!_native_ready || _fn_bvis_cmd == null || _fn_safe_read == null) { _bv_hook = 5; _bv_hook_status = "not inserted: the native helper did not load"; return; }
        if (_bv_ring == null) {
            _bv_ring = (byte*)Marshal.AllocHGlobal(k_bv_ring * k_bv_slot);   // kept for the session
            for (int i = 0; i < k_bv_ring * k_bv_slot; i++) _bv_ring[i] = 0;
        }
        _h_bv_parse = h_bv_parse;
        new FhCallLocation("FFX.exe", k_bv_parse).try_resolve(out _a_bv_parse);
        new FhCallLocation("FFX.exe", k_bv_comdata).try_resolve(out _a_bv_comdata);
        if (_a_bv_parse == 0) { _bv_hook = 4; _bv_hook_status = "not inserted: FFX.exe not found"; _logger.Warning("battle log: " + _bv_hook_status); return; }
        // review N6: the function's first bytes, read before the insertion and logged (they are not known from the notes, so
        // nothing is compared yet: the next build can check them, as the damage-number and fast-sphere detours do)
        string first = "";
        try { for (int i = 0; i < 8; i++) first += FhUtil.get_at<byte>(k_bv_parse + i).ToString("X2", CI) + (i < 7 ? " " : ""); } catch { first = "not read"; }
        bool ok = false; string why = "";
        diag7_insert_begin();   // no main-thread detour resolves its original while this writes Fahrenheit's table (B7-1)
        try { ok = new FhCallHandle<d_bv_parse>(new FhCallLocation((void*)_a_bv_parse)).hook(this, _h_bv_parse!); }
        catch (Exception e) { why = e.GetType().Name; _logger.Error($"battle log: the detour: {e}"); }
        finally { diag7_insert_end(); }
        d_bv_parse? o = null;
        if (ok) { try { o = d7_chain(_a_bv_parse, _h_bv_parse!); } catch { } }
        if (o != null) System.Threading.Volatile.Write(ref _o_bv_parse, o);
        _bv_hook = ok ? 1 : 3;
        _bv_hook_status = ok ? I($"inserted at 0x{_a_bv_parse:X8} (MsParseCommand 0x7AE370, its first bytes before the insertion: {first}; the insertion stops the game's threads for a moment: one hitch, once a session){(o == null ? "; its original resolves at its first call" : "")}")
                             : "NOT inserted: Fahrenheit refused the insertion" + (why.Length > 0 ? " (" + why + ")" : " (another hook on 0x7AE370? core.log)") + ": the cmd: lines are polled only (its first bytes: " + first + ")";
        _logger.Info("battle log: the MsParseCommand detour: " + _bv_hook_status); d7_log("cmd: the MsParseCommand detour: " + _bv_hook_status);
    }

    /// The game's MsParseCommand (0x7AE370; the game's main thread, once per executed command). The original always runs, with
    /// the game's own argument, and its result is returned unchanged. With diag_battle on, the cue's fields are copied before
    /// it and the parsed command's record and name after it, into the ring; no string, no log, nothing written to the game.
    /// Never throws into the caller.
    private int h_bv_parse(byte* cue) {
        d_bv_parse o = System.Threading.Volatile.Read(ref _o_bv_parse) ?? d7_resolve_main(ref _o_bv_parse, _a_bv_parse, _h_bv_parse!);
        if (!_diag_battle || cue == null || _bv_ring == null) return o(cue);
        _bv_calls++;
        byte* ev = stackalloc byte[k_bv_slot];
        bool have = false;
        try { have = bv_cue_read(cue, ev); } catch { _bv_faults++; }
        System.Threading.Volatile.Write(ref _bv_busy, 1);   // the poll waits: this command's event is not in the ring yet
        int r = o(cue);
        try { if (have) bv_hook_after(ev); } catch { _bv_faults++; }
        System.Threading.Volatile.Write(ref _bv_busy, 0);
        return r;
    }
    /// Before the original: the cue entry's actor, group and ids (one guarded copy of its 0x48 bytes).
    private bool bv_cue_read(byte* cue, byte* ev) {
        var rd = _fn_safe_read;
        if (rd == null) return false;
        for (int i = 0; i < k_bv_slot; i++) ev[i] = 0;
        byte* c = stackalloc byte[0x48];
        if (rd(cue, c, 0x48) == 0) return false;
        ev[0] = c[0]; ev[1] = c[2]; ev[2] = c[3];
        if (c[2] <= 3) {
            byte* g = c + 8 + c[2] * 0x10;
            *(ushort*)(ev + 10) = *(ushort*)g; *(ushort*)(ev + 12) = *(ushort*)(g + 2); *(uint*)(ev + 16) = *(uint*)(g + 8);
        }
        else ev[1] = 0xFF;
        return true;
    }
    /// After the original (the game's thread): the command the game parsed, the actor's command count and targets, the
    /// record and the names through the game's own lookup; then the event is published.
    private void bv_hook_after(byte* ev) {
        var rd = _fn_safe_read;
        if (rd == null) return;
        int id = (int)(FhUtil.get_at<uint>(k_bv_parsed) & 0xFFFF);
        *(ushort*)(ev + 8) = (ushort)id;
        nint chrs = FhUtil.get_at<nint>(k_bv_chrs);
        if (chrs != 0 && ev[0] < k_bv_actors) {
            byte* a = (byte*)chrs + ev[0] * k_bv_chr_size; byte n = 0; uint t = 0;
            if (rd(a + 0x6DF, &n, 1) != 0) { ev[3] = n; ev[5] = 1; }
            if (rd(a + 0xDC0, &t, 4) != 0 && t != 0) *(uint*)(ev + 16) = t;   // the targets MsParseCommand itself read
        }
        var f = (delegate* unmanaged[Cdecl]<int, byte**, byte*>)_a_bv_comdata;
        if (f != null) {
            if (bv_comdata(f, id, ev + 20, ev + 68)) ev[4] = 2;
            int l0 = *(ushort*)(ev + 10);
            if (l0 != id) bv_comdata(f, l0, null, ev + 108);
        }
        int w = _bv_w;
        if (w - System.Threading.Volatile.Read(ref _bv_r) >= k_bv_ring) { _bv_dropped++; return; }   // the render thread is not reading (16 commands behind)
        byte* slot = _bv_ring + (w & (k_bv_ring - 1)) * k_bv_slot;
        for (int i = 0; i < k_bv_slot; i++) slot[i] = ev[i];
        System.Threading.Volatile.Write(ref _bv_w, w + 1);
    }
    /// The game's own lookup, only for an id whose top nibble names a kernel table (2 items, 3 commands, 4 / 6 monster
    /// commands: the ids MsParseCommand itself resolves). rec gets the record's first 0x30 bytes, name its name's bytes.
    private bool bv_comdata(delegate* unmanaged[Cdecl]<int, byte**, byte*> f, int id, byte* rec, byte* name) {
        int kind = id >> 12;
        if (kind != 2 && kind != 3 && kind != 4 && kind != 6) return false;
        if (FhUtil.get_at<nint>(k_bv_tables[kind == 3 ? 0 : kind == 2 ? 1 : kind == 4 ? 2 : 3]) == 0) return false;   // (review N5: that kind's kernel table is loaded)
        byte* strings = null;
        byte* r = f(id, &strings);
        return bv_rec_copy(r, strings, rec, name);
    }
    /// A kernel record and its name, by guarded copies (either destination may be null). The name is 40 bytes at most, in
    /// the game's text encoding, always terminated.
    private bool bv_rec_copy(byte* r, byte* strings, byte* rec, byte* name) {
        var rd = _fn_safe_read;
        if (rd == null || r == null) return false;
        byte* tmp = stackalloc byte[0x30];
        if (rd(r, tmp, 0x30) == 0) return false;
        if (rec != null) for (int i = 0; i < 0x30; i++) rec[i] = tmp[i];
        if (name != null) {
            for (int i = 0; i < 40; i++) name[i] = 0;
            if (strings != null) {
                byte* s = strings + *(ushort*)tmp;
                if (rd(s, name, 40) == 0) for (int i = 0; i < 40; i++) { name[i] = 0; if (rd(s + i, name + i, 1) == 0 || name[i] == 0) break; }   // (a name can end a region)
            }
            name[39] = 0;
        }
        return true;
    }
    /// Our own walk of a kernel table for an id (guarded reads; the layout from ffx-rust's kernel.md): the polled lines and the
    /// table dump, where the game's lookup is not called (the render thread).
    private bool bv_kernel(int id, byte* rec, byte* name) {
        var rd = _fn_safe_read;
        if (rd == null) return false;
        int kind = id >> 12, t = kind == 3 ? 0 : kind == 2 ? 1 : kind == 4 ? 2 : kind == 6 ? 3 : -1;
        if (t < 0) return false;
        nint tbl = FhUtil.get_at<nint>(k_bv_tables[t]);
        if (tbl == 0) return false;
        byte* h = stackalloc byte[56];
        if (rd((void*)tbl, h, 56) == 0) return false;
        int segs = *(short*)h, low = id & 0xFFF;
        if (segs > 4) segs = 4;
        for (int s = 0; s < segs; s++) {
            byte* d = h + 8 + 12 * s;
            int first = *(ushort*)d, last = *(ushort*)(d + 2), size = *(ushort*)(d + 4), len = *(ushort*)(d + 6); uint off = *(uint*)(d + 8);
            if (size == 0 || low < first || low > last) continue;
            return bv_rec_copy((byte*)tbl + off + size * (low - first), (byte*)tbl + off + len, rec, name);
        }
        return false;
    }

    // -- the render thread ---------------------------------------------------------------------------------------------------

    /// A name in the game's text encoding (src, 40 bytes at most) to printable ASCII in dst (40 bytes, terminated): Fahrenheit's
    /// decoder; if it throws, the plain characters by BvText.Ascii.
    private void bv_text(byte* src, byte* dst) {
        int n = 0;
        while (n < 40 && src[n] != 0) n++;
        dst[0] = 0;
        if (n == 0) return;
        byte* u = stackalloc byte[256];
        int m = -1, k = 0;
        try { m = FhEncoding.decode(new ReadOnlySpan<byte>(src, n < 40 ? n + 1 : n), new Span<byte>(u, 256), flags: FhEncodingFlags.IMPLICIT_END); }
        catch { m = -1; _bv_text_faults++; }
        if (m > 0) { for (int i = 0; i < m && i < 256 && k < 39; i++) { byte c = u[i]; dst[k++] = c >= 32 && c < 127 ? c : (byte)'?'; } }
        else k = BvText.Ascii(new ReadOnlySpan<byte>(src, n), new Span<byte>(dst, 39));
        dst[k] = 0;
    }
    /// The record's fields of a bvis::CmdEv: +5 the animation kind (record +0x15), +7 the sub-menu (+0x18), u16 +14 the target
    /// flags (+0x1A), u32 +20 the flags (+0x1C); +6 how it was read (0 not, 1 our table walk, 2 the game's lookup).
    private static void bv_ev_record(byte* ev, int how, byte* rec) {
        ev[6] = (byte)how;
        if (how == 0) return;
        ev[5] = rec[0x15]; ev[7] = rec[0x18]; *(ushort*)(ev + 14) = *(ushort*)(rec + 0x1A); *(uint*)(ev + 20) = *(uint*)(rec + 0x1C);
    }
    /// The detour's events, in order, to native (the cmd: lines).
    private void bv_drain() {
        if (_bv_ring == null || _fn_bvis_cmd == null) return;
        int w = System.Threading.Volatile.Read(ref _bv_w);
        byte* ev = stackalloc byte[k_bv_ev];
        for (int guard = 0; _bv_r != w && guard < k_bv_ring; guard++) {
            byte* e = _bv_ring + (_bv_r & (k_bv_ring - 1)) * k_bv_slot;
            for (int i = 0; i < k_bv_ev; i++) ev[i] = 0;
            ev[0] = 0; ev[1] = e[0]; ev[2] = e[3]; ev[3] = e[1]; ev[4] = e[2];
            bv_ev_record(ev, e[4], e + 20);
            *(ushort*)(ev + 8) = *(ushort*)(e + 8); *(ushort*)(ev + 10) = *(ushort*)(e + 10); *(ushort*)(ev + 12) = *(ushort*)(e + 12);
            *(uint*)(ev + 16) = *(uint*)(e + 16);
            bv_text(e + 68, ev + 24); bv_text(e + 108, ev + 64);
            System.Threading.Volatile.Write(ref _bv_r, _bv_r + 1);
            _fn_bvis_cmd(ev, k_bv_ev);
        }
    }
    /// A command the detour did not report (native's poll asked): its record and names by our own table walk, its cue entry if
    /// it is still queued; back to native as a polled event.
    private void bv_polled(int actor, int id, int count, uint targets) {
        if (_fn_bvis_cmd == null) return;
        byte* ev = stackalloc byte[k_bv_ev]; byte* rec = stackalloc byte[0x30]; byte* nm = stackalloc byte[40];
        for (int i = 0; i < k_bv_ev; i++) ev[i] = 0;
        ev[0] = 1; ev[1] = (byte)actor; ev[2] = (byte)count; ev[3] = 0xFF;
        *(ushort*)(ev + 8) = (ushort)id; *(uint*)(ev + 16) = targets;
        int nc = FhUtil.get_at<sbyte>(k_bv_cue_count);
        if (nc > 60) nc = 60;
        byte* cues = FhUtil.ptr_at<byte>(k_bv_cue);   // the game's command cue (the exe's own statics, 0x48 bytes each)
        for (int k = 0; k < nc; k++) {
            byte* c = cues + k * 0x48;
            if (c[0] != actor || c[2] > 3) continue;
            byte* g = c + 8 + c[2] * 0x10;
            ev[3] = c[2]; ev[4] = c[3]; *(ushort*)(ev + 10) = *(ushort*)g; *(ushort*)(ev + 12) = *(ushort*)(g + 2);
            break;
        }
        if (bv_kernel(id, rec, nm)) { bv_ev_record(ev, 1, rec); bv_text(nm, ev + 24); }
        int l0 = *(ushort*)(ev + 10);
        if (l0 != id && l0 != 0 && bv_kernel(l0, null, nm)) bv_text(nm, ev + 64);
        _fn_bvis_cmd(ev, k_bv_ev);
    }

    /// From push_all, every frame (after push_player: the follow is this frame's). Off: one test. On, out of battle: one
    /// static read. In battle: one guarded read of the battle actor records (122 KB), the frame's actor-table snapshot, a
    /// handful of the exe's statics; native writes the lines.
    private void bv_tick() {
        if (!_diag_battle) {
            if (_bv_active) bv_stop();
            System.Threading.Volatile.Write(ref _bv_r, System.Threading.Volatile.Read(ref _bv_w));   // (review N11: events left in the ring are not the next logged battle's)
            return;
        }
        if (_fn_bvis_frame == null || _fn_bvis_cmd == null || _fn_safe_read == null || _fn_diag_line == null) return;
        if (_bv_hook == 0 && _native_ready) bv_install();
        try { bv_dump_step(); bv_frame(); }
        catch { _bv_tick_faults++; }
    }
    private void bv_stop() {
        _bv_active = false;
        if (_fn_bvis_frame != null) _fn_bvis_frame(null, 0, null, 0, null, null, 0);   // ends the battle's log (its summary line)
    }
    private void bv_frame() {
        sbyte bst = FhUtil.get_at<sbyte>(k_bv_state);
        if (bst == 0) { if (_bv_active) bv_stop(); bv_drain(); return; }
        if (_bv_buf == null) {   // kept for the session
            _bv_buf = (byte*)Marshal.AllocHGlobal(k_bv_actors * k_bv_chr_size); _bv_in = (byte*)Marshal.AllocHGlobal(k_bv_actors * k_bv_rec);
            _bv_mats = (float*)Marshal.AllocHGlobal(k_bv_mats.Length * 16 * sizeof(float));
        }
        _bv_active = true;
        nint chrs = FhUtil.get_at<nint>(k_bv_chrs), mons = FhUtil.get_at<nint>(k_bv_mons);
        int n = 0, gflags = 0;
        if (chrs != 0) {
            if (_fn_safe_read((void*)chrs, _bv_buf, (uint)(k_bv_actors * k_bv_chr_size)) != 0) n = k_bv_actors;
            else if (_fn_safe_read((void*)chrs, _bv_buf, (uint)(0x14 * k_bv_chr_size)) != 0) { n = 0x14; gflags |= 16; }
        }
        if (mons != 0 && chrs != 0) gflags |= mons == chrs + 0x14 * k_bv_chr_size ? 4 : 8;
        Actor* actors = Globals.actors; Actor* snap = tbl_snap(actors);
        if (snap == null) { _bv_snap_miss++; bv_drain(); return; }   // (review N10: the actor table did not read this frame: no states are sent, rather than every character "not in the table")
        for (int i = 0; i < k_bv_actors * k_bv_rec; i++) _bv_in[i] = 0;
        for (int i = 0; i < n; i++) {
            byte* c = _bv_buf + i * k_bv_chr_size; byte* o = _bv_in + i * k_bv_rec;
            nint ap = (nint)(*(uint*)c);
            if (ap == 0) { bv_name(i, 0, c, snap, -1, o + 16); continue; }   // no character: its name only (a party member on the bench, an aeon not called)
            o[0] = 1;
            for (int k = 0; k < 5; k++) o[1 + k] = c[0x15 + k];
            o[8] = c[0x6DF]; *(ushort*)(o + 10) = *(ushort*)(c + 0xF5C); *(uint*)(o + 12) = *(uint*)(c + 0xDC0);
            int slot = wpn_slot(actors, ap);
            o[9] = (byte)(slot >= 0 ? slot : 0xFF);
            byte fl = 0, hide = 0;
            if (slot >= 0 && snap != null) {
                byte* a = (byte*)(snap + slot);
                hide = a[0x180];
                if (*(uint*)(a + 0x354) == 0) fl |= 1;             // the alpha word is 0 (a float 0: not drawn)
                if ((*(uint*)(a + 0x198) & 0x20) != 0) fl |= 2;    // the model is released
                if ((*(uint*)(a + 0x194) & 0x2000) != 0) fl |= 8;  // bit 13: Ch_DrawEnEffect (effect scripts; its effect on the draw is not confirmed)
            }
            else fl |= 4;                                          // the character is not one of the 48 table slots (or the table did not read)
            o[6] = hide; o[7] = fl;
            bv_name(i, ap, c, snap, slot, o + 16);
        }
        int* g = stackalloc int[k_bvg_n];
        g[0] = bst; g[1] = FhUtil.get_at<int>(k_bv_magic_id); g[2] = FhUtil.get_at<int>(k_bv_magic_handle); g[3] = FhUtil.get_at<int>(k_bv_magic_hidden);
        g[4] = FhUtil.get_at<byte>(k_bv_cam_hides); g[5] = FhUtil.get_at<byte>(k_bv_cmd_actor); g[6] = FhUtil.get_at<sbyte>(k_bv_cue_count);
        g[7] = FhUtil.get_at<byte>(k_roster_off) | FhUtil.get_at<byte>(k_roster_off + 1) << 8 | FhUtil.get_at<byte>(k_roster_off + 2) << 16;
        g[8] = _btl_follow_id; g[9] = _fp_mode;
        g[10] = (_bv_hook == 1 ? 1 : 0) | (System.Threading.Volatile.Read(ref _bv_busy) != 0 ? 2 : 0) | gflags;
        float* mats = null;
        if (g[1] != -1 && g[2] != -1) {   // a magic file is loaded: the effect code's matrices (the exe's own statics)
            for (int k = 0; k < k_bv_mats.Length; k++) { float* m = FhUtil.ptr_at<float>(k_bv_mats[k]); for (int j = 0; j < 16; j++) _bv_mats[k * 16 + j] = m[j]; }
            mats = _bv_mats;
        }
        uint* pol = stackalloc uint[32];
        int np = _fn_bvis_frame(_bv_in, n, g, k_bvg_n, mats, pol, 8);
        for (int i = 0; i < np && i < 8; i++) bv_polled((int)pol[4 * i], (int)pol[4 * i + 1], (int)pol[4 * i + 2], pol[4 * i + 3]);
        bv_drain();
    }
    /// A battle actor's name for the lines, into o (32 bytes, ASCII): the record's own name (+0x540, the game's encoding; for
    /// monsters that this is their name is not confirmed) and the character's model name. Kept per actor while the character,
    /// the model id and the name's first bytes are the same.
    private void bv_name(int i, nint ap, byte* c, Actor* snap, int slot, byte* o) {
        long key = (long)(uint)ap | (long)(*(uint*)(c + 4)) << 32;
        key ^= (long)(*(uint*)(c + 0x540)) * 31L;
        if (key == 0) key = 1;
        if (key != _bv_name_key[i]) {
            _bv_name_key[i] = key;
            byte* t = stackalloc byte[40];
            bv_text(c + 0x540, t);
            string s = Marshal.PtrToStringAnsi((nint)t) ?? "";
            string m = slot >= 0 && snap != null ? actor_name(snap[slot].chr_name) : "";
            string full = s.Length > 0 && m.Length > 0 ? s + " (" + m + ")" : s.Length > 0 ? s : m;
            for (int k = 0; k < 32; k++) { char ch = k < full.Length && k < 31 ? full[k] : '\0'; _bv_names[i * 32 + k] = ch >= 32 && ch < 127 ? (byte)ch : (byte)0; }
        }
        for (int k = 0; k < 32; k++) o[k] = _bv_names[i * 32 + k];
        o[31] = 0;
    }

    /// "Log the command table": every record of the four kernel tables, id and name, 120 a frame (cmdtab: lines, 2000 at most).
    private void bv_dump_step() {
        if (_bv_dump_t < 0 || _fn_safe_read == null) return;
        if (_bv_dump_t >= k_bv_tables.Length || _bv_dump_lines >= 2000) { d7_log(I($"cmdtab: === end ({_bv_dump_lines} records)")); _bv_dump_t = -1; return; }
        int t = _bv_dump_t;
        nint tbl = FhUtil.get_at<nint>(k_bv_tables[t]);
        byte* h = stackalloc byte[56]; byte* rec = stackalloc byte[0x30]; byte* nm = stackalloc byte[40]; byte* txt = stackalloc byte[40];
        if (tbl == 0 || _fn_safe_read((void*)tbl, h, 56) == 0) { d7_log(I($"cmdtab: table {k_bv_table_name[t]}: not loaded (pointer 0x{(ulong)tbl:X})")); _bv_dump_t++; _bv_dump_id = 0; return; }
        int segs = *(short*)h, done = 0; bool more = false;
        if (_bv_dump_id == 0) d7_log(I($"cmdtab: table {k_bv_table_name[t]}: {segs} segment(s){(segs > 4 ? " (only the first 4 are read: the list is SHORT)" : "")}"));
        if (segs > 4) segs = 4;
        for (int s = 0; s < segs && !more; s++) {
            byte* d = h + 8 + 12 * s;
            int first = *(ushort*)d, last = *(ushort*)(d + 2), size = *(ushort*)(d + 4), len = *(ushort*)(d + 6); uint off = *(uint*)(d + 8);
            if (size == 0 || last < first || last > 0xFFF) continue;
            for (int low = Math.Max(first, _bv_dump_id); low <= last; low++) {
                if (done >= 120) { _bv_dump_id = low; more = true; break; }
                done++; _bv_dump_lines++;
                int id = k_bv_table_top[t] << 12 | low;
                if (!bv_rec_copy((byte*)tbl + off + size * (low - first), (byte*)tbl + off + len, rec, nm)) { d7_log(I($"cmdtab: 0x{id:X4} (record not readable)")); continue; }
                bv_text(nm, txt);
                d7_log(I($"cmdtab: 0x{id:X4} '{Marshal.PtrToStringAnsi((nint)txt) ?? ""}' | anim kind 0x{rec[0x15]:X2}, sub-menu 0x{rec[0x18]:X2}, target flags 0x{*(ushort*)(rec + 0x1A):X4}, flags 0x{*(uint*)(rec + 0x1C):X8}"));
            }
        }
        if (!more) { _bv_dump_t++; _bv_dump_id = 0; }
    }

    // -- the battle body hide's key table ------------------------------------------------------------------------------------

    /// body_hide_push_inner, after ffxvr_fp_others_set, on every frame the field path did not take (render thread). Only in a
    /// first-person battle with the body hide on and the frame going to the headset's 3D view: anything else returns at once
    /// and nothing is pushed (native then judges nothing: the diorama, third person, the virtual screen, the field).
    /// c (game space) = the followed actor's drawn root, or the hide centre when the root is not known.
    private void bh_battle_push(Actor* actors, int pick, bool field, bool used, Vector3 c) {
        if (_fn_fp_battle_set == null || _fn_fp_battle_due == null || field || actors == null || (uint)pick >= 48) return;
        if (!used || _fp_mode != 1 || !_fp_hide_body) return;
        if (_bh_q == null) {   // kept for the session
            _bh_q = (uint*)Marshal.AllocHGlobal(k_bh_max * 16); _bh_seg = (byte*)Marshal.AllocHGlobal(k_bh_segs * 0x6C);
            _bh_roots = (float*)Marshal.AllocHGlobal(k_bh_owners * 3 * sizeof(float)); _bh_who = (byte*)Marshal.AllocHGlobal(32);
            _bh_own = (byte*)Marshal.AllocHGlobal(k_bh_owners * 2);
            _bh_who[0] = 0;
        }
        int flags = (_fp_battle_hide == 1 ? 1 : 0) | (_diag_battle ? 2 : 0);
        Actor* snap = tbl_snap(actors);
        if (snap != null && (nint)snap[pick].chr_name != _bh_who_key) {   // the followed model's name, for native's line
            _bh_who_key = (nint)snap[pick].chr_name;
            string s = actor_name(snap[pick].chr_name);
            for (int k = 0; k < 32; k++) { char ch = k < s.Length && k < 31 ? s[k] : '\0'; _bh_who[k] = ch >= 32 && ch < 127 ? (byte)ch : (byte)0; }
        }
        if (_fp_battle_hide != 1 || snap == null) {   // the distance rule by choice; or the table did not read: no keys this frame
            int r0 = _fn_fp_battle_set(null, 0, _bh_gen, flags, null, null, 0, pick, _bh_who);
            _bh_mode = r0 & 0xFF; _bh_hidden = r0 >> 8; _bh_n = 0; _bh_self_n = 0; _bh_owners = 0;
            return;
        }
        ulong sig = 1469598103934665603UL ^ _fph_scene_gen;
        sig = (sig ^ (ulong)pick) * 1099511628211UL;
        for (int i = 0; i < 48; i++) {
            if (!FphWalk.Live(snap[i].chr_enabled, (nint)snap[i].chr_name)) continue;
            byte* a = (byte*)(snap + i);
            sig = (sig ^ (ulong)i) * 1099511628211UL; sig = (sig ^ (ulong)*(uint*)(a + 0x830)) * 1099511628211UL; sig = (sig ^ (ulong)*(uint*)(a + 0x1C4)) * 1099511628211UL;
        }
        if (_fn_fp_battle_due((uint)(sig ^ (sig >> 32))) != 0) bh_build(actors, snap, pick);
        _bh_roots[0] = -c.X; _bh_roots[1] = -c.Y; _bh_roots[2] = c.Z;   // render space negates X and Y
        for (int k = 1; k < _bh_owners; k++) {
            Vector3 r = *(Vector3*)((byte*)(snap + _bh_owner_slot[k]) + 0x200);
            bool ok = finite(r) && MathF.Abs(r.X) + MathF.Abs(r.Y) + MathF.Abs(r.Z) >= 0.01f;   // (not built: a fresh slot, a parked model)
            _bh_roots[3 * k] = ok ? -r.X : float.NaN; _bh_roots[3 * k + 1] = ok ? -r.Y : float.NaN; _bh_roots[3 * k + 2] = ok ? r.Z : float.NaN;
        }
        int ret = _fn_fp_battle_set(_bh_n > 0 ? _bh_q : null, _bh_n, _bh_gen, flags | (_bh_incomplete ? 8 : 0), _bh_roots, _bh_own, _bh_n > 0 ? _bh_owners : 0, pick, _bh_who);
        _bh_mode = ret & 0xFF; _bh_hidden = ret >> 8;
    }
    /// The key table: the followed actor's segments as owner 0, then every other live actor's with a model, one owner number
    /// each (IbChain.Walk, the field path's walk). No keys from the followed model (not built yet, a read failed), or a model
    /// larger than the walk reads (review S1: IbChain.Walk stops at 16 mesh instances and at k_bh_segs segments a mesh, and
    /// the table at k_bh_max keys: a part of him would be missing from his own keys): no table at all (native: the distance
    /// rule meanwhile; it asks for the next build 4 frames on). An actor attached to the followed one (+0x19C, his weapon)
    /// is flagged: native leaves its draws to the distance rule, as before.
    private void bh_build(Actor* actors, Actor* snap, int pick) {
        _bh_gen++; _bh_n = 0; _bh_self_n = 0; _bh_owners = 1; _bh_incomplete = false; _bh_owner_slot[0] = pick;
        _bh_own[0] = (byte)pick; _bh_own[1] = 0;
        byte* a = (byte*)(snap + pick);
        uint res0 = *(uint*)(a + 0x1C4); int same = 0;
        string why = bh_chain_big(*(nint*)(a + 0x830));
        IbChain.Walk(*(nint*)(a + 0x830), 0u, _bh_q, ref _bh_n, k_bh_max, _bh_seg, k_bh_segs, _fn_safe_read, out bool fail);
        if (why.Length == 0 && _bh_n >= k_bh_max) why = "his model alone fills the key table";
        if (why.Length == 0 && fail) why = "a read of his chain failed";
        if (why.Length == 0 && _bh_n == 0) why = "his model gave no keys yet";
        if (why.Length > 0) { _bh_n = 0; bh_build_log(snap, pick, why, 0); return; }
        _bh_self_n = _bh_n;
        for (int i = 0; i < 48; i++) {
            if (i == pick || !FphWalk.Live(snap[i].chr_enabled, (nint)snap[i].chr_name)) continue;
            byte* b = (byte*)(snap + i);
            if (*(uint*)(b + 0x830) == 0) continue;
            if (_bh_owners >= k_bh_owners || _bh_n >= k_bh_max) { _bh_incomplete = true; break; }   // no room: an actor is missing from the table (native: partial)
            int n0 = _bh_n;
            IbChain.Walk(*(nint*)(b + 0x830), (uint)_bh_owners, _bh_q, ref _bh_n, k_bh_max, _bh_seg, k_bh_segs, _fn_safe_read, out bool f2);
            if (f2 || _bh_n >= k_bh_max) _bh_incomplete = true;
            if (_bh_n == n0) continue;   // no keys: no owner number spent
            if (*(uint*)(b + 0x1C4) == res0) same++;
            _bh_owner_slot[_bh_owners] = i;
            _bh_own[2 * _bh_owners] = (byte)i; _bh_own[2 * _bh_owners + 1] = (byte)(*(nint*)(b + 0x19C) == (nint)(actors + pick) ? 1 : 0);
            _bh_owners++;
        }
        bh_build_log(snap, pick, "", same);
    }
    /// Review S1: is the model object larger than IbChain.Walk reads (more than 16 mesh instances, or a mesh of more than
    /// k_bh_segs segments)? The reason, or "" (also when it does not read: the walk reports that itself).
    private string bh_chain_big(nint h) {
        var rd = _fn_safe_read;
        if (h == 0 || rd == null) return "";
        uint hc = 0; nint arr = 0;
        if (rd((void*)h, &hc, 4) == 0 || rd((byte*)h + 4, &arr, (uint)sizeof(nint)) == 0 || arr == 0) return "";
        int ni = (int)(hc & 0x7FFFFFFF);
        if (ni > 16) return I($"his model has {ni} mesh instances (the walk reads 16)");
        for (int i = 0; i < ni; i++) {
            nint inst = 0, mesh = 0; uint sc = 0;
            if (rd((byte*)arr + i * sizeof(nint), &inst, (uint)sizeof(nint)) == 0 || inst == 0 || rd((void*)inst, &mesh, (uint)sizeof(nint)) == 0 || mesh == 0 || rd((void*)mesh, &sc, 4) == 0) continue;
            int ns = (int)(sc & 0x7FFFFFFF);
            if (ns > k_bh_segs) return I($"a mesh of his model has {ns} segments (the walk reads {k_bh_segs})");
        }
        return "";
    }
    /// The module log's line per new key set (only with the battle log on; 30 a session). why = why there is no table ("": there is one).
    private void bh_build_log(Actor* snap, int pick, string why, int same) {
        if (!_diag_battle || _bh_dumps >= 30) return;
        ulong ks = 1469598103934665603UL ^ (ulong)pick;
        for (int e = 0; e < _bh_self_n * 4; e++) ks = (ks ^ _bh_q[e]) * 1099511628211UL;
        ks = (ks ^ (ulong)_bh_owners) * 1099511628211UL; ks = (ks ^ (ulong)why.Length) * 1099511628211UL;
        if (ks == _bh_logged) return;
        _bh_logged = ks; _bh_dumps++;
        byte* a = (byte*)(snap + pick);
        int his = 0;
        for (int k = 1; k < _bh_owners; k++) if ((_bh_own[2 * k + 1] & 1) != 0) his++;
        _logger.Info(I($"body hide: BATTLE table build {_bh_gen} on map {map_name()}: following actor {pick} '{actor_name(snap[pick].chr_name)}' (+0x830 0x{*(uint*)(a + 0x830):X}, +0x1C4 0x{*(uint*)(a + 0x1C4):X}): {_bh_self_n} keys of his own{(why.Length > 0 ? " (NO TABLE: " + why + "; the distance rule meanwhile, retried)" : "")}, {_bh_n - _bh_self_n} keys of {_bh_owners - 1} other actors, {his} of them attached to him (his weapon: left to the distance rule){(_bh_incomplete ? " (PARTIAL: another actor's chain did not read whole, or the table is full: his keys hide only draws near the camera)" : "")}{(same > 0 ? I($"; {same} other actor(s) on the SAME MODEL resource (the nearest root decides their shared keys)") : "")}") + (_bh_dumps == 30 ? " (no more of these lines this session)" : ""));
    }

    // -- F8 ------------------------------------------------------------------------------------------------------------------

    private const string k_bh_tip = "In a first-person battle your own character's body is hidden. By model: only the draws of your character's own model are hidden, so an aeon or a party member who comes close is never hidden with it (if the model cannot be read yet, the distance rule is used meanwhile). By distance: every character draw at the eyes' anchor is hidden, as before.";
    /// F8 > View > First person.
    private void bh_ui() {
        ImGui.Text("Your body in battle:"); ImGui.SameLine();
        ui_radio("fp_battle_hide", "hide by model (only your own character)##fbh", ref _fp_battle_hide, 1, UiTag.None, k_bh_tip); ImGui.SameLine();
        ui_radio("fp_battle_hide", "hide by distance (the old rule)##fbh", ref _fp_battle_hide, 0, UiTag.None, k_bh_tip);
    }
    private static readonly string[] k_bh_rule = { "off (not a first-person battle)", "distance (the setting)", "distance meanwhile (no keys yet)", "distance meanwhile (his keys not seen on a draw yet)", "identity" };   // = native bvis::M_*
    /// F8 > Developer > Inspect.
    private void bv_ui() {
        ui_check("diag_battle", "Battle research log (diag_battle): in ffxvr_diag.log, in battles: every executed command (\"cmd:\"), every change of a character's visibility (\"vis:\"), the spell effects' matrices (\"fx:\"); installs one game hook while it is on", ref _diag_battle, UiTag.None,
                 "Read-only research log. While it is on, a detour sits on the game's command parser (MsParseCommand); it always runs the game's own function and changes nothing. Developer mode alone does not install it.");
        if (ImGui.Button("Log the command table##bv") && _bv_dump_t < 0) { _bv_dump_t = 0; _bv_dump_id = 0; _bv_dump_lines = 0; d7_log("cmdtab: === the game's command tables: id, name, record fields (the tables' layout per ffx-rust's kernel.md: not confirmed by us)"); }
        ui_tip("Writes every command, item and monster-command id with its name to ffxvr_diag.log, once (\"cmdtab:\" lines). Needs the battle research log on; the names are there once a save is loaded. The game stutters for a second or two while it writes.");
        if (_bv_dump_t >= 0) { ImGui.SameLine(); ImGui.TextDisabled(esc(I($"writing... {_bv_dump_lines} records"))); }
        else if (!_diag_battle) { ImGui.SameLine(); ImGui.TextDisabled("(needs the battle research log on)"); }
        int n = 0;
        if (_fn_bvis_status != null) { fixed (int* st = _bv_st) { n = _fn_bvis_status(st, 14); } }
        if (n >= 14) {
            string[] rule = k_bh_rule;
            int m = _bv_st[8];
            ImGui.TextDisabled(esc(I($"battle log: {(_bv_st[0] != 0 ? I($"logging battle {_bv_st[1]}") : _diag_battle ? "waiting for a battle" : "off")} | lines this session: cmd {_bv_st[2]}, vis {_bv_st[3]}, fx {_bv_st[4]}, not written (the budgets) {_bv_st[5]} | this battle's commands: from the hook {_bv_st[6]}, polled {_bv_st[7]} | magic id {_bv_st[13]}")));
            ImGui.TextDisabled(esc(I($"battle log: MsParseCommand detour: {_bv_hook_status} | calls {_bv_calls}, faults {_bv_faults}, dropped {_bv_dropped}, text faults {_bv_text_faults}, frame faults {_bv_tick_faults}, frames without the actor table {_bv_snap_miss}")));
            ImGui.TextDisabled(esc(I($"battle body hide: rule {(m >= 0 && m < rule.Length ? rule[m] : "?")} | his keys {_bv_st[9]}, table {_bv_st[10]} keys (build {_bh_gen}, {Math.Max(_bh_owners - 1, 0)} other actors) | draws hidden by his keys last frame {_bv_st[11]} | body hide: BATTLE lines {_bv_st[12]} | at the last push: rule {_bh_mode}, hidden {_bh_hidden}, his keys {_bh_self_n}")));
        }
    }
}

/// <summary>The fallback for a name in the game's Western text encoding when Fahrenheit's decoder throws: its plain characters
/// only (digits from 0x30, the space and punctuation from 0x3A, capitals from 0x50, small letters from 0x70; from memory of
/// the encoding, not checked against the game here), anything else '?'.</summary>
internal static class BvText {
    public static int Ascii(ReadOnlySpan<byte> src, Span<byte> dst) {
        int k = 0;
        for (int i = 0; i < src.Length && k < dst.Length; i++) {
            int b = src[i];
            if (b == 0) break;
            int c = b >= 0x30 && b <= 0x39 ? b : b >= 0x3A && b <= 0x49 ? b - 0x1A : b >= 0x4A && b <= 0x4F ? b - 0x10 : b >= 0x50 && b <= 0x89 ? b - 0x0F : '?';
            dst[k++] = (byte)c;
        }
        return k;
    }
}
