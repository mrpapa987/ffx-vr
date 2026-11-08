// SPDX-License-Identifier: MIT

namespace FFXVR;

// Overdrive gestures (roadmap 4.3, 6 Nov 2026; native ffxvr_odgest.h, ABI 47). While an overdrive's input window is open
// (exactly one of the state words at 4 and the shared timer running; native reads them every Present in battle), the Quest
// controllers become the weapon, and the sticks and buttons keep working at the same time:
//   Lulu's Fury     circles of either hand, in any plane: the right stick turns with the hand (the game's quadrants)
//   Tidus's Swordplay  a quick swing of the right hand presses A once per swing (the bring-back and a reach up are not
//                   swings); off by default (od_gest_tidus_swing=0) until the owner's test: a press outside the zone RESTARTS the bar
//                   (0x8923F8..0x89244F: the cursor back to its start while the timer runs), so a false A costs a traversal
//   Auron's Bushido od_gest_auron: 0 off (the default), 1 slashes and punches: slashes left / right / up / down = the d-pad, a punch
//                   forward = LB (left hand) / RB (right hand), the face buttons on the controllers; 2 strike the targets (native
//                   ffxvr_odtarget.h, ABI 52-54): one big bright plate at chest height shows the current input, the d-pad
//                   arrows in a left spot and A / B / X / Y, L1 / R1 in a right spot (the next one small above its own spot); a
//                   blade in either hand cuts it along its sliding chevrons (A down, B right, X left, Y up as on the Xbox pad, L1
//                   left, R1 right), and the next appears in its spot; the press sent is always the game's expected
//                   one for its own progress (never a wrong press), one at a time
// Practice targets (F8, any time with the headset on and no overdrive window open, outside battle too): a random 3-8 symbols or
// the last Bushido seen, a 4 s bar, the full hit test, visuals and buzz, nothing sent; F8 and the log get "practice: N of N in x s".
// Native decides and logs (odg: lines: one per window, Auron's expected sequence and every press the game saw); this file
// pushes the settings, draws the F8 rows, and (Debug, a diagnostic profile, this session only) sets the game's own
// "overdrives always ready" flag. A hand grabbing the world gestures nothing; in an injecting Bushido window the grips do not
// grab (a clenched punch is a punch), a quick squeeze is not L1 / R1 (only a punch is) and the VR triggers are held back.
// Bushido's opposite pairs in a row (Shooting Star's Left, Right; Banishing Blade's Right, Left) go at once when the game
// expects that button next (the map of the sequence values: the game's own pad bindings, read at each window's open, else
// the built-in k_auron_map; the mod still learns each value it sees as a cross-check and logs any disagreement); any other
// bring-back of the same hand within 0.3 s is ignored.
// ini: od_gestures (1), od_gest_lulu (1), od_gest_tidus_swing (0; the old key od_gest_tidus, saved as 1 by the first build,
// is ignored so the new default reaches every ini), od_gest_auron (0 / 1 / 2, default 0; an older ini's bool 1 / true reads as 1,
// slashes and punches, as before), od_targets_blade_pitch (-45..45 deg; 0), od_targets_dist2 (0.35-0.9 m; 0.5), od_targets_height2
// (-0.5..0 m; -0.3), od_targets_size (0.6-1.6, a multiplier on the 24 cm plate; 1), od_targets_time (1-3; 1: the game's Bushido
// countdown runs that many times slower in a strike-the-targets window, practice's bar too), od_targets_spread (0.25-0.6 m between
// the two spots' centres; 0.36; at least the plate + 2 cm), od_targets_art (1: the local target art pack when present); the first build's od_targets_dist / _height
// (0.6 / -0.2 for its row of plates) are ignored so the one-target defaults reach every ini; od_gest_haptics (1), od_circle_cm (1-20; 4),
// od_slash_speed (0.8-5 m/s; 1.6), od_lulu_stick (0 right, 1 left, 2 both; 0), od_visuals (1) and od_visual_size (0.5-2; 1): the
// overdrive visuals (native ffxvr_odvis.h: trails, bursts, Lulu's ring and spin count, Tidus's blade and bar, Auron's input row),
// drawn near the hands in the headset only while that overdrive's window is open with its gestures on.
public sealed unsafe partial class FFXVRModule {
    private bool  _od_gestures = true, _od_gest_lulu = true, _od_gest_tidus = false, _od_gest_haptics = true;
    private int   _od_gest_auron = 0;   // 0 off, 1 slashes and punches, 2 strike the targets (ABI 52)
    private bool _od_tgt_old_logged;                                                                                     // R5-N2: the old keys' one line
    private float _od_tgt_pitch = 0f, _od_tgt_dist = 0.50f, _od_tgt_height = -0.30f, _od_tgt_size = 1f;   // ini od_targets_blade_pitch / _dist2 / _height2 / _size
    private float _od_tgt_time = 1f;                                                                      // ini od_targets_time (the countdown's stretch)
    private float _od_tgt_spread = 0.36f;                                                                 // ini od_targets_spread (between the two spots)
    private bool _od_tgt_art = true;                                                                      // ini od_targets_art (the local target art pack, <mod>\art\targets)
    private bool _od_tgt_badge = false;                                                                   // ini od_targets_badge (a small button letter; off: arrows only)
    private bool _od_tgt_hide2d = true, _od_tgt_hide_at_launch;                                           // ini od_targets_hide2d: hide the game's Bushido row (hooked at launch)
    // the game's Bushido input row (0x892740, cdecl void(), called once from the battle UI at 0x89B411; prologue 55 8B EC 0F):
    // skipped while native says a strike-the-targets window's targets are drawn (ffxvr_odt_hide2d, ABI 56)
    private const int k_odt_row = 0x492740;                                                               // VA 0x892740
    private delegate* unmanaged[Cdecl]<int> _fn_odt_hide2d;                                               // ABI 56
    private delegate* unmanaged[Cdecl]<int, int> _fn_odt_row_patch;                                       // ABI 57: the call-site patch at 0x89B411 (native, at init)
    private delegate* unmanaged[Cdecl]<int, int, int, int, int> _fn_odt_row_stat;                        // ABI 57: our counts for the window line; returns the patch's state
    // the detour's install state (section 15): 0 off, 1 queued, 2 inserted, 3 late-inserted, 4 not inserted (late insertion failed),
    // 5 not hooked (refused, the bytes, no native); checked once at the first EXEC frame (odt_row_resolve)
    private int _odt_row_install, _odt_patch_state, _odt_row_check_frames; private bool _odt_row_checked; private string _odt_row_check = "not checked yet (the first frame after launch)";
    private d7_void? _h_odt_row, _o_odt_row; private nint _a_odt_row; private bool _odt_row_hook_ok;
    private string _odt_row_status = "not hooked (od_targets_hide2d=0 at launch)";
    private int _odt_row_calls, _odt_row_hidden, _odt_row_faults; private uint _odt_row_frame = uint.MaxValue; private bool _odt_row_val;
    private float _od_circle_cm = 4f, _od_slash_speed = 1.6f;
    private int   _od_lulu_stick = 0;
    private bool  _od_always_ready;     // Debug, a diagnostic profile, session only (never saved): Btl.debug.always_available_overdrive in every battle
    private bool  _od_always_wrote;     // we set the flag in this battle (cleared at its end)
    private delegate* unmanaged[Cdecl]<int, float, float, int, void> _fn_odg_set;   // ABI 47
    private delegate* unmanaged[Cdecl]<int, float, int> _fn_odv_set;                 // ABI 49: the overdrive visuals
    private delegate* unmanaged[Cdecl]<float, float, float, float, float, float, int, int, void> _fn_odt_set; // ABI 52: strike the targets; 53: + time; 54: + spread; 55: + art; 56: (pitch, distance, height, size, time, spread, art, badge)
    private delegate* unmanaged[Cdecl]<int, int> _fn_odt_practice;                   // ABI 52: Practice targets (1 random, 2 the last Bushido)
    private bool  _od_visuals = true;    // ini od_visuals: what the hands do, drawn near them in the headset while a gesture overdrive's window is open
    private float _od_visual_size = 1f;  // ini od_visual_size (0.5-2)
    private delegate* unmanaged[Cdecl]<int, byte*, int, int> _fn_odg_text;
    private static readonly string[] k_odg_stick = { "right stick", "left stick", "both sticks" };

    private void odg_bind(Func<string, nint> req) {
        _fn_odg_set = (delegate* unmanaged[Cdecl]<int, float, float, int, void>)req("ffxvr_odg_set");
        _fn_odg_text = (delegate* unmanaged[Cdecl]<int, byte*, int, int>)req("ffxvr_odg_text");
        _fn_odv_set = (delegate* unmanaged[Cdecl]<int, float, int>)req("ffxvr_odv_set");
        _fn_odt_set = (delegate* unmanaged[Cdecl]<float, float, float, float, float, float, int, int, void>)req("ffxvr_odt_set");   // ABI 56 (+ time, + spread, + art, + badge)
        _fn_odt_hide2d = (delegate* unmanaged[Cdecl]<int>)req("ffxvr_odt_hide2d");   // ABI 56
        _fn_odt_row_patch = (delegate* unmanaged[Cdecl]<int, int>)req("ffxvr_odt_row_patch");   // ABI 57
        _fn_odt_row_stat = (delegate* unmanaged[Cdecl]<int, int, int, int, int>)req("ffxvr_odt_row_stat");   // ABI 57
        _fn_odt_practice = (delegate* unmanaged[Cdecl]<int, int>)req("ffxvr_odt_practice");
    }
    private void odg_clear() { _fn_odg_set = null; _fn_odg_text = null; _fn_odv_set = null; _fn_odt_set = null; _fn_odt_practice = null; _fn_odt_hide2d = null; _fn_odt_row_patch = null; _fn_odt_row_stat = null; }

    /// <summary>At module init, after dmg3d_init (Fahrenheit's commit stops at the first failing insertion: a refusal here takes
    /// nothing else with it): with od_targets_hide2d=1 the hook on the game's Bushido input row.</summary>
    private void odt_row_init() {
        _od_tgt_hide_at_launch = _od_tgt_hide2d;
        // section 15: native's own call-site patch first (it does not depend on Fahrenheit's hook commit); the detour as well
        if (_native_ok && _fn_odt_row_patch != null) _odt_patch_state = _fn_odt_row_patch(_od_tgt_hide2d ? 1 : 0);
        if (!_od_tgt_hide2d) return;
        _odt_row_install = 5;
        if (!_native_ok || _fn_odt_hide2d == null) { _odt_row_status = "not hooked: the native helper did not load"; return; }
        byte b0 = 0, b1 = 0, b2 = 0, b3 = 0;   // the bytes checked (push ebp; mov ebp, esp; movsx ...): anything else is left alone
        try { b0 = FhUtil.get_at<byte>(k_odt_row); b1 = FhUtil.get_at<byte>(k_odt_row + 1); b2 = FhUtil.get_at<byte>(k_odt_row + 2); b3 = FhUtil.get_at<byte>(k_odt_row + 3); } catch { }
        if (b0 != 0x55 || b1 != 0x8B || b2 != 0xEC || b3 != 0x0F) { _odt_row_status = I($"not hooked: 0x892740 starts {b0:X2} {b1:X2} {b2:X2} {b3:X2}, not 55 8B EC 0F (another mod, or another exe): the game's row stays"); _logger.Warning("odt: " + _odt_row_status); return; }
        _h_odt_row = h_odt_row; new FhCallLocation("FFX.exe", k_odt_row).try_resolve(out _a_odt_row);
        if (_a_odt_row == 0) { _odt_row_status = "not hooked: FFX.exe not found"; return; }
        try {
            _odt_row_hook_ok = new FhCallHandle<d7_void>(new FhCallLocation((void*)_a_odt_row)).hook(this, _h_odt_row!);
            _odt_row_status = _odt_row_hook_ok ? "the Bushido row 0x892740 queued (its calls in a battle prove it)" : "the Bushido row hook was REFUSED (another mod hooks 0x892740?): the game's row stays";
            if (_odt_row_hook_ok) _odt_row_install = 1;
        }
        catch (Exception e) { _odt_row_status = $"the Bushido row hook FAILED ({e.GetType().Name}): the game's row stays"; }
        _logger.Info("odt: " + _odt_row_status);
    }
    /// <summary>Render thread, with dmg3d_resolve: the detour's original, once.</summary>
    private void odt_row_resolve() {
        if (_h_odt_row == null || !_odt_row_hook_ok) return;
        bool asked = false;   // Fahrenheit answered (it throws before EXEC: then the check waits)
        if (_o_odt_row == null) { try { System.Threading.Volatile.Write(ref _o_odt_row, d7_chain(_a_odt_row, _h_odt_row)); asked = true; } catch (Exception e) { _logger.Error($"odt: resolving the original: {e}"); } }
        else asked = true;
        // the check: once, 30 frames in (well after Fahrenheit's commit), only when Fahrenheit answered the chain question
        if (!_odt_row_checked && asked && ++_odt_row_check_frames >= 30) { _odt_row_checked = true; if (_o_odt_row == null) { try { System.Threading.Volatile.Write(ref _o_odt_row, d7_chain(_a_odt_row, _h_odt_row)); } catch { } } odt_row_check(); }
    }
    /// <summary>Section 15, render thread, once (the first EXEC frame, after Fahrenheit's commit): is the detour really in? MinHook
    /// writes a jmp (E9) over 0x892740's first bytes, and Fahrenheit keeps a chain entry for an inserted hook. Not in: one late
    /// insertion (Fahrenheit allows it on a function with no hooks; it rejects one whose queued hooks were applied). One log line.</summary>
    private void odt_row_check() {
        string bytes = odt_row_bytes(); bool chain = _o_odt_row != null;
        bool jmp = bytes.StartsWith("E9", StringComparison.Ordinal);
        if (chain) { _odt_row_install = 2; _odt_row_check = jmp ? I($"inserted at 0x{_a_odt_row:X8} (bytes {bytes})") : I($"inserted at 0x{_a_odt_row:X8} (Fahrenheit's chain has it), but the first bytes are {bytes}, not a jmp (E9): another patch over it?"); }
        else {
            bool late = false; string why = "";
            diag7_insert_begin();
            try { late = new FhCallHandle<d7_void>(new FhCallLocation((void*)_a_odt_row)).hook(this, _h_odt_row!); }
            catch (Exception e) { why = e.GetType().Name; }
            finally { diag7_insert_end(); }
            d7_void? o = null; try { o = d7_chain(_a_odt_row, _h_odt_row!); } catch { }
            if (o != null) System.Threading.Volatile.Write(ref _o_odt_row, o);
            string after = odt_row_bytes();
            if (late && o != null) { _odt_row_install = 3; _odt_row_check = I($"NOT inserted after the commit (bytes {bytes}, no chain entry); late-inserted now (bytes {after})"); }
            else { _odt_row_install = 4; _odt_row_check = I($"NOT inserted (bytes {bytes}, no chain entry); the late insertion {(late ? "was accepted but has no chain entry (bytes " + after + ")" : "failed" + (why.Length > 0 ? " (" + why + ")" : " (see core.log)"))}: the call-site patch must hide the row"); }
        }
        _logger.Info("odt: Bushido row detour: " + _odt_row_check);
        d7_log("odt: Bushido row detour: " + _odt_row_check + I($"; call-site patch state {_odt_patch_state} (1 patched, 2 off, < 0 not patched: see its own line)"));   // ffxvr_diag.log too
    }
    private static string odt_patch_name(int st) => st switch { 1 => "patched", 2 => "off", -1 => "FAILED: FFX.exe", -2 => "not patched: the call site's bytes differ (another mod?)", -3 => "FAILED: VirtualProtect", -4 => "not patched: a battle was on at launch", -5 => "not patched: the DLL could not be pinned", _ => "not asked (no native helper)" };
    private string odt_row_bytes() {
        try { byte* p = (byte*)_a_odt_row; return I($"{p[0]:X2} {p[1]:X2} {p[2]:X2} {p[3]:X2} {p[4]:X2}"); } catch { return "unreadable"; }
    }
    /// <summary>The game's Bushido input row (0x892740, main thread): skipped while native says our targets are drawn, latched per
    /// game frame; otherwise the original runs.</summary>
    private void h_odt_row() {
        d7_void o = System.Threading.Volatile.Read(ref _o_odt_row) ?? d7_resolve_main(ref _o_odt_row, _a_odt_row, _h_odt_row!);
        _odt_row_calls++;
        bool skip = false;
        try {   // our own work (as h_dmg_draw's): any failure falls through to the original, so the row is drawn
            var sk = _fn_odt_hide2d;
            if (sk != null) {
                uint f = FhUtil.get_at<uint>(0x1FCBBF0);
                if (f != _odt_row_frame) { _odt_row_frame = f; _odt_row_val = sk() != 0; }
                skip = _odt_row_val;
            }
        }
        catch { _odt_row_faults++; skip = false; }
        if (skip) { _odt_row_hidden++; return; }
        o();
    }
    private bool odg_parse(string k, string v) {
        switch (k) {
            case "od_gestures":     pb(k, v, ref _od_gestures); return true;
            case "od_gest_lulu":    pb(k, v, ref _od_gest_lulu); return true;
            case "od_gest_tidus":   return true;   // round 2: the first build's key (saved as 1) is ignored: Tidus's swing starts off
            case "od_gest_tidus_swing": pb(k, v, ref _od_gest_tidus); return true;
            case "od_gest_auron":   // 0 / 1 / 2; the builds before ABI 52 saved a bool (1 / 0, true / false): 1 = slashes and punches, as then
                if (int.TryParse(num(v), System.Globalization.NumberStyles.Integer, CI, out int am) && am >= 0 && am <= 2) _od_gest_auron = am;
                else if (try_bool(v, out bool ab)) _od_gest_auron = ab ? 1 : 0;
                else _ini_bad.Add($"{k}='{v}'");
                return true;
            case "od_targets_blade_pitch": pf(k, v, ref _od_tgt_pitch, -45f, 45f); return true;
            case "od_targets_dist":     // the first build's keys (its row of plates: 0.6 / -0.2) are ignored: the one-target defaults;
            case "od_targets_height":   // one log line a session says so (review R5-N2: else the plate moves without a word)
                if (!_od_tgt_old_logged) {
                    _od_tgt_old_logged = true;
                    _logger.Info($"settings: {k}={v} is from the first targets build (a row of plates): od_targets_dist and od_targets_height are ignored; the one big plate uses od_targets_dist2 / od_targets_height2 (defaults 0.5 m / -0.3 m; F8 'target distance (m)' / 'target height (m)')");
                }
                return true;
            case "od_targets_dist2":   pf(k, v, ref _od_tgt_dist, 0.35f, 0.9f); return true;
            case "od_targets_height2": pf(k, v, ref _od_tgt_height, -0.5f, 0f); return true;
            case "od_targets_size":   pf(k, v, ref _od_tgt_size, 0.6f, 1.6f); return true;
            case "od_targets_time":   pf(k, v, ref _od_tgt_time, 1f, 3f); return true;
            case "od_targets_spread": pf(k, v, ref _od_tgt_spread, 0.25f, 0.6f); return true;
            case "od_targets_art":    pb(k, v, ref _od_tgt_art); return true;
            case "od_targets_badge":  pb(k, v, ref _od_tgt_badge); return true;
            case "od_targets_hide2d": pb(k, v, ref _od_tgt_hide2d); return true;
            case "od_gest_haptics": pb(k, v, ref _od_gest_haptics); return true;
            case "od_circle_cm":    pf(k, v, ref _od_circle_cm, 1f, 20f); return true;
            case "od_slash_speed":  pf(k, v, ref _od_slash_speed, 0.8f, 5f); return true;
            case "od_lulu_stick":   pe(k, v, ref _od_lulu_stick, 0, 1, 2); return true;
            case "od_visuals":      pb(k, v, ref _od_visuals); return true;
            case "od_visual_size":  pf(k, v, ref _od_visual_size, 0.5f, 2f); return true;
            default: return false;
        }
    }
    private string odg_ini_text() {
        var ci = CI;
        return
            $"od_gestures={(_od_gestures ? 1 : 0)}\n" +
            $"od_gest_lulu={(_od_gest_lulu ? 1 : 0)}\n" +
            $"od_gest_tidus_swing={(_od_gest_tidus ? 1 : 0)}\n" +
            $"od_gest_auron={_od_gest_auron.ToString(ci)}\n" +
            $"od_targets_blade_pitch={_od_tgt_pitch.ToString("0.#", ci)}\n" +
            $"od_targets_dist2={_od_tgt_dist.ToString("0.0#", ci)}\n" +
            $"od_targets_height2={_od_tgt_height.ToString("0.0#", ci)}\n" +
            $"od_targets_size={_od_tgt_size.ToString("0.0#", ci)}\n" +
            $"od_targets_time={_od_tgt_time.ToString("0.0#", ci)}\n" +
            $"od_targets_spread={_od_tgt_spread.ToString("0.0#", ci)}\n" +
            $"od_targets_art={(_od_tgt_art ? 1 : 0)}\n" +
            $"od_targets_badge={(_od_tgt_badge ? 1 : 0)}\n" +
            $"od_targets_hide2d={(_od_tgt_hide2d ? 1 : 0)}\n" +
            $"od_gest_haptics={(_od_gest_haptics ? 1 : 0)}\n" +
            $"od_circle_cm={_od_circle_cm.ToString("0.#", ci)}\n" +
            $"od_slash_speed={_od_slash_speed.ToString("0.##", ci)}\n" +
            $"od_lulu_stick={_od_lulu_stick.ToString(ci)}\n" +
            $"od_visuals={(_od_visuals ? 1 : 0)}\n" +
            $"od_visual_size={_od_visual_size.ToString("0.0#", ci)}\n";
    }
    /// <summary>Every frame from push_all: the settings; the Debug test aid.</summary>
    private void odg_push() {
        if (_fn_odg_set != null)
            _fn_odg_set((_od_gestures ? 1 : 0) | (_od_gest_lulu ? 2 : 0) | (_od_gest_auron != 0 ? 4 : 0) | (_od_gest_tidus ? 8 : 0) | (_od_gest_haptics ? 16 : 0) | (_od_gest_auron == 2 ? 32 : 0),
                        _od_circle_cm, _od_slash_speed, _od_lulu_stick);
        if (_fn_odv_set != null) _fn_odv_set(_od_visuals ? 1 : 0, _od_visual_size);
        if (_fn_odt_set != null) _fn_odt_set(_od_tgt_pitch, _od_tgt_dist, _od_tgt_height, _od_tgt_size, _od_tgt_time, _od_tgt_spread, _od_tgt_art ? 1 : 0, _od_tgt_badge ? 1 : 0);
        if (_fn_odt_row_stat != null) _odt_patch_state = _fn_odt_row_stat(_odt_row_install, _odt_row_calls, _odt_row_hidden, _odt_row_faults);   // section 15: the window line's row part
        odg_always_tick();
    }
    /// <summary>The test aid (design 4.3 step 6): Btl + 0x28 (debug flags) + 0x14 = VA 0x112A90C, the game's own "overdrive always
    /// available" (debug-revival, zanarkand, fh-structs agree; Fahrenheit's BtlDebugFlags.always_available_overdrive). Set in every
    /// battle while ticked, cleared at the battle's end and when unticked; the box is not saved. Never in the Release logging
    /// profile (no diagnostic key on: diag_any): the box is off there. While it is on the gauge is not spent (debug-revival
    /// battle.md: MsCheckUseCommand / MsCalcMagic), so a save made later keeps a full gauge.</summary>
    private void odg_always_tick() {
        if (!diag_any()) _od_always_ready = false;
        byte* btl = (byte*)Globals.Battle.btl;
        if (btl == null) return;
        bool live = read(btl + 0x10, out byte bs) && bs != 0;
        if (_od_always_ready && live) {
            if (read(btl + 0x3C, out byte f) && f == 0) { btl[0x3C] = 1; if (!_od_always_wrote) d7_log("odg: test aid: overdrives always ready in this battle (the game's debug flag, F8 Debug)"); _od_always_wrote = true; }
        }
        else if (_od_always_wrote) {
            if (read(btl + 0x3C, out byte f) && f != 0) btl[0x3C] = 0;
            _od_always_wrote = false;
        }
    }
    private string odg_text(int which) {
        if (_fn_odg_text == null) return "";
        byte* b = stackalloc byte[768];
        int n = _fn_odg_text(which, b, 768);
        return n > 0 ? Marshal.PtrToStringAnsi((nint)b) ?? "" : "";
    }
    /// <summary>F8, the Controllers section.</summary>
    private void odg_ui() {
        ImGui.Checkbox("overdrive gestures (the controllers keep working too):", ref _od_gestures);
        if (!_od_gestures) ImGui.BeginDisabled(true);
        ImGui.SameLine(); ImGui.Checkbox("Lulu's Fury: circles##odg", ref _od_gest_lulu);
        ImGui.SameLine(); ImGui.Checkbox("Tidus's Swordplay: swing (off until you tick it: a miss restarts the bar)##odg", ref _od_gest_tidus);
        if (!_od_gest_tidus && ImGui.IsItemHovered(ImGuiHoveredFlags.None)) ImGui.SetTooltip("Tick it to swing for Slice & Dice / Spiral Cut: a swing of the right controller presses A once; the light blade and a small copy of the bar show where it lands. Off by default: an early swing restarts the bar.");
        ImGui.SameLine(); ImGui.Checkbox("buzz (also on a wrong Bushido input or a Swordplay miss from a button)##odg", ref _od_gest_haptics);
        ImGui.Text("Auron's Bushido:"); ImGui.SameLine();
        ImGui.RadioButton("off##odga", ref _od_gest_auron, 0); ImGui.SameLine();
        ImGui.RadioButton("slashes and punches (experimental)##odga", ref _od_gest_auron, 1); ImGui.SameLine();
        ImGui.RadioButton("strike the targets##odga", ref _od_gest_auron, 2);
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.None)) ImGui.SetTooltip("One big plate at chest height shows the input to give now: the d-pad arrows in the left spot, A / B / X / Y and L1 / R1 in the right spot (the next one small above its own spot). Cut it with the blade in either hand along its sliding chevrons: an arrow its way; the face buttons where they sit on an Xbox pad (A down, B right, X left, Y up); L1 left, R1 right. The mod sends the game's own expected button, one at a time, so a stroke never sends a wrong press; your buttons still work.");
        ImGui.SetNextItemWidth(140f); ImGui.SliderFloat("smallest circle (cm)##odg", ref _od_circle_cm, 1f, 20f, "%.0f");
        ImGui.SameLine(); ImGui.SetNextItemWidth(140f); ImGui.SliderFloat("slash / swing speed (m/s)##odg", ref _od_slash_speed, 0.8f, 5f, "%.1f");
        ImGui.SameLine(); ImGui.SetNextItemWidth(120f);
        if (ImGui.BeginCombo("Fury's stick##odg", k_odg_stick[_od_lulu_stick])) {
            for (int i = 0; i < 3; i++) if (ImGui.Selectable(k_odg_stick[i], i == _od_lulu_stick)) _od_lulu_stick = i;
            ImGui.EndCombo();
        }
        ImGui.Checkbox("overdrive visuals##odv", ref _od_visuals);
        if (_od_visuals) { ImGui.SameLine(); ImGui.SetNextItemWidth(140f); ImGui.SliderFloat("visual size##odv", ref _od_visual_size, 0.5f, 2f, "%.1f"); }
        ImGui.SameLine(); ImGui.TextDisabled("near the hands, in the headset: Fury a ring and the spins; Swordplay a light blade and the bar; Bushido the inputs ahead");
        if (!_od_gestures) ImGui.EndDisabled();
        string warn = odg_text(3);
        if (warn.Length > 0) { ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.6f, 0.2f, 1f)); ImGui.TextWrapped(esc(warn)); ImGui.PopStyleColor(1); }
        ImGui.TextDisabled(esc(odg_text(0)));
        if (_od_gest_auron == 1) ImGui.TextDisabled("Bushido: an opposite pair in a row (Left, Right) goes at once when the game asks for it; any other bring-back of the same hand within 0.3 s is ignored; L1 / R1 is a punch (a quick squeeze is not L1 / R1 there, and the triggers are held back)");
        if (_od_gest_auron == 2) ImGui.TextDisabled("Bushido targets: arrows on the left, A / B / X / Y and L1 / R1 on the right; cut the big plate along its chevrons, either hand (alternating is fastest; A down, B right, X left, Y up, L1 left, R1 right; a wrong way flashes the rim red and buzzes); a wrong button of yours starts them over; while they are up a quick squeeze is not L1 / R1, the grips do not grab and the triggers are held back");
        ImGui.TextDisabled("last: " + esc(odg_text(1)));
        odt_ui();
    }
    /// <summary>F8, under the overdrive gesture rows: Practice targets (works with the gestures off too) and the targets' layout.</summary>
    private void odt_ui() {
        if (ImGui.Button("Practice targets##odt") && _fn_odt_practice != null) _fn_odt_practice(1);
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.None)) ImGui.SetTooltip("A random 3-8 Bushido symbols and a 4 s bar, any time with the headset on (no overdrive window open): the blades, the hits and the buzz as in battle, nothing is sent to the game. Close F8 (or move it aside) after pressing: its panel is drawn over the 3D view, plates included.");
        string replay = odg_text(5);
        ImGui.SameLine();
        if (replay.Length == 0) ImGui.BeginDisabled(true);
        if (ImGui.Button("Replay last Bushido##odt") && _fn_odt_practice != null) _fn_odt_practice(2);
        if (replay.Length == 0) ImGui.EndDisabled();
        ImGui.SameLine(); ImGui.TextDisabled(esc(odg_text(4)));
        ImGui.SetNextItemWidth(130f); ImGui.SliderFloat("blade pitch (deg)##odt", ref _od_tgt_pitch, -45f, 45f, "%.0f");
        ImGui.SameLine(); ImGui.SetNextItemWidth(130f); ImGui.SliderFloat("target distance (m)##odt", ref _od_tgt_dist, 0.35f, 0.9f, "%.2f");
        ImGui.SameLine(); ImGui.SetNextItemWidth(130f); ImGui.SliderFloat("target height (m)##odt", ref _od_tgt_height, -0.5f, 0f, "%.2f");
        ImGui.SameLine(); ImGui.SetNextItemWidth(130f); ImGui.SliderFloat("target size##odt", ref _od_tgt_size, 0.6f, 1.6f, "%.1f");
        ImGui.SameLine(); ImGui.SetNextItemWidth(130f); ImGui.SliderFloat("target spacing (m)##odt", ref _od_tgt_spread, 0.25f, 0.6f, "%.2f");
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.None)) ImGui.SetTooltip("Between the left spot (the d-pad arrows) and the right spot (A / B / X / Y, L1 / R1). Never less than the plate's width + 2 cm (bigger targets push them apart).");
        ImGui.SetNextItemWidth(130f); ImGui.SliderFloat("Bushido time (x, strike the targets)##odt", ref _od_tgt_time, 1f, 3f, "%.1f");
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.None)) ImGui.SetTooltip("More time for Bushido in strike the targets: the game's input timer runs this many times slower while the targets window is open (1 = the game's own time). Practice targets' bar uses it too. The game also counts the extra seconds left at the end toward Bushido's damage bonus (up to x1.5 with the whole time left), so a fast finish with more time hits harder. Nothing changes for the slashes and punches or any other overdrive.");
        ImGui.Checkbox("target art (local pack)##odt", ref _od_tgt_art);
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.None)) ImGui.SetTooltip("Textured medallions for the targets from the art pack in the mod folder's art\\targets (the optional download ffxvr-target-art, AI-generated, or your own made with tools\\target-art; not in the mod's zip). Without the pack, or off, the flat plates.");
        ImGui.SameLine(); ImGui.TextDisabled(esc(odg_text(6)));
        ImGui.Checkbox("show the button letter (small)##odt", ref _od_tgt_badge);
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.None)) ImGui.SetTooltip("Every target shows one big arrow: the way to cut (A down, B right, X left, Y up, L1 left, R1 right; the d-pad as itself). Tick this to also show the button's letter, small, at the bottom of the rim.");
        ImGui.SameLine(); ImGui.Checkbox("hide the game's Bushido row (from the next launch)##odt", ref _od_tgt_hide2d);
        if (ImGui.IsItemHovered(ImGuiHoveredFlags.None)) ImGui.SetTooltip(I($"From just before a strike-the-targets window opens until it closes, the game's own row of Bushido buttons is left out of the UI panel (it is drawn above the targets in the headset). The timer stays. Takes effect at the next launch. Two ways, either is enough: the detour on 0x892740 ({_odt_row_status}; {_odt_row_check}; calls {_odt_row_calls}, hidden {_odt_row_hidden}, faults {_odt_row_faults}) and the call-site patch at 0x89B411 ({odt_patch_name(_odt_patch_state)}). Each Bushido's odt line in ffxvr_diag.log says which ran."));

        ImGui.TextDisabled(replay.Length > 0 ? "last Bushido seen: " + esc(replay) : "Replay last Bushido: after one Bushido this session");
    }
    /// <summary>F8 Debug: the test aid and Auron's raw sequence.</summary>
    private void odg_debug_ui() {
        if (!ImGui.TreeNode("Overdrive gestures (test aid)")) return;
        if (diag_any()) {
            ImGui.Checkbox("overdrives always ready (the game's debug flag, in every battle this session; not saved; the gauge is not spent while on, so a later save keeps a full gauge)", ref _od_always_ready);
            ImGui.SameLine(); ImGui.TextDisabled(_od_always_wrote ? "set in this battle" : "");
        }
        else ImGui.TextDisabled("overdrives always ready: only with a diagnostic logging profile (e.g. diag_verbose=1), never in Release");
        ImGui.TextDisabled("Auron's expected sequence (raw, the last read): " + esc(odg_text(2)));
        ImGui.TreePop();
    }
}
