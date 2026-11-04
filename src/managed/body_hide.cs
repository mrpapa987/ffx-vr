// SPDX-License-Identifier: MIT

namespace FFXVR;

// The first-person body hide's on-foot ownership (design-notes "NPCs vanishing in first-person cutscenes (4 Nov 2026)").
// Native hides any skinned draw within hide_radius + the step margin of the hide centre; an NPC standing beside Tidus in a
// cutscene (Auron on lc/lchb0500, Yuna on lu/luca0600) went with him whenever the margin widened. Every frame push_player sends
// native the followed actor's drawn root and a re-pick key (index and model: a new key resets the margin), and in the field, in
// first person on foot with the body hide on, the drawn roots (+0x200) of the other live actors within 30 units: a draw nearer
// another actor's root than the player's is not the player's (ffxvr_fpown.h). Riding is unchanged (native's chocobo rule).
// npc-anchor (ABI 39): with the roots go the actors' head sub-positions. Two actors on one root (a scene anchor: the event Tidus
// and Yuna on mi/mihn0100) tie every draw; native resolves a tie by the model whose palette carries a head at the tied actor's head.
public sealed unsafe partial class FFXVRModule {

    private delegate* unmanaged[Cdecl]<float, float, float, int, int, float*, int> _fn_fp_self_set;    // ABI 37; ABI 38: returns 1 when the coming frame uses the other roots
    private delegate* unmanaged[Cdecl]<float*, float*, int, int> _fn_fp_others_set;                   // ABI 37; ABI 39: + the heads
    private float _fph_radius = 0.75f;   // native: the on-foot hide radius in effect (hide_radius + the capped margin)
    private int   _fph_kept, _fph_others_n;   // native: last frame's draws refused as another actor's; the roots sent this frame

    /// <summary>push_player, right after ffxvr_player_set (render thread): the followed actor's drawn root (game space; root_kind
    /// 0 = not known, native then uses the hide centre alone; 1 = read at Present; 2 = the smooth anchor's snapshot, used alone)
    /// and the re-pick key (-1 none), and in the field the other actors' roots near it. centre (game space) is where the 30-unit
    /// range is measured from when the drawn root is not known.</summary>
    private void body_hide_push(Actor* actors, int pick, int key, bool field, Vector3 root, int root_kind, Vector3 centre) {
        long t0 = cpu_now();   // cpu-lean: the cpu: line's body hide section
        body_hide_push_inner(actors, pick, key, field, root, root_kind, centre);
        _cpu_body += cpu_now() - t0;
    }
    private void body_hide_push_inner(Actor* actors, int pick, int key, bool field, Vector3 root, int root_kind, Vector3 centre) {
        // cpu-lean (c): native says whether the frame about to render can use the other actors' roots at all (ABI 38: 0 = the
        // frame goes to the virtual screen, as the menus and the sphere grid do: the override is suspended there, and every
        // user of the roots needs it; fixed until the next Present). Then nothing is walked: n = 0, which those draws ignore.
        bool used = false;
        // review SF2: the re-pick key also carries a scene generation (the map name, the event id and the followed actor's model
        // instance +0x830, which a load re-creates): a map change or a load in the same slot and model is a new key, so native
        // starts a new texture table and a recycled texture address cannot inherit the last scene's verdict.
        if (key >= 0) key = (key & 0x3FFFFF) | (int)((body_hide_scene_gen(actors, pick) & 0x1FF) << 22);
        if (_fn_fp_self_set != null) { float r = 0f; used = _fn_fp_self_set(-root.X, -root.Y, root.Z, root_kind, key, &r) != 0; _fph_radius = r; }
        if (_fn_fp_others_set == null) return;
        float* buf = stackalloc float[48 * 3]; float* dist = stackalloc float[48]; float* heads = stackalloc float[48 * 3];
        int n = 0;
        // N4: only where native uses them (first person on foot, the body hide on); the table walk reads the bytes live() needs
        if (used && field && actors != null && pick >= 0 && _fp_mode == 1 && _fp_hide_body && !_ride_att) {
            Vector3 c = root_kind != 0 ? root : centre;
            // cpu-lean (c): the walk runs every frame, on this frame's snapshot of the table (one guarded read for all 48 slots,
            // shared with the follow pick) when it reads in one piece, else slot by slot as before. Same rule, same order.
            Actor* snap = tbl_snap(actors);
            var o = new Span<float>(buf, 48 * 3); var d = new Span<float>(dist, 48); var h = new Span<float>(heads, 48 * 3);
            if (snap != null) { var rd = new FphSnapReader(snap); n = FphWalk.Collect(ref rd, pick, c, o, d, h); body_hide_anchor_log(actors, snap, pick, c); }
            else if (_fn_safe_read != null) { var rd = new FphLiveReader(actors, _fn_safe_read); n = FphWalk.Collect(ref rd, pick, c, o, d, h); }   // (none bound: n = 0, as read() failing at slot 0 gave)
        }
        _fph_others_n = n;
        _fph_kept = _fn_fp_others_set(n > 0 ? buf : null, n > 0 ? heads : null, n);
    }

    // -- review SF2: the scene generation ---------------------------------------------------------------------------------
    private ulong _fph_scene_sig; private uint _fph_scene_gen;
    private uint body_hide_scene_gen(Actor* actors, int pick) {
        ulong sig = 1469598103934665603UL;
        try { sig = (sig ^ FhUtil.get_at<ulong>(0x1FCBC60)) * 1099511628211UL; sig = (sig ^ FhUtil.get_at<ulong>(0x1FCBC68)) * 1099511628211UL; } catch { }
        sig = (sig ^ (ulong)(Globals.event_id != null ? *Globals.event_id : -1)) * 1099511628211UL;
        if (actors != null && (uint)pick < 48 && read((byte*)(actors + pick) + 0x830, out nint mh)) sig = (sig ^ (ulong)mh) * 1099511628211UL;
        if (sig != _fph_scene_sig) { _fph_scene_sig = sig; _fph_scene_gen++; }
        return _fph_scene_gen;
    }

    // -- npc-anchor: who shares the followed actor's root ---------------------------------------------------------------
    // The owner's repro (mi/mihn0100): the event Tidus and Yuna on one root. A root shared legitimately can be an attachment (one
    // actor on another's bone, or both on one parent), a script override matrix, or a scene anchor. When another live, drawn
    // actor's root is within 0.05 units of the followed one's, both actors' attach fields (+0x19C parent, +0x1A0 bone, +0x1A4..
    // +0x1AC offset, +0x1B0 scale, +0x1B8 the parent's id), the override matrix pointer (+0x10C) and the head of their model chain
    // (+0x830 handle -> instances -> the first instance's segment and material counts; debug-revival.md section 3) go to the
    // module log: once per pairing (pick, other, both chr_ids, the attach words), at most 40 a session. The frame's table
    // snapshot only, plus a few guarded reads on a logged change.
    private int _fpa_lines; private ulong _fpa_last;
    private void body_hide_anchor_log(Actor* actors, Actor* snap, int pick, Vector3 c) {
        if (_fpa_lines >= 40 || snap == null || (uint)pick >= 48) return;
        for (int i = 0; i < 48; i++) {
            if (i == pick || !FphWalk.Live(snap[i].chr_enabled, (nint)snap[i].chr_name) || slot_hidden(actors, snap, i)) continue;
            Vector3 r = *(Vector3*)((byte*)(snap + i) + 0x200);
            if (!finite(r) || Vector3.DistanceSquared(r, c) > 0.05f * 0.05f) continue;
            byte* a = (byte*)(snap + pick), b = (byte*)(snap + i);
            ulong key = (ulong)pick | (ulong)i << 6 | (ulong)snap[pick].chr_id << 12 | (ulong)snap[i].chr_id << 28 | (ulong)(*(uint*)(a + 0x19C) ^ *(uint*)(b + 0x19C) * 31u) << 44;
            if (key == _fpa_last) return;
            _fpa_last = key; _fpa_lines++;
            _logger.Info(I($"body hide: actors {pick} '{actor_name(snap[pick].chr_name)}' and {i} '{actor_name(snap[i].chr_name)}' share a root ({r.X:F2}, {r.Y:F2}, {r.Z:F2}) on map {map_name()} (a scene anchor, an attachment or a script matrix?) | {pick}: {anchor_fields(actors, snap, pick)} | {i}: {anchor_fields(actors, snap, i)}") +
                         (_fpa_lines == 40 ? " (no more of these lines this session)" : ""));
            return;
        }
    }
    /// <summary>One actor's attach fields, override matrix and model chain head, for body_hide_anchor_log.</summary>
    private string anchor_fields(Actor* actors, Actor* snap, int i) {
        byte* a = (byte*)(snap + i);
        nint par = *(nint*)(a + 0x19C); int pi = par != 0 && (ulong)((byte*)par - (byte*)actors) < 48UL * (ulong)sizeof(Actor) ? (int)(((byte*)par - (byte*)actors) / sizeof(Actor)) : -1;
        string attach = par == 0 ? "not attached" : I($"attached to {(pi >= 0 ? $"actor {pi}" : $"0x{(ulong)par:X}")}, bone {*(int*)(a + 0x1A0)}, offset ({*(float*)(a + 0x1A4):F2}, {*(float*)(a + 0x1A8):F2}, {*(float*)(a + 0x1AC):F2}), scale {*(float*)(a + 0x1B0):F2}");
        string model = "model +0x830 none";
        nint h = *(nint*)(a + 0x830);
        if (h != 0) {
            model = I($"model +0x830 0x{(ulong)h:X}");
            if (read((void*)h, out uint hn) && read((byte*)h + 4, out nint inst) && inst != 0 && read((void*)inst, out nint i0) && i0 != 0) {
                uint sn = 0, mn = 0; nint seg = 0, mat = 0;
                bool ok = read((void*)i0, out seg) && seg != 0 && read((void*)seg, out sn);
                bool okm = read((byte*)i0 + 0x10, out mat) && mat != 0 && read((void*)mat, out mn);
                model += I($": {hn & 0x7FFFFFFF} instances, the first 0x{(ulong)i0:X} with {(ok ? (sn & 0x7FFFFFFF).ToString() : "?")} segments, {(okm ? (mn & 0x7FFFFFFF).ToString() : "?")} materials");
            }
        }
        return I($"{attach} (+0x1B8 {*(int*)(a + 0x1B8)}), override matrix +0x10C {(*(nint*)(a + 0x10C) == 0 ? "none" : "set")}, +0x180 0x{a[0x180]:X2}, +0x0C ({*(float*)(a + 0x0C):F1}, {*(float*)(a + 0x10):F1}, {*(float*)(a + 0x14):F1}) + offset +0x6C ({*(float*)(a + 0x6C):F2}, {*(float*)(a + 0x70):F2}, {*(float*)(a + 0x74):F2}), {model}");
    }

    // -- cpu-lean (c, e): the actor table, once a frame ------------------------------------------------------------------
    // One guarded read of all 48 slots (48 x sizeof(Actor) = 48 x 0x880 bytes, a buffer allocated once), taken the first time
    // this frame the follow pick's rescan or the body hide walks the table. It replaces up to 48 x 4 guarded reads (a
    // VirtualQuery each) with one. null when the table does not read in one piece: the callers then read slot by slot, as
    // before (their walks stop at the first unreadable slot; a table that reads whole has none).
    private Actor* _tbl_buf; private Actor* _tbl_src; private ulong _tbl_frame = ulong.MaxValue; private bool _tbl_ok;
    private Actor* tbl_snap(Actor* actors) {
        if (actors == null || _fn_safe_read == null) return null;
        if (_tbl_frame == _frames && _tbl_src == actors) return _tbl_ok ? _tbl_buf : null;
        _tbl_frame = _frames; _tbl_src = actors;
        if (_tbl_buf == null) _tbl_buf = (Actor*)Marshal.AllocHGlobal(48 * sizeof(Actor));   // kept for the session
        _tbl_ok = _fn_safe_read(actors, _tbl_buf, (uint)(48 * sizeof(Actor))) != 0;
        return _tbl_ok ? _tbl_buf : null;
    }
    /// <summary>The script-hide bits (+0x180 &amp; 0x13, FP-H1) of slot i: from the snapshot when there is one this frame.</summary>
    private bool slot_hidden(Actor* actors, Actor* snap, int i) => snap != null ? (((byte*)(snap + i))[0x180] & 0x13) != 0 : script_hidden(actors + i);

    /// <summary>The body hide walk's reads, live (slot by slot, guarded: the code before cpu-lean).</summary>
    private readonly struct FphLiveReader : IFphReader {
        private readonly Actor* _t; private readonly delegate* unmanaged[Cdecl]<void*, void*, uint, int> _rd;
        public FphLiveReader(Actor* t, delegate* unmanaged[Cdecl]<void*, void*, uint, int> rd) { _t = t; _rd = rd; }
        public bool Enabled(int i, out int v) { ushort e = 0; bool ok = _rd(&_t[i].chr_enabled, &e, sizeof(ushort)) != 0; v = e; return ok; }   // (Actor.chr_enabled is a ushort)
        public bool NamePtr(int i, out nint v) { nint p = 0; bool ok = _rd(&_t[i].chr_name, &p, (uint)sizeof(nint)) != 0; v = p; return ok; }
        public bool Hidden(int i) { byte h = 0; return _rd((byte*)(_t + i) + 0x180, &h, 1) != 0 && (h & 0x13) != 0; }
        public bool Root(int i, out Vector3 v) { Vector3 r = default; bool ok = _rd((byte*)(_t + i) + 0x200, &r, (uint)sizeof(Vector3)) != 0; v = r; return ok; }
        public bool Head(int i, out Vector3 v) {
            Vector4* op = stackalloc Vector4[26]; Vector3 pos = default; nint par = 0; v = default;
            if (_rd((byte*)(_t + i) + 0x524, op, 26 * 16) == 0 || _rd((byte*)(_t + i) + 0x0C, &pos, (uint)sizeof(Vector3)) == 0 || _rd((byte*)(_t + i) + 0x19C, &par, (uint)sizeof(nint)) == 0) return false;
            return FphWalk.HeadOf(new ReadOnlySpan<Vector4>(op, 26), pos, par != 0, out v);
        }
    }
    /// <summary>... and from this frame's snapshot of the table (plain memory).</summary>
    private readonly struct FphSnapReader : IFphReader {
        private readonly Actor* _t;
        public FphSnapReader(Actor* t) { _t = t; }
        public bool Enabled(int i, out int v) { v = _t[i].chr_enabled; return true; }
        public bool NamePtr(int i, out nint v) { v = (nint)_t[i].chr_name; return true; }
        public bool Hidden(int i) => (((byte*)(_t + i))[0x180] & 0x13) != 0;
        public bool Root(int i, out Vector3 v) { v = *(Vector3*)((byte*)(_t + i) + 0x200); return true; }
        public bool Head(int i, out Vector3 v) => FphWalk.HeadOf(new ReadOnlySpan<Vector4>((byte*)(_t + i) + 0x524, 26), *(Vector3*)((byte*)(_t + i) + 0x0C), *(nint*)((byte*)(_t + i) + 0x19C) != 0, out v);
    }

    /// <summary>The followed actor's drawn root for the ownership test (game space): 2 = on foot, the smooth anchor's post-join
    /// snapshot (what this frame draws) when it was used; 1 = +0x200 read now (up to a tick off); riding, the rider's root (1:
    /// native's riding rule does not use it); 0 = none (native then compares with the hide centre).</summary>
    private int body_hide_root(Actor* actors, int pick, bool attached, out Vector3 root) {
        root = default;
        if (attached) { root = _ride_rider; return 1; }
        if (_sa_used && finite(_r7j.td)) { root = _r7j.td; return 2; }
        if (actors == null || pick < 0 || !read((byte*)(actors + pick) + 0x200, out Vector3 td) || !finite(td)) return 0;
        if (MathF.Abs(td.X) + MathF.Abs(td.Y) + MathF.Abs(td.Z) < 0.01f) return 0;
        root = td; return 1;
    }

    /// <summary>The First person / follow section's body hide line (F8, Debug).</summary>
    private void body_hide_ui() {
        if (_fn_fp_self_set == null) return;
        if (_ride_att) { ImGui.TextDisabled(esc($"body hide: riding (the chocobo rule, radius {_ride_hide_radius:F1})")); return; }
        ImGui.Text(esc($"body hide: radius {_fph_radius:F2}, NPC parts kept visible last frame {_fph_kept}") + "  "); ImGui.SameLine();
        ImGui.TextDisabled(esc($"(other actors' roots sent: {_fph_others_n}; a draw nearer one of them than the player's root is theirs; a draw tied with one, a shared scene anchor, goes by its model)"));
    }
}

/// <summary>cpu-lean (c): the body hide walk's reads of one actor slot (live, guarded, or from the frame's snapshot).</summary>
internal interface IFphReader {
    bool Enabled(int i, out int v);       // chr_enabled
    bool NamePtr(int i, out nint v);      // chr_name (the pointer only: live() tests its range, the walk never reads the name)
    bool Hidden(int i);                   // the game's script hide, +0x180 & 0x13 (false when unreadable: script_hidden's rule)
    bool Root(int i, out Vector3 v);      // the drawn root, +0x200
    bool Head(int i, out Vector3 v);      // npc-anchor: the head sub-position (chr_optpos +0x524, push_player's rule; FphWalk.HeadOf), game space
}

/// <summary>The body hide's table walk (npc-hide N4, unchanged by cpu-lean apart from where the bytes come from): the other live,
/// not script-hidden actors with a built drawn root within k_range of c, as render-space roots, nearest k_max first. Pure: the
/// scratch test drives it with fake tables. Returns n; o gets x y z per root, d the distances.</summary>
internal static class FphWalk {
    public const int k_max = 32;      // native keeps at most 32 (fpown::k_others_max)
    public const float k_range = 30f; // units around the followed actor's root (an NPC's parts sit on its own root)
    public const float k_head_range = 12f;   // npc-anchor: heads are read for roots this near (a tie needs a root within the hide radius + 0.5 of a matched draw)
    public static bool Live(int enabled, nint name) => enabled == 1 && name > 0x10000 && name < 0x7FFF0000;   // = FFXVRModule.live
    public static bool Finite(Vector3 v) => float.IsFinite(v.X) && float.IsFinite(v.Y) && float.IsFinite(v.Z) && MathF.Abs(v.X) < 1e5f && MathF.Abs(v.Y) < 1e5f && MathF.Abs(v.Z) < 1e5f;   // = FFXVRModule.finite
    public static int Collect<R>(ref R rd, int pick, Vector3 c, Span<float> o, Span<float> d) where R : struct, IFphReader => Collect(ref rd, pick, c, o, d, default);
    /// <summary>npc-anchor: with h (x y z per root, render space; all zero = not known; empty = not wanted) the actors' head
    /// sub-positions in the same order, for roots within k_head_range of c. o, d and n are those of the call without h.</summary>
    public static int Collect<R>(ref R rd, int pick, Vector3 c, Span<float> o, Span<float> d, Span<float> h) where R : struct, IFphReader {
        int n = 0;
        for (int i = 0; i < 48; i++) {
            if (!rd.Enabled(i, out int en)) break;   // (the other table walks stop at the first unreadable slot too)
            if (i == pick || en != 1 || !rd.NamePtr(i, out nint nm)) continue;
            if (!Live(en, nm) || rd.Hidden(i)) continue;   // live()'s own rule on the two fields; a script-hidden actor is not drawn
            if (!rd.Root(i, out Vector3 td) || !Finite(td)) continue;
            if (MathF.Abs(td.X) + MathF.Abs(td.Y) + MathF.Abs(td.Z) < 0.01f) continue;   // not built (a fresh slot, a parked model)
            float dd = Vector3.Distance(td, c);
            if (!(dd < k_range)) continue;
            o[n * 3] = -td.X; o[n * 3 + 1] = -td.Y; o[n * 3 + 2] = td.Z; d[n] = dd;   // render space negates X and Y
            if (h.Length >= (n + 1) * 3) {
                Vector3 hd = default; bool hk = dd < k_head_range && rd.Head(i, out hd) && Finite(hd);
                h[n * 3] = hk ? -hd.X : 0f; h[n * 3 + 1] = hk ? -hd.Y : 0f; h[n * 3 + 2] = hk ? hd.Z : 0f;
            }
            n++;
        }
        if (n > k_max) {   // nearest first: a partial selection sort of the 32 nearest
            for (int k = 0; k < k_max; k++) {
                int b = k; for (int j = k + 1; j < n; j++) if (d[j] < d[b]) b = j;
                if (b == k) continue;
                (d[k], d[b]) = (d[b], d[k]);
                for (int q = 0; q < 3; q++) (o[k * 3 + q], o[b * 3 + q]) = (o[b * 3 + q], o[k * 3 + q]);
                if (h.Length >= n * 3) for (int q = 0; q < 3; q++) (h[k * 3 + q], h[b * 3 + q]) = (h[b * 3 + q], h[k * 3 + q]);
            }
            n = k_max;
        }
        return n;
    }
    /// <summary>npc-anchor: an actor's head as push_player picks the followed actor's (the eye): the highest sub-position (the
    /// lowest game-space Y) not on the vertical axis through the origin, folded onto the actor's position when it looks relative
    /// (more than 100 units off it), but never for an attached actor (review N3, push_player's rule: its +0x0C is not where it is;
    /// its sub-positions are built from the parent's bone and are world positions). False when none is set. Game space.</summary>
    public static bool HeadOf(ReadOnlySpan<Vector4> optpos, Vector3 pos, bool attached, out Vector3 head) {
        float hy = float.MaxValue; head = default; bool have = false;
        for (int k = 0; k < optpos.Length && k < 26; k++) { Vector4 sp = optpos[k]; if (sp.Y < hy && MathF.Abs(sp.X) + MathF.Abs(sp.Z) > 0.01f) { hy = sp.Y; head = new Vector3(sp.X, sp.Y, sp.Z); have = true; } }
        if (have && !attached && MathF.Abs(head.X - pos.X) + MathF.Abs(head.Z - pos.Z) > 100f) head += pos;
        return have;
    }
}
