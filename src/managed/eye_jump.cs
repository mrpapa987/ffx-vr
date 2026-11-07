// SPDX-License-Identifier: MIT

namespace FFXVR;

// Eye jumps (roadmap 4.1; native ffxvr_eyejump.h, ABI 44).
// The native side blinks when the first-person eye base moves more than eye_jump_units in one frame (battle start, a swap or
// a summon handing the eyes on, the manual cycle, a scripted teleport), at most one blink start per 30 frames, and the
// blink now also blanks frames with a UI pass (all of battle, field frames with a text box) while the UI panel stays.
// This side asks native to face the enemies when the battle follow changes (battle_face): the centre of the live monsters,
// sent once the battle is drawn and some are found (review B1: the first follow change comes at battle_state 4, about 2-3 s
// before the battle is drawn, so nothing counts down during the load chain), and keeps the field heading over the battle
// (ffxvr_eye_event 2 at the start, 3 at the end; native keeps and restores it only with battle_face on).
//   ini eye_jump_blink (0-15 frames, 6; 0 = off), eye_jump_units (4-40, 8), battle_face (0 off, 1 body forward (default),
//   2 where you look). F8 > camera cuts, the "eye jumps:" and "battle: face the enemies:" lines.
public sealed unsafe partial class FFXVRModule {

    private int   _eye_jump_blink = 6;    // ini eye_jump_blink: black frames when the eye jumps (0 = off)
    private float _eye_jump_units = 8f;   // ini eye_jump_units: a jump is a move of more than this in one frame (about 0.7 m)
    private int   _battle_face = 1;       // ini battle_face: 0 off, 1 body forward (the enemies ahead of the recentred forward), 2 where you look
    private delegate* unmanaged[Cdecl]<int, float, int, int, int*, int, int> _fn_eye_jump_set;   // ABI 44
    private delegate* unmanaged[Cdecl]<int, float, float, float, void> _fn_eye_event;            // ABI 44

    // S3: the monster filter. The allocator 0x793530 makes one block of 31 Chr: 20 party, then 11 monsters (zanarkand.md,
    // "New detail from the allocator"); Chr+0x10 != 0 is the game's own per-Chr gate for every battle anchor (0x79F380,
    // exe-analysis-additions.md 19.2); ram (+0x540) current_hp +0x1A4 is Fahrenheit a12's ChrRam (chr.cs; fh-structs.md).
    // A monster farther than k_ej_mon_dist from the followed character, or at (0, 0, 0), is not one.
    // Dead or alive is the game's own test (fix-battle, 7 Nov logs finding 2): the target window's masks (0x898330, read by
    // 0x898640) take a chr when stat_death Chr+0xDCC == 0 (0x795780: != 0 is dead), stat_cursor Chr+0xDD9 == 1 (0x794CA0:
    // targetable) and stat_far Chr+0x4FF == 0 (0x794E00). Only the death byte is used here: an untargetable or far monster
    // is still an enemy to face. Chr+0x17 (Fahrenheit's stat_hide) is NOT a hide flag: it is 1 on visible, fighting monsters
    // (all 4 of the Sinspawn fight at 12000 / 4000 / 800 HP, 6 Nov; the turned requests of 7 Nov faced the one monster with
    // 0), so it is only logged, with the other bytes 0x16..0x1C, for the next capture.
    private const int   k_ej_mon_n = 11;
    private const int   k_ej_chr_in = 0x10, k_ej_chr_death = 0xDCC, k_ej_chr_cursor = 0xDD9;
    private const float k_ej_mon_dist = 400f;

    private int    _ej_face_left;          // live frames the face request still looks for monsters (0 = none pending)
    private string _ej_face_why = "";
    private int    _ej_face_logs;          // face: lines this session (60 at most)
    private readonly int[] _ej_stats = new int[7];   // native: jumps, blinks, extended, held back, headings turned, kept, cut blinks held back

    private bool ej_parse(string k, string v) {
        switch (k) {
            case "eye_jump_blink": pi(k, v, ref _eye_jump_blink, 0, 15); return true;
            case "eye_jump_units": pf(k, v, ref _eye_jump_units, 4f, 40f); return true;
            case "battle_face":    pe(k, v, ref _battle_face, 0, 1, 2); return true;
            default: return false;
        }
    }
    private string ej_ini_text() {
        var ci = CI;
        return
            $"eye_jump_blink={_eye_jump_blink.ToString(ci)}\n" +
            $"eye_jump_units={_eye_jump_units.ToString("F1", ci)}\n" +
            $"battle_face={_battle_face.ToString(ci)}\n";
    }
    /// <summary>Every frame (push_all, before push_player): the settings, in battle (last frame's, for the eye: lines), and the
    /// native counts for the overlay.</summary>
    private void ej_push() {
        if (_fn_eye_jump_set == null) return;
        fixed (int* o = _ej_stats) _fn_eye_jump_set(_eye_jump_blink, _eye_jump_units, _battle_face, _btl_was ? 1 : 0, o, 7);
    }
    /// <summary>push_player, at the in_battle edge (before the follow state is reset for the new battle). Native keeps the field
    /// heading only with battle_face on (S1: off clears it, so nothing is restored).</summary>
    private void ej_battle_edge(bool in_battle) {
        _ej_face_left = 0;
        if (_fn_eye_event != null) _fn_eye_event(in_battle ? 2 : 3, 0f, 0f, 0f);
    }
    /// <summary>push_player: the battle follow moved to another party member (start, swap, summon, dismissal, manual).</summary>
    private void ej_follow(string why) {
        if (_battle_face == 0 || (_fp_mode != 1 && _fp_mode != 3)) return;
        _ej_face_left = 120; _ej_face_why = why ?? "";
    }
    /// <summary>B1: Btl.battle_state classes, equal to ffxvr_eyejump.h ej::face_bst (keep the two the same): 1 the load chain
    /// (4..0x15 and 0x22: wait, no countdown), 2 live (0x16 the intro drawn in 3D, 1 / 2 the battle: count down and send),
    /// 0 anything else (the end states: count down only).</summary>
    private static int ej_bst(int bst) => (bst >= 4 && bst <= 0x15) || bst == 0x22 ? 1 : (bst == 0x16 || bst == 1 || bst == 2) ? 2 : 0;
    /// <summary>push_player, every battle frame (bst = Btl.battle_state): while a face request is pending and the battle is
    /// drawn, the centre of the monsters that pass the S3 filter, sent to native in render space (-x, -y, z).</summary>
    private void ej_face_tick(int bst) {
        if (_ej_face_left <= 0 || _fn_eye_event == null) return;
        int cls = ej_bst(bst);
        if (cls == 1) return;   // the load chain: the battle is not drawn yet, its actors not set up
        _ej_face_left--;
        if (cls != 2) { if (_ej_face_left == 0) ej_face_log(I($"face: battle follow ({_ej_face_why}): the battle ended (battle_state 0x{bst:X2}) before a monster was found, the heading is kept")); return; }
        // the followed character's position (the distance limit); none yet: keep looking
        Chr* plys = Globals.Battle.player_characters;
        if (plys == null || _btl_chr < 0 || !read(plys + _btl_chr, out Chr fc) || fc.actor == null || !read(fc.actor, out Actor fa) || !live(fa)) {
            if (_ej_face_left == 0) ej_face_log(I($"face: battle follow ({_ej_face_why}): the followed character is not readable within 120 live frames, the heading is kept"));
            return;
        }
        Vector3 me = new Vector3(fa.chr_pos_vec.X, fa.chr_pos_vec.Y, fa.chr_pos_vec.Z);
        int n = ej_monsters(me, out Vector3 sum, false, out _);
        if (n == 0) {
            if (_ej_face_left == 0) { ej_monsters(me, out _, true, out string all); ej_face_log(I($"face: battle follow ({_ej_face_why}): no live monster within 120 live frames, the heading is kept; monsters: {all}")); }
            return;
        }
        _ej_face_left = 0;
        Vector3 c = sum / n;
        _fn_eye_event(1, -c.X, -c.Y, c.Z);
        if (_ej_face_logs < 60) { ej_monsters(me, out _, true, out string all); ej_face_log(I($"face: battle follow ({_ej_face_why}) at battle_state 0x{bst:X2} -> the centre of {n} monster(s) at ({c.X:F0} {c.Y:F0} {c.Z:F0}), battle_face {_battle_face}; monsters: {all}")); }
    }
    /// <summary>S3: the monsters that count (Chr+0x10 set, actor live, stat_visible, alive by the game's own test (stat_death
    /// Chr+0xDCC == 0), HP above 0, not at (0, 0, 0), within k_ej_mon_dist of the followed character). Returns their number
    /// and position sum (game space); with detail, one entry per slot that has an actor: "i: in/vis/death/cursor/hp/dist",
    /// the bytes 0x16..0x1C, and "ok" or why not.</summary>
    private int ej_monsters(Vector3 me, out Vector3 sum, bool detail, out string text) {
        sum = Vector3.Zero; text = ""; int n = 0;
        Chr* mons = Globals.Battle.monster_characters;
        if (mons == null) { text = "no monster list"; return 0; }
        System.Text.StringBuilder sb = detail ? new System.Text.StringBuilder() : null;
        for (int i = 0; i < k_ej_mon_n; i++) {
            if (!read(mons + i, out Chr mc) || mc.actor == null || !read(mc.actor, out Actor ma)) continue;
            byte* mb = (byte*)&mc;   // the copy: Fahrenheit's Chr is the game's 0xF90 bytes
            byte inb = mb[k_ej_chr_in], death = mb[k_ej_chr_death];
            Vector3 p = new Vector3(ma.chr_pos_vec.X, ma.chr_pos_vec.Y, ma.chr_pos_vec.Z);
            float d = Vector3.Distance(p, me);
            bool zero = MathF.Abs(p.X) + MathF.Abs(p.Y) + MathF.Abs(p.Z) < 0.01f;
            int hp = mc.ram.current_hp;
            string why = ej_mon_why(live(ma), inb, mb[0x16], death, hp, zero, d);
            if (why == null) { sum += p; n++; }
            if (sb != null) {
                if (sb.Length > 0) sb.Append("; ");
                sb.Append(I($"{i}: in {inb} vis {mb[0x16]} death {death} cursor {mb[k_ej_chr_cursor]} hp {hp} dist {d:F0} b16-1C {mb[0x16]:X2} {mb[0x17]:X2} {mb[0x18]:X2} {mb[0x19]:X2} {mb[0x1A]:X2} {mb[0x1B]:X2} {mb[0x1C]:X2} {why ?? "ok"}"));
            }
        }
        if (sb != null) text = sb.Length > 0 ? sb.ToString() : "none";
        return n;
    }
    /// <summary>The filter's verdict for one monster slot (null: it counts). Pure: in = Chr+0x10, vis = stat_visible Chr+0x16,
    /// death = stat_death Chr+0xDCC (the game's own dead test, 0x795780), hp = the current HP, zero = its root at (0, 0, 0),
    /// d = its distance from the followed character. Mirrored by scratchpad/fixb_t/ej_replay.py (the 6 / 7 Nov replay).</summary>
    internal static string ej_mon_why(bool live, byte inb, byte vis, byte death, int hp, bool zero, float d) =>
        !live ? "not live" : inb == 0 ? "not in battle" : vis == 0 ? "not visible" : death != 0 ? "dead"
        : hp <= 0 ? "no HP" : zero ? "at (0, 0, 0)" : !(d <= k_ej_mon_dist) ? "too far" : null;
    private void ej_face_log(string s) {
        if (_ej_face_logs >= 60) return;
        _ej_face_logs++;
        _logger.Info(_ej_face_logs == 60 ? s + " (no more of these lines this session)" : s);
    }
    /// <summary>F8, under "camera cuts:" (next to "blink").</summary>
    private void ej_ui() {
        ImGui.Text("eye jumps:"); ImGui.SameLine();
        ImGui.SetNextItemWidth(140); ImGui.SliderInt("blink frames##ej", ref _eye_jump_blink, 0, 15, _eye_jump_blink == 0 ? "off" : "%d"); ImGui.SameLine();
        ImGui.SetNextItemWidth(140); ImGui.SliderFloat("jump over (units)##ej", ref _eye_jump_units, 4f, 40f, "%.0f");
        if (_fn_eye_jump_set != null) { ImGui.SameLine(); ImGui.TextDisabled($"seen {_ej_stats[0]}, blinked {_ej_stats[1]} (extended {_ej_stats[2]}, held back {_ej_stats[3]}){(_cut_blink ? $"; cut blinks held back {_ej_stats[6]}" : "")}"); }
        ImGui.Text("battle: face the enemies:"); ImGui.SameLine();
        ImGui.RadioButton("off##bf", ref _battle_face, 0); ImGui.SameLine();
        ImGui.RadioButton("body forward##bf", ref _battle_face, 1); ImGui.SameLine();
        ImGui.RadioButton("where you look##bf", ref _battle_face, 2);
        if (_fn_eye_jump_set != null) { ImGui.SameLine(); ImGui.TextDisabled($"turned {_ej_stats[4]}, kept {_ej_stats[5]} (under 15 deg)"); }
    }
}
