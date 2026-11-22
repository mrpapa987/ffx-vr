// SPDX-License-Identifier: MIT

namespace FFXVR;

// crash-1440: the managed side's last words. The owner's 2560x1440 session exited with 0xC0000409 and no dump at the headset
// start. A managed exception that escapes a callback the game or Fahrenheit calls (a game-function detour on the main thread,
// anything Fahrenheit's Present hook runs outside render_imgui's own catch) ends the process through .NET's fail-fast:
// 0xC0000409, and no dump, since Fahrenheit's stage 0 does not dump a first-chance managed exception. Two hooks, installed
// first thing at init:
//   - AppDomain.UnhandledException: the exception, logged before the runtime ends the process (when the runtime raises it);
//   - FirstChanceException: the first 20 distinct exception sites of the session (type and throwing method), one line each,
//     so the last one before a fail-fast is in the logs even when UnhandledException is never raised. Handled exceptions
//     show up there too (an unreadable ini, a busy file); that is expected, and the cap keeps it to a few lines.
// Both write to the module log and, once the native helper is bound, to ffxvr_diag.log (d7_log), and never throw.
public sealed unsafe partial class FFXVRModule {

    private const int k_fce_max = 20;
    private readonly HashSet<string> _fce_seen = new();
    private int _fce_lines = 0;
    [ThreadStatic] private static bool t_fce_busy;

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
        if (t_fce_busy || _fce_lines >= k_fce_max) return;
        t_fce_busy = true;   // anything below that throws must not log itself
        try {
            Exception e = a.Exception;
            string site = $"{e.GetType().FullName} at {e.TargetSite?.DeclaringType?.Name}.{e.TargetSite?.Name}";
            bool add;
            lock (_fce_seen) { add = _fce_lines < k_fce_max && _fce_seen.Add(site); if (add) _fce_lines++; }
            if (!add) return;
            string s = $"crash: first-chance managed exception #{_fce_lines} on thread {Environment.CurrentManagedThreadId}: {site}: {e.Message}{(_fce_lines == k_fce_max ? " (no more of these lines this session)" : "")}";
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
