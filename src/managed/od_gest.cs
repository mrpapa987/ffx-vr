// SPDX-License-Identifier: MIT

namespace FFXVR;

// Overdrive gestures (roadmap 4.3, 6 Nov 2026; native ffxvr_odgest.h, ABI 47). While an overdrive's input window is open
// (exactly one of the state words at 4 and the shared timer running; native reads them every Present in battle), the Quest
// controllers become the weapon, and the sticks and buttons keep working at the same time:
//   Lulu's Fury     circles of either hand, in any plane: the right stick turns with the hand (the game's quadrants)
//   Tidus's Swordplay  a quick swing of the right hand presses A once per swing (the bring-back and a reach up are not
//                   swings); off by default (od_gest_tidus_swing=0) until the owner's test: a press outside the zone RESTARTS the bar
//                   (0x8923F8..0x89244F: the cursor back to its start while the timer runs), so a false A costs a traversal
//   Auron's Bushido slashes left / right / up / down = the d-pad, a punch forward = LB (left hand) / RB (right hand), the face
//                   buttons on the controllers; off by default (od_gest_auron=0) until one capture confirms the game's values
// Native decides and logs (odg: lines: one per window, Auron's expected sequence and every press the game saw); this file
// pushes the settings, draws the F8 rows, and (Debug, a diagnostic profile, this session only) sets the game's own
// "overdrives always ready" flag. A hand grabbing the world gestures nothing; in an injecting Bushido window the grips do not
// grab (a clenched punch is a punch). Bushido's opposite pairs in a row (Shooting Star's Left, Right; Banishing Blade's
// Right, Left) go at once when the game expects that button next (the play2 capture's map of the sequence values, native
// k_auron_map; the mod still learns each value it sees as a cross-check and logs any disagreement); any other bring-back of
// the same hand within 0.3 s is ignored.
// ini: od_gestures (1), od_gest_lulu (1), od_gest_tidus_swing (0; the old key od_gest_tidus, saved as 1 by the first build,
// is ignored so the new default reaches every ini), od_gest_auron (0), od_gest_haptics (1), od_circle_cm (1-20; 4),
// od_slash_speed (0.8-5 m/s; 1.6), od_lulu_stick (0 right, 1 left, 2 both; 0).
public sealed unsafe partial class FFXVRModule {
    private bool  _od_gestures = true, _od_gest_lulu = true, _od_gest_tidus = false, _od_gest_auron = false, _od_gest_haptics = true;
    private float _od_circle_cm = 4f, _od_slash_speed = 1.6f;
    private int   _od_lulu_stick = 0;
    private bool  _od_always_ready;     // Debug, a diagnostic profile, session only (never saved): Btl.debug.always_available_overdrive in every battle
    private bool  _od_always_wrote;     // we set the flag in this battle (cleared at its end)
    private delegate* unmanaged[Cdecl]<int, float, float, int, void> _fn_odg_set;   // ABI 47
    private delegate* unmanaged[Cdecl]<int, byte*, int, int> _fn_odg_text;
    private static readonly string[] k_odg_stick = { "right stick", "left stick", "both sticks" };

    private void odg_bind(Func<string, nint> req) {
        _fn_odg_set = (delegate* unmanaged[Cdecl]<int, float, float, int, void>)req("ffxvr_odg_set");
        _fn_odg_text = (delegate* unmanaged[Cdecl]<int, byte*, int, int>)req("ffxvr_odg_text");
    }
    private void odg_clear() { _fn_odg_set = null; _fn_odg_text = null; }
    private bool odg_parse(string k, string v) {
        switch (k) {
            case "od_gestures":     pb(k, v, ref _od_gestures); return true;
            case "od_gest_lulu":    pb(k, v, ref _od_gest_lulu); return true;
            case "od_gest_tidus":   return true;   // round 2: the first build's key (saved as 1) is ignored: Tidus's swing starts off
            case "od_gest_tidus_swing": pb(k, v, ref _od_gest_tidus); return true;
            case "od_gest_auron":   pb(k, v, ref _od_gest_auron); return true;
            case "od_gest_haptics": pb(k, v, ref _od_gest_haptics); return true;
            case "od_circle_cm":    pf(k, v, ref _od_circle_cm, 1f, 20f); return true;
            case "od_slash_speed":  pf(k, v, ref _od_slash_speed, 0.8f, 5f); return true;
            case "od_lulu_stick":   pe(k, v, ref _od_lulu_stick, 0, 1, 2); return true;
            default: return false;
        }
    }
    private string odg_ini_text() {
        var ci = CI;
        return
            $"od_gestures={(_od_gestures ? 1 : 0)}\n" +
            $"od_gest_lulu={(_od_gest_lulu ? 1 : 0)}\n" +
            $"od_gest_tidus_swing={(_od_gest_tidus ? 1 : 0)}\n" +
            $"od_gest_auron={(_od_gest_auron ? 1 : 0)}\n" +
            $"od_gest_haptics={(_od_gest_haptics ? 1 : 0)}\n" +
            $"od_circle_cm={_od_circle_cm.ToString("0.#", ci)}\n" +
            $"od_slash_speed={_od_slash_speed.ToString("0.##", ci)}\n" +
            $"od_lulu_stick={_od_lulu_stick.ToString(ci)}\n";
    }
    /// <summary>Every frame from push_all: the settings; the Debug test aid.</summary>
    private void odg_push() {
        if (_fn_odg_set != null)
            _fn_odg_set((_od_gestures ? 1 : 0) | (_od_gest_lulu ? 2 : 0) | (_od_gest_auron ? 4 : 0) | (_od_gest_tidus ? 8 : 0) | (_od_gest_haptics ? 16 : 0),
                        _od_circle_cm, _od_slash_speed, _od_lulu_stick);
        odg_always_tick();
    }
    /// <summary>The test aid (design 4.3 step 6): Btl + 0x28 (debug flags) + 0x14 = VA 0x112A90C, the game's own "overdrive always
    /// available" (debug-revival, zanarkand, fh-structs agree; Fahrenheit's BtlDebugFlags.always_available_overdrive). Set in every
    /// battle while ticked, cleared at the battle's end and when unticked; the box is not saved. Never in the Release logging
    /// profile (no diagnostic key on: diag_any): the box is off there. While it is on the gauge is not spent (debug-revival
    /// battle.md: MsCheckUseCommand / MsCalcMagic), so a save made later keeps a full gauge.</summary>
    private void odg_always_tick() {
        if (!diag_any()) _od_always_ready = false;
        byte* btl = (byte*)Globals.Battle.btl;
        if (btl == null) return;
        bool live = read(btl + 0x10, out byte bs) && bs != 0;
        if (_od_always_ready && live) {
            if (read(btl + 0x3C, out byte f) && f == 0) { btl[0x3C] = 1; if (!_od_always_wrote) d7_log("odg: test aid: overdrives always ready in this battle (the game's debug flag, F8 Debug)"); _od_always_wrote = true; }
        }
        else if (_od_always_wrote) {
            if (read(btl + 0x3C, out byte f) && f != 0) btl[0x3C] = 0;
            _od_always_wrote = false;
        }
    }
    private string odg_text(int which) {
        if (_fn_odg_text == null) return "";
        byte* b = stackalloc byte[768];
        int n = _fn_odg_text(which, b, 768);
        return n > 0 ? Marshal.PtrToStringAnsi((nint)b) ?? "" : "";
    }
    /// <summary>F8, the Controllers section.</summary>
    private void odg_ui() {
        ImGui.Checkbox("overdrive gestures (the controllers keep working too):", ref _od_gestures);
        if (!_od_gestures) ImGui.BeginDisabled(true);
        ImGui.SameLine(); ImGui.Checkbox("Lulu's Fury: circles##odg", ref _od_gest_lulu);
        ImGui.SameLine(); ImGui.Checkbox("Tidus's Swordplay: swing (experimental: a miss restarts the bar)##odg", ref _od_gest_tidus);
        ImGui.SameLine(); ImGui.Checkbox("Auron's Bushido: slashes and punches (experimental)##odg", ref _od_gest_auron);
        ImGui.SameLine(); ImGui.Checkbox("buzz (also on a wrong Bushido input or a Swordplay miss from a button)##odg", ref _od_gest_haptics);
        ImGui.SetNextItemWidth(140f); ImGui.SliderFloat("smallest circle (cm)##odg", ref _od_circle_cm, 1f, 20f, "%.0f");
        ImGui.SameLine(); ImGui.SetNextItemWidth(140f); ImGui.SliderFloat("slash / swing speed (m/s)##odg", ref _od_slash_speed, 0.8f, 5f, "%.1f");
        ImGui.SameLine(); ImGui.SetNextItemWidth(120f);
        if (ImGui.BeginCombo("Fury's stick##odg", k_odg_stick[_od_lulu_stick])) {
            for (int i = 0; i < 3; i++) if (ImGui.Selectable(k_odg_stick[i], i == _od_lulu_stick)) _od_lulu_stick = i;
            ImGui.EndCombo();
        }
        if (!_od_gestures) ImGui.EndDisabled();
        string warn = odg_text(3);
        if (warn.Length > 0) { ImGui.PushStyleColor(ImGuiCol.Text, new Vector4(1f, 0.6f, 0.2f, 1f)); ImGui.TextWrapped(esc(warn)); ImGui.PopStyleColor(1); }
        ImGui.TextDisabled(esc(odg_text(0)));
        if (_od_gest_auron) ImGui.TextDisabled("Bushido: an opposite pair in a row (Left, Right) goes at once when the game asks for it; any other bring-back of the same hand within 0.3 s is ignored");
        ImGui.TextDisabled("last: " + esc(odg_text(1)));
    }
    /// <summary>F8 Debug: the test aid and Auron's raw sequence.</summary>
    private void odg_debug_ui() {
        if (!ImGui.TreeNode("Overdrive gestures (test aid)")) return;
        if (diag_any()) {
            ImGui.Checkbox("overdrives always ready (the game's debug flag, in every battle this session; not saved; the gauge is not spent while on, so a later save keeps a full gauge)", ref _od_always_ready);
            ImGui.SameLine(); ImGui.TextDisabled(_od_always_wrote ? "set in this battle" : "");
        }
        else ImGui.TextDisabled("overdrives always ready: only with a diagnostic logging profile (e.g. diag_verbose=1), never in Release");
        ImGui.TextDisabled("Auron's expected sequence (raw, the last read): " + esc(odg_text(2)));
        ImGui.TreePop();
    }
}
