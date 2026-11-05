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
// with FileShare.Read | Delete (the converter renames over it while the game runs), at most once per 5 s per name.
public sealed unsafe partial class FFXVRModule {

    private string _fmv_fvd_for = "";                 // the file name the cached check is for
    private FmvSync.Fvd _fmv_fvd;                     // ... and its result
    private long _fmv_fvd_tick;                       // ... and when (Environment.TickCount64)

    /// <summary>The Debug header's FMV line (render thread, only while the header is drawn).</summary>
    private void fmv_debug_ui() => ImGui.TextDisabled(esc(fmv_line()));

    private string fmv_line() {
        if (_fn_safe_read == null) return "FMV: not read (no native helper)";
        if (!read(FhUtil.ptr_at<uint>(FmvSync.k_mgr), out uint mgr) || mgr == 0) return "FMV: no movie player";
        byte* s = stackalloc byte[FmvSync.k_s_len]; byte* p = stackalloc byte[FmvSync.k_p_len]; byte* n = stackalloc byte[FmvSync.k_name_len];
        byte* m = (byte*)(nint)mgr;
        if (_fn_safe_read(m + FmvSync.k_s_lo, s, FmvSync.k_s_len) == 0 || _fn_safe_read(m + FmvSync.k_p_lo, p, FmvSync.k_p_len) == 0
            || _fn_safe_read(m + FmvSync.k_n_lo, n, FmvSync.k_name_len) == 0) return "FMV: the movie player is not readable";
        var st = FmvSync.Parse(new ReadOnlySpan<byte>(s, FmvSync.k_s_len), new ReadOnlySpan<byte>(p, FmvSync.k_p_len), new ReadOnlySpan<byte>(n, FmvSync.k_name_len));
        read(FhUtil.ptr_at<int>(FmvSync.k_movie_id), out int ev_id);
        int view = !_xr_on ? -1 : _fn_xr_screen_now != null ? _fn_xr_screen_now() : -2;
        var fvd = st.name.Length > 0 ? fmv_fvd(st.name) : default;
        return FmvSync.Line(st, ev_id, view, fvd);
    }

    /// <summary>The depth file's header check, cached per name for 5 s.</summary>
    private FmvSync.Fvd fmv_fvd(string game_name) {
        string? file = FmvSync.FileName(game_name);
        if (file == null) return new FmvSync.Fvd { status = FmvSync.k_no_name };
        long now = Environment.TickCount64;
        if (file == _fmv_fvd_for && now - _fmv_fvd_tick < 5000) return _fmv_fvd;
        _fmv_fvd_for = file; _fmv_fvd_tick = now;
        var r = new FmvSync.Fvd { status = FmvSync.k_absent, file = file };
        try {
            string path = Path.Join(_h2_mod_dir, "fmvdepth", file);
            if (_h2_mod_dir.Length > 0 && File.Exists(path)) {
                using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete, 1, FileOptions.None);
                Span<byte> h = stackalloc byte[128];
                int got = 0;
                while (got < 128) { int k = fs.Read(h.Slice(got)); if (k <= 0) break; got += k; }
                r = FmvSync.ParseFvd(h.Slice(0, got), fs.Length, game_name); r.file = file;
            }
        }
        catch (Exception) { r.status = FmvSync.k_unreadable; }
        _fmv_fvd = r;
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

    /// <summary>s: mgr + 0x6D0 (0x80 bytes), p: mgr + 0x130 (0x5C), n: mgr + 0x4F0 (0x80).</summary>
    public static State Parse(ReadOnlySpan<byte> s, ReadOnlySpan<byte> p, ReadOnlySpan<byte> n) {
        var st = new State { playing = s[0] != 0, paused = s[0x6FC - k_s_lo] != 0, frame = U32(s, 0x6D8 - k_s_lo), id = U32(s, 0x6DC - k_s_lo),
                             dur_ms = U64(p, 0x130 - k_p_lo), count = U32(p, 0x138 - k_p_lo) };
        var c = new char[k_name_len]; int len = 0;
        for (; len < k_name_len && len < n.Length && n[len] != 0; len++) c[len] = n[len] >= 0x20 && n[len] <= 0x7E ? (char)n[len] : '?';
        st.name = new string(c, 0, len);
        return st;
    }

    /// <summary>The video's frame count by the game's duration (29.97 fps = 30000/1001), rounded.</summary>
    public static uint FramesFromMs(ulong ms) => (uint)(ms * 30.0 / 1001.0 + 0.5);

    /// <summary>"OPN_us.webm" -> "opn_us.fvd" (FORMAT.md; the converter's filter [A-Za-z0-9_]+.webm, no device name), else null.</summary>
    public static string? FileName(string game) {
        if (game.Length <= 5 || !game.EndsWith(".webm", StringComparison.OrdinalIgnoreCase)) return null;
        string stem = game.Substring(0, game.Length - 5);
        foreach (char ch in stem) if (!(ch >= 'a' && ch <= 'z' || ch >= 'A' && ch <= 'Z' || ch >= '0' && ch <= '9' || ch == '_')) return null;
        stem = stem.ToLowerInvariant();
        if (stem is "con" or "prn" or "aux" or "nul") return null;
        if (stem.Length == 4 && (stem.StartsWith("com") || stem.StartsWith("lpt")) && stem[3] >= '0' && stem[3] <= '9') return null;
        if (stem.Length + 5 > 48) return null;   // native's buffer: the same names on both sides
        return stem + ".fvd";
    }

    public const int k_ok = 1, k_absent = 2, k_no_name = 3, k_unreadable = 4, k_invalid = 5;
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
        return new Fvd { status = k_ok, frames = frames, name_match = game != null && string.Equals(src, game, StringComparison.OrdinalIgnoreCase) };
    }

    static string FvdText(in Fvd f) => f.status switch {
        k_ok => f.name_match ? $"present ({f.frames} frames)" : $"present ({f.frames} frames; its source name differs)",
        k_absent => "absent", k_no_name => "absent (the name has no depth file form)", k_unreadable => "unreadable",
        k_invalid => "invalid (" + f.why + ")", _ => "not checked" };

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
