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
    private delegate* unmanaged[Cdecl]<uint*, int, uint, int> _fn_fp_ib_set;                          // ABI 40: the index-buffer table
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
        bool ib_used = false;
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
            ib_used = true;
        }
        _fph_others_n = n;
        _fph_kept = _fn_fp_others_set(n > 0 ? buf : null, n > 0 ? heads : null, n);
        if (ib_used) body_hide_ib(actors, tbl_snap(actors), pick, root_kind != 0 ? root : centre);   // after the roots: native's order of use
    }

    // -- npc-anchor round 3: the eye on a body drawn away from its root --------------------------------------------------
    // On mi/mihn0100 the event Tidus's root is the scene anchor (shared with Yuna) and the animation places his body up to ~70 units
    // from it, and back near it (the owner's retest: feet 1.4 units from it, head 8.4). While another actor's root is on his,
    // the eye follows the drawn head whatever the distance (EyeLogic); normal play has no shared root. The module log gets each
    // change (40 a session).
    // eye review SF2: the flag can turn on only while another live, drawn actor's root is on the followed actor's (within
    // fpown::k_tol, 0.5): the shared-anchor signature that caused the bug, so normal play cannot turn it on whatever joints the
    // 26 sub-positions are. N3: the state's key carries the scene generation (a map change or load starts it again).
    // eye-grab (7 Nov 2026, the owner's sphere grabs on dj/djyt0000 and dj/djyt0600): the cloister sphere (f010, an object) sits
    // on Tidus's root while he takes or places it, so the shared root alone switched the rule on (8 s, two blinks) with his body
    // drawn at the root. Now the rule enters only while the root is shared AND the head sub-position (what the eye would follow)
    // is more than EyeLogic.k_enter units from the root horizontally; once on it stays on while the root stays shared (round 3b's
    // walk-back). A shared run that does not switch is logged once at its end (the sharing actor's slot, model and chr_id type,
    // the head's displacement): the hook for the carried-sphere backlog item.
    private EyeDisp _eye_disp; private int _eye_key = -1, _eye_lines, _eye_dec_lines;   // on / off lines 40 a session; decline lines their own 20 (review N1)
    private int _eye_dec_n, _eye_dec_slot = -1, _eye_dec_id, _eye_dec_far_k = -1; private float _eye_dec_max, _eye_dec_last, _eye_dec_far; private string _eye_dec_name = "", _eye_dec_who = "";
    private bool eye_disp_step(Actor* actors, ref Actor p, int pick, bool allowed, Vector3 head) {
        int key = (int)(((uint)pick << 16 | (uint)(p.chr_id & 0xFFFF)) & 0x3FFFFF | (body_hide_scene_gen(actors, pick) & 0x1FF) << 22);
        if (!allowed || key != _eye_key) { eye_decline_end(); _eye_disp = default; _eye_key = key; if (!allowed) return false; }
        fixed (Actor* pp = &p) {
            Vector3 root = *(Vector3*)((byte*)pp + 0x200);
            float h = EyeLogic.HeadOffset(head, root);
            int other = eye_root_shared(actors, pick, root, out int other_id, out nint other_name, out float other_d);   // every frame: the shared root
            _eye_share_slot = other; _eye_share_id = other_id; _eye_share_name = other_name; _eye_share_d = other_d; _eye_share_frame = _frames;   // fix-eye: the capture's columns and summary
            bool dec = EyeLogic.Declines(in _eye_disp, h, other >= 0);
            if (EyeLogic.Step(ref _eye_disp, h, other >= 0) && _eye_lines < 40) {
                _eye_lines++;
                float d = EyeLogic.BodyOffset(new ReadOnlySpan<Vector4>((byte*)pp + 0x524, 26), root);
                _logger.Info(I($"eye: {(_eye_disp.on ? "from the drawn head: another actor's root is on" : "back to the root: no other actor's root on")} '{actor_name(p.chr_name)}''s root for {EyeLogic.k_frames} frames{(_eye_disp.on && other >= 0 ? $" (actor {other} '{actor_name((char*)other_name)}', chr_id 0x{other_id:X})" : "")}, his head sub-position {h:F1} units from it horizontally (the nearest of his sub-positions {d:F1}) on map {map_name()}") + (_eye_lines == 40 ? " (no more of these lines this session)" : ""));
            }
            if (dec) {
                if (_eye_dec_n == 0 || other != _eye_dec_slot) {   // a new run, or another actor took the root: the run's actor is the latest
                    if (_eye_dec_n > 0 && other != _eye_dec_slot) eye_decline_end();
                    _eye_dec_slot = other; _eye_dec_id = other_id; _eye_dec_name = actor_name((char*)other_name); _eye_dec_who = actor_name(p.chr_name); _eye_dec_max = 0f; _eye_dec_far = 0f; _eye_dec_far_k = -1;
                }
                _eye_dec_n++; _eye_dec_last = h; if (h > _eye_dec_max) _eye_dec_max = h;
                // the drawn body's farthest sub-position from the root (the head pick of c001 is a helper over the root: this says
                // whether the body itself was drawn away from it during the run; diagnostic only)
                float far = EyeLogic.BodyFar(new ReadOnlySpan<Vector4>((byte*)pp + 0x524, 26), root, out int fk);
                if (far > _eye_dec_far) { _eye_dec_far = far; _eye_dec_far_k = fk; }
            }
            else eye_decline_end();
        }
        return _eye_disp.on;
    }
    /// <summary>eye-grab: the end of a shared-root run that the rule declined (k_frames frames or more): one module-log line (20 a
    /// session, a budget of their own: a cloister-heavy session cannot silence the on / off lines; review N1).</summary>
    private void eye_decline_end() {
        int n = _eye_dec_n; _eye_dec_n = 0;
        if (n < EyeLogic.k_frames || _eye_dec_lines >= 20) return;
        _eye_dec_lines++;
        uint type = (uint)_eye_dec_id >> 12;
        string kind = type == 0 ? "a party / event character model" : type == 2 ? "an NPC" : type == 4 ? "a 'w' model" : type == 5 ? "an object (chr/obj)" : $"type {type}";
        _logger.Info(I($"eye: stays on the root: actor {_eye_dec_slot} '{_eye_dec_name}' (chr_id 0x{_eye_dec_id:X}, {kind}) shared '{_eye_dec_who}''s root for {n} frames, but his head sub-position was at most {_eye_dec_max:F1} units from it horizontally (last {_eye_dec_last:F1}; the drawn head follows only past {EyeLogic.k_enter:F0}: a body drawn at its root, e.g. a carried or placed object); his farthest sub-position {_eye_dec_far:F1} units out (index {_eye_dec_far_k}) on map {map_name()}") + (_eye_dec_lines == 20 ? " (no more of these lines this session)" : ""));
    }
    /// <summary>Another live actor the game draws (not script-hidden) with its drawn root within 0.5 of `root` (game space): its slot
    /// (the first such), chr_id and name pointer; -1 when none.</summary>
    private int eye_root_shared(Actor* actors, int pick, Vector3 root, out int id, out nint name, out float dist) {
        id = 0; name = 0; dist = 0f;
        Actor* snap = tbl_snap(actors); if (snap == null) return -1;
        for (int i = 0; i < 48; i++) {
            if (i == pick || !FphWalk.Live(snap[i].chr_enabled, (nint)snap[i].chr_name) || slot_hidden(actors, snap, i)) continue;
            Vector3 r = *(Vector3*)((byte*)(snap + i) + 0x200);
            if (finite(r) && Vector3.DistanceSquared(r, root) < 0.5f * 0.5f) { id = (int)snap[i].chr_id; name = (nint)snap[i].chr_name; dist = Vector3.Distance(r, root); return i; }
        }
        return -1;
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

    // -- npc-anchor round 2: the exact owner by index buffer ----------------------------------------------------------------
    // research/fh-structs.md 1a, checked in the disassembly (0x63BC80 reads the chain, 0x63C540 / 0x66F7D0 build it, 0x592C60
    // binds it): actor +0x830 -> model object {+0 count | 0x80000000, +4 PMeshInstance*[]} -> instance +0 PMesh -> {+0 count,
    // +4 segments, 0x6C each}; a segment's index data at +0x30: +0x08 index count, +0x14 buffer count (0x7FFFFFFF), +0x18 the
    // buffer entries (inline when 1, else a pointer; 0x14 bytes each, ID3D11Buffer* at +0), +0x2C the byte offset, +0x30 bit 0 a
    // GPU buffer. Phyre draws a segment with IASetIndexBuffer(that buffer, format, that offset) and DrawIndexed(count, 0, 0).
    // The followed actor's segments and those of the live, drawn actors within 6 units go to native as (buffer, offset, count,
    // owner): a matched draw in the table belongs to that owner. Rebuilt when the set of actors, an actor's +0x830 or +0x1C4
    // (the shared model resource), the pick or the scene changes (and every 30 frames while a chain did not read whole); sent
    // every frame (native uses it only in the frame after a call). +0x830 is 0 for a few frames after a load (deferred
    // creation): that actor adds nothing until it is set. Two actors of one model share the mesh: their keys have both owners
    // and decide nothing. Each build is dumped to the module log (30 a session).
    private const int k_ib_max = 192, k_ib_segs = 96; private const float k_ib_range = 6f;
    private uint* _ib_q; byte* _ib_seg; private int _ib_n, _ib_dumps, _fph_ib_dec; private uint _ib_gen; private ulong _ib_sig, _ib_built, _ib_last_call = ulong.MaxValue - 1, _ib_logged; private bool _ib_incomplete;
    private void body_hide_ib(Actor* actors, Actor* snap, int pick, Vector3 c) {
        if (_fn_fp_ib_set == null) return;
        if (snap == null || (uint)pick >= 48) { _fph_ib_dec = _fn_fp_ib_set(null, 0, _ib_gen); return; }
        int* idx = stackalloc int[48]; int m = 0; idx[m++] = pick;
        for (int i = 0; i < 48; i++) {
            if (i == pick || !FphWalk.Live(snap[i].chr_enabled, (nint)snap[i].chr_name) || slot_hidden(actors, snap, i)) continue;
            Vector3 r = *(Vector3*)((byte*)(snap + i) + 0x200);
            if (finite(r) && Vector3.DistanceSquared(r, c) < k_ib_range * k_ib_range) idx[m++] = i;
        }
        ulong sig = 1469598103934665603UL ^ _fph_scene_gen;
        for (int k = 0; k < m; k++) {
            byte* a = (byte*)(snap + idx[k]);
            sig = (sig ^ (ulong)idx[k]) * 1099511628211UL; sig = (sig ^ (ulong)*(uint*)(a + 0x830)) * 1099511628211UL; sig = (sig ^ (ulong)*(uint*)(a + 0x1C4)) * 1099511628211UL;
        }
        // review 2 SF1: also after any gap in the pushes (a load, a menu, no followed actor: the buffers may have been freed and
        // reused at the same addresses behind an unchanged signature) and every 60 frames regardless; while a chain did not read
        // whole, every 30
        bool gap = _frames - _ib_last_call != 1;
        _ib_last_call = _frames;
        if (sig != _ib_sig || gap || _frames - _ib_built >= 60 || (_ib_incomplete && _frames - _ib_built >= 30)) ib_build(actors, snap, idx, m, sig, sig == _ib_sig && !gap);
        _fph_ib_dec = _fn_fp_ib_set(_ib_n > 0 ? _ib_q : null, _ib_n, _ib_gen);
    }
    private void ib_build(Actor* actors, Actor* snap, int* idx, int m, ulong sig, bool periodic) {
        if (_ib_q == null) { _ib_q = (uint*)Marshal.AllocHGlobal(k_ib_max * 16); _ib_seg = (byte*)Marshal.AllocHGlobal(k_ib_segs * 0x6C); }   // kept for the session
        _ib_sig = sig; _ib_built = _frames; _ib_gen++; _ib_n = 0; _ib_incomplete = false;
        var sb = _ib_dumps < 30 ? new System.Text.StringBuilder() : null;   // (no strings once the budget is spent)
        uint res0 = *(uint*)((byte*)(snap + idx[0]) + 0x1C4);
        string[] names = fp_names();
        for (int k = 0; k < m; k++) {
            byte* a = (byte*)(snap + idx[k]);
            // review 2 B1: a slot whose model is in the fp_actor list (c001, c101: a Tidus double, the field model coming back before
            // the event model leaves) is the player's, never "another actor's"
            int rank = k == 0 ? 0 : fp_name_rank(idx[k], snap[idx[k]].chr_name, snap[idx[k]].chr_id, names);
            uint owner = IbChain.Owner(k == 0, rank);
            int n0 = _ib_n; int segs = IbChain.Walk(*(nint*)(a + 0x830), owner, _ib_q, ref _ib_n, k_ib_max, _ib_seg, k_ib_segs, _fn_safe_read, out bool fail);
            if (fail) _ib_incomplete = true;
            if (k == 0 || !fail) fp_chain_mark(idx[k], *(nint*)(a + 0x830), fail && k == 0, snap[idx[k]].chr_name);   // fix-eye finding 1: the followed actor's model object failed (any whole read of it clears it; review B1: keyed on the object)
            uint res = *(uint*)(a + 0x1C4);
            if (sb != null) {
            sb.Append(I($" | actor {idx[k]} '{actor_name(snap[idx[k]].chr_name)}' {(k == 0 ? "the player's" : owner == 0 ? "the player's (a listed model: fp_actor)" : res == res0 ? "another actor's, SAME MODEL as the player's (its keys decide nothing)" : "another actor's")}: +0x830 0x{*(uint*)(a + 0x830):X}, +0x1C4 0x{res:X}, {segs} segments, {_ib_n - n0} keys{(fail ? " (a read failed: retried)" : "")}"));
            for (int e = n0; e < _ib_n && e < n0 + 4; e++) sb.Append(I($" {_ib_q[e * 4]:X8}+{_ib_q[e * 4 + 1]}/{_ib_q[e * 4 + 2]}"));
            if (_ib_n - n0 > 4) sb.Append(" ...");
            }
            // review 2 N1: without the followed actor's own keys the table could hold a same-model actor's keys as "another
            // actor's" alone: no table then (n = 0: the texture and position rules), and no reads for the others
            if (k == 0 && (fail || _ib_n == 0)) { _ib_n = 0; sb?.Append(" | the player's chain gave no keys: no table sent"); break; }
        }
        // review 2 N3: a build is logged only when its key set differs from the last one logged (30 a session)
        ulong ks = 1469598103934665603UL;
        for (int e = 0; e < _ib_n * 4; e++) ks = (ks ^ _ib_q[e]) * 1099511628211UL;
        if (sb != null && ks != _ib_logged) {
            bool changed_periodic = periodic && _ib_logged != 0;
            _ib_logged = ks; _ib_dumps++;
            _logger.Info(I($"body hide: IB table build {_ib_gen} on map {map_name()}: {_ib_n} entries (buffer+offset/count){(changed_periodic ? " (a periodic rebuild: the keys had changed under an unchanged signature)" : "")}{sb}") + (_ib_dumps == 30 ? " (no more of these lines this session)" : ""));
        }
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
        ImGui.TextDisabled(esc($"IB table: {_ib_n} segment keys (build {_ib_gen}); matched draws decided by it last frame: the player's {(uint)_fph_ib_dec >> 16}, another actor's {_fph_ib_dec & 0xFFFF}"));
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
    /// <summary>The first-person head, the one rule for push_player, optpos_log and the post-join snapshot (d7_join_record, which
    /// sa_apply's head comes from on almost every frame), and HeadOf with reach 0: the highest sub-position (game space has Y down,
    /// so the lowest Y) not on the vertical axis through the origin, leaving out, when reach > 0, points more than reach units
    /// sideways (X / Z) from sub-position 0 (the body's base). The other sub-positions of every party battle model sit within about
    /// 6 units of it, but Wakka's battle model carries index 11 about 14.5 units out and 8-12 units above his head (the ball he
    /// holds up), which put his eye ~1 m high (owner, 4-5 Nov 2026 captures). The callers pass the reach in battle only and 0
    /// elsewhere (swimming, cutscene poses and riding were never checked against it: review of 9b406b9, SF), and reach 0 is the old
    /// rule exactly. The reach is also skipped when sub-position 0 is on the axis (or its X / Z is not a number). When it applies,
    /// sub-position 0 qualifies itself (distance 0), so the limited pick only comes up empty if sub-position 0's Y is not a number;
    /// the old-rule pick is the fallback then. old = the pick without the reach (the old rule), for the "skipped" log line.
    /// Returns the index, or -1 (nothing off the axis).</summary>
    public static int HeadPick(ReadOnlySpan<Vector4> optpos, float reach, out int old) {
        int n = Math.Min(optpos.Length, 26);
        old = -1; float hy = float.MaxValue;
        for (int k = 0; k < n; k++) { Vector4 sp = optpos[k]; if (sp.Y < hy && MathF.Abs(sp.X) + MathF.Abs(sp.Z) > 0.01f) { hy = sp.Y; old = k; } }
        if (!(reach > 0f) || old < 0) return old;
        Vector4 b = optpos[0];
        if (!(MathF.Abs(b.X) + MathF.Abs(b.Z) > 0.01f)) return old;
        float r2 = reach * reach; int pick = -1; hy = float.MaxValue;
        for (int k = 0; k < n; k++) {
            Vector4 sp = optpos[k];
            if (!(sp.Y < hy) || !(MathF.Abs(sp.X) + MathF.Abs(sp.Z) > 0.01f)) continue;
            float dx = sp.X - b.X, dz = sp.Z - b.Z; if (!(dx * dx + dz * dz <= r2)) continue;
            hy = sp.Y; pick = k;
        }
        return pick >= 0 ? pick : old;
    }
    /// <summary>npc-anchor: another actor's head for the npc-anchor ties: HeadPick with reach 0 (the old highest-point rule) on
    /// purpose. The ties are field-only (body_hide_push gets !in_battle), and the reach is a battle-only rule (Wakka's ball is a
    /// battle-model point), so the field keeps exactly the rule push_player uses there. Folded onto the actor's position when it
    /// looks relative (more than 100 units off it), but never for an attached actor (review N3, push_player's rule: its +0x0C is not
    /// where it is; its sub-positions are built from the parent's bone and are world positions). False when none is set. Game space.</summary>
    public static bool HeadOf(ReadOnlySpan<Vector4> optpos, Vector3 pos, bool attached, out Vector3 head) {
        head = default;
        int k = HeadPick(optpos, 0f, out _); if (k < 0) return false;
        Vector4 sp = optpos[k]; head = new Vector3(sp.X, sp.Y, sp.Z);
        if (!attached && MathF.Abs(head.X - pos.X) + MathF.Abs(head.Z - pos.Z) > 100f) head += pos;
        return true;
    }
}

/// <summary>npc-anchor round 3: the eye's displaced-body state (EyeLogic.Step).</summary>
internal struct EyeDisp { public bool on; public int n_in, n_out; }
/// <summary>npc-anchor round 3: does the eye follow the drawn head (the body may be drawn anywhere around a shared root)? Pure
/// (the scratch test drives it). Round 3b (the owner's retest, 14:28): the distance off-switch put the eye back on the anchor
/// when Tidus's feet walked back near it (1.4 units) with his head still 8.4 units away. The rule is now the shared root itself:
/// on after k_frames frames with another live, drawn actor's root on the followed one's, off after k_frames without; the body's
/// distance only guards plausibility (none set, or over k_max, breaks a run).
/// eye-grab (7 Nov 2026): a cloister sphere (f010) on Tidus's root while he takes it met that rule with his body drawn at the
/// root. Entering now also needs the head sub-position (what the eye follows) more than k_enter units from the root
/// horizontally, on each of the k_frames frames; leaving is unchanged (the shared root gone for k_frames), so the walk-back
/// stays on.</summary>
internal static class EyeLogic {
    public const float k_max = 400f;   // units (horizontal): a head farther than this is not believed
    public const int k_frames = 10;    // consecutive frames to switch either way
    // eye-grab: 12 units (1.05 m at 11.4 units/m). A body drawn at its root cannot put its head that far out horizontally by a
    // lean or a reach (a standing bend or a kneel moves the neck ~0.5-0.8 m, 6-9 units); in the owner's sphere grabs it was 0.0
    // (c001's head pick is a helper point exactly 18 units over the root) and the drawn palette head within 3.9 of it; on
    // mi/mihn0100 the displaced head was 67 units out when the scene began.
    public const float k_enter = 12f;
    /// <summary>eye-grab: the horizontal distance from the root to the head sub-position (game space); -1 when not finite.</summary>
    public static float HeadOffset(Vector3 head, Vector3 root) {
        float dx = head.X - root.X, dz = head.Z - root.Z;
        return float.IsFinite(dx) && float.IsFinite(dz) ? MathF.Sqrt(dx * dx + dz * dz) : -1f;
    }
    /// <summary>eye-grab: this frame the root is shared but the rule stays on the root because the head is near it (for the log).</summary>
    public static bool Declines(in EyeDisp s, float h, bool shared) => !s.on && shared && h >= 0f && h <= k_enter;
    /// <summary>eye-grab (diagnostics): the horizontal distance from the root to the FARTHEST sub-position (those off the origin's
    /// axis, push_player's rule) and its index (k); 0 / -1 when none is set.</summary>
    public static float BodyFar(ReadOnlySpan<Vector4> optpos, Vector3 root, out int k) {
        float best = 0f; k = -1;
        for (int i = 0; i < optpos.Length && i < 26; i++) {
            Vector4 sp = optpos[i];
            if (!(MathF.Abs(sp.X) + MathF.Abs(sp.Z) > 0.01f) || !float.IsFinite(sp.X) || !float.IsFinite(sp.Z)) continue;
            float dx = sp.X - root.X, dz = sp.Z - root.Z, d = MathF.Sqrt(dx * dx + dz * dz);
            if (d > best && d <= k_max) { best = d; k = i; }
        }
        return best;
    }
    /// <summary>The horizontal distance from the root to the nearest sub-position (those off the origin's axis, push_player's
    /// rule), game space; -1 when none is set.</summary>
    public static float BodyOffset(ReadOnlySpan<Vector4> optpos, Vector3 root) {
        float best = float.MaxValue;
        for (int k = 0; k < optpos.Length && k < 26; k++) {
            Vector4 sp = optpos[k];
            if (!(MathF.Abs(sp.X) + MathF.Abs(sp.Z) > 0.01f) || !float.IsFinite(sp.X) || !float.IsFinite(sp.Z)) continue;
            float dx = sp.X - root.X, dz = sp.Z - root.Z, d = MathF.Sqrt(dx * dx + dz * dz);
            if (d < best) best = d;
        }
        return best == float.MaxValue ? -1f : best;
    }
    /// <summary>One frame: on after k_frames frames in a row with another actor's root on the followed one's (shared; eye review
    /// SF2) and the head h more than k_enter units from it horizontally (eye-grab); off after k_frames without the shared root,
    /// whatever h (round 3b). An unknown or implausible h (none, over k_max) breaks a run in progress and changes nothing. True
    /// when it switched.</summary>
    public static bool Step(ref EyeDisp s, float h, bool shared) {
        if (!(h >= 0f) || h > k_max) { s.n_in = 0; s.n_out = 0; return false; }
        if (!s.on) { s.n_in = shared && h > k_enter ? s.n_in + 1 : 0; if (s.n_in >= k_frames) { s.on = true; s.n_in = 0; s.n_out = 0; return true; } }
        else { s.n_out = !shared ? s.n_out + 1 : 0; if (s.n_out >= k_frames) { s.on = false; s.n_in = 0; s.n_out = 0; return true; } }
        return false;
    }
}

/// <summary>npc-anchor round 2: one actor's model chain to its segments' index-buffer keys (the layout in body_hide_ib's comment).
/// Pure but for the guarded reads (rd: the native safe_read; a test passes its own). Appends (buffer, byte offset, index count,
/// owner) quadruples to q from n (up to max); seg is scratch for up to max_segs segment records. Returns the segments read;
/// fail = a read inside a set chain failed (the caller retries later).</summary>
internal static unsafe class IbChain {
    /// <summary>review 2 B1: the followed actor and any slot whose model is in the fp_actor list (name rank >= 0) are the player's (0),
    /// every other actor another actor's (1).</summary>
    public static uint Owner(bool followed, int name_rank) => followed || name_rank >= 0 ? 0u : 1u;
    public static int Walk(nint h, uint owner, uint* q, ref int n, int max, byte* seg, int max_segs, delegate* unmanaged[Cdecl]<void*, void*, uint, int> rd, out bool fail) {
        fail = false; int segs = 0;
        if (h == 0 || rd == null) return 0;
        uint hc = 0; nint arr = 0;
        if (rd((void*)h, &hc, 4) == 0 || rd((byte*)h + 4, &arr, (uint)sizeof(nint)) == 0 || arr == 0) { fail = true; return 0; }
        int ni = (int)(hc & 0x7FFFFFFF); if (ni > 16) ni = 16;
        for (int i = 0; i < ni; i++) {
            nint inst = 0, mesh = 0, sp = 0; uint sc = 0;
            if (rd((byte*)arr + i * sizeof(nint), &inst, (uint)sizeof(nint)) == 0 || inst == 0 || rd((void*)inst, &mesh, (uint)sizeof(nint)) == 0 || mesh == 0
                || rd((void*)mesh, &sc, 4) == 0 || rd((byte*)mesh + 4, &sp, (uint)sizeof(nint)) == 0) { fail = true; continue; }
            int ns = (int)(sc & 0x7FFFFFFF); if (ns > max_segs) ns = max_segs;
            if (ns == 0 || sp == 0) continue;
            if (rd((void*)sp, seg, (uint)(ns * 0x6C)) == 0) { fail = true; continue; }
            for (int s = 0; s < ns; s++) {
                byte* blk = seg + s * 0x6C + 0x30;   // PIndexDataBlockD3D11, inline in the segment
                if ((blk[0x30] & 1) == 0) continue;   // no GPU buffer (0x592C60 binds none)
                uint cnt = *(uint*)(blk + 0x08), off = *(uint*)(blk + 0x2C), bc = *(uint*)(blk + 0x14) & 0x7FFFFFFF;
                if (bc == 0) continue;
                segs++;
                for (int k = 0; k < (int)bc && k < 4 && n < max; k++) {
                    uint ib = 0;
                    if (bc == 1) ib = *(uint*)(blk + 0x18);   // the one entry inline: its ID3D11Buffer* first
                    else { nint ea = *(nint*)(blk + 0x18); if (ea == 0 || rd((byte*)ea + 0x14 * k, &ib, 4) == 0) { fail = true; continue; } }
                    if (ib == 0) continue;
                    q[n * 4] = ib; q[n * 4 + 1] = off; q[n * 4 + 2] = cnt; q[n * 4 + 3] = owner; n++;
                }
            }
        }
        return segs;
    }
}
