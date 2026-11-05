// SPDX-License-Identifier: MIT

namespace FFXVR;

// FMV sync probe (S2 of the FMV depth plan, 4 Nov 2026; design-notes "FMV sync probe (S2, 4 Nov 2026)"): F8's one FMV line.
// The per-FMV log lines are native's (ffxvr_native.cpp fmv_frame, ffxvr_fmv.h, which lists every field with its evidence);
// this line is read here only while the Debug header is drawn, with the same guarded reads (ffxvr_safe_read), so no export
// changed. Module offsets (VA - 0x400000), as FhUtil.ptr_at takes them:
//   0x8DED3C  dword  the movie manager (VA 0xCDED3C), 0x750 bytes; +0x6D0 playing, +0x6D8 the game's frame number (0xFFFE =
//                    ended), +0x6DC the video id, +0x6FC paused, +0x4F0 char[0x80] the file name; the embedded player at
//                    +0x38: +0x130 u64 the duration in ms, +0x138 u32 frames uploaded (the frame on screen is that - 1)
//   0x840E28  dword  the event's movie id (-1 none)
// The depth file: <mod>\fmvdepth\<name lower-cased without .webm>.fvd (tools/fmvdepth/FORMAT.md), its 128-byte header read
// on a pool thread (review SF1: no file I/O on the render thread; a cloud-only file is not opened) with FileShare.Read |
// Delete (the converter renames over it while the game runs), only while a movie plays (review N2), at most once per 5 s
// per name; "checking..." until the first answer. Per frame nothing is allocated unless something shown changed (N2).
public sealed unsafe partial class FFXVRModule {

    private readonly byte[] _fmv_name_raw = new byte[FmvSync.k_name_len];   // the name bytes last turned into _fmv_name
    private string _fmv_name = "";
    private string _fmv_fvd_for = "";                 // the game name the cached check is for
    private FmvSync.Fvd _fmv_fvd;                     // ... and its result (k_checking until the first answer)
    private long _fmv_fvd_tick;                       // ... and when it was asked (Environment.TickCount64)
    private System.Threading.Tasks.Task<FmvSync.Fvd>? _fmv_fvd_task;
    private FmvSync.State _fmv_last_st; private int _fmv_last_ev = int.MinValue, _fmv_last_view = int.MinValue, _fmv_last_fvd_st = -1; private uint _fmv_last_fvd_n;
    private string _fmv_line = "";

    /// <summary>The Debug header's FMV line (render thread, only while the header is drawn).</summary>
    private void fmv_debug_ui() => ImGui.TextDisabled(esc(fmv_line()));

    private string fmv_line() {
        if (_fn_safe_read == null) return "FMV: not read (no native helper)";
        if (!read(FhUtil.ptr_at<uint>(FmvSync.k_mgr), out uint mgr) || mgr == 0) return "FMV: no movie player";
        byte* s = stackalloc byte[FmvSync.k_s_len]; byte* p = stackalloc byte[FmvSync.k_p_len]; byte* n = stackalloc byte[FmvSync.k_name_len];
        byte* m = (byte*)(nint)mgr;
        if (_fn_safe_read(m + FmvSync.k_s_lo, s, FmvSync.k_s_len) == 0 || _fn_safe_read(m + FmvSync.k_p_lo, p, FmvSync.k_p_len) == 0
            || _fn_safe_read(m + FmvSync.k_n_lo, n, FmvSync.k_name_len) == 0) return "FMV: the movie player is not readable";
        var raw = new ReadOnlySpan<byte>(n, FmvSync.k_name_len);
        if (!raw.SequenceEqual(_fmv_name_raw)) { raw.CopyTo(_fmv_name_raw); _fmv_name = FmvSync.Name(raw); }
        var st = FmvSync.Parse(new ReadOnlySpan<byte>(s, FmvSync.k_s_len), new ReadOnlySpan<byte>(p, FmvSync.k_p_len), _fmv_name);
        read(FhUtil.ptr_at<int>(FmvSync.k_movie_id), out int ev_id);
        int view = !_xr_on ? -1 : _fn_xr_screen_now != null ? _fn_xr_screen_now() : -2;
        var fvd = st.playing && st.name.Length > 0 ? fmv_fvd(st.name) : default;   // N2: the file only while a movie plays
        if (_fmv_line.Length > 0 && FmvSync.Same(st, _fmv_last_st) && ev_id == _fmv_last_ev && view == _fmv_last_view && fvd.status == _fmv_last_fvd_st && fvd.frames == _fmv_last_fvd_n) return _fmv_line;
        _fmv_last_st = st; _fmv_last_ev = ev_id; _fmv_last_view = view; _fmv_last_fvd_st = fvd.status; _fmv_last_fvd_n = fvd.frames;
        return _fmv_line = FmvSync.Line(st, ev_id, view, fvd);
    }

    /// <summary>The depth file's header check: asked on a pool thread, cached per name for 5 s; the last answer (or
    /// "checking...") meanwhile.</summary>
    private FmvSync.Fvd fmv_fvd(string game_name) {
        var t = _fmv_fvd_task;
        if (t != null) {
            if (!t.IsCompleted) return game_name == _fmv_fvd_for ? _fmv_fvd : new FmvSync.Fvd { status = FmvSync.k_checking };   // one check at a time
            _fmv_fvd_task = null;
            if (game_name == _fmv_fvd_for)   // an answer for a name no longer shown is dropped
                _fmv_fvd = t.Status == System.Threading.Tasks.TaskStatus.RanToCompletion ? t.Result : new FmvSync.Fvd { status = FmvSync.k_unreadable };
        }
        long now = Environment.TickCount64;
        if (game_name == _fmv_fvd_for && now - _fmv_fvd_tick < 5000) return _fmv_fvd;
        if (game_name != _fmv_fvd_for) _fmv_fvd = new FmvSync.Fvd { status = FmvSync.k_checking };   // the same name keeps its last answer meanwhile
        _fmv_fvd_for = game_name; _fmv_fvd_tick = now;
        string? file = FmvSync.FileName(game_name);
        if (file == null) return _fmv_fvd = new FmvSync.Fvd { status = FmvSync.k_no_name };
        string dir = _h2_mod_dir;
        _fmv_fvd_task = System.Threading.Tasks.Task.Run(() => fmv_fvd_read(dir, file, game_name));
        return _fmv_fvd;
    }

    /// <summary>Pool thread: the attributes first (a cloud-only file is not opened, so not hydrated), then 128 bytes.</summary>
    private static FmvSync.Fvd fmv_fvd_read(string mod_dir, string file, string game_name) {
        var r = new FmvSync.Fvd { status = FmvSync.k_absent, file = file };
        try {
            if (mod_dir.Length == 0) return r;
            string path = Path.Join(mod_dir, "fmvdepth", file);
            var fi = new FileInfo(path);
            if (!fi.Exists) return r;
            if (FmvSync.CloudOnly((int)fi.Attributes)) return new FmvSync.Fvd { status = FmvSync.k_cloud, file = file };
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete, 1, FileOptions.None);
            var h = new byte[128];
            int got = 0;
            while (got < 128) { int k = fs.Read(h, got, 128 - got); if (k <= 0) break; got += k; }
            r = FmvSync.ParseFvd(h.AsSpan(0, got), fs.Length, game_name); r.file = file;
        }
        catch (Exception) { r = new FmvSync.Fvd { status = FmvSync.k_unreadable, file = file }; }
        return r;
    }
}

/// <summary>The FMV line's pure part (offsets, the header check of FORMAT.md, the text). Kept free of game types for the
/// scratch test; the native ffxvr_fmv.h makes the same checks for the log.</summary>
internal static class FmvSync {
    public const int k_mgr = 0x8DED3C, k_movie_id = 0x840E28;   // VA 0xCDED3C, 0xC40E28
    public const int k_s_lo = 0x6D0, k_s_len = 0x80, k_p_lo = 0x130, k_p_len = 0x5C, k_n_lo = 0x4F0, k_name_len = 0x80;
    public const uint k_frame_end = 0xFFFE;

    public struct State { public bool playing, paused; public uint frame, id, count; public ulong dur_ms; public string name; }
    static uint U32(ReadOnlySpan<byte> b, int o) => (uint)(b[o] | b[o + 1] << 8 | b[o + 2] << 16 | b[o + 3] << 24);
    static int U16(ReadOnlySpan<byte> b, int o) => b[o] | b[o + 1] << 8;
    static ulong U64(ReadOnlySpan<byte> b, int o) => U32(b, o) | (ulong)U32(b, o + 4) << 32;

    /// <summary>s: mgr + 0x6D0 (0x80 bytes), p: mgr + 0x130 (0x5C); name: Name() of mgr + 0x4F0 (the caller keeps it).</summary>
    public static State Parse(ReadOnlySpan<byte> s, ReadOnlySpan<byte> p, string name) =>
        new State { playing = s[0] != 0, paused = s[0x6FC - k_s_lo] != 0, frame = U32(s, 0x6D8 - k_s_lo), id = U32(s, 0x6DC - k_s_lo),
                    dur_ms = U64(p, 0x130 - k_p_lo), count = U32(p, 0x138 - k_p_lo), name = name };
    /// <summary>The name buffer (0x80 bytes, NUL-terminated or full) as printable ASCII.</summary>
    public static string Name(ReadOnlySpan<byte> n) {
        var c = new char[k_name_len]; int len = 0;
        for (; len < k_name_len && len < n.Length && n[len] != 0; len++) c[len] = n[len] >= 0x20 && n[len] <= 0x7E ? (char)n[len] : '?';
        return new string(c, 0, len);
    }
    public static bool Same(in State a, in State b) => a.playing == b.playing && a.paused == b.paused && a.frame == b.frame && a.id == b.id
        && a.count == b.count && a.dur_ms == b.dur_ms && ReferenceEquals(a.name, b.name);
    /// <summary>Offline, recall on open or recall on data access (a cloud placeholder): not opened.</summary>
    public static bool CloudOnly(int attributes) => (attributes & (0x1000 | 0x40000 | 0x400000)) != 0;

    /// <summary>The video's frame count by the game's duration (29.97 fps = 30000/1001), rounded.</summary>
    public static uint FramesFromMs(ulong ms) => (uint)(ms * 30.0 / 1001.0 + 0.5);

    /// <summary>"OPN_us.webm" -> "opn_us.fvd" (FORMAT.md; the converter's filter [A-Za-z0-9_]+.webm, no device name), else null.</summary>
    public static string? FileName(string game) {
        int slash = Math.Max(game.LastIndexOf('/'), game.LastIndexOf('\\'));   // the game stores the full archive path
        if (slash >= 0) game = game.Substring(slash + 1);
        if (game.Length <= 5 || !game.EndsWith(".webm", StringComparison.OrdinalIgnoreCase)) return null;
        string stem = game.Substring(0, game.Length - 5);
        foreach (char ch in stem) if (!(ch >= 'a' && ch <= 'z' || ch >= 'A' && ch <= 'Z' || ch >= '0' && ch <= '9' || ch == '_')) return null;
        stem = stem.ToLowerInvariant();
        if (stem is "con" or "prn" or "aux" or "nul") return null;
        if (stem.Length == 4 && (stem.StartsWith("com") || stem.StartsWith("lpt")) && stem[3] >= '0' && stem[3] <= '9') return null;
        if (stem.Length + 5 > 48) return null;   // native's buffer: the same names on both sides
        return stem + ".fvd";
    }

    public const int k_ok = 1, k_absent = 2, k_no_name = 3, k_unreadable = 4, k_invalid = 5, k_checking = 6, k_cloud = 7;
    public struct Fvd { public int status; public uint frames; public string file, why; public bool name_match; }

    /// <summary>The header checks 1-5 of FORMAT.md, as ffxvr_fmv.h's fvd_parse (64-bit arithmetic).</summary>
    public static Fvd ParseFvd(ReadOnlySpan<byte> b, long size, string? game) {
        Fvd bad(string why) => new Fvd { status = k_invalid, why = why };
        if (b.Length < 128 || size < 128) return bad("shorter than a header");
        if (b[0] != (byte)'F' || b[1] != (byte)'V' || b[2] != (byte)'D' || b[3] != (byte)'1') return bad("not an .fvd file");
        if (U16(b, 4) != 1) return bad("a version this build does not know");
        int flags = b[27];
        if ((flags & 1) == 0) return bad("unfinished");
        ulong hs = (ulong)U16(b, 6), fsz = (ulong)size; uint frames = U32(b, 12);
        if (hs < 128 || U16(b, 8) == 0 || U16(b, 10) == 0) return bad("header size or frame size");
        if (b[24] > 2 || b[25] > 1) return bad("codec or predictor");
        ulong io = U64(b, 40), nidx = 32UL * frames;
        if (io < hs || io > fsz || nidx > fsz - io) return bad("index out of the file");
        if ((flags & 2) != 0) {
            ulong to = U64(b, 48), tw = (ulong)U16(b, 56), th = (ulong)U16(b, 58), tb = frames * tw * th;
            if (tw == 0 || th == 0 || to < io + nidx || to > fsz || tb > fsz - to) return bad("thumbnails out of the file");
        }
        int len = 0; while (len < 47 && b[80 + len] != 0) len++;
        string src = System.Text.Encoding.ASCII.GetString(b.Slice(80, len));
        return new Fvd { status = k_ok, frames = frames, name_match = game != null && string.Equals(src, game.Substring(Math.Max(game.LastIndexOf('/'), game.LastIndexOf('\\')) + 1), StringComparison.OrdinalIgnoreCase) };
    }

    static string FvdText(in Fvd f) => f.status switch {
        k_ok => f.name_match ? $"present ({f.frames} frames)" : $"present ({f.frames} frames; its source name differs)",
        k_absent => "absent", k_no_name => "absent (the name has no depth file form)", k_unreadable => "unreadable",
        k_invalid => "invalid (" + f.why + ")", k_checking => "checking...", k_cloud => "not checked (cloud-only)", _ => "not checked" };

    /// <summary>"FMV: OPN_us.webm id 2 frame 1234/4750 uploaded 1234, mode screen, depth file: present (4750 frames)".
    /// view: ffxvr_xr_screen_now (0 3D, 1 screen, 2 stereo screen), -1 no headset, -2 unknown.</summary>
    public static string Line(in State st, int event_id, int view, in Fvd fvd) {
        string mode = view switch { 0 => "3D", 1 => "screen", 2 => "stereo screen", -1 => "monitor (no headset)", _ => "?" };
        string nm = st.name.Length > 0 ? st.name : "(no name)";
        if (!st.playing)
            return FormattableString.Invariant($"FMV: none playing (last {nm} id {st.id}, uploaded {st.count}{(st.frame == k_frame_end ? ", ended" : "")}; event movie id {event_id}), mode {mode}");
        string total = fvd.status == k_ok ? fvd.frames.ToString(System.Globalization.CultureInfo.InvariantCulture) : "~" + FramesFromMs(st.dur_ms).ToString(System.Globalization.CultureInfo.InvariantCulture);
        string frame = st.frame == k_frame_end ? "END" : st.frame.ToString(System.Globalization.CultureInfo.InvariantCulture);
        return FormattableString.Invariant($"FMV: {nm} id {st.id} frame {frame}/{total} uploaded {st.count}{(st.paused ? " (paused)" : "")}, mode {mode}, depth file: {FvdText(fvd)}");
    }
}
