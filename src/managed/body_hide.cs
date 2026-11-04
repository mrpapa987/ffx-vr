// SPDX-License-Identifier: MIT

namespace FFXVR;

// The first-person body hide's on-foot ownership (design-notes "NPCs vanishing in first-person cutscenes (4 Nov 2026)").
// Native hides any skinned draw within hide_radius + the step margin of the hide centre; an NPC standing beside Tidus in a
// cutscene (Auron on lc/lchb0500, Yuna on lu/luca0600) went with him whenever the margin widened. Every frame push_player sends
// native the followed actor's drawn root and a re-pick key (index and model: a new key resets the margin), and in the field, in
// first person on foot with the body hide on, the drawn roots (+0x200) of the other live actors within 30 units: a draw nearer
// another actor's root than the player's is not the player's (ffxvr_fpown.h). Riding is unchanged (native's chocobo rule).
public sealed unsafe partial class FFXVRModule {

    private delegate* unmanaged[Cdecl]<float, float, float, int, int, float*, int> _fn_fp_self_set;    // ABI 37; ABI 38: returns 1 when the coming frame uses the other roots
    private delegate* unmanaged[Cdecl]<float*, int, int> _fn_fp_others_set;                           // ABI 37
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
        if (_fn_fp_self_set != null) { float r = 0f; used = _fn_fp_self_set(-root.X, -root.Y, root.Z, root_kind, key, &r) != 0; _fph_radius = r; }
        if (_fn_fp_others_set == null) return;
        float* buf = stackalloc float[48 * 3]; float* dist = stackalloc float[48];
        int n = 0;
        // N4: only where native uses them (first person on foot, the body hide on); the table walk reads the bytes live() needs
        if (used && field && actors != null && pick >= 0 && _fp_mode == 1 && _fp_hide_body && !_ride_att) {
            Vector3 c = root_kind != 0 ? root : centre;
            // cpu-lean (c): the walk runs every frame, on this frame's snapshot of the table (one guarded read for all 48 slots,
            // shared with the follow pick) when it reads in one piece, else slot by slot as before. Same rule, same order.
            Actor* snap = tbl_snap(actors);
            var o = new Span<float>(buf, 48 * 3); var d = new Span<float>(dist, 48);
            if (snap != null) { var rd = new FphSnapReader(snap); n = FphWalk.Collect(ref rd, pick, c, o, d); }
            else { var rd = new FphLiveReader(actors, _fn_safe_read); n = FphWalk.Collect(ref rd, pick, c, o, d); }
        }
        _fph_others_n = n;
        _fph_kept = _fn_fp_others_set(n > 0 ? buf : null, n);
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
    }
    /// <summary>... and from this frame's snapshot of the table (plain memory).</summary>
    private readonly struct FphSnapReader : IFphReader {
        private readonly Actor* _t;
        public FphSnapReader(Actor* t) { _t = t; }
        public bool Enabled(int i, out int v) { v = _t[i].chr_enabled; return true; }
        public bool NamePtr(int i, out nint v) { v = (nint)_t[i].chr_name; return true; }
        public bool Hidden(int i) => (((byte*)(_t + i))[0x180] & 0x13) != 0;
        public bool Root(int i, out Vector3 v) { v = *(Vector3*)((byte*)(_t + i) + 0x200); return true; }
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
        ImGui.TextDisabled(esc($"(other actors' roots sent: {_fph_others_n}; a draw nearer one of them than the player's root is theirs)"));
    }
}

/// <summary>cpu-lean (c): the body hide walk's reads of one actor slot (live, guarded, or from the frame's snapshot).</summary>
internal interface IFphReader {
    bool Enabled(int i, out int v);       // chr_enabled
    bool NamePtr(int i, out nint v);      // chr_name (the pointer only: live() tests its range, the walk never reads the name)
    bool Hidden(int i);                   // the game's script hide, +0x180 & 0x13 (false when unreadable: script_hidden's rule)
    bool Root(int i, out Vector3 v);      // the drawn root, +0x200
}

/// <summary>The body hide's table walk (npc-hide N4, unchanged by cpu-lean apart from where the bytes come from): the other live,
/// not script-hidden actors with a built drawn root within k_range of c, as render-space roots, nearest k_max first. Pure: the
/// scratch test drives it with fake tables. Returns n; o gets x y z per root, d the distances.</summary>
internal static class FphWalk {
    public const int k_max = 32;      // native keeps at most 32 (fpown::k_others_max)
    public const float k_range = 30f; // units around the followed actor's root (an NPC's parts sit on its own root)
    public static bool Live(int enabled, nint name) => enabled == 1 && name > 0x10000 && name < 0x7FFF0000;   // = FFXVRModule.live
    public static bool Finite(Vector3 v) => float.IsFinite(v.X) && float.IsFinite(v.Y) && float.IsFinite(v.Z) && MathF.Abs(v.X) < 1e5f && MathF.Abs(v.Y) < 1e5f && MathF.Abs(v.Z) < 1e5f;   // = FFXVRModule.finite
    public static int Collect<R>(ref R rd, int pick, Vector3 c, Span<float> o, Span<float> d) where R : struct, IFphReader {
        int n = 0;
        for (int i = 0; i < 48; i++) {
            if (!rd.Enabled(i, out int en)) break;   // (the other table walks stop at the first unreadable slot too)
            if (i == pick || en != 1 || !rd.NamePtr(i, out nint nm)) continue;
            if (!Live(en, nm) || rd.Hidden(i)) continue;   // live()'s own rule on the two fields; a script-hidden actor is not drawn
            if (!rd.Root(i, out Vector3 td) || !Finite(td)) continue;
            if (MathF.Abs(td.X) + MathF.Abs(td.Y) + MathF.Abs(td.Z) < 0.01f) continue;   // not built (a fresh slot, a parked model)
            float dd = Vector3.Distance(td, c);
            if (!(dd < k_range)) continue;
            o[n * 3] = -td.X; o[n * 3 + 1] = -td.Y; o[n * 3 + 2] = td.Z; d[n] = dd; n++;   // render space negates X and Y
        }
        if (n > k_max) {   // nearest first: a partial selection sort of the 32 nearest
            for (int k = 0; k < k_max; k++) {
                int b = k; for (int j = k + 1; j < n; j++) if (d[j] < d[b]) b = j;
                if (b == k) continue;
                (d[k], d[b]) = (d[b], d[k]);
                for (int q = 0; q < 3; q++) (o[k * 3 + q], o[b * 3 + q]) = (o[b * 3 + q], o[k * 3 + q]);
            }
            n = k_max;
        }
        return n;
    }
}
