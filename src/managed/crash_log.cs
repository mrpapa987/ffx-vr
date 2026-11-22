// SPDX-License-Identifier: MIT

namespace FFXVR;

// crash-1440: the managed side's last words. The owner's 2560x1440 session exited with 0xC0000409 and no dump at the headset
// start. A managed exception that escapes a callback the game or Fahrenheit calls (a game-function detour on the main thread,
// anything Fahrenheit's Present hook runs outside render_imgui's own catch) ends the process through .NET's fail-fast:
// 0xC0000409, and no dump, since Fahrenheit's stage 0 does not dump a first-chance managed exception. Two hooks, installed
// first thing at init:
//   - AppDomain.UnhandledException: the exception, logged before the runtime ends the process (when the runtime raises it);
//   - FirstChanceException: distinct exception sites (type and throwing method), one line each, so the last one before a
//     fail-fast is in the logs even when UnhandledException is never raised (whether .NET raises it for an exception that
//     escapes an UnmanagedCallersOnly callback before its fail-fast is not verified). Handled exceptions show up there too
//     (an unreadable ini, a busy file); that is expected. Two budgets (review S2): the session's first 20 sites, and,
//     separately, 20 sites in each headset start window (from the frame the headset is asked for until 30 s after the
//     session runs: crash_fce_window, from push_all), deduplicated anew in each window, so harmless sites earlier in the
//     session can never use up the lines the start needs.
//   Cost: outside a start window, an exception whose (type, HResult) pair has already been seen k_fce_pair_max times
//   returns after one dictionary lookup under a lock, before anything is formatted; otherwise TargetSite (resolved from
//   the stack trace) and a string: microseconds, only noticeable for something that throws in a hot loop, which the pair
//   cap then cuts off.
// Both write to the module log and, once the native helper is bound, to ffxvr_diag.log (d7_log), and never throw. The
// module log gets the multi-line exception text unstamped, ffxvr_diag.log stamps every line: they line up by the first line.
public sealed unsafe partial class FFXVRModule {

    private const int k_fce_max = 20;                // sites a session, and again sites a start window
    private const int k_fce_pair_max = 8;            // outside a window: a (type, HResult) pair seen this often returns at once
    private const long k_fce_window_ms = 30000;      // the start window lasts this long after the start is no longer pending
    private readonly HashSet<string> _fce_seen = new(), _fce_win_seen = new();
    private readonly Dictionary<(Type, int), int> _fce_pairs = new();
    private int _fce_lines = 0, _fce_win_lines = 0;
    private long _fce_win_until = 0;                 // Environment.TickCount64 until which the start window is open (0: never opened)
    [ThreadStatic] private static bool t_fce_busy;

    /// <summary>From push_all, every frame (render thread): pending = the headset is asked for and the session is not on yet.
    /// A window that opens anew starts its own budget.</summary>
    private void crash_fce_window(bool pending) {
        if (!pending) return;
        long now = Environment.TickCount64;
        lock (_fce_seen) {
            if (now >= _fce_win_until) { _fce_win_seen.Clear(); _fce_win_lines = 0; }   // closed (or never open): a new window
            _fce_win_until = now + k_fce_window_ms;
        }
    }

    private void crash_log_install() {
        try {
            AppDomain.CurrentDomain.UnhandledException += crash_unhandled;
            AppDomain.CurrentDomain.FirstChanceException += crash_first_chance;
            _logger.Info($"crash: unhandled and first-chance managed exceptions are logged (first-chance: the first {k_fce_max} distinct sites)");
        }
        catch (Exception e) { _logger.Warning($"crash: the exception hooks could not be installed ({e.Message})"); }
    }

    private void crash_unhandled(object? sender, UnhandledExceptionEventArgs a) {
        try {
            string s = $"crash: UNHANDLED managed exception on thread {Environment.CurrentManagedThreadId} (terminating: {a.IsTerminating}): {a.ExceptionObject}";
            _logger.Error(s);
            crash_diag(s);
        }
        catch { }
    }

    private void crash_first_chance(object? sender, System.Runtime.ExceptionServices.FirstChanceExceptionEventArgs a) {
        if (t_fce_busy) return;
        t_fce_busy = true;   // anything below that throws must not log itself
        try {
            Exception e = a.Exception;
            bool win;
            lock (_fce_seen) {   // the cheap part first: nothing is formatted for an exception neither budget can take
                win = Environment.TickCount64 < _fce_win_until && _fce_win_lines < k_fce_max;
                if (!win) {
                    if (_fce_lines >= k_fce_max) return;
                    var key = (e.GetType(), e.HResult);
                    _fce_pairs.TryGetValue(key, out int c);
                    if (c >= k_fce_pair_max) return;
                    _fce_pairs[key] = c + 1;
                }
            }
            string site = $"{e.GetType().FullName} at {e.TargetSite?.DeclaringType?.Name}.{e.TargetSite?.Name}";
            bool add_s, add_w; int n;
            lock (_fce_seen) {
                add_s = _fce_lines < k_fce_max && _fce_seen.Add(site); if (add_s) _fce_lines++;
                add_w = win && _fce_win_lines < k_fce_max && _fce_win_seen.Add(site); if (add_w) _fce_win_lines++;
                n = add_w ? _fce_win_lines : _fce_lines;
            }
            if (!add_s && !add_w) return;
            string tail = add_w ? (_fce_win_lines == k_fce_max ? " (no more of these lines in this start window)" : "") : (_fce_lines == k_fce_max ? " (no more of these lines this session outside a headset start)" : "");
            string s = $"crash: first-chance managed exception #{n}{(add_w ? " in the headset start window" : "")} on thread {Environment.CurrentManagedThreadId}: {site}: {e.Message}{tail}";
            _logger.Info(s);
            crash_diag(s);
        }
        catch { }
        finally { t_fce_busy = false; }
    }

    /// <summary>Into ffxvr_diag.log when the native helper is bound (its writer opens and closes the file per line).</summary>
    private void crash_diag(string s) {
        if (_fn_diag_line == null) return;
        d7_log(s);
    }
}
