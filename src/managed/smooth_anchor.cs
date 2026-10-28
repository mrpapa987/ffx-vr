// SPDX-License-Identifier: MIT

namespace FFXVR;

// Smooth follow anchor (design-notes "Smooth follow anchor"; SYNTHESIS 4.1, exe-engine 1.3, exe-verify 8).
// The followed actor's +0x0C (chr_pos_vec) is the 30 Hz logic position; FFX-60fps draws the world from the interpolated root
// matrix +0x1D0 (translation at +0x200, = +0x0C + chr_offset +0x6C, lerped between ticks), restored at the next invocation's
// pre(). Read at Present the root may be the lerp, the canonical tick or mid-write, so it is snapshotted on the main thread in
// build 7's job-join post-hook (FhUtil 0x644B30; the first join after the frame counter moved is Sg_MainLoop's post-join call),
// with +0x0C, +0x6C, the head sub-position (push_player's rule) and the game frame stamp, through the D7Join seqlock. Here
// (render thread) the anchor becomes +0x0C + d and the head head + d, with d = drawn - (logic + chr_offset): the same
// interpolation applied to both, so the eye moves at the drawn rate. Used only when fresh (stamp 0..2 frames old), for the
// actor push_player follows, and not for attached actors (+0x19C) or an override matrix (+0x10C); otherwise +0x0C as before.
public sealed unsafe partial class FFXVRModule {

    private bool   _smooth_anchor = true;     // ini smooth_anchor (default 1)
    private bool   _sa_used;                  // this frame's anchor came from the snapshot
    private int    _sa_age = 999;             // the snapshot's age in game frames (0..2 = fresh)
    private string _sa_status = "";
    private Vector3 _sa_prev_pos, _sa_prev_logic; private bool _sa_have_prev;
    private float  _sa_step, _sa_step_logic;  // per-Present step of the anchor used and of +0x0C (render-space units), for the Motion log
    private bool   _h7_join_ok;               // the job-join hook was queued (diag7_init)
    private uint   _sa_prev_stamp; private int _sa_stamp_step;   // SM-3: the stamp step between the snapshots used at consecutive Presents (1 = paired every frame)
    private int    _sa_rejects, _sa_flat;     // SM-1: snapshots refused as implausible; SM-7: used frames in a row with no interpolation while moving
    private const float k_sa_max_d = 16f;     // SM-1: the interpolation offset never exceeds one logic step (a run step is a few units)

    /// <summary>push_player, render thread: the followed actor's anchor (game space, in/out) and head (in/out). The logic
    /// position comes in as today's value; it is replaced by the smoothed one when the snapshot allows.</summary>
    private void sa_apply(nint act, ref Vector3 pos, ref Vector3 head, ref bool have_head) {
        Vector3 logic = pos;
        d7_read(_d7j, ref _d7j_seq, ref _r7j, ref _r7j_seq);
        uint frame = FhUtil.get_at<uint>(0x1FCBBF0);
        _sa_age = _r7j_seq != 0 ? (int)(frame - _r7j.stamp) : 999;
        _sa_used = false;
        string why;
        if (!_smooth_anchor) why = "off (smooth_anchor=0): the 30 Hz logic position, as before";
        else if (!_h7_join_ok) why = "unavailable: the job-join hook is not installed (smooth_anchor was 0 and diag_build7=0 at launch: it hooks at the next launch; or the hook failed); the 30 Hz logic position";
        else if (_r7j_seq == 0 || _r7j.valid == 0) why = "waiting for a snapshot (the 30 Hz logic position meanwhile)";
        else if (_r7j.actor != act) why = "the followed actor just changed (the 30 Hz logic position for a frame)";
        else if (_r7j.flags != 0) why = (_r7j.flags & 1) != 0 ? "attached actor (riding / carried): the 30 Hz logic position" : "script matrix (+0x10C): the 30 Hz logic position";
        else if (_sa_age < 0 || _sa_age > 2) why = "snapshot too old: the 30 Hz logic position";   // (the age is shown separately)
        else {
            Vector3 d = _r7j.td - (_r7j.pos + _r7j.off);   // the interpolation FFX-60fps applied to the drawn root this invocation
            if (d.LengthSquared() > k_sa_max_d * k_sa_max_d) { _sa_rejects++; why = "drawn root far from the logic position (not built yet?): the 30 Hz logic position"; }   // SM-1
            else {
                pos = _r7j.pos + d;
                if (_r7j.head_ok != 0) head = _r7j.head + d; else have_head = false;   // SM-4: never a Present-time head with the snapshot's position
                _sa_used = true; why = "on: the drawn (interpolated) root";
                float step_logic = _sa_have_prev ? Vector3.Distance(logic, _sa_prev_logic) : 0f;
                if (d.LengthSquared() < 1e-8f) { if (step_logic > 0.1f) _sa_flat++; } else _sa_flat = 0;   // SM-7: with FFX-60fps smoothing on, d is non-zero on every logic invocation
            }
        }
        _sa_status = why;
        // Motion log: per-Present steps of the anchor used and of +0x0C (render space distances), and the stamp step (SM-3)
        if (_sa_have_prev) { _sa_step = Vector3.Distance(pos, _sa_prev_pos); _sa_step_logic = Vector3.Distance(logic, _sa_prev_logic); }
        _sa_stamp_step = _sa_used ? (int)(_r7j.stamp - _sa_prev_stamp) : 0; if (_sa_used) _sa_prev_stamp = _r7j.stamp;
        _sa_prev_pos = pos; _sa_prev_logic = logic; _sa_have_prev = true;
        sa_motion_cols(act);
    }

    /// <summary>The Motion log's managed columns (render space), every frame (native prints them only while the log runs):
    /// build 7's post-join drawn root, the Present-time drawn root, the post-join logic position, the exec byte, the snapshot's
    /// age and validity; then the smooth anchor: used, age, the step of the anchor used and of +0x0C this Present, and the stamp
    /// step between the snapshots used at consecutive Presents (1 every line = paired; 0 / 2 = a repeated / skipped snapshot).</summary>
    private void sa_motion_cols(nint act) {
        if (_fn_motion_cols == null) return;
        float* mc = stackalloc float[17];
        Vector3 ptd = default; bool pok = act != 0 && read((byte*)act + 0x200, out ptd);
        Vector3 jd = d7_render(_r7j.td), pd = d7_render(ptd), jp = d7_render(_r7j.pos);
        mc[0] = jd.X; mc[1] = jd.Y; mc[2] = jd.Z; mc[3] = pok ? pd.X : 0f; mc[4] = pok ? pd.Y : 0f; mc[5] = pok ? pd.Z : 0f;
        mc[6] = jp.X; mc[7] = jp.Y; mc[8] = jp.Z; mc[9] = _r7j.exec; mc[10] = Math.Min(_sa_age, 999); mc[11] = _r7j_seq != 0 && _r7j.valid != 0 ? 1f : 0f;
        mc[12] = _sa_used ? 1f : 0f; mc[13] = Math.Min(_sa_age, 999); mc[14] = _sa_step; mc[15] = _sa_step_logic; mc[16] = Math.Clamp(_sa_stamp_step, -999, 999);
        _fn_motion_cols(mc, 17);
    }
}
