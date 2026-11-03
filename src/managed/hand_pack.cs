// SPDX-License-Identifier: MIT

namespace FFXVR;

using System.Text.Json;

// VR hands v2, stage S3: the hand packs under <mod>\hands\<folder>\ and their hand.json (format 1; research hands-v2-spec.md
// 1.4, 5; hands-assets.md 1). Self-contained (the BCL only: System.Text.Json), so it can be tested on its own; hands.cs owns
// the list, the F8 combo and the fallback walk, native gets a VrHandsPackDesc and never parses JSON.
//
// hand.json (every field optional; a wrong type or an out-of-range value is a note in the log and the default / the clamped
// value is used; only what makes the pack unusable is an error, and that pack is greyed out in F8 with the reason):
//   format              1 (absent: 1). A higher number: made for a newer FFX VR (error).
//   name, author, licence, source   shown in F8 (name: the folder name when absent; at most 48 / 80 characters).
//   private             true: made from the user's own model (tools/hands/import-hands.py writes it): "(private)" in F8.
//   right, left         the glb file names in the pack folder, or null. Absent both: right.glb / left.glb if they exist.
//   mirror              true: the left hand is the right mirrored (also when left.glb exists); false with both files: a
//                       two-handed pack. Default: true unless both files are given. Only a left file: the right is its mirror.
//   scale               0.5 .. 2, default 1 (multiplies vr_hands_scale).
//   wrist_offset_m      [x, y, z] metres, each +-0.10, in the hand frame (out from the thumb side, back of the hand, toward the
//                       elbow; x mirrored for the left hand), after the calibration (PackFix: spec 2 row 1 (d)).
//   wrist_rotation_deg  [yaw, pitch, roll] degrees, each +-45, in the hand frame (yaw and roll mirrored for the left hand).
//   tint                [r, g, b] 0 .. 4, multiplies the base colour; default [1, 1, 1].
//   rest_curl           {"thumb": c, "index": c, "middle": c, "ring": c, "little": c} or [5 numbers], each -1 .. 1: added to
//                       the controller pose's curl of that finger (for a model whose bind pose is not flat).
//   preview             a PNG name: accepted, not shown in this build.
// Limits (a user's folder must not be able to hurt the game): 64 pack folders, hand.json <= 64 KB, glb <= 32 MB (native has
// its own caps per hand: 65535 vertices, 393216 indices, 4 images of <= 2048 px, shrunk to 1024 at load).
internal sealed class HandPack {
    public string id = "";                       // the folder name (ini vr_hands_pack)
    public string dir = "";
    public string name = "", author = "", licence = "", source = "";
    public bool priv, shipped, has_json;
    public string? right, left;                  // absolute paths of the files used (null: not given)
    public bool mirror = true;
    public float scale = 1f;
    public readonly float[] off_m = new float[3], rot_deg = new float[3], tint = { 1f, 1f, 1f }, rest = new float[5];
    public string? error;                        // set: unusable (why)
    public readonly List<string> notes = new();  // fields clamped or ignored (logged at the scan)
    public uint stamp;                           // the glbs' sizes and write times (a changed pack loads again)
    public string? native_error;                 // the native loader's rejection, until the next rescan
    public bool usable => error == null && native_error == null;
    /// <summary>The F8 label: the name, "(private)", author and licence (no '#' pairs: ImGui's id separator).</summary>
    public string label => (name + (priv ? " (private)" : "") + (author.Length > 0 || licence.Length > 0 ? " - " + author + (author.Length > 0 && licence.Length > 0 ? ", " : "") + licence : "")).Replace("##", "#");
}

internal static class HandPackJson {
    public const int MaxPacks = 64, MaxJsonBytes = 64 * 1024, MaxName = 48, MaxText = 80;
    public const long MaxGlbBytes = 32L << 20;
    public static readonly string[] Shipped = { "generic-glove", "generic" };   // = tools\paths.ps1 $FfxvrHandPacks
    static readonly string[] k_fingers = { "thumb", "index", "middle", "ring", "little" };
    static readonly JsonDocumentOptions k_opt = new() { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip, MaxDepth = 16 };

    /// <summary>A file's (exists, size, last write ticks UTC); the scan's view of the disk (a test passes its own).</summary>
    public delegate (bool exists, long size, long ticks) Stat(string path);
    public static (bool, long, long) disk_stat(string p) {
        try { var f = new FileInfo(p); return f.Exists ? (true, f.Length, f.LastWriteTimeUtc.Ticks) : (false, 0L, 0L); }
        catch (Exception) { return (false, 0L, 0L); }
    }

    /// <summary>Every pack folder under handsDir (sorted, at most MaxPacks; a folder with neither hand.json nor a .glb is not a
    /// pack). dirNote: why nothing could be listed, or more folders than the cap; null when fine.</summary>
    public static List<HandPack> scan(string handsDir, out string? dirNote) {
        var list = new List<HandPack>(); dirNote = null;
        string[] dirs;
        try { dirs = Directory.Exists(handsDir) ? Directory.GetDirectories(handsDir) : Array.Empty<string>(); }
        catch (Exception e) { dirNote = "the hands folder could not be listed: " + e.Message; return list; }
        if (dirs.Length == 0 && !Directory.Exists(handsDir)) dirNote = "there is no hands folder (reinstall the mod)";
        Array.Sort(dirs, StringComparer.OrdinalIgnoreCase);
        int n = 0;
        foreach (string d in dirs) {
            string id = Path.GetFileName(d.TrimEnd('\\', '/'));
            if (id.Length == 0 || id[0] == '.') continue;
            bool json = File.Exists(Path.Join(d, "hand.json"));
            bool glb;
            try { glb = Directory.GetFiles(d, "*.glb").Length > 0; } catch (Exception) { glb = false; }
            if (!json && !glb) continue;
            if (++n > MaxPacks) { dirNote = I($"more than {MaxPacks} pack folders: the rest are not listed"); break; }
            string? text = null, readErr = null;
            if (json) {
                try {
                    var fi = new FileInfo(Path.Join(d, "hand.json"));
                    if (fi.Length > MaxJsonBytes) readErr = I($"hand.json is {fi.Length / 1024} KB (at most {MaxJsonBytes / 1024} KB)");
                    else text = File.ReadAllText(fi.FullName);
                }
                catch (Exception e) { readErr = "hand.json could not be read: " + e.Message; }
            }
            HandPack p = parse(id, d, text, disk_stat);
            if (readErr != null) { p.error = readErr; p.notes.Clear(); }
            list.Add(p);
        }
        return list;
    }

    /// <summary>One pack from its folder name, folder and hand.json text (null: no hand.json, the defaults). Pure but for
    /// `stat` (the glb files' existence, size and time).</summary>
    public static HandPack parse(string id, string dir, string? json, Stat stat) {
        var p = new HandPack { id = id, dir = dir, name = clean(id, MaxName), has_json = json != null };
        p.shipped = Array.Exists(Shipped, s => string.Equals(s, id, StringComparison.OrdinalIgnoreCase));
        string? rname = null, lname = null; bool r_key = false, l_key = false, mirror_set = false;
        if (json == null) p.notes.Add("no hand.json: the defaults are used");
        else {
            JsonDocument doc;
            try { doc = JsonDocument.Parse(json, k_opt); }
            catch (JsonException e) { p.error = I($"hand.json is not valid JSON (line {(e.LineNumber ?? 0) + 1}: {first_line(e.Message)})"); return p; }
            catch (Exception e) { p.error = "hand.json could not be parsed: " + first_line(e.Message); return p; }
            using (doc) {
                JsonElement root = doc.RootElement;
                if (root.ValueKind != JsonValueKind.Object) { p.error = "hand.json is not a JSON object ({ ... })"; return p; }
                var unknown = new List<string>();
                foreach (JsonProperty f in root.EnumerateObject()) {
                    JsonElement v = f.Value;
                    switch (f.Name) {
                        case "format":
                            if (v.ValueKind != JsonValueKind.Number || !v.TryGetDouble(out double fm) || fm != Math.Floor(fm)) { p.error = "format is not a whole number"; return p; }
                            if (fm > 1) { p.error = I($"format {fm} is for a newer FFX VR (this build reads format 1)"); return p; }
                            if (fm < 1) { p.error = I($"format {fm} is not a hand pack format (1)"); return p; }
                            break;
                        case "name":    p.name = text(p, f.Name, v, MaxName) ?? p.name; if (p.name.Length == 0) p.name = clean(id, MaxName); break;
                        case "author":  p.author = text(p, f.Name, v, MaxText) ?? p.author; break;
                        case "licence": case "license": p.licence = text(p, f.Name, v, MaxText) ?? p.licence; break;
                        case "source":  p.source = text(p, f.Name, v, MaxText) ?? p.source; break;
                        case "private": if (v.ValueKind == JsonValueKind.True || v.ValueKind == JsonValueKind.False) p.priv = v.GetBoolean(); else p.notes.Add("private is not true or false: ignored"); break;
                        case "right":   r_key = true; if (!file_name(p, f.Name, v, out rname)) return p; break;
                        case "left":    l_key = true; if (!file_name(p, f.Name, v, out lname)) return p; break;
                        case "mirror":
                            if (v.ValueKind == JsonValueKind.True || v.ValueKind == JsonValueKind.False) { p.mirror = v.GetBoolean(); mirror_set = true; }
                            else p.notes.Add("mirror is not true or false: ignored");
                            break;
                        case "scale":   { float s = 1f; if (number(p, f.Name, v, 0.5f, 2f, ref s)) p.scale = s; } break;
                        case "wrist_offset_m":     vec(p, f.Name, v, -0.10f, 0.10f, p.off_m); break;
                        case "wrist_rotation_deg": vec(p, f.Name, v, -45f, 45f, p.rot_deg); break;
                        case "tint":               vec(p, f.Name, v, 0f, 4f, p.tint); break;
                        case "rest_curl":          rest(p, v); break;
                        case "preview": break;   // accepted; not shown in this build
                        default: unknown.Add(f.Name); break;
                    }
                }
                if (unknown.Count > 0) p.notes.Add("unknown fields ignored: " + string.Join(", ", unknown.Count > 8 ? unknown.GetRange(0, 8) : unknown) + (unknown.Count > 8 ? ", ..." : ""));
            }
        }
        // The files: named ones must exist; with neither key, the usual names if present.
        if (!r_key && !l_key) {
            if (stat(Path.Join(dir, "right.glb")).exists) rname = "right.glb";
            if (stat(Path.Join(dir, "left.glb")).exists) lname = "left.glb";
        }
        if (rname == null && lname == null) { p.error = r_key || l_key ? "hand.json names no glb for either hand" : "no right.glb or left.glb in the folder"; return p; }
        if (!mirror_set) p.mirror = !(rname != null && lname != null);
        bool need_l = lname != null && (rname == null || !p.mirror);
        uint h = 2166136261u;
        foreach ((string? fname, bool need) in new[] { (rname, rname != null), (lname, need_l) }) {
            if (!need || fname == null) continue;
            string path = Path.Join(dir, fname);
            var (ex, size, ticks) = stat(path);
            if (!ex) { p.error = fname + " is missing" + (p.priv ? " (run tools\\import-hands.ps1 again)" : ""); return p; }
            if (size <= 0 || size > MaxGlbBytes) { p.error = I($"{fname} is {size / 1024} KB (at most {MaxGlbBytes >> 20} MB)"); return p; }
            foreach (long x in new[] { size, ticks }) for (int k = 0; k < 8; k++) { h ^= (uint)((x >> (8 * k)) & 255); h *= 16777619u; }
        }
        if (rname != null) p.right = Path.Join(dir, rname);
        if (lname != null && need_l) p.left = Path.Join(dir, lname);
        if (rname != null && lname != null && p.mirror) p.notes.Add("mirror is true: " + lname + " is not used (the left hand is the right mirrored)");
        p.stamp = h;
        return p;
    }

    static string I(FormattableString f) => FormattableString.Invariant(f);
    static string first_line(string s) {   // System.Text.Json appends " LineNumber: .. | BytePositionInLine: .." and " Path: ..": we say the line ourselves
        int i = s.IndexOfAny(new[] { '\r', '\n' }); if (i >= 0) s = s.Substring(0, i);
        foreach (string cut in new[] { " LineNumber:", " Path:" }) { int k = s.IndexOf(cut, StringComparison.Ordinal); if (k > 0) s = s.Substring(0, k); }
        return s.TrimEnd();
    }
    /// <summary>Printable text: control characters dropped, trimmed, at most max characters.</summary>
    static string clean(string s, int max) {
        var sb = new System.Text.StringBuilder(Math.Min(s.Length, max));
        foreach (char c in s) { if (sb.Length >= max) break; if (!char.IsControl(c)) sb.Append(c); }
        return sb.ToString().Trim();
    }
    static string? text(HandPack p, string key, JsonElement v, int max) {
        if (v.ValueKind == JsonValueKind.Null) return "";
        if (v.ValueKind != JsonValueKind.String) { p.notes.Add(key + " is not a string: ignored"); return null; }
        string s = v.GetString() ?? "", c = clean(s, max);
        if (c.Length < s.Trim().Length) p.notes.Add(I($"{key} shortened to {max} characters"));
        return c;
    }
    /// <summary>A plain .glb file name in the pack folder (no path: a pack cannot point the loader anywhere else); null ok.</summary>
    static bool file_name(HandPack p, string key, JsonElement v, out string? name) {
        name = null;
        if (v.ValueKind == JsonValueKind.Null) return true;
        string s = v.ValueKind == JsonValueKind.String ? (v.GetString() ?? "").Trim() : "";
        bool ok = s.Length > 4 && s.Length <= 64 && s.EndsWith(".glb", StringComparison.OrdinalIgnoreCase) && s.IndexOfAny(new[] { '/', '\\', ':', '*', '?', '"', '<', '>', '|' }) < 0
                  && s != ".." && !s.StartsWith(".", StringComparison.Ordinal);
        foreach (char c in s) if (char.IsControl(c)) ok = false;
        if (!ok) { p.error = key + (v.ValueKind == JsonValueKind.String ? ": '" + clean(s, 40) + "' is not" : " is not a string:") + " a .glb file name in the pack folder"; return false; }
        name = s; return true;
    }
    static bool number(HandPack p, string key, JsonElement v, float lo, float hi, ref float o) {
        if (v.ValueKind != JsonValueKind.Number || !v.TryGetDouble(out double d) || !double.IsFinite(d)) { p.notes.Add(key + " is not a number: ignored"); return false; }
        float f = (float)Math.Clamp(d, lo, hi);
        if (f != d && Math.Abs(f - d) > 1e-6) p.notes.Add(I($"{key} {d:G4} clamped to {f:G4}"));
        o = f; return true;
    }
    static void vec(HandPack p, string key, JsonElement v, float lo, float hi, float[] o) {
        if (v.ValueKind != JsonValueKind.Array || v.GetArrayLength() != 3) { p.notes.Add(key + " is not 3 numbers: ignored"); return; }
        var t = new float[3]; int i = 0;
        foreach (JsonElement e in v.EnumerateArray()) { float f = o[i]; if (!number(p, I($"{key}[{i}]"), e, lo, hi, ref f)) return; t[i++] = f; }
        Array.Copy(t, o, 3);
    }
    static void rest(HandPack p, JsonElement v) {
        if (v.ValueKind == JsonValueKind.Array) {
            if (v.GetArrayLength() != 5) { p.notes.Add("rest_curl is not 5 numbers (thumb, index, middle, ring, little): ignored"); return; }
            int i = 0; var t = new float[5];
            foreach (JsonElement e in v.EnumerateArray()) { float f = 0f; if (!number(p, I($"rest_curl[{i}]"), e, -1f, 1f, ref f)) return; t[i++] = f; }
            Array.Copy(t, p.rest, 5); return;
        }
        if (v.ValueKind != JsonValueKind.Object) { p.notes.Add("rest_curl is not an object or 5 numbers: ignored"); return; }
        foreach (JsonProperty f in v.EnumerateObject()) {
            int k = Array.IndexOf(k_fingers, f.Name);
            if (k < 0) { p.notes.Add("rest_curl." + clean(f.Name, 20) + ": not a finger (thumb, index, middle, ring, little): ignored"); continue; }
            float c = 0f; if (number(p, "rest_curl." + f.Name, f.Value, -1f, 1f, ref c)) p.rest[k] = c;
        }
    }
}
