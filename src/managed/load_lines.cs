// SPDX-License-Identifier: MIT

namespace FFXVR;

// Loads build (design-notes "Loads: the lock release on every load, load: lines for every load, release defaults (branch
// `loads`)"). Three things here, all on top of load_diag.cs's hooks (no hook of its own):
//
// 1. Non-battle loads. A map free outside 0x909A20 (hl_mapfree with _ld_depth_map 0) is the request of an area change, an
//    event map change, a save load or the title's load: 0x9093B0 (the field map loader, from the event set-up 0x872EF0 and the
//    event script's map commands 0x878510 / 0x878530 / 0x878550), 0x909E60 (the free alone, from the event teardown 0x872D20)
//    and 0x909680 call 0x909EA0 directly. The main thread opens a load there (the next free of the same load is counted, not a
//    new load) and the native load phase: 3 an area or event map load, 4 a load from the title (the event id 0x12FBC40 is
//    0x17, Fahrenheit's "main menu" test). It arms the lock release only with load_lock_release=2 (all loads, a test mode;
//    review LD2-1), and it ends at the first frame with the new map's camera (LD2-3), not at the line. The render thread
//    follows the load at every Present: the first
//    frame with the new map's camera (a real 3D camera upload and scene draws, the game frame advancing, after the free
//    returned, and either a frame without a 3D camera since the request or another view matrix than at the free), then
//    "settled" once the loader thread has been idle for 10 Presents. The load: line is written then and the phase ends (also
//    on a 60 s timeout, or with no new camera seen: the loader idle for 300 Presents while the game runs).
//    Its kind, decided at the line: from event 0x17 with Fahrenheit's save / load screen open in the 10 s before the request =
//    a save load, else from event 0x17 = a new game (the title's load); to event 0x17 = to the title (boot, quit, game over);
//    another event id = an area change; the same event id = an event map change (a cutscene's own map, a script-driven map switch).
// 2. The Present tracker, for every load: line (battle entry, way back and the loads above): the Presents in the load, the
//    longest gap between two Presents, the Presents on which the game frame counter (0x1FCBBF0) had not moved, and the longest
//    such stall (the game thread stuck while the render thread keeps presenting).
// 3. The lock release's counts for each load (ffxvr_load_info, the session counters by load kind; a load's share is the
//    difference between its start and its line), with the holders (LD2-4): who held the lock at the contended re-acquires,
//    and who the watcher saw inside our release windows (about every 1 ms; main = main-thread work mid-list).
//
// diag_load_lines (ini, default 1) writes these lines; it needs no hook of its own beyond the load hooks (which the lock
// release needs anyway). The expensive parts of the old battle lines (the address-space walks, the chr cache table, the
// wait-all callers' stack walks, the low-2 GB reports) stay with diag_load (default 0 since this build).
public sealed unsafe partial class FFXVRModule {

    private bool _diag_load_lines = true;      // ini diag_load_lines (default 1): one load: line per load in ffxvr_diag.log (cheap: no extra hook)
    private delegate* unmanaged[Cdecl]<int*, int, int> _fn_load_info;   // ABI 24: (out, n) -> count of values (60)
    private const int k_li_n = 60, k_ev_title = 0x17;

    // -- render thread: this Present's values (li_read, once per Present) ------------------------------------------------------
    private readonly int[] _li_now = new int[k_li_n], _li_in = new int[k_li_n], _li_out = new int[k_li_n], _li_fl = new int[k_li_n];
    private bool _li_ok, _li_3d, _li_adv; private int _li_cam3d_prev = -1; private uint _li_frame_prev;

    // -- the Present tracker ----------------------------------------------------------------------------------------------------
    private struct LoadTrk { public bool on; public long t0, t_prev, gap_max, t_adv, stall_max; public uint frame; public int presents, stuck; }
    private LoadTrk _trk_in, _trk_out, _trk_fl;
    private static void trk_reset(ref LoadTrk k, long t, uint frame) { k = default; k.on = true; k.t0 = k.t_prev = k.t_adv = t; k.frame = frame; }
    private static void trk_feed(ref LoadTrk k, long t, uint frame) {
        if (!k.on) return;
        k.presents++; long g = t - k.t_prev; if (g > k.gap_max) k.gap_max = g; k.t_prev = t;
        if (frame == k.frame) k.stuck++;
        else { long st = t - k.t_adv; if (st > k.stall_max) k.stall_max = st; k.t_adv = t; k.frame = frame; }
    }
    private static string trk_text(in LoadTrk k, long t) {
        long st = Math.Max(k.stall_max, t - k.t_adv);   // a stall still running at the line counts too
        return I($"frames: {k.presents} Presents, longest Present gap {ms(k.gap_max):F0} ms, game frame not advanced on {k.stuck} Presents (longest game-thread stall {ms(st):F0} ms)");
    }

    // -- the non-battle load: main thread at the request ------------------------------------------------------------------------
    private volatile int _fl_seq;              // one per load request (main thread; published after the fields below)
    private volatile int _fl_open;             // 1 from the request (main) until the render thread writes its line (render)
    private long _fl_t_req, _fl_t_free_done;   // QPC: the request; the first free returned (main; Volatile)
    private int _fl_ev0, _fl_field0, _fl_frees, _fl_wn; private bool _fl_menu_recent; private ulong _fl_m0a, _fl_m0b;
    private readonly long[] _lt_base_fl = new long[(int)LT.N];
    private long _fl_menu_t;                   // QPC of the last Present with Fahrenheit's save / load screen latched open (render thread; Volatile)

    /// <summary>hl_mapfree, before the original, for a free outside 0x909A20 (main thread). True when this free opened a load
    /// (its return then marks the free done).</summary>
    private bool fl_request(long t0) {
        if (_fl_open != 0) { System.Threading.Interlocked.Increment(ref _fl_frees); return false; }   // the same load's next free (an area change frees in 0x872D20's 0x909E60, then again in 0x9093B0)
        _fl_ev0 = FhUtil.get_at<int>(0xEFBC40);             // event_id (Fahrenheit Globals.event_id, VA 0x12FBC40): the event being left
        _fl_field0 = FhUtil.get_at<short>(0xD2CA94);         // the field map word (as ld_map_request)
        _fl_m0a = FhUtil.get_at<ulong>(0x1FCBC60); _fl_m0b = FhUtil.get_at<ulong>(0x1FCBC68);   // the map name's raw bytes (no string on this thread)
        long mt = vr(ref _fl_menu_t);
        _fl_menu_recent = mt != 0 && t0 - mt < 10 * System.Diagnostics.Stopwatch.Frequency;
        _fl_frees = 1; vw(ref _fl_t_free_done, 0); vw(ref _fl_t_req, t0);
        lt_snap(_lt_base_fl); _fl_wn = _ld_wn;
        _fl_open = 1;
        lp_phase(_fl_ev0 == k_ev_title ? 4 : 3);           // the native load phase: arms the lock release (at most 60 s)
        _fl_seq++;
        return true;
    }

    // -- the non-battle load: render thread ------------------------------------------------------------------------------------
    private int _fl_seen, _fl_rph, _fl_n, _fl_idle, _fl_fp0, _fl_rev0, _fl_rfield0, _fl_phase; private bool _fl_gap, _fl_fp0_set, _fl_rmenu, _fl_arm_ended;
    private long _fl_rt_req, _fl_t_cam; private string _fl_name0 = "";
    private readonly int[] _fl_kind_rel = new int[5];   // the session's releases by line kind: area, event map, save load, new game, to the title
    private static readonly string[] k_fl_kinds = { "area change", "event map change", "save load", "new game (from the title)", "to the title" };

    /// <summary>ld_present, once per Present (render thread): ffxvr_load_info, the 3D-frame test, the game frame, the save-screen
    /// latch; then the trackers and the non-battle load's progress.</summary>
    private void li_present(long t) {
        uint frame = FhUtil.get_at<uint>(0x1FCBBF0);
        _li_adv = frame != _li_frame_prev; _li_frame_prev = frame;
        var f = _fn_load_info;
        if (f != null) { fixed (int* p = _li_now) _li_ok = f(p, k_li_n) >= k_li_n; }
        int c3 = _li_now[0];
        _li_3d = _li_ok && _li_cam3d_prev != -1 && c3 != _li_cam3d_prev && _li_now[1] >= 3;   // a real 3D camera upload and scene draws in the frame just finished
        _li_cam3d_prev = _li_ok ? c3 : -1;
        if (_fh_menu_open) vw(ref _fl_menu_t, t);   // push_all's latch from the last frame (OPEN to CLOSED)
        trk_feed(ref _trk_in, t, frame); trk_feed(ref _trk_out, t, frame);
        fl_present(t, frame);
    }

    private void fl_present(long t, uint frame) {
        long fq = System.Diagnostics.Stopwatch.Frequency;
        int sq = _fl_seq;
        if (sq != _fl_seen) {   // a new request; the previous load has had its line (the main thread opens one only after fl_log dropped _fl_open)
            _fl_seen = sq; _fl_rph = 1; _fl_n++;
            _fl_rt_req = vr(ref _fl_t_req); _fl_rev0 = _fl_ev0; _fl_rfield0 = _fl_field0; _fl_rmenu = _fl_menu_recent; _fl_phase = _fl_ev0 == k_ev_title ? 4 : 3;
            _fl_name0 = map_name_raw(_fl_m0a, _fl_m0b);
            trk_reset(ref _trk_fl, t, frame); Array.Copy(_li_now, _li_fl, k_li_n);
            _fl_gap = _fl_fp0_set = _fl_arm_ended = false; _fl_t_cam = 0; _fl_idle = 0;
        }
        if (_fl_rph == 0) return;
        trk_feed(ref _trk_fl, t, frame);
        bool freed = vr(ref _fl_t_free_done) != 0;
        if (_fl_rph == 1) {
            if (!_li_3d) _fl_gap = true;
            if (freed) {
                if (!_fl_fp0_set) { _fl_fp0_set = true; _fl_fp0 = _li_now[2]; }   // the view as the free returned (the old map's last camera)
                else if (_li_3d && _li_adv && (_fl_gap || _li_now[2] != _fl_fp0)) {
                    _fl_t_cam = t; _fl_rph = 2; _fl_idle = 0;
                    lp_phase(-_fl_phase); _fl_arm_ended = true;   // LD2-3: the release window ends with the black part of the load; the line still waits for the settle
                }
            }
            if (_fl_rph == 1) {   // no new camera (a movie, a menu, or a new map with the old view and no 2D frame): settled after 300 Presents
                                  // (about 5 s) with the loader idle and the game running throughout (no game-frame stall over 100 ms)
                _fl_idle = freed && ld_loader_idle() && t - _trk_fl.t_adv <= fq / 10 ? _fl_idle + 1 : 0;
                if (_fl_idle >= 300) { fl_log(t, " (no new camera seen: settled after 300 Presents with the loader idle and the game running)"); return; }
            }
        }
        else if (_fl_rph == 2) {
            _fl_idle = ld_loader_idle() ? _fl_idle + 1 : 0;
            if (_fl_idle >= 10) { fl_log(t, ""); return; }
        }
        if (t - _fl_rt_req > 60 * fq) fl_log(t, " (timed out after 60 s)");
    }

    /// <summary>The non-battle load's line, and its end: the native phase, the main thread's open flag (render thread).</summary>
    private void fl_log(long t, string note) {
        _fl_rph = 0; _trk_fl.on = false;
        if (!_fl_arm_ended) lp_phase(-_fl_phase);   // ends it only if it is still this load's (a battle phase opened since keeps the arm); before the open flag drops
        // LD2-7: everything the main thread's next request rewrites is copied before the open flag drops
        int ev1 = FhUtil.get_at<int>(0xEFBC40), fld1 = FhUtil.get_at<short>(0xD2CA94), frees = _fl_frees, wn0 = _fl_wn;
        long[] b = (long[])_lt_base_fl.Clone();
        _fl_open = 0;           // from here the main thread may open the next load
        int kind = _fl_rev0 == k_ev_title ? (_fl_rmenu ? 2 : 3) : ev1 == k_ev_title ? 4 : ev1 != _fl_rev0 ? 0 : 1;
        int rel = _li_ok ? _li_now[4] - _li_fl[4] : 0;
        _fl_kind_rel[kind] += Math.Max(0, rel);
        if (!(_diag_load_lines || _diag_load) || _fn_diag_line == null) return;
        long wt = lt(LT.wait_t) - b[(int)LT.wait_t], wn = lt(LT.wait_n) - b[(int)LT.wait_n];
        string cam = _fl_t_cam != 0 ? I($"{ms(_fl_t_cam - _fl_rt_req):F0} ms to the first frame with the new map's camera") : "no frame with a new 3D camera";
        string win = _load_lock_release == 2 ? I($" (release window: to +{ms((_fl_arm_ended ? _fl_t_cam : t) - _fl_rt_req):F0} ms)") : "";
        d7_log(I($"load: {k_fl_kinds[kind]} {_fl_n} ({(_fl_name0.Length > 0 ? _fl_name0 : "?")} -> {map_name()}, event 0x{_fl_rev0:X} -> 0x{ev1:X}, field map {_fl_rfield0} -> {fld1}): total {cam}, settled at +{ms(t - _fl_rt_req):F0} ms{win}{note} | map frees {frees}")
             + " | " + ld_hd(b) + I($" | blocking waits {ms(wt):F0} ms ({wn}: {ld_waits(wn0)}) | ") + trk_text(_trk_fl, t) + " | " + li_lock_text(_li_fl, _fl_phase));
    }

    /// <summary>The map name from its raw 16 bytes (as map_name, which reads them live).</summary>
    private static string map_name_raw(ulong a, ulong b) {
        var sb = new System.Text.StringBuilder();
        for (int i = 0; i < 15; i++) { byte c = (byte)((i < 8 ? a >> (8 * i) : b >> (8 * (i - 8))) & 0xFF); if (c == 0 || c < 32 || c > 126) break; sb.Append((char)c); }
        return sb.ToString();
    }

    /// <summary>The lock release's part of a load: line, since the snapshot `b` (render thread). `kind` is the native load kind
    /// of this load (1 entry, 2 way back, 3 area / event map, 4 from the title); kind 0 is a loadPCluster call alone.</summary>
    private string li_lock_text(int[] b, int kind) {
        if (!_li_ok) return "lock release: no counts (the native helper)";
        int d(int i) => _li_now[i] - b[i];
        int k = Math.Clamp(kind, 0, 4);
        string state = _load_lock_release == 0 ? "off (load_lock_release=0)" : _li_now[3] == 0 ? "unavailable (see the overlay's load lock row)"
                     : _load_lock_release == 2 ? "all loads (test)" : k >= 3 ? "battle loads (this kind is released only in mode 2)" : "battle loads";
        int n = d(4); if (n == 0) return I($"lock release {state}: no release");
        // LD2-4: the holders in this load's phase: at the contended re-acquires, and the watcher's ~1 ms looks inside our windows
        int bl = d(25 + 3 * k), bm = d(26 + 3 * k), bo = d(27 + 3 * k);
        int tl = d(40 + 4 * k), tm = d(41 + 4 * k), to = d(42 + 4 * k), tf = d(43 + 4 * k), sm = d(41);   // sm: kind 0's main looks (a loadPCluster alone)
        return I($"lock release {state}: {n} releases ({d(10 + k)} in this load's phase, {d(10)} for a loadPCluster alone), {d(5)} contended re-acquires (holder at the re-acquire in this load's phase: loader {bl}, main {bm}, other {bo}), {d(6)} re-acquires over 11 ms, {d(7) / 1000.0:F1} ms re-acquiring in all")
             + I($" | lock taken inside our windows (looks about every 1 ms, this load's phase): loader {tl}, MAIN {tm}, other {to}, free {tf}{(sm > 0 ? I($"; a loadPCluster alone: MAIN {sm}") : "")}{(d(9) > 0 ? I($" | BAD {d(9)} (the release turned itself off)") : "")}");
    }

    /// <summary>The overlay's per-kind session counts (the main window's load lock row).</summary>
    private string li_session_text() {
        if (!_li_ok) return "";
        return I($"releases this session: battle entry {_li_now[11]}, way back {_li_now[12]}, area change {_fl_kind_rel[0]}, event map {_fl_kind_rel[1]}, save load {_fl_kind_rel[2]}, new game {_fl_kind_rel[3]}, to the title {_fl_kind_rel[4]}, a loadPCluster alone {_li_now[10]}{(_fl_rph != 0 ? " (a load in progress)" : "")}");
    }
}
