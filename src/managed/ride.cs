// SPDX-License-Identifier: MIT

namespace FFXVR;

// Riding (chocobo; design-notes "Chocobo rides: how the game does it, and first person on the bird").
// Mounting swaps the actors round: the chocobo (f051) takes actor slot 0 and is the actor the player steers, and Tidus (c001)
// comes back in a high slot ATTACHED to the bird: +0x19C points at the chocobo's actor, +0x1A0 is the bone, +0x1A4..+0x1B0 an
// offset and +0x1B4 a scale. The root builder (0x832760) then makes his root matrix +0x1D0 from the chocobo's bone and root and
// never reads his +0x0C, which stays at a stale (0, 36.3, 0) (the Calm Lands logs, 3 Nov 2026). So for an attached actor the
// position is the drawn root +0x200 (world), and its optpos (built from that root, 0x8350F0) are world positions too.
// FFX-60fps (SMOOTH-002-CHILDREL, on by default) moves the rider with the bird's interpolation, so the rider's +0x200 at the
// post-join snapshot is what the frame draws.

/// <summary>The pure rules of the chocobo work, kept free of game types so the scratch tests can run them on the logged tables.</summary>
internal static class RideLogic {
    /// <summary>The table index an attachment pointer (+0x19C) points at, or -1: inside the table, on a slot boundary, below
    /// count, and not the actor itself (the same test FFX-60fps' SMOOTH-002-CHILDREL makes).</summary>
    public static int ParentIndex(long table, long parent, int stride, int count, int self) {
        if (table == 0 || parent == 0 || stride <= 0 || count <= 0) return -1;
        long off = parent - table;
        if (off < 0 || off % stride != 0) return -1;
        long i = off / stride;
        return i < count && i != self ? (int)i : -1;
    }

    /// <summary>One step of a first-order low-pass toward raw, time constant tau seconds over dt seconds. It snaps to raw (and
    /// becomes valid) when not valid yet, when tau is 0 (off), when dt is not sane (0, or over 0.25 s: a stall, a load, a menu),
    /// or when raw is more than snap away (a warp: a mount, a dismount, a map change).</summary>
    public static float Lpf(float s, ref bool valid, float raw, float dt, float tau, float snap) {
        if (!float.IsFinite(raw)) { valid = false; return raw; }
        if (!valid || !float.IsFinite(s) || tau <= 1e-4f || !(dt > 0f) || dt > 0.25f || System.MathF.Abs(raw - s) > snap) { valid = true; return raw; }
        return s + (raw - s) * (1f - System.MathF.Exp(-dt / tau));
    }

    /// <summary>First person's score of one live field actor, lower is better, -1 = no candidate (FP-1, FP-2). rank = its place
    /// in the fp_actor list (-1 = not listed); hidden = the game's script hide (actor+0x180 &amp; 0x13).
    /// - a listed model: its rank; hidden: 500 + rank (after every visible listed model; FP-2);
    /// - a party model (chr_id 1..0xFFF): 1000 + chr_id; hidden: 200000 + chr_id, after the actor-0 last resort (chocobo: while
    ///   Tidus is swapped onto the bird the field has no listed model for a second and the party models c002..c007 sit hidden at
    ///   (0, 0, 0); the eyes went there, now they go to actor 0, which is the bird);
    /// - actor 0: 100000; anything else: -1.</summary>
    public static int FpScore(int rank, bool hidden, int chr_id, int idx) {
        if (rank >= 0) return hidden ? 500 + rank : rank;
        if (chr_id > 0 && chr_id < 0x1000) return hidden ? 200000 + chr_id : 1000 + chr_id;
        return idx == 0 ? 100000 : -1;
    }

    /// <summary>What a score means, for the "first person: following" line.</summary>
    public static string FpHow(int best, string[] names) {
        if (best < 500) return "model " + names[best];
        if (best < 1000) return "model " + names[best - 500] + ", hidden by the game (no visible listed model)";
        if (best < 100000) return "no listed model on this map: the party model with the lowest id";
        if (best < 200000) return "nothing matched: actor 0";
        return "nothing matched and no actor 0: a party model hidden by the game";
    }

    /// <summary>CR-1: the rider's root to use this frame (game space). A new rider actor (Tidus comes back in a fresh slot at the
    /// mount) can be live a Present before the logic job built his root, so +0x200 can be about (0, 0, 0) or the previous
    /// occupant's. built = td is not about zero and, with a known mount, within 60 units of it. Not built: the last good root
    /// (same rider, same parent) if it is near the mount, else the mount's root; with neither, td (the caller then treats the
    /// actor as not attached).</summary>
    public static System.Numerics.Vector3 RiderRoot(System.Numerics.Vector3 td, bool mount_ok, System.Numerics.Vector3 mount,
                                                    bool last_ok, System.Numerics.Vector3 last, out bool built) {
        bool zero = System.MathF.Abs(td.X) + System.MathF.Abs(td.Y) + System.MathF.Abs(td.Z) < 0.01f;
        built = !zero && (!mount_ok || System.Numerics.Vector3.Distance(td, mount) < 60f);
        if (built) return td;
        if (last_ok && (!mount_ok || System.Numerics.Vector3.Distance(last, mount) < 60f)) return last;
        return mount_ok ? mount : td;
    }

    /// <summary>CR-2: bob smoothing only for the steered mount: the parent is actor 0 (the chocobo takes actor 0 and is steered,
    /// the 3 Nov logs). Cutscene carries, lifts and throws attach the followed actor too and must follow the bone unfiltered.</summary>
    public static bool SmoothApplies(int mode, float ms, bool mount_ok, int mount_idx) => mode > 0 && ms > 0f && mount_ok && mount_idx == 0;

    /// <summary>The eye base while riding (game space): the rider's drawn root with its offset from the mount's drawn root
    /// low-passed (mode 1: the height only, mode 2: all three axes), so the bird's running bob is filtered while the eye never
    /// trails the bird (the mount's root glides at the drawn rate). s / valid are the filter's state (x, y, z).</summary>
    public static System.Numerics.Vector3 EyeBase(System.Numerics.Vector3 rider, System.Numerics.Vector3 mount, int mode, float dt, float tau,
                                                  ref System.Numerics.Vector3 s, ref bool vx, ref bool vy, ref bool vz) {
        const float snap = 6f;   // units (CR-2): three times a running bob (about +-2); a bigger jump is a re-seat or a scripted move: snap
        System.Numerics.Vector3 o = rider - mount, e = rider;
        if (mode >= 1) { s.Y = Lpf(s.Y, ref vy, o.Y, dt, tau, snap); e.Y = mount.Y + s.Y; } else vy = false;
        if (mode >= 2) { s.X = Lpf(s.X, ref vx, o.X, dt, tau, snap); s.Z = Lpf(s.Z, ref vz, o.Z, dt, tau, snap); e.X = mount.X + s.X; e.Z = mount.Z + s.Z; }
        else { vx = false; vz = false; }
        return e;
    }
}

public sealed unsafe partial class FFXVRModule {

    private int   _ride_smooth = 1;          // ini ride_smooth: 0 off, 1 the rider's height above the bird (default), 2 height and horizontal offset
    private float _ride_smooth_ms = 150f;    // ini ride_smooth_ms: the low-pass time constant (0..1000)
    private bool  _ride_hide_mount = false;  // ini ride_hide_mount: first person also hides the bird (if its head and neck are in the way)
    private float _ride_hide_radius = 6f;    // ini ride_hide_radius: the body hide's fixed radius around the rider's root while riding (ABI 33)
    private delegate* unmanaged[Cdecl]<float*, int, int> _fn_ride_stats;   // ABI 33: the riding hide's 10 s statistics
    private long _ride_stats_t; private int _ride_stats_lines;
    private delegate* unmanaged[Cdecl]<int, float, float, float, float, float, float, float, uint> _fn_ride_set;   // ABI 32

    // this frame (render thread, push_player)
    private bool    _ride_att;                 // the followed actor is attached (+0x19C != 0)
    private int     _ride_rider_idx = -1, _ride_mount_idx = -1, _ride_bone = -1;
    private nint    _ride_parent;              // +0x19C as read at Present
    private Vector3 _ride_rider, _ride_mount;  // game space: the rider's drawn root (the body-hide centre), the mount's drawn root
    private bool    _ride_mount_ok;
    private string  _ride_mount_name = "", _ride_rider_name = "", _ride_status = "on foot";
    private uint    _ride_counts;              // native: last frame's first-person matches (rider parts low 16, mount parts high 16)
    private bool    _ride_was; private int _ride_log_rider = -1, _ride_log_mount = -1;   // what the last riding line was about
    private int     _ride_check_in = -1, _ride_mount_key = -1;       // frames until the one "riding check:" line after a mount
    private Vector3 _ride_s; private bool _ride_vx, _ride_vy, _ride_vz; private long _ride_t;
    private bool    _ride_built;               // CR-1: this frame's rider root was built (else the last good one or the mount's)
    private Vector3 _ride_last; private bool _ride_last_ok; private int _ride_last_rider = -1; private nint _ride_last_parent;   // the last good rider root
    private int     _ride_unbuilt_n;           // frames with an unbuilt rider root (the riding check line)
    private long    _ride_log_t;               // N1: the last riding line (at most one a second while riding)

    private bool ride_parse(string k, string v) {
        switch (k) {
            case "ride_smooth":     pi(k, v, ref _ride_smooth, 0, 2); return true;
            case "ride_smooth_ms":  pf(k, v, ref _ride_smooth_ms, 0f, 1000f); return true;
            case "ride_hide_mount": pb(k, v, ref _ride_hide_mount); return true;
            case "ride_hide_radius": pf(k, v, ref _ride_hide_radius, 0.5f, 12f); return true;
            case "ride_hide_cap":   return true;   // ea5a22f's cap (3) on the step margin: replaced by ride_hide_radius, not carried over (3 missed Tidus)
            default: return false;
        }
    }
    private string ride_ini_text() {
        var ci = CI;
        return
            $"ride_smooth={_ride_smooth.ToString(ci)}\n" +
            $"ride_smooth_ms={_ride_smooth_ms.ToString("F0", ci)}\n" +
            $"ride_hide_mount={(_ride_hide_mount ? 1 : 0)}\n" +
            $"ride_hide_radius={_ride_hide_radius.ToString("F1", ci)}\n";
    }

    /// <summary>push_player, field only: is the followed actor attached (+0x19C)? If so, its drawn root (+0x200, read now; the
    /// post-join snapshot replaces it in sa_apply when fresh) and, when the parent is a live actor of the table, the mount's.</summary>
    private bool ride_probe(Actor* actors, int pick) {
        _ride_att = false; _ride_mount_ok = false; _ride_mount_idx = -1; _ride_bone = -1; _ride_parent = 0;
        if (_fn_safe_read == null || actors == null || pick < 0) return false;
        byte* a = (byte*)(actors + pick);
        nint par = 0; int bone = -1;
        if (_fn_safe_read(a + 0x19C, &par, 4) == 0 || par == 0) return false;
        Vector3 td;
        if (!read(a + 0x200, out td) || !finite(td)) return false;   // no drawn root to use: keep +0x0C (as before)
        _fn_safe_read(a + 0x1A0, &bone, 4);
        int mi = RideLogic.ParentIndex((long)actors, (long)par, sizeof(Actor), 48, pick);
        bool mok = false; Vector3 mt = default;
        if (mi >= 0 && read(actors + mi, out Actor m) && live(m) && read((byte*)(actors + mi) + 0x200, out mt) && finite(mt)) {
            mok = true;
            int mk = (mi << 16) | m.chr_id;
            if (mk != _ride_mount_key) { _ride_mount_key = mk; _ride_mount_name = actor_name(m.chr_name); }   // R22: the name is read once per mount, not per frame
        }
        bool last_ok = _ride_last_ok && _ride_last_rider == pick && _ride_last_parent == par;
        Vector3 rr = RideLogic.RiderRoot(td, mok, mt, last_ok, _ride_last, out bool built);   // CR-1
        if (!built && !mok && !last_ok) { _ride_last_ok = false; return false; }   // nothing to stand on: +0x0C as before, for this frame
        _ride_att = true; _ride_parent = par; _ride_rider = rr; _ride_rider_idx = pick; _ride_bone = bone; _ride_built = built;
        if (mok) { _ride_mount_idx = mi; _ride_mount = mt; _ride_mount_ok = true; }
        if (built) { _ride_last = td; _ride_last_ok = true; _ride_last_rider = pick; _ride_last_parent = par; } else _ride_unbuilt_n++;
        return true;
    }
    private static bool finite(Vector3 v) => float.IsFinite(v.X) && float.IsFinite(v.Y) && float.IsFinite(v.Z) && MathF.Abs(v.X) < 1e5f && MathF.Abs(v.Y) < 1e5f && MathF.Abs(v.Z) < 1e5f;

    /// <summary>push_player, after sa_apply: pos is the rider's drawn root (the body-hide centre, kept in _ride_rider); it becomes
    /// the eye base with the bob filtered (ride_smooth), and the head moves with it, so the head's offset from pos stays the seat's.</summary>
    private void ride_eye(ref Vector3 pos, ref Vector3 head) {
        _ride_rider = pos;
        long now = System.Diagnostics.Stopwatch.GetTimestamp();
        float dt = _ride_t != 0 ? (float)((now - _ride_t) / (double)System.Diagnostics.Stopwatch.Frequency) : 0f; _ride_t = now;
        if (_ride_built) _ride_last = pos;   // the last good root as used (the snapshot's when fresh)
        if (!RideLogic.SmoothApplies(_ride_smooth, _ride_smooth_ms, _ride_mount_ok, _ride_mount_idx)) { _ride_vx = _ride_vy = _ride_vz = false; return; }   // CR-2
        Vector3 e = RideLogic.EyeBase(pos, _ride_mount, _ride_smooth, dt, _ride_smooth_ms * 0.001f, ref _ride_s, ref _ride_vx, ref _ride_vy, ref _ride_vz);
        Vector3 corr = e - pos;
        pos = e; head += corr;
    }

    /// <summary>push_view_rules, every frame: the rider / mount roots to native (render space negates X and Y), the counts back,
    /// and once per change the status text and the riding line (nothing is formatted on the frames in between, R22).</summary>
    private void ride_push() {
        bool on = _ride_att;
        if (!on) { _ride_vx = _ride_vy = _ride_vz = false; _ride_t = 0; _ride_last_ok = false; _ride_stats_t = 0; }
        int flags = on ? 1 | (_ride_mount_ok ? 2 : 0) | (_ride_hide_mount ? 4 : 0) : 0;
        if (_fn_ride_set != null)
            _ride_counts = _fn_ride_set(flags, -_ride_rider.X, -_ride_rider.Y, _ride_rider.Z, -_ride_mount.X, -_ride_mount.Y, _ride_mount.Z, _ride_hide_radius);
        if (on) {
            int mk = _ride_mount_ok ? _ride_mount_key : -1;
            long now = System.Diagnostics.Stopwatch.GetTimestamp();
            // N1: a new rider, or a new known mount (a mount check flickering to "unknown" is not a change); at most one line a second
            bool change = !_ride_was || _ride_rider_idx != _ride_log_rider || (mk != -1 && mk != _ride_log_mount);
            if (change && (!_ride_was || now - _ride_log_t >= System.Diagnostics.Stopwatch.Frequency)) {
                string map = map_name();
                _ride_was = true; _ride_log_rider = _ride_rider_idx; _ride_log_mount = mk; _ride_check_in = 90; _ride_log_t = now;
                Actor* actors = Globals.actors;
                _ride_rider_name = actors != null && read(actors + _ride_rider_idx, out Actor ra) ? actor_name(ra.chr_name) : "?";
                string mount = _ride_mount_ok ? $"'{_ride_mount_name}' (actor {_ride_mount_idx}, bone {_ride_bone})" : $"something outside the actor table (+0x19C {(uint)_ride_parent:X8})";
                _ride_status = _ride_mount_ok ? $"riding: {_ride_mount_name} (actor {_ride_mount_idx})" : "attached (the parent is outside the actor table)";
                _logger.Info($"riding: '{_ride_rider_name}' (actor {_ride_rider_idx}) is attached to {mount} on map {map}: the eyes use the rider's drawn root (+0x200), not +0x0C" +
                             $" | rider root ({_ride_rider.X:F1}, {_ride_rider.Y:F1}, {_ride_rider.Z:F1}){(_ride_built ? "" : " (not built yet: the last good one or the bird's)")}" + (_ride_mount_ok ? $" bird root ({_ride_mount.X:F1}, {_ride_mount.Y:F1}, {_ride_mount.Z:F1}), {_ride_mount.Y - _ride_rider.Y:F1} units above it" : "") +
                             $" | bob smoothing {(_ride_smooth == 0 ? "off" : _ride_smooth == 1 ? "height" : "height + sideways")} {_ride_smooth_ms:F0} ms, hide the bird {(_ride_hide_mount ? "on" : "off")}, hide radius {_ride_hide_radius:F1}");
            }
            if (_ride_check_in > 0 && --_ride_check_in == 0) ride_check_log();
            ride_stats_log();
        }
        else if (_ride_was) {
            _ride_was = false; _ride_log_rider = -1; _ride_log_mount = -1; _ride_check_in = -1; _ride_status = "on foot";
            _logger.Info($"riding: off ('{_ride_rider_name}' is no longer attached; or first person follows another actor) on map {map_name()}");
        }
    }

    /// <summary>Every 10 s while riding: the native hide's statistics (first person frames only; 100 lines a session).</summary>
    private void ride_stats_log() {
        if (_fn_ride_stats == null) return;
        long now = System.Diagnostics.Stopwatch.GetTimestamp();
        if (_ride_stats_t == 0) { _ride_stats_t = now; float* z = stackalloc float[6]; _fn_ride_stats(z, 6); return; }   // start a window
        if (now - _ride_stats_t < 10 * System.Diagnostics.Stopwatch.Frequency) return;
        _ride_stats_t = now;
        float* v = stackalloc float[6]; for (int i = 0; i < 6; i++) v[i] = 0;
        if (_fn_ride_stats(v, 6) < 6 || v[0] < 1f || _ride_stats_lines >= 100) return;
        _ride_stats_lines++;
        _logger.Info($"riding hide (10 s, first person): frames {v[0]:F0}, the rider matched in {v[1]:F0}, missed in {v[2]:F0} | matched parts up to {v[5]:F2} units from the rider's root (radius {_ride_hide_radius:F1})" +
                     $" | an unmatched skinned draw within 12 units in {v[3]:F0} frames (nearest up to {v[4]:F2}){(_ride_stats_lines == 100 ? " (no more of these lines this session)" : "")}");
    }

    private static float ride_nearest(float* wp, int n, Vector3 g) {   // render-space skinned translations vs a game-space point
        float bd = float.MaxValue;
        for (int k = 0; k < n; k++) { float dx = wp[k*3] + g.X, dy = wp[k*3+1] + g.Y, dz = wp[k*3+2] - g.Z; float d = dx*dx + dy*dy + dz*dz; if (d < bd) bd = d; }
        return n > 0 ? MathF.Sqrt(bd) : -1f;
    }
    /// <summary>A second and a half after a mount, once: where the skinned draws are relative to the two roots (the evidence that
    /// the body hide matches the rider and leaves the bird), and what the hide matched last frame.</summary>
    private void ride_check_log() {
        if (_fn_world_positions == null) return;
        float* wp = stackalloc float[128 * 3]; int nwp = _fn_world_positions(wp, 128);
        _logger.Info($"riding check: map {map_name()} | nearest skinned draw to the rider's root {ride_nearest(wp, nwp, _ride_rider):F2} units" + (_ride_mount_ok ? $", to the bird's root {ride_nearest(wp, nwp, _ride_mount):F2}" : "") +
                     $" (of {nwp}) | frames with an unbuilt rider root so far {_ride_unbuilt_n} | first person matched last frame: rider parts {_ride_counts & 0xFFFF}, bird parts {_ride_counts >> 16} ({(_ride_hide_mount ? "hidden" : "kept")})" +
                     $" | view {(_fp_mode == 1 ? "first person" : _fp_mode == 3 ? "third person" : _fp_mode == 2 ? "diorama" : "seat")} | smooth anchor: {_sa_status}");
    }

    /// <summary>The First person / follow section's riding block (F8, Debug).</summary>
    private void ride_ui() {
        ImGui.Text("chocobo:"); ImGui.SameLine();
        if (_ride_att) ImGui.Text(esc(_ride_status) + $" - rider parts matched last frame {_ride_counts & 0xFFFF}, bird parts {_ride_counts >> 16} ({(_ride_hide_mount ? "hidden" : "shown")}; first person only)");
        else ImGui.TextDisabled("on foot (riding = the followed actor is attached to another actor's bone, +0x19C)");
        ImGui.Text("  bob smoothing while riding:"); ImGui.SameLine();
        ImGui.RadioButton("off##rs", ref _ride_smooth, 0); ImGui.SameLine();
        ImGui.RadioButton("height##rs", ref _ride_smooth, 1); ImGui.SameLine();
        ImGui.RadioButton("height + sideways##rs", ref _ride_smooth, 2); ImGui.SameLine();
        ImGui.SetNextItemWidth(140); ImGui.SliderFloat("ms##rsms", ref _ride_smooth_ms, 0f, 1000f, "%.0f");
        ImGui.Checkbox("first person: also hide the chocobo (if its head and neck are in the way)", ref _ride_hide_mount); ImGui.SameLine();
        ImGui.SetNextItemWidth(120); ImGui.SliderFloat("hide radius while riding", ref _ride_hide_radius, 0.5f, 12f, "%.1f");
    }
}
