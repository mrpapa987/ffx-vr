// SPDX-License-Identifier: MIT

namespace FFXVR;

// Release prep: the logging profile and the version (docs/release.md section 5; tools/set-logging.ps1).
//
// What a fresh install logs (no ffxvr.ini: every diag_* key at its code default, all off):
//   ffxvr_diag.log   started fresh each session (the previous one is kept as ffxvr_diag.prev.log), capped at 4 MB a session
//                    (64 MB with diag_verbose=1; ffxvr_native.cpp diag_open_ex). It opens with the versions and the
//                    diagnostics that are on (log_hello), then only events: the headset session, view-mode changes, errors,
//                    loading holds ("sr: hold"), the perf: line every 30 s, settings notes.
//   ffxvr_native.log a fixed-size status snapshot (hook counters), rewritten every 30 s.
//   Fahrenheit's own log for this module: events only (settings, battle follow, map changes).
// diag_verbose=1 adds the periodic lines (the native 10 s summaries ui:, view:, uidepth:, imgui:, sr:; the 1 s cursor art:
// line; the shadow per-map lines; the camera trace ffxvr_trace.log; the legacy markers' 1 s "window:" line and the blitzball
// positions in Fahrenheit's log) and lets Ctrl+F9 write a constant-buffer dump. The other diag_* keys are their own
// features' diagnostics, all default 0. The captures (pick, census, sequence, Measure, bones) stay on their buttons.
//
// The version: ffxvr.csproj's <Version> is the assembly's informational version (and the manifest's); the native DLL
// reports its own from ffxvr_version.h (tools/build-mod.ps1). Both show in the overlay and open ffxvr_diag.log.
public sealed unsafe partial class FFXVRModule {

    private bool _diag_verbose = false;        // ini diag_verbose (default 0; an ini older than rev 2 without the key reads 1: settings_migrate)
    private bool _diag_verbose_read;           // the launch read saw the key (settings_migrate)
    private const int k_log_cap_mb = 4, k_log_cap_verbose_mb = 64;
    private int _log_pushed = -1;              // the diag_verbose value last sent to native (-1: not yet)
    private bool _log_hello_done;
    private string _native_version = "?";
    private double _log_size_at; private long _log_size = -1;   // the Debug line's file size, read once a second
    private delegate* unmanaged[Cdecl]<int, int, void> _fn_log_config;   // ABI 26
    private delegate* unmanaged[Cdecl]<byte*> _fn_build_info;             // ABI 26

    private static readonly string s_mod_version = assembly_version(typeof(FFXVRModule).Assembly);
    private static readonly string s_fh_version  = assembly_version(typeof(FhModule).Assembly);

    /// <summary>An assembly's informational version ("0.1.0-alpha.1+&lt;commit&gt;"), the commit cut to 7 characters.</summary>
    private static string assembly_version(System.Reflection.Assembly a) {
        try {
            string v = System.Reflection.CustomAttributeExtensions.GetCustomAttribute<System.Reflection.AssemblyInformationalVersionAttribute>(a)?.InformationalVersion
                       ?? a.GetName().Version?.ToString() ?? "?";
            int plus = v.IndexOf('+');
            if (plus >= 0 && v.Length > plus + 8) v = v[..(plus + 8)];
            return v;
        }
        catch { return "?"; }
    }

    /// <summary>Every diagnostic key that is on (the keys tools/set-logging.ps1 -Profile Release turns off).</summary>
    private List<string> diag_on() {
        var on = new List<string>();
        if (_diag_verbose) on.Add("diag_verbose");
        if (_diag7) on.Add("diag_build7");
        if (_diag_load) on.Add("diag_load");
        if (_diag_load_lines) on.Add("diag_load_lines");
        if (_diag_actors) on.Add("diag_actors");
        if (_diag_cluster_bytes) on.Add("diag_cluster_bytes");
        if (_diag_va_log) on.Add("diag_va_log");
        if (_diag_transition) on.Add("diag_transition");
        if (_diag_load_prof != 0) on.Add(I($"diag_load_prof={_diag_load_prof}"));
        return on;
    }

    /// <summary>load_native, right after the ABI check: the native version, and the log settings before ffxvr_init (which
    /// starts the camera trace only with diag_verbose=1).</summary>
    private void log_native_loaded() {
        try { if (_fn_build_info != null) _native_version = Marshal.PtrToStringAnsi((nint)_fn_build_info()) ?? "?"; } catch { _native_version = "?"; }
        log_push();
        var on = diag_on();
        _logger.Info($"FFX VR {s_mod_version}, native {_native_version}; diagnostics on: {(on.Count > 0 ? string.Join(", ", on) : "none")}");
    }

    /// <summary>From push_all, every frame: diag_verbose and the cap, sent only when they change.</summary>
    private void log_push() {
        int want = _diag_verbose ? 1 : 0;
        if (_fn_log_config == null || want == _log_pushed) return;
        _log_pushed = want;
        _fn_log_config(want, _diag_verbose ? k_log_cap_verbose_mb : k_log_cap_mb);
    }

    /// <summary>Once, at the first ready frame: the head of this session's ffxvr_diag.log (after the native build line).</summary>
    private void log_hello() {
        if (_log_hello_done || _fn_diag_line == null) return;
        _log_hello_done = true;
        var on = diag_on();
        d7_log(I($"FFX VR {s_mod_version} | native {_native_version} | Fahrenheit {s_fh_version} | .NET {Environment.Version} | Windows {Environment.OSVersion.Version}"));
        d7_log(I($"settings: {(_ini_len_seen < 0 ? "no ffxvr.ini yet (defaults)" : I($"ffxvr.ini, {_ini_loaded_keys} keys, rev {_ini_rev_read}"))} | headset_autostart {(_headset_autostart ? 1 : 0)}, start_view {_start_view} | diagnostics on: {(on.Count > 0 ? string.Join(", ", on) : "none")} | log cap {(_diag_verbose ? k_log_cap_verbose_mb : k_log_cap_mb)} MB"));
    }

    /// <summary>The main window's last line: the version, and a reminder while diagnostics are on.</summary>
    private void log_main_ui() {
        ImGui.TextDisabled(esc($"FFX VR {s_mod_version}   native {_native_version}"));
        int n = diag_on().Count;
        if (n > 0) { ImGui.SameLine(); ImGui.TextDisabled(I($"| diagnostics: {n} on (Debug)")); }
    }

    /// <summary>Top of Debug: the versions, which diagnostics are on, diag_verbose, and the log's size.</summary>
    private void log_debug_ui() {
        ImGui.TextDisabled(esc($"FFX VR {s_mod_version} | native {_native_version} | Fahrenheit {s_fh_version}"));
        var on = diag_on();
        if (on.Count > 0) {
            ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.8f, 0.3f, 1f));
            ImGui.TextWrapped(esc(I($"diagnostics: {on.Count} on ({string.Join(", ", on)}). A player's install has them all off: tools\\set-logging.ps1 -Profile Release")));
            ImGui.PopStyleColor(1);
        }
        else ImGui.TextDisabled("diagnostics: all off (the release default; tools\\set-logging.ps1 -Profile Dev turns the developer's set on)");
        ImGui.Checkbox("verbose log (diag_verbose): the periodic summaries in ffxvr_diag.log, the camera trace, Ctrl+F9 dumps; log cap 64 MB instead of 4", ref _diag_verbose);
        if (_now >= _log_size_at) {
            _log_size_at = _now + 1.0;
            try { var fi = new FileInfo(Path.Combine(Path.GetDirectoryName(_ini_path) ?? ".", "ffxvr_diag.log")); _log_size = fi.Exists ? fi.Length : -1; } catch { _log_size = -1; }
        }
        ImGui.TextDisabled(_log_size < 0 ? "ffxvr_diag.log: not written yet this session"
                                         : I($"ffxvr_diag.log: {_log_size / 1024} KB this session (cap {(_diag_verbose ? k_log_cap_verbose_mb : k_log_cap_mb)} MB; the previous session is ffxvr_diag.prev.log)"));
    }
}
