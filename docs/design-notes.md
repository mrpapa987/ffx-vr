
## Battle camera control (noted 17 Oct 2026)

Beyond head tracking, battles should offer deliberate camera control:

- **Grab-and-move, Demeo style**: grab the world with a controller and drag
  it; grab with both hands to zoom (scale) and rotate. The world moves, not
  the player, which is comfortable and makes the battle a tabletop/diorama.
- **Free-flight exploration, Walkabout Mini Golf style**: a fly-around mode
  for looking at the arena and the characters from anywhere.

Both fit the existing override model (base camera + offset); the gesture
math drives the offset. Scale (zoom) means the eye separation must scale with
the world so stereo stays correct. Exploration flight in the overworld would
hit culling; in battle the arena is small and fully drawn.

## Culling and the "Calm Lands from above" view (17 Oct 2026)

Wanted: observe a whole area from high above (Calm Lands, tiny chocobo).
Three mechanisms could hide geometry; status unknown until tested there:
1. Frustum culling against the game's camera. Fixes: widen the game camera's
   FOV/planes so its frustum encloses ours; or move the real game camera via
   the scripted "cam" functions Fahrenheit catalogues (then culling, lighting
   and UI projection all follow; field movement becomes camera-relative to
   the new spot, fine for an observe mode).
2. Zone loading: unloaded zones cannot be shown. Accept.
3. Distance fog: parameters live in the renderer globals block (and in every
   per-object buffer we patch) and can be pulled back for an observe mode.
Test: Calm Lands, override on, numpad 7 / minus until looking down.

## External design doc, assessed (17 Oct 2026)

A brainstormed "PCVR Technical Design & Integration Guide" was reviewed.

Adopt now:
- **Test the battle debug free-camera flag** (`Btl.debug.free_camera`, in
  Fahrenheit's `BtlDebugFlags`). If it is a developer camera, driving the
  game's own camera makes culling, lighting and native cursors follow.
- **UnX (Kaldaien)** as the reference for FFX's 30 Hz logic tick vs render
  rate when we need 90 Hz for the headset.

Later, if wanted:
- Sphere grid: parse grid files offline (sphere-grid viewer project) for a
  3D version; flat panel first.
- Farplane / FFXProjectEditor as cross-references for battle memory, esp.
  the CTB turn queue for a turn-order display.
- Overdrive minigames as gesture-to-input mappings (they are inputs: timing
  bar, stick rotation, button sequences), e.g. Lulu's Fury = stick rotation.

Not applicable / overstated: frustum-culling suppression (the game does not
appear to cull by frustum); Fahrenheit "lifecycle events" (it has pre/post
main loop and a warp hook; we read battle state directly); UnX as a UI/3D
render-pass separation (we mapped the UI pass ourselves).

## 2D screens in the headset: the virtual screen (stopgap that is also the final piece)

Problem: title screen, load/save menus, loading screens, FMVs and any other
frame without a perspective camera break the eye-2 pipeline (no clear ends
the overlay phase, no camera buffer, flat draws straight to the backbuffer),
so the two eyes disagree and the screen is unreadable or missing.

Plan: per frame, if no perspective camera was uploaded, treat the frame as
2D: ignore eye 2 and show eye 1's final image on an OpenXR quad layer -- a
flat panel ~2.5 m away, ~2.5 m wide (~50 deg), world-locked in front of the
recentred forward direction; Home re-places it. Identical in both eyes, so
always readable. 3D frames keep the projection layers; a frame or two of
hysteresis on the switch. Overlay gets a "force screen mode" toggle for the
edge cases (a menu over a frozen 3D background would otherwise stay in
projection mode).

This quad layer is the design's "virtual screen" for FMVs and is the natural
home of every screen where the game is a 2D program, so it is not throwaway.
Not covered: HUD/menus drawn over a 3D scene still come through the
projection layers identical in both eyes (read as "at infinity"); the
floating UI panel is a later job that needs the UI draws separated from the
scene draws.

## Camera policies per mode (overworld, cutscene) and the spectator ghost

Three policies, shared by overworld and in-engine cutscenes (they differ only
in defaults):
- Director: authored camera as is; blink (~100 ms black) on cuts; on a cut,
  re-reference so the new camera's forward is where the head looks now.
- Seat (default): authored camera *position* (+ clamped lean), head owns
  orientation; authored pitch/roll discarded, authored yaw used only as the
  reference heading reset at cuts; optional slow low-pass "follow pans"
  (default off: forced rotation is the sickness source). NOTE: the current
  build layers head rotation on the live authored rotation, so scripted
  pans rotate the world -- must change.
- Spectator: free position (stick free flight) and look; director's camera
  drawn as a ghost: camera glyph + direction vector, a faint 16:9
  viewfinder frame floating along the vector sized to the authored FOV
  (default on), optional full frustum lines. Cuts move the ghost, not you.
  Button to "sit in the director's chair" (blink, snap to authored
  position) and back.
The ghost is real 3D geometry drawn per eye with each eye's view/projection
by a small runtime-compiled line pass into the eye images before submission;
v1 ignores depth (shows through walls), later tests against the depth twins.

First person / third-person follow (overworld, per-area user choice,
remembered per area): camera at Tidus's head (actor pos + ~18 units) or a
set distance behind/above; headset orientation; input remap -- rotate the
stick vector by (head yaw - authored camera yaw) before the game sees it,
since movement is relative to the game's camera; zone cuts stop mattering;
hand back to the authored policy when a scripted event takes control.
Expected imperfections: backfaces, LOD, backdrop cards, culling behind the
authored camera, body inside the near plane.

Shared prerequisites: mode system with a reliable "player has control" /
"cutscene playing" signal, controller input (OpenXR actions) and an input
injection path into the game, the line pass. Comfortable defaults for the
community release (Seat, authored-with-head-look, blink on cuts); spectator
and first person one toggle away.

## Sharpness in the headset (next steps)

Why it went soft with the full FOV: the 3840x2160 target that covered the
game's 45x26 deg window (~85 px/deg) now covers ~100x100 deg: ~38 px/deg
horizontally (above the Quest 3's ~25) but ~22 px/deg vertically (below
it). The 16:9 target is the limit, not the GPU.

1. DONE in code: eye swapchains at the full 3840x2160 (no mod-side
   downscale to 2496x1404 before the runtime's own resample).
2. NEXT TEST: render taller. Everything we do is independent of the
   target's shape (our projection maps whatever rectangle the game renders
   onto the headset FOV), so a taller game resolution fixes it directly.
   Find the launcher's settings store (Square Enix launcher; usually a small
   config under Documents\SQUARE ENIX\FINAL FANTASY X&X-2 HD Remaster\ or
   next to FFX&X-2_LAUNCHER.exe) and try a custom value such as 3840x3600
   (~36 px/deg vertical). The monitor window becomes tall and odd; it does
   not matter in VR. If the game refuses custom values, override the
   swapchain/target size at creation from the mod instead.
   Note: the virtual-screen and preview paths assume 16:9 for the panel
   size; with a taller target, letterbox or crop the panel copy.
3. Optional: a contrast-adaptive sharpen (FSR-1 RCAS style) in the copy
   pass, mild default, slider.
4. Later: supersample 1.3-1.5x the headset density and let the runtime
   downsample, once 2 holds and the GPU has headroom.
Not DLSS: no motion vectors in the game, temporal ghosting is worse in VR,
and 8x MSAA is already the better anti-aliasing for a headset.

## First person: swimming (noted 18 Oct 2026)

When the player enters water the character goes prone and swims at the
surface, but the first-person eye stays at `player position + eye height`
with the heading frame upright, so swimming looks like walking on water:
the eye is a full standing height above the surface and the body is hidden
as if standing.

What to do about it, in order of effort:
- Detect the swimming state (the field actor's animation/motion state, or
  the player's height relative to the water plane; a stated flag in the
  actor struct is the thing to look for) and drop the eye height to a
  swimming value (a few units above the surface), with a short slew between
  the two so the transition is a dip, not a cut.
- Optionally pitch the resting view slightly down and widen the body-hide
  radius, since the prone model extends further from the actor origin.
- Underwater sections (free swimming in three axes) are a different mode
  again: the eye should follow the actor origin directly, and the game's
  own underwater distortion pass (the same family as the water-area
  history-blend pass) will need the same treatment as that pass.
- Also noted for the same sections: the frame-rate collapse after several
  underwater battles, under investigation with the perf telemetry.

## First person: the player's body, arms and hands (noted 18 Oct 2026)

Goal: the overworld from Tidus's real perspective, body visible, hands where
the controllers are. The game already animates a full skinned Tidus every
frame and we already rewrite the constant buffers that carry his bones, so
this is staged work on top of what exists.

Stage 1 - the body (the game's animation, our eyes):
- Hide only the head and hair draws instead of the whole model. Parts are
  identified by mesh (stable per model); a one-time "tick the head parts"
  list under Debug, saved against the model name (c101) like the player
  lookup.
- Eye on the head bone: read the head bone's world position from the skinned
  buffer (7 KB layout, bone palette) each frame instead of actor position +
  eye height. The eye then bobs and leans with the walk cycle; looking down
  shows the torso and legs, animated by the game's own locomotion.
- Near plane: the game's clips close geometry; the chest and shoulders may
  need a nearer plane in the projection we already write.
- Cutscenes: a head-bone eye under scripted head animation is sickening;
  hand off to spectator mode (planned anyway). Swimming and Blitzball: eye
  follows the actor, not the bone (see the swimming note).

Stage 2 - arms and hands (IK):
- Prerequisite: controller poses from OpenXR (an action set with grip/aim
  poses; a day or two; wanted for menus and pointing regardless).
- Two-bone IK per arm (shoulder-elbow-wrist) with the controller as the
  target; torso yaw follows the head; legs stay on the game's animation.
  Solve on the CPU, overwrite those bones in the skinned buffer before the
  GPU reads it (same mechanism as the camera override, further down the
  buffer).
- Skeleton map needed: arm bone indices, parents, bind pose, the game's
  bone-matrix convention. The FFX-60fps mod's SMOOTH-003 reads bones and
  motion channels per actor (Ch_SkeletonJob, Ch_StepMotion, SeqFrame): start
  from its findings.
- Polish: elbow hinting (no flipped elbows), hand poses from controller
  buttons, reach clamping so the arm never dislocates.
- Estimate: two to three weeks including a week of tuning.

Not planned: leg/foot IK (no trackers; the walk animation is better than a
guess) and finger tracking (button-driven hand poses are enough).

Order: Stage 1 first (quick, and it tells us whether a body at true scale
feels right - Tidus at ~20 units is a 1.75 m man), then controllers, then
arms.

## Painted-backdrop rooms (Besaid hut interiors; found 18 Oct 2026)

Hut interiors are not 3D rooms. The walls are the game's layered-2D
background system: 1456-byte constant buffers with a dummy camera (identity
view at (0,0,-1), fixed projection P[0]=2.5), the backbuffer size at +384,
and scale/offset transforms in 512x448 PS2-framebuffer space further down.
The props and characters in the room are ordinary 3D draws. Overriding the
dummy camera smeared the layers across the room; it is now recognised and
left alone.

Treatment today (overlay setting, "painted-backdrop rooms"): the whole frame
on the virtual screen while such draws are present (default), or 3D props in
stereo over the flat backdrop (FF7-style, head rotation moves the props but
not the painting).

Later: a 2.5D diorama. The layer transforms carry a depth (block 11/14 z),
so each painted layer could be placed as a plane at its depth in front of
the authored camera, giving parallax between layers and letting the props
sit among them. Needs the layer order and depth convention from a few rooms.

## Character culling: where the game decides (found 18 Oct 2026)

Characters are not culled by the render-command interpreter (0x615550; NOPing
its two draw-model skips changed nothing) and not by the camera FOV fields
(0x01137000 blocks; scaling them widens nothing we could see). The decision is
in `Ch_DrawMain` (0x82ED50, the actor draw walk the FFX-60fps mod also hooks):

- loop 1 skips an actor when `chr_enabled` (+2) is 0, byte +0x180 is non-zero
  or bit 0x20 of the dword at +0x198 is set (0x827700 sets that bit when the
  actor's model is released: a permanent hide, not a per-frame cull);
- loop 3 calls `Ch_VisibleTest` (0x82E220) per actor, then hides the model
  (`0x63C390(model, 0)`) when it returns non-zero, when +0x354 (the actor's
  alpha) is 0, or when byte +0x180 is non-zero; otherwise `0x63C390(model, 1)`
  and the draw submit `0x63BB30(model, actor, scale...)`.

`Ch_VisibleTest(actor)` clears bits 0x08/0x20 of +0x180, reads the camera
position (`0x7BD470(1, &pos)`), stores the actor's distance to it at +0x178
and returns 1 (hidden) when +0x180 is already set, when the model is not
loaded (bit 0x800 of +0x194 and `0x63B230(model)` == 0 -> bit 0x08), or when
the distance exceeds the actor's own cull distance at +0x174 (-> bit 0x20).
+0x174 is copied from the actor template (0x828758) and set by a script
setter (0x82B171). `0x641CC0()` non-zero disables the distance test entirely
(a battle/menu state). One map (id 0x24F9) tests a scaled distance instead.

The distance patches alone changed nothing: the actor table (Debug, with the
new state columns) showed every hidden NPC with hide byte 0x08, not 0x20. Bit
0x08 is the other branch of `Ch_VisibleTest`: `0x63B230(model)` walks the
model's parts and asks the renderer (`0x6603C0`, this = `0xCCB9D8`) whether
the part's scene node (part+0x1C, linked/unlinked by `0x66D910`) intersects
the game camera's frustum (`0x663CC0(node, renderer->[0xDC]+0x74)` ->
`0x663A30` bounding test, after the node's transform). No part in view ->
bit 0x08 -> hidden. The same `0x663CC0` is what the scene renderer runs on its
two node lists (0x652D35, 0x652EBC) before submitting (`0x5B9A70`): that is
the geometry cull. `0x6603C0` skips the test when renderer+0x10D8C is set (a
debug camera mode that also swaps the camera, so not usable as a switch).

Patches (verified bytes, restorable; main window):
- "keep characters drawn": 0x82E32A `jne`->`jmp` and 0x82E378 `je`->nop (the
  distance cull), 0x6603D1 `jne`->`jmp` (0x6603C0 always answers "visible").
- "keep all geometry drawn": 0x663CC0 entry -> `mov al,1; ret 4`, so every
  node in the scene lists is submitted. Costs GPU time (everything behind the
  authored camera is drawn too); the PC has the budget, and the headset needs
  it since the eyes look where the authored camera does not.

Second pass (19 Oct 2026, static, after the first set changed nothing visible):
the engine (Phyre, the renderer object at 0xCCB9D8) runs its bounding test in
more places than the two node lists. The "frustum" is a 4x4 matrix at
camera+0x74 (the camera lives at renderer+0xDC; a second camera at +0x654),
and the test proper is `0x663A30(node, matrix)` (thiscall, ret 4), which
transforms the node's bounds (node+0xC transform, 0x4CA360) and answers in/out.
Every caller found:
- 0x663CC0 (the wrapper patched above): node lists 1 and 2 (0x652D35,
  0x652EBC) and the model parts via 0x6603C0;
- 0x6505D6: a third node list (renderer+0x3874, count +0x386C) tested against
  the second camera and submitted with 0x5B9A90 - probably the dynamic /
  skinned objects, which would explain why characters stayed hidden with the
  first set on;
- 0x6C8236 / 0x6C8294 / 0x6C83B0: an "anything visible?" aggregate over a
  node's parts (flags on the stack), run before the expensive work;
- 0x9F3275: another scene traversal (lights or particles).
No other reader of the frustum matrix exists in .text (the two remaining
users, 0x6527A1 and 0x65E6ED, only copy it with 0x453340 for the sites above).
So the one switch that covers everything is 0x663A30 itself: entry ->
`mov al,1; ret 4`. "keep all geometry drawn" now applies 0x663CC0 and 0x663A30
as a set. What this cannot reach: anything the game never registers in the
scene at all (the field system at 0x1876E00 decides what it hands to the
engine; nothing camera-dependent was found in its draw entry 0x90BF10, which
builds light / fog / material packets, and the actor walk has no other hide
path than +0x180 / +0x198).

The automatic actor log (module log, every 10 s: live / drawn counts and the
state of every actor without a draw) and the GPU time in the `perf:` line are
there to settle what remains with evidence rather than more reading.

## First person: showing the body, hiding the head (19 Oct 2026)

The player's parts are the skinned draws whose world translation (+640 of
the 1872 B+ buffer) sits on the actor position; nothing else is that close
(an NPC in conversation is 1+ units away), so the hide radius is 0.75 now,
not 3 - the old value also caught NPCs at talking distance. Within that match
the draws come in the game's part order, stable from frame to frame, so a
part is addressed by its index among the player's draws into the scene
target (shadow / reflection passes are not counted). Three modes: hide the
whole body (every pass, as before), show the body and hide the parts listed
as the head, or show everything. "show only part #" (works in third person
too) is the finder: step through the indices, note which ones are the head,
hair and face, and type them into the head list. The list is saved per ini,
not per model yet; Tidus is the only first-person model so far. Tidus (c101):
parts 0, 1, 2, 3 are the hair, 9 the face/head - the default is `0,1,2,3,9`
with "show my body, hide the head parts" on.

Later: the arms follow the Quest controllers (see "First person: the
player's body, arms and hands").

### Eyes on the head bone (19 Oct 2026)

With the body visible, an eye fixed at actor origin + 18 units shows the
neck and shoulders sliding out under the camera whenever the run animation
tips the torso forward. The eye is now anchored to the head bone - the
highest of the actor's 26 sub-positions (`chr_optpos`), passed each frame in
render space - plus an offset (default 2 units up), smoothed with a per-frame
blend (default 0.35) so the run cycle's bob does not reach the headset. This
also makes the swimming pose right by construction (the eye follows the head
down to the surface). The old fixed eye height remains as the fallback and as
an option.

Second pass: the head sub-position is the neck joint (back of the skull), so
the eye also goes a head-length forward of it (default 1.5 units), along the
lean - the horizontal part of the head offset from the actor origin - or the
heading when upright. The offset, not the absolute position, is what gets
smoothed, so the eye never trails a sprint. The movement direction was tried
first and shifted the eye sideways: not worth chasing why.

Swimming, resolved the easy way (19 Oct 2026, later): with the body hidden
the forward lean no longer matters, so the eye takes only the head bone's
HEIGHT (smoothed offset from the actor origin + the up offset) and keeps the
actor origin horizontally. Prone in the water puts the eye at the surface;
crouches and kneels follow too; nothing is ever in front of the camera.
"eye height" in the overlay: fixed / head bone height (default) / full head
bone (the lean experiment, kept for the body view later).

Outcome of the body view (19 Oct 2026): parked. With the body shown and the eye on the head
bone, the run lean still left part of the neck in view at some angles, and
the tuning was not converging. Default is back to hiding the whole body with
the fixed eye height; the part map (hair 0-3, face 9), the head-bone anchor
and the offsets stay in the overlay for when the controller-driven arms come
(generic arms and hands drawn by the mod, not the game's model - see "First
person: the player's body, arms and hands").

## Quest controllers (phase 1, 19 Oct 2026)

The game reads pads through one import, `XInputGetState`, which the mod
already owns (the left-stick remap). So the controllers come in through the
OpenXR action system (`ffxvr_xr.cpp`: one action set, both hand subaction
paths, bindings for `oculus/touch_controller` with `khr/simple_controller` as
the fallback, synced right after `xrWaitFrame` at the frame's display time)
and are turned into the XInput state the game asks for, merged with a
physical pad when one is present (buttons OR, the stick with more deflection
wins, triggers max). The right controller's menu button is the system button
and never reaches applications, so Start is the left menu button.

Default map: A/B/X/Y as labelled, left stick = movement (the first-person
remap still applies), right stick = smooth turn (and whatever the game does
with it), triggers = LT/RT, grips = LB/RB, left menu = Start, left stick
click = Select, right stick click = right thumb. Grip and aim poses and
haptics are read/available for phase 2 (world grab, Demeo-style scale and
rotate, mode chords - `g_vrpad_suppress` already blanks the game's view of
the pad while a gesture is in progress) and phase 3 (hands and arms).

Virtual Desktop's own gamepad emulation must stay off, or the sticks arrive
twice. The overlay shows the live state of every action and the bound
profile per hand; "buzz" buttons test haptics.

### Phase 2: chords and the world grab (19 Oct 2026)

- Both stick clicks (press edge): first person on / off, with the same scale
  save / restore as the End key. Haptic tick on both hands.
- Seat view, gestures on: the grips are the grab and are not forwarded as
  LB/RB (first person keeps them). One grip pans: the world follows the hand,
  i.e. the seat offset moves by minus the hand's motion in the heading frame,
  scaled to units. Two grips: pan with the midpoint, zoom with the distance
  (hands apart = fewer units per metre = bigger world; floor 5, ceiling
  2000), rotate with the angle between the hands about the midpoint (yaw
  only; hands clockwise from above = world clockwise = seat yaw grows). Zoom
  and rotate solve for the offset that keeps the world point under the
  midpoint fixed, head position included when "head position (lean)" is on.
- The results are `g_gest_move` (added to the manual numpad offset),
  `g_gest_scale` (multiplies the overlay's world scale) and the seat yaw
  itself; numpad 5 / "reset grab" clear the first two. While a grip is held
  the game sees no pad input at all (`g_vrpad_suppress`).
- Poses are in the runtime's reference space and go through the same
  recentre transform as the head (`hand_to_ref`).

Revisions (same day): the chord is right grip + right stick click (both
stick clicks is Virtual Desktop's performance overlay). Two hands also tilt:
the mean pitch of the two grip poses' forward axes, wrists rolling together,
changes the seat pitch about the midpoint (far edge of the "tray" up = the
seat looks down at it; "tilt gain" slider, negative inverts), solved together
with the yaw so the midpoint stays put. First person ignores the gesture
scale multiplier: it is always true scale; the diorama scale comes back
with the seat view.

## UI panel: double resolve on the boat (19 Oct 2026)

On the boat to Kilika the subtitles and dialogue choices came back in double
vision. The census showed why: the scene target is resolved once after the
depth clear that opens the UI pass and before any UI draw, then again after
the UI draws (81 of them). The UI state machine took the first resolve as
the end of the pass, so the real UI draws were re-issued as scene. Now a
resolve ends the pass only when UI draws have happened; an earlier one just
becomes the newer base (the pass reopens at the next depth clear), and once
the pass is done, later resolves leave it done. The `ui:` diag line (every
10 s) reports frames with / without a captured pass and the resolve / depth
clear counts the detector saw, so this class of problem shows up without a
census.

## The diorama is the follow view (19 Oct 2026)

The authored-camera seat latched its heading across cuts but kept the
camera's position, so every cut teleported the viewer; and the authored
cameras often start nearly side-on. The diorama is now the follow mode
(`fp_mode 2`): anchored to the player's position (smoothed, 0.5 per frame,
since the actor table moves at 30 Hz under 60 Hz rendering), heading latched
and turned by the stick (which orbits the seat around the player, as the
offset lives in the heading frame), pitch from the tilt gesture (starts 20
degrees down), offset from the grab gestures (starts behind and above:
follow distance / height). Camera cuts are ignored entirely, as in first
person. The chord and End toggle first person <-> diorama; the authored seat
remains as the third radio button. The grab math is shared: yaw / pitch /
base offset are taken from whichever mode is active.

## Pending: Re-Remastered texture pack (noted 19 Oct 2026)

Nexus mod 244, FFX **Medium** (2.5x) archive chosen; downloading on a free
account (hours). When it lands: `tools\install-texture-pack.ps1 -Archive
<the .7z> -Compress`, launch, check `Fahrenheit.Runtime.FhFileLoaderModule.log`
for "replaces file" lines and the `perf:` vram figure. No EFL DLL, no UPX.

### Battle: the diorama centres on the fight (19 Oct 2026)

In battle the party leader may be swapped out, so the diorama anchors to the
centre of all combatants (the battle's player and monster lists, actors with
a live position; monsters only while visible), passed from the managed side
as an anchor override and smoothed slowly (0.05 per frame) so the table is
near-static. Battle start and end snap the anchor. First person is left as it
is (watching from where Tidus stands, even when he is out of the fight, is
part of the fun).

### Diorama floor (19 Oct 2026)

No terrain collision is available (the walkmesh pointers in Fahrenheit's
Globals.Map are still commented out), so the floor is relative to the
anchor: the final eye position is clamped to at least N units (default 8)
above the player's feet / the fight's centre, except while a grip is held, so
a deliberate drag below ground works and the clamp returns on release. 0
turns it off. Hills rising behind the player can still put the seat inside
terrain; that needs the walkmesh.

## Third person (fp_mode 3, 19 Oct 2026)

A third view between first person and the diorama: life-size (k_true_scale,
like first person), the camera 24 units behind and 22 above the player's
feet, pitched down 8 degrees, the body drawn. The chord cycles first person
-> third person -> diorama; End does the same.

- Anchor and offset are the diorama's (`g_fp_anchor`, `g_gest_move` in the
  heading frame, the floor clamp), so the grips work the same way: one hand
  pans, two hands pan / rotate / tilt. The two-hand zoom differs: in third
  person it scales the offset vector (closer / further, 4..400 units) and
  leaves the world scale alone, so the player stays life-size.
- The heading is fixed, as in first person: you turn by turning (head,
  body, or the right stick), and the left stick is relative to where you
  look. That is what makes it feel like first person; the first cut had the
  camera swing round behind the run and the control was awful - the remap
  against the live view chased the swinging camera. The swing survives as
  an option (`tp_follow_rate` > 0, default 0): `g_fp_yaw` eases toward the
  direction of travel, capped, weighted by (1 + cos d) / 2, and the stick
  remap is latched while the stick is pushed so it cannot chase itself.
- Reset chord: left grip + left stick click puts any mode back to its
  default (`view_reset`); in third person it also snaps the heading behind
  the way he last went. The seat and diorama get the same chord.
- The battle centroid override applies to the diorama only; third person
  stays on the player actor.
- Settings: `tp_distance`, `tp_height`, `tp_pitch` (degrees), `tp_follow_rate`
  (`ffxvr_tp_set`). Still pending: walkmesh collision for the camera.

## The save screen, and ImGui in the headset (19 Oct 2026)

The save screen was black in the headset because it is not the game's: the
Fahrenheit runtime replaces it with its own ImGui window (FhSaveUiX), and
Fahrenheit renders all ImGui - its menus and our overlay - into the
backbuffer after calling the modules' render_imgui, i.e. after our frame
capture, inside the same Present. The game draws nothing meanwhile, so the
virtual screen showed its black backbuffer; the menu only ever existed on
the monitor (the captures: the final backbuffer image is all zeros).

Fix: the draws into the backbuffer during the "overlay phase" (between our
hook and the game's first clear) are re-issued into an ImGui-layer texture,
the same way the game's UI pass is captured (alpha-blended, no depth), and
submitted as a third composition layer - a head-locked quad at 95% of the UI
panel's distance, shown with the virtual screen too. One frame of latency.
Side effect, welcome: the mod's own overlay is now visible in the headset
(F8 hides it; F7 is Fahrenheit's own mod-settings menu, which takes input from the game while open). Checkbox "Fahrenheit menus (save screen) and this overlay in
the headset", ini `imgui_layer`.

Follow-ups (19 Oct 2026): with the overlay hidden a sliver of ImGui still
reached the headset (Fahrenheit draws small things of its own), so the layer
is shown only when the overlay window is open, the save screen is open
(state word at 0x8CB994: 0 closed / 1 opening / 2 open) or at least 12 draws
were made (a real menu); the `ui:` line logs the layer's draw count. Pending:
the opening cutscene's credits were in double vision - the only place the UI
pass is missed since the boat fix; needs a census at the intro.

## Eye 2 blank: twin reuse ignored the sample count (19 Oct 2026)

After the ImGui-layer build eye 2 showed only its clear colour. The census
showed the mirrored clear of the 4x scene target landing on a non-MSAA twin:
the twin lookup reused an existing twin on width / height / format alone,
so when the game freed a plain 3840x2160 texture and allocated the MSAA
scene target at the same address, the 1-sample twin was kept and every eye-2
draw (colour twin 1 sample, depth twin 4) was refused by D3D11. Latent since
the twins were written; the extra ImGui-layer texture changed the allocation
pattern enough to trigger it. The reuse check now compares sample count,
mip count and bind flags too.

## Pending: arms and hands on the controllers (phase 3 plan, 19 Oct 2026)

The game splits the model into parts by material and PS2 bone budget, so an
arm is spread across several parts and a part can hold torso triangles too -
hiding parts never isolates an arm (seen with the part finder). Drive BONES,
not parts: replace the forearm and hand matrices in every part's bone palette
with ones built from the controller grip pose; every arm triangle in every
part follows the hand, the rest stays as animated. Steps:
1. Palette layout in the 1872+ byte buffer (bones from about +960; size and
   row order), and the per-part slot -> skeleton bone mapping (read from the
   part data, or inferred each frame by matching slot translations to the
   actor's sub-positions in `chr_optpos`, which include hands and elbows).
   Start with a capture of Tidus's buffers in a known pose.
2. Arm chain: hand from the grip pose (already in game space), elbow from
   the shoulder sub-position + arm length, forearm aimed at the hand (two-bone
   IK, floating-forearm style).
3. Write the matrices into the buffers we already rewrite per frame.
Plan B if the palette stays opaque: the ripped PS2 Tidus OBJ (user's local
folder; 4,877 verts, T-pose, parts by material; cut forearm+hand by vertex x
beyond the elbow, ~500 tris per arm, PS2 textures) drawn by our own shader at
the grip poses, scaled to the 18-unit Tidus. Square Enix assets: never in the
repo; loaded from a user folder.

## Tidus's skeleton: bone palettes decoded (20 Oct 2026)

Capture: `ffxvr_bones_capture` (one frame, every skinned buffer on the
player, raw) and `ffxvr_bones_seq` (one record's palette every frame for N
frames). Findings:

- Every part's 7 KB buffer carries a **bone palette at +1344**: 64-byte
  row-major affine matrices, **world (render) space**, M = W_joint *
  Bind^-1, one per slot. The player uploads 26 records a frame (13 parts x 2
  passes) with 26 / 53 / 9 / 5 / 3 slots; ~78 distinct bones. The same bone
  is byte-identical in every part that uses it, so a slot picked in one part
  is found in the others by value (`g_grp`, rebuilt each frame end).
- Hierarchy and pivots from motion: for a child c with parent p the joint
  is a fixed point in both bind spaces, so (R_c - R_p) B = t_p - t_c over
  the 240 frames of a run gives B (the joint in bind space) with ~0 residual
  for the true parent. Bind space is model space: x = Tidus's left, y DOWN,
  18 units tall. Results for the 53-slot reference part (record 10):

  | joint | bind position | slots |
  |---|---|---|
  | L shoulder | (1.90, -14.70, -0.01) | 20, 21 (+23, 24 at the same pivot: pauldron?) |
  | L elbow | (3.82, -12.79, -0.04) | 25, 26, 44 |
  | L wrist | (5.57, -11.03, -0.07) | 49 hand, 45/46 wrist twist, fingers 47 48 50 51, thumb 52 (5.76, -10.53, -0.46) |
  | knuckles (middle) | (6.31, -10.65, 0.09) | |
  | R side | mirrored in x | upper 41 42 43 (+28), elbow 38 39 40, hand 33 35, fingers 31 32 34 37, thumb 36 |
  | head / neck | (0, -15.81, 0.05) | 0, 1, 27, 30 (hair bones live in the 26-slot head parts) |
  | hip | (0, -11.25) | 4, 15 | legs: (+-0.85, -9.73) knee-ish 2 3 16 17, ankles (+-0.85, -5.40), feet (+-0.85, -0.90) |

  Upper arm 2.71 units, forearm 2.48.
- The user's `bone_labels.txt` (lift-one-bone finder) agrees with the fit.

### VR hands: state of play (tabled 21 Oct 2026)

Everything below is in the code, off by default and hidden from the overlay
(`hands_dev=1` in ffxvr.ini shows the section again). What works:

- Hands (with animated fingers) placed at the controller grips in first
  person: rigid world-space delta per hand group about the wrist, no bind
  rotations needed. Calibration: a 10 s countdown with both arms straight
  out, palms down, captures grip -> hand rotation per controller
  (`g_hands_Rc`, ini `hands_cal`); yaw/pitch/roll sliders trim; out/up/back
  offsets move the wrist off the grip. Saved, reloaded.
- Which bind side is which arm is MEASURED (left = up x direction of travel
  vs the shoulder-to-shoulder vector) and locked at calibration: +x is
  Tidus's RIGHT (ini `hands_px_left=0`). The flat-screen labels had it the
  other way round. A "flip" button exists.
- Arms hang off virtual shoulders beside/below the eyes that turn with the
  head (`g_hb_eye`, `g_hb_right`, `g_hb_fwd`), not off Tidus's animated
  shoulders: he turns his body to run, so arms rooted on him crossed the
  chest when backing up. Two-bone IK with the reach clamped, arms stretch
  along their bones up to `hands_stretch` (1.6) to match real arm length.
- Draw levels (`hands_show`): whole body / arms / forearms + hands / hands
  only. Parts without hand bones are not drawn at all in the last two.

What does not work yet - the reason it is tabled:

- Seams. The game blends bone matrices LINEARLY in world space (zeroing a
  bone sends its vertices to the world origin, proven by the white ray), so a
  vertex on a seam between a drawn bone and a collapsed one lands between
  the two destinations. Any arrangement with two destinations in one draw
  leaves a "string" at the seam between them. Tried, in order: collapse to
  the neck (strings wrist -> neck); forearm to a dot at the wrist (strings
  from a shoulder bone that the proximity matcher mislabelled as forearm);
  nearest-wrist rule (chest bones map the bind wrist close to the real
  wrist with the arms at rest -> a chest sheet between the hands); all-zero
  matrices (would be exact with homogeneous blending; the shader ignores w);
  and finally the SPLIT: each hand-bearing part drawn once per hand per eye
  with one collapse point at that hand's wrist (eye-2 replay machinery,
  `EyeImage::eye1b/eye2b`, `split`, 3 extra passes from `ffxvr_on_draw`,
  `ffxvr_eye2_begin` pass 0 = right hand into eye 1). Standing still that is
  clean.
- Still wrong with the split: (1) strings while MOVING - the deltas are
  built from LAST frame's reference palette transported by the actor matrix
  (`g_ref_pal`, X = W * Wref^-1), so the pivot W0 lags the arm animation by
  a frame; the hand lands at P + (that error) while the collapse point is P
  exactly, and the seam shows the difference. Fix: take W0 (and the hand
  frame) from the CURRENT record's own hand-bone matrix - every
  hand-bearing record has it - instead of the transported reference; keep
  the reference only for identifying slots. (2) One hand renders in one eye
  only with the split: pass 2 (right variant into eye 2) is not landing;
  check `g_eye2_slots[sl] == g_eye2_cur` in `ffxvr_eye2_begin` (the ring
  lookup may pick a different EyeImage for the same cb) and that
  `ffxvr_eye2_end` between passes restores eye1 before pass 2.
- Bone identity in non-reference records is by proximity to the transported
  reference bones (`group_of`), which mislabels shoulder bones sharing a
  pivot with the upper arm. Byte-equality within the frame is exact but the
  reference (53-slot) part uploads 10th; records before it would need last
  frame's equality map keyed by record order (which looked stable in every
  dump: 26 records, same order).
- Plan B remains: draw our own hand mesh (the PS2 OBJ's hands, cut at the
  wrist) with our own shader at the grip poses; no seams by construction,
  but new render code and the textures to load.

## Overnight dig (21 Oct 2026): battle follow, cursors, hands

- **ini "resets" explained**: the mod rewrites `ffxvr.ini` whenever any
  setting changes while the game runs, from its in-memory values, so an
  edit made on disk while the game is up is overwritten (that was every
  "overlay_at_start keeps going back to 1"). The mod now re-reads the file
  when its mtime changes (`settings_reload_if_edited`, once a second);
  launch-only keys update their stored values without re-applying.
- **Battle party mechanics** (from the per-second `window:` log): the party
  list has all 7 characters; reserves not in the party sit at (67, 0, -1282).
  Front members stand on the area's `party_pos` slots; a swap sends the
  outgoing character to a `party_run_pos` ("bench", ~85 units from the slot
  in the Besaid test) and the incoming one runs from the bench to the slot.
  `BtlWindow.cur_chr_id` is whose turn it is; `type == Change` while the
  swap menu is open. The follow logic keys on: slot = the party_pos Tidus is
  found standing on (within 6 units), benched = within 20 units of any
  party_run_pos or stat_visible == 0 for 10+ frames, successor = the visible
  party member nearest the slot. The first version used a 120-unit
  "walked off" threshold, larger than the bench distance - it never
  switched.
- **Cursors**: no named routine in the 60 fps mod's signatures; the sphere
  grid has `omd/CURSOR.omd` (a model) but the battle cursor is a UI-pass
  sprite. The frame census now lists each UI draw with its textures and the
  head of its constant buffer; a with/without-cursor pair of censuses is the
  next step to pick the draw(s) to drop.
- **Hands**: see exe-analysis section 13. Two fixes from it: the palette
  offset is found per record (three variants), and the seam is handled by
  the NaN cut (`g_hands_nan_cut`, default on) instead of the split draw
  (`g_hands_split`, off): the adjoining segment's bones get NaN matrices,
  the seam triangles vanish, both hands draw in one pass per eye. The hand
  pivot is also taken from each record's own wrist bone (ref slots 45/46/49
  and 33/35) rather than last frame's transported reference, which removes
  the string that appeared while moving. Untested in the headset at the time
  of writing; still hidden behind `hands_dev=1`.

### Next session: what to test (written 21 Oct 2026, nothing below is verified yet)

Rebuild first (`tools\build-mod.ps1`). Then, in order:

1. **Launch defaults**: the game should come up in the headset, first person,
   no overlay. ini: `headset_autostart=1 start_view=1 overlay_at_start=0`.
   If any of it is off, the module log's `settings: loaded ...` line says what
   was read; edit the ini only with the game closed, or through the overlay
   (the mod now also picks up on-disk edits within a second).
2. **Battle follow**: first person, get into a battle, swap Tidus out. The eyes
   should move to the replacement once Tidus reaches the bench. The module
   log has `battle follow: <name> on slot N of area A` and `battle follow:
   now <name>`; the overlay's "now: c1xx" shows the current one. If it never
   switches, send the log: the per-second `window:` lines now include each
   party member's `vis`/`hide` bytes and positions.
3. **Battles start in** (overlay, first-person tree): pick diorama or third
   person; the view should switch at battle start and return after.
4. **Cursors**: in battle with the target cursor visible, press the frame
   census; again with no cursor. `ffxvr_frame.log` then lists every UI draw
   (`ui draw N ... s0:<tex> WxH | cb0 <size>B: <floats>`); the lines that
   exist only with the cursor are the ones to drop.
5. **Hands** (optional, `hands_dev=1`): hands only, NaN cut on (default),
   split off. Expect clean hand ends with no strings, both eyes, no change
   while moving. If a hand or finger vanishes entirely, the NaN hit a padding
   index: untick "NaN cut at the seam" and report which part vanished.

## Blitzball and chocobo rides: what to expect, what to capture (21 Oct 2026, documentation only)

### Detection
- Blitzball: the map name at gva 0x23CBC60 starts with `bltz` (exe-analysis
  section 14); `Globals.event_id` is also set per event. A mode flag from
  that lets the mod pick a view policy automatically.
- Chocobo: no static signal found; the first ride should be logged (actor
  table, `actors:` lines, a frame census) to see whether the player actor
  stays `c101`.

### Blitzball, expected VR problems
- It is a turn-based sphere-pool game with a heavily cut authored camera,
  an underwater look (the swimming eye-height rule already exists for
  `Swim` animation sets) and a thick 2D UI (command windows, timers, the
  mini-map). First person on "the player" makes little sense: the
  controlled athlete changes every possession.
- Plan: on `bltz` maps default to the diorama anchored to the fight centroid
  machinery (`g_anchor_ovr`) fed with the centroid of the live athlete
  actors (or the ball, if it is an actor), ignore camera cuts as the diorama
  already does, and keep the UI panel. Third person on the possessing
  athlete is a later option (needs the "who has the ball" state, unknown).
- Risk: the stadium is the one place where the scene-culling patches might
  show something odd (the pool is a closed volume); check with cull_scene on
  and off.

### Chocobo rides, expected VR problems
- First person: if Tidus stays `c101`, the head-bone eye height works
  unchanged (he sits higher; `fp_head_anchor=1` takes the bone's height), and
  the controls are the existing stick remap. If the pair becomes one actor
  with another name, `fp_actor` has to follow it (a "controlled actor"
  rule - fall back to actor 0 - is already the code's last resort).
- The body hide uses a radius around the player actor's position; a chocobo
  actor glued to the rider would share that position and be hidden too
  (first person without a chocobo under you), or, with the body shown,
  the rider's head collapse would not touch the bird. Expect to need a
  per-actor hide (by model handle / actor index) rather than by radius.
- Speed: the hide radius widens with the actor's step (`1.5 * step`); the
  chocobo runs ~2x, so nearby NPCs may vanish again at talking distance
  while mounted - the radius term may need a cap. Camera cuts along the
  Mi'ihen road are frequent; first person ignores them already.
- The chocobo mini-games (Calm Lands) run on their own timers and scripted
  cameras: treat like cutscenes (seat view) unless they turn out to be
  ordinary field control.

### First captures to take there
1. `log this table` in the actor table while mounted / mid-match.
2. A frame census (structure + UI draws) once each.
3. The module log's `actors:` lines for 30 s of riding (which actor the
   player is, hide bytes while NPCs pass).

## Battle cleanup plan (21 Oct 2026)

Done: follow by field slot (Tidus, else the lowest party index), Tidus takes
the eyes when he joins, left grip + right stick click / Insert cycles the
field, "battles start in" view switch, diorama on the fight centroid, floor.

To do, by layer:
- Eyes: face the enemies at battle start and on every eye switch (slot ->
  monster centroid); a blink on eye jumps (`cut_blink` path); on a summon
  the eyes go to YUNA (she stays on the field to command the Aeon while the
  others walk off) - implemented on the assumption that the Aeon is a
  visible party entry with chr_id 8+ (logged as `aeon out:` when first seen;
  capture one summon's `window:` lines to confirm the index);
  check big-boss diorama (centroid / floor); note how knockback feels.
- 2D layer: hide the game's target / turn cursors (capture: census with and
  without a cursor); damage numbers, Miss, status text and enemy readouts are
  placed by the game's projection and will float off their targets too -
  decide hide vs redraw at actor positions through the marker pipeline
  (capture: census with a damage number up); confirm the command window, CTB
  bar and HP boxes are readable at the panel distance; try every overdrive
  input with the controllers (Lulu's stick spin is the risk).
- Transitions: encounter shatter, results screen, game over / retry - how
  each looks in the headset (capture: a census at the shatter if possible,
  else a description).
- Interactions: marker labels naming the targeted enemy (later); escape and
  menu mapping on the controllers.

