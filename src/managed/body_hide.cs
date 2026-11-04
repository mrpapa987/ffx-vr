// SPDX-License-Identifier: MIT

namespace FFXVR;

// The first-person body hide's on-foot ownership (design-notes "NPCs vanishing in first-person cutscenes (4 Nov 2026)").
// Native hides any skinned draw within hide_radius + the step margin of the hide centre; an NPC standing beside Tidus in a
// cutscene (Auron on lc/lchb0500, Yuna on lu/luca0600) went with him whenever the margin widened. Every frame push_player sends
// native the followed actor's drawn root and table index (a new index resets the margin), and in the field the drawn roots
// (+0x200) of the other live actors within 30 units: a draw nearer another actor's root than the player's is not the player's
// (ffxvr_fpown.h). While riding the mount is left out (it has its own rule, ride.cs); riding itself is unchanged.
public sealed unsafe partial class FFXVRModule {

    private const int   k_fph_max = 32;      // native keeps at most 32 (fpown::k_others_max)
    private const float k_fph_range = 30f;   // units around the followed actor's root (an NPC's parts sit on its own root)
    private delegate* unmanaged[Cdecl]<float, float, float, int, int, float*, void> _fn_fp_self_set;   // ABI 37
    private delegate* unmanaged[Cdecl]<float*, int, int> _fn_fp_others_set;                           // ABI 37
    private float _fph_radius = 0.75f;   // native: the on-foot hide radius in effect (hide_radius + the capped margin)
    private int   _fph_kept, _fph_others_n;   // native: last frame's draws refused as another actor's; the roots sent this frame

    /// <summary>push_player, right after ffxvr_player_set (render thread): the followed actor's drawn root (game space; root_ok
    /// false = not known, native then uses the hide centre alone) and index, and in the field the other actors' roots near it.
    /// centre (game space) is where the 30-unit range is measured from when the drawn root is not known.</summary>
    private void body_hide_push(Actor* actors, int pick, bool field, Vector3 root, bool root_ok, Vector3 centre) {
        if (_fn_fp_self_set != null) { float r = 0f; _fn_fp_self_set(-root.X, -root.Y, root.Z, root_ok ? 1 : 0, pick, &r); _fph_radius = r; }
        if (_fn_fp_others_set == null) return;
        float* buf = stackalloc float[48 * 3]; float* dist = stackalloc float[48];
        int n = 0;
        if (field && actors != null && pick >= 0) {
            Vector3 c = root_ok ? root : centre;
            int skip = _ride_att && _ride_mount_ok ? _ride_mount_idx : -1;   // riding: the bird has its own rule
            for (int i = 0; i < 48; i++) {
                if (!read(actors + i, out Actor a)) break;
                if (i == pick || i == skip || !live(a) || script_hidden(actors + i)) continue;   // a script-hidden actor is not drawn
                if (!read((byte*)(actors + i) + 0x200, out Vector3 td) || !finite(td)) continue;
                if (MathF.Abs(td.X) + MathF.Abs(td.Y) + MathF.Abs(td.Z) < 0.01f) continue;   // not built (a fresh slot, a parked model)
                float d = Vector3.Distance(td, c);
                if (!(d < k_fph_range)) continue;
                buf[n * 3] = -td.X; buf[n * 3 + 1] = -td.Y; buf[n * 3 + 2] = td.Z; dist[n] = d; n++;   // render space negates X and Y
            }
            if (n > k_fph_max) {   // nearest first: a partial selection sort of the 32 nearest
                for (int k = 0; k < k_fph_max; k++) {
                    int b = k; for (int j = k + 1; j < n; j++) if (dist[j] < dist[b]) b = j;
                    if (b == k) continue;
                    (dist[k], dist[b]) = (dist[b], dist[k]);
                    for (int q = 0; q < 3; q++) (buf[k * 3 + q], buf[b * 3 + q]) = (buf[b * 3 + q], buf[k * 3 + q]);
                }
                n = k_fph_max;
            }
        }
        _fph_others_n = n;
        _fph_kept = _fn_fp_others_set(n > 0 ? buf : null, n);
    }

    /// <summary>The followed actor's drawn root for the ownership test (game space): on foot the smooth anchor's post-join
    /// snapshot (what this frame draws) when it was used, else +0x200 read now; riding, the rider's root (native's riding rule
    /// does not use it). False when there is none (native then compares with the hide centre).</summary>
    private bool body_hide_root(Actor* actors, int pick, bool attached, out Vector3 root) {
        root = default;
        if (attached) { root = _ride_rider; return true; }
        if (_sa_used && finite(_r7j.td)) { root = _r7j.td; return true; }
        if (actors == null || pick < 0 || !read((byte*)(actors + pick) + 0x200, out Vector3 td) || !finite(td)) return false;
        if (MathF.Abs(td.X) + MathF.Abs(td.Y) + MathF.Abs(td.Z) < 0.01f) return false;
        root = td; return true;
    }

    /// <summary>The First person / follow section's body hide line (F8, Debug).</summary>
    private void body_hide_ui() {
        if (_fn_fp_self_set == null) return;
        if (_ride_att) { ImGui.TextDisabled(esc($"body hide: riding (the chocobo rule, radius {_ride_hide_radius:F1})")); return; }
        ImGui.Text(esc($"body hide: radius {_fph_radius:F2}, NPC parts kept visible last frame {_fph_kept}") + "  "); ImGui.SameLine();
        ImGui.TextDisabled(esc($"(other actors' roots sent: {_fph_others_n}; a draw nearer one of them than the player's root is theirs)"));
    }
}
