// SPDX-License-Identifier: MIT

namespace FFXVR;

// Sphere hands, stages 1 + 2 (research/sphere-hands-design.md sections 9 / 10; the logic native, ffxvr_sphere.h, unit-tested;
// the glue in ffxvr_native.cpp "Sphere hands"). In a Cloister of Trials, in first person with the VR hands: a label near the
// sphere or socket a hand comes near, and a grip there takes or places the sphere with no prompt (the game's own interaction:
// one A, then its lines answered one at a time while the prompt is hidden in the headset), then a note at the hand.
// This file reads the game for native every frame (the reads of sphere_log.cs, shared): the talk-enabled workers near Tidus (the
// game's own candidates, for the mirrored choice), the trial's sockets with their sphere actors and variables, the controller's
// state, and the first three render-side message windows with their page text.
// Only the trials with a socket table (SphSockets) take part: Djose for now (logs9). The automation matches the prompts' exact
// English text: in another language the grip still presses A once (the game's own interaction) and the prompt then shows as
// usual (logged "unexpected text").
// ini: sphere_hands 0/1, default 1 (it acts only with vr_hands on, in first person, in a trial with a socket table).
public sealed unsafe partial class FFXVRModule {

    private bool _sphere_hands = true;   // ini sphere_hands
    private delegate* unmanaged[Cdecl]<SphHead*, SphZone*, int, SphTarget*, int, SphWin*, int, void> _fn_sph_set;   // ABI 64
    private delegate* unmanaged[Cdecl]<float*, int, byte*> _fn_sph_status;                                      // ABI 64

    // = native sph::Head / Zone / Target / Win (x86; 76, 52, 60, 116 bytes)
    [StructLayout(LayoutKind.Sequential)] private struct SphHead { public int bytes, on, control, free_play, carried, talk_target; public uint gen; public float tx, ty, tz, player_radius; public fixed byte carried_name[32]; }
    [StructLayout(LayoutKind.Sequential)] private struct SphZone { public int widx, kind; public float cx, cy, cz, hx, hz, rot, h, range, htol, atol; public int target; }
    [StructLayout(LayoutKind.Sequential)] private struct SphTarget { public int widx, kind, value, free_slots; public float ax, ay, az; public fixed byte name[32]; }
    [StructLayout(LayoutKind.Sequential)] private struct SphWin { public int open, state, choice_mode, choice_row, n; public fixed byte page[96]; }

    private const int k_sphh_zones = 24, k_sphh_targets = 16;
    private SphZone* _sphh_z; private SphTarget* _sphh_t; private SphWin* _sphh_w;   // native-shaped buffers (kept for the session)
    private int _sphh_nz, _sphh_nt; private double _sphh_scan_t = -1;
    private readonly int[] _sphh_sock = new int[k_sphh_targets], _sphh_slot = new int[k_sphh_targets];   // per target: its socket table row, its sphere actor (sockets) or the pedestal actor
    private readonly float[] _sphh_zc = new float[k_sphh_targets * 3];   // per target: its zone centre (the anchor's fallback)
    private uint _sphh_res;              // the sphere model resource (+0x1C4) of the sphere attached to Tidus (the carried one, shown or hidden)
    private string _sphh_gate = "";      // why the feature is off now (F8)
    private bool _sphh_sent_on;

    private bool sphh_parse(string k, string v) { if (k != "sphere_hands") return false; pb(k, v, ref _sphere_hands); return true; }
    private void sphh_bind(Func<string, nint> req) {
        _fn_sph_set = (delegate* unmanaged[Cdecl]<SphHead*, SphZone*, int, SphTarget*, int, SphWin*, int, void>)req("ffxvr_sph_set");
        _fn_sph_status = (delegate* unmanaged[Cdecl]<float*, int, byte*>)req("ffxvr_sph_status");
    }
    /// The feature wants the game's state this frame (sph_tick then reads it, with or without the research log).
    private bool sphh_wanted() =>
        _sphere_hands && _vr_hands && _fp_mode == 1 && _fn_sph_set != null && _sph_trial >= 0 && SphSockets.For(SphTrial.T[_sph_trial].script) != null;

    /// Off: a head with on = 0 (native also treats a state older than 3 frames as off).
    private void sphh_push_off() {
        if (_fn_sph_set == null || !_sphh_sent_on) return;
        var h = new SphHead { bytes = sizeof(SphHead), carried = -1, talk_target = -1 };
        _fn_sph_set(&h, null, 0, null, 0, null, 0);
        _sphh_sent_on = false;
        _sphh_gate = "not in a trial with a socket table, or off";
    }

    /// From sph_tick (render thread), after its reads: this frame's state to native.
    private void sphh_push(uint ctl, bool pl_ok, uint pl, in GameGates.Text tx, int carried) {
        if (_sphh_z == null) {
            _sphh_z = (SphZone*)Marshal.AllocHGlobal(sizeof(SphZone) * k_sphh_zones);
            _sphh_t = (SphTarget*)Marshal.AllocHGlobal(sizeof(SphTarget) * k_sphh_targets);
            _sphh_w = (SphWin*)Marshal.AllocHGlobal(sizeof(SphWin) * 3);
        }
        SphSockets? sk = SphSockets.For(SphTrial.T[_sph_trial].script);
        Actor* actors = Globals.actors; Actor* snap = tbl_snap(actors);
        int ti = sph_player_slot(actors, pl);
        // the gates (any closed: on = 0; the labels go, a running sequence stops)
        string gate = "";
        int vb = hands2_view_bits();
        if (ctl == 0) gate = "no Atel controller";
        else if (sk == null) gate = "no socket table for this trial";
        else if (_sph_chunk == 0) gate = "the trial's script not found";
        else if (carried < 0) gate = "the trial's variables not read";
        else if (snap == null || ti < 0) gate = "Tidus not found";
        else if ((vb & 0x100) != 0) gate = "a game read failed";
        else if ((vb & 1) != 0) gate = "battle";
        else if ((vb & 0x80) != 0) gate = "a field menu";
        else if ((vb & 0x2000) != 0) gate = "a movie";
        else if (_save_st != 0 || _fh_menu_open) gate = "the save screen";
        else if (_in_blitz) gate = "blitzball";
        else if (!_xr_on) gate = "the headset is off";
        else if (_vrp_shown) gate = "the F8 panel is shown in the headset (its grips are the panel's)";
        _sphh_gate = gate;
        bool on = gate.Length == 0;
        if (on && (_sphh_scan_t < 0 || _now - _sphh_scan_t >= 0.25 || _now < _sphh_scan_t)) sphh_scan(actors, snap, ti, sk!);
        var h = new SphHead { bytes = sizeof(SphHead), on = on ? 1 : 0, carried = carried, talk_target = -1 };
        if (ctl != 0) {
            byte* c = _sph_ctl;
            h.control = pl_ok && pl != 0 ? 1 : 0;
            h.free_play = (c[0] & 2) != 0 && c[3] == 0 ? 1 : 0;   // logs9: 0x8B with control, 0x89 / 0x81 (and byte 3 0x10 / 0x30) in an event
            h.talk_target = *(short*)(c + 0x1E8); h.player_radius = *(float*)(c + 0x1F0);
            h.tx = *(float*)(c + 0x218); h.ty = *(float*)(c + 0x21C); h.tz = *(float*)(c + 0x220);   // the player's position as the game tests it
        }
        h.gen = (uint)fp_scene_sig() ^ (uint)(nint)_sph_chunk;
        sphh_name(carried, h.carried_name);
        int nt = 0, nz = 0;
        if (on) {
            nz = _sphh_nz;
            for (int i = 0; i < _sphh_nt; i++) { sphh_target_now(i, actors, snap, sk!); nt++; }
            sphh_windows(tx);
        }
        _fn_sph_set(&h, nz > 0 ? _sphh_z : null, nz, nt > 0 ? _sphh_t : null, nt, on ? _sphh_w : null, on ? 3 : 0);
        _sphh_sent_on = true;
    }

    /// Every 0.25 s: the talk-enabled workers within 60 units (the game's candidates, in its order), the trial's sockets among them
    /// (with their sphere actors), and the sphere model.
    private void sphh_scan(Actor* actors, Actor* snap, int ti, SphSockets sk) {
        _sphh_scan_t = _now;
        // the sphere model: the object attached to Tidus (+0x19C = his actor; logs9: f113 on bone 50, shown while carried)
        _sphh_res = 0;
        uint tptr = (uint)(nint)(actors + ti);
        for (int i = 0; i < 48; i++) {
            if (i == ti || !live(snap[i]) || ((snap[i].chr_id >> 12) & 0xF) != 5) continue;
            byte* r = (byte*)(snap + i);
            if (*(uint*)(r + 0x19C) == tptr) { _sphh_res = *(uint*)(r + 0x1C4); break; }
        }
        Vector3 pp = new Vector3(*(float*)(_sph_ctl + 0x218), *(float*)(_sph_ctl + 0x21C), *(float*)(_sph_ctl + 0x220));
        int count = Math.Min((int)*(ushort*)(_sph_ctl + 0x0C), 1024);
        int nz = 0, nt = 0;
        for (int i = 0; i < count; i++) {
            if (!sph_worker_read(i, actors, out SphW w) || !w.block || (w.ab & 1) == 0 || w.type < 1 || w.type > 3) continue;
            int row = w.chunk == _sph_chunk ? sk.Row(w.local) : -1;
            bool near = Vector3.Distance(w.pos, pp) < 60f;
            int tix = -1;
            if (row >= 0 && nt < k_sphh_targets) {
                tix = nt++;
                SphTarget* t = _sphh_t + tix;
                t->widx = w.idx2e; t->kind = sk.kind[row]; t->value = -1; t->free_slots = 0;
                _sphh_sock[tix] = row;
                _sphh_zc[tix * 3] = w.pos.X; _sphh_zc[tix * 3 + 1] = w.pos.Y; _sphh_zc[tix * 3 + 2] = w.pos.Z;
                _sphh_slot[tix] = sk.kind[row] == 1 ? w.actor_slot : sphh_socket_sphere(snap, w.pos);
            }
            if (near && nz < k_sphh_zones) {
                SphZone* z = _sphh_z + nz++;
                z->widx = w.idx2e; z->kind = w.type; z->cx = w.pos.X; z->cy = w.pos.Y; z->cz = w.pos.Z;
                z->hx = w.p2.X; z->hz = w.p2.Z; z->rot = w.type == 3 ? w.p2.Y : 0f;   // type 3: half-extents +0x30 / +0x38, rotation +0x34; type 2: the other end
                z->h = w.contact_h; z->range = w.range; z->htol = w.htol; z->atol = w.atol; z->target = tix;
            }
        }
        _sphh_nz = nz; _sphh_nt = nt;
    }
    /// A socket zone's sphere actor: the sphere model's actor (not attached) nearest its centre, horizontally, within 15 units.
    private int sphh_socket_sphere(Actor* snap, Vector3 c) {
        if (_sphh_res == 0) return -1;
        int best = -1; float bd = 15f;
        for (int i = 0; i < 48; i++) {
            if (!live(snap[i])) continue;
            byte* r = (byte*)(snap + i);
            if (*(uint*)(r + 0x1C4) != _sphh_res || *(uint*)(r + 0x19C) != 0) continue;
            float dx = snap[i].chr_pos_vec.X - c.X, dz = snap[i].chr_pos_vec.Z - c.Z, d = MathF.Sqrt(dx * dx + dz * dz);
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }
    /// Target i this frame: its content (the trial's variables) and where its sphere is.
    private void sphh_target_now(int i, Actor* actors, Actor* snap, SphSockets sk) {
        SphTarget* t = _sphh_t + i; int row = _sphh_sock[i], slot = _sphh_slot[i];
        int v0 = sphh_var(sk.var0[row]), v1 = sk.var1[row] >= 0 ? sphh_var(sk.var1[row]) : -2;
        if (v0 < 0 || v1 == -1) { t->value = -1; t->free_slots = 0; }
        else { t->value = Math.Max(v0, Math.Max(v1, 0)); t->free_slots = (v0 == 0 ? 1 : 0) + (v1 == 0 ? 1 : 0); }
        sphh_name(t->value, t->name);
        Vector3 a = new Vector3(_sphh_zc[i * 3], _sphh_zc[i * 3 + 1] - 10f, _sphh_zc[i * 3 + 2]);   // fallback: 10 units above the zone's centre
        if (t->kind == 0 && slot >= 0 && live(snap[slot])) a = new Vector3(snap[slot].chr_pos_vec.X, snap[slot].chr_pos_vec.Y, snap[slot].chr_pos_vec.Z);
        else if (t->kind == 1 && slot >= 0 && live(snap[slot])) {   // a pedestal: its shown spheres' drawn roots, else 10 units above it
            uint pptr = (uint)(nint)(actors + slot); Vector3 sum = default; int n = 0;
            for (int k = 0; k < 48; k++) {
                if (!live(snap[k])) continue;
                byte* r = (byte*)(snap + k);
                if (*(uint*)(r + 0x19C) != pptr || (_sphh_res != 0 && *(uint*)(r + 0x1C4) != _sphh_res)) continue;   // shown: a drawn root (a hidden one's is 0, logs9)
                Vector3 d = *(Vector3*)(r + 0x200);
                if (finite(d) && d.LengthSquared() > 1f) { sum += d; n++; }
            }
            byte* pr = (byte*)(snap + slot); Vector3 pd = *(Vector3*)(pr + 0x200);
            Vector3 pc = finite(pd) && pd.LengthSquared() > 1f ? pd : new Vector3(snap[slot].chr_pos_vec.X, snap[slot].chr_pos_vec.Y, snap[slot].chr_pos_vec.Z);
            a = n > 0 ? sum / n : pc + new Vector3(0f, -10f, 0f);
        }
        t->ax = a.X; t->ay = a.Y; t->az = a.Z;
    }
    private int sphh_var(int off) {
        ref readonly SphTrial t = ref SphTrial.T[_sph_trial];
        int i = (off - t.lo) / t.size;
        return (off - t.lo) % t.size == 0 ? sph_var(i) : -1;
    }
    /// A value's name from the trial's table into a native char[32] ("" when not named).
    private void sphh_name(int v, byte* dst) {
        for (int k = 0; k < 32; k++) dst[k] = 0;
        if (_sph_trial < 0 || v <= 0) return;
        var t = SphTrial.T[_sph_trial];
        for (int i = 0; i < t.vals.Length; i++) {
            if (t.vals[i] != v) continue;
            string s = t.names[i]; int n = Math.Min(s.Length, 31);
            for (int k = 0; k < n; k++) dst[k] = s[k] < 128 ? (byte)s[k] : (byte)'?';
            return;
        }
    }
    /// The render-side windows 0..2 this frame (the block sph_tick read): open, state, the cursor, the page text (96 bytes).
    private void sphh_windows(in GameGates.Text tx) {
        for (int k = 0; k < 3; k++) { SphWin* w = _sphh_w + k; w->open = 0; w->state = 0; w->choice_mode = 0; w->choice_row = 0; w->n = 0; }
        if (_sph_blk == null) return;
        uint blk_va = (uint)(nint)FhUtil.ptr_at<byte>(GameGates.k_blk);
        fixed (byte* b = _sph_blk) {
            for (int k = 0; k < 3; k++) {
                SphWin* w = _sphh_w + k;
                if ((tx.open & (1 << k)) == 0) continue;
                uint p = *(uint*)(b + GameGates.k_win_tab + k * 4); long o = (long)p - blk_va;
                if (o < 0 || o + GameGates.k_win_len > GameGates.k_win_tab) continue;
                w->open = 1; w->state = *(ushort*)(b + o + 0x2C);
                uint cp = *(uint*)(b + GameGates.k_cur_tab + k * 4); long co = (long)cp - blk_va;
                if (co >= GameGates.k_cur_lo && co + GameGates.k_cur_len <= GameGates.k_cur_tab) { w->choice_mode = b[co + 1]; w->choice_row = (sbyte)b[co + 2]; }
                uint tp = *(uint*)(b + o + 0x94);
                if (tp != 0 && _fn_safe_read((void*)(nint)tp, w->page, 96) != 0) w->n = 96;
            }
        }
    }

    /// F8 > Hands: the setting and the state.
    private void sphh_ui() {
        ui_check("sphere_hands", "Grab spheres with your hands (Cloisters of Trials)##sphh", ref _sphere_hands, UiTag.None,
                 "In a Cloister of Trials, in first person with the VR hands: a label names the sphere or socket a hand comes near; a squeeze of that hand's grip there takes or places the sphere with no prompt, then a note says what happened. The prompts are answered automatically only in English; in another language the grip presses A once and the prompt shows as usual. Djose only for now.");
        if (_sphere_hands && !_vr_hands) { ImGui.SameLine(); ImGui.TextDisabled("(needs the VR hands on)"); }
    }
    /// F8 > Developer > Inspect: the native state (the target, the prediction, the sequence).
    private void sphh_dev_ui() {
        if (_fn_sph_status == null) return;
        float* v = stackalloc float[16];
        for (int i = 0; i < 16; i++) v[i] = 0f;
        byte* s = _fn_sph_status(v, 16);
        string line = s != null ? Marshal.PtrToStringAnsi((nint)s) ?? "" : "";
        ImGui.TextWrapped(esc("sphere hands: " + line));
        ImGui.TextDisabled(esc(I($"gate: {(_sphh_gate.Length > 0 ? _sphh_gate : "open")} | zones {_sphh_nz}, targets {_sphh_nt}, sphere model 0x{_sphh_res:X8} | label target worker {v[2]:F0} hand {v[3]:F0} action {v[4]:F0} at {v[5]:F2} m (opacity {v[6]:F2}) | reach L {v[14]:F0} R {v[15]:F0} | sequence phase {v[7]:F0} why {v[8]:F0} presses {v[9]:F0}")));
    }
    /// The compact sph: line's part (the grab state), and its share of the line's signature.
    private string sphh_brief(out long sig) {
        sig = 0;
        if (_fn_sph_status == null) return "";
        float* v = stackalloc float[16];
        for (int i = 0; i < 16; i++) v[i] = 0f;
        _fn_sph_status(v, 16);
        sig = (long)(v[0] + v[1] * 3 + v[2] * 7 + v[4] * 11 + v[7] * 13 + v[10] * 17 + v[14] * 19 + v[15] * 23);
        return I($" | grab: on {v[0]:F0} predicted {v[1]:F0} label w{v[2]:F0} act {v[4]:F0} {v[5]:F2} m reach L{v[14]:F0} R{v[15]:F0} seq {v[7]:F0} (grabs {v[10]:F0}, done {v[11]:F0}, stopped {v[12]:F0}, refused {v[13]:F0}){(_sphh_gate.Length > 0 ? " gate: " + _sphh_gate : "")}");
    }
}

/// The trials' sockets for sphere hands: per worker (its index in the trial's script), its kind (0 a socket zone, 1 a pedestal)
/// and its variables (event data offsets; a pedestal's second slot, or -1). From the screen reader's cloister bindings (facts),
/// confirmed for Djose by the capture of 10 Nov (logs9): every transfer moved exactly these.
internal sealed class SphSockets {
    public readonly int[] local, kind, var0, var1;
    SphSockets(int[] local, int[] kind, int[] var0, int[] var1) { this.local = local; this.kind = kind; this.var0 = var0; this.var1 = var1; }
    public int Row(int l) { for (int i = 0; i < local.Length; i++) if (local[i] == l) return i; return -1; }
    // Djose: the eight recesses 48..55, the pedestal (1, two slots), the upper pedestal (6), and the Destruction Sphere's place
    // (62, +0x9F: the screen reader's binding; not yet seen in a capture: its prompts unchecked, so a grab there may stop at
    // "unexpected text" and show the prompt, which is safe).
    static readonly SphSockets s_djose = new(
        new[] { 48, 49, 50, 51, 52, 53, 54, 55, 1, 6, 62 },
        new[] { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0 },
        new[] { 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0xA0, 0x9F },
        new[] { -1, -1, -1, -1, -1, -1, -1, -1, 0x9E, -1, -1 });
    public static SphSockets? For(string script) => script == "djyt0600" ? s_djose : null;
}
