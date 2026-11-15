// SPDX-License-Identifier: MIT

namespace FFXVR;

// Carry: a game model drawn at a VR hand (native ffxvr_carry.h, unit-tested; the glue "Carry" in ffxvr_native.cpp; the design:
// research/weapon-hands-design.md section 6). Native moves the model's own draws: at a draw of one of the model's segments
// (its index buffer, byte offset and index count) it changes the world matrix, the products built from it and the bone
// palette in the constant-buffer images of both eyes, so the game draws the model at the hand with its own shaders and
// textures. Nothing is written to the game.
//
// The first user (this file): the Cloister of Trials sphere Tidus carries. In Djose (logs9, 10 Nov 2026; logs11, 13 Nov) it is
// its own object actor ('f113'), attached to his actor (+0x19C) at bone 50 and shown only while one is carried; every Djose
// sphere is the same mesh, so the carried one is told apart by its drawn root (+0x200): its upload's world translation is
// exactly that root (the captures: 0.00 off), and the other spheres' roots are sent too (a draw nearer one of those is theirs).
// Each frame, in a trial with a socket table, in first person with the VR hands and sphere hands on, this file sends native
// the carried actor's segment keys, its drawn root, the other shown actors of the same model and the palm offset. Native's
// rule (carry::SphereGate) then decides: in the palm of the hand whose grip took it (the right hand for the A button) while
// the player has control in free play with no text window and no grab sequence running; the game's own draw during the take
// and place animations, any event, a menu, or whenever the hands are not drawn. With sphere_carry_seq (sphere_fast.cs; native
// carry::SeqHold) a grab sequence the mod started from a grip squeeze keeps it in the palm through those animations too.
// ini: sphere_carry 0/1 (default 1; Hands tab), sphere_carry_offset 0..0.30 m (default 0.10; Developer): how far the sphere's
// centre sits from the palm's centre, out of the palm side.
public sealed unsafe partial class FFXVRModule {

    private bool  _sphere_carry = true;        // ini sphere_carry
    private float _sphere_carry_off = 0.10f;   // ini sphere_carry_offset (metres)
    private bool  _carry_hide_test;            // session only (Developer): the matched draws are hidden instead of moved
    private delegate* unmanaged[Cdecl]<CarryDesc*, uint*, float*, int> _fn_carry_set;   // ABI 66
    private delegate* unmanaged[Cdecl]<int, float*, int, byte*> _fn_carry_status;       // ABI 66

    // = native carry::Desc (x86; 88 bytes: ffxvr_carry.h's static_assert; native checks bytes at every push)
    [StructLayout(LayoutKind.Sequential)] private struct CarryDesc { public int bytes, slot; public uint flags; public int hand; public fixed float m[12]; public fixed float root[3]; public float tol; public int nkeys, navoid; }
    private const int k_carry_slot = 2, k_carry_keys = 16, k_carry_avoid = 24;
    private const uint k_cf_on = 1, k_cf_hand = 2, k_cf_hide = 4, k_cf_near = 8, k_cf_sphere = 16;   // = carry::F_*
    private const float k_carry_tol = 4f;      // units (0.35 m): how far the upload's world translation may be from the root read at Present (a frame older)
    private const float k_carry_fwd = 0.02f;   // metres toward the fingers (the palm frame's -Z)

    private uint* _carry_keys, _carry_q; private byte* _carry_seg; private float* _carry_avoid;   // kept for the session
    private int _carry_nk, _carry_na, _carry_actor = -1, _carry_moved, _carry_lines; private nint _carry_model; private double _carry_keys_t = -1;
    private bool _carry_sent; private string _carry_note = "";

    private bool carry_parse(string k, string v) {
        switch (k) {
            case "sphere_carry":        pb(k, v, ref _sphere_carry); return true;
            case "sphere_carry_offset": pf(k, v, ref _sphere_carry_off, 0f, 0.30f); return true;
            default: return false;
        }
    }
    private string carry_ini_text() => $"sphere_carry={(_sphere_carry ? 1 : 0)}\nsphere_carry_offset={_sphere_carry_off.ToString("F2", CI)}\n";
    private void carry_bind(Func<string, nint> req) {
        _fn_carry_set = (delegate* unmanaged[Cdecl]<CarryDesc*, uint*, float*, int>)req("ffxvr_carry_set");
        _fn_carry_status = (delegate* unmanaged[Cdecl]<int, float*, int, byte*>)req("ffxvr_carry_status");
    }
    private void carry_unbind() { _fn_carry_set = null; _fn_carry_status = null; }

    /// Off: one push with no flags (native also drops a slot not pushed for a frame, and its sphere rule starts over).
    private void carry_push_off() {
        if (_fn_carry_set == null || !_carry_sent) return;
        var d = new CarryDesc { bytes = sizeof(CarryDesc), slot = k_carry_slot, hand = -1 };
        _fn_carry_set(&d, null, null);
        _carry_sent = false; _carry_actor = -1; _carry_nk = 0; _carry_na = 0;
    }

    /// From push_all, every frame, after sph_tick (its push is the state native's rule reads). Cheap: the frame's actor-table
    /// snapshot; the model's segment walk once per carried actor and every 2 s.
    private void carry_tick() {
        if (_fn_carry_set == null) return;
        if (!_sphere_carry || !sphh_wanted()) { _carry_note = !_sphere_carry ? "off (the setting)" : "off (needs sphere hands on, the VR hands, first person, a trial with a socket table)"; carry_push_off(); return; }
        if (_carry_keys == null) {
            _carry_keys = (uint*)Marshal.AllocHGlobal(k_carry_keys * 12); _carry_q = (uint*)Marshal.AllocHGlobal(k_carry_keys * 16);
            _carry_seg = (byte*)Marshal.AllocHGlobal(32 * 0x6C); _carry_avoid = (float*)Marshal.AllocHGlobal(k_carry_avoid * 12);
        }
        var d = new CarryDesc { bytes = sizeof(CarryDesc), slot = k_carry_slot, flags = k_cf_hand | k_cf_near | k_cf_sphere | (_carry_hide_test ? k_cf_hide : 0u), hand = -1, tol = k_carry_tol };
        d.m[0] = 1f; d.m[5] = 1f; d.m[10] = 1f;                  // the palm's own axes
        d.m[7] = -_sphere_carry_off; d.m[11] = -k_carry_fwd;     // out of the palm side (the palm frame's -Y), a little toward the fingers (-Z)
        Actor* actors = Globals.actors; Actor* snap = tbl_snap(actors);
        int who = -1; _carry_na = 0;
        gs_player(out uint pl);
        int ti = snap != null ? sph_player_slot(actors, pl) : -1;
        if (snap != null && ti >= 0 && ti < 48) who = CarryPick.Carried(snap, (uint)(nint)(actors + ti), ti);
        if (who < 0) { _carry_note = snap == null ? "the actor table did not read" : ti < 0 ? "Tidus not found" : "no shown object rides Tidus (nothing carried)"; _carry_actor = -1; _carry_nk = 0; }
        else {
            byte* r = (byte*)(snap + who);
            Vector3 root = *(Vector3*)(r + 0x200);
            d.root[0] = -root.X; d.root[1] = -root.Y; d.root[2] = root.Z;   // render space negates X and Y
            _carry_na = CarryPick.Others(snap, who, new Span<float>(_carry_avoid, k_carry_avoid * 3));
            nint model = (nint)(*(uint*)(r + 0x830));
            if (who != _carry_actor || model != _carry_model || _carry_nk == 0 || _now - _carry_keys_t >= 2.0 || _now < _carry_keys_t) carry_keys(who, model, snap);
            if (_carry_nk > 0) { d.flags |= k_cf_on; _carry_note = I($"actor #{who} '{actor_name(snap[who].chr_name)}', {_carry_nk} segments, {_carry_na} other actors of the model"); }
            else _carry_note = I($"actor #{who}: its model's segments did not read");
        }
        d.nkeys = _carry_nk; d.navoid = _carry_na;
        _carry_moved = _fn_carry_set(&d, _carry_nk > 0 ? _carry_keys : null, _carry_na > 0 ? _carry_avoid : null);
        _carry_sent = true;
    }

    /// The carried actor's segments (IbChain: buffer, byte offset, index count each). A chain that did not read whole gives no
    /// keys (a part must not be left behind): tried again at the next call. A new actor or model is logged (12 a session), with
    /// the model's bounds as the first mesh instance holds them (for the palm offset; that layout is not verified).
    private void carry_keys(int who, nint model, Actor* snap) {
        bool fresh = who != _carry_actor || model != _carry_model;
        _carry_actor = who; _carry_model = model; _carry_keys_t = _now; _carry_nk = 0;
        if (model == 0 || _fn_safe_read == null) return;
        int n = 0; int segs = IbChain.Walk(model, 0u, _carry_q, ref n, k_carry_keys, _carry_seg, 32, _fn_safe_read, out bool fail);
        if (!fail) { for (int e = 0; e < n; e++) { _carry_keys[e * 3] = _carry_q[e * 4]; _carry_keys[e * 3 + 1] = _carry_q[e * 4 + 1]; _carry_keys[e * 3 + 2] = _carry_q[e * 4 + 2]; } _carry_nk = n; }
        if (!fresh || _carry_lines >= 12) return;
        _carry_lines++;
        var sb = new System.Text.StringBuilder(I($"carry: the carried model: actor #{who} '{actor_name(snap[who].chr_name)}' model 0x{(ulong)model:X}, {segs} segments{(fail ? " (a read failed: no keys)" : "")}:"));
        for (int e = 0; e < n; e++) sb.Append(I($" {_carry_q[e * 4]:X8}+{_carry_q[e * 4 + 1]}/{_carry_q[e * 4 + 2]}"));
        byte* bb = stackalloc byte[0x24];
        if (read((byte*)model + 4, out nint arr) && arr != 0 && read((void*)arr, out nint inst) && inst != 0 && read((byte*)inst + 0x1C, out nint bnd) && bnd != 0 && _fn_safe_read((void*)bnd, bb, 0x24) != 0) {
            float* f = (float*)bb;
            sb.Append(I($" | bounds (instance +0x1C, unverified): min ({f[0]:F2},{f[1]:F2},{f[2]:F2}) size ({f[4]:F2},{f[5]:F2},{f[6]:F2})"));
        }
        byte* r = (byte*)(snap + who);
        sb.Append(I($" | actor scale +0x5C {*(float*)(r + 0x5C):F3}, bone +0x1A0 {*(int*)(r + 0x1A0)} | palm offset {_sphere_carry_off:F2} m{(_carry_lines == 12 ? " (no more of these lines this session)" : "")}"));
        d7_log(sb.ToString());
    }

    /// F8 > Hands: the setting.
    private void carry_ui() {
        ui_check("sphere_carry", "Carried sphere in your hand##carry", ref _sphere_carry, UiTag.None,
                 "In a Cloister of Trials, in first person with the VR hands: the sphere Tidus carries is drawn in the palm of the hand whose grip took it (the right hand if you took it with the A button). During the game's own take and place animations it is where the game draws it (a grab started with your grip keeps it in your palm: \"Sphere straight to and from your hand\", below), and so it is whenever the hands are hidden. Djose only for now.");
        if (_sphere_carry && !_sphere_hands) { ImGui.SameLine(); ImGui.TextDisabled("(needs \"Grab spheres with your hands\" on)"); }
    }
    /// F8 > Developer > Inspect: the palm offset, the test mode and the native state.
    private void carry_dev_ui() {
        ui_slider("sphere_carry_offset", "Carried sphere: distance from the palm##carry", ref _sphere_carry_off, 0f, 0.30f, "%.2f m", UiTag.None,
                  "How far the carried sphere's centre sits from the centre of the palm, out of the palm side. Raise it if the sphere is inside the hand.", 200f);
        ImGui.Checkbox("Carried sphere: hide it instead of moving it (identification test, this session)##carry", ref _carry_hide_test);
        if (_fn_carry_status == null) return;
        float* v = stackalloc float[16];
        for (int i = 0; i < 16; i++) v[i] = 0f;
        byte* s = _fn_carry_status(k_carry_slot, v, 16);
        string line = s != null ? Marshal.PtrToStringAnsi((nint)s) ?? "" : "";
        ImGui.TextWrapped(esc("carry: " + line));
        ImGui.TextDisabled(esc(I($"managed: {_carry_note} | carried value {v[9]:F0}, quiet frames {v[10]:F0} | last frame: nearest buffer {v[8]:F2} units from the root (tolerance {k_carry_tol:F1}), moved by translation only {v[7]:F0}")));
    }
}

/// The carried object among the actors of the frame's table snapshot (plain reads of its bytes).
internal static unsafe class CarryPick {
    private static bool Live(Actor* snap, int i) => snap[i].chr_enabled == 1 && (nint)snap[i].chr_name > 0x10000 && (nint)snap[i].chr_name < 0x7FFF0000;
    private static bool Root(Actor* snap, int i, out Vector3 v) {
        v = *(Vector3*)((byte*)(snap + i) + 0x200);
        return float.IsFinite(v.X) && float.IsFinite(v.Y) && float.IsFinite(v.Z) && MathF.Abs(v.X) < 1e5f && MathF.Abs(v.Y) < 1e5f && MathF.Abs(v.Z) < 1e5f && v.LengthSquared() > 1f;   // (a hidden or unbuilt actor's root is 0)
    }
    /// The object actor (chr_id type 5) attached to the player's actor (+0x19C), shown (+0x180 &amp; 0x13 clear) with a built
    /// drawn root: the first such slot, -1 none.
    public static int Carried(Actor* snap, uint player_ptr, int player_slot) {
        for (int i = 0; i < 48; i++) {
            if (i == player_slot || !Live(snap, i) || ((snap[i].chr_id >> 12) & 0xF) != 5) continue;
            byte* r = (byte*)(snap + i);
            if (*(uint*)(r + 0x19C) != player_ptr || (r[0x180] & 0x13) != 0 || !Root(snap, i, out _)) continue;
            return i;
        }
        return -1;
    }
    /// The other shown actors of the carried one's model (the same resource, +0x1C4): their drawn roots in render space, 3
    /// floats each. Returns the count (at most o.Length / 3).
    public static int Others(Actor* snap, int who, Span<float> o) {
        uint res = *(uint*)((byte*)(snap + who) + 0x1C4); int n = 0;
        for (int i = 0; i < 48 && (n + 1) * 3 <= o.Length; i++) {
            if (i == who || !Live(snap, i)) continue;
            byte* r = (byte*)(snap + i);
            if (*(uint*)(r + 0x1C4) != res || (r[0x180] & 0x13) != 0 || !Root(snap, i, out Vector3 v)) continue;
            o[n * 3] = -v.X; o[n * 3 + 1] = -v.Y; o[n * 3 + 2] = v.Z; n++;
        }
        return n;
    }
}
