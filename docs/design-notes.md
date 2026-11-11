
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
since the actor table moves at 30 Hz under 60 Hz rendering; see build 4 below:
it ran per camera upload, effectively a snap, and is now a time constant,
`anchor_smooth_ms`, 0 by default, 24 ms to try), heading latched
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
as an anchor override and smoothed slowly (0.05 per frame; since build 4 the
time constant `battle_anchor_smooth_ms`, 0 by default, 325 ms to try) so the table is
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

Done: follow from the game's field roster (Tidus, else the slot's occupant,
else the lowest id), Tidus takes the eyes when he joins, summon -> summoner's
eyes, left grip + right stick click / Insert cycles the field, "battles start
in" view switch, diorama on the fight centroid, floor.

To do, by layer:
- Eyes: face the enemies at battle start and on every eye switch (slot ->
  monster centroid); a blink on eye jumps (`cut_blink` path) [done 5 Nov: "Eye jumps" at the end]; on a summon
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

### Cursor census (21 Oct 2026)

Three frame censuses from one battle (`ffxvr_frame.log`, markers on). The
UI pass draws from the same handful of atlases every frame (four 2048x4096
glyph/widget sheets, a 4096x4096 and an 8192x4096) - 64 draws in a quiet
command-window frame. The differences:

- one frame had a single extra draw, first in the UI pass, from a
  **1024x1024** texture (0x21855824);
- one frame had a block of ten draws from that same 1024x1024 texture,
  each paired with a textureless draw, plus one draw of a 1632x1088 texture,
  and ten more glyph draws - the target-selection moment, by the look of it
  (a cursor per selectable target, or the target list).

So the working guess is that the cursor sprite lives on the 1024x1024
atlas. The census does not carry sprite positions (the VS constant buffer
head is zeros; the transform must be in the vertices), so the test is the
experiment switch under Battle in the overlay, `ui_hide`: 0 off, 3 hide
1024x1024 draws in battle only, 1 always, 2 also the textureless draws.
"hidden last frame" counts what it removed.

Result: the 1024x1024 atlas is the CTB bar and the menu backgrounds, not
the cursor. The cursor is one of the **textureless** draws (hiding those
removed it), and a quiet frame has three textureless draws of its own, so
"textureless" alone is too coarse too. The census now writes a second line
per UI draw: the draw call (indexed or not, index count), VS / PS / blend
state pointers, topology, vertex stride, and the first six vertices as the
vertex buffer holds them (x y z w: screen positions for the UI). Geometry
buffers are copied at Unmap while a census is armed (`ffxvr_frame_arm`
sets `g_vb_capture`; cleared after the dump). The cursor quad should stand
out by its index count or its vertex positions (over the target, moving
between censuses); then the hide rule is "textureless + that signature".
`ui_hide=4` hides only the textureless draws in battle, to see what the
other three are.

Second census (21 Oct, 116 UI draws, target selection up; crashed the game
afterwards - `frame_dump` runs under `g_mutex` and the new cleanup locked it
again; fixed). Vertices were "(not captured)": the UI's position stream
(stride 12) is not written through Map/Unmap of a buffer under 8 MB, so
capture now also covers UpdateSubresource and buffers up to 64 MB, and the
dump header counts what it caught. The shader pairs already say a lot:
every textureless draw uses VS 02AC4278 / PS 215166F8 (the sprite draws use
0CBB3A38 / 2158C7B8, the 1024x1024 and other icon draws 214D3838 /
21533AF8). The three everyday textureless draws are **18-index** (three
quads each: the window backgrounds). The target-selection block adds
textureless draws of 6, 12, 66 and 54 indices, interleaved with 1024x1024
sprites. So `ui_hide=5` = textureless draws that are not 18-index, in
battle; if that removes only the cursor, that is the rule (pending the
vertex positions to make it exact).

### The settings kept resetting: it was the build script (21 Oct 2026)

Every "my ini reset" so far (autostart off, overlay back, hand calibration
gone) lined up with a rebuild: `tools/build-mod.ps1` deployed with
`Remove-Item $dest -Recurse`, wiping the whole mod folder - `ffxvr.ini`,
`bone_labels.txt`, captures and logs included - and the game then started
with defaults ("settings: no ffxvr.ini yet") and wrote them back on the
first change. The earlier theories (File.Exists on a sed-edited file, the
game clobbering the ini) were this. The deploy now deletes only build
output (`*.ini`, `*.bak`, `*.log`, `*.txt`, `*.bin` and the manifest stay).

### Cursor vertices (21 Oct 2026)

With geometry capture working (26 Map/Unmap copies, 6 buffers), the UI's
coordinate space is a virtual screen with the origin at the centre, 1920 x
1080 (x -960..960, y -540..540 down); the game's layout only uses about
-815..815 of it. The textureless draws in a command-window
frame with target selection up:

| draw | n | where | what |
|---|---|---|---|
| 1, 23, 46 | 18 | x 290..814, three 12-high strips at y 328, 380, 432 | party status rows (bottom right) |
| 65 | 6 | a ~20-unit triangle near (-284, 60), wobbling between censuses | a pointer: the command / target cursor |
| 69..81 | 6-12 | x 677..726, bars 48 apart, y -58..-346 | CTB list bars (top right) |
| 83, 84 | 66, 54 | first vertices all at (-376000, -260200) | projected through the game's camera: off-screen in our frame; more vertices now logged |

The census prints up to 24 distinct vertices of every textureless draw now,
and marks draws the hide rule removed (`HIDDEN by ui_hide`), so the next
capture with `ui_hide=5` on shows exactly which draws go.

Third census, `ui_hide=5` on: it hid draws 65, 69-84 (the triangle, the
CTB bars, the two far-away draws) and the cursor stayed - so the cursor is
not textureless at all. Diffing the two full captures (different targets)
finds exactly one sprite that jumped: a single quad of **68x45 units from
the 1024x1024 atlas** (texture 0x218294E4), at (-588..-520, -9..36) in one
and (-157..-89, -146..-101) in the other. The same atlas and nearly the same
quad size as the CTB portraits (70x46, at x 692..802), which is why
"hide 1024x1024" takes the CTB with it. The 1632x1088 texture is a 34x31
icon at the top right; the far-away textureless pair (66 / 48-54 indices)
is still unexplained (vertices now logged in full).

Rule in the build (`hide_cursor`, default on, battle only): a 1024x1024
single-quad draw whose rectangle is 55-82 wide, 35-56 high and ends left of
x 660 (the CTB column starts at 677). The sprite's rectangle comes from the
slot-0 vertex stream. First attempt copied the UI streams at every Unmap:
they turned out to be large dynamic ring buffers in write-combined memory,
and reading them froze battles. Now only the last Map pointer per UI stream
is remembered and the four vertices are read through an SEH-guarded read,
within the frame the game wrote them (the upload ring is not recycled until
the GPU has consumed it); the census still copies whole buffers, but only
in its one armed frame. The UI position stream is a 12 MB ring. First run
of the rule hid nothing: the game maps the ring a frame ahead of the draws,
so "mapped this frame" never matched; the lookup now accepts a Map from the
last two frames, and the overlay shows "candidates / rect unknown" counts.

Still open which of two things is the cursor the user sees: the 68x45 quad,
or the pair of 42/66-index draws (a textureless pass then a textured one
over the same geometry) whose first vertices sit at (-376000, -260200).
The buggy build that hid "every textured draw that is not one quad" did
hide the cursor, which points at the pair; the census now logs full vertex
lists for hidden draws and for every 1024x1024 draw, and the whole cb0
(60 floats) of sprite draws, since the UV stream is zeros and the atlas
rect must come from the constant buffer.

If the cursor over an enemy at the far right survives, the next
refinement is the UV rectangle (slot-1 stream, now in the census as
`streams:`), which is position-independent.

### UI pick tool (21 Oct 2026)

Guessing signatures from census diffs took five rounds and produced a
fragile rule, so identification is now a mouse pick: in flat mode, hover
the element and press **F9**. The next frame's UI pass tests every 2D draw's
quad(s) against the point (UI space is a centred 1920 x 1080 layout, x
-960..960, y -540..540 down, scaled to the backbuffer; the pick is passed
normalised; calibrated from two near-misses: the command-cursor triangle
and the HP window both landed exactly once 1920 x 1080 replaced the
1632 x 918 guess) and logs each hit
to `ffxvr_diag.log` - texture pointer and size, index count, rectangle, and
the whole cb0 - and lists the hits under Debug > "Pick a UI element". A
miss logs the nearest draw instead, which is also how to check the
coordinate mapping (pick the HP window, see if the rect agrees). **F10**
toggles slow motion (a sleep per frame, slider in the same panel) so
damage numbers and other flashes can be hovered. (The old native
constant-buffer capture also sat on F9 - a multi-second dump that looked
like a freeze; it is Ctrl+F9 now.) The sprite's atlas rect
should be in cb0; once it is known for the cursor, the hide rule becomes
"this texture + this atlas rect", position-independent.

First try picked nothing ("nearest:" empty): with the headset off, the UI
pass was never recognised - the scene-target / resolve / depth-clear state
machine only ran in stereo, because it exists to build the UI layer. Flat
mode now has its own small tracker (scene target = most draws with depth
this frame; its resolve, then a depth clear on it, opens the pass), and UI
draws found that way go through pick, census and the hide rules, then draw
normally. F9 is also no longer shared with the native constant-buffer dump
(Ctrl+F9 now).

### Picks (21 Oct 2026, evening)

- Target cursor over a stationary enemy: **1024x1024 atlas, one quad,
  68x45** (at 170..238, -183..-138 that time). The CTB portraits are 70x46,
  so the hide rule is now the exact size, no position test. The pick on a
  moving enemy missed (the quad had moved on by the next frame).
- "Turn cursor" pick landed on the 1600x55 strip across the top of the
  screen (8192x4096 texture at y -444..-389): the enemy-name bar, not a
  cursor; the turn highlight is probably part of the CTB list.
- Damage numbers under slow motion: two misses - one frame where 102 UI
  draws were recognised but none had a readable vertex pointer (the "mapped
  within two frames" test again; dropped, the pointer alone is used now),
  one where nothing was near the point. They may not be UI-pass draws at
  all (world-placed sprites in the scene pass); next: a pick that also
  reports scene-pass draws near the point.
- Turn cursor (the pointer floating over the acting character - Tidus at
  the time, upper right of the flat view), three picks: a **textureless
  6-index quad**, width pulsing 15..28, height 30. (First read as the CTB
  list marker from its x; the user corrected that - the pick point says
  where the thing was, not what it is.)
- Hiding is now a **hide list** of sprite signatures (texture size or
  textureless, index count, width and height ranges), kept in the ini as
  `hide_list`, defaulting to the target pointer and the turn pointer. Each
  pick hit has a "hide this" button that adds its signature (width range
  widened for pulsing); entries show how many draws they removed last frame
  and can be removed. `hide_cursor` switches the whole list, battle only,
  headset only.
- The target cursor then escaped the list in the next session: the pick
  returned a 1024-atlas draw of **12 indices spanning 1200 x 130** - the
  game had batched the cursor quad with another sprite into one draw, so
  neither "one quad" nor the draw's bounding box matched. Signatures are
  now per quad (texture + quad size, no index count) and every quad of a
  draw is tested; a matching quad inside a batch is cut out by drawing the
  rest in two pieces around it (`ffxvr_draw_skip`, bit 17 of the draw
  result). The pick reports the quad under the point, not the draw's
  bounds, and "hide this" no longer adds duplicates; the list has a reset
  button. (The user's list had collected 20 junk entries from repeated
  clicks on the batched draw; the ini was reset.)
- Damage numbers, four more misses with 193 UI draws recognised and
  nothing within reach of the point: they are not UI-pass draws. Next
  census line for scene draws carries the slot-0 texture and index count,
  and a pick miss reports how many scene-pass draws sampled a 2048x4096
  sheet (the font sheets) that frame - the numbers are expected there.
- cb0 is the orthographic projection and identity matrices, identical for
  every UI draw; no PS constant buffers are bound; vertex stream 1 reads
  (0,0,1)/(0.5,0.5,1) for every sprite and stream 2 is a colour. The atlas
  rectangle is not in any of them, which means it is in a stream we are
  not reading (slot 3+), or packed in the 12-byte position stream. Not
  needed for the cursor any more; worth knowing for the damage numbers.

### The pointer rule (22 Oct 2026)

The turn pointer defeated the size list: it is a spinning billboard, so
its width runs 18..40 within a battle, and its height changed from 27 to
40 between battles (the game camera's distance sets the size). Eight
hand-added entries still left an edge-on sliver. Sizes are the wrong
handle; position is the right one. The game places both pointers by
projecting the actor through its own camera, and we have that camera
(`g_cam`, the one the VR override is built from) and this frame's actor
root positions (`g_world_live`, the translations of the skinned world
matrices). So `hide_pointers` (default on, battle, headset): project the
actors into UI units at the first UI draw; any quad that is textureless or
from the 1024 atlas, at most 90 units on a side, whose centre is within 55
units of an actor's column and whose bottom sits from 30 below to 420 above
the actor's root, is a pointer - cut out of its draw like a list hit. The
signature list stays as the manual fallback. The highlight shows the rule's
work: red boxes are quads it would hide, green crosses are the projected
actor roots.

First test took the CTB portraits with it (picked: 70x46 at x 692..768,
rows 48 apart). Two causes, both fixed: the list entry for the target
cursor said 66-70 wide and 70x46 is exactly the portrait (now 66-69), and
the pointer rule had no notion of "on a window" - a character standing at
the right of the screen projects into the CTB column. The rule now records
the frame's window backgrounds (quads at least 100 x 100 drawn from the
1024 or 8192 sheets, or textureless; the CTB panel is a 128 x 288 quad of
the 1024 sheet) and a small quad lying inside one is an icon, never a
pointer.

### The game's own cursors, re-projected (22 Oct 2026)

With the pointer quads identified at draw time, hiding them is only half
the job: the same moment is the chance to keep them. When a pointer quad
is cut out of the UI pass, its complete draw state is captured - texture
and sampler, VS / PS, input layout, blend state, the ortho cb0, and the
UV and colour streams at that quad's own vertex offset (slot offsets
re-based to `base + 4q`), plus the quad's size and the actor it floated
over (from the pointer rule's match). At the end of the frame, where the
3D markers are drawn, each captured sprite is drawn again into each eye
image with the game's own pipeline: our position stream (four vertices in
UI units, the game's index pattern 0 1 2 2 1 3), bottom-centred on OUR
projection of the marker nearest that actor, so the real cursor - with its
spin and its art - sits over the target in stereo. Markers that got a
sprite skip the stand-in triangle. `markers_game_art`; the overlay counts
sprites drawn per frame.

Outcome: nothing drawn. The re-issued draw depends on the game's vertex
streams carrying the sprite's atlas coordinates, and the picks showed that
streams 1 and 2 hold the same values for every sprite (a constant and a
colour) - where the UVs come from is still unknown (a stream beyond slot
2, or generated in the shader). Rather than dig further, the decision (22
Oct) was to make our own markers good: the option stays, off by default,
as "experimental".

### Markers, second pass (22 Oct 2026)

A first redesign (arrow head + shaft, outline, yellow / white, bob and
spin) was rejected: the target pointer must stay red, and a generic arrow
is not what is wanted. Reverted to the triangle in the original colours
(target red, turn cyan). Kept from the pass: the tip sits just above the
actor's **head** (the highest sub-position in the actor table, which
follows each monster's own height) instead of a fixed height above the
feet, with "pointer clearance" and "pointer size" sliders (`marker_scale`
saved); the turn marker is tagged by alpha 0xFE in its colour word so the
native side can treat the two kinds differently later.

Options for the look, discussed 22 Oct:
1. keep the triangle (now);
2. the game's own art, by hand: dump the 1024x1024 sprite sheets the UI
   samples ("Dump UI atlases" under Captures writes them as .dds - they
   are BC3), cut the cursor out, ship it as a texture in the mod and draw
   it as a textured billboard at the marker. Authentic look, no runtime
   dependency on the game's UV source; the only work is finding the
   sprite in the sheet once;
3. hand-made geometry imitating the game's pointer shape (needs a
   reference: the cut-out from 2 would be that too);
4. the sphere grid's cursor model (`omd/CURSOR.omd`) - a real 3D object,
   but a loader is a project of its own.

Decision (22 Oct): leave the triangle as it is for now. When this is
picked up again, option 2 is the plan: press "Dump UI atlases" (Debug >
Captures) in a battle with a target selected, find the two pointers in
the `ui_atlas_1024x1024_*.dds` files (the target pointer is 68x45 UI units
and red; the turn pointer is a plain coloured quad, so it may be geometry
rather than art), cut them out, ship them as a texture in the mod, and
draw them at the markers as textured billboards through the marker pass
(it already draws per eye; it needs a textured variant of its shader and
a sampler). The capture-and-reissue path (`markers_game_art`) stays in the
code, off, in case the UV source turns up: the next thing to try there is
reading vertex streams 3+ in a pick (the pick prints up to slot 5 now).

### Battle cleanup: where things stand (22 Oct 2026)

Done this session: battle follow from the game's field roster; the game's
two battle pointers identified (by mouse pick, with hover highlight) and
hidden in the headset by a position rule plus a signature list; the
pointers re-drawn as 3D markers above each actor's head; the mouse pick,
slow motion, highlight and atlas-dump tools. Still open from the plan:
face the enemies on battle start / eye switch, blink on eye jumps, boss
diorama check, transitions, overdrive inputs, marker labels.

### Hover highlight (21 Oct 2026)

**F11** cycles a highlight drawn on the monitor from the same data the pick
uses: the quads under the mouse (yellow, filled, with a size + atlas label)
or every UI quad the mod recognises (white outlines). It shows what F9
would pick before pressing it, and, in the "every quad" mode, which sprites
the mod can see at all - a cursor that is visible on screen but has no
outline is being drawn outside the UI pass. Native: `ffxvr_ui_hover`
(mouse, mode) each frame, `ffxvr_ui_hover_rects` read at Present.

### Farplane check (21 Oct 2026)

[Farplane](https://github.com/topher-au/Farplane) has no camera or cursor
knowledge (its only camera item is the FreeCamera debug byte we already use
through Fahrenheit), but chasing its `PartyInBattleFlags` through the exe
turned up the game's own **field roster**: three chr ids at `FhUtil`
0xD2C895, kept current by battle setup, swaps, summons and dismissals
(exe-analysis.md section 15). That replaces every distance heuristic in
`btl_follow`:

- field detection at the first command window -> read the three bytes;
- benched = "60 units from the slot" -> chr id gone from the roster;
- successor = "visible member within 40" -> the id that took the slot;
- Tidus joins -> chr id 0 appears in the roster;
- Aeon out = "visible ply with chr_id 8-63" -> `field[0]` in 8..0x11, and
  the summoner is the aeon entity's +0x6D1 (no Yuna assumption needed);
  the pre-summon party sits at 0xD2C89C until the dismissal.

Confirmed in the log (21 Oct, two battles): `[2,5,1]` from the first
battle frame, a Change window flips a slot in place (`[0,5,1]`, Auron to the
reserve list), a summon leaves `[8,255,255]` with the party saved at
0xD2C89C and `cur_chr_id=8` on the Aeon's turns. One caveat: before setup
runs the bytes are stale (`[0,0,0]` seen on the first frame), so a roster
with a repeated id is ignored. (7 Nov) They are also the last battle's
party until battle_state 7: MsBtlReadSetScene 0x783E20 writes this battle's
(Chr init 0x79C110) and moves on to 8 or 10, so the roster is ignored in
states 4..7 (about 20 frames) and the eyes keep the field pick meanwhile;
7 Nov window 3 had followed Auron from the stale `[5,2,4]` for a second.

`btl_follow` now runs on the roster: the eyes follow a chr id; Tidus when he
is on the field (and the moment he joins), else the id that took the
followed slot, else the lowest id; a summon hands the eyes to the summoner
read from the aeon entity (+0x6D1), dismissal hands them back; manual cycle
walks the three slots. Entities are only used to turn an id into an actor
position. A swap-in waits until the newcomer has stood still for a quarter
second (or 5 s) so the eyes do not ride the run-in.

First test (21 Oct, log 142113): battle start `[2,5,1]` -> Yuna's eyes,
correct. Tidus swapped in (`[0,5,1]`) but the eyes never moved: the join is
a one-frame event and the arrival wait dropped it on the next frame. Fixed:
a pending switch stays wanted while its character is on the field. Summon
of Ixion showed `[255,255,11]` - the Aeon takes the summoner's slot, not
slot 0, so the aeon check scans all three.


## Movement ghosting on controller turns and walking (22 Oct 2026)

Reported: turning with the right stick, or moving, shows ghosting in the
headset; turning the head alone does not. The asymmetry is the diagnosis:
the runtime reprojects every displayed frame for head motion (the frame is
submitted with the head pose it was rendered from; the compositor rotates
it to the pose at display time), so head turns are smooth at the display
rate whatever we render at. Anything the head did not do - a stick turn,
the character walking - only changes when we render a new frame, and is
shown stepwise at our frame rate; Virtual Desktop's spacewarp (if it is
on) then synthesises in-between frames from image flow, which is where
smearing / double edges ("ghosting") come from.

Two rates matter. The game's logic runs at 30 Hz; the FFX-60fps mod draws
60 by interpolating camera and character motion between ticks ("SMOOTH").
Our view takes the player's position from the actor table, which may well
be the 30 Hz value - then the eye moves in 30 Hz steps while the world
around it is drawn at 60, the worst case. The stick turn is applied per
drawn frame (60). Numpad 5 / "Motion log (3 s)" (Captures) writes one line
per drawn frame with the actor-table position, the nearest rendered skinned
position, the view yaw and the head yaw: if the actor position changes only
every other line while the rendered one changes every line, the fix is to
drive the eye from the rendered position (or interpolate ourselves).

Beyond that: a headset refresh that is a multiple of the draw rate (120 Hz
for 60 fps) gives an even cadence; Virtual Desktop's SSW set to off turns
smearing into plain steps (a useful test); snap turning (`turn_mode`)
avoids the issue for turns. Smoothing a virtual turn properly would mean
re-submitting the last frame at display rate with a pose that carries the
interpolated yaw - a submit thread, rotation only; walking cannot be
smoothed without drawing faster.

(Changed in the stickrep build, 28 Oct 2026: snap turning no longer exists, and the stick turn is now re-submitted at the
display rate from the render thread's ring wait, rotation only, as described here; see "Stick reprojection" at the end.)

## Black screen during scene transitions; presenter thread (22 Oct 2026, design only)

Reported: some transitions show black for a few seconds - a random battle
starting, talking to Cid to get the airship destination menu. The perf
lines show why: frames of 1.6 s (`frame ... max 1612; gpu max 345`) while
the game loads the next scene. On a monitor the last image just stays up.
In the headset, the OpenXR frame loop runs on the game thread inside
Present, so it stops with the game; after a couple of missed frames the
runtime blanks the app. The black is our loop going silent, not the
game's fade.

Shortening the stalls: the likely big contributor is the Re-Remastered
pack (18 GB of loose files read synchronously through the file loader);
the user wants to keep it. Smaller: our twin targets being recreated when
the game recreates its 4K / 4xMSAA targets (the 345 ms GPU peaks) - a
twin pool by size/format would remove that part.

The real fix, deferred (user, 22 Oct): a **presenter thread**.

- It owns the OpenXR session and runs the loop at the display rate:
  xrWaitFrame -> locate views -> acquire -> copy the latest completed eye
  pair -> xrEndFrame. It never waits on the game. During a stall it keeps
  submitting the last frame with the pose it was rendered with, so the
  runtime reprojects it (steady, head-tracked); after ~0.5 s with no new
  frame, dim it or add a "loading" quad.
- The game thread stops making OpenXR calls. It finishes its eye images
  as now, then publishes them (and the UI / ImGui layer textures) into a
  small ring of shared textures stamped with the head pose it rendered
  with. Poses flow back: the presenter leaves the latest prediction in a
  shared slot that the game thread reads at frame start.
- The obstacle: the game's immediate context is not thread-safe, so the
  presenter cannot use the game's device. It gets its **own D3D11 device**;
  the session is bound to it; eye images cross as **shared textures**
  (shared NT handle + keyed mutex: game writes, releases; presenter
  acquires, copies into the swapchain image, releases). One extra copy per
  eye per frame. Our eye images are already resolved non-MSAA copies we
  create, so making them shareable is a flag (MiscFlags SHARED_NTHANDLE |
  SHARED_KEYEDMUTEX).
- Also moving: xrSyncActions / controller input and the space-change
  (recentre) events (published as a snapshot the gesture code reads), the
  UI and overlay quad layers.
- Pacing: the game no longer blocks in xrWaitFrame; it waits on a frame
  event from the presenter to keep 1:1 cadence, falling through if the
  presenter is behind.
- Shutdown and device loss (headset off, VD disconnect, game exit with a
  live session) need care.
- Bonus: once the presenter picks the pose per display frame, stick turns
  can be smoothed - present the last frame rotated by the yaw the turn has
  reached now - so virtual turns become as smooth as head turns at any
  frame rate. Walking still needs a re-render.

Effort: the biggest change since the camera override - most of
ffxvr_xr.cpp restructured, a second device, the shared ring, the thread.
A few days with testing. Risks: shared-texture setup failing silently as
black, cadence errors giving worse judder than today, shutdown crashes.
Milestones: (1) the presenter shows the last frame during a stall,
nothing else changes; (2) pose-based turn smoothing; (3) loading overlay.

## Backlog (22 Oct 2026)

Release: Fahrenheit dependency (pin a commit or get a release; questions
for the crew drafted); our licence; LICENSE + THIRD-PARTY-NOTICES;
packaging script; versioning; changelog; user-facing README (see
docs/release.md).

Presentation / comfort: presenter thread (above); blink on eye jumps;
face the enemies at battle start and on eye switches; camera wall
collision; intro credits double vision.

Battle: aeon handling for every aeon (Magus Sisters are three ids); boss
diorama; transitions (shatter, results, game over); overdrive inputs
(Lulu's stick spin); target-name labels; cursor art from the atlas dump;
damage numbers / status text (scene-pass draws). Battle first person (4 Nov
2026): when the view switches to Wakka, his eye sits about 2 ft (~0.6 m, ~7
units) too high; the other characters are fine. Suspect the head point
picked for his model (tall hair?). Needs a battle census, actor table and
motion log while viewing as Wakka.

Simple settings page (owner, 8 Nov 2026): a game-styled ImGui page (the blue gradient windows, a game-like font, the
hand cursor) with only the common settings (the `C` tags in research/vr-menu-design.md 2.3), drawn by the F8 menu's
own widget helpers; opened from the controllers (both stick clicks clash with Virtual Desktop's overlay: left grip +
left menu button, or R3 held 1 s while paused); the game's Config menu stays untouched, as Fahrenheit did for the save
screens. After the F8 reorganisation and the VR panel (S1-S3) are done and agreed.

Controls, Quest controllers only (owner, 4 Nov 2026; design research/quest-controls-design.md, built 5 Nov on branch
quest-ctl, ABI 44; the pure rules in src/native/ffxvr_pad.h with a unit test; owner test pending). Done:
(1) **Save / load screen**: Fahrenheit's save UI is ImGui, and ImGui's Win32 backend reads pad 0 through its own XInput
DLL (xinput1_4 first), not the game's XINPUT9_1_0 import. imgui_pad.cs detours that DLL's XInputGetState /
XInputGetCapabilities (FhCallHandle, like the D3D11 detours); native merges the VR pad in (ffxvr_imgui_xstate: buttons OR,
the stronger stick, no rotation, no tap, no Start / Back: Start is Fahrenheit's soft reset with L1 + R1; buttons already
held when the screen opens stay masked until released) and posts one WM_DEVICECHANGE (DBT_DEVNODES_CHANGED) so the backend
asks the capabilities again. It logs the functions' first bytes (another hooker shows as a jump). If the game's import
resolves into the same DLL (xinput9_1_0 forwarding into xinput1_4) it installs anyway and logs so: the game's own calls
pass through untouched (a thread-local re-entry guard set around the game's call; the managed shim throttles only
ImGui's render-thread reads); `imgui_vrpad_strict=1` refuses instead. It always refuses when ImGui picked xinput9_1_0
without our import hook (no guard). Pushing ImGui key events from our module instead was rejected: with an Xbox pad connected the
backend re-adds the pad's state every packet and the two sources fight (repeated presses). The game gets nothing more:
Fahrenheit locks its input while the save screen is open. ini `imgui_vrpad` 0 / 1 (default: the save screen only) / 2 (all
of ImGui, Start and Back too). (2) **Rotated stick in battle**: the cause was our first-person movement remap
(`g_move_delta` = the view's yaw, head included, minus the game camera's), applied in every view but the seat, battle
included. The walk gate: the remap now applies only while the player can walk (not battle, a field menu, no player
control, a choice box, the save screen, the sphere grid; riding and blitzball keep it; game bits older than 250 ms mean
raw). ini `move_remap_walk_only` (default 1; 0 = the old behaviour for an A/B). (3) **Battle actions**: verified in our exe
(design 3.1): Defend = **Y** (bit 0x10; our exe-analysis tables label 0x10 Square and 0x80 Triangle: that looks swapped,
the owner's test confirms), switch = LB, CTB scroll = RB / RT held, overdrive = stick left and Weapon / Armor / Escape =
stick right at the command menu, help = Back. The grips were lost to the seat / diorama grab and the left-grip chords
leaked an LB press: now a **quick squeeze** (released within `grip_tap_ms`, 300 ms; the hand moved < 3 cm and turned
< 10 degrees, or more for a shorter squeeze: 8 cm / 40 degrees up to 130 ms, linear to 3 cm / 10 degrees at 250 ms (7 Nov:
every 63-125 ms squeeze had been refused for its own motion, `ffxvr_pad.h` `grip_limits`); no stick click meanwhile; the
other grip not down) is LB / RB, sent on release as a press of 2 new packets
(`grip_tap_press_ms`, default 0, can lengthen it up to 150 ms: a hold past 0.2333 s trips the game's repeat query
0x630D30, the switch list's next / previous), and a grab's nudge during it is undone. Why 2: the CTB scroll
(0x89F021) steps a row when R1 is held and the list is at rest, then slides 11 px (half a row) a tick, so R1 seen on 1 or 2
UI ticks is one row and the tap's 3 polls / 100 ms would be two; the game polls once per logic frame (about 24 polls a
second in the owner's 4 Nov log), so 2 polls is one row when the UI ticks once per poll or slower (unit test against a
model of the handler); a held grip stays the grab and the chord modifier and
the game never sees it. In battle (all views) and the seat / diorama only (`grip_lbrb` 1; 0 = the old behaviour, 2 =
everywhere); in the field's first / third person the grips are LB / RB at once as before (the main menu's character pages
hold L1 / R1). `vd_overlay_mask` (1): both stick clicks (Virtual Desktop's overlay) send neither Back nor R3. Left: Auron's
button sequence and Lulu's stick spin as VR gestures (meanwhile the Xbox pad; Lulu's spin works with the raw stick);
whether blitzball's pass / shoot menus want the raw stick (owner test); RB held (continuous CTB back-scroll) is now one row
per squeeze in battle; a follow-up: the game's own walk direction g_PlayerDirFix (0x13007C4 flag / 0x13007C8 yaw, read by
Sg_ControlPlayer at 0x82D795 instead of the latched camera yaw 0x130079C) could walk the player relative to the headset
with no stick rotation at all, but event scripts also set it (0x82D990, called from 0x83B519 and 0xA77C07), so we would
have to own it only in our first-person views and give the script's value back; untested.

Carried cloister sphere in the VR hand (owner, 7 Nov 2026). In first person, while Tidus carries a cloister sphere, show it
resting in the VR right hand's palm (right by default, as in the game) instead of where Tidus's hand would be. Plan, option 2
first: hide the carried sphere's game draws in first person (the body hide's IB / draw identity work: the sphere's model
chain gives its index-buffer keys as "another actor's") and draw a simple glowing sphere in the palm, coloured by the sphere
type. Drawing only: the game's sphere and puzzle logic are untouched. Option 1, re-drawing the game's own sphere model at the
palm, comes later if option 2 is not good enough. Hook: the eye's shared-root lines (round 3c) name the actor on Tidus's root
during a grab (dj/djyt0000: actor 13 `f010`, an object model, 2 segments); while carried it is not on his root (the rule was
off between grabs), so finding it then (attached to his hand bone? +0x19C) is the first step.

Field / modes: per-area diorama memory by event_id; swimming / underwater
eye height and the distortion pass; blitzball and chocobo (doc only);
gesture tuning.

Tabled: VR hands (NaN-cut build untested, hands_dev=1).

Not tried yet (ideas, 22 Oct): loading-stall dim quad as a stopgap;
pre-warming the texture pack's files for the next area; a twin-target
pool; snap turn + blink in battle, smooth in the field; enemy name drawn
over the target in 3D; per-mode panel placement; dialogue boxes placed
near the speaking NPC; save screen as a panel instead of the virtual
screen; haptics on damage / your turn (xr_haptic is unused); one-press
seat height calibration; head-gaze targeting; auto MSAA 4->2 when frame
time crosses ~13 ms; photo mode (freeze + free head); per-scene cutscene
view memory.

## Blitzball, first capture session (22 Oct 2026)

A league / tournament match from a late save (save sphere > Play
Blitzball), first person on, frame census, image sequence, actor table,
atlas dump. Findings:

- **Actors.** 13 live: 12 athletes and the ball. Our team at indices 0-5
  (`c001` = Tidus in his blitz model, `n172`..`n176`), the opponents at
  6-11 (`n197`, `n198`, `n278`, `n199`, `n200`, and `n198` again for the
  keeper), the **ball is `w041`** (index 12; "not drawn" in most actor
  logs - probably while held). Keepers sit on the long axis at z = +-580;
  the field spans about x -330..330. `fp_actor=c101` does not exist here;
  first person fell back to actor 0, which happened to be Tidus.
- **Every athlete reports y = -520 in the actor table.** The game's blitz
  logic looks planar (the radar is a flat disc), with the visual height
  added elsewhere. Next capture: the rendered skinned positions next to
  the actor positions, to see how the 3D placement is derived. Pool centre
  for a globe anchor: about (0, -520, 0), radius ~600.
- **The in-pool markers are 3D.** The ball-holder circle with its
  triangle and the yellow floor emblem are scene geometry drawn with the
  camera (present in the scene snapshot): they are already right in
  stereo. Unlike battle, nothing floats; no pointer work needed.
- **The UI**: Breakthrough / command menus, the stat bars top right, the
  radar, time and score - panel material. **But the radar dots are not
  UI-pass draws**: after the scene resolve the game draws ~40 small
  textureless blended quads (VS cb 256 B, no z-write, n=6, two of n=18)
  with no camera we recognise ("NO IMAGE" in the census, scene draws
  417..460). The eye-1 image has the radar dots, the eye-2 image has an
  empty radar. In the headset they presumably sit in one eye at screen
  depth. To confirm with the scene bisect ("Find a draw", hide 417-460):
  if the dots vanish, route that run into the UI layer like the UI pass.
- **Performance**: GPU 19-20 ms per frame in a match vs 11-15 on the
  field; the game settles at 30 fps (33.4 ms frames) on the 72 Hz
  headset - the ghosting recipe. Extra 4096x4096 targets (reflection /
  refraction) at 4K + 4x MSAA. A per-mode MSAA (2x in blitz) or the
  auto-MSAA idea is the first lever.
- Eye images: the large left/right offset between the two eye images is
  the Quest 3's canted, asymmetric per-eye frustums (tan l -1.376 r 0.839
  vs l -0.839 r 1.376), not an error.
- The atlas dump caught a 1024x1024 stone texture (a scene texture the UI
  pass also binds), not the UI sheet - and its DDS header had the pixel
  format one dword off (fixed). The dump should filter on the UI's own
  shaders.

Next captures there: the scene bisect on the radar run; a log of rendered
vs actor positions for 10 s of play; picks (F9) on the stat bars and the
radar background; who-has-the-ball (Farplane's blitz pointers).

### Blitzball, second session (22 Oct 2026)

- The automatic position log did not run: map names carry a folder prefix
  (`bl/bltz0000`, `te/test20`, `me/memochek`), and the test was
  `StartsWith("bltz")`. Fixed (match after the slash). A save-sphere match
  is `bl/bltz0000` with event_id 62.
- **Picks: the radar is ordinary UI.** Its grid (418x1 and 2x313 lines),
  and the athlete dots (textureless 23x32 / 31x22 triangles, ui draws
  12..21) are UI-pass draws, so they go to the panel with everything else.
  The empty radar in the eye-2 flat dump is a dump artifact (UI draws are
  not re-issued into eye 2's backbuffer copy); retracted as a problem.
- Picks on the command window: a 472x223 window (1024 atlas, textureless
  frame and fill), the menu hand cursor a 68x45 quad of a 4096x4096 sheet,
  text glyphs from the 2048x4096 font sheets. **The glyph draws have a
  vertex stream 3, stride 8: the atlas UVs.** That is the UV source the
  cursor-art idea lacked (pointer quads printed only streams 1-2 then).
- **The scoreboard (time, score, team badges) could not be picked**: not a
  UI-pass draw. The first session's census has the candidates: 44
  "NO IMAGE" scene draws right before the UI pass opens (scene draws
  417..460 of 461; 40 quads of 6 indices, two of 18, VS cb 256 B, blended,
  no z-write, depth bound). Drawn into the scene target with no camera we
  recognise, they land identically in both eyes. To confirm: the scene
  bisect hiding the last ~45 scene draws. If it is the scoreboard, the fix
  is to treat that run like the UI pass (into the UI layer), keyed on its
  VS cb size and position right before the UI-pass depth clear.
- Headset report: the scoreboard is barely visible and doubled - as
  predicted for a camera-less overlay painted into both eye images.
  Census detail: the run is two parts - 42 quads after the scene resolve
  (VS cb 256 B, two shader pairs), then a resolve, then two more draws
  (one into an 832x624 viewport at the radar's place: the radar's disc,
  drawn as geometry) - and only then the depth clear that opens the UI
  pass. **Fix in the build (`blitz_overlay`, default on, bltz maps,
  headset)**: on a blitz map, the first draw into the scene target after
  the resolve with no camera image, blending on and no depth writes opens
  the UI pass early; the resolve after the run pauses it rather than
  ending it; the next such draw or the usual depth clear resumes it without
  re-copying the eye images (the game's resolve then has the overlay baked
  in) or clearing the UI layer. The run ends up on the head-locked panel.
  The overlay counts routed draws. Field effects without a camera (fog,
  water) are untouched: the rule only runs on blitz maps.
- First test of the routing was worse: UI in one eye, a 2D pop-up at the
  "Blitz Off" banner, UI hard to see after. The log shows "captured UI pass
  0" on every frame: the resume re-armed the early flag, so the resolve
  after the real UI pass paused it again instead of ending it; the frame
  never counted as a UI frame and fell back to the backbuffer path, where
  the UI draws had been diverted out of eye 2. Fixed (the resume does not
  re-arm). Untested.
- Position log (147 lines, 1716 athlete samples): the drawn root equals
  the actor position (|dxz| 0, |dy| <= 14) - the actor table is the truth,
  in 3D. Most of the match is at y = -520; athletes rise to -353 at times
  (shots, keepers). Farplane's blitz pointers read zero here (game@0, mode
  all 0): not useful at those offsets.

### Alternative: a stereo cinema screen for blitzball (22 Oct 2026, idea)

The user's proposal: a large flat screen in the room showing the game's
own camera (cuts, chase camera and all), in stereo. It fits blitzball
better than any free view: the authored camera is how the game is meant
to be read, the screen is fixed in the room (no motion sickness), and
everything 2D - menus, radar, the camera-less scoreboard - is identical in
both eyes, which on a screen is exactly right (it sits on the screen
plane). The overlay routing above becomes unnecessary in this mode.

How: the override already renders both eyes from views we build. A
"screen stereo" view = the game's own view and projection, the two eyes
shifted by +-sep/2 along the camera's right vector (sep in game units,
a slider), with a horizontal image shift in the projection so the
convergence plane sits at a chosen depth (otherwise everything pops out
of the screen). Submission: two quad layers, one per eye
(XrCompositionLayerQuad.eyeVisibility LEFT / RIGHT), each sampling its
eye's image, at the screen's pose - the virtual-screen layer we already
have, doubled. Cost: what we render now. Open points: the screen's size
and distance (a cinema-sized default), the depth amount, whether the
pre-rendered FMVs and cutscenes want the same mode later.

### Stereo screen, first build (22 Oct 2026)

Built (untested): `stereo_screen_blitz` (default on) and
`stereo_screen_all` (off; the general view option a viewer asked for).
On 3D frames while it is active:
- `build_override_view` returns the game's own camera moved -+sep/2
  along its right axis (eye 1 left, eye 2 right) - no head pose, no first
  person, no diorama, no seat;
- `write_camera` skips the headset FOV and instead adds a horizontal
  image shift to both projection variants, P[2] += P[0] * (+-sep/2)/plane,
  so the centre point at the screen-plane distance has zero disparity;
- the UI pass is not split off (no panel, no blitz overlay routing): UI and
  camera-less overlays stay in the picture, identical in both eyes, on the
  screen plane; body hiding, cut blinks and cut recentres are off (a
  recentre would move the screen);
- `xr_frame` submits the virtual screen as two quads, eye 1's image to
  the left eye and eye 2's to the right (`eyeVisibility`), the right one
  from a sixth swapchain (5) of the same size.
Separation follows the 1/30 rule: sep = depth x plane / 30 (depth 1 =
standard, 0 = flat); plane default 300 game units. Screen size from the
virtual screen's width (slider repeated next to the stereo options);
distance and placement as before (Home recentres it in front of you).
The overlay reports "showing: stereo screen". Mono 2D frames (menus, FMV)
still use the plain screen.

### Stereo screen: auto depth (22 Oct 2026)

Report from the first run: works well until something is close to the
camera - blitzball's chase camera and cut-ins put athletes next to the
lens, and at a fixed plane of 300 a player at 30 units pops out ten times
further than the plane, past what the eyes fuse (double images).

`stereo_screen_auto` (default on): once per frame, from the game camera
and the previous frame's skinned actor roots (`g_world_report`), every
actor in the picture is projected; the **screen plane follows the
subject** (the actor nearest the image centre, bounded 60..2000) and the
**separation is capped** so the nearest visible actor's crossed disparity
stays under `stereo_screen_max_pop` (2.5% of the screen width by default;
disparity = P0 * sep * (1/d - 1/plane) / 2 of the width). Targets ease
with a 0.3 s time constant and snap on camera cuts. The depth slider is
the strength when nothing caps it (1/30 rule); the plane slider only
applies with auto off. The overlay shows what it is doing (characters in
view, nearest, subject, plane, separation). Not covered: close scenery
(walls, stands) - only actors are known; the depth buffer would be the
next step if that shows.

Second pass (user at depth 0.88, pop-out 0.5% - the slider's minimum - and
wanted it more aggressive): two things were wrong with the first version.
(1) The 1/30 rule ignores the physical screen: with a 5 m screen at 2.5 m,
sep = plane/30 puts the far background 16 cm apart on the screen, beyond
the eyes' 6.4 cm - divergence, which reads as "too much depth" however
the pop-out is set. The separation is now also capped so the background
disparity stays under 0.8 x 64 mm over the screen width (1% of a 5 m
screen). (2) The pop-out cap reduced the separation, flattening the whole
shot whenever anyone was near the lens; now it moves the screen plane
forward toward the nearest actor instead, keeping the depth (the subject
sits slightly behind the screen). The pop-out slider goes down to 0 =
nothing ever in front of the screen. The background cap scales with the
screen width slider: a narrower screen allows more depth.

Third pass (22 Oct): (a) athletes "wiggle between double vision and 3D" -
the subject (actor nearest the image centre) flipped between athletes
frame to frame, dragging the plane with it. The subject is now sticky
(kept while in view; a challenger must be nearer the centre by 0.15 ndc^2
for 0.5 s; cuts reset), the plane eases with a 0.8 s constant, the
separation 0.25 s, and the eased pair is re-checked against the pop-out
cap every frame so lag never lets a close actor split. (b) The screen
distance slider only took effect at a recentre: the screen pose is now
re-derived from the reference whenever the distance changes, and the
slider sits next to the stereo options. (c) **VR-hands leftover**: with
the hands off, `hands_apply` still ran its NaN cut on the adjoining
forearm segments (the cut did not check `g_hands_on`), so the player's
model lost forearm pieces in every view within the hide radius -
including the blitz model. `hands_apply` now returns at once when the
hands are off.

## Review fixes, build 1 (24 Oct 2026)

From the 24 Oct review (items R1, R5, R6, R7a/b/d, R13, R34-R38). Frame time and
settings pushes only; nothing retuned.

- **Settings reach native every frame (R1).** Before, the setters ran inside the F8
  window code, so with the overlay closed some never ran (pitch follow, cuts and pans,
  backdrop, hist, UI panel, noimg, bisect) and the frame time came from the last push
  of the overlay - stale or the 1/72 default. Turning and follow ran at about 83% speed
  with the overlay closed, about 42% in blitzball. Now `push_all()` sends everything
  once per frame (guarded: several setters are edge-triggered, integrate per call, or
  read-and-clear, so none may run twice), and the window only draws widgets, reads
  status and runs one-shot buttons. Keys live in `handle_hotkeys()`, the gesture poll
  in `poll_gestures()` (its only caller). The native side measures its own frame time
  with QueryPerformanceCounter at the top of `ffxvr_begin_frame`, clamped 1-50 ms;
  the dt that `ffxvr_cuts_set` still receives is only used until that first measure.
  The screen3d / hide-list / virtual-screen pushes sat inside an unrelated
  `if (_fn_ui_hide != null)`: each now has its own check (`push_view_rules`), and they
  also run before the actor table exists. `xr_set` goes first in `push_all`, so on the
  frame the headset is unticked native has already left XR when the override and
  eye 2 are pushed (it ORs the headset flag into both): no one-frame flash. No defaults changed: the 90 deg/s turn was
  tuned with the overlay open, where dt was right. Blitzball turning and auto depth
  get about 2.4x quicker - the intended speed.
- **One ABI (R13, R38).** Every export is required (`TryGetExport`, missing names
  collected), plus `ffxvr_abi()` = 1 on both sides; bump it with any export change. A
  missing DLL, a mismatched pair, an init failure or failed detours open the F8
  window with the reason (detours: a red line over the normal UI). That forced open is
  not written to the ini; `overlay_open` keeps the earlier value until F8 is pressed.
  F8 always shows something: without the native helper, a status panel (the reinstall
  hint only for a missing or mismatched DLL; an init failure says the renderer could
  not be hooked). After a mismatch every bound export pointer is nulled. A fault in
  the window code is caught and logged once per message, so `push_all` still runs.
- **Hotkeys while typing (R36).** F8, F10 and F11 always work, F9 once the native helper
  is running; Home, End, Insert and the numpad are ignored while a text box has the
  keyboard (`WantTextInput`).
- **Battle cycle (R34).** Insert and the left-grip chord set a request that the battle
  follow uses in the same frame or drops; it no longer fires late at the next battle.
- **hands=1 with hands_dev=0 (R35)** no longer runs the VR hands.
- **Magus Sisters (R37).** Player loops run to 18 entries (Sandy and Mindy are ids
  16-17), monsters stay 16.
- **Headset off (R6).** `leave_xr_state()` resets the VR-only per-frame state on every
  exit (unticked, session lost, shutdown): screen mode, override suspend, stereo
  screen, backdrop room, blink, stick remap, UI pass state, gesture/grab and pad
  suppression, anchor and latches. Owner decision: the desktop view goes back to how
  it was - managed remembers the override and eye-2 switches when the headset goes on
  (which forces both on) and restores them when it goes off.
- **Headset start back-off (R7a).** With autostart on and Virtual Desktop not
  connected, `xr_start` used to run (instance create/destroy, log lines) every frame.
  Now in `ffxvr_xr_set`: retry after 2 s, then every 5 s; from the third failure on
  the attempt is muted and a summary line is logged on the 2nd and every 60th. The
  status line says "retrying in N s". Unticking resets it. A session that ends within
  5 s of starting (Virtual Desktop half-connected) counts as a failed attempt too; the
  count only clears once a session has lived 5 s.
- **Quiet XR failures (R7b).** `fail()` logs a repeating failure once, then
  "(xN in the last S s)" at most every 10 s; the status line still updates every time.
  Reset at the start of each `xr_start`.
- **tick() once per frame (R7d)** from `begin_frame` (Ctrl+F9 poll, heartbeat), not per
  Unmap; the safety-valve capture dump moved into begin_frame's first locked block.
- **Scripts (R5).** build-mod deletes only build output (`*.dll *.pdb *.deps.json
  *.runtimeconfig.json`, not recursive: the managed output has no subfolders, owner
  checked) and logs how many other files it kept;
  captures (`img_*.bmp`, `ui_atlas_*.dds`, `ffxvr_bones_*.bin`) survive a deploy.
  run-ffx also collects `.dds` and `.bin`.

## Review fixes, build 2 (24 Oct 2026)

Free frame time (review items R2, R3, R7c, R19, R20, R23 step 1, R24). The diagnostics
stay; they cost nothing while nobody looks at them.

- **Diagnostics gate (M1).** `g_diag_until`: the costly per-frame diagnostics run for
  the first 1800 frames (about 30 s) and while the overlay's Debug header is expanded
  (managed calls `ffxvr_diag_keepalive(2)` every such frame; closing the header or the
  window disarms them within two frames). Gated: the slot-0 readback and its latch,
  and the heartbeat cadence. Not gated: the captures (F9 pick, frame census, cb dump,
  Measure, 8-frame sequence) - they read data that is always kept. ABI 2 (two new
  exports, both required).
- **Slot-0 readback (R2).** It mapped a staging copy with a blocking Map every frame,
  so the CPU waited for the GPU to finish the frame (frame time near CPU + GPU instead
  of max(CPU, GPU)). Its only reader is the heartbeat's "changed since previous frame"
  line. Now: only while armed; a ring of three staging buffers tagged with their frame,
  mapped with DO_NOT_WAIT oldest first (stop at the first still in flight); the diff
  is printed as "n/a" when the two copies are not consecutive frames. Disarmed, the
  latched buffer is released and nothing is copied. If head motion feels laggier, the
  blocking Map was acting as a frame limiter: a one-frame-old event query wait is the
  fallback.
- **Scene snapshots on demand (R3).** Two full-resolution copies of the scene target
  (eye 1 and its twin) were taken every stereo frame for the eye preview and Measure.
  Now only while wanted (`g_snap_until`): the preview on "scene"/"scene eye 1" asks for
  them through `ffxvr_eye2_size/srv`, Measure through `ffxvr_measure` - which runs at
  once if the frame just finished has a snapshot, else at the end of the next
  begin_frame that has one (5 frames at most). Snapshots idle for 600 frames are freed
  (2x scene-target VRAM), with the preview's two scene-size copies of them. The per-bind `texture_of` in `ffxvr_on_ps_srvs` only runs
  while a snapshot is wanted (and stereo is on).
- **Heartbeat (R7c).** Every 1 s while armed, every 30 s otherwise. The buffer list
  (capped at 300 lines + "... N more") and the override stats are copied under the
  lock and printed after it. The trace header says so.
- **CB copies (R19).** `rec.data` keeps the first 256 bytes of each upload (nothing
  reads past 64 floats); printers show the real size. `record_buffer` itself is never
  gated: the camera and world extraction run on every write.
- **Camera scan (R20).** A buffer with no recognised camera is skipped at once when the
  frame has no known proj*view yet; otherwise the scan reads the original and copies
  into the scratch only at the first hit (`wr != rd`: the eye-2 call works in place),
  and the comparison tolerance is computed once per product instead of per block.
- **XInput (R24).** While the game polls XInput through its own import, stick turning
  uses what the game's call returned (recorded by the hook, per slot) and makes no
  XInput calls of its own - the 2-s probe of absent pads was a periodic 0.5-4 ms spike.
  Without the hook, or when the game is not polling, the old direct read stays (starting
  at the slot the game last read successfully), probing one slot every 2 s. The
  heartbeat log counts the game's XInputGetState calls.
- **Eye-2 same inputs (R23 step 1, measurement only).** `g_e2_same_inputs` counts eye-2
  re-issues that swap no input texture and upload no eye-2 constant data that differs
  from eye 1's (uploads with identical bytes do not count as different): an upper
  bound of the passes whose eye-2 output could equal eye 1's. On the `perf:` line,
  the heartbeat's eye-2 line and Debug > Second eye. Step 2 (skipping them) only if
  this shows a big target.

## Review fixes, build 3 (24 Oct 2026)

Small crash-proofing (review items R9, R10, R14, R16, R40, R42 parts, R43, R44, with
the R45/R46 hands reorder). No feel or default changes; ABI unchanged (2).

- **UI vertex pointers (R9).** The UI hide rules read the game's vertex streams
  through the pointer of their last Map. A buffer freed at an area change and a new
  one created at the same address could be read through the dead mapping. Now
  `hk_CreateBuffer` drops any record keyed by a newly created buffer's address (it is
  learned again at its first UI draw), and each record is checked with VirtualQuery
  once per frame before the guarded reads (an access violation is fatal under
  Fahrenheit's debugger even inside __try, so the check, not SEH, is the guard). No
  age window: the "mapped within two frames" test broke damage numbers and pointers
  before (Picks, 21 Oct). The heartbeat's "ui vertex streams" line shows purges,
  unreadable mappings and the oldest mapping a read used.
- **F9 pick (R10).** The stream dump read 8 floats into 4-float stack arrays and up to
  28 bytes past the checked range; it now reads exactly min(stride/4, 4) floats.
- **Ring slots (R14).** Every reuse of an eye-image ring slot resets all its fields
  (`ring_take`), and the split-hand passes run only when the right-hand variants
  really have the buffer's size. With it, the hands reorder (R45/R46): in the 3D path,
  eye 2's camera is written before the hands are applied and eye 1's image is taken
  after them. With the hands off (and no bone wiggle) nothing changes: the camera
  blocks and the bone palette don't overlap.
- **Hook hygiene (R16).** `patch` refuses to record our own hook as the original; a
  failed `ffxvr_init` (-2) puts back the vtable slots it changed (only those still
  holding our pointer; the diag log lists them); CreateBuffer and CreateTexture2D
  pass straight through for devices other than the game's (the vtable is shared by
  every device of the class), so the MSAA override can't touch someone else's
  targets. Unpatching at exit stays won't-fix (other threads may be inside a hook).
- **Export argument checks (R42).** Null pointers and non-positive sizes return early
  in world_positions, ui_hide_list, draw_skip, ui_hide_hits, ui_hover_rects,
  markers_set, screen3d_status, perf_status (a size of 0 aborted the process through
  the CRT) and vrpad_status.
- **Stale actor list (R40).** `g_world_report` is emptied after 2 frames without
  skinned draws, so an empty landscape shot drops the previous subject: auto depth
  eases back to the set plane.
- **Pointer sprites (R43, R44).** They were released at the top of begin_frame, before
  xr_tick's marker pass could draw them; now released right after it. Each sprite now
  keeps the index of its actor in the live list (the projected list skips off-screen
  actors). Native default `g_ptr_reuse = false`, matching the overlay. The "game's own
  cursor art" experiment can now really be run.
- **Small ones (R42a, b, d).** Window backgrounds are recorded once per draw (hover mode
  filled the 48 slots twice as fast); RGBA atlas dumps get the right channel masks,
  and the two sRGB 8-bit formats are dumped too; the never-installed hk_Draw*,
  hk_UpdateSubresource and twin_plain_srv are deleted (the UpdateSubresource geometry
  capture they held never ran, so `g_vb_updates` was always 0).

## Review fixes, build 4 (24 Oct 2026)

Smoothing once per frame (review item R4), a low-resolution warning, and a
troubleshooting row.

- **Once-per-frame view filters (R4).** The diorama / third-person anchor
  smoothing (0.5 per update), the battle-centre smoothing (0.05 per update), the
  third-person heading slew and the seat follow-pans all ran inside the view
  builder, which runs for **every camera upload** (dozens per frame), both eyes
  and the marker / projection callers. So the anchor converged within a frame
  (effectively a snap), different objects in one frame saw different anchors,
  and the rate-limited slews ran far faster than their deg/s said. Now they
  advance once per frame: at the first eye-1 upload of a real 3D camera
  (|P0|,|P5| > 0.2), on the render thread. Every other call (eye 2,
  `ffxvr_project`, the markers) only reads them. The latches (heading, seat)
  still fire from any caller; they are idempotent within a frame. The anchor is
  a time constant, `k = 1 - exp(-dt * gap / tau)` with the native frame time:
  `anchor_smooth_ms` (field, 0-500) and `battle_anchor_smooth_ms` (0-3000),
  **both 0 = snap by default**, the closest to before, so this build changes
  consistency only (one anchor for every object and both eyes). It snaps after
  more than 2 frames without advancing (screen, stereo screen, backdrop, seat,
  first person, loading), on a 50-unit warp in the field, and at battle start /
  end. Sliders: Debug > First person / follow, under third person. A/B targets:
  24 ms (halves the 30 Hz step at 60 fps, about one frame of lag) and 325 ms.
  The `view:` diag line every 10 s gives frames, ticks, eye-1 camera uploads per
  frame and "uploads with another camera" (if that is ever non-zero in a scene,
  the follow-pans should take their yaw from the frame's main camera).
- **Honest rates.** `tp_follow_rate` (third-person heading follows the run) and
  `follow_rate` (seat follow pans) are now real deg/s, frame-rate independent.
  Defaults unchanged (both off by default; the owner's ini has both off).
  Before, the pans ran on every view build (both eyes of every camera upload,
  the markers, the projections: 20-60x the slider) and the third-person slew on
  every eye-1 build, so at the default 20 deg/s the seat was locked to the
  camera within a frame or two. Now: the pans turn at the slider's real deg/s
  (a 90-degree pan at 20 deg/s takes 4.5 s), the seat pitch (with pitch follow
  > 0) eases over about 0.5 s, and the third-person heading approaches with a
  real 2/3 s time constant under its rate cap. Anyone turning them on should
  start around 120 deg/s. The pan rate slider now goes to 360.
  `ffxvr_anchor_set` (never bound) is replaced by `ffxvr_view_smooth_set`.
  At 0/0 the battle table now jumps to a new centre at once where it used to
  glide over 3-4 frames in small fights (0.05 per call converged 70-95% per
  frame); 325 ms is the A/B candidate for that. The `view:` line prints only
  with the headset on or diagnostics armed, and also counts eye-1 uploads
  before the tick (drawn with last frame's anchor; expected 0 in 3D scenes).
- **Low-resolution warning.** Exclusive fullscreen only gets the modes of the
  display that is active at launch; with the monitor off, Virtual Desktop's
  virtual display offers 1920x1080 and the game renders at that, so the headset
  image is blurry. With the headset on, the backbuffer is compared each frame
  with the eye images' size (the headset's recommended width at 16:9; the raw
  recommended height is a taller-than-16:9 shape that even 4K would fail): below
  it, the overlay shows one line and `ffxvr_diag.log` gets a `resolution:` line
  once per change. Two causes, and the line names the likely one: at 1920x1080
  or less, the monitor off at launch (or the game set that low); above it, the
  game's resolution below a high VD quality preset (2560x1440 from
  `game-settings.ps1 -Profile vr` can trip it at the highest presets), so raise the
  game's resolution or lower the preset. INSTALL-ALPHA and the zip README have a
  troubleshooting row for both. The flag is cleared when the headset goes off.
- ABI 3 (`ffxvr_view_smooth_set`, `ffxvr_lowres_status`; `ffxvr_anchor_set` gone).

Owner feedback on builds 1-4 (24 Oct 2026): stick-turn ghosting is much improved,
most likely from the correct turn timestep (build 1's native frame time, the same
with the overlay closed) and steadier frame pacing (build 2 removed the blocking
per-frame GPU readback). Head-movement-only motion is very clear. In first person,
`fp_head_smooth` 0.35 vs 1.0 and the new anchor smoothing made no visible
difference. Defaults stay as they are.

## Review fixes, build 5 (24 Oct 2026)

OpenXR robustness (review items R8a, R15, R41, the watchdog wake, xrEndSession).

- **Swapchain images (R8a).** `submit_eye` acquired, waited 250 ms and, on a
  timeout, released an image it had not waited for (invalid by the spec), then
  repeated the 250 ms wait every frame of a Virtual Desktop stall (the monitor
  crawled). Now each swapchain is a small state machine: acquire if idle, wait if
  acquired, and only a waited image is written and released. A wait that times
  out leaves the image acquired; the next frame retries with a 2 ms timeout. The
  base layer's swapchains are prepared as a set (both eyes, or screen + right
  screen) and only released together, so a layer never pairs a new left eye with
  an old right one; once one member of a set has failed in a frame, the others'
  first waits use the short timeout too (one 250 ms wait per stalled frame, not
  one per swapchain). Only a timeout or an error is retried; other success codes
  (SESSION_LOSS_PENDING) count as waited. Log: every timeout until 5 have
  happened since launch; after that, try 1 of each episode per swapchain and
  every 300th try; plus "recovered after N tries". On failure the layer is
  dropped as before.
- **No eye image (R15).** xr_tick returned early when the frame had no eye image
  (e.g. the backbuffer copy failed), skipping xrEndFrame / xrWaitFrame for that
  Present. Now xr_frame always runs. As an interim part of R8b (holding the last
  frame on timeouts too comes later), the last base layer submitted with fresh
  images is kept and re-sent for up to 30 frames when a frame has no eye image
  (the swapchains still hold those images), then zero layers (black). Logged as
  "no eye image this frame: holding the last one" / "fresh frames again after N
  held". Swapchain timeouts still drop the layer (black) as before.
- **Lost position tracking (R41).** With the headset cameras covered, the views
  came without a valid position and both eyes were put at the origin (zero eye
  separation, the world jumping). Now the last valid head centre is held and the
  eyes sit at +-IPD/2 along each view's own right axis, with the IPD measured
  while tracking was good (0.04-0.09 m, default 0.063). The held position resets
  on xr_stop and when the runtime recentres its space. One diag line when it is
  lost, one when it comes back.
- **Watchdog wake.** The stall watchdog slept 1 s per loop, so xr_stop could
  block up to a second (headset off, game exit). It now waits on an auto-reset
  event that xr_stop signals.
- **xrEndSession only in STOPPING.** xr_stop called it in any running state
  (invalid outside STOPPING); poll_events already ends the session on STOPPING,
  and destroying a running session is allowed.
- **Test hooks** (Debug > Headset tests, session only): "drop the eye image every
  frame" and "simulate 20 / 60 swapchain wait timeouts". While the drop toggle is
  on, the main window shows a red "TEST ON" line (it stays on when Debug is
  collapsed). The heartbeat counts held frames and wait timeouts. ABI 4
  (`ffxvr_xr_debug`).
- Small ones from the review: the hold is cleared when the runtime stops the
  session and its episode counter on xr_stop; the watchdog's handles are closed
  only once the thread has exited.
- **Done in R8b (branch `night`, see "R8b" below):** hold the last frame on swapchain timeouts
  too (and the UI / ImGui quads); escalate a swapchain whose wait keeps failing
  with a hard error (not a timeout): after ~300 consecutive failures (~5 s) set
  the status "swapchain N stuck" and xr_stop, so build 1's back-off restarts the
  session (today it retries forever with the headset black).

## Review fixes, build 6 (24 Oct 2026)

Eye-2 twin housekeeping (R12) and cursor hiding over several ranges (R39 + R48).
R11 (real bindings in eye2_begin/end, the backbuffer sentinel) is build 7, on its own.

- **Twin SRV cache (R12).** A twin's SRV cache that reached 256 entries was
  emptied inside `twin_srv`, in the middle of a draw: a view bound to an earlier
  slot of the same draw, or the eye preview's `srvs[nullptr]` that ImGui still
  holds, could be freed. Now `twin_srv` only flags it and keeps adding;
  `twin_srv_trim()` runs at the frame boundary (begin_frame, after
  `sweep_caches`) and empties the caches of 256+ entries except the `nullptr`
  key. The heartbeat's eye-2 line shows "twin srvs dropped".
- **Failed twins (R12).** A twin that failed to create was remembered with no
  views and never retried for the session. Now the record keeps the description
  it failed for: the same texture is retried after 600 frames, the back-off
  doubling per repeated failure up to 36000; a different texture at the same
  address is tried at once. Failed records don't refresh `last_used`, so the
  sweep drops them (and with them the back-off) after the idle time. The
  "twin FAILED" diag is capped at 20 lines and gives the retry delay. `fail()`
  now also releases a view it created (rtv / dsv) before the texture.
- **Missing depth twin (R12).** When the game has a depth target and it has no
  twin, eye 2 draws without depth, as before; it is now counted ("no depth twin"
  on the heartbeat's eye-2 line). No behaviour change.
- **Cursor hiding, several ranges (R39).** A batched UI draw could hide one
  index range only: two hidden quads with visible ones between them (two target
  pointers with a name or damage digits between) hid everything between them,
  and the pointer rule did not run once the hide list matched. Now one shared
  read per draw (up to 32 quads, was 12) feeds the hover highlight, the hide
  list and the pointer rule; a quad is hidden when either matches, each run of
  hidden quads becomes a skipped range (up to 8; more runs stay visible and are
  counted), and the managed side draws the gaps between them, for eye 1 and the
  eye-2 re-issues alike. All quads hidden still skips the draw. A single
  pointer in a batch gives the same one range as before. ABI 5:
  `ffxvr_draw_skip_ranges`; `ffxvr_draw_skip` stays as a shim that returns the
  envelope (first range start to last range end).
- **Base vertex and start index (R48).** The quad readers read at the bound
  offset only, ignoring the draw's base vertex and start index (the cursor-art
  capture applied the base). Now every reader starts at the base vertex, plus
  quad `start / 6` when the start index is a multiple of 6; any other start (or
  a negative base) is not read, so no hiding for that draw.
- **Counters** on the `perf:` line (since the previous one): `ui verts read`,
  `truncated` (batches of a hidden kind with more than 32 quads: the quads past
  32 are never hidden; raise the constant to 64 if this shows), `start!=0` (and
  `not read`), `base!=0` (draws R48 now reads at a different place) and
  `skip overflow`.
- Review fixes: the 32-bit quad mask is pinned by a `static_assert` (64 quads
  needs a 64-bit mask); a run past the 8th is taken out of the hidden set, so
  the game's cursor art is not captured for a pointer that stays visible (the
  hit counters still count it; `skip overflow` flags it); zero-index draws are
  not examined; "no depth twin" counts only draws that got an eye-2 pass; the
  F9 pick dumps streams 1-5 at the picked quad's own vertices (base vertex
  included), like stream 0.

## Build 7: battle-cursor and engine diagnostics (log only)

This is stage C1 of the battle-cursor plan, plus the section-6 diagnostics from the 24 Oct research round
(research SYNTHESIS.md; exe-verify.md's corrections apply). Nothing visible changes. Every hooked game
function is always called, and the old cursor path (hide list, pointer rule, markers) is untouched.

All of it hangs off one ini switch, `diag_build7` (default 1; **default 0 since the loads build**, and the game-function hooks now follow the features that use them: see "Loads" at the end). With `diag_build7=0` and a restart (before the loads build):
- no game function is hooked;
- XInputSetState is not hooked;
- the stall watchdog does not run;
- no build-7 diagnostic runs. Two things stay: the heartbeat still shows its state ("off"), and the F9 pick line still has the texture format.

An edit while the game runs turns only the logging on or off. The hooks are only ever installed at launch.

**Managed (diag7.cs, a partial of the module):** four game-function detours, queued at module `init()`.
Fahrenheit applies them after every module has initialised, and refuses a late hook on a function that is
already hooked.
- **Target pointer**, FhUtil 0x49C870, cdecl void().
- **Turn pointer**, 0x492590, cdecl void(int chr). It has its own delegate: Fahrenheit's generated handle has
  no argument.
- **Job-join wrapper**, 0x644B30, cdecl void(). Not Sg_MainLoop's entry: FFX-60fps checks that prologue.
- **Backbuffer re-acquire**, 0x199210, thiscall void(obj, w, h).

What the detours do:
- They run on the game's main thread. Each calls the original first.
- Each copies values into a two-slot seqlock. No strings, no logging, no D3D.
- Target snapshot: the game frame counter (FhUtil 0x1FCBBF0), the candidate mask 0xF3D178, the gates, and
  the windows whose draw callback is the target stub. Per target:
  - the anchor exactly as 0x8354F0 computes it, with the fallback used;
  - the logic and drawn roots, heading, attached/override flags;
  - Chr+0xF34/F38.
- Turn snapshot: the chr argument against 0x1FCC088, θ, both colours, and optpos[1].
- Post-join snapshot of the followed actor: the first join after the frame counter moves is Sg_MainLoop's
  post-join call. It records the drawn root, logic position, offset and exec byte. The exec byte is at VA
  0xC496D4 = **FhUtil 0x8496D4**; SYNTHESIS section 6 prints 0x8C96D4 by mistake.

At Present, managed reads the snapshots and writes these lines to ffxvr_diag.log through a new export:
- `cursor:` on every change and once a second while a cursor is drawn. It includes our projection of the
  anchor through the game's own camera minus the game's own screen anchor.
- `timing (30 s)`: lag histograms, Presents vs post-join snapshots, call counts.
- `cursor elements` once per battle.
- `resize`.
- `pan-scan:` on every map change and every 5 s.

The Motion log (Numpad 5) gets the post-join and Present-time drawn root as extra columns.
Debug > Build 7 has a status line and a "show cursor anchors" toggle. The toggle draws dots on the monitor
only, never in the headset.

**Native:**
- The XInputSetState IAT hook logs motor changes, 20 per battle.
- The loading-stall watchdog fires after 300 ms without a Present, once per stall. While the main thread is
  suspended it only does GetThreadContext and one stack read: no allocation, no lock, no logging, nothing that
  can raise. It logs after ResumeThread, and logs the stall's end at the next Present.
- The backbuffer desc is logged on identification. The prediction is format 28, R8G8B8A8_UNORM.
- The F9 pick line now includes the SRV's texture format.
- Heartbeat lines report the counts.

ABI 6: `ffxvr_diag7_set`, `ffxvr_diag_line`, `ffxvr_main_note`, `ffxvr_motion_cols`.

Review fixes:
- **The four originals are resolved on the render thread**, once, at the top of render_imgui_inner and before
  our own late D3D hook insertions. Fahrenheit's chain table is not locked for readers. If a detour runs before
  the first Present (the join and the resize can), it resolves on its own thread instead:
  - from an absolute address cached at init, with no module lookup;
  - it retries a miss or an exception until Fahrenheit has written the entry;
  - an Interlocked handshake keeps it out of install_draw_detours' insertions.
- The join is queued last.
- The cursor-lag histograms count only Presents that brought a new snapshot (target, turn and join apart).
- rot-err decays to 0 once a logic tick passes with no turn.
- The turn line logs the game's own anchor A (0xF3FA68, written by the original) next to ours. The cyan dot uses it.
- The watchdog:
  - never watches the render thread or itself;
  - re-checks for a Present before suspending;
  - is idle in slow motion;
  - drops an open stall when switched off.
- diag() retries a contended open off the render thread.
- The pan-scan map test compares raw bytes, and map changes bypass the line cap.
- The follow actor is cleared when there is no actor table.

## Build 8: settings file

These are G1-final's settings items: R21, R22 and R27-R33.

### When the file is read
- **At start** (`settings_load`, the only place that sleeps):
  - Order: the path, then the bone labels, then a read retried 10 × 100 ms, then a validated parse, then the launch effects (autostart, start view).
  - The file's mtime, length and raw text are remembered. So the first frame does not reload, and the first write of a session still writes the canonical form and makes `.bak`.
  - If the file parses fewer than 10 known keys while a `.bak` exists, there is a warning in core.log and an orange line in the overlay ("looks truncated").
  - The `.bak` is not loaded automatically, and it is not overwritten that session.
- **While playing** (`settings_reload_check`, once a second of wall time):
  - A hand edit is read back, with no sleep.
  - The mtime and length are marked as seen only after a successful read, so a locked file is retried. A read failure is logged once, and recovery is logged too.
  - Only the keys the edit changed are applied, compared with the file as we last read or wrote it. With nothing unsaved, that is the same as reading the whole file. An overlay change not yet written is kept, and is written together with the edit.
  - File values win, with no launch effects. A new `headset_autostart=0` stays 0. `world_scale` goes to the seat/diorama scale even in first or third person.
  - Whether the overlay is open stays as it is for the session.
  - The file stays as typed, comments included, until a setting really changes.
  - A deleted ini is not recreated until the next change.

### Parsing (R27, R32)
- A bad value keeps the current one, and each load or reload logs one "ignored bad values" line.
- Numbers use the invariant culture and are clamped. The ranges are G1's, with `follow_rate` 1..360 and the anchor smoothing values 0..500 / 0..3000 ms.
- Bools accept 1/0, true/false, yes/no and on/off.
- U+2212 reads as '-', and ints are written in the invariant culture.

### Saving (R21, R29, R30)
- **When it writes:** a snapshot every 0.5 s, written once the text has been the same for 1 s. A slider drag therefore writes once, after the release.
- **How it writes:**
  - Under a lock, because the ExitProcess detour flushes from its own thread first thing.
  - Never over a hand edit: if the file's mtime or length moved since our last read or write, nothing is written and the file is reloaded.
  - Temp file, then `Flush(true)`, then `File.Move(overwrite)`.
- **The `.bak`:** made once a session, only from a startup file that parsed fully (10+ known keys), in its own try. A failed backup never blocks saving.
- **Failures:** a failed write is logged once and retried every 5 s; "saving works again" is logged on recovery. The `.tmp` is removed on failure.

### Per-frame savings (R22)
- `name_is`: one 16-byte read, no allocation. The "not found" result is not cached, as agreed.
- `_in_blitz` is cached on map change, with an ordinal compare.
- The pick result is polled only for 3 s (plus 3 frames) after F9.
- The head-parts mask is cached.

### Timers (R33)
- The ini check runs on `_now`, which is ImGui time.
- The battle-follow waits stay frame-based, and the comment now says why.

Review fixes (build 8):
- **A truncated startup ini is not rewritten at launch.** It stays short until a real change, so the note repeats every session. The `.bak` is copied once to `ffxvr.ini.bak-keep`, which nothing overwrites or loads. The note also shows when native failed.
- **The `.bak` is refreshed** through its own temp file, with ReadOnly cleared, because File.Copy carries the attribute over.
- **Before every save,** the file's content is compared with what we last read or wrote, not only its mtime and length.
- **A reloaded value that had to be ignored or clamped** gets the corrected text written. Clamping is now logged too.
- **The reload body runs under the same lock as saving.** After the exit flush, no other save runs.
- **A failed start-up read is completed by the reload** ("read at last"; the launch options apply from the next start). A file deleted during that read counts as "no file yet".
- **A read-only ini fails fast,** with no temp file or fsync per retry.
- **The exit flush exists only while the D3D/ExitProcess detours are installed,** i.e. when native init succeeded. Without them, a change made in the last ~1.5 s before quitting is lost.

## Build 9: eye-2 bindings (R11)

This is G4-final R11 (a)-(c) and the R42 sub-item, with the research updates from SYNTHESIS 5.1.

- **(a) Eye 2 uses the real bindings.** `eye2_begin` saves the views the context really has bound, through `OMGetRenderTargets(8)` and `PSGetShaderResources(0, highest slot the game binds)`.
  - Each saved view is AddRef'd, so it stays alive until we release it.
  - The twins are derived from those views, and `eye2_end` restores the same views.
  - The tracked `g_cur_rtv` / `g_cur_ps_srv` raw pointers are now only keys for comparisons; they are never dereferenced. Counters show how often they differ from the real bindings.
  - `g_in_eye2` is set before the twin bind and cleared only after the restore, because the restore passes our own `OMSetRenderTargetsAndUAVs` / `PSSetShaderResources` detours.
  - Restores take up to 8 targets and keep the UAVs (R42). That covers eye 2, the copy pass and both marker passes.
- **(b) The backbuffer has a destruction sentinel.**
  - At identification, an IUnknown is attached to the backbuffer texture with `SetPrivateDataInterface`. It holds no reference on the texture.
  - When the texture is destroyed, the sentinel's last Release nulls `g_backbuffer`, with an interlocked compare-exchange, and queues the dead texture's twin for release two frames later.
  - So a non-null `g_backbuffer` is alive, and the per-tick GetDesc stays (it also refreshes the cached size and format).
  - Per the research, the game re-acquires its backbuffer on the main thread, in 0x599210, after flushing the render thread. Our OMSetRenderTargets hook therefore identifies the new backbuffer, and attaches its sentinel, on the main thread while the render thread is idle. Nothing in that path relies on being on the render thread.
- **The 0x599210 hook is a second, hard signal.** Build 7's hook on 0x599210 now calls `ffxvr_bb_resized(w, h)` before the original, using interlocked stores only.
  - At the next begin_frame, a backbuffer identified before that call is dropped, in case the sentinel did not fire. It is re-identified at the next bind.
  - With `diag_build7=0` the hook does not exist, and the sentinel alone covers this.
- **The backbuffer format is 28 (R8G8B8A8_UNORM).** It is cached alongside the size. No colour path changes.
- **(c) Bind-time caches.** RTV0's texture, size and sample count are taken when the view is bound (`g_cur_rtv0_*`), and on_draw / clear_twin use them.
  - The UI and census paths read slot textures through `ps_tex(slot)`, which asks the context.
  - The history-draw aspect test uses the cached backbuffer size.
- **ABI 7:** `ffxvr_bb_resized`.

Review fixes (build 9):
- The D3D method is `OMSetRenderTargetsAndUnorderedAccessViews`.
- 0x599210 is reported in two phases: before the call and after it. Only a completed re-acquire is acted on, and a backbuffer identified inside the call carries the new count. Acting before the call would have dropped the still-live backbuffer at the last pre-flush Present.
- Backbuffer twins are flagged at creation. Two frames after any sentinel firing or completed re-acquire, every flagged twin that isn't the current backbuffer's is released, and so is the current one's if its size or format changed (address reuse). There is no single dead-pointer slot any more.
- The UI-branch, copy-pass and marker-pass binds now also keep UAVs. So R42 ("restores keep the UAVs") holds on every path.
  - The caveat: with UAVs kept, an eye-2 re-issue would also write any game PS UAV, which has no twin. The game isn't believed to use PS UAVs.
- A failed sentinel attach is logged once. Without the 0x599210 hook (diag_build7=0), such a backbuffer is guarded only as in build 8.
- ABI 8: `ffxvr_bb_resized(w, h, phase)`.

## Cursor stage C2 (branch `cursors`, from build 7)

This is SYNTHESIS stage C2 (sections 2.4-2.7 and 3). It is based on the owner's build-7 data:
- target anchors match within 1 px;
- the turn anchor matches the game's own exactly;
- the snapshot lags a steady 1 game frame;
- the 30 Hz correction is 0 in battle;
- every cursor element is index 1.

What changes:
- **Our markers come from the game's own cursor draws** (`cursor_source=game`, the default). The two build-7 detours record a snapshot every time the game draws its target hand or turn triangle. The game only calls those functions under its own gates (a party member choosing a command, the UI alive, the battle state, not auto-battle), and the target detour mirrors the auto-battle early return.
  - **Target markers** (red, as before): every highlighted candidate in the game's mask, at `optpos[Chr+0xDF9]` with the correction kept (≈0), lifted 2 units (the game's own turn-pointer lift; slider "pointer lift above the game's anchor").
  - **Turn marker** (as before): at the game's triangle apex, `optpos[1]` − 2.3 (A − 0.3).
  - **Expiry:** a snapshot counts for 3 Presents. When the game stops drawing a cursor (confirm, enemy turn, auto-battle), our marker goes within a few frames.
- **In the headset, the game's cursors are skipped at the source.**
  - Native decides once per frame at begin_frame and publishes the result interlocked; the detours ask `ffxvr_cursor_skip()` on the main thread.
  - The condition: game source live, headset on, the session up, markers in 3D, the last frame had the UI pass, and neither the virtual screen nor the stereo screen showing.
  - On the monitor, or on screen frames, the originals run as before.
  - Both functions only draw (exe-verify), so skipping them loses nothing else.
- **With the game source:**
  - The hide list is pushed without its two default cursor entries, and the pointer rule is off. The code stays, for the legacy source and for stage C4.
  - The flat ImGui triangles are not drawn.
- **The markers_game_art experiment is retired:** it is always pushed off, its toggle is gone, and the ini key is kept but ignored. It drew the captured sprite with wrong UVs ("rotated ~90°"). Stage C3 brings the game's look back from battle.dds.
- **Fallback to legacy:** with `diag_build7=0` at launch the hooks don't exist, so the game source falls back to legacy and the Battle markers status says why. The same happens if the hooks were queued but the turn detour is never called while the game's turn-pointer gates are open for 3 s.
- **ABI 7 on this branch:** `ffxvr_cursor_game`, `ffxvr_cursor_skip`. Master's build 9 is ABI 8, so this needs renumbering at merge.

### Review fixes, cursor stage C2

These are the fixes from the C2 review. Where they differ from the section above, they replace it.
- **Expiry by game frames.** A snapshot counts while its stamp is 0-2 game frames behind the frame counter (the measured lag is a steady 1). A negative lag does not count.
  - It must also not be older than the frame the game source and markers were last (re)enabled, the current battle's first frame, or (for targets) the first frame of the current turn (the turn snapshot's chr changing).
  - Toggles and source flips therefore never flash an old snapshot.
  - A main-thread hitch stops the counter, so the markers stay rather than blink. A Present backstop (180 Presents, ~2 s at 90 Hz) only matters if the counter stops while nothing else changes.
- **Dead-hook detection, per hook, cumulative.**
  - The turn hook counts as dead after 60 Presents in all with its gates open and no call.
  - The target hook counts as dead after 60 Presents with a target-stub window open (state ≥ 3) and no call.
  - The counts do not reset when the gates close, and the call counters are now kept in the detours, whatever the recording state.
  - Both hooks must have been queued (`one()` said "queued").
  - **One hook missing or dead puts both cursors on legacy.** Mixing sources would leave the missing cursor unmarked, or marked twice, and unhidden. One consistent source is the safer choice. The status names the hook.
- **The acting member as target.** The red marker is stacked above the turn marker (5 × scale + 0.5 units higher), so both are visible.
- **The skip predicate is sticky.**
  - It requires the UI panel on (`g_ui_enabled`) and no screen frame.
  - A 3D frame without the UI pass lets go only after 2 such frames in a row. This closes the first-frame windows at headset start and after a virtual- or stereo-screen frame.
  - The detours latch the skip per game frame, so the target and turn detours of one frame agree.
  - **Not closed:** the main thread builds a frame about one ahead of the decision. A mode switch can therefore show one frame with the game's cursor and no hiding, or one with no cursor. It is rare in battle (the first command input comes long after the intro) and harmless.
- **Overlay.** With the game source:
  - the pointer-rule checkbox is greyed and says it is off;
  - the hide-list rows show the hits of their own pushed slot, and the two default rows read "(off: game cursor source)";
  - a note says the game's cursors are skipped at the source, and that only the legacy checkbox brings them back.
- **Settings.** `marker_lift` is saved (−5..20, default 2).
- **Accepted deviations from SYNTHESIS stage C2,** for C3/C4:
  - no `marker_anchor_smooth`, no `marker_anchor=game|top`, and no explicit kind tag. The owner's data (correction ≈ 0 in battle, every element index 1, colours unchanged) makes them unnecessary.
  - `hide_cursor` stays on with the user's entries, instead of being pushed 0, so picks keep working.

## Cursor stage C3 + first-person markers (branch `cursors`)

The owner asked for the game's own cursor look in the headset ("they look better"), and for the yellow turn marker to stop sitting in his face in first person.

### The game's look

`cursor_look=game` is the default; `mod` gives our red / yellow triangles. It works only with the game cursor source; the legacy source always draws our triangles.
- **The turn pointer** is the game's own triangle, as 0x892590 builds it:
  - world-space vertices `A + (sin θ·r, dy, cos θ·r)`, with (r, dy) = (0, −0.3), (−1, −2.3), (+1, −2.3) in game units and y down. It is not screen-facing: it spins about the vertical axis by θ (`[0x133FA7C]`).
  - The apex is drawn in `[0x133FA88]` and the base in `[0x133F7D4]`, with the gradient between them. Both are RGBA bytes where 0x80 = full; alpha 0 is read as opaque.
  - θ and both colours come from the turn snapshot, so θ steps at the game's 30 Hz like the game's own.
  - It is the game's exact size at "pointer size" 1.
- **The target hand** is a quad from battle.dds:
  - party targets (chr < 0x14, type 4): texels (566,523)-(634,568), placed x −100..−32, y −22..+23 UI px from the anchor, so it sits left of the anchor and points right;
  - monsters: (566,463)-(634,508), mirrored, placed x 0..68, so it sits right of the anchor;
  - this is 0x8C0670's placement. The anchor is the game's own (optpos[Chr+0xDF9]), not lifted.
  - World size is 14 game UI px per unit. The turn triangle's 2 units show as 24-34 px in the default battle view (the old hide-list signature), so the hand is 4.9 × 3.2 units. "Hand size" scales it.
  - It faces the head: both eyes use eye 1's right and up axes, so there is no stereo shear. It is sampled trilinear, with the art untinted.
- **Finding battle.dds.** Native looks at UI-pass draws while the game look is on and the atlas is not yet held.
  - The bound texture must be a square power-of-two of 1024 or more. That allows an HD pack, since the UVs are normalised.
  - The draw's slot-3 UV stream (exe-verify 6) must hold a quad whose box equals a rect the exe draws from battle.dds: one of the two target-pointer rects (0x8C0670's constants) or one of the ten damage digits (0x8FB840: (444 + 49·(d%5), 254 + 65·(d/5)), 49 × 65).
  - Each rect is accepted as written or with v flipped. The match records which, and we then sample exactly as the game's stream does (the exe writes (u, 1 − v)).
  - The search reads at most 24 draws × 12 quads per frame. The stream is learned on first sight and readable from its next Map.
- **Getting the first sight.** In the headset the game's hand is skipped, and digits only come with damage.
  - So while the atlas is not held, the target detour lets the game draw its hand for at most 60 Presents per session. We draw no target marker over it during that time.
  - The search normally catches it within a few frames, so the game's hand shows on the panel for a moment at the first target menu.
  - On the monitor, the pointer is drawn anyway.
  - After the 60 Presents, the hand falls back to our red triangle until a damage number lets the search find the atlas.
- **Lifetime.**
  - The SRV is kept with our own reference, so it can never be freed under us.
  - It is released only when a probe finds that the game holds no reference any more and no UI draw has bound it for 600 frames. The search then starts again.
- **Checks.**
  - On a find, a one-time staging readback of mip 0 counts the blocks (BC1/BC3) or pixels (RGBA8) with visible alpha in both hand rects, both ways up. It goes to ffxvr_diag.log together with the atlas description and how it was found.
  - "Dump the battle atlas" writes battle_atlas.dds through dump_dds.
  - The heartbeat has a "cursor stage C3:" line.
  - A debug checkbox samples the atlas the other way up, in case the first look shows the wrong art.

### First person

`fp_self_marker=low` is the default; the others are `hide` and `normal`. These rules apply in first person (fp_mode 1) with the headset on, to the actor first person looks out of (`_d7_follow_actor`; battle follow puts it on the acting member):
- **The turn marker** on that actor is not drawn (`low`, `hide`).
- **A target marker** on that actor (a self-target):
  - with `low`, it is drawn at a fixed spot in front of us: 0.5 m ahead along the view's horizontal forward and 0.35 m below the eyes. Native computes the spot from eye 1's view, so both eyes agree. It is small: the triangle at half size, or the hand at 0.6 size turned a quarter clockwise, pointing down at the player;
  - with `hide`, it is not drawn.
- `normal` keeps the old behaviour. Third person and the diorama are unchanged.

### ABI and ini

- **ABI 8 on this branch:** + `ffxvr_cursor_look`, `ffxvr_markers_kinds`, `ffxvr_cursor_atlas_dump`. Master's build 9 is also ABI 8, so this needs renumbering at the merge (to 10 with C2's renumber).
- **Marker kinds:** markers_set resets them, so the legacy path keeps our triangles.
- **New ini keys:** `cursor_look`, `cursor_hand_size`, `fp_self_marker`.

### Review fixes, cursor stage C3

These are the fixes from the C3 review. Where they differ from the section above, they replace it.
- **The game's hand is always skipped in the headset.** The discovery let-through is gone.
  - battle.dds is found from the CTB panel's own copy of the pointer. 0x8A0590 draws the same 0x903EE0(400, …) rect for every targeted row (0x8A0A30..0x8A0B5C) and is called from 0x89B360 directly, so our detour never skips it. The find therefore normally comes at the first targeting frame.
  - Until then there is a short wait (at most 6 Presents of a live target per session, headset on) with no target marker, then our red triangle.
  - The find line logs the matched quad's UI size (68x45 for the hand, smaller for the CTB copy).
- **Native's atlas state is asked before the markers are built** (`c3_look`), so the first frame after a find already shows the hand.
- **The search runs only in battle,** with the game source and the markers on.
- **No release.** Once the held atlas has not been bound for 600 frames (stale), the search runs again and a new find swaps it: AddRef the new one, then Release the old one, so the art is never missing. A hand kind without an atlas draws nothing; it is never a triangle at the unlifted anchor.
- **The readback check runs once per session,** at the first find. It is one staging copy of both hand rects, both ways up (x 566..634, rows 456..568), mapped with DO_NOT_WAIT on later frames, so there is no stall.
- **The atlas sampler stops at mip 2,** so neighbouring sprites do not bleed into the hand at a distance.
- **The hand is pulled toward the head** by a tenth of the distance (at most 30 units), along the line of sight. An anchor inside a body no longer puts the hand behind the surface it is drawn over.
- **The self point** uses the midpoint of the two eye positions.
- **C2-7 stacking** uses the game triangle's 2 × scale height in the game look.
- **Comment fixes:** the BC1 alpha test in the log, and the wording of who "the actor we look out of" is.

## Smooth follow anchor (branch `cursors`)

The owner's top priority after C3 is smoothness. His perf lines show steady 16.7 ms frames and 11-13 ms of GPU, and he still sees ghosting when moving with the sticks.

**Cause** (SYNTHESIS 4.1, exe-engine 1.3, exe-verify 8):
- The eye anchor came from the followed actor's +0x0C, the 30 Hz logic position, read at Present.
- FFX-60fps draws the world, and the body, from the root matrix +0x1D0, interpolated at 60 Hz. Its translation is +0x200 = +0x0C + chr_offset +0x6C, lerped between ticks, and restored at the next invocation's pre().
- So the eye stepped every other frame while the world glided, which ghosts under Virtual Desktop's frame smoothing.
- +0x1D0 cannot be read at Present: the main thread may already be in the next invocation, before or after pre(), or the job may be writing it.

**Fix: snapshot on the main thread, use it at Present.**
- **Snapshot.** Build 7's job-join post-hook (FhUtil 0x644B30) now records the snapshot whenever `smooth_anchor` is on, not only with `diag_build7`. The first join after the frame counter moves is Sg_MainLoop's post-join call. The record, written through the existing D7Join seqlock, holds:
  - the followed actor's +0x0C, +0x6C and +0x200;
  - its head sub-position, chosen by push_player's own rule;
  - the attached (+0x19C) and override-matrix (+0x10C) flags;
  - the actor pointer and the game frame stamp.
- **Use (push_player, render thread).**
  - With d = drawn − (logic + chr_offset), the anchor becomes logic + d (= drawn − chr_offset, so the anchor keeps its old meaning) and the head becomes head + d. Both move by the interpolation FFX-60fps applied to the body.
  - The snapshot is used only when it is fresh (0..2 game frames old) and of the actor being followed, and the actor is neither attached nor driven by a script matrix. Otherwise the anchor is +0x0C exactly as before.
  - Native is unchanged. The 50-unit warp rule, the head smoothing and the third-person and diorama anchor filters all see the smoothed positions. First person, third person and the diorama all go through ffxvr_player_set.
  - The battle-centre override for the diorama (combatant centroid, heavily filtered) still uses +0x0C.
- **Toggle.** `smooth_anchor=1` is the default, with a checkbox and a status line in First person / follow. With `diag_build7=0` at launch the hook does not exist, so the anchor stays on +0x0C and the status says so.
- **Motion log (Numpad 5).** push_player now pushes the managed columns every frame (same export): build 7's 12 floats (drawn, present drawn, logic, exec, age, valid), plus "smooth anchor USED/off (age N) step used X step logic Y", the per-Present step of the anchor used and of +0x0C.
  - Expected while walking: the step used is about the same every frame, while the logic step alternates between about 2× and 0.
  - The line's "actor" column is the anchor native received.

**Yaw.** No part of the first-person heading follows 30 Hz data: it is the latched heading plus stick turns plus the headset.
- Third-person follow and the view reset use ffxvr_player_set's move direction. That direction now comes from the smoothed positions, at 60 Hz instead of every other frame.
- `fp_head_anchor=2`'s forward offset follows the head lean, which is already filtered exponentially (fp_head_smooth).
- `g_fp_step` (the hide radius margin, "moving" for third-person follow) is now the per-frame step, about half the old per-tick one, so the hide margin shrinks with it. The body is now where the eye anchor is.

**ABI.** No export change (still 8 on the branch).

### Review fixes, smooth follow anchor

- **Plausibility check.** A snapshot whose interpolation offset d is longer than 16 units is refused, and the anchor uses +0x0C. An actor placed after Ch_CalcMain has a correct +0x0C but a stale +0x1D0 until the next tick. The overlay counts the refusals.
- **Body-hide margin.** The three body-hide checks use `g_fp_step_hide`, the sum of the last two per-call distances (warps count as 0). That keeps the old margin in both modes: at 30 Hz it is tick + 0, with the smooth anchor half + half. `g_fp_step` is unchanged for third-person follow and the hand calibration.
- **Pairing column.** The Motion log adds "stamp step": the stamp difference between the snapshots used at consecutive Presents. 1 on every line means each Present got the next invocation; 0 or 2 means a repeated or skipped snapshot (a pairing slip).
- **Head.** A used snapshot without a head sends no head; it never sends a Present-time head with the snapshot's position.
- **Status.**
  - With no followed actor, the status reads "no followed actor".
  - The status strings are literals, with the age shown separately.
  - The managed Motion-log columns now pair with the same line's "actor" column.
- **Smoothing-off hint.** After about 60 moving logic frames in a row with d = 0 while the snapshot is used, the status adds "the drawn root equals the logic position: is FFX-60fps smoothing on (F7)?".

## Merge `night`: cursors + master (builds 8, 9) + scripts-pass

- **ABI 10 on both sides.** Master's 7 and 8 (`ffxvr_bb_resized`, then its phase argument) are kept, followed by the branch's C2 exports (9: `ffxvr_cursor_game`, `ffxvr_cursor_skip`) and C3 exports (10: `ffxvr_cursor_look`, `ffxvr_markers_kinds`, `ffxvr_cursor_atlas_dump`). `clear_native_fns` nulls all of them.
- **Settings.** Build 8's validated parser takes the branch's keys:
  - `smooth_anchor` (bool);
  - `cursor_source` (game|legacy) and `cursor_look` (game|mod), through a new two-word helper `pw` (anything else is listed as a bad value and the current value kept);
  - `marker_lift` (-5..20) and `cursor_hand_size` (0.3..3), clamped;
  - `fp_self_marker` (low|hide|normal).

  All of them are written by `ini_text`, so build 8's reload of changed keys only, its write-back of corrected text, and its truncation count cover them like any other key.
- **R11.**
  - The C3 battle.dds search takes the SRV really bound at slot 0 from the context (PSGetShaderResources, whose reference becomes ours on a find), and checks liveness with `ps_tex(0)`. It no longer reads the tracked raw `g_cur_ps_srv[0]`.
  - The marker pass merged onto R11's 8-target save and `om_restore_keep`, with UAVs kept.
  - The pass's own PS SRV0 / sampler0 save and restore are real bindings (Get / Set) under InEye2, so `on_ps_srvs` ignores them.

## R8b: hold the last frame on timeouts too; escalate a stuck swapchain (branch `night`)

G4-final R8b, on top of R8a (build 5) and R15's interim hold.

- **The held base layer** is now re-sent whenever the frame's base layer cannot go out fresh, for at most 30 frames, then zero layers (black) as before. The causes are a missing eye image (R15), a swapchain wait that timed out (R8a's all-or-nothing set leaves the partner SC_WAITED), or no located views.
  - The conditions are unchanged: `shouldRender`, every swapchain the held layer references has been released at least once, and no space change, stop or loss since. Those events clear the held state.
  - Re-sending a layer while a newer image of the same swapchain is acquired or waited but not yet released is legal: the compositor uses the last released image, and the held poses are the ones those images were rendered with.
- **The UI panel and the ImGui layer** each keep their last fresh quad, with their own age.
  - **UI panel:** held when its own image could not go out (a timeout on swapchain 3), or when the base is held (a no-image frame has no UI image either). It is never held in the screen modes, nor when a fresh frame legitimately has no UI.
  - **ImGui layer:** held only when its image exists (the overlay is drawing) but its own wait timed out.
  - Both go out only when a base layer does.
- **B5-2.**
  - A swapchain whose acquire or wait fails with a hard error (XR_FAILED, not a timeout) 300 times in a row (about 5 s) is escalated: after that frame's xrEndFrame, xr_frame logs "swapchain N stuck", calls xr_stop and sets the status "swapchain N stuck: restarting".
  - xr_tick then sees the session gone (leave_xr_state), and the managed side's next ffxvr_xr_set(1) restarts it through the build 1 start back-off.
  - A timeout or a success resets the count.
- **Not used:** exe-engine's FMV / loading flag ([0xCDED3C]+0x6D0/+0x6D2). The hold is limited to 30 frames, so an FMV or a loading screen that keeps the image away longer still goes black. Telling the two apart would only matter for a longer hold, which this build does not add.
- **No ABI change.** The heartbeat's held count now covers every hold cause.

## Battle load diagnostics + model cache option (branch `night`)

Research: battle-load.md 2.1, battle-data-load.md (with its section 7 correction), and exe-verify-round2 Part C. The verifier's corrections override the research.

- **`diag_load=1`** (default; hooks at the next launch): log-only detours, queued at init before build 7's job join (which stays last in Fahrenheit's commit). **Since the loads build** `diag_load` defaults to 0 and only adds the expensive parts of the lines; the hooks follow `diag_load_lines`, `load_lock_release` and `hd_cluster_cache` (see "Loads" at the end).
  - **The hooks** (FhUtil offsets): teardown 0x381590, map load 0x509A20, map free 0x509EA0, map build 0x508000, chr flush 0x440030, the loader wait-all 0x36C490, chr model 0x429EF0, chr motion 0x42A4C0, loadPCluster 0x29B940 (thiscall, chained with Fahrenheit's file-loader hook).
  - **Resolution** follows B7-1's pattern: `ld_resolve` on the render thread before our late D3D hooks, and `d7_resolve_main` on the detour's own thread before that.
  - **Return values:** every detour returns the original's eax.
  - **Not hooked:** MsBattleExe 0x381020 and the return 0x461160. FFX-60fps's QOL-RETRY sig-scans both prologues. Sg_MainLoop is not touched.
  - **The battle start** is seen in the wait detour. MsBattleExe (mode 0, the random-battle call) calls the wait-all before it clears the request byte [0x112A8E2] (1 random, 2 script request), so the detour knows the battle kind and takes the per-battle baseline there.
- **Timing.**
  - Cumulative counters (Interlocked) are snapshotted on the main thread at the battle start and the teardown.
  - Battle states are polled at Present (main-thread polling would miss them: Sg_MainLoop skips its tail while the loader is busy).
  - Two summary lines per battle in ffxvr_diag.log, "load: battle N (...)" and "load: battle N way back". They give the phases, the HD cluster loads (count, ms, hits < 0.1 ms), the model and motion loads, the blocking waits with their callers, the chr cache entries in use, the game pool's reserved/committed counters (FhUtil 0x153CD44/48), and the address space (free and largest block, overall and below 2 GB).
  - **Wait callers** come from a new native stack walk (`ffxvr_game_callers`: the first return addresses inside FFX.exe, as Ghidra VAs). The wait has about 25 call sites, including the chr wrapper 0x82A5E0.
  - **Address space** comes from a VirtualQuery walk (`ffxvr_va_stats`) once per battle start and end.
- **`battle_cache_models=0`** (default; applies at once; **retired in the HD cluster cache build**: the key is read and ignored). The decision is taken only at the flush call inside the map free (0x909F93), never at another time. The flush is skipped when all of these hold, otherwise the game's flush runs:
  1. the map load is a random battle's own load (battle_state **9** at the call: the state-8 code writes 9 before it calls the map load; kind 1, map = Btl.battlefield & 0x3FF), or the first map load after that battle's teardown is the very field map that was current at its start;
  2. at most 60 (way in: the battle then adds its own models) / 80 (way back) of the 100 cache entries are in use;
  3. the largest free block below 2 GB is at least 256 MB (the last battle start or end measurement; not measured means no skip).

  Why the bound: model and motion buffers come from the game pool, which never evicts, and the table-full eviction 0x8409A0 leaks the HD character (verifier B6-B8). So the cache must never fill.

  Area changes, script battles, save loads and the title never match, so they always flush. The extra memory is bounded by about 80 entries plus one battle's models, and Fahrenheit grows the game pool bottom-up below 2 GB.
- **ABI 11:** + `ffxvr_game_callers`, `ffxvr_va_stats`.

## Rumble to Quest haptics (branch `night`)

Research: vr-opportunities.md item 2.
- **Capture.** The XInputSetState IAT hook (build 7) is installed when `diag_build7` or `haptics` is on (also later, if haptics are switched on mid-session). It records pad 0's motor values with interlocked stores, then always calls the real XInputSetState, so a physical pad still rumbles.
- **Decode.** The PC port writes `round(b * 65535.0)` truncated to 16 bits, so a PS2 motor byte b ≥ 1 arrives as 65536 − b. Values 65282..65535 decode to b = 254..1. Anything else is taken as a linear level.
  - **Fallback:** if no call came for 500 ms, the game's pad block is read (FhUtil 0xF30288: +0x9D the in-game Vibration option, +0xFE large, +0xFF small).
- **Apply** (render thread, in xr_tick after the input sync).
  - The left (large) motor drives the left controller, the right (small) motor the right controller.
  - Amplitude = strength × max(0.15, sqrt(b / 254)).
  - 150 ms pulses are renewed every 100 ms, or at once on a change of more than 0.05. `xrStopHapticFeedback` (now loaded) runs when the motor stops.
- **Quiet when** haptics are off, the strength is 0, the session is not FOCUSED, or a physical pad answers on port 0 while the VR pad is off (then only the pad rumbles).
- **The in-game Vibration option** is respected through the game itself: with it off, no motor values are produced.
- **Settings:** `haptics=1` and `haptics_gain=0.8`, through build 8's parser, with a checkbox, a strength slider and a live status line under the controller rows. The existing "buzz left/right" buttons are unchanged.
- **ABI 12:** + `ffxvr_haptics_set`.
- **Also in this commit:** the optional R8-4 re-review nit. A 3D ↔ screen mode change now drops the held base layer for good.

### Review fixes, battle load diagnostics

- **LD-1: state 9, not 8.** The battle's own map load runs with battle_state 9 (0x7831A1 writes 9 before 0x908600), so the entry rule tests 9 and also requires kind 0x13 / 0x1A. The log's state-8 column is gone.
- **LD-2: lower entry bound.** At most 60 entries in use on the way in (80 on the way back). The table is sampled every 30 Presents during a battle and at live and at the teardown, and the peak is logged. "chr cache TABLE FULL" is logged if 100 is ever seen.
- **LD-3: the battle kind is owned by the main thread.**
  - It is set with the teardown count at the start wait, and is valid only until that battle's teardown, which clears it.
  - The entry rule needs `kind == 1 && kind_tear == tear_seq`.
  - The render thread keeps its own copy for the log and never writes the main thread's.
- **LD-4: the start-wait timer.** The "ended before live" timeout never runs during MsBattleExe's wait, and counts from the wait's end.
- **LD-5: per-battle resets on the main thread.** The way-in values are reset at a start seen in the wait, at the battle's own map request, and at the teardown. The way-back values are reset at the teardown.
- **LD-6: atomic times.** The cross-thread long times use Volatile reads and writes. The render thread keeps a "logged" teardown count instead of zeroing a main-thread field.
- **LD-7: queue order.** The nine hooks are queued after build 7's job join, from init.
- **LD-8: hooks seen working.** A seen-mask bit is set at each detour's first call. The option needs the map load and map free hooks seen working, and the summary lines run with whatever hooks are in. The mask is logged once and shown in the overlay.
- **LD-10:** the stack walk keeps its frame pointer (`#pragma optimize("y", off)`) and captures 24 frames.
- **LD-11 / LD-12 / LD-13:** a comment fix, unused fields removed, and `ld_resolve` runs once with a try per hook.
- **LD-9** (the VirtualQuery walk on the render thread, twice per battle) is left as is for this diagnostic build.

## HD cluster cache (phase 1) (branch `hdcache`)

Research: hd-cluster-cache-spec.md (its section 9, the verifier's answers, overrides), battle-load.md, exe-verify-round2 C4. Code: `hd_cache.cs` (the cache), `load_diag.cs` (the hooks it rides on), native `ffxvr_va_watch` / `ffxvr_va_report`.

- **Mechanism.** The ClusterManager (`[0xCCCA4C]`) caches HD clusters by path hash with a refcount. Holding one extra reference per eligible texture cluster makes the game's own release (`0x671300` → `0x69BE90` at a map free) leave the cluster alive, so the next load of that path is a hit (refcount + 1, no open, read, parse or GPU create).
- **Acquire** (loading thread, inside our loadPCluster detour, right after the game's own call returned):
  - The path filter: ends with `.dds.phyre` and contains `/map/`, `/btlmap/`, `/chr/` or `/magic/`, ASCII case-insensitive. Never `/menu/` (force-unloaded by `0x69B310`), `/shaders/` (never freed anyway) or `/event/`. Phase 2 (scenes, `.dae.phyre` models) is not in this build.
  - The acquire calls the same chain once more with the same path. That is a hit by construction: the entry is in the tree and the caller holds it.
  - A thread-local flag passes that call, and anything re-entering during it, straight through our detour.
  - At most one held reference per cluster: a re-hit only refreshes its stamp, and a CAS flag lets only one acquire run at a time (a second thread skips rather than waits).
  - No allocation, lock or log on that thread. The 400 slots are preallocated arrays (state, pointer, bytes, stamp, path) published with Volatile / Interlocked.
- **Release** (main thread only, in our map free post-hook, right after `0x909EA0` returned):
  1. What `0x671300` does before its own releases: submit the context's partly recorded list (`[[0x8CB9D8]+0xD0]->vtbl[2]`, called only if it points into FFX.exe), then the full render flush `0x5BC3F0`. If the context cannot be read, nothing is released at that map free.
  2. Per slot, an identity check: `fiosUnifyFilename(path)` (what loadPCluster hashed), then `getPClusterByName` at its hooked entry must return our pointer. If it does not, the slot is forgotten and counted.
  3. `releasePCluster` **by pointer** (`0x69BDB0`). It is refcount-aware and frees only if the game holds no reference. Never by name (it hashes its raw argument), never the force unload.
- **Classification** (decided at the map load request `0x909A20`, applied at its nested map free):
  - **ENTRY**: the battle's own map load (battle_state 9, kind 0x13 / 0x1A, map = battlefield & 0x3FF) of a random encounter from the field map we hold (LD-3's kind). Keep everything, admit.
  - **Script battle entry**: keep everything, admit `/magic/` only. Its battle map is usually unique, and on its return the field textures we hold still give a fast way back (spec 3.1, last bullet).
  - **EXIT**: `0x909A20` at battle_state 0 (MAPREG, its only other caller) loading the field map we hold. Keep everything, admit.
  - **ZONE**, anything else: MAPREG to another map, any other `0x909A20`, and every map free outside `0x909A20` (`0x9093B0` / `0x909680` / `0x909E60`: area changes, save loads, the title, events, a soft reset). Release everything, then admit the next field's loads, which also sets the held zone.
  - The spec's MsBattleExe (`0x381020`) and MAPREG task (`0x461650`) hooks are not used. `0x381020` is a QOL-RETRY site of FFX-60fps, so the kind comes from the existing wait-detour logic. The battle_state 0 rule identifies MAPREG.
- **Budget** (at each map free):
  - held ≤ min(64 MB, (reserved − committed + held) / 2) of the game pool; over it, the oldest stamp goes first.
  - No admission while the largest free block below 2 GB is < 160 MB.
  - Release all when it is < 136 MB, or when committed + 32 MB > reserved (a pool growth is near).
  - At most 400 held; the manager has 1024 entry slots.
  - "held" uses each cluster's measured bytes. An unmeasured one (held on a hit, or without the pool hooks) counts at the measured average, or 1 MB.
- **"Hits saved".** At each map free the manager's 1024-entry pool (`+0x1C`, used flags at `+0x601C`) is copied once with a guarded read. A held cluster whose entry has refcount 1 is held only by us. The next hit on it counts as saved. The HDC: line shows those clusters and their MB, which is the cache's real extra memory.
- **`hd_cluster_cache=0`** (default for the first A/B; applies at once). Off: nothing is admitted, and whatever is held is released at the next map free. The diagnostics run either way.
- **Segments** (spec section 4; per thread, one window per outermost loadPCluster, through that cluster's GPU create):
  - **Hooks:** the PStreamReaderFile ctor `0x5766D0` (an open inside the window means a miss), `PStreamFile::read` `0x608090` (ms and bytes), and `PCluster` platform init `0x441620` (the GPU create: real only when +0x44 bit 1 was clear; it closes the window).
  - **`diag_cluster_bytes=1`** (hooks at the next launch): the pool allocator `0x6FB910` / free `0x6FB8B0`, net bytes inside the window (block = `*(raw − 0xC) − 0x10`). Off: the bytes read stand in.
  - Outside a window these detours only read a thread-local int.
  - The five new targets were checked against the disassembly: standard prologues, no branch into the patched bytes, no FFX-60fps signature over them, none hooked by Fahrenheit. They are queued after the core nine, with the pool pair last.
  - **The "load:" lines:** "HD clusters N loads (H hits …; E cacheable paths) T ms; the M misses X ms = open + read (MB) + parse; GPU create G ms (n); the misses kept B MB of the game pool | HD cache on/off: S hits saved (…), held K (MB)".
  - **One "HDC:" line per map free:** case, held / measured / largest / held only by the cache, released and why, cap, admission, manager entries in use, largest free block below 2 GB, pool counters.
- **Low-2 GB reservation log** (spec 3.4; native; with `diag_load`):
  - **Hook:** a 5-byte patch of `ntdll!NtAllocateVirtualMemory`. Its `mov eax, imm32` is replaced by one atomic 8-byte compare-exchange. Anything else there (another hook) and nothing is patched. The patch is never removed.
  - **What it records:** every MEM_RESERVE ≥ 1 MB below 2 GB by this process, as base, size, thread and 12 return addresses, into a lock-free ring. No allocation, lock or module lookup on that path.
  - **The report** (`ffxvr_va_report`) runs on the render thread at each battle start and end and at a zone change. It lists the new reservations with their caller: the first frame outside ntdll, kernelbase, kernel32 and the CRT, as module+offset, plus the frame after it. Then the ones still reserved, summed by caller since launch. Then the low 2 GB in use by region type (private / mapped / image) with the change since the last report.
  - Mapped views (D3DKMT locks, file mappings) do not pass through NtAllocateVirtualMemory: the census covers those.
- **Retired:** `battle_cache_models` (the chr-flush skip): its UI and behaviour are removed, the ini key is read and ignored, and the "would skip" columns are gone. The chr flush hook stays, for timing.
- **ABI 13:** + `ffxvr_va_watch`, `ffxvr_va_report`.

### Review fixes (HD cluster cache)

- **HC-1 (leak):** on a release name-check mismatch, the pointer is looked up in the entry-pool copy taken just before the releases; if present with refcount >= 1 it is released by pointer anyway (counted "by pointer, name check failed"). A pointer truly gone is "forgotten" and admission is disabled for the session. The first mismatch logs the raw path, its fios form, what getByName returned and the pool refcount.
- **HC-2:** `diag_cluster_bytes` now defaults to 0. The resident size comes from the Phyre cluster header read in the read hook (marker 0x50485952, `m_totalDataSize` +0x38 + `m_maxTextureBufferSize` +0x50), so no hot-path allocator hook is needed. The global pool hooks stay opt-in for a cross-check.
- **HC-3:** the game pool's reserved size is remembered at each map free; if it grew while the cache held anything, everything is released and admission is disabled for the session (logged).
- **HC-4:** the DXGI video-memory readout (CurrentUsage / Budget) is in the HDC: line; admission closes above 85 % of the budget; `k_hc_max` is 260.
- **HC-5:** the kick + render flush and the releases run only on the main thread; otherwise the release is put off to the next main-thread map free.
- **HC-6:** the ntdll `NtAllocateVirtualMemory` patch is behind `diag_va_log` (default 0). The default is a patch-free VirtualQuery region diff on the main thread at each map free (`ffxvr_va_regions`): new or grown bases >= 1 MB below 2 GB, with a hint (module for image, mapped file name for mapped, heap signature for private), plus the low-2 GB totals by type and the VRAM use.
- **HC-7:** about a dozen sample loadPCluster paths are logged across the session with their verdict and fios form (every 64th, plus the first rejected `.dds.phyre`). The "GCM" heuristic is gone. `/btlmap/` is removed from the filter (it does not exist; battle maps are under `/map/`).
- **HC-8 / HC-10 / HC-11 / HC-12 / HC-13 / HC-14:** the ring-lap read fix; the trampoline is set execute-only and freed on refusal, and the status prints on each report; the "hooks seen" line builds its expected mask from what was queued; the read counter uses bytes actually read; "cacheable, not yet held"; a put-off release freezes admission until a flush succeeds.
- **ABI 14:** + `ffxvr_va_regions` (on top of 13's `ffxvr_va_watch`, `ffxvr_va_report`). `battle_cache_models` stays retired; `diag_va_log` is the new ini key.

### Re-review fixes (HD cluster cache)

- **HC-R-1:** the region diff's heap-signature probe reads base+8 through `ffxvr_safe_read` (range_readable rejects uncommitted, PAGE_GUARD and PAGE_NOACCESS pages); no reliance on SEH.
- **HC-R-2 / HC-R-4:** the region diff counts every non-free region per allocation base (reserved and committed reported separately) and diffs on reserved bytes; one row per base (consecutive in the walk), the diff is a merge of two sorted tables, and a full table is marked.
- **HC-R-3:** the module-name cache (`va_mod_of`) is appended under an SRW lock; it is used by the render-thread ntdll report and the main-thread region diff, never by the ntdll hook (which only stores return addresses).
- **HC-R-5:** the main thread id is captured once, at the map load request 0x909A20 (never from the wait-all); the render thread id at Present. Releases and the flush never run on the render thread.
- **HC-R-9:** a pool growth while holding no longer ends the session at once: the first releases everything and brakes admission until the next zone change ("HDC: BRAKE"); after the zone change admission re-arms ("brake released"); a second growth while holding disables the cache for the session. The overlay shows on / BRAKED / DISABLED.
- **Nits:** the vram readout is taken at the start of each map free (HC-R-6); the header test uses the bytes actually read (HC-R-7); the orphan figure is labelled "before the releases" (HC-R-8). No export change: ABI stays 14.

### Retune from session 1 (HD cluster cache)

Session 1 (4cfb5a7, kl/klyt0000, 9 battles): stable, no mismatch, no brake, but the cache was idle. The largest free block below 2 GB sat at 135-138 MB against a 160 MB admission gate, and the cap was 1-3 MB because it was half the game pool's slack (~250 of 256 MB committed). A texture cluster leaves almost nothing in the pool after its load (0-6 MB for ~82 misses); its data lives in VRAM (3.4-4.4 of 11.3 GB). The 135 MB hole is the game's own transient 128 MB pool growth block, reserved during each battle load (kernelbase+0x13D130, loader thread) and freed afterwards: reserved alternates 256 / 384 MB at map frees.

- **Two budgets.** The game-pool bytes the held set pins (the pool hooks' net bytes with `diag_cluster_bytes`, else 64 KB per cluster), capped at min(8 MB, half the pool's free part). The VRAM bytes (Phyre header texture + vertex + index sizes, else the file bytes read), capped at min(1 GB, 25 % of the budget), with no admission above 75 % use. The old mixed 64 MB cap and the "committed + 32 MB > reserved" drop rule are gone.
- **Low-2 GB gate.** Admission needs a free block of 129 MB (the game's 128 MB growth + 1 MB), or the transient block is up (reserved above the session's base), when the game already has its room. Drop all only when the transient block is down and the largest block is < 129 MB. The HDC: line shows the largest block now and with nothing held.
- **Brake.** Only a reserved size above the high-water mark seen while the cache held nothing, or a reserved size still above the base once the way back has settled (the render thread's settled check, applied at the next map free), counts as a growth the cache may have caused. The first releases all and pauses admission until the next zone change; the second disables the cache for the session. The cache-off battles at the start of a session teach the mark the normal transient 384 MB.
- **Measurement fixes.** The HDC: line now reports the held set before the releases (session 1's "0 measured" was the post-release count after a release-all). The header fields are captured by stream offset across the window's reads, not only from a first read of >= 0x54 bytes. The "load:" lines print "header sizes for H of M misses".
- No export change (ABI 14).

### Retune review fixes (HD cluster cache)

- **HC-T-1:** no admission while the pool's transient growth block is up. A cluster is admitted only if the reserved counter equals the session base both when its load window opens and at the acquire (both read on the loading thread); the `transient_up` exception to the low-2 GB gate is gone. Skips are counted ("skipped: the pool's transient block was up").
- **HC-T-3:** a growth is reserved > base + 128 MB (the one known transient block), so a launch with `hd_cluster_cache=1` saved does not brake at its first entry. The empty-cache high-water mark is kept for the log only.
- **HC-T-2:** the settled test runs and logs on every way back ("HDC: settled after the way back …"), cache on or off, including the 60 s timeout. It arms after a cache-off way back settles at the base; once armed, a failing way back while holding closes admission at once and the next map free releases and brakes. HC-T-4 with it: an empty way back that settles above the base re-bases.
- **HC-T-5:** each held cluster counts max(measured, 64 KB) of game pool.
- **HC-T-6:** the header VRAM size is rejected (file bytes stand in) when a field exceeds 512 MB or the sum exceeds 4x the file bytes read; counted as "out of bounds".
- **HC-T-7 / HC-T-8:** a brake applied at a zone change ends there; the pool cap is computed with the transient block down and held over block-up frees, and eviction starts only past cap + 1/8.

## Battle transition (glass break) (branch `transition`)

Research: battle-transition.md. Code: native `bt_tick` and friends (`ffxvr_native.cpp`, before `xr_tick`), the capture / shard detection in `ffxvr_on_draw`, `xr_pin` / `xr_hold_uncapped` (`ffxvr_xr.cpp`), managed `battle_transition.cs`.

- **Why it was lost.** The battle start's shatter is the HD port's `BrokenScreenEffect` (singleton `[0xCDEC98]`). It copies the previous frame into `BrokenScreen_capturedFrame` at the top of a renderer frame, then each frame draws a black/fade pass (depth clear, full-screen quad, resolve on the scene target) and two passes of shards that are projected on the CPU (NDC, no game camera) and sample the capture. The fade pass has the exact signature of the game's UI pass, so the UI detector opened the UI pass there:
  - the eye images were copied before any shard was drawn;
  - the fade quad went to the head-locked UI panel (the owner's "small black rectangle");
  - the map free at battle_state 8 left the frames without a camera, so the view flipped to the virtual screen and R8b's held frame was dropped.
- **Phases** (render thread, at the top of `xr_tick`; ReadProcessMemory reads only, no game-code hook):
  - **armed:** `battle_trigger [0x112A8E2] == 1` at battle_state 0, or the transition handle `[0x112C024]`;
  - **shatter:** from the capture draw (the overlay phase's one-target, no-depth draw that is not into the backbuffer), a shard draw (slot 0 samples that target), or the singleton's active byte (+5);
  - **after:** handle 0, +5 clear and no shard draw for 6 Presents;
  - **idle again:** battle_state 1 with a 3D camera.
  - Other uses of the effect (+4/+5 alone, outside an encounter) only get the no-UI-split part. A new window opens only after every signal has been seen clear, so a stuck byte cannot keep the UI split off.
- **`battle_transition`** (ini and overlay, applies at once):
  - **2, frozen world (default).** No UI split, and the view stays in the mode the window started in. The override's head pose is frozen (since BT-H-2: at once in a window opened by `battle_trigger` 1, from the first BrokenScreen signal in a window opened on the handle alone; see "Scripted haze battles" below): `xr_apply_poses`, stick turning and the world grab are skipped. The projection layer is submitted with the poses and FOV of the frame the pin was taken on, so the player's own view breaks apart world-locked where it was. In the after phase the eye image is withheld, so the pinned last frame is re-sent with no 30-frame cap over the game's load black until the battle is live.
  - **1, head-locked.** No UI split and the mode held only.
  - **0, old behaviour.** The phases still run for the diagnostics.
  - A recentre or a pin dropped by the runtime ends only the freeze; the window goes on head-locked.
  - Frames without a camera inside a window are submitted with the headset's FOV, because their image is the capture of a headset-FOV view.
- **Diagnostics.**
  - `diag_transition=1` (default for this test build; default 0 since the loads build) writes one `bt:` line per Present inside a window: phase, states, handle, +4/+5, mode, UI frame, scene draws, camera uploads, depth clears, late draws, capture and shard draws, held frames, eye source and the Present gap. The lines are batched (one file open per ~30 lines) and capped at 900 per window and 6000 per session.
  - The phase changes and one summary per window are always logged and shown in the overlay. The summary gives the phase durations, the battle_state range during the shards and the time of the map free: the race the research could not settle statically.
- **Not in this build.**
  - The render-thread keepalive (vr-opportunities.md 1). Frames where Present does not run at all still go to the runtime's own handling. The summary's longest Present gap shows whether it is needed here.
  - The B+ stereo capture.
  - The trail override during the shards.
- **Also:** the battle start's VirtualQuery walk (load diagnostics, "at the start") now runs on the main thread just before MsBattleExe's wait-all blocks. It used to run on the render thread in the Present that opens the transition. A start not seen in the wait reads it at live, and the opt-in ntdll report (`diag_va_log`) is put off to live while a window is open.
- **ABI 15:** + `ffxvr_bt_set`, `ffxvr_bt_status`.

### Review fixes (battle transition)

- **BT-3, the battle-live release.** The after phase ends on handle 0 and a 3D camera for 2 frames in a row, with battle_state in a running battle state: 1 or 2 (the 1 <-> 2 loop, 0x7917D0) or 0x17..0x21. The load chain 4..0x16 and 0x22 do not count. A battle that starts with an enemy acting (state 2, an ambush) is no longer held until the next state-1 frame or the 30 s backstop.
- **BT-2, the end of the shards.** The shatter ends after 6 quiet Presents in a row: handle 0, +5 clear, no draw sampling the capture. That holds whether or not shards were detected. It used to be "6 frames since the last shard", which was always true without detection, so a mid-flight frame could be held.
  - The capture candidate is tracked even while idle (pointer compares only, tagged with its frame), so a window that opens at the capture frame's own Present adopts it.
  - Only the first match per frame becomes the candidate; a frame with 2+ matches is counted.
- **BT-1, cancel without a battle.** An encounter window with no handle and no effect byte seen is cancelled after 20 Presents with trigger, battle_state and handle all 0. That is the case of an encounter cleared by 0x781530 at a map jump, or MsBattleExe's early return. Before, the 3 s rule left the view frozen for 3 s.
- **BT-4 / BT-5, the log.**
  - A bt: batch that cannot be written (the log is open on another thread, and the render thread never waits) is kept and retried; it is also flushed at headset-off (with the window's summary) and at shutdown.
  - The buffer is 64 KB and is written at phase changes, at the window's end and when 3/4 full: about one file open during the shards.
- **BT-6, the capture and shard rules.**
  - Once per window, the capture target's size and sample count, from the bind-time cache, and the number of overlay matches.
  - Per window, the draws sampling the capture, by case: the shard rule, slot 1-3, with a camera image, without depth. Near misses (probed in the shatter until the frame's first match) count as shard activity for the phases.
  - The session's first shatter arms one frame census. Since the `dream` branch it goes to its own file, `ffxvr_frame_auto.log`, and waits for a manual census in progress instead of clearing it (see "Dream overlay", census files).
- **Nits.**
  - The summary is 1 KB and starts with "ended: ...". "frozen" means the freeze was ever taken.
  - The per-frame line in the after phase is thinned to every 10th Present while nothing but the frame, gap and held count changes, and the session cap is 12000 lines.
  - The load diagnostics' put-off start walk is labelled with its real reason and runs 30 Presents after live (its own "load:" line), not on the transition's release frame.
- **BT-9 (documented, not changed).** A runtime (Quest) recentre mid-freeze drops the pin inside the `xr_frame` that then submits. That frame and the next one go out with live poses although they were rendered from the frozen pose: 1-2 frames of the frozen image following the head. The old poses belong to the old space, so this is the least bad option.

### Re-review fixes (battle transition)

- **BT-R-1:** the 3 s "nothing happened" cancel also applies in the shatter phase. An encounter window that reached the shatter on a false start (no handle, no effect byte, battle_state 0) ends after 180 Presents and 3 s instead of staying frozen until the 30 s backstop.
- **BT-R-2:** near-miss probes (draws outside the shard rule, checked against the capture) are limited to draws into the scene target, at most 64 per frame. The rule draws are checked as before.
- **BT-R-3:** the one-time frame census at the first shatter runs only with `diag_transition=1`, as before; the gate is now stated in the code.
- **BT-R-4:** the bt: log buffer has one user at a time (an interlocked owner flag), and only the render thread ends a window. At shutdown, the main thread flushes the kept lines first and keeps the buffer for good, so a render thread that is still presenting only drops lines. A window still open at exit gets a one-line note.

### Session 1 fix: the view stayed frozen for the whole battle (battle transition)

**What happened** (owner session 1, on 3783837). In mode 2 the glass showed, but the window never left the shatter phase, so the head pose stayed frozen through the live battle and the views could not be changed.
- **The capture was replaced.** The window adopted the real capture (3840x2160). Later in the load, a second overlay-phase target (4096x4096, most likely a shadow atlas) replaced it. That is the summary's "capture 4096x4096 in 2 frames".
- **Every frame then looked like the shards.** From battle_state 12 on, one battle draw a frame sampled that texture and matched the shard rule (`shard=1`), so the shatter never got 6 quiet Presents. The mode-0 windows ended only at the 30 s timeout.
- **Present gaps:** at most 130 ms, so no keepalive is needed.
- **The race:** the map free came at +390..515 ms, well inside the ~3 s of shards (battle_state 0x22).

**Fixes:**
- **The capture is taken once per window,** then dropped when the shards end. A later candidate is logged once and ignored.
- **About the address:** `BrokenScreen_capturedFrame` is the singleton's render target, created lazily and resized only on a window resize. A reused address is not expected, but the drop at the end of the shards rules it out, and the per-window detail line below compares the matched texture's size with the capture's.
- **The shatter ends on the first of these:**
  - 6 Presents in a row with handle 0, +5 clear and fewer than 8 shard draws (the real effect draws ~100 a frame);
  - the battle-live rule (handle 0, a 3D camera for 2 frames, battle_state 1, 2 or 0x17..0x21), which goes straight to idle and unpins;
  - a 5 s cap, after which the window moves to the after phase.
- **The after phase** has a 15 s cap, and can no longer return to the shatter.
- **Any user view action ends the window at once:**
  - Home or the headset's recentre;
  - the turn keys and the view reset;
  - a view hotkey (End, Insert, the keypad), a gesture chord or a world grab;
  - any overlay widget in use (the managed side's flag, `ffxvr_bt_set` mode | 0x100);
  - while frozen: a new controller grip or stick click, or right-stick turning.

  The battle's automatic view switch (`battle_view`) is not a user action.
- **Diagnostics:**
  - once per window, the details of a capture-sampling draw after the real shards: slot, texture size / format / mips / samples against the capture's size, view format, RT0, depth, camera image, draw kind / count, topology;
  - per-frame lines cover the shatter plus 2 s.
- **ABI 16.**

### Session 1 fix review (battle transition)

- **BT-S-1:** the shatter's 5 s cap counts from the last Present with 8 or more shard draws, or from the phase start if there was none. The phase is limited to 12 s from its start. The real glass can no longer be cut off mid-flight and held, for example when the shatter phase opened well before the shards were drawn at battle_state 0x22.
- **BT-S-2:** a capture candidate is adopted only if its size matches capturedFrame's size, read from the singleton's +0x6C/+0x70 with a guarded read. A candidate of another size also no longer moves armed to shatter. If the size cannot be read, the first candidate is adopted as before, and the window says so in its log.
- **BT-S-3:** the battle-live rule is checked before the phase switch, in every phase including armed.
- **BT-S-4:** "with another target, ignored" counts only during the shatter.

### Scripted haze battles: the battle UI doubled at the start (BT-H-1, BT-H-2; branch `fhport`, 2 Nov 2026)

**What happened** (owner, 2 Nov, `battle_transition=1`). At the start of the Luca machina battle (`lc/lchb0400`) the battle UI was shown twice, unfusable, for 1-2 s. That battle enters through a haze, not a glass break.
- **Script battles open a window too.** The design assumed they never do. They go through the same transition call as the glass break (`0x79E5E0`, the handle's only writer, from MsBattleExe's transition path); what picks the effect is `screen_transition [0x112A9D5]` (`0x7829B0`: -1 none, 1..3 types 1/3, anything else types 0/2), which a script request (`0x781C90`, `battle_trigger` 2) writes from the script's own argument. MsBattleExe's real "no transition" path sets battle_state 5 with no handle; the existing `no transition (...)` cancel handles it. The Luca haze is most likely a type 1/3 that does not use BrokenScreen (review of 2c4793e, from the disassembly; confidence M). It sets the handle `[0x112C024]` (0x0A00, kind byte 10, as the glass break), and the window opens on the handle alone: trigger already 0, +4 / +5 never set, no capture, no shard draw.
- **The window outlived the haze.** BT-3 waits for handle 0. The handle stays set for about 92 frames after battle_state is already 1 and the battle draws its UI (`dclr=1`, `live` 138 -> 305). While a window is open there is no UI split, so the UI went into both eye images at the same pixel position. The asymmetric eye frusta put that pixel about 30 deg apart between the eyes.
- **Evidence** (`ffxvr_diag.log` of 2 Nov, research battle-ui-double.md):
  - lchb0400: window 22191-22206 (f=6909, `h=00010A00`, `bs4=bs5=0`); haze 22207-22386 (dclr 0, no UI pass); bst=1 with the handle set 22387-22478 (f=7089-7180); handle 0 at 22479; the window ended at 22480 (f=7182).
  - lchb0300: the same, frame for frame (21741-22033, bst=1 with the handle set at f=1815-1906).
  - An earlier session's lchb0200: 91 such frames (21046-21337).
  - Glass breaks never reach a running state while armed: armed lasts 2 frames at battle_state 0..4 and goes to the shatter at the capture / first shard draw / bs5 (13304-13310, 13823-13829, 14349-14355, 14986-14992). A scan of every window for `ph != idle, bst=1, dclr=1` finds 0 in the glass breaks and 91 / 93 / 93 in the three handle-opened windows.

**Fixes** (native `bt_tick` / `bt_begin`, no ABI change):
- **BT-H-1, the battle-live release without the handle.** Each window tracks whether it has seen a BrokenScreen signal (`g_bt_bs_seen`: +4 or +5, a capture adopted or a shard draw; reset when a window opens). In the **armed** phase of an encounter window with no such signal, that went through the load chain (battle_state 4..0x16, `g_bt_load_seen`; the haze windows open at 4), with no capture candidate of capturedFrame's size and no shard draw in the frame, and battle_state 1 or 2, BT-3 accepts the battle as live with the handle still set. The window ends after 2 such frames, as the existing release does: about 2 frames doubled instead of about 94. The shatter and after phases, and any window with glass-break activity, still need handle 0. The battle end states 0x17..0x21 don't count for this rule. The log line says so: `armed -> idle: the battle is live in the armed phase (battle_state 1 with a 3D camera, 2 frames; the transition handle 00010A00 still set, no BrokenScreen signal: a transition without a glass break)`. After the window the re-arm block waits for the handle to clear (`every signal clear`), so the haze's tail does not open a new window.
- **BT-H-2, mode 2: freeze only for a glass break.** Before, a mode-2 window opened on the handle froze the head pose at once, which here would hold a frozen world for the ~3 s haze. Trigger vs handle is only a proxy for the effect type (screen_transition decides it); it is what the log supports today. Now:
  - a window opened by `battle_trigger` 1 at state 0 (the random path) freezes at once, exactly as before. This has to stay: the capture copies the previous frame (research battle-transition.md 1.3), and that frame must already be rendered with the pinned pose. The first BrokenScreen signal only shows up at the capture frame itself, 2 frames after the window opened (bs4 is never caught set at a Present; the mode-2 window at log line 10254 took its pin at the opening frame 12671, line 10256, and saw the capture and bs5 at 12673), so waiting for it would pin one frame of head motion after the captured image;
  - a window opened on the handle alone waits for its first BrokenScreen signal; the freeze then starts with that frame's submit (`bt: frame N: the freeze starts: a BrokenScreen signal (...)`). A haze never shows one, so it stays head-tracked; the summary reads `mode 2, no freeze: no BrokenScreen signal`. The open line says `the freeze waits for a BrokenScreen signal`;
  - a mode change away from 2 while waiting drops the wait.
  - **Known limits of the proxy:**
    - a story battle whose script asks for a glass type (trig 2, screen_transition 0 or any other value outside -1..3) is a real glass break whose window opens on the handle. In mode 2 it freezes at the capture frame: one game frame of head motion (~16 ms) between the captured image and the pinned pose. No stuck view. The same holds for a random encounter whose trigger Present was missed (rarer: trig was visible for 2 Presents in all 9 logged random windows);
    - a `battle_trigger` 1 encounter whose formation carries screen_transition 1..3 is a haze opened by the trigger. In mode 2 it still freezes for the whole haze, as before; BT-H-1 now ends it at live.
  - **screen_transition is logged** (signed byte, guarded read) on the `window N opens` line (`screen_transition S, trig T`) and in the summary (`screen_transition S at the open`); `not readable` if the read fails. The freeze does not use it yet. Follow-up once the log confirms the mapping (expected 1..3 for the Luca haze and 0 (some value outside -1..3) for random encounters): open a window also on `trig == 2` at state 0, freeze at once for a glass type and wait for the signal only for a haze type, keeping the BrokenScreen-signal rule for an unreadable byte.

## First person: finding Tidus by model name (FP-1, 25 Oct 2026)

On the Luca dock (`lc/lchb0201`) first person followed the wrong actor. The
actor table showed actor 0 = `f001` (chr_id 0x5001, a model from `chr/obj/`:
an object) and Tidus at actor 1 as `c001` (chr_id 1). `fp_actor` was the
single name `c101`, which is Tidus only in the opening; since then the player
had only been found through the "actor 0" fallback (Besaid and blitzball
captures also show Tidus as `c001` at actor 0). The actor table's chr_id is the
model id: type in the high bits (0x0 `c` party models, 0x2 `n` NPCs, 0x4 `w`,
0x5 `f` objects) and the model number below.

- `fp_actor` is now a comma list, best first: default `c001,c101`. An ini that
  still holds the old default `c101` is read as the new list.
- Every live actor gets one score, lower is better: a listed name = its rank;
  a party model (chr_id 1..0xFFF) = 1000 + chr_id, so Tidus beats a follower;
  actor 0 = 100000 (last resort); NPCs and objects are no candidates. The
  current actor is kept while it is live, has the same chr_id (slots are reused
  across map changes) and scores 0; otherwise the table is rescanned at once if
  it died or changed, every frame while on the actor-0 last resort, else every
  30 frames. Ties keep the current actor.
- To use only `c101`, write `c101,` (exactly `c101` is read as the old default).
- Each change is logged: `first person: following actor N 'name' (how) on map X`;
  the overlay shows the same text next to the model names.
- Battle is unchanged (it follows the roster, not names).

## Load lock: profiler and lock release (branch `loadlock`)

Research: parse-time.md (sections 1.6, 4 and 5). Code: native "Load lock build" block at the end of `ffxvr_native.cpp` (before `DllMain`), the `XrHost` wait callbacks around `xrWaitFrame` (`ffxvr_xr.cpp`), managed `load_prof.cs` plus hooks in `load_diag.cs` / `hd_cache.cs`.

- **The question.** The `load:` lines' "parse" (46-93 ms per HD texture cluster on a battle entry) is not CPU work: the texture section `0x465AB0` takes Phyre's global render-interface lock (`0xC94EE4`, a recursive lock: waiters +0x0C, owner tid +0x10, count +0x14) before its mip reads and keeps it through `CreateTexture2D` (`0x58FC20`). The render thread's list interpreter `0x615550` holds the same lock for every whole command list, the Present command included, so in VR also while our code sleeps in `xrWaitFrame`. The lock guards the immediate context: every function that reads `[0xC94F70]` takes it.
- **`diag_load_prof`** (ini; 2 in this test build, **0 since the loads build**, which also writes no `prof:` block with 0; the probes hook at launch, the sampler follows the setting at once; needs the load hooks):
  - A watcher thread samples the loading thread every ~1 ms. It runs while a battle load phase is open (the entry: from MsBattleExe's wait to the entry's `load:` line; the way back: from the teardown to the way-back line; at most 60 s) or while a `loadPCluster` call is in progress. With `diag_load_prof=2` it also samples the render thread on the ticks where the loader waits for the lock.
  - Each sample is SuspendThread, GetThreadContext, one ReadProcessMemory of the stack (8 KB, 4 KB for the render thread), plain reads of the lock words (and the render thread's XR stage), then ResumeThread. Nothing else runs in between: no heap, no CRT, no log, no lock, no module or VirtualQuery call. Classification, the module table, formatting and the write happen with nothing suspended, into fixed buffers.
  - The return addresses come from the EBP chain inside the stack copy, in chain order. Scanned FFX return addresses (values after a call instruction) go first only within 0x40 bytes of ESP, where a frameless leaf's caller sits; without a chain, the scan in slot order is used. This list gives the innermost FFX function in a range table. That decides the bucket: LOCK_WAIT (with the owner read while suspended), LOCK_REL, MGR_MUTEX, ALLOC_WAIT, ALLOC_FILL, TEX_CREATE (by leaf: d3d11, driver, ntdll), SRV, READ, OPEN, CLOSE, FIXUP, PLAT_CPU, MANAGED (JIT code), POST, IDLE, OTHER. Every tick also records the lock's owner (render, loader, main, other, free).
  - The two probes (managed, last in the hook queue) time the texture section and its creates inside each window. "Before the first mip read" is the lock wait plus the 2x temporary buffer, and the misses are split by file size.
  - One `prof:` block follows each `load:` line: the header, buckets in and out of window, the render thread while the loader waited, the owner per tick, the lock release's counts, the probes, the top exclusive addresses (FhUtil, mapped with `re/out/functions.tsv`) and the field loads in between.
  - The `load:` line's "GPU create" (0x441620) segment is now called "post fixups": the textures are created inside parse.
- **`load_lock_release`** (ini and overlay, live, default 0, **never saved**: every launch starts at 0). **Changed in the loads build:** default 1, saved like any other setting, and it acts during every load, not only battle loads (see "Loads" at the end):
  - 1: the render thread gives the lock up around our `xrWaitFrame`. There is no other mode (see "Review fixes").
  - It acts only while a battle load phase is open, only when the render thread owns the lock exactly once (owner == its tid, count == 1), and only when someone waits for it or a loadPCluster is in progress.
  - The game's own release (`0x4319D0`) and acquire (`0x431840`) are called, so the waiter count and the event stay consistent, and the lock is taken back before the game runs again: the game sees exactly owner = us, count = 1. If a re-acquire ever finds anything else, the release turns itself off for the session.
- **Why mode 1 is safe (and where it is not proven).**
  - Across the wait the render thread relies on its command list (ring slot memory it owns until the list ends), the interpreter state on its stack, the renderer singleton's device / context / swapchain pointers, the immediate context's bound state, and the objects the rest of the list draws with.
  - During `xrWaitFrame` the render thread runs no Phyre code and makes no D3D call. The device is created without SINGLETHREADED (D3D11CreateDeviceAndSwapChain at 0x595940 passes Flags 0), so it is free-threaded.
  - The loader's locked section only creates objects through the device (CreateTexture2D, the SRV, CreateBuffer) and writes them into the PTexture it is loading. No list references those until the main thread registers them. Frees happen on the main thread after a full flush, and that flush waits for this list.
  - Deadlock: no lock holder ever waits for the render thread (vanilla would deadlock at the next list start otherwise), and we hold no lock of ours across the wait.
  - **Not proven:** about 28 lock-protected functions use the immediate context (Map / Update paths). If one runs on the main thread inside our window, its writes land in the middle of the current list instead of between lists. That is D3D-legal (we do not touch the context then), but the rest of this list could see next-frame data: at worst a one-frame visual glitch. The block's "owner per tick: main" and the release line's "holder main" show whether it happens.
- **ABI 17:** + `ffxvr_prof_set`, `ffxvr_prof_mark`, `ffxvr_prof_phase`, `ffxvr_prof_report`, `ffxvr_lock_release_set`, `ffxvr_lock_release_status`.

### Review fixes (load lock)

- **LL-1, no release around the game's Present.** The draft's mode 2 (a call-site wrapper at 0x614D89) is deleted.
  - Over the whole Present call, the DXGI Present, Fahrenheit's ImGui and our `ffxvr_begin_frame` all use the immediate context.
  - The main thread's lock holders (Map / Unmap / UpdateSubresource paths such as 0x562E00, 0x564CB0, 0x59D8F0) would then share it unserialised: a crash or a device removal.
  - The ini takes 0 or 1 only, and the native side treats anything else as 0.
- **LL-2, a test toggle, not a setting.** `load_lock_release` is always written as 0 and forced to 0 at the launch read; a hand edit still applies live. The overlay says "not saved".
- **LL-3, stack order.** The profiler takes the FFX return addresses in stack-slot order, innermost first. A frameless leaf's caller at [esp] then decides the stage. Example: the pool's 0xCD fill, `0x6FB910` → memset, is ALLOC_FILL, not PLAT_CPU. The top-exclusive table uses the same innermost address.
- **LL-4.** The re-acquire runs under the XR stage name "lock re-acquire". A hang there is named as ours by the xr watchdog, not blamed on `xrWaitFrame`.
- **LL-6, mid-list main-thread work made visible.** Every tick with our window open records who holds the lock. The prof block prints "inside our release windows … MAIN n". Main > 0 means main-thread lock work ran mid-list, which risks the one-frame glitch.
- **LL-7.** If the window stays open for more than 1 s (the re-acquire is stuck), the watcher samples the holder thread once and logs its tid, owner class and innermost FFX return addresses.
- **LL-8.** The way-back phase also ends when no way-back line will come: a battle that ended before live, or the early return in `ld_log_out`. The native phase timeout is 60 s, which matches the way back's own.
- **LL-9.** If the release did not clear the owner, the lock is re-taken and the release disabled. After a re-acquire with a count above 1, the extra holds are released and the release disabled.
- **Nits:**
  - LL-10: Fahrenheit's managed `h_fopen` counts as OPEN.
  - LL-11: "dxgi present" needs a dxgi return address that follows a call instruction.
  - LL-12: the 1 ms timer runs only while sampling.
  - LL-14: the render-thread suspends are reported separately, with their total, because they lengthen the measured wait.
  - LL-15: the re-acquires are counted in bins (≤ 1, 1-5, 5-11, > 11 ms), so late VR frames are counted, not just the worst one.
  - LL-16: dropped blocks are reported in the next block.
  - LL-17: the release's state text is snapshotted on the render thread.
- **Re-review (LL-R-1, LL-R-2).**
  - The frame chain is the default order again. A scanned return address goes in front only within 0x40 bytes of ESP, or when the chain found nothing. Stale scan hits further up the stack no longer outrank real frames.
  - The watcher thread also starts when `load_lock_release=1`, with `diag_load_prof=0` too. It then samples nothing, but the stuck re-acquire check (LL-7) always runs while the release is on.

## Dream overlay (branch `dream`)

Research: dream-overlay.md. Code: native `dream_rule` (called from `ffxvr_on_draw`), `dream_tick` (top of `xr_tick`, after `bt_tick`), the mode-2 branch at the top of `ffxvr_eye2_begin`, the census destinations next to `ffxvr_frame_arm`; managed `dream_overlay.cs`.

- **What it was.** The HD port's own dream filter, `0x63DF70`, draws through the central 2D quad submit (`0x63EEE0`) after the depth clear that opens the UI pass. It draws three full-screen quads (film grain `Noise.dds` 1024x1024 at alpha 30, a textureless sepia quad at alpha 90, `Vignette.dds` 1024x512 at alpha 200) and, every third frame, three 16x16 dust specks. Our UI split sent them to the head-locked UI panel. On the monitor they cover the screen; in the headset they were a flat-screen-sized grainy sepia rectangle in front of an untinted world.
- **How the game drives it.** `0x8223A0`, every frame from `Sg_MainLoop`, keys on the field map number `[0x1330844]` (seven maps) and a marker layer's visibility. The live state is `[0x12FB8F0]` (active) and `[0x12FB8F4]` (a warm-up count; the filter draws at 5). Screen fades scale its alpha through `[0x12FFAE8]` / `[0x12FFAF4]`.
- **`dream_overlay`** (ini and overlay, applies at once, default 1):
  - 0: on the panel, as before.
  - 1: matched draws go into the game's own target only (`ffxvr_on_draw` returns 0). The monitor is unchanged; neither the panel nor the eyes get them. They still count as UI draws (`g_ui_draws_frame`), because on quiet frames they are the whole UI pass and `UI_DONE` at the resolve needs a count above 0.
  - 2: the three full-screen layers are drawn into both eye images (`g_eye1_final` / `g_eye2_final`) with the game's shaders, textures, viewport and blend (`ffxvr_on_draw` returns 2; `ffxvr_eye2_begin` binds the eye image with no depth; `ffxvr_eye2_end`'s UI branch restores the targets and the blend). The tint covers the whole view and the vignette sits at the lens edge. The specks are hidden: head-locked dust at zero disparity reads as dirt on the lenses. The eye pass is skipped (counted) if the viewport is not the eye image's size at 0,0. A blend with alpha-to-coverage is copied without it (see below).
- **The rule.** Both must hold. While the gate is closed the rule does nothing at all: no texture query, no vertex read, no stream learning (DR-1), so normal play runs exactly as before this build.
  - **The game's flag.** `dream_tick` reads `[0x12FB8F0]` (8 bytes: active, count), `[0x12FFAE8]` (16 bytes: fade on, direction, length, factor) and `[0x1330844]` once per Present with ReadProcessMemory, which never faults under the stage-0 debugger.
    - Live = active != 0 (from the first warm-up frame: those frames' UI draws learn the UI streams before the first filter frame, DR-1) and the field map number is one of the seven the switch `0x8223A0` drives the filter in: 0x86, 0xA5, 0xBF, 0xF9, 0xFC, 0x129, 0x187 (DR-3). A flag left stale where the switch did not run cannot open the gate anywhere else.
    - A live read opens the gate for this frame and the next 2 (the latch). A failed read closes it. A full fade does not close it (the filter draws nothing then); those frames are counted.
  - **The draw**, tested cheapest first: a UI-pass draw in the headset path (`g_ui_draw_now`), DrawIndexed with 6 indices; then the slot-0 texture: 1024x1024, 1024x512 or none (a layer), or 16x16 (a speck); only then the quad (the shared guarded read, `ui_quads_cached`): -950..950 x -530..530 or more and at most 2000 x 1200 for a layer, 36-43 x 24-31 UI units for a speck (10.5 PS2 units).
  - **The colour (DR-4).** When the slot-2 colour stream can be read (its checked map pointer, then ReadProcessMemory), the first vertex's RGB must also be the filter's, within 0.02: (186, 158, 85)/255 for the sepia quad, 190/255 grey for Noise, 128/255 grey for Vignette and the specks. A screen fade scales only the alpha. A failed read does not block the match (counted as "matched without a colour read"). A draw of the right shape and texture but another colour is left alone, counted, and logged once a window.
  - It cannot hit normal UI (the gate is closed outside the seven maps and their marker layer; inside them, glyphs, windows and icons fail the texture or geometry test), screen fades (the HD ScreenFade post-process, not UI quads), the glass break (`Draw n=3`; and no UI split in battle_transition modes 1 and 2), FMV (a screen frame: no UI split) or battle cursors (at most 90 units). The flat path is never touched.
- **Mode 2 and alpha-to-coverage (DR-5).** If the game's blend has alpha-to-coverage, which on a one-sample eye image would cut alpha 30/90/200 to 0 or 1, the eye passes use a copy of the game's own blend description with it turned off. The copy is created once per distinct description. If that creation fails, the eye pass is skipped and counted.
- **Diagnostics** (`ffxvr_diag.log`, batched on the render thread and written with `diag_try`, never from inside a draw):
  - `dream: game filter on at frame F: map '…' field map 0x…, mode M …`, and `dream: game filter off at frame F (on since frame F0) after S s (why): frames with matches X / gate frames G, draws handled L layers + K specks (per frame max P), mode M, eye-pass skips E, …, matched without a colour read C, filter-shaped quads with the wrong colour R (not handled)`.
  - `dream: matched draws in frame F: ui#0 layer 1024x1024 n=6 [-960,-540..960,540] (1920x1080) rgba … , …`: the window's first frame with matches. If the colour stream was not known yet, the line waits up to 5 frames with matches and prints `rgba ? (geometry only)`. A match after another UI draw in the same frame adds "not a prefix of the UI pass".
  - Once a window: `dream: frame F: ui#N layer-shaped … has the filter's shape but not its colour …: not handled`. The session count is in the 10 s `ui:` line (`dream handled N (mode, filter on/off), filter-shaped quads with the wrong colour R this session`) and in the overlay row.
  - The draft's "full-screen quad without the flag" diagnostic is gone with DR-1: it needed a vertex read of every single-quad UI draw in normal play.
  - Census: the matched draws keep their census lines, tagged `DREAM layer (headset: hidden)`, `DREAM layer (headset: per eye)`, `DREAM speck (headset: hidden)` or `… (headset: on the panel, mode 0)`.
- **Census files.**
  - The overlay's Frame census goes to `ffxvr_frame.log`. The automatic one (the session's first glass break) goes to `ffxvr_frame_auto.log`.
  - One census at a time: an arm while another is armed or live waits. It is armed at the Present that writes the other one (manual first) and goes live at the Present after (DR-7: a frame armed first, so geometry the game maps a frame ahead is captured). Nothing clears or truncates a census in progress; a second press while a manual census runs asks for one more frame after it.
  - Every frame's header line ends with `| <local time> | game frame N | map '…' | <reason>`, taken when the census goes live.
  - N1: the closing `-- Present --` line is written (it was dropped: it ran after the census stopped being live).
  - N2: UI census lines decode the blend state: `(en, c src/dst, op, a src/dst, op, wm, a2c)`.
- **ABI 18:** + `ffxvr_dream_set`, `ffxvr_dream_status`.

## Character shadows: the light camera (branch `shadows`)

Research: shadows-one-eye.md. Code: native `light_proj`, `lightcam_note`, `lightcam_frame_end` and the `light` branch of `maybe_override`, the fp_self_shadow block near the top of `ffxvr_on_draw`; managed `shadow_cam.cs`. The game side is in exe-analysis.md section 16 (the ShadowManager).

- **What it was.** Character shadows showed in one eye only, a different eye for different characters, with odd shapes; environment shadows were fine. The game's ShadowManager draws each character from straight above into its own tile of a 4096x4096 8x8 atlas before the scene, then the ground receives the tiles through CPU-built decal quads. The casters' constant buffer is the skinned-character layout with the light in it: the light's view at the view slot, the light's **orthographic** projection in the near-variant slot (+128), and the scene's **perspective** projection left in the main slot (+256). `maybe_override` only tested the main slot, so it took every caster upload (about 70 a frame in the Luca locker room) for the scene camera. `write_camera` then put the head view into the light view and each eye's asymmetric FOV into the orthographic matrix. In an orthographic matrix the FOV's frustum offset `P[2] = (r+l)/(r-l)` (-0.242 / +0.242) becomes a shear proportional to depth, so each eye's tile only got a sheared side slice of characters about 15 degrees to that eye's side of the gaze. The receiver then showed each eye's broken tile faithfully.
- **The rule (`light_proj`).** A camera buffer that already passed the camera test (a view at +64 or +48, its inverse agreeing, a perspective main projection) is a light camera when:
  - the near-variant slot is orthographic: last row 0 0 0 1 (within 1e-3 / 1e-4) with a finite, non-zero x, y and depth scale;
  - the main slot is a real 3D perspective: last row 0 0 -1 0 and |P0|, |P5| > 0.2 (never the title/intro's near-2D camera);
  - it is not the 2D layer system's dummy camera (that keeps its own path, checked first).
  - the buffer is at least 1872 bytes, the skinned layout (SH-3: every caster in the captures is 6992 B), so a ~1 KB scene buffer can never be reclassified.
  The scene camera has a perspective in both slots, so it cannot match. UI, the glass-break shards and the dream filter carry no camera at all (they never passed the camera test), so they cannot match either: the rule only reclassifies buffers that were already taken as cameras. Whether the light looks straight down is not part of the rule; it is counted ("not straight down"), and the first such match per map is logged in full, so a match that is not the ShadowManager's would show in the log.
- **`shadow_cam`** (ini and overlay; default 1; applied at the next Present, so both eyes of a frame agree):
  - 1: a light camera upload is returned unchanged: no override, no eye image (any eye image taken earlier in the frame for the same buffer is dropped, so neither the draw nor eye 2's re-issue can pick up an older object's camera). Eye 1 draws the game's tile; eye 2's re-issue uploads nothing for it, so the atlas twin gets the same tile. It is kept out of `g_cam_uploads_frame` / `g_cam3d_uploads_frame` (the 3D-or-screen decision, the battle transition's "3D camera" test), the view tick and its "another camera" statistic, the first-person heading and seat latches, `g_last_PV` (write_camera is not called), `g_cam` (`camera_from_write`: markers, pointers, `ffxvr_project`), the bone records and the first-person body hide.
  - 0: overridden as the scene camera, as before. For an A/B comparison only; the light cameras are still counted and tagged.
- **`fp_self_shadow`** (ini and overlay; default 1). With `shadow_cam` 1 the player's casters carry no eye image, so the body hide (which reads the image's world position) no longer removes them.
  - 1: the player's own shadow is drawn with the game's light, stable and the same in both eyes, in every view and body mode. In first person with `fp_body_mode` 0 that is new (the old hide removed it). With `fp_body_mode` 1, the VR hands, or when the old hide did not apply, it replaces the sheared slice the head camera used to put in the tile (build-shadows-review.md SH-2).
  - 0: hidden in first person's whole-body modes only. on_draw keeps a 32-entry table of this frame's light-camera buffers with their world translation; a draw whose slot-0 buffer holds a light camera, into a target without depth (the atlas), within the body hide's radius of the player, is skipped while the whole body is hidden: `fp_body_mode` 0 (the old result), or the VR hands with floating hands (`hands_show` >= 2), where the hand parts' shadow is hidden too (a light camera has no bone record to tell them apart). With `fp_body_mode` 1 or 2, or the VR hands with `hands_show` < 2, it hides nothing.
  - The VR hands (tabled) do not move the shadow's arms any more: the casters get the game's pose.
- **Diagnostics.**
  - `perf:` line: `light cams N (the game's | shadow_cam 0: overridden)`, plus `not straight down T this session, own shadow hidden H` when either is non-zero.
  - One line per map (`shadow: map '…' frame F: N light cameras in the frame …`) with the first light camera's size, near-variant, main and view rows, its position and forward axis, and the caster's world translation. And once per map, the first light camera that does **not** look straight down, in the same detail (`shadow: map '…' frame F: a light camera NOT looking straight down …`; at most 50 a session; SH-3). Rate-limited: the map name is read (ReadProcessMemory) at most once a second and only while light cameras are uploaded; a line at most every 5 s, 200 a session; a map with no light camera for 120 frames is forgotten, so coming back logs again.
  - `shadow: shadow_cam A -> B at frame F` at every switch. Every `shadow:` line the render thread cannot write at once (the log open elsewhere) is kept and retried at the next Present (SH-5).
  - Census: one `LIGHT CAM uploads: N since the last event | first: cb … B ortho x y z … | light forward (…) | world (…) | light cam (game's): left alone, no eye image` line per run of uploads, written with the run line at the next event, so the per-tile caster runs stay whole (SH-4); the draw-run lines count `light cam (game's) K`.
  - Overlay: Debug > Character shadows (both settings and the status), and the main window row next to "headset field of view".
- **The stereo screen's auto depth (blitzball) changes** (SH-1). `screen3d_auto` runs on the frame's first eye-1 `write_camera`, which in maps with the shadow atlas (blitzball's pool, bl/bltz0005, among them) was a caster: the convergence and separation came from a straight-down light with an orthographic projection, and each eye's light was shifted by half the separation (shadow disparity). Now they come from the scene camera, so the depth may feel different; `stereo_screen_depth` / `stereo_screen_plane` may need re-tuning.
- **Expected in lc/lchb1300 with shadow_cam 1:** `light cams` about 65-75; `same-input` up from about 11 to about 80; the `view:` line's "uploads with another camera" about 0 and its "camera uploads/frame" down by the same 70; the first `fov apply` line of a session shows a perspective near variant. In first person with `fp_body_mode` 0 the `hidden` count drops by the player's caster draws.
- **Follow-up, not done: fix B (share the atlas).** Eye 2 re-issues the casters and blur quads into the atlas and temp twins, which after fix A only reproduces the game's tiles: about 100 draws a frame and two 64 MB twins for an identical result. The atlas is view-independent, so eye 2 could sample the game's own atlas instead: mark the atlas set per frame (RT0 of the light-camera draws, plus the blur temp: a square non-screen-shaped one-sample target without depth before the first MSAA scene bind that samples a set member), skip eye 2 for draws into it, have `twin_srv` return null for set members so the receiver's eye 2 samples the game's atlas, and let the sweep release their twins. Drop a texture from the set for the session if it is ever bound with depth or as an MSAA target. It would also cover atlas draws in the overlay phase (eye 2 off), which today can leave a twin stale during loads. Setting `shadow_share` (0/1, default 0 until verified). Spec: shadows-one-eye.md 3 B.
- **ABI 19:** + `ffxvr_shadow_set`, `ffxvr_shadow_status`.

## Adaptive UI panel: depth and backing plate (branch `uidepth`)

Code: native `uid_depth_pass`, `uid_present`, `uib_copy` and the shaders `k_uid_*_hlsl` (the block after `copy_pass_init`), the depth-row note and the slot detection in `write_camera`, the call in `ffxvr_on_clear_twin`, the `XrUiPanel` fields `im_dist` / `im_width` and `xr_srgb()` (ffxvr_xr.h); managed `uidepth.cs`. Review: build-uidepth-review.md (UD-1 to UD-14; the fixes are in the second commit).

- **What it was.** Subtitles, menus and the battle UI are captured into the UI texture during the game's UI pass and shown on a head-locked OpenXR quad (VIEW space, `(0, 0, -ui_distance)`, `ui_width` x 9/16), composited over the projection layer with no depth. When scene geometry is nearer than the panel (an NPC close by in first person), its disparity says "nearer" while the text covering it says "the text is nearer". The text is hard to focus on there.
- **Adaptive distance** (`ui_adaptive`, default 1; `ui_adaptive_min` 0.6 m; `ui_adaptive_margin` 0.85). The panel goes to `min(ui_distance, nearest * margin)`, floored at `ui_adaptive_min`, where `nearest` is the nearest scene depth behind the visible UI content. Its width is scaled by `distance / ui_distance`, so the text keeps its angular size.
  - **Where the depth is read.** The UI pass opens with a depth-only clear of the scene target, so by the time the UI texture is finished the scene depth is gone. The managed `ClearDepthStencilView` detour calls `ffxvr_on_clear_twin` *before* the game's clear, so at the clear that opens the pass the depth of this very frame is intact, in eye 1's target and in eye 2's twin (the twin is cleared after, in the same function). `uid_depth_pass` runs there, once a frame (a second opener after an extra resolve would find the depth already cleared).
    - It runs only if that clear is to 1.0, the standard-Z far value (UD-4; otherwise one log line).
    - The first pass of a session logs the viewport's depth range, the clear value, the depth target and both slots' depth rows. A reversed viewport range skips the pass.
  - **The depth pass.** The game's depth is `R24G8_TYPELESS`, bind 0x48 (depth + shader resource), 4x MSAA (the msaa override; 8x by default) at the game's resolution.
    - It is read through an SRV of ours (`R24_UNORM_X8_TYPELESS`, `TEXTURE2DMS`, sample 0), with no resolve or copy.
    - One pixel-shader pass writes 64 x 36 R32F: per panel tile, the nearest linear depth in metres. Each tile takes 8 x 8 samples over the tile and half a tile around it, from both eyes (each through its own panel footprint), so the other eye's parallax is covered.
    - The depth target and its twin are unbound from the output merger for the pass (our own target, no DSV) and come back with the restore.
    - Our SRV holds a reference on the texture, so its pointer cannot be reused while we compare it. Both references are dropped when adaptive depth is off or no depth pass has run for 300 frames (UD-8), and at headset off.
  - **The footprint.** The panel lies on the head's view axis at the current distance. Its half size in tangents is `hw = ui_width / (2 ui_distance)`, `hh = hw * 9/16` (constant, since the width follows the distance). Seen from eye e at `-+ipd/2` along the head's x (the views are parallel), its x range is `-+hw - ex / d` and its y range `-+hh`. That is mapped into the eye image with the field of view the frame was rendered with (`g_xr_fov_used[e]`: `u = (t - l) / (r - l)`, `v = (u_t - t) / (u_t - d_t)`). With `headset_fov=0` there is no adaptive distance (one log line).
  - **Linearising.** The override puts the headset's FOV into x/y only and leaves the depth rows of both projection slots alone. `write_camera` notes `P[10], P[11], P[14], P[15]` of the near-variant slot (+128 at base 64) and of the main slot (+256) at the frame's first eye-1 upload of a perspective camera. Eye z = `(P11 - ndc P15) / (ndc P14 - P10)`, distance = -z.
    - In every map in the logs, the near variant's row2 is `(0 0 -1 -1)` (ndc = 1 - 1/d: D3D [0,1], near 1 unit, standard Z, infinite far).
    - The main slot's row2 is `(0 0 -1 -2)`: the game builder's GL-style matrix, of which the near variant is the D3D remap. `(0 0 -1 -0.002)` appears only in a session's first camera. So **the main slot reads 2x too far**.
    - **Slot detection (UD-3).** The slot is detected, not assumed. At eye-1 uploads, until it is decided for the frame (at most 16 uploads a frame, UD-R-2), the scene's VP block (+192) is compared with `Pnear * V` and `Pmain * V`. `same4` alone cannot separate the two (they differ only by P11's difference in element 11, under its 1 % tolerance), so the z rows' residuals decide.
    - The default "auto" uses the detected slot (the near variant until one is known). Debug > UI panel can force either slot (not saved).
    - The 10 s line and the census print the slot in use and how many frames each source was detected.
  - **Units.** A head motion of 1 m moves the eye `g_xr_scale * (g_fp_mode == 2 ? g_gest_scale : 1)` game units (`build_override_view`: the head translation and the eye separation are scaled by it; only the diorama zooms the world), so a point at d units along the view axis is seen at `d / that` metres. That is the same view axis and plane as the panel's distance (eye 1's view is the head pose under the override).
    - **11.4 units/m in first and third person** (`k_true_scale`, true scale). The ini's world_scale (94 in the owner's ini) applies in the seat and diorama views, times the zoom in the diorama.
    - Worked check, first person: an NPC 1 m away is 11.4 units, so ndc = 0.912, which linearises to 11.4 units, i.e. 1.00 m. The near plane, 1 unit, is 8.8 cm at true scale.
  - **The UI side, at Present** (`uid_present`, in `xr_tick` before `xr_frame`).
    - A 64 x 36 pass takes each tile's max UI alpha (12 x 12 bilinear samples). Above 0.05 it keeps the tile's depth (1e4 when the depth pass did not run this frame); otherwise -1.
    - The tile pass writes R32G32: the tile's depth (or -1), and the tile's max UI alpha (UD-R-4).
    - That is copied into a ring of 4 staging textures, mapped with `DO_NOT_WAIT` (1-2 frames of latency, no stall). A slot still in flight is never restarted (UD-5): that frame's copy is skipped, and a slot 30 frames old is taken as lost. A slot not ready yet does not block a newer finished one (UD-R-3); the newest frame wins.
    - The CPU counts the UI tiles and takes the second-nearest depth among them (one stray tile does not pull the panel in).
  - **No pull-in** (the target is `ui_distance`): no UI pixels; more than 60 % of the tiles have UI (a full-screen menu: no scene shows through, so there is no conflict); no depth this frame; nothing nearer than the panel; no fresh readback for 30 panel frames (UD-5). For the first frames without a readback, the distance holds.
  - **Smoothing.**
    - In: exponential, tau 100 ms, at once.
    - Out: only after the target has been more than 6 % + 2 cm further for 300 ms, then exponential with tau 600 ms until it arrives. Small rises are ignored and dips are followed, so it does not pump.
    - With no 3D panel for 30 frames the distance goes back to `ui_distance`. Adaptive off: `ui_distance` at once (A/B).
  - **Camera cuts (UD-11).** A cut (`g_cut_count`: the seat view's authored cameras, including battle cameras) is not eased across. The first readback of a frame after the cut sets the distance at once, so the cut masks the jump (2-3 frames after it). The distance then holds until 200 ms after the cut, so a burst of cuts does not pump it. If no post-cut readback comes within 200 ms, normal following resumes. (A hold-then-ease alone would show the move after every cut.)
  - **The ImGui layer (UD-10, UD-R-1).** It keeps its own `0.95 x ui_distance`, except while the UI panel's distance is pulled in: then it sits at 0.95 x that distance, at the same angular size, so it stays in front of the panel it is drawn over. This is keyed on the held distance, not on whether this frame has a UI pass, so it does not jump on frames without one.
  - **Not in:** the screen modes (FMV, title, 2D screens), the stereo screen, a battle transition window (no UI split there), while the dream filter's gate is open, or with the headset off.
- **Backing plate** (`ui_backing`, 0..1, default 0.35; 0 = off). A soft dark halo behind the UI's text:
  - **The seed (UD-9, UD-R-4).** Only near-opaque UI makes a plate (text, borders, icons). A translucent window's fill makes none of its own, so the default does not darken behind dialogue and battle windows as a whole, only around their text.
    - The threshold is relative to M, the highest UI-texture alpha seen this session (the tile pass's per-tile maxima, read back with the depth tiles; reset at headset off; 1 until one is seen): none at 0.55 M, full at 0.9 M.
    - So it also works if the game's UI uses PS2-style alpha (opaque = 0.502, window fills about 0.19, from the census's vertex colours).
    - The 10 s line prints the window's and the session's max UI alpha and the resulting range.
  - **The blur.** The seed is dilated to quarter resolution (the max of each 4 x 4 block) and blurred, 7 taps horizontally then vertically, Gaussian sigma 1.5 quarter px: about 6 px at the UI's resolution, reaching about 12 px.
  - **Compositing.** It is composited under the UI in the UI quad's swapchain copy (`ffxvr_copy_pass` -> `uib_copy`, the shader that already stretches the UI into the 2560 x 1440 swapchain image), so there is no full-resolution pass of its own. The plate is `b = saturate(2 * blur) * ui_backing`.
  - **Alpha (UD-1).** The layer keeps `XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT`, the UI texture's colour is premultiplied by its blend, the swapchain is `R8G8B8A8_UNORM_SRGB` (our UNORM bytes are copied in, so the runtime decodes them) and the compositor blends in linear. The plate writes:
    - `a' = a + b (1 - a)`
    - `rgb' = enc(dec(rgb) a / a')` (`dec` / `enc` the sRGB curve)
    - so the compositor shows `dec(rgb') a' + scene (1 - a') = dec(rgb) a + scene (1 - b)(1 - a)`: exactly the old picture over a darkened scene. With a plain UNORM swapchain (`xr_srgb()` false) the rescale is done on the bytes.
  - Opaque UI is unchanged and areas with no UI content are not darkened. The plate fades out between 50 % and 80 % UI tiles (full-screen menus). It is skipped on frames the last readback found empty, and in the same cases as the adaptive distance.
- **Shaders** compile at the first `xr_tick` with the headset on (at Present), not inside the game's frame (UD-6). The depth shader's outer loop is not unrolled.
- **Diagnostics.**
  - Every 10 s, while there were 3D panel frames: a `uidepth:` line with:
    - the settings, the distance (min / avg / max), the fraction of frames pulled in and the nearest depth seen;
    - the frames with no UI / a full-screen menu / no depth / waiting / stale readbacks / held after a cut (and the number of cuts);
    - the backing plate's frame count;
    - the GPU time per frame of the depth pass, the tile pass and the backing passes (timestamp queries read 3 frames late without flushing; the depth pass sits inside the frame's own disjoint span and uses the last frequency);
    - the depth row in use (auto / set), the detected VP source per frame, and the units per metre.
  - Census: a `UIDEPTH depth pass …` line at the UI pass's opening clear (depth target, footprints, panel distance, depth row) and a `UIDEPTH present passes …` line before `-- Present --` (tiles, plate, swapchain encoding, the distance decided).
  - Overlay: the main window row under "UI on its own panel" shows the live distance and state. Debug > UI panel has every setting, the projection slot and the full status.
- **Cost (estimate, not measured).** Depth pass: 2304 pixels x 128 MSAA loads. Tile pass: 2304 x 144 bilinear samples. Plate: one quarter-resolution pass reading the 4K UI texture once (about 33 MB) plus two 960 x 540 blurs, and one extra sample per pixel in the existing swapchain copy. About 0.1-0.2 ms on an RTX 3080 Ti at 4K; the log line measures it.
- **ABI 20:** + `ffxvr_uidepth_set(int adaptive, float min_m, float margin, float backing, int proj)` (proj 0 auto, 1 near variant, 2 main), `ffxvr_uidepth_status(float* out, int n)` (15 values since the review fixes; same signature).
- **Open points.**
  - Parallax between the panel footprint and objects at the panel's edges is covered only by half a tile.
  - A short, thin subtitle fragment can be missed by the tile pass's 12 x 12 samples (UD-13).
  - No adaptive distance with a single-sample scene depth (UD-12).
  - The plate's reference M is a session maximum: an opaque full-screen menu with a higher alpha than the text (if the text is PS2-style and menus are not) would raise it, and the subtitles' halo would weaken for the rest of the session. The 10 s line shows M.

## Fahrenheit's save screen in the headset (branch `saveui`)

Code: native `ensure_ui_blend` (before `ui_pass_begin`), the ImGui branch of `ffxvr_eye2_begin`, the copy shaders `ps_premul` / `ps_premul_srgb` in `k_copy_hlsl` and `ffxvr_copy_pass`'s `mode`, the menu block and the `imgui:` line in `xr_tick`, `ffxvr_imgui_layer_set` / `ffxvr_imgui_status`; ffxvr_xr.h `XrUiPanel::im_world`, `xr_imgui_size`; ffxvr_xr.cpp the ImGui swapchain size and the `imq` quad; managed `save_screen_state`, `fh_menu_ui`, ini `fh_menu_on_screen`. Research: save-ui-vr.md (scratchpad).

- **What was wrong.** The Fahrenheit developers saw the boot Load menu in our headset video "looking off, no transparency, huge text"; the same menu at a save sphere looked better.
  - **Boot only: the capture did not blend.** The ImGui re-issue (the overlay phase's draws into the backbuffer, copied into the ImGui-layer texture) binds `g_ui_blend`, which was created only by `ui_pass_begin`, the opener of a 3D frame's UI pass. At boot every frame is SCREEN, so the state was still null and `OMSetBlendState(nullptr)` meant blending off: every ImGui draw overwrote the capture. Translucent frames and gradients punched see-through holes, every glyph quad left a dark box, the drop shadows were erased. After the first gameplay UI pass the state existed, so later menus (save spheres) were captured right.
  - **Everywhere: a double multiply.** Blending into a target cleared to (0,0,0,0) leaves `rgb = c a` (premultiplied), but the quad was flagged `UNPREMULTIPLIED_ALPHA`, so the compositor multiplied by `a` again. With the sRGB swapchain a white glyph edge at 50 % coverage came out at 0.107 linear instead of 0.5: dark fringes on all ImGui text and on our overlay's borders.
  - **Size.** The whole 16:9 menu, designed as full-screen TV UI, filled a head-locked quad of 56° at 1.4 m: its corners 28° to the side and 17° up or down, out of comfortable eye rotation, glued to the head, in front of the world-locked virtual screen. (A separate, Fahrenheit-side contributor: alpha12's `ConfigDpiScaleFonts = true` scales every font by the Windows display scale on the monitor too; upstream turned it off on 24 Oct, commit 5173f28. Not changed here; the Fahrenheit port turns it off from our module, FH-1 in "Fahrenheit port" below.)
- **A. The capture always blends.** `ensure_ui_blend()` makes the state (ImGui's own equation: colour `SRC_ALPHA / INV_SRC_ALPHA`, alpha `ONE / INV_SRC_ALPHA`) before any use: from `ui_pass_begin`, at every `xr_tick` where the ImGui texture is made, and in `ffxvr_eye2_begin` before anything is saved or bound. If it still cannot be made, the re-issue is refused (counted, logged once): the draw reaches the backbuffer only and is never captured unblended.
  - Audit of the other state: the re-issue binds only our render target (no depth) and the blend state. The shaders, input layout, sampler, rasterizer (with ImGui's scissor), depth-stencil state and viewport are ImGui's own, bound by its backend just before each draw; the capture has the backbuffer's size and format, so the viewport and scissor rects apply unchanged. The swapchain copy's shaders and sampler are made lazily by `copy_pass_init` at their first use, the swapchain intermediates at `xr_start`. Nothing else was tied to the 3D UI pass.
  - A new capture texture (first headset start, a backbuffer resize) is cleared to transparent when it is made, and not shown until it holds a frame's draws.
- **B. Premultiplied end to end.** The swapchain copy has a mode: 0 opaque, 1 alpha kept (the UI panel, unchanged, with its backing-plate path), 2 the ImGui layer. Mode 2 filters the premultiplied texels as they are (filtering is only correct on premultiplied data); for an sRGB swapchain it writes `enc(dec(rgb / a) a)`, so the compositor, which decodes and then blends premultiplied, shows `dec(c) a + scene (1 - a)`: straight alpha in linear light. For a UNORM swapchain it copies the bytes, which is exactly the monitor's `c a + dst (1 - a)`. The ImGui quad is now flagged `BLEND_TEXTURE_SOURCE_ALPHA` only. Over opaque pixels nothing changes; glyph edges, the F8 overlay's window edges, Fahrenheit's popups and tooltips lose the dark fringe. Every ImGui draw still blends in gamma inside the capture, exactly as on the monitor; only the compositor's step (capture pixels with 0 < A < 1 over the base layer) is in linear light. So the save screen (opaque `ffx_bg`) and text inside opaque windows look exactly as on the monitor; only window outlines, modal dims and text drawn straight over the game (Fahrenheit's mod list at the title) differ, with white anti-aliased edges a little bolder (0.5 linear at 50 % coverage, the monitor's gamma blend 0.214) (SU-4).
  - The two new shaders compile from a source of their own, `k_copy_premul_hlsl` (SU-3): FXC parses the whole source for every entry point, so an error there loses only them (the layer falls back to the single-tap `ps_alpha`) and never the copies every headset image needs. `copy pass: ready (the ImGui layer: …)` says which path is in use. The copy shaders compile at headset start (`ffxvr_xr_set`), with a backstop in `xr_tick`, never inside the first frame's submit (SU-8). The UI panel keeps `UNPREMULTIPLIED_ALPHA` and its own plate math (its c a² look is a separate question, UD-14).
- **C. The save screen goes on the virtual screen** (`fh_menu_on_screen`, default 1, applies at once; main window, under the ImGui layer checkbox).
  - **The flag.** Managed reads Fahrenheit's save-screen state once a frame with a guarded read (`FhSavePal.pal_addr_screen_state`: RVA 0x8CB994 for FFX, a DWORD in FFX.exe's `.data`, VA 0xCCB994, read by the game at 0x63FF76, 0x647F70, 0x648102 and written by its setter at 0x6486E0). The menu counts as open from the first OPEN (2) frame to CLOSED (0), so the unnamed states 1 / 3 / 4 cannot make it flap. `ffxvr_imgui_layer_set` gets it as a fourth argument (ABI 21), only with the ImGui layer on.
  - **Forced screen mode.** While it is open, `xr_tick` sets `screen_next`, except inside a battle transition window (the glass break keeps the view it started in). The stereo screen is off then (it requires `!screen_next`) and the override is suspended, as for any screen frame, so the game renders flat and there is no UI pass, no adaptive panel and no dream-filter split.
  - **The game's state machine** (review, from the handler 0x81FF80): a close is 2 -> 3 -> 4 -> 0, one handler tick a step; state 4 can also go back to 1 and reopen (4 -> 1 -> 2). The latch (set at 2, cleared at 0) spans all of it without flapping. Managed passes the raw word with the latch (`menu = 1 | state << 8`).
  - **The edges are inside the menu's fades.** Fahrenheit draws the menu only in OPEN, and its first OPEN frame draws a full-screen black that fades out over 0.5 s. The flag set at that frame reaches `xr_tick` one frame later, together with that frame's ImGui image (the black): the first forced screen frame shows the screen covered by black, and the game's last 3D frame on it is hidden. At the close the menu has faded to black and run its action before the state goes to CLOSED; the flag drops with the first frame whose ImGui image no longer has the menu, so the layer never shows the menu head-locked.
    - **Latched frames without a menu image (SU-1).** The close states 3 / 4, a 4 -> 1 reopen, and at the open the rare read race (we see 2, Fahrenheit's selector, which reads first in the same Present, saw 1) or the menu's textures not loaded yet: the capture has no menu and no black. For at most 8 such frames since the last frame with a menu image (the word OPEN and ImGui draws), eye 1 is cleared to black, as a cut blink does, so the field never shows flat on a floating screen. After that the frame is shown as is (a game prompt in a long state 4 stays visible). At the title this turns 2 or 3 title frames black after the menu has faded to black.
    - **Fahrenheit redraws its menu after the fade (SU-6).** When `fade_out` ends, the action runs and the fade is done, so until the game's handler moves the word to 3, Fahrenheit draws the full menu again, unfaded, for a frame or more. That happens on the monitor too; in the headset it shows on the screen exactly as there. Not ours.
    - **Watchdog (SU-2).** If the latch never clears (the handler stopped with the word at 1 / 3 / 4, for example a soft reset from inside the menu), it is released after 5 s with the word not OPEN and fewer than `min_draws` ImGui draws in every one of those frames, with one `imgui: save screen latch released …` line. It re-arms when managed unlatches or the word is OPEN again.
  - **Where.** At the title (already SCREEN) and from the stereo screen: the usual virtual screen. Opened from the 3D view (a save sphere): the screen is placed from the head's yaw and position at that moment, `g_xr_screen_dist` ahead, upright, as the recentre places it, without a recentre (the seat, the first-person yaw and the reference stay). Home or the headset's recentre while it is open re-places the reference, and the menu follows that screen. The pose is dropped at close and at headset off.
  - **The ImGui layer on the screen.** With the menu open over a mono screen base, `XrUiPanel::im_world` puts the ImGui quad in the reference space with the screen's pose and width (16:9 onto 16:9, no 0.95 factor), after the screen quad in the layer order: the menu lies exactly over the game's frame, as on the monitor. Otherwise it stays head-locked at 0.95 x `ui_distance` with UD-10. A held ImGui quad (its own wait timed out) is re-sent only in the same space, so a head-locked one never reappears on the screen or the reverse.
  - **Sizes.** The menu's text is the virtual screen's: on the owner's 6.7 m screen at 6 m (58°) the 86 px text has a cap height of about 1.0°; the default 2.5 m at 2.5 m (53°) gives about 0.9°. That is what every other 2D screen in the mod uses.
  - **The F8 overlay.** Without the save screen it is exactly as before (head-locked). If it is open together with the save screen it rides on the screen with the menu (the whole ImGui layer goes there), at about the same angular size as head-locked (0.33-0.50° em on a 58° screen against 0.32-0.48°).
- **D. The ImGui swapchain is 1920x1080** (was 2560x1440). From the owner's 4K capture that is exactly 2:1. The copy takes four bilinear taps a quarter of a target pixel around the centre: at 2:1 they fall on the four source texel centres, an exact 2x2 box; at other backbuffer sizes they are a small area filter rather than one tap that skips texels (no mip chain needed). On the 56° head-locked quad it is about 34 px/deg, still above the Quest 3's roughly 25 px/deg, so the overlay's small text is limited by the display as before, with less shimmer from the compositor's unfiltered minification.
- **Unchanged:** Fahrenheit's ImGui context, fonts, DPI and style, except the two DPI flags since the Fahrenheit port (FH-1, below) (we only scale the capture, never the shared context, so the monitor is unchanged); the UI panel's path; the glass break; FMV; the dream filter.
- **Diagnostics.** `imgui layer: first capture at frame N, blend ready (...)` once; `imgui: frame N: Fahrenheit's save screen open / closed ...` at each edge; `imgui: save screen latch released …` if the watchdog acts; every 10 s while the layer was shown (SU-7: not for the title's mod list alone, which draws every frame), an `imgui:` line (screen frames included): frames with draws and the max per frame, screen frames and how many showed the layer, 3D frames with the layer, frames on the virtual screen, the frames eye 1 was blacked out, the layer settings, the capture and swapchain sizes, the copy mode, and re-issues refused for a missing blend state. Overlay: the row under the ImGui layer checkbox shows where the layer went and whether the save screen is forcing the screen.
- **ABI 21:** `ffxvr_imgui_layer_set(int on, int force_show, int min_draws, int menu)`, + `ffxvr_imgui_status(int* out, int n)` (6 values: forcing, where the layer went, placed from the head, refused, swapchain width and height).
- **Open points.** A latched stretch without a menu image longer than 8 frames shows the game's frame flat on the screen (by design, for a game prompt), and the watchdog ends a stuck latch after 5 s. "Opened from 3D" is decided from the edge frame alone (SU-9, not done). The optional `fh_menu_scale` (a smaller menu on a very large screen) is not done. The DPI font scaling is Fahrenheit's: since the Fahrenheit port our module turns it off (FH-1, "Fahrenheit port" below) until alpha13 does it upstream.

## Heat haze in fire (branch `haze`)

Research: fire-haze.md. Code: native `haze_match`, `haze_ps_upload` / `haze_ps_restore`, `haze_census`, `haze_probe_other`, `haze_tick` (the "Heat haze" block before `ffxvr_on_draw`), their calls in `ffxvr_on_draw`'s scene-pass block, `ffxvr_eye2_begin` and `ffxvr_eye2_end`; managed `haze.cs`.

- **What it was.** Inside torch flames (Luca, lc/lchb0000 and others) eye 2 showed a cold grey-blue patch, read as a reflection; eye 1 was right. The flames' heat shimmer is a PS2-VFX primitive whose PS2 texture is the framebuffer ("FrameBuffer"), emulated with a copy of the frame: the game draws its colour buffer into a single-sample, scene-sized render target, and the next frame's haze quads sample that copy (it is one frame old in both eyes, the game's design). The copy is a draw into a twinned target, so eye 2 already samples its own copy. The quads are placed by the vertex shader (VS slot 0, a 256 B product-only buffer, which gets its eye-2 image like any other), but the copy's UV comes from the **pixel** shader's own constant buffer (PS slot 0, another 256 B product-only buffer). `maybe_override` patches that one for eye 1 and stores its eye-2 image in the ring, but `ffxvr_eye2_begin` only ever uploaded images into VS slots (PS constant-buffer binds are not tracked). So eye 2 drew the quads over its own torch and sampled its own copy at eye 1's screen position, about 240 px to the side at 960 wide (the torch's disparity between the canted Quest 3 frustums): sky and the arch's underside instead of the scene behind the flame. That the PS buffer is a separate buffer is inferred from the images; the census now prints both pointers (below), and the heartbeat counts the case where it is the same buffer.
- **The signature (`haze_match`).** Run in `ffxvr_on_draw`'s scene-pass block, so the draw is stereo, not in the overlay phase, into an MSAA RT0 with depth bound. Then, cheapest first: not a UI / ImGui / dream draw, not in the UI pass; the VS buffer's eye image is product-only (kind 2); blending on RT0 and depth writes off; and only then one `PSGetShaderResources(0, 8)`: a view whose texture is single-sample, a render target, exactly the scene target's size, not RT0 itself, and has a twin, and no slot samples the glass-break capture (HZ-3: this replaced a "no battle transition window" rule, which kept the old patch on spell haze for the battle's first seconds). What it cannot hit: water (camera-kind buffers, 4096x4096 reflection inputs), the Phyre distortion grid (no camera image, drawn outside the MSAA pass), the glass-break shards (no image, and they sample the capture), the dream filter (UI pass), the blitz scoreboard and radar (no image), the save sphere's refraction (camera-kind or no image). If a match turns out not to be haze, mode 1 still only gives it eye 2's own camera data, which is correct for any draw. There is no shader-pointer cache (the spec's option): the cost is a handful of context calls on the ~40 product-kind, blended, no-z-write draws a frame, and shader pointers are allocation addresses.
- **`heat_haze`** (ini and overlay; default 1; applied at the next Present). **Since the loads build the default is 0** (off in the headset): mode 1 did not fix the Luca torches in testing and is labelled experimental; the 10 s `haze:` line is written in modes 1 and 2 only:
  - 1, fixed per eye: for a matching draw, after the VS uploads, `eye2_begin` reads the real PS bindings (`PSGetConstantBuffers(0, 14)`) and uploads the eye-2 image of every PS buffer that has one this frame and is not bound in any VS slot (a buffer in both was already uploaded by the VS loop: counted, logged once). Before an upload both images must be exactly the live buffer's size. The reference is held until `eye2_end`, which writes eye 1's bytes back (with the VS restore, while the re-entrancy guard is still set). Other draws are untouched.
  - 0, off in the headset: matching draws return -1 from `ffxvr_on_draw` (not drawn, no eye-2 pass) while the headset shows the 3D view (`g_xr_want`, not suspended, not a virtual-screen or stereo-screen frame). In headset mode the monitor shows eye 1, the game's own backbuffer, so **the monitor loses the shimmer too** while the headset is on. Flat mode (no headset) is never touched; screen frames draw it as the game does. The fire sprites themselves stay.
  - 2, as before: no PS uploads, no skips (the A/B baseline). The signature still runs, so the counts and the census show matches.
- **Diagnostics.**
  - Census (step 0): every scene-draw line now ends its CB sizes with `| vs0 <ptr>(img|-) ps0 <ptr>(img|-)` and `same` when VS cb0 and PS cb0 are one buffer, and matching draws are tagged `HAZE`. Each matching draw adds a `HAZE (heat_haze M: …): screen copy sN <tex> WxH, twin <tex> | ps0 <ptr> 256B img, not a VS buffer (eye images differ): to upload …` line (written before `eye2_begin` runs, so it says what is to be uploaded, never that it was: HZ-5), then, for each PS buffer with its own image, the 64-byte blocks where eye 1's and eye 2's images differ (16 floats each, `ps0 +64 eye1: …` / `eye2: …`): the blocks that carry the projection. The blocks are dumped only for the first match of each (VS, PS) shader pair, at most 4 a frame, and the HAZE / HAZE? lines stop after 32 a frame (HZ-2), so a busy battle frame keeps the rest of the census. While a census is live, camera-kind draws that pass rules 4 and 5 are tagged `HAZE?(camera kind)` with a `HAZE?` line (never handled; HZ-4: tells a battle census whether spell haze uses camera-kind buffers). Expected at a torch: `vs0 X(img) ps0 Y(img)` with X ≠ Y. A `same` there means the UV does not come from a separate PS buffer, and mode 0 is the fix.
  - The census log is now opened shared (`_wfsopen`, `_SH_DENYNO`): a log viewer holding `ffxvr_frame.log` no longer makes the frame vanish; a failed open writes `census: could not open … frame N lost`.
  - `haze:` every 10 s in `ffxvr_diag.log` when anything was counted: the mode, matched draws and distinct shader pairs, PS uploads (no PS image `I`, PS buffer = VS buffer `S`, size mismatch), skipped, the rejections by rule (not product, blend / z-write, no screen copy, samples the glass-break capture), camera-kind near misses (census frames only), stale ring entries dropped, and **other eye-2 draws with a PS-only image `O` of the sampled ones** (one eye-2 draw in 32, rotating with the frame; counts only), with the first such draw's slot, size and kind. `S > 0` means the theory does not hold for those draws; `I > 0` means a ring miss; `O` decides whether to make the PS upload general (a `ps_cb_eye2` setting, not built): it counts non-haze draws whose pixel-shader camera data eye 2 never gets today. `haze: heat_haze A -> B at frame F` at each switch; one `haze: … is also VS slot …` line the first time a haze draw's PS buffer is a VS buffer.
  - Overlay: the main window's View block has the three-way row with the live `heat haze: N draws this frame, fixed (U eye-2 uploads) / matched, nothing uploaded / skipped` text (headset on; "fixed" only when uploads really happened, HZ-5); Debug > Heat haze has the same setting, an explanation and the session counts.
- **Stale ring entries (HZ-1, all eye-2 uploads).** `ffxvr_on_updatesub` now drops this frame's ring entries for a buffer whenever an upload of it takes no ring slot (no camera in it, nothing of ours to replace, a screen frame, the override off). Before, the older entry stayed "this frame": eye 2 (the VS loop and the PS upload alike) uploaded an older object's image, and `eye2_end` restored that object's eye-1 bytes over the game's. 32 pointer compares per such upload, under the lock it already holds. Counted in the `haze:` line as `stale ring entries dropped`.
- **Measure counters.** PS uploads and restores are not in `g_e2_uploads` / `g_e2_identical`, but count (twice per buffer) in the re-entered updates, so a Measure at a torch shows a few more re-entered updates than uploads explain.
- **ABI 22:** + `ffxvr_haze_set(int mode)`, `ffxvr_haze_status(int* out, int n)` (12 values: the mode, last frame's matches, uploads and skips, then the session's matches, uploads, no-image, same-buffer, other PS-only draws, sampled draws, of which the images differ, size mismatches).
- **Open points.** Fire spells and Ifrit use the same "FrameBuffer" path and should be fixed by the same rule; other spells (Thunder, Water, Ultima) may too. The one-frame-old copy is the game's and is left alone. A general PS upload for every eye-2 draw waits for the heartbeat's `O`.

## Loads: the lock release on every load, load: lines for every load, release defaults (branch `loads`)

Background: the load lock build (above). The owner's Besaid test with `load_lock_release=1`: battle entry 11.5-16.7 s down to 6.1 s, the way back about 3 s down to 0.9 s, no glitch, main-thread lock holders inside our window on 11 profiler ticks. Then a Luca area change (lc/lchb0000 to lc/lchb1800, an in-engine cutscene) sat on a dark screen for a long time: FFX-60fps logged a game-thread stall of about 1.75 s and the game did not advance for about 186 frames while the Presents went on. The release was off, and it did not cover such loads anyway. Code: native "Load lock build" block (`lr_release_begin` / `lr_release_end`, `ffxvr_load_info`), managed `load_lines.cs` (new), `load_diag.cs`, `load_prof.cs`, `diag7.cs`, `hd_cache.cs`.

- **Which loads arm the release** (as revised by the review, below: mode 1, the default, arms phases 1 and 2 only; mode 2 is the test mode that arms everything in this list). A load phase (the native `g_pf_phase`, opened and ended by the managed side, at most 60 s) or a loadPCluster call in progress on the loader thread:
  - 1 battle entry (MsBattleExe's wait to the entry's `load:` line) and 2 the way back (the teardown to the way-back line), as before;
  - 3 an area or event map load and 4 a load from the title (new). Their request is a **map free outside 0x909A20**: 0x909A20 has only two callers (the battle's state 8/9 code and MAPREG), while the field map loader 0x9093B0 (called through 0x908370 / 0x9085D0 from the event set-up 0x872EF0 and the event script's map commands 0x878510 / 0x878530 / 0x878550), the free-only 0x909E60 (from the event teardown 0x872D20) and 0x909680 call the map free 0x909EA0 directly. An area change frees twice (0x872D20's free, then 0x9093B0's); the second free is counted in the open load, not a new one. The phase is 4 when the event id at the request (0x12FBC40, Fahrenheit's `Globals.event_id`) is 0x17, the main menu (Fahrenheit's mod list uses the same test), else 3.
  - The load is **settled** when the render thread has seen the first frame with the new map's camera and then the loader idle (AsyncLoadManager's two task counts zero) for 10 Presents. "The new map's camera": a real perspective camera upload (`g_cam3d_total`, the projection test of the frame-type decision, counted in flat mode too) with 3+ scene draws, the game frame counter advancing, after the first free returned, and either a frame without a 3D camera since the request or another view matrix than the one at the free's return. Without such a frame (a movie, a menu), it settles when the loader has been idle for 300 Presents (about 5 s) with the game running throughout ("no new camera seen"); else after 60 s.
  - **The gates are the old ones**: the render thread, owner == the render thread, count 1, no release already open, the self-repair and session disable when anything looks wrong (LL-9), the stuck re-acquire check (LL-7, the watcher runs whenever the release is on). A loadPCluster call **outside** every load phase (a spell's textures streaming in during a battle, an NPC model) was first armed with a waiter; after the review (LD2-2) it is armed only in mode 2 and only while the main thread is blocked in the loader's wait-all.
  - A newer phase replaces an open one; `-v` ends phase v only if it is still the current one, so an area change that starts during a way back keeps the arm, and a battle phase opened during a field load is not ended by the field load's end.
- **Counted per load kind.** The render thread keeps the session's releases, contended re-acquires and re-acquires over 11 ms per kind (0 a loadPCluster alone, 1 entry, 2 way back, 3 area / event map, 4 from the title), never reset. `ffxvr_load_info` hands them over with the 3D-camera count, the last frame's scene draws and a hash of the last view matrix (25 values). Each `load:` line prints its load's share (the difference since its start); the managed side splits kind 3 into area / event map and kind 4 into save load / new game by the line's own kind. The main window's row shows the session counts.
- **`load:` lines for the other loads** (`diag_load_lines`, default 1; needs no hook beyond the load hooks):
  ```
  load: area change N (lc/lchb0000 -> lc/lchb1800, event 0x.. -> 0x.., field map A -> B): total X ms to the first frame with the new map's camera, settled at +Y ms | map frees 2 | HD clusters ... the M misses ... = open + read + parse | HD cache ... | blocking waits ... | frames: P Presents, longest Present gap G ms, game frame not advanced on S Presents (longest game-thread stall T ms) | lock release on: R releases (r in this load's phase, s for a loadPCluster alone), c contended re-acquires, l over 11 ms (late VR frames), Z ms re-acquiring in all
  ```
  The kind: from event 0x17 with Fahrenheit's save / load screen latched open in the 10 s before the request = **save load**; from 0x17 otherwise = **new game (from the title)**; to 0x17 = **to the title** (the boot, a quit, a game over); another event id = **area change**; the same event id = **event map change** (a cutscene's own map, a script map switch). The battle lines get the same `frames:` and `lock release` parts. The Present tracker measures at our Present (render thread): the longest gap between two Presents, and the Presents on which the game frame counter 0x1FCBBF0 had not moved, with the longest such stall in ms (the game thread stuck while the render thread presents: the owner's 186 frames).
- **Release defaults.** Every diagnostic is off by default: `diag_build7`, `diag_load`, `diag_transition`, `diag_load_prof` (0), `diag_cluster_bytes`, `diag_va_log`. The cheap `load:` lines stay on behind `diag_load_lines=1`. `load_lock_release` defaults to 1 (battle loads, after the review) and is saved again (LL-2's "always write 0 / force 0 at launch" is gone; the overlay toggle stays). `hd_cluster_cache` stays 0. `heat_haze` defaults to 0 (off in the headset); mode 1 is labelled experimental (it did not fix the Luca torches), and the 10 s `haze:` line is written in modes 1 and 2 only.
- **Hooks follow their features, not the diagnostics.** At launch:

  | Feature | Hooks | ini key that installs them |
  |---|---|---|
  | Cursor stage C2 (game source) | target pointer 0x49C870, turn pointer 0x492590 | `cursor_source=game` (default) or `diag_build7=1` |
  | Smooth anchor | job join 0x644B30 (queued last of these four) | `smooth_anchor=1` (default) or `diag_build7=1` |
  | Backbuffer invalidation (build 9) | resize 0x199210 | always |
  | Haptics | XInputSetState IAT | `haptics=1` (default) or `diag_build7=1` (as before) |
  | Load phases, the lock release, the `load:` lines, the HD cache | teardown, map load / free / build, chr flush, wait-all, chr model / motion, loadPCluster, open, read, post fixups | any of `diag_load_lines`, `load_lock_release`, `hd_cluster_cache`, `diag_load` |
  | Pool bytes | pool alloc / free | `diag_cluster_bytes=1` |
  | Profiler probes | 0x465AB0, 0x58FC20 (last) | `diag_load_prof` > 0 |
  | Glass break, dream overlay, adaptive depth, heat haze | none (native, D3D-level and memory polls) | - |

  The queue order is unchanged (build 7's hooks with the join last, then the load hooks, the pool hooks, the probes). `diag_build7` now only switches the build-7 logs and the stall watchdog (live). `diag_load` adds, at once, what costs: the address-space walks at each battle start and end, the chr cache table samples, the wait-all callers' stack walks, the hooks-seen line, and, with the cache off, the HD cache's per-map-free measurement (two address-space walks, the manager pool copy) and its HDC: and sample lines. With the cache off and nothing held, a map free now only keeps the pool's base (the settled test's input); the full path comes back at the first map free with the cache on, and its caps stay 0 until then.
- **ABI 23:** + `ffxvr_load_info(int* out, int n)` (ABI 24 after the review: 60 values).
- **Risks.**
  - The release now runs during area / event / title loads and mid-battle cluster loads, where the main thread is busier than in a battle entry: more chances for LL-6's main-thread lock work mid-list (at worst a one-frame glitch). Each line's lock part and the overlay's per-kind counts show where releases happen.
  - The settle and "new camera" rules are inferred, not measured: a load whose first new frame reuses the old view matrix with no 2D frame in between gets no camera time ("no frame with a new 3D camera") and settles by the 300-Present idle rule.
  - A save load is told from a new game only by Fahrenheit's load screen having been open in the 10 s before the request.

### Review fixes (loads, LD2-1 .. LD2-10)

- **LD2-1, three modes.** `load_lock_release`: 0 off; **1 battle loads (the default)**: phases 1 and 2 only, the case the Besaid test covered; **2 all loads (test)**: also phases 3 and 4 and the loadPCluster-alone case. The main window offers off / battle loads; "all loads (test)" is in Debug > Battle loading only, and it is never saved (re-review R-3): `ini_text` writes it as 1 and a launch read turns a 2 into 1, so every launch is back on the tested mode (a hand edit while the game runs still applies at once). The main window says so while mode 2 is on. The static audit (review) found no lock holder that waits on render progress while holding 0xC94EE4 on the zone, event, save, title or battle-cluster paths, but a hang there cannot be recovered, so the wider scope waits for field evidence.
- **LD2-2, a loadPCluster alone.** Only in mode 2, and only while the game's main thread is blocked in the loader's wait-all 0x76C490 (managed `hl_wait` on the main thread sends `ffxvr_prof_mark` bit 2 in and out; native counts `g_pf_main_wait`). That wait is the stall the release can shorten, and the main thread was blocked at the release. It is a point-in-time check (re-review): the wait-all can return inside the same window, and the main thread can then take the lock mid-list; each line counts that as `a loadPCluster alone: MAIN n`. With that gate a cluster load in progress is enough (no waiter needed), as in a load phase.
- **LD2-3.** The native phase of an area / event / title load ends at the first frame with the new map's camera; the `load:` line still waits for the settle. The release then covers the black part of the load only (in mode 2), not the cutscene's first seconds. The line says "release window: to +X ms" in mode 2.
- **LD2-4, the holders on every line.** `ffxvr_load_info` has 60 values: per load kind also the holder seen at the contended re-acquires (loader / main / other) and the watcher's looks at the owner inside our windows (loader / MAIN / other / free). The watcher waits 1 ms instead of 20 ms whenever a release may open (the release on and a load phase or a cluster load), and at each wake-up with a window open it reads the lock's owner word (a plain read of the lock object checked at init) and counts it under the window's kind, with or without `diag_load_prof`. Every `load:` line prints both for its own kind (plus kind 0's main looks). MAIN > 0 is main-thread work mid-list: the one-frame-glitch risk.
- **LD2-5, `ini_rev`.** `ini_text` writes `ini_rev=1` first. A launch read (also the late one) of an ini without it, or older, replaces only the old defaults, once (re-review R-2): `load_lock_release=0` becomes 1 (every build before 76ce742 wrote LL-2's forced 0, and 76ce742 never reached a player, so a 0 there is never a choice), `heat_haze=1` becomes 0 (the haze build's default), and `heat_haze` 0 or 2 stay. It logs `settings: ffxvr.ini rev 0 (< 1, …): migrated once: …` with what changed. The `diag_*` keys are not touched; every launch logs `settings: diag_* keys on: …` (core.log and, at the first ready frame, `ffxvr_diag.log`).
- **LD2-6.** The main thread's id is captured at the first map free too (the boot's title load), not only at 0x909A20 (battles), and named to the native side (`ffxvr_prof_mark(2)`). Native ignores loader marks from the main thread, and the owner classes test main before loader, so before the first battle the main thread is no longer counted as "the loader" and cannot arm the cluster-load case.
- **LD2-7.** `fl_log` copies the HD baseline, the wait index, the free count and the new event / field ids before it drops `_fl_open`, so a load that starts right after another cannot mix into its line.
- **LD2-8.** The `actors:` line (Fahrenheit's log, every 10 s) is behind the new `diag_actors` (default 0). The stuck re-acquire line now says "(still waiting; a slow load can do this: a hang only if an xr: STALL … 'lock re-acquire' line follows)", and a second line comes at 5 s ("STILL waiting after 5 s").
- **LD2-9.** "over 11 ms (late VR frames)" is now "re-acquires over 11 ms" (11 ms is the 90 Hz frame; at 72 Hz the limit is 13.9 ms). Whether `game frame not advanced on S Presents` means stalls or the 30 Hz logic rate is to be read from a field line with no load (S near 0 means stalls).
- **LD2-10.** Comment: the join is no longer our last insertion in Fahrenheit's commit (the load hooks follow it), and the resize hook is queued whatever diag_build7 says.
- **ABI 24:** `ffxvr_load_info` returns 60 values, `load_lock_release` takes 2, `ffxvr_prof_mark` bit 2.
- **Re-review (R-1 .. R-3).** The watcher looks at the owner about every 1 ms only when a release can actually open (`lr_may_arm`: a window open; mode 1 with phase 1 / 2 live; mode 2 with any phase live or a cluster load while the main thread waits), else at its old 20 ms rate. The wait-all gate is a point-in-time check: the main thread was blocked when the lock was released, but the wait-all can return inside the same window (the loader finishes thanks to the release), and the main thread can then take the lock mid-list; the comments say so, and each line counts it as `a loadPCluster alone: MAIN n`. R-2 and R-3 are in the LD2-1 and LD2-5 bullets above.

## First person: a hidden Tidus model ranks below a visible one (FP-2, 26 Oct 2026)

In the Luca cutscene `lc/lchb1800` the actor table holds two Tidus models: the
field model `c001` at actor 0, script-hidden (bit 0x01 of actor+0x180, not drawn,
parked off-stage at about (-234, -60, -4)), and the event model `c101` at actor 12,
visible and moving. `fp_actor=c001,c101` ranked c001 first, so first person stayed
on the hidden, parked actor. A listed model with that bit now scores 500 + its rank:
after every visible listed model, still before the party fallback. Bits 0x08 and 0x20
of the same byte (the frustum and distance culls, see the code patches) are not
counted; bits 0x02 and 0x10, the game's other persistent script hides, are (FP-H1). While the eyes are on a hidden listed model the table is rescanned every
frame, so the visible one is taken at once and the field model comes back when the
event model leaves.

## Stick reprojection: extra frames at the headset rate (branch `stickrep`)

Reported: stick turns ghost (a double image) while head turns are clean. Research `stick-reproject.md` (28 Oct) confirmed
the cause: one OpenXR frame per game Present (about 59.5/s), the stick yaw integrated once per Present and baked into the
camera, so every refresh without a new game frame re-shows the last one, which VD reprojects for the head only. At 120 Hz
every image is seen twice. Code: native "Stick reprojection" block (end of `ffxvr_native.cpp`), `turn_advance` and the
`SrFrame` record (near `turn_by` / in `build_override_view`), `ffxvr_xr.cpp` (`wait_begin_locate`, `xr_extra_frame`),
managed `stickrep.cs` (new).

- **Extra frames.** The render loop's idle wait between command lists (0x616060, FhUtil 0x216060: `call 0x4302A0`,
  PEvent::wait INFINITE) is patched once, at headset start, to `rt_ring_wait` (a `__fastcall` replacement, ECX = the
  PEvent, returns 0; an aligned 8-byte compare-exchange after checking the render loop's prologue, the instructions around
  the call, the call's target and the target's body against FFX.exe; never unpatched, disarmed by the settings). While no
  list is queued, it waits on {the ring event, a high-resolution waitable timer}; at the open frame's deadline it sends an
  **extra frame**: xrEndFrame with exactly the layers the last Present sent (the base from R8b's `g_held`, the UI panel and
  the ImGui quad from `g_last_sent`), re-referencing the swapchain images that Present released (no acquire, no copy:
  VDXR resubmits an unchanged image without a copy), then xrWaitFrame -> xrBeginFrame -> xrSyncActions -> xrLocateViews
  (`wait_begin_locate`, shared with `xr_frame`), `xr_apply_poses`, the controller state. The next game frame renders with
  that slot's head pose and yaw; the next Present ends the frame the extra began.
- **The rotation.** The projection layer's two poses are turned by the stick yaw added since the held image was rendered:
  `d = g_turn_accum - img.accum` (every heading change `turn_by` makes is accumulated), `Q = Rb(yaw + d, pitch) Rb(yaw,
  pitch)^T` (the seat frame, `seat_frame`), moved to LOCAL with the image's reference (`Q_L = ref_q Q ref_q^-1`), about the
  image's turn pivot `c_L = ref_p + ref_q (-mv'/sc)` (the reference origin in first person and the seat; the player in third
  person and the diorama). `orientation' = Q_L orientation`, `position' = c_L + Q_L (position - c_L)`, renormalised. A
  host-side test (20000 random yaw / pitch / head / reference / offset cases) confirms the sign and the axis: far points land
  where a fresh render puts them (0.003 deg, float), and exactly for every distance when the pivot is the eye.
  `SrFrame` is captured in `build_override_view` at the frame's first eye-1 3D camera upload (`advance`), and becomes the
  held image's record at a Present whose base went out fresh. **Off** (a plain hold, unturned) for: screens, the stereo
  screen, the save screen, a backdrop room, frames with no latched heading, the glass-break pin (`xr_pinned` /
  `g_bt_frozen`), another heading epoch (`g_sr_epoch`: latch resets and re-latches, cuts, recentres, mode changes, the
  pin taken or released, the headset stopping), third person and the diorama below `stick_reproject=2`.
- **The yaw on the display clock.** With `stick_reproject` > 0 the stick and the Numpad keys (now `ffxvr_turn_keys`, -1 /
  0 / +1 every frame, instead of managed's per-frame `ffxvr_turn(rate * dt)`) are integrated by `turn_advance(T)` after
  every xrBeginFrame (xr_tick after `xr_frame`, and every extra) against the predicted display time T: `dt = T - T_prev`
  (at most 50 ms), the same `x |x| rate` curve and deg/s, so the tuned turn speed is unchanged. In an extra the yaw stops
  `delta_max = max(3 deg, 2.5 x rate x the game frame time)` past the held image (`g_turn_frozen`, cleared by the next
  fresh frame): during a load the view turns at most a few degrees and then stays, and does not jump when frames return.
  With `stick_reproject=0`, or without a running session, `turn_tick` uses the Present interval as before.
- **Threads and pacing.** Everything is on the render thread: the extras in the ring wait, between lists; `xr_frame` in
  the Present list. One OpenXR call at a time, always End -> Wait -> Begin, one Wait per slot; a Begin that fails after its
  Wait is retried by the next call without a second Wait (VDXR would block that Wait for good). No extra once the next
  frame has started (a draw, a clear or a camera upload after the Present list: counters snapshotted at the first ring wait
  after a Present, so ImGui's draws in the Present list do not count). A queued list is never delayed: the ring event is
  checked with a zero timeout first and before every extra. The game's render-interface lock (0xC94EE4) is taken around
  the extra's xrEndFrame with a try (the game's own protocol: compare-exchange the owner, then the count; busy = retry in
  0.5 ms, then skip the slot), given up around its xrWaitFrame, and taken back with the game's acquire for Begin / sync /
  locate. **The flush gate:** managed pre-hooks the full flush 0x615EA0 (gate set) and post-hooks the kick 0x615210 (gate
  cleared), both on the main thread, queued last of all our hooks after their prologues were checked with guarded reads;
  originals resolved as B7-1 (render thread before our late insertions, else the detour itself via `d7_resolve_main`). No
  extra while the gate is set, unless for 250 ms with the immediate context's multithread protection on (the backstop,
  `keepalive_mt_protect`, `SetMultithreadProtected(TRUE)` at headset start; no Enter/Leave around VDXR's calls: a VDXR
  configured to submit on the app's device would deadlock on it). **The deadline** locks onto VDXR's wait: a blocking
  xrWaitFrame returns at VDXR's running start, 2 ms before the slot, so the open frame's deadline is `w + P - margin` (w its
  wait's return, P `predictedDisplayPeriod`, margin `stick_reproject_margin_ms`, 2.5); a wait that returned at once (late)
  takes the next slot by the phase of the last blocking wait. A slot whose deadline passed by more than the margin (a late
  wake-up, the gate, a busy lock, a frame already drawing) is skipped, never filled late; an extra after skipped slots
  advances the yaw to its real display slot. **Predictive:** a list that arrives too late for the open slot (now + c_est >
  deadline; c_est = the 90th percentile of a Present list's start-to-Present time) gets the extra first.
- **Layers.** Head-locked quads (the UI panel, the ImGui layer) and the save screen's world-locked ImGui quad are re-sent as
  the last Present sent them, for 150 ms after the last Present (a load then shows the bare frame: a stale menu is worse);
  screens are held unturned; zero layers after 30 s without a fresh frame (a runtime that dislikes long re-sends). If the
  last Present sent no base (R8b's 30-frame cap, a mode change), no extra is sent. shouldRender false: no extras.
- **The loading keepalive** is the same path: the extras go on while the game stalls between frames, head-tracked,
  turned at most `delta_max`. `keepalive=1` with `stick_reproject=0` sends unturned extras only after max(25 ms, 1.6 game
  frames) without a Present, then at the slot deadlines until the next Present.
- **Settings.** `stick_reproject` 0 off / **1 first person + seat (default)** / 2 also third person + diorama (main window,
  under the turn speed: "smooth stick turns at the headset rate" with "first person + seat / all views"); Debug >
  "Stick reprojection and the loading keepalive": `stick_reproject_margin_ms` (0.5-6, 2.5), `stick_reproject_predict` (1),
  `stick_overscan` (0-4 deg per side, 0: widens `g_xr_fov_used` sideways, the FOV the frame is rendered and submitted
  with), `keepalive` (0), `keepalive_mt_protect` (1, applied once at the session's first headset start), the direction test (+5 deg right on every extra,
  not saved), "stall the main thread 3 s" (a Sleep in the job-join detour). The flush gate hooks are installed at launch
  when `stick_reproject` or `keepalive` is on.
- **Diagnostics.** `xr: display period 8.33 ms (120.0 Hz; runtime refresh 120.0 Hz)` at the first wait and every change,
  with `; half rate: VD spacewarp (SSW) active` when the period is about twice the refresh period (XR_FB_display_refresh_rate
  and XR_KHR_win32_convert_performance_counter_time are enabled when VDXR lists them; xrCreateInstance falls back to D3D11
  alone). Every 10 s with the headset on: `sr: 10 s | mode 1 | period ... | slots N: fresh F (late L), extras E = x/s
  (predictive, zero-layer, failed), skipped S (by us: past the deadline, mid-frame) | yaw per extra avg / max deg
  (rotating %, clamped) | extra cpu avg / max ms (end, wait, begin+locate), lock busy, re-acquire max, gate skips
  (overridden) | list start delay avg / max (n lists) | c_est | phase jitter | photon latency | copies 0`, plus `not active:
  <why>` when it is not running. `sr: hold 1.42 s, 171 extras, yaw frozen at delta_max (max 3.1 deg), quads dropped after
  150 ms` for a gap of 100 ms or more between Presents. The `perf:` line ends with `| sr extras N/s`; the overlay row
  shows extras/s, missed slots/s and the period.
- **ABI 25:** + `ffxvr_sr_set`, `ffxvr_sr_gate`, `ffxvr_sr_status`, `ffxvr_turn_keys` (`ffxvr_turn` stays exported).
- **Risks.** Pacing against VD's closed-source compositor (the deadline is inferred from VDXR's running start: watch
  skipped and late); a list delayed by an extra's xrWaitFrame (the list start delay; FFX-60fps's PERF-002 step-down); an
  immediate-context user the gate does not see (MT protection is the backstop); VDXR's xrEndFrame now also runs from the
  ring wait while the game's lock is held by us; third person / diorama at level 2 are approximate (the player double-images,
  the table slides); a load that stalls mid-frame (a frame that has started drawing) still goes black.

### Review fixes (stickrep, SR-1 .. SR-14)

- **Default 1, a deviation from the spec** (3.1 asked for 0 in the first test build): the in-session A/B needs the flush
  gate hooks, which are installed only when the feature is on at launch. Unticking it in the overlay is today's behaviour.
- **SR-1, exit.** `sr_extra` counts itself in flight (`g_sr_in_extra`) and backs out when `g_sr_exiting` is set; the XrHost
  `extra_abort` callback stops an extra before its xrEndFrame, its xrWaitFrame and its xrBeginFrame. `ffxvr_shutdown` (the
  ExitProcess detour) sets the flag, then waits up to 100 ms (1 ms sleeps) for no extra in flight before `xr_stop`. The
  window-close path: the game's window (the largest visible unowned top-level window of the process, found at arming) is
  checked with `IsWindow` before every extra and at each of its OpenXR calls; once it is gone no extra runs.
- **SR-2, the keepalive-only spin.** The keepalive threshold moves on with every skipped slot (`+ skip_k max(P, 8 ms)`); a
  deadline passed by several slots is skipped in one pass (`skip_k += k`, every lost slot counted); a run of more than 32
  passes or 20 ms without a wait falls back to the game's wait for that list and logs `sr: the ring wait passed N times
  without waiting ...` (at most 20 lines).
- **SR-3, c_est** is measured from the frame's first list (the first list after a Present) to the Present; the old measure
  (the Present list's own span) is on the sr: line as "last list" for comparison.
- **SR-4, the Present's wait.** With extras running, the frame open at a Present was begun by an extra, so the Present's
  xrWaitFrame blocks to VDXR's next running start (2-8 ms at 120 Hz) inside the Present list, with the game's lock held
  and ahead of the 1/1 present semaphore. Not changed now; the sr: line shows `Present wait avg / max` and the extras'
  `lock hold avg / max`, and the owner test asks for V-Sync off and watches the `perf:` wait and FFX-60fps's PERF-002.
  Cheap mitigation, documented only: run the load lock build's release (`lr_release_begin`'s own gates: the render thread,
  owner us, count 1) around that wait whenever extras are armed, not only in armed load phases; it would drop the hold but
  carries LL-6's mid-list main-thread exposure to every frame. The structural fix for later: when extras are armed, the
  Present only ends the frame, and the ring wait runs the Wait / Begin with nothing held before the next list.
- **SR-5.** Each `sr: hold` line says how many slots the hold lost to the game's lock, the flush gate, a started frame and a
  late wake-up. The try-lock is not a registered waiter; if the lock count is high in loads, the options are an untried
  lock-free send under multithread protection or the game's own (blocking) acquire.
- **SR-6.** The extra frames' yaw bound (delta_max) applies in every hold: against the held image's record when it is a
  live view, else against the accumulator at the last fresh frame (a screen, a backdrop room, a seat not latched yet).
- **SR-7.** A retried Begin refused with `XR_ERROR_CALL_ORDER_INVALID` clears the pending flag (the next call waits); a
  session that reaches READY with a frame waited but not begun is stopped (VDXR keeps its frame counters across a stop;
  the start back-off recreates the session). STOPPING no longer clears the flag.
- **SR-8.** The save screen's world-locked ImGui quad is re-sent in extras whatever the 150 ms quad rule says.
- **SR-9.** Multithread protection is switched on only while the flush gate is free, and the `ID3D11Multithread` interface
  is released at once. The owner test has an A/B with `keepalive_mt_protect=0`.
- **SR-10.** The flushing thread is recorded; only its own next kick clears the gate. A flush from a thread that is not the
  game's main thread is logged once.
- **SR-12.** For a wait that returned at once, the open frame's slot is the grid point at or after the wait's return only
  within VDXR's 2 ms running start before it, else the one before; the phase anchor is dropped when the headset stops.
- **SR-11, SR-13:** no change (the VD bitrate question is in the test plan; `g_in_eye2` stays process-wide: only the gate
  override window could expose it, and VDXR's extra End issues no call we hook).
- **Re-review (SR-R-1, SR-R-2, SR-R-6).** A sent extra ends a spin run (it waited in xrWaitFrame and the lock re-take), so
  the 32-pass / 20 ms cap bounds only the skip and mid-frame paths; an extra over 20 ms is logged on its own (`sr: a slow
  extra frame: ...`, the sr: line's `slow extras`) and the loop carries on. When the window found at arming is gone, the
  search runs again (consoles excluded); "gone" latches only when the game has no visible top-level window left. The
  catch-up skip count is clamped before its integer cast. SR-R-3 / 4 / 5 wait for the owner's logs.

### Session 1 of stickrep (29 Oct 2026): the headset stuck on the virtual screen (SR-X1)

The first session with 7603645 showed the game flat on the virtual screen from the start, with the
save screen missing. The log: every frame "mode -> SCREEN", `camera uploads 0`, `draws 0 eye2 0`, and
eye 2 "no image 16923, ok 0" while the draw hooks counted 14,000 DrawIndexed calls and the view matrix
changed every frame. Cause: `g_in_eye2` was a process-wide global. Every guarded section used to run
inside the Present hook, when the main thread uploads nothing; the extra frames run in the render
thread's ring wait, between command lists, exactly while the main thread uploads the next frame's
constant buffers. With the guard set for the whole of each extra (its xrWaitFrame blocks up to a
display period, 60-120 times a second), `maybe_override`'s `if (g_in_eye2) return src;` dropped most
camera uploads: no 3D camera counted, the screen decision never flipped back, no override, no eye-2
images. Fix: the guard is `thread_local`; it only ever meant "this thread is inside our own work".
Also seen: extras during the Luca load took 50-130 ms waiting to re-take the game's lock from the
loader (the SR-5 cost); no hang.

### Session 1 of stickrep, the real cause (29 Oct 2026): multithread protection hid the game from the mod (SR-X2)

The per-thread guard (SR-X1) was correct but not the cause. The decisive evidence is the native log's
session counters: in the broken runs the detours saw DrawIndexed 14,092 and UpdateSubresource 1,266;
in the working run (the same build, `stick_reproject=0 keepalive_mt_protect=0`) 1,047,878 and 3,497,937.
The game rendered normally the whole time; the mod saw about 1% of its draws and none of its camera
uploads, and the few it saw were the second before `sr_arm`. Cause: `SetMultithreadProtected(TRUE)`
on the immediate context (the "backstop", SR-9) makes the D3D11 runtime route the context's hot entry
points through a different, locking set of function bodies; Fahrenheit's inline detours sit on the
bodies read from the vtable at init, so the game's calls no longer passed through them. No camera ->
"2D" -> the virtual screen for the session, no eye copies, no save screen. Fixed by never turning the
protection on (`sr_mt_init` only reports the state; `keepalive_mt_protect` is read and ignored,
written 0). The flush gate is the protection, as the review had accepted; without MT protection the
gate is never overridden (an extra always yields to the main thread). The post-mortem, with the
alternatives weighed and the presenter-thread end state (B2) recommended, is research/stickrep-postmortem.md
in the session scratchpad; its substance is summarised here.

## TODO, next big item: the presenter thread (B2) — the long-term fix for stick-turn ghosting (29 Oct 2026)

**Status.** Parked by the owner: the stick reprojection shipped in aa650ec (the ring-wait design, A) is "a
significant improvement, but not a full fix". The reason it cannot be a full fix is structural: an extra frame
is only delivered when the game's render thread happens to be idle in its ring wait, the game's lock is free and
the flush gate is clear, so some display slots are still missed, and during loads the lock re-takes cost up to
155 ms. Head turning never ghosts because the runtime reprojects the head pose on *every* refresh; the design
below puts stick yaw on that same path on every refresh, which is what the owner's constraint asks for.

**The owner's constraint, restated.** Head motion never ghosts; only right-stick rotation does. Any design must
leave head handling untouched and make stick turning as clean as head turning (not just better than today).
Tested headset settings: VD 120 Hz, SSW Disabled, game V-Sync off.

**What was tried and why it is not the end state** (full post-mortem: the "Session 1 of stickrep" sections above):
- A (current): re-send the last frame from the render thread's ring wait. Works; delivery is phase-dependent.
- B1 (a submit thread on the game's device): needs the context's multithread protection on *before* the detours
  are installed (SR-X2 shows why it is otherwise fatal), keeps the GPU fence coupling, breaks under VDXR's
  application-device quirk. Rejected.
- fps=120 in FFX-60fps: not feasible at 4K (gpu 10-13 ms/frame); any missed frame brings the double image back.
- VD SSW Always: image-based smear, not the head path; useful only as a side-by-side reference.

**B2, the recommended end state: a presenter thread with its own D3D11 device.**
The OpenXR session is created on a device we own; the presenter thread owns xrWaitFrame / xrBeginFrame /
xrSyncActions / xrLocateViews / xrEndFrame entirely and runs them at the display rate. The game's render thread
never calls OpenXR and VDXR never touches the game's context, so the ring-wait patch, the flush gate, the lock
juggling, MT protection and the InEye2-around-runtime-calls all go away; the only shared state is a mailbox.
VDXR itself already does this cross-device handoff on this machine (its own submission device), so the sharing
path is proven on this GPU and runtime. Cost: one extra copy of each eye per game frame (~0.1 ms GPU), one fence
per frame; the render thread gets its Present wait back. Effort estimate: a few days.

**Spec for the implementer (B2)**
1. Session on our device: `D3D11CreateDevice` on the game's adapter LUID (DXGI from the game's device),
   `XrGraphicsBindingD3D11KHR` bound to it. `xr_create` / `xr_stop` unchanged otherwise.
2. Shared frame set `SrShared`: per logical image (eye 1, eye 2, screen, screen R, UI, ImGui) a texture on our
   device (`MISC_SHARED_NTHANDLE`, exact swapchain format), opened on the game's device; one `ID3D11Fence` created
   on our device (`D3D11_FENCE_FLAG_SHARED`), opened on the game's (`OpenSharedFence`). Two sets, ping-pong, so a
   copy never overwrites the image the presenter is re-sending.
3. Game side (`xr_tick`, Present hook, render thread): choose the set not in use, `CopyResource` / stretch-copy
   the sources into it under `InEye2` (the current `submit_set`/`submit_eye` sources), `Signal(fence, ++n)` on
   the game's context, publish `SrPublish{n, set, layers wanted, poses used, FOV used, screen/ui/imgui quads,
   SrFrame record}` by pointer swap (`InterlockedExchangePointer`). Never block.
4. Presenter thread (above-normal priority, MMCSS "Games"): loop `xrWaitFrame` -> take the latest `SrPublish`;
   if `n` is new: `Wait(fence, n)` on our context, acquire + copy + release the swapchain images, remember the
   layers; else re-send the remembered layers with `rotate_pose(sr_rotation(display_time))` and the quads as
   `xr_extra_frame` does (quads dropped after 150 ms without a fresh frame, zero layers after 30 s, as now).
   `xrEndFrame`, `xrBeginFrame`, `xrSyncActions`, `xrLocateViews(display_time)`; publish
   `SrSlot{display_time, period, views[2], fov[2], input}` by pointer swap.
5. Render thread at `ffxvr_begin_frame`: `xr_apply_poses(latest SrSlot)`; yaw clock `turn_advance(T)` with `T` =
   the slot this frame will land on (`display_time + k*period`, `k` from `now + c_est` against the slot grid).
6. Remove: the 0x616060 patch, `sr_wait_list_or`, the flush gate hooks, `sr_mt_init`, the Phyre lock
   take/release in extras, `InEye2` around `xr_frame` (keep it around our own copies).
7. Keep: the yaw clock, `sr_rotation`, the epoch rules, the pin, the hold rules, the sr: line (from the presenter).
8. Shutdown: `ffxvr_shutdown` sets exiting, joins the presenter (bounded), then `xr_stop`, then releases the set.
9. Diagnostics: per 10 s, slots fresh / extra / skipped-by-runtime (from `predictedDisplayTime` gaps), fence wait
   time, presenter CPU per slot, yaw per extra, `late fresh` (a fresh frame published after its slot).
Risks and their guards: device-loss / shutdown ordering (stop the presenter before `xr_stop`, before the game
device is torn down; `g_sr_exiting` and the in-flight count exist); sRGB / typeless format matching across devices
(create the shared textures with the exact swapchain format, as `ensure_copy` does); the fence needs
`ID3D11Device5` (Windows 10 1703+; VDXR already requires it on this machine).

**Expected result when done:** every display slot gets a frame; stick turning indistinguishable from head
turning; no black loading screens; the `sr:` line shows extras ~= slots - fresh with 0 skipped-by-us.

## Fahrenheit port: release v1.0.0-alpha12 (branch `fhport`, 29 Oct 2026)

Plan: research/fh-port-plan.md (revision 2) and its review, in the session scratchpad. Code: managed `dpi_apply`,
`our_font_px`, `text_scale_step`, `render_imgui` (module.cs), `esc` (diag7.cs); ini `fh_dpi_fix`, `overlay_text_scale`;
`src/managed/nuget.config`; tools `paths.ps1`, `build-mod.ps1`, `install-fahrenheit.ps1`, `build-fahrenheit-full.ps1`,
`check-prereqs.ps1`, `package-release.ps1`; packaging `install-mod.ps1`, `README-zip.md`. No native change (ABI 25).

- **Target: the official release tag `v1.0.0-alpha12`** (7e85839b2efb67124d1760598c9f220cb559a41a, a lightweight tag on
  main), not main (ff743f0), and not the old pin ecb6702.
  - It is the only Fahrenheit players can download: a maintainer-built, Authenticode-signed zip plus the NuGet packages.
    Nobody has to build Fahrenheit any more.
  - From ecb6702 to the tag the API we compile against is identical, and so is the SDK package (`src/sdk` and
    `Directory.Build.*` diffs are empty). The hooks, the Present path and the loaders are the same apart from #369.
  - **Upstream effects at the tag.** #366, the chain-registry lock (`c2fe4fc`; `4caa85d` drops its per-lookup log lines):
    a benefit, the torn-read risk noted in diag7.cs is gone. #369 (stage-0 dumping under WINE): `Fahrenheit.Stage0.vcxproj`
    copies a third debugger DLL, `dbgcore.dll`, so a source build without the Debugging Tools fails one more MSB3030 copy
    (build-fahrenheit-full knows it). The save screen's help texture lives under `/map/hiku`, so the HD cluster cache
    admits it like a map texture and releases it at the next zone change: harmless, left as is.
- **FH-1: Fahrenheit's ImGui DPI font scaling off** (ini `fh_dpi_fix`, default 1 for everyone, applies at once; Debug row).
  - Fahrenheit alpha12 sets `io.ConfigDpiScaleFonts` / `ConfigDpiScaleViewports = true` once, in `platform_bind`
    (runtime/imgui.cs:90-91, from the game's `D3D11CreateDeviceAndSwapChain` hook). With the flag on, ImGui 1.92.9b's
    `SetCurrentViewport` sets `style.FontScaleDpi` to the viewport's DPI scale, so every font is multiplied by the Windows
    display scale although the game is DPI-unaware: at 175 % (the owner's) and 4K, the save screen's 86 px text is drawn
    at about 150 px and outgrows its frames. Upstream main turned both off (#376, 5173f28); alpha13 will ship that.
  - Our `render_imgui` runs only from Fahrenheit's Present hook, after `NewFrame`, so our write always follows Fahrenheit's,
    and nothing writes the flags again. `dpi_apply` (every frame, writes on an edge only): on the first frame it logs
    Fahrenheit's values; with `fh_dpi_fix=1` and either flag on it turns both off and sets `FontScaleDpi = 1` once (ImGui
    leaves it as it was when the flag goes off); a re-made context or someone turning them on again is handled the same way,
    logged 5 times, then only counted. `fh_dpi_fix=0` puts Fahrenheit's own values back; ImGui re-applies the scale at the
    next frame (`SetCurrentViewport` runs at the first viewport change of a frame). Where Fahrenheit already has the flags
    off (alpha13) nothing is written.
  - **A bounded exception to "never write Fahrenheit's ImGui state"** (the saveui rule, above). The rule kept the monitor
    exactly as Fahrenheit draws it. Upstream has made this exact change for everyone, so taking it early only changes
    alpha12's monitor look to alpha13's: the save screen fits its frames again, and Fahrenheit's F7 menu and title mod list
    are smaller than before above 100 %. We write the two flags and one `FontScaleDpi`, nothing else.
  - **One mixed frame at the flip.** Modules that drew before ours in the frame where the flags flip used the old scale.
    Harmless, documented, not fixed.
- **FH-2: our own text keeps its size** (20 px x the Windows scale), and `overlay_text_scale` adjusts it.
  - One `ImGui.PushFont(null, size)` around all our ImGui output (F8 window, Eye 2, markers, hover labels, diagnostics),
    popped in a `finally` before Fahrenheit's next module; the push is outside the `try`, so a Pop never runs without it.
    NULL keeps the current font. The pushed size is applied before `FontScaleDpi` (imgui.cpp:9836) and also sets the
    foreground draw list's text size, so the markers follow it.
  - `size = 20 x DpiScale x overlay_text_scale / FontScaleDpi`, computed once per frame **before** the push. The
    `FontSizeBase` trap: `UpdateCurrentFontSize` copies the pushed size into `style.FontSizeBase` (imgui.cpp:9826), so
    reading it inside our push would compound (45 px instead of 30 at 150 %). The base is Fahrenheit's constant 20 px
    (`AddFontFromFileTTF(..., 20f)`, core/gui/gui.cs:38). `viewport->DpiScale` is computed whatever the flags are.
  - Result: with `fh_dpi_fix` on or off the F8 overlay is the size it had on stickrep (35 px at 175 %), which is what the
    headset sizes were tuned with. On the monitor that keeps the size #376 calls oversize, for our window only; the owner
    chose that, with an adjustment.
  - **`overlay_text_scale`** (ini float, default 1, read clamped to 0.5..2 like every ini float, `pf`, and rounded to a step
    of 0.05 on read, `text_scale_step`, so the file and the Debug row agree; written `0.##`): a slider at the bottom of the
    F8 window ("text size of this overlay, Eye 2 and the labels", with a "default" button and the resulting px). It is a
    `SliderInt` over the step count 10..40, shown as `0.50`..`2.00` (a format with no `%`, displayed as is): a float slider's
    keyboard / gamepad step (1 % of the range, 0.015) would round back to the same 0.05 step, so the pad in the headset could
    not move it. Whole steps also mean a drag bakes about 30 font sizes in 1.92's dynamic atlas, not hundreds. A Ctrl+click
    entry takes the step count (ImGui falls back to `%d` for input), clamped. The `FontScaleDpi` used in the size is taken
    only within 0.5..4 (else 1), like the DPI scale. Marker labels keep their fixed pixel offset from the triangle (at 2.0 a
    label overlaps it; cosmetic, not changed). Debug row: `our text <GetFontSize()> px (pushed <size>,
    overlay_text_scale <k>)`, i.e. 20 x s x k.
  - **No `ini_rev` bump.** The migration exists to replace old *defaults* that carried no choice of the player's. Both new
    keys are absent from every older ini, and absent means the default, which is the intended value (1, 1.0); the first
    save tick writes them (`settings_first_read_done` keeps the raw text as `_ini_last`). Older builds ignore the keys
    (`default: known--`) and drop them at their next save: a rollback round trip turns an explicit `fh_dpi_fix=0` back into
    the default 1, and `overlay_text_scale` back to 1. Harmless.
- **ImGui text is a printf format.** Hexa's `ImGui.Text*(string)` passes our string to ImGui as the format with a garbage
  `va_list` (cimgui `igTextDisabled0`); ImGui formats through UCRT. A `%` followed by an invalid conversion fail-fasts the
  process (0xC0000409, no catch stops it); a valid one (`% o`, `% u`) prints stack garbage.
  - Fixed: hd_cache.cs:603 (`{pct} % of the budget`, `% use`: garbage numbers) writes `%%`; uidepth's status line ("UI on
    NN% of the panel", built in a helper, so no grep finds it) is shown through `esc()`.
  - `esc(s)` (diag7.cs) doubles every `%`. **Every non-literal argument of an ImGui text call goes through it**: fields,
    our own status helpers' return values, native status buffers, the OpenXR runtime and controller profile names, pick
    lines, bone labels from bone_labels.txt, actor names read from the game (the actors table and the battle-follow "now:"
    row), the native / detour status. Only interpolated strings whose holes are numbers or our own literal ternaries are
    exempt.
  - The grep rules in release.md section 5 run before every release: two per-call commands, which cannot see a string built
    elsewhere and passed in (hence the esc rule), and a catch-all over every literal with its four known, reviewed hits. Slider and drag formats (`"%.1f"`) are formats on purpose, and widget
    labels are not formatted (`"% of the screen"` in the stereo screen's slider label is fine).
- **Build and install.**
  - **One source for the version:** `tools/paths.ps1` (`$FfxvrFhVersion = '1.0.0-alpha12'`, `$FfxvrFhCommit`), dot-sourced
    by every script involved and shipped in the player zip. `Get-FfxvrFhInfo <bin>` reads `fh.dll`'s ProductVersion
    (`1.0.0-alphaNN+<40-hex commit>`) and Authenticode status: `1.0.0-alpha12 (7e85839, official release)` against
    `1.0.0-alpha12 (ecb6702, built from source)`; any other status (a root not fetched, a broken signature) prints as
    `signature: <status>` (the official zips are signed; alpha11's signer is the maintainer's
    Certum open-source code-signing certificate). `ffxvr.csproj`'s `Sdk="Fahrenheit.Sdk/<version>"` must equal it:
    build-mod and package-release stop otherwise.
  - **SDK isolation:** `src/managed/nuget.config` clears every package source (and `disabledPackageSources`) but nuget.org,
    and puts the packages into the repo's own `_ext/nuget`. A local Fahrenheit build and the official release share the
    version string, so neither a global source nor the global cache may decide which one we compile against. It sits in
    `src/managed`, not at the root, so it never reaches the Fahrenheit source build in `_ext/fahrenheit`. build-mod reads
    each package's `.nupkg.metadata` `source` after the build: `fahrenheit` from anywhere but nuget.org is an ERROR (nothing
    deployed), `fahrenheit.sdk` a WARNING (its content equals the release's). An MSB4236 in the log gets a hint. The first
    build needs the network. `NUGET_PACKAGES` set overrides the folder (build-mod warns). Determinism, not correctness:
    the API is identical either way at alpha12; at alpha13 the same trap would have different content.
  - **Fallback, only if `Fahrenheit.Sdk <version>` is ever missing from nuget.org:** take only the SDK from a local
    Fahrenheit build (content-identical at alpha12). In `src/managed/nuget.config` add to `packageSources`
    `<add key="fh-sdk-local" value="..\..\_ext\fahrenheit\artifacts\pkg\rel" />` and, after `</config>`:
    ```xml
      <packageSourceMapping>
        <packageSource key="fh-sdk-local"><package pattern="Fahrenheit.Sdk" /></packageSource>
        <packageSource key="nuget.org"><package pattern="*" /></packageSource>
      </packageSourceMapping>
    ```
    build-mod's provenance line then reads WARNING for `fahrenheit.sdk` and must still say nuget.org for `fahrenheit`.
  - **Installing Fahrenheit:** `install-fahrenheit.ps1 -Zip <official zip>` (owner and developers; players extract the
    zip by hand, README-zip section 2): unblocks, extracts to a temp folder (always removed, also on an error exit), finds
    `bin\` by its `fhstage0.exe`, checks six files (`fh.dll`, `fhr.dll` added), empties `<game>\fahrenheit\bin`, copies,
    supplies dbghelp.dll from SysWOW64 if missing (alpha11's zip had none), ensures `mods\loadorder`, prints the version.
    Without `-Zip` it installs the source build in `_ext\fahrenheit\artifacts\deploy\rel\bin`, the one-command rollback.
  - `tools/build-fahrenheit.ps1` (the legacy managed-only build) is **deleted**: it reset `_ext\fahrenheit` to origin/main
    and wrote main's managed DLLs over the pinned build, which silently broke that rollback. `build-fahrenheit-full.ps1`
    is the developer route only; its default commit comes from paths.ps1, it fetches tags (`-Commit v1.0.0-alpha12`), and
    it accepts the three debugger-DLL copy errors (`dbghelp`, `dbgcore`, `symsrv`).
  - Player side: `install-mod.ps1` prints `Fahrenheit version : 1.0.0-alpha12 (7e85839, official release)` and counts
    alpha11 or older as a problem (a literal floor: alpha11 predates the game patch, a fact that never moves);
    check-prereqs prints the same; package-release fills `{FAHRENHEIT_RELEASE}` / `{FAHRENHEIT_COMMIT}` from paths.ps1
    and warns when `_ext/nuget` did not get the packages from nuget.org.
- **Kept as is.** The diag7 `_d7_inserting` / `_d7_resolving` handshake: redundant after #366, removed only after the soak
  test. `ffxvr_safe_read` / VirtualQuery-first: an access violation under the stage-0 debugger is still fatal.
  `docs/exe-analysis.md` (#388 is main only); the `FhSavePal.pal_addr_screen_state` comment (right at the tag).
- **Why not main.** Main adds #379's unlocked main-thread `UpdateSubresource` / `GenerateMips` on the immediate context at
  save-screen open (`load_texture_game` handing the immediate context to WIC off the render thread), which breaks the
  assumption stickrep's extra frames rely on; #388's new first-chance exception semantics, which would need a soak test of
  their own; and a `1.0.0-alpha13` version string that will collide with the official alpha13. What we give up: the
  built-in DPI fix (FH-1 replaces it), #388, the save UI refactor (#374/#378), #372, #380, #383. None is needed by FFX VR.
  We keep alpha12's soft-reset combo (main moved it to EFP, #373).
- **Revisit when the official alpha13 is released.** Check these first:
  - (a) #379: does `load_texture_game` still hand the immediate context to WIC off the render thread (main
    `src/runtime/resources.cs:124-142`, `src/core/wic.cs:570-660`, `src/core/save_mgr.cs:271`)? **The owner will not report
    it upstream: we add a local guard before moving to alpha13.** No further action now.
  - (b) A #388 soak test.
  - (c) FH-1 becomes a no-op (it sees Fahrenheit's flags already off; the first `ImGui:` line says `off`): delete it, keep
    FH-2 and `overlay_text_scale` (our overlay's size is the owner's choice, not Fahrenheit's).
  - (d) The version bump: `tools/paths.ps1` (`$FfxvrFhVersion`, `$FfxvrFhCommit`) and `ffxvr.csproj:7` (build-mod checks
    that the two agree); the literal prose in INSTALL-ALPHA step 4, `docs/release.md` sections 1 / 2 / 4 and this entry
    (`grep -rn "alpha12" INSTALL-ALPHA.md docs packaging`); the stage-0 debugger paragraph of
    `docs/exe-analysis.md` (about line 37; #388 changes first-chance exception handling) and the `FhSavePal` comment in module.cs; an INSTALL note on the soft reset (#373).
  - (e) The publish folder, checked for `TerraFX.Interop.Windows.dll`.

## VR hands v2, stage S1: boxes at the controllers (branch `hands-s1`, 2 Nov 2026)

Spec: research/hands-v2-spec.md (session scratchpad; S1 section), with the owner's decisions of 2 Nov 2026, which win
where they differ: **our hands are drawn only in first person with the body hidden** (`fp_body_mode` 0, the default; with the
body shown nothing of ours is drawn, no forearm cutting); generic hands first, user packs later (S3); **no optical hand
tracking** (the spec's S5 is dropped); **off by default** (`vr_hands=0`). Code: native "VR hands v2" block after the 3D
markers pass (ffxvr_native.cpp), `src/native/ffxvr_hands.cpp/.h` (CPU only), managed `src/managed/hands.cs`. ABI 27.

- **Where it draws.** `ffxvr_pre_resolve`, called by the managed ResolveSubresource detour *before* the game's resolve. At
  the first resolve of this frame's scene target (`g_scene_live_tex`, >= 20 eye-2 draws), with rtv0 = that texture and a DSV
  bound: one draw per eye, eye 1 into the game's MSAA rtv0 + DSV, eye 2 into `twin_rtv` / `twin_dsv` (no depth twin: eye 2
  skips, counted). Depth LESS_EQUAL with write, opaque, no culling in S1 (closed boxes), gamma-space light fixed in view
  space. The hands then ride the game's resolve and our mirrored resolve into the eye images, so transparents test against
  them, post-processing applies, and the UI panel is over them. rtv1 (the second MSAA target) is not written; the census
  line names it.
- **When.** Inputs are latched at the frame's tick (`hands_latch` in write_camera's advance: the controllers, the recentre
  reference, the near-variant projection). At the resolve the views are rebuilt from `g_tick_v` (`build_override_view`,
  read-only, under g_mutex) and each eye's projection is the latched one with that eye's FOV applied exactly as write_camera
  applies it. Cross-checks: `pkt_mismatch` (the pad packet at the tick vs at the draw) and `pv_diff` (our eye-1 VP vs the
  last uploaded `g_last_PVn`, once per 10 s).
- **Placement.** `H = Grip . (Cal | Default) . Trim` in reference space, then `w = P + Rb^T (sc r + mv')` (build_override_view
  now also publishes `g_hb_mv` and `g_hb_mode`). The hand frame is OpenXR's joint convention at the palm (+Y out of the back
  of the hand, -Z toward the fingers). **Default** = Meta's palm pose on the Touch grip from VDXR's table (grip . Rx(-70 deg)
  at (-+1.0, 1.3, 0.75) cm) times Rz(+90 deg) left / Rz(-90 deg) right: Meta's palm pose keeps the grip's X (the palm
  normal) and has +Y toward the thumb, so the spec's plain `grip . Rx(-70)` would have been a quarter turn off about the
  fingers. **Cal** (the "calibrate" button, arms out, palms down) is captured in reference space only (shoulder estimate from
  the head), replaces Default's rotation, keeps its offset; the old `hands_cal` is migrated once (Cal . Rz(+-90 deg)).
  **Trim**: `vr_hands_yaw/pitch/roll`, `vr_hands_off_right/up/back` in the hand frame, mirrored for the left hand.
- **Hidden** (first match, counted): off, headset off, screen frames (FMV, menus, save screen), stereo screen, painted
  backdrop, battle transition, dream filter, frozen view, not first person, body shown (fp_body_mode != 0, the body hide
  off, or the old bone hands on), no tick, pass not ready, no projection; at the resolve `no_dsv` / `wrong_rt`; per hand an
  untracked grip; a frame without a qualifying scene resolve is `no_resolve`.
- **Logging.** Events always: `hands: on|off` (off with the session totals), `hands: pass ready (vs+ps compiled N ms at
  Present)`, `hands: first drawn ...`, the first skip of each kind, `hands: calibrated L R (wrist vs default ...)`. With
  `diag_verbose=1` the 10 s line `hands: drawn e1 N e2 N / N frames | hidden ... | skip no_dsv wrong_rt no_twin_dsv no_twin |
  grips | pv_diff | pkt_mismatch | calibrated | last hidden`. Frame census: `HANDS e1 -> ...` / `HANDS e2 -> ...` before the
  scene `RSV`. Debug bits: 1 wrist axes, 4 one placement line per drawn frame (300 per switch-on), the pose-dump button.
- **Review round** (hands-s1-review.md): the hands draw only at a resolve of last frame's scene target (`not_prev`
  otherwise, not latched); a calibration request expires 0.5 s after its countdown; the adaptive UI panel's depth is
  measured before the hands are drawn (the measurement is skipped on a frame where that could not happen), so a hand under
  the dialogue box does not pull the panel in; the game's bound viewport is used; with `head_position=0` the hands are
  placed relative to the eye.
- **Not done in S1**: the old bone hands stay as they are (retired in S2, spec section 4); no A/B of the alpha the hands
  write into the scene target (review N6).

## VR hands v2, stage S2: the generic hand, controller-driven fingers, the diorama (branch `hands-s2`, 3 Nov 2026)

Spec: research/hands-v2-spec.md S2 (session scratchpad), with the owner's decisions, which win: hands only with the body
hidden in first person (no "show my body"), no optical hand tracking, off by default, and **a setting to show the hands in
the diorama too** ("to give our hands some context when adjusting the view"). ABI 28.

- **Packs.** `assets/hands/generic-glove` (dark glove, the default look) and `assets/hands/generic` (skin mannequin), each
  `hand.json` + `right.glb` (the left hand is the X mirror), written by `tools/hands/make-generic-hand.py` (ours, MIT; the
  committed glb is what ships). Deployed to `<mod>\hands\<name>\` by build-mod / install-mod; package-release copies only
  these two (an allowlist) and fails on anything else under `hands\` or anything marked private. The managed side names the
  file (`vr_hands_pack`, the other shipped pack as the fallback, else none); hand.json is not parsed yet (S3).
- **Loader** (`ffxvr_hands.cpp`, CPU only): cgltf 1.15 (vendored, `third_party/cgltf`, MIT) from memory; glb with its embedded
  buffer and images only; joints by name (also `XR_HAND_JOINT_*_EXT`); palm rebuilt if missing; rigid binds only; weights to
  unorm8; <= 65535 vertices; textures via WIC on the worker thread (<= 2048 px, else the base colour). Native reads the file
  on a worker thread (CreateThread, one at a time) and uploads at Present (immutable buffers, CPU mips); a failure is logged
  and the S1 boxes are drawn. Tested in the container: both committed glbs load; 3000 corrupted copies under ASan/UBSan fail
  cleanly.
- **Skinning on the GPU** in our pass: VS b1 holds a 26-joint palette per hand (3 float4 rows each); one DrawIndexed per hand
  per texture batch per eye. The palette puts the pack's palm joint on S1's hand frame (Default / Cal, Trim, PackFix), scaled
  about it (`vr_hands_scale` x the pack's scale), then `w = P + Rb^T (sc r + mv')` as the boxes. No culling (as S1).
- **Fingers** (hands-input 4.1-4.3): four new actions on `oculus/touch_controller` (trigger, thumbstick, thumbrest, A-B / X-Y
  touch; optional: refused bindings fall back to the old set). Index = lerp(0, 0.3 + 0.7 trigger, trigger touch); middle /
  ring / little = rest + (1 - rest) squeeze (ring ^0.85, little ^0.7; `vr_hands_rest_curl` 0.20); thumb = the 2x2 blend of
  FREE / UP / ON / WRAP by thumb touch and squeeze (ON nudged for the face buttons / thumbrest). MCP 8..80, PIP 10..100, DIP
  5..70 deg, splay and cupping as the research; the thumb's key poses were tuned on the generic hand from rendered previews.
  Smoothing on the display clock (touch on 35 ms, off 70 ms; values 15 ms). A controller still for 10 s (< 5 mm, < 2 deg)
  with nothing touched hides its hand until it moves or is touched.
- **Views** (`vr_hands_views`, bits): first person always; 2 the diorama, 4 third person, 8 the seat view (default 1). The
  placement needs nothing new: sc is the view's units per metre (11.4 in first and third person, `world_scale` x the grab
  zoom in the diorama and the seat), and the eye is placed with the same sc and mv', so a hand is exactly where and as big as
  your real hand relative to your eye: life-size to you, giant beside the miniature (a 0.19 m hand is 2.2 units in first
  person, about 18-23 units in the diorama). Unit-tested: the palm relative to the eye in real metres equals the real
  controller offset to 1e-6 m in both. The depth test is the scene's, so a hand pushed into the miniature goes behind its
  ground and walls; the near plane (1 unit) is about 1 cm in the diorama. The one addition: the diorama / third-person floor
  lifts the eye (`g_hb_dy`, published by build_override_view) and the hands are lifted with it. The body rule applies only
  in first person (Tidus is part of the scene in the others).
- **Logging.** Events: `hands: pack 'generic-glove' loaded: right 1072 v / 2024 t, 2 prims (1 batch), left mirrored, ...`,
  `hands: pack 'x' rejected: right.glb: joint 'thumb_tip' missing -> boxes`, `hands: views now first person + diorama`,
  `hands: first drawn in the diorama at frame N | 122.20 units/m ...`, `hands: right controller put down ...` (first per
  hand). The 10 s line adds `| mesh 'generic-glove' 2144 v | views ...: drawn fp N dio N tp N seat N | fingers L t i m R t i m
  | put down L N R N`. Debug bit 2 draws the boxes instead of the mesh.
- **Not done in S2:** the retire commit of the old bone hands (spec section 4; the spec puts it after S2 passes in the
  headset); hand.json parsing and user packs (S3); the game's lights (S4).

## VR hands v2, stage S3: hand packs, hand.json, the importer, the generic hand's proportions (branch `hands-s3`, 3 Nov 2026)

Spec: research/hands-v2-spec.md S3 (+ 1.4, 1.5, 1.6, 5). The owner on S2: "decent placeholders, a little floppy / long
fingers". ABI 29.

- **Packs** (`src/managed/hand_pack.cs`, managed only; native never parses JSON). Every folder under `<mod>\hands\` with a
  `hand.json` or a `.glb` is a pack (64 at most), scanned at the first frame and on F8's **rescan**. hand.json format 1
  (<= 64 KB, System.Text.Json, comments and trailing commas allowed): `name`, `author`, `licence`, `source`, `private`,
  `right` / `left` (plain `.glb` names in the pack folder only: no paths), `mirror`, `scale` (0.5..2), `wrist_offset_m`
  (+-0.10 m each), `wrist_rotation_deg` (+-45 each), `tint` (0..4), `rest_curl` (object by finger or 5 numbers, -1..1),
  `preview` (accepted, not shown). A wrong type or an out-of-range value is a logged note and the default / clamped value is
  used; only what makes a pack unusable is an error (bad JSON, a newer format, a missing or oversized glb, a file name with a
  path), and that pack is greyed out in F8 with the reason as its tooltip. Mirror: `true` builds the left from the right
  even if left.glb exists; both files and no `mirror`: a two-handed pack; only a left file: the right is its mirror.
- **Choice and fallback.** `vr_hands_pack` is any folder name; the walk is the selection, then `generic-glove`, then
  `generic`, then the boxes (each step logged). Native keeps drawing the old pack until the new one is in, also when the new
  one is rejected (S2 dropped to the boxes); its reason (`ffxvr_hands2_pack_error`) is kept on the pack until a rescan.
  `VrHandsPackDesc` + `stamp` (the glbs' sizes and write times): the same paths with changed files load again, so a pack
  re-made while the game runs shows after a rescan.
- **Native.** `hv_pack_parse` does right + mirror, left only, or both (two-handed); the worker decodes the textures with WIC,
  halves any over 1024 px and builds the mips there (S2 built mips at Present: review SF-1 item 4). The S2 caps stay (65535
  vertices, 393216 indices, 4 images a hand, 2048 px decoded, 32 MB a file). `ffxvr_hands2_status` 21 values (+ the drawn
  pack's textures, failed images, worker and upload ms), shown under the F8 combo.
- **The importer** (`tools/hands/import-hands.py` + `objio.py`, `tools/import-hands.ps1`): the research prototype, hardened.
  A full-body OBJ in a T or A pose, Y up: both hands found at the X ends (or `--tip`), cut at the cuff (`--cut*`), five
  fingers from the mesh's geodesic merge tree, the 26 joints, one scale for both hands from the middle finger, textures
  embedded (PNG / JPEG; a missing one is warned and drawn in its flat colour). It writes `"private": true`, the licence
  "private - made from your own copy, do not share" and `PRIVATE-do-not-share.txt`; it refuses an output inside a git
  checkout (a `.git` folder or file above it), a shipped pack's folder, and a folder that is not its own. The wrapper finds
  Python 3.8+ and numpy and resolves `<mod>\hands\<Name>`. package-release ships the scripts by name, never a pack; its guard
  now also fails on a private marker or a stray `.glb` anywhere in the zip. On the owner's PS2 Tidus OBJ (scratchpad only,
  never in the repo): right 602 t, 2 textures; left 694 t, 4 textures (gauntlet, chains, bracelet); both load through the
  runtime's loader, pose and skin cleanly in all seven gestures.
- **The generic hand's proportions** (the "floppy / long fingers"). Measured on the S2 mesh: the palm ended at the knuckles,
  so each finger showed its whole length from the MCP joint (middle finger 0.55 of the 0.19 m hand); a real finger shows
  about 0.43 from its web (ANSUR II median hand length 193 mm men, breadth 88 mm; webs 15-25 mm past the MCPs and phalanx
  lengths from Buryanov & Kotiuk, Int. J. Morphol. 28(3), 2010). And the joints blended over 0.35 of the bone either side
  (a 90 deg PIP bend spread over ~25 mm: rubbery). Now: phalanges to those lengths (middle MCP -> tip 96 -> 91 mm), the
  knuckles 4 mm further out (hand length stays 0.19 m), palm-side webs 10-13 mm past the knuckles weighted into the
  proximal bones, fingers 5 % fuller, joint blend 0.22. Visible middle finger 0.55 -> 0.43 of the hand; palm breadth 0.47 L
  (ANSUR 0.46). In the solver: a touch must be off for 50 ms before its finger opens (a flicker of a capacitive sensor
  twitched a finger by 25 % in S2), then opens with 50 ms (was 70; the 10 % point stays ~160 ms); the grip has a 0.04 dead
  zone. `vr_hands_rest_curl` stays 0.20 (an ini keeps its value; raise it in F8 if the idle hand looks too open).
- **Not done in S3:** the pack's `preview` image in F8; per-hand (left vs right) wrist offsets for asymmetric two-handed
  packs; the retire commit of the old bone hands; the game's lights (S4).

### Review fixes (hands-s3-review.md: B1, B2, SF-1..SF-3, N1-N7, N9)

- **B1**: a lone surrogate escape (`"\ud800"`) in any hand.json threw past the parser (System.Text.Json unescapes
  lazily) and emptied the pack list. Now that pack alone is unusable with the reason; the scan catches per folder; the
  list and "scanned" are set only after a scan returns.
- **B2**: `--preview` follows the pack's rules: a bare name goes into the pack folder, a path inside a git checkout or
  another (non-private) pack folder is refused; `.gitignore` also ignores `preview*.png` / `debug-preview.png`.
- **SF-1**: the importer welds identical corners (Tidus right 1806 -> 430 vertices) and checks the runtime's caps:
  > 65535 vertices or > 393216 indices a hand, or a glb over 32 MB, is an `error:`; a texture over 2048 px is shrunk to
  1024 with Pillow (else warned, flat colour); more than 4 textures a hand is warned by name.
- **SF-2**: previews decode JPEG by content (Pillow); a preview failure never fails a written pack.
- **SF-3**: the worker reads, parses and frees one glb at a time (hv_pack_parse with a read callback).
- **The fuzz found** cgltf_validate reading a misaligned uint16 index accessor before our own alignment check: every
  accessor, sparse accessors and the binary chunk are now checked before cgltf_validate.
- **Hand length**: the two hands' wrist estimates differed (Tidus: 0.198 vs 0.181 m wrist -> middle tip, the bracelet
  moves the narrowest section). Both wrists are now moved along their hands so wrist -> middle tip = `--hand-length`
  (right -8.2 mm, left +9.4 mm on Tidus); `--keep-proportions` keeps the model's.
- Nits: N1 `##` runs, N2 notes capped at 16 and keys cleaned, N3 a 255-char `vr_hands_pack`, N4 `images_dropped` from the
  rig, N5 face / thumbrest holds exclusive, N6 `-OutDir` / `-Preview` trailing `\` trimmed and `"` refused in `-Title`,
  N7 no trailing space or dot in a name, N9 an OSError is an `error:` line. Skipped: N8 (a rescan with the selection still
  broken reloads the fallback pack once: harmless, about 1 ms).

## Hands: the tap gesture, and the fist fix (branch `hands-tap`, 3 Nov 2026)

The owner: "let's start with the tap. I think much more could get difficult". ABI 30. The first hand gesture that drives
the game: a quick forward jab of a pointing hand presses confirm in the field. (The detector's numbers below are the first
build's; the review round and the retune after the owner's first headset round, further down, change several.)

- **The button.** The game's Phyre pad poll (0x62DBA0) stores XInput A (0x1000) in the second of its PS2-order bytes
  (+0x398 square = X 0x4000, **+0x399 cross = A**, +0x39A circle = B, +0x39B triangle = Y, then L1 R1 L2 R2 select start
  L3 R3 and the d-pad); cross is confirm in the PC (international) build, and our VR pad maps the right controller's A
  button to XInput A. So the tap ORs `XINPUT_GAMEPAD_A` into the VR pad's state (`vrpad_apply`, the game's input thread);
  `vr_hands_tap_button` (ini only) can pick B / X / Y. The same poll **skips a state whose packet number did not change**
  (`cmp ecx,[esi+0x3B4]; je`), so a press is held for `vr_hands_tap_press_ms` (100 ms, about 3 logic frames at 30 Hz)
  **and** at least 3 new packets (our packet moves once a rendered frame), never past 400 ms; then the 400 ms cooldown keeps
  it released long enough for the game to see the edge. Never stuck: the render thread lets it go at once when the tap or
  the hands go off, the headset or the VR pad goes off, the session loses focus, a chord / grab starts
  (`g_vrpad_suppress`, which also blanks it in the hook) or the view leaves first person, and after 500 ms regardless.
- **The detector** (`hv_tap_update`, ffxvr_hands.cpp; pure, unit-tested): per hand, on the frames the hands are drawn
  (hands_draw_frame), from the posed index fingertip (the pack's joint 10 through the pack's palm frame and scale; the
  boxes: 10 cm along the hand) and the head, both in reference space at the latched display time. The fingertip's
  velocity **relative to the head** (a lean or a step that carries the arm along is no jab) along the head's horizontal
  forward: a jab starts above 0.35 m/s within ~45 deg of forward (cos 0.70; a poke a little down at a chest counts), ends
  when the forward speed falls below 0.40 x its peak (the stop or the retraction), and is a tap if its peak (the mean of two
  frames: noise halved) reached **0.70 m/s / sensitivity**, it travelled 3.5-45 cm forward, took 50-250 ms from the start
  to the stop and at least 3 samples. Blocked at the start, and aborted during: the point pose not held 100 ms (fingers as
  drawn: trigger touch faded below 0.15, trigger < 0.05, grip > 0.60), the gate shut, the right stick past half deflection
  (turning), the head faster than 0.5 m/s or 100 deg/s, a raw speed over 5 m/s (a tracking jump). A blocked stroke is not
  started later in the same stroke. A gap over 100 ms (a hidden frame, a menu) starts over.
- **Why these numbers.** Quick pokes peak at 0.8-1.5 m/s (the brief; minimum-jerk strokes of 5-20 cm in 0.10-0.25 s give
  0.9-3.7 m/s); a deliberate reach peaks at 0.3-0.7 m/s and lasts 0.5-1 s, so 0.70 m/s and 250 ms above 0.35 m/s sit in
  the gap between them; 3.5 cm and 3 samples reject the one- or two-frame steps tracking makes when it re-finds a
  controller (4-10 cm steps never fire in the test); 0.5 m/s of head speed is above a lean or a nod and below a jerk.
- **The gate** (F8 shows which): VR hands drawn (that already excludes FMVs, menus and the save screen (screen frames),
  loads, the stereo screen, painted backdrops, battle transitions, the dream filter, the frozen view, a controller put
  down), first person (`g_hb_mode` 1), the touch sensors bound (without the trigger's touch a finger resting on it looks
  pointed), the session focused, the VR pad on, no chord or grab; and managed's game gate every frame
  (`ffxvr_hands2_tap_set`, stale after 3 frames): `Btl.battle_state != 0`, the sphere grid open (FhUtil 0x1685F70,
  Fahrenheit `Globals.SphereGrid.is_open`), Fahrenheit's save / load screen (0x8CB994 not closed, or latched open), a bltz
  map, and **a text box**: any TOMesWinWork window of the current bank (FhUtil 0xF26D70 + (bank x 8 + i) x 0x2C, bank =
  signed byte FhUtil 0xF26B82) with a non-zero state word at +0x14. The game's own per-frame loop (0x871F30) copies each
  window's state there (4 = closed, written back as 0) and counts a window with a non-zero state and +0x1D bit 1 as one the
  player waits on; our stricter reading counts any non-zero state. **Static only, not yet seen in play**: the F8 readout and
  a `hands: tap gate (game): ...` line per change (with the windows' states and flags) are there to confirm it. Cutscenes
  with no text box are not gated: confirm does nothing there (the game ignores it outside a text box), and a voiced line
  is a text box.
- **Dialogue: off by default** (`vr_hands_tap_dialogue`, "also advance dialogue"). A tap to advance text would be welcome
  (the same button), but a choice box takes the highlighted answer at a confirm and we cannot yet tell a choice box from a
  text box (perhaps +0x1D's other bits), and the text-box signal itself is unconfirmed; one tick turns it on.
- **Default on** (`vr_hands_tap=1`) with VR hands, which are off by default anyway: the tap needs a deliberate pose and a
  sharp jab, and it only acts where A would talk / examine / open; testing it is the point of the build.
- **Feedback**: a 30 ms, 0.45 haptic tick on that hand (`hands_frame_end`, like calibration's; `vr_hands_tap_haptic`). No
  fingertip flash: the hand's constant buffer is per eye and shared by both hands, so a per-hand tint is a shader change.
- **Logging**: events always (`hands: tap R: A pressed (peak 1.12 m/s forward, 96 mm in 140 ms ...)`, `hands: tap: A let
  go after 117 ms, seen in 7 new pad packets`, an early release with its reason, the first 30 near misses, 10 pointing jabs with the
  gate shut, the game gate changes (200 a session), on / off); the 10 s `hands: tap |` line (taps, not-taps by reason,
  blocked starts by reason, how often the gate was open) with diag_verbose only.
- **The unit test** (scratchpad htapchk/tap_test.cpp, 44 cases, ASan + UBSan): single pokes of 3-30 cm in 0.10-0.70 s
  (20 noisy trials each, 0.5 mm per sample, frame jitter): 0 / 320 meant taps missed, 0 / 520 non-taps fired at
  sensitivity 1; at 30 / 72 / 90 / 120 Hz; up, down, 30 deg aside fire, 45 deg aside, sideways swings, chops and pulls do
  not; a lean, a head jerk, a fast head turn (also with a poke during it), walking in place, sweeping the pointing hand do
  not; slow reaches, a gentle nudge, a 60 cm thrust do not, a fast 40 cm punch does; the stick turning, the gate shut or
  shutting mid-jab, an open hand, the pose changing mid-jab or formed 40 ms before do not, formed 150 ms before does;
  pokes every 0.25 / 0.35 s fire every other one (the cooldown), every 0.5 / 0.8 s all six; tracking steps of 4-10 cm,
  tracking lost mid-jab, holding still with 2 mm noise do not. (The first version's false positive, a forward-back shake
  of the pointing hand, is fixed in the review round below; the test now has 63 cases, judged at every sensitivity.)
- **The fist fix** (shared finger code, every pack). As a finger curls past 0.25 (smoothstepped to a full curl), its
  bones turn sideways back onto the hand's long axis: `HvMesh::abd[f][3]`, measured at load about the wrist's +Y (the
  proximal bone from the wrist's -Z, the intermediate and distal from their parent), applied as a yaw before each joint's
  flexion, and the solver's own splay goes to 0 with it. The open hand, the idle curl and a point's index are unchanged.
  Generic hand: the curled tips used to drift 5-7 mm toward the little finger (ring and little fanned 8 / 16 deg); now
  within 2.5 mm. The owner's gap between middle and ring was mostly **the importer**: the Tidus right middle knuckle sat
  12 mm from the index and 36 mm from the ring (its proximal bone 26 deg off, then 19 deg back), because each MCP was
  extended back along the direction of the finger's first two centreline rings, which bend toward a neighbour at the web.
  `import-hands.py` now extends along the line fitted through the whole centreline: knuckles 20 / 23 / 21 mm apart (left
  23 / 22 / 21), fist spacing 20-27 mm on both hands (was 12-41 mm). An existing pack needs the importer run again.

### Review round (hands-tap-review.md: S1-S5, nits)

- **S1, shakes.** A forward-back shake of the pointing hand fired about once a second (the known false positive). A jab
  may not start within 0.12 s of a backward stroke (the two-frame mean of the forward speed below -0.6 x 0.35 m/s): a tap
  starts from rest or a slow approach, a shake's forward stroke follows its back stroke within 30-60 ms. New reason
  "shake". Shakes of 4-8 cm at 2-4 Hz: 0 taps at every sensitivity (were 30-54 in 30 s); six pokes every 0.35 / 0.5 / 0.8 s
  still give 3 / 6 / 6, every 0.25 s now 1. Left as is: above the default a slow 6 cm / 1.5 Hz shake (0.57 m/s, itself a
  jab at that sensitivity) taps now and then (a 0.15 s lockout would catch it but costs the 0.35 s cadence).
- **S2, the head moving back.** The jab speed is fingertip minus head, so straightening up (the head back at 0.45 m/s,
  under head_v) with the hand still fired at sensitivity 2 (20 / 20). The fingertip's own forward peak (reference space)
  must now reach 0.5 x v_on too: 0 at every sensitivity, the poke grid unchanged.
- **S3, text boxes.** The gate also takes the game's own result of the 0x871F30 loop: bit 2 of byte +1 of the current
  Atel controller (`[FhUtil 0xF26B28]`, null: no event, nothing to read); either it or a non-zero window state gates. The
  F8 / log text shows both. Seeing it change in a choice box is now a must-pass owner test step. Known gap: managed reads
  the gate at frame N's Present and the hands use it in N+1, so a box an event opens in N+1 can take a tap fired in that
  frame (a deliberate jab in a 16-33 ms window). Follow-up: count the menu finger (TkMn2DrawCrossCursor 0x8C0670 type 0)
  per frame, which appears whenever a choice is pending (a new detour; not this build).
- **S4, logging.** The pose is checked before the gate, so "a jab with the gate shut" is a pointing jab; those lines have
  their own budget (10), the near misses 30, the pressed / let-go pair 200 a session.
- **S5, the press as a pure function.** `hv_tap_hook` (ffxvr_hands.cpp) decides per poll: hold, release, count a new
  packet; vrpad_apply only applies it. It also makes one new packet without the button reach the game after a release
  before the next press is ORed (review N6: two presses never merge). Unit-tested against a model of the game's poll
  (a repeated packet keeps the old state) and its 30 Hz logic: polls at 30 / 60 / 120 / 144 Hz, a press between polls,
  two presses 405-420 ms apart, poll stalls, the clock going back, and the release-before-press sequence.
- **N5.** The press is not let go when the gate shuts mid-press (a text box opening because of the tap itself): it is no
  longer than a real A press, and FFX acts on the edge.
- Nits: the help text says "(confirm ...)" only for A; the sensitivity is clamped after the slider (Ctrl+click typing);
  the Numpad turn keys block a jab like the stick; the text-box text is rebuilt only when what was read changes; the
  importer fits the base two thirds of each centreline (a curled bind pose's chord would pull the knuckle palmward; Tidus
  unchanged within 1 mm); exe-analysis.md: the TOMesWinWork bank is a signed byte.

### The owner's first headset round: retuned for his poke (3 Nov 2026)

The owner: "it seems like it worked very well once but then I could never do it again". His log: 2 taps (R 1.67 m/s,
201 mm in 183 ms; 3.50 m/s, 243 mm in 101 ms), then 12 "sideways" with strong forward peaks (1.44-2.63 m/s, 136-284 mm),
5 "reach" (0.56-1.68 m/s, 121-313 mm, cut at 250 ms), and in the 10 s lines many starts blocked by "pose" and "shake". Not
a reset bug: his natural poke is a long (15-30 cm), fast, slightly arcing jab, often after a small wind-up, angled down at
the NPC. Rebuilt as unit cases (an arc of 3-5 cm, 15-25 deg down, half with a 3 cm wind-up, at 60 and 120 Hz), the first
build rejected them as he saw (sideways, reach, shake). The causes and the changes:
- **Direction** was judged per raw sample against the head's level forward: one noisy or arcing sample (at the start of an
  arc the sideways speed exceeds the forward one) killed the jab, and a poke angled down counted against it. Now the whole
  jab is judged: its **net displacement within 50 deg** of the forward at the stop; the forward is the head's own with its
  pitch kept within -60..+30 deg (a poke down at a chest you look at is straight ahead). Chosen over the index finger's
  direction: the finger's aim depends on the per-pack wrist fit and calibration, the head's gaze is where the NPC is.
- **Speeds over >= 25 ms** (the newest sample against one >= 25 ms older: 2 frames at 60-72 Hz, 3 at 90-120 Hz) for the
  start, the stop, the peaks and the strokes: at 120 Hz a 2-frame mean was too noisy (early false stops: "slow"). The raw
  speed still catches a tracking jump (> 5 m/s), and a raw forward peak above 1.6 x the 25 ms one is a one-frame step.
- **max_dur 0.36 s** (was 0.25) above 0.35 m/s, **max_travel 0.50 m** (was 0.45): his 20-31 cm jabs take 0.28-0.31 s there.
  A slow 30 cm reach in 0.8 s still is one ("reach"). Reclassified: a brisk 25 cm reach in 0.55 s at 0.85 m/s is his
  poke style and now taps.
- **The shake rule needs a real oscillation**: a start within 0.12 s of a backward stroke that itself turned round from a
  forward stroke within 0.30 s (forward, back, forward with no pause). A lone wind-up before a poke passes; a poke,
  pull-back and an immediate re-poke counts as a shake.
- **The pose** came from the drawn fingers, whose trigger-touch fades out over ~160 ms after the finger lifts, plus 100 ms
  held: a point formed just before a jab was 250 ms short. Now from the raw bits: grip > 0.5, trigger < 0.05, the touch
  sensor off for 30 ms (back on for 40 ms ends it, so a sensor flicker mid-jab does not abort), held 80 ms.
- **The log** says why: a near miss gives the net direction (degrees off forward, yaw and pitch), the worst sample's angle
  and the pose inputs; a gripping jab blocked by the pose gives the trigger, the touch bit with its on / off times and how
  long the pose was held (15 a session); a shake block is logged (10); a tap gives its direction too.
- The unit test (92 cases, judged at sensitivity 0.6 / 1 / 1.5 / 2): the owner's 14 logged jabs (the 7 "sideways" the
  coordinator listed of his 12, the 5 "reach", the 2 that fired) x 10 trials x 60 / 120 Hz all tap at the default, except
  his 0.56 m/s one, below the default's 0.70 m/s (it taps from sensitivity 1.5); the review's shake and head-back cases
  stay at 0; grid 0 / 320 missed, 0 / 480 false at the default. Above the default a slow 6 cm / 1.5 Hz shake (0.57 m/s)
  taps about once a cycle (a 0.16 s lockout would stop it but costs pokes every 0.35 s).

### Review 2 of the retune (hands-tap-review2.md: R1-R5, N1-N5)

The retune let the owner's jabs through (78 / 78 under every clock the reviewer tried: 60 / 144 Hz samples, the game's 60
fps latching 72 / 120 / 144 Hz display times, repeated display times) but opened four holes, closed with the reviewer's
prototype, adopted as is (htrv2_t/r2_proto.diff):
- **R1, tracking steps at the real clocks** (14 / 30 on "game 60 on 72 Hz": a 27.8 ms frame is longer than the 25 ms window,
  so the raw-vs-window jolt check could not fire): the jab's largest one-sample forward step above 0.7 x its travel is a
  glitch (a real jab spreads its travel over >= 4-5 samples). 0 / 30 at every clock.
- **R2, raises and drops** (the pitched forward's 50 deg cone reached a hand raised into view, or dropped while looking
  down): the net displacement's elevation against level within +35 / -70 deg. Raises and drops 0 at every gaze; steep pokes
  at the floor (30-65 deg down, looking 35-60 deg down) still tap 10 / 10.
- **R3, sweeps** (a turn passed as a stop: its forward speed fell, its speed did not): at the stop the whole speed must be
  below 0.7 x the peak. Sweeps 0 / 10; "talking with the hands" with the point held 38 -> 8 taps / 10 min.
- **R4, the shake rule**: a shake's back stroke must travel >= 4.5 cm (a wind-up is 3-5 cm, shortened by the window), and
  the strokes end with hysteresis (+-0.2 v_start). Raise into view + wind-up + poke: 0 -> 10 / 10; the noisy 120 Hz shake 0.
- **N1**: the head's speed and turn rate over the same >= 25 ms window as the hand's (one-frame head noise aborted jabs at
  120 Hz: 1.5 mm noise 60 -> 77 / 78). **N2**: a reset ends an open forward stroke (the shake rule sees it). **N3**: the shake
  line gives the back stroke's length, ms since it ended and ms after the forward one; near misses the elevation against
  level. **N4**: the fingertip is the drawn index tip, whose extension animation (50 ms hold, 50 ms time constant) can still
  run when the raw-bit pose turns valid (30 + 80 ms after the lift): up to ~0.15 m/s more forward at the start; harmless at
  the default (it helps), and not a detector bug if sensitivity 2 behaves oddly right after forming the point. **N5**: the
  wind-up wording holds after R4.
- **R5, the cost (by design, an owner test step)**: with the point pose held, a real 30-45 cm reach in 0.45 s taps (in 0.55 s
  35 cm 2 / 10; 0.65 s or more 0), a deictic point at the screen / an NPC 20 / 20, everyday repositioning ~20 taps / 10 min.
  These are his poke kinematically; the pose is the guard. If they bother him, the levers are pose_hold or a slightly
  higher v_on, not the direction rules.
- Known and reported, not judged: 3 mm head / hand noise at 120 Hz (6x Quest's) splits strokes beyond the hysteresis
  (an 8 cm / 2 Hz shake: 4-6 taps in 18 s) and costs a third of the jabs.
- Tests: the reviewer's adversarial cases are a judged test now (htapchk/tap_test_adv.cpp: 78 judged lines, 37 reported):
  ALL OK at 0.6 / 1 / 1.5 / 2; the branch test (92) ALL OK at all four.

### The owner's second headset round: the tap redesigned around the grip (3 Nov 2026)

With de2d735: 9 taps, ~50 rejected: ~20 "sideways" from R3's turn rule (his push ends with the hand dropping, elevation -27
to -44, so the speed stays high at the "stop": taken for a sweep; net direction only 11-36 deg off), ~25 pose blocks (the index
still on the trigger, or the point formed < 80 ms before), a few "shake" (a 6-38 cm back stroke, then the jab). The owner: "I'm
not 100% sure the exact gesture I need to make to register", and "I think we should be more lenient about allowing a tap
because it should only work with the controller grip activated". Agreed: the grip is a deliberate act, so it is the guard,
and the motion rule becomes one sentence: **squeeze the grip and push your hand forward about a hand's width toward what
you're facing**. ABI 31.
- **Armed**: that hand's grip above 0.7 (off below 0.5). "require a pointing finger (index off the trigger)", off by
  default, also needs the raw touch bit off for 30 ms. No pose hold. The touch sensors are needed only with that option.
- **Fire**: a push starts when the fingertip's speed relative to the head along the head's 3-D forward exceeds 0.30 m/s (over
  >= 25 ms); it taps as soon as the fingertip has moved **10 cm / sensitivity** forward from there within 0.5 s, the net
  displacement within **60 deg** of the forward (no elevation limit beyond that cone). Kept: the one-sample step check
  (review 2 R1) and raw speed > 5 m/s (tracking jumps), the fingertip's own forward travel >= half the distance (review S2:
  the head moving back alone), the head fast / the stick or Numpad turning, the 0.4 s cooldown (and across the hands), and
  re-arming only once the forward speed has dropped below 0.15 m/s (a long reach taps once). Dropped: the stop, the
  per-sample direction, the elevation limit (R2), the turn rule (R3), the shake rule (S1 / R4), the pose hold. The press, its
  release guarantees, the game gate and the logging caps are unchanged.
- **The grip is LB / RB.** Our pad mapping forwards the grips as LB / RB (PS2 L1 / R1, the pad word's 4 / 8). Neither
  design-notes nor exe-analysis records a field action for L1 / R1 (they page lists in the menus and are used in battle);
  not verified in the exe this round. Nothing new: the grips have gone through since phase 1, and the first tap build's point
  pose needed the grip too. Added anyway, cheap and safe: "don't pass the grip to the game while a tap can fire" (ini
  `vr_hands_tap_hold_grip`), **off by default** (it changes what the game sees, for a risk not seen): while the gate is open
  on a drawn first-person frame, a grip squeezed is **held back from the game until it is let go** (review 3 S1: a per-hand
  latch in the hook, `hv_grip_strip`, unit-tested). The first version only stripped while the gate was open, so a grip
  still held as a battle, the menu or a text box came up became a fresh L1 / R1 press, and frames the hands failed to draw
  (grip pose lost, a buffer map failing) pulsed RB at frame rate. With the latch a grip held back stays held back through
  gate changes and undrawn frames; letting go always clears it, and it only ever removes bits, so nothing can stick. A grip
  squeezed while the gate is shut (in battle, a menu) goes to the game as before; one squeezed in the field and held into
  battle needs a re-squeeze there. The hold flag is time-stamped: a render stall ends it after 250 ms (N1).
- **F8** (gestures): `armed (grip R)` and `push forward: L 0.0  R 6.3 cm of 10 cm`, the two new options, the distance by the
  sensitivity slider. **Log**: a tap gives how far it was pushed (and the hand itself), its peak, direction and elevation; a
  near miss (half the distance or more) its reason ("short", "reach", "sideways", "head back", "glitch", "grip") and the same
  numbers with the grip / trigger / touch inputs; a push while not armed the grip value.
- **Review 3** (no blockers): S1 above. N2: a frame hitch of 40-100 ms mid-push put most of a short push into one sample and
  failed it as a tracking step; a sample over 40 ms and over twice the shortest recent frame interval now counts its step
  as spread over those frames (a tracking step comes in one normal frame; a step landing on a hitch is caught only by the
  5 m/s check). N3: F8's push readout shows the push in progress, else the last push (any ending) for 2 s, so a short push
  shows how far it got. N5: the "hand did not move, the head moved back" reason is "head back" (beside "head moving").
- No fingertip glow while armed: the hand's lighting constants are per eye and shared by both hands (a shader change).
- **Tests**: tap_test.cpp rewritten (65 lines: 59 judged, 6 reported) and tap_test_adv.cpp (115: 48 judged, 67 reported),
  ALL OK at 0.6 / 1 / 1.5 / 2 (ASan + UBSan). Must tap: the grid's pushes of >= 13 cm at 0.5-4 m/s within 0.8 s (0 / 280
  missed at 1); the owner's 27 logged jabs (round 1's 14 and 13 of round 2's "sideways") x 6 under 4 clocks (162 / 162 each)
  and the reviewer's 13 x 6 under 6 clocks (78 / 78); pushes 30 deg up / down / 45 deg aside, along a 40 deg-down gaze, steep
  pokes at the floor; a push right as the grip is squeezed; re-pushes after a pause; a long reach once. Must not: pushes <=
  7 cm or slower than 0.25 m/s (0 / 300), 70 deg off, sideways swings, chops, pull backs, the head moving back (S2, 6-15
  cm), leans with the arm carried, head turns and a push during one, walking, nodding, the stick turning, the gate shut,
  the grip released (a push, a reach, a shake, mid-push), tracking steps 4-15 cm (60 Hz and the game's 72 / 120 Hz clocks),
  holding still with 2 mm noise. **Reported, by design (the grip is the guard)**: brisk reaches with the grip squeezed (30-45 cm
  in 0.45-0.8 s: 10 / 10), a shake with the grip (a tap every 0.4-0.5 s), sweeps into the front, talking with the hands
  with the grip held (~400 taps / 10 min), everyday repositioning with the grip held (78 / 10 min), raising the hand into
  view or dropping it along the gaze. **Changed expectations** (review 2's adversarial cases): the 8 cm / 2 Hz shakes, B2
  talking, B4b the lean-in reach, B6 drops, B7 raises, B8 sweeps went from judged 0 to reported; the grid's "not a tap" class
  is now distance / speed only.

### Owner's third headset round (20f6a1f, 3 Nov 2026): accepted

"This seems to work much better, I have no complaints with how it works so far." The log: 8 taps (both hands, pushes of
52-71 mm in 67-100 ms at sensitivity 2.00 = a 50 mm push, 7-56 deg off forward), every armed push while a text box was up
blocked (`gate shut (a text box)`), nothing fired on the save screen. **Not yet seen in play: a Yes/No choice box** (owner
test step 6b). The gate is expected to hold there (the text-box bank or the game's waiting bit), but if a push ever picks a
highlighted answer, untick "tap to press action" and look at the `text boxes:` readout on that prompt. The owner chose to
merge and come back to it if it shows up. Default sensitivity stays 1.00 (100 mm) until he says whether 2.00 is his pick.

## Hands: ideas parked (3 Nov 2026)

The owner: "I think much more could get difficult ... just take a note of the other ideas for now". One line each, with
the catch.

- **Point at an enemy to target, tap to confirm in battle**: the battle's target cycling is camera-relative pad input
  (0x780CE0's tables), so pointing needs a ray-to-combatant pick and a way to set the game's target directly; and a tap in
  battle must never fire a command by accident (the reason the tap is gated out of battle now).
- **A laser pointer into the F8 overlay**: ImGui would need our own mouse position from a hand ray against the head-locked
  ImGui quad, and a click gesture that is not the game's confirm (or the overlay gets both).
- **A haptic buzz when a hand touches scene geometry**: needs the scene depth at the hand's position each frame (a GPU
  read-back of the depth target, a frame late), and the game's depth has no idea which surface is "touchable".
- **Body gestures** (a hand over the shoulder = the menu, both hands to the face = recentre): reliable only with a body
  model (shoulder and head positions from the head pose alone), and a reach behind the head loses controller tracking.
- **Overdrive gestures** (Lulu's Fury as stick-like circles, Tidus's timing bar): each overdrive has its own input
  routine and timing window; a gesture must feed the same pad input within the game's frames, and failing it costs a turn.
- **Point-and-walk** (point where to go): the game walks by stick only; we would need the walkmesh (still not mapped:
  Fahrenheit's Globals.Map pointers are commented out) to turn a pointed spot into stick input and stop there.
- **Grabbing a sphere in the Cloister of Trials**: the game then shows a confirmation dialog ("Take the sphere?") that
  would still need a click (a tap with "also advance dialogue" would do) or a way to skip it; and the grab itself must map
  to the game's examine at the right pedestal.
- **Importer: mesh subdivision / smoothing and local texture upscaling for low-res user models** (the owner: "the hands
  are a little low res, but that's maybe just the nature of the files I sourced"): Loop subdivision with skin weights
  carried over, and an offline upscaler (Python + a model on the owner's GPU, his preference for local compute); the
  catch is the cost (more vertices skinned per frame is fine, but a 4x texture is 16x the memory in a 32-bit process) and
  that smoothing rounds off the model's own silhouette.


### JKXR's touch-to-use, compared (3 Nov 2026; research/jkxr-touch.md in the session scratchpad)

Team Beef's JKXR (GPL-2.0: design only, no code) fires "use" when a hand is more than 0.35 m out from the headset,
repeating every 250 ms; its real guard is their rewritten game code (use only works when a short trace from the hand hits
something usable). FFX decides confirm itself from Tidus's facing and each object's talk range, and many examine spots
(signs, doors) have no actor, so the grip + push + text-box gate stays our design; the owner (who played JKXR a lot)
agreed. Parked, in order of value: (1) a second haptic tick when the game actually responds (a text box opens; the
"last interaction" field Fahrenheit names at +0x1E8 is a guess: log it first); (2) ignore taps ~0.5 s after a text box,
battle or menu closes; (3) log Tidus's body heading against the head yaw per tap (a tap that does nothing may be Tidus
facing elsewhere); (4) turn Tidus toward the gaze before pressing A, only if (3) shows that; (5) an optional slow-reach
trigger (grip held, hand ~40 cm out, fires once). Not recommended: touching the actual target, dwell / hold-to-repeat,
dropping the grip. Also from JKXR: their menu pointer maps controller angles straight to a cursor, the cheap route to the
parked F8 laser pointer.

## Overdrives in VR: ideas parked (3 Nov 2026)

The owner: "we'll come back to that when I run out of important stuff to work on". Most overdrives are timed controller
inputs, and the tap gesture's pad injection (hv_tap_hook, the gate, the release guarantees) already turns a gesture into
a game input. The controller always keeps working, so a missed gesture costs no more than a missed press.

| Overdrive | The game wants | VR version | Effort |
|---|---|---|---|
| Lulu, Fury | spin the right stick as often as possible | draw circles with a hand: its angle about a centre point becomes the stick angle | small |
| Tidus, Swordplay | a press while the cursor is in the bar's zone | swing the controller like a sword: the swing is the press (the tap's detector with an arc) | small |
| Wakka, Slots | a press per reel | a punch or throw per reel | small |
| Auron, Bushido | a button sequence shown on screen, against the clock | a slash per arrow (up / down / left / right), a left / right thrust for the shoulder buttons; the player reads the prompt | medium |
| Auron, "Beat Saber" | (same) | read the sequence from memory and draw one 3D target per input to slash through (the hands S1 box renderer can draw them) | large |
| Kimahri, Rikku, Yuna, aeons | menu choices | nothing worth doing | - |

What every one needs first: a reliable "an overdrive minigame is running, and which" signal (a gate like the tap's battle
and text-box checks), and for Auron the expected sequence if it is readable. Plan when picked up: one research round
(FFXDataParser, Fahrenheit's battle structs, a capture of each overdrive: start it, log the frame census and the actor
table), then Lulu's circles and Tidus's swing, then Auron's slashes, and the Beat Saber version only if the sequence is
in memory. Haptic feedback on every gesture that counts.

## Special views to check in play (3 Nov 2026)

Places where the player is not simply Tidus walking around; the chocobo case (Tidus attached to the bird's bone, branch
`chocobo`) gave a general "the followed actor is attached to another actor" rule that should cover the first group. From
game knowledge, not captures: take an actor-table log, one image and a frame census at each when reached.
- **Rides / attachment:** the shoopuf on the Moonflow (may play as a cutscene), the Bevelle Cloister glyph track, lifts
  and elevators (Mushroom Rock Road, Home, temples), the airship bridge and deck, the SS Liki / Winno decks.
- **Minigames with their own cameras:** the Calm Lands chocobo training games and the Remiem race (the notes said: treat
  as cutscenes unless they are field control; the riding rule may now catch them), Thunder Plains lightning dodging
  (field control, a screen-flash cue), the Macalania butterfly hunt, Cactuar hide-and-seek, the Jecht Shot, blitzball.
- **Swimming:** Baaj, the Besaid swim, Via Purifico, the Gagazet underwater passage (eye height is already a backlog item).
- **Battles:** Evrae on the airship deck (the distance changes), big bosses from the summoner's view (aeons already hand
  the eyes to the summoner: check only the scale), overdrives (above).
- **Special screens:** the airship destination map and its coordinate search, the sphere grid (a 3D view), the Monster
  Arena menus.

## Chocobo rides: how the game does it, and first person on the bird (branch `chocobo`, 3 Nov 2026)

The owner's first ride (Calm Lands, `na/nagi0000`): three actor-table dumps a few seconds apart while moving the bird, a frame
census and eye captures. First person showed the map's edge from about the world origin, with the chocobo and its rider far
off. This answers the questions left open in "Chocobo rides, expected VR problems" (21 Oct) and exe-analysis section 14.

### What the game does
- **Two actors, and a swap.** On foot Tidus (`c001`) is actor 0. Mounting, the chocobo (`f051`) takes actor 0: it is the one
  that moves between the dumps (-79.7, 45.2, 34.1) -> (-136.2, 28.6, -77.9) -> (-66.3, 9.9, -202.2), it is the actor the
  player steers, and it is the only actor with bit 0x04 of +0x198 (`00000115`; the script setter 0x82B130 owns that bit; the
  other birds read `00000111`). Tidus comes back as a new actor in a high slot (28).
- **The rider is attached to the bird's bone.** +0x19C = the parent actor, +0x1A0 its bone, +0x1A4..+0x1B0 an offset (w 1),
  +0x1B4 a scale (setters 0x832630 / 0x832680 / 0x8326E0; 0x832630 resolves bone and offset from an attach point,
  0x833A70). The root builder 0x832760 then makes the rider's root +0x1D0 = scale x parent bone x offset x parent root, and
  **never reads his +0x0C**. That is why `c001` shows (0.0, 36.3, 0.0), "drawn no", in every dump: a stale script position.
  The wild birds' riders in the same table look the same (`n064` actor 14 on bird 13, `n069` actor 16 on bird 15, both at
  (0, 36.3, 0)), so this is the game's general rider pattern, not something of Tidus's.
- **The rider's sub-positions are world positions.** optpos is built from +0x1D0 (0x8350F0), so the head bone is right while
  riding. Our "fold it in if it looks relative" rule (head more than 100 units from +0x0C: add +0x0C) fired on every dump and
  put the head 36.3 units low.
- **FFX-60fps** (SMOOTH-002-CHILDREL, on by default) moves an attached actor with its parent's interpolation, so the rider's
  +0x200 at the post-join snapshot is what the frame draws.
- **The mount moment**: for about a second the table had no `c001`; first person took `c002` (Yuna, script-hidden, parked at
  (0, 0, 0)) as "the party model with the lowest id" (`22:19:30 following actor 1 'c002'`), then `c001` at 28.
- The frame census and captures show nothing else wrong: the rider is drawn (with the hide centred on (0, 36.3, 0) it was
  never matched, so first person drew Tidus), the mini-map goes to the UI pass as usual.

### What first person does now
- **Riding = the followed actor is attached** (+0x19C non-zero), field only (battle follows the roster, unchanged). No name,
  map or flag guess: an on-foot actor has no parent, so the `f176` "nothing matched" case on `hi/hiku2100` and every other
  map are untouched. The followed actor stays Tidus (the fp_actor rule), so mount and dismount are the existing re-pick.
- **Position**: the rider's drawn root +0x200, from the post-join snapshot when fresh (`D7Join` now also carries the parent's
  +0x0C / +0x6C / +0x200), else read at Present. The head gets the bird's interpolation (head + bird's drawn - logic root),
  never the fold. The F8 smooth-anchor status says "riding: the rider's drawn root (60 fps, on the bird's bone)".
- **Heading**: unchanged. First person's heading is the latched one plus stick turns plus the headset, never the actor's;
  the left stick (move_remap) steers the bird relative to where you look, as it walks Tidus.
- **Bob smoothing** (`ride_smooth`, `ride_smooth_ms`): the rider's offset from the bird's drawn root is low-passed (default:
  the height only, 150 ms), and the eye base is the bird's root plus that. The bird's root glides at the drawn rate, so the
  eye never trails a sprint and does not lag a climb; only the seat's bob relative to the bird is filtered (a 2.5 Hz bob is
  cut to about 40 %). Mode 2 also filters the sideways / fore-aft sway. 0 = off. It applies to every view that follows the
  player (first person, third person, the diorama: all anchor on ffxvr_player_set's position).
- **Body hide**: ffxvr_ride_set (ABI 32) gives native the rider's drawn root (the hide centre, since player_set may now
  carry the smoothed eye base) and the bird's. (Superseded after headset round 1, below: a fixed `ride_hide_radius` of 6.) While riding the step margin is capped (`ride_hide_cap`, 3 units: the margin
  `0.75 + 1.5 * step` grows with speed; the three dumps give the bird about 14-36 units/s, 0.25-0.6 units a Present, so the
  cap is a guard for a fast sprint or a hitch, not a measured problem), and a draw nearer the bird's root than the
  rider's is the bird's and never hidden as the player's. The bird is drawn (you see its neck and back in front of you) unless
  "first person: also hide the chocobo" (`ride_hide_mount`) is on; its shadow then goes too with fp_self_shadow 0. The body
  modes (whole body hidden, head parts with the VR hands) apply to the rider as on foot.
- **The mount moment, rider root (review CR-1)**: Tidus comes back in a fresh slot, and his +0x200 is written only by the
  logic job, so the first Present can see about (0, 0, 0) or the previous occupant's root. A root that is about zero, or 60+
  units from the bird's, is "not built": that frame uses the last good root (same rider, same parent) or the bird's root,
  with no head (the eye is that root + eye height). The snapshot's root is also refused when it is more than 16 units from
  the one read at Present (as SM-1 on foot), and is not mixed with a bird root read at Present (one source for the offset).
  The riding check line counts the unbuilt frames.
- **Which attachments are smoothed (CR-2)**: only a parent at actor 0, the steered bird. Cutscene carries, lifts and throws
  attach the followed actor the same way and follow the bone unfiltered. The filter snaps when the offset jumps more than 6
  units (three times a running bob of about +-2), not 20.
- **The re-pick jump (N4)**: when the attached flag changes, ffxvr_ride_set zeroes the body-hide step margin, so the bird
  -> rider and rider -> Tidus jumps do not widen the hide radius for two frames (that flashed the bird or an NPC beside Tidus).
- **Logs (N1)**: the riding line is written for a new rider or a new known mount (a mount check dropping to "unknown" is not
  a change), at most once a second.
- **The mount moment**: a script-hidden party model now ranks after actor 0 (`RideLogic.FpScore`: 200000 + chr_id), so the
  second without `c001` follows the bird at actor 0 instead of jumping to Yuna at the origin. A visible follower still beats
  actor 0; a hidden one is still taken when nothing else is live. So, generally (FP-1 / FP-2 rule): a script-hidden party
  model is now a last resort below actor 0, and as a score of 100000+ it is rescanned every frame, so an un-hidden one is
  taken back at once.
- **Logs**: once per change, `riding: 'c001' (actor 28) is attached to 'f051' (actor 0, bone N) on map ...` with both roots
  and the settings; 1.5 s later one `riding check:` line (nearest skinned draw to each root: the rider's should be about 0, so
  the hide matches; the counts the hide matched last frame); `riding: off` on dismount. The actor table has an `att` column
  (the parent's index, `ext` outside the table) and prints an attached actor's drawn root; its "drawn" test (and the 10 s
  `actors:` line's) uses that root.
- **F8 > Debug > First person / follow**: "chocobo: riding: f051 (actor 0) - rider parts matched last frame N, bird parts M",
  the bob smoothing radio buttons and ms slider, the hide-the-chocobo checkbox and the hide cap; the model-names line ends in
  "| riding: f051 (actor 0)".

### Not done / open
- Per-part hiding of the bird (only its neck) would need the part finder for the mount's draws; the whole-bird toggle first.
- The chocobo mini-games (Calm Lands training, the races) are untested: if they are ordinary field control the above holds;
  if they run scripted cameras, the seat view is the fallback.
- The seat height while riding comes from the head bone (`fp_head_anchor` 1, the default). With `fp_head_anchor` 0 (fixed
  eye height) the eye is the rider's (smoothed) root + `fp_eye_height` (18). The rider's root is the seat, not the feet, so
  that eye sits roughly a seat height above his head: keep the head-bone height for riding.

### Headset round 1 of `chocobo` (ea5a22f, 3 Nov 2026): Tidus half visible, the tap, and a crash

The owner: "this works fairly well. It seems like Tidus is visible about half of the time (depending on the chocobo stride
maybe?) also we had a crash eventually."

**Tidus visible about half the time.** The `riding check:` line showed the hide right at that moment (rider parts 13, the
nearest skinned draw 0.00 units from the rider's root), so the miss was intermittent. Cause: while riding, the radius was
`min(0.75 + 1.5 x step, 3)`. The step came from ffxvr_player_set's position, which is the bob-smoothed eye base, so at a
canter it stayed near 0.75-1.6. The rider's root moves with the bird's bone, and the bone only updates at the 30 Hz ticks
(FFX-60fps' CHILDREL interpolates the parent's root, not its skeleton). So on frames where the hide centre and the draws
were a tick apart, the parts were one stride step (1-2 units) off and were missed. Fix (ABI 33): while riding the radius is a
fixed `ride_hide_radius` (default 6), independent of the step. Nothing else is at seat height: an NPC's root is on the
ground, 15 units below. The bird rule is unchanged (a draw nearer the bird's root is the bird's). The ea5a22f key
`ride_hide_cap` is read and ignored. Evidence for next time: every 10 s while riding in first person, a
`riding hide (10 s, first person)` line gives the frames, the frames with the rider matched and missed, how far the
matched parts were from the centre, and the nearest unmatched skinned draw within 12 units (ffxvr_ride_stats).

**The tap while riding.** 34 taps fired on the bird (`hands: tap L/R: A pressed`, the gate "field (open)"). The owner was
moving his hands about, and the grip armed the pushes. His decision: no taps from the bird ("you can't really touch someone
from the chocobo anyway"; press A). The gate now has a `riding` reason (game bit 32, from `_ride_att`). The taps did nothing
visible in the log: no `riding: off`, no text box, no map or event change.

**The crash (23:22:58).**
- **The fault.** An access violation writing to address 0x10 at FFX+0x2D1736 (gva 0x6D1736, `mov [esi+0x10],eax` with
  esi = 0), on the game's main thread. The image base in the dump is 0x130000, so the console's `eip=00401736` is the
  same address.
- **The call path.** WinMain's loop calls the renderer frame (0x642BBC -> 0x668930), then the scene render 0x653600, then
  the ShadowManager singleton (gva 0xCDEC28, via 0x6D2390) at 0x6C9E50 -> 0x6CA2D3, which reaches 0x6D1530.
- **The cause.** 0x6D1530 walks tile list 1 (count at manager+0x37AC; the dump shows it on its first entry, i = 0). For a
  slot without an instance it asks `GetASpareGradMeshInstance` (0x6CC980). That function scans 16 in-use flags for the
  current buffer half (manager+0xDC4 + half x 16, the half at gva 0xCDEC24 flipping per frame) and, when all are set,
  printf's "GetASpareGradMeshInstance() , Shouldn't get here, contact cg please" and returns null. The caller writes through
  the null.
- **(Corrected by review 2, below.)** The first reading here, that nothing ever frees the pool, was wrong: ShadowManager::Render
  frees all 32 flags at the end of every render (0x6CADD0, addressed as `[esi-0x10]` / `[esi]` off +0xDD4, which a grep for
  `+0xdc4` cannot find). A null on the first entry means this render's earlier walks (list 0, then list 1 of 0x6D12F0) had
  already taken the 16 instances.
- **The stall line is not a hang.** `stall #3` (4125 ms, main thread in coreclr) is the crash in progress: the exception
  being dispatched and the core dumped. The 2D1733 / 2D1736 values on its stack are copies of the faulting site.
- **Our likely part (medium confidence).** In Ch_DrawMain the shadow-tile request (0x830240 -> 0x82E3E0 -> 0x644A00 ->
  0x6D3000) runs only for actors whose Ch_VisibleTest passed (`test edi,edi; jne` at 0x82F4B9). Our "keep characters
  drawn" patches make that test pass for every actor. In the Calm Lands that is about 25 actors spread over 2 km (birds,
  riders, NPCs, objects), against the handful the game's camera would show. So the ShadowManager handles far more shadows
  than its designers allowed for, in a pool with a hard limit.
- **Not ours.** The A presses (no input in this path), the ride hide (render-thread draw skipping only), the smooth anchor
  and the ride probe (reads only), and the grip strip (input only). FFX-60fps doubles the render invocations; whether that
  speeds the pool up is unknown.
- **What settled it:** review 2 (chocobo-review2.md) traced the per-render allocator. See "The shadow pool, corrected".

### The shadow pool, corrected (review 2; ABI 35, branch `chocobo`)

The prevention in f654547 (ABI 34) rested on a wrong reading, and review 2 found two blockers in it. This section replaces
that section, which is kept below for the record, struck by this one.

**How the pool really works: a per-render scratch allocator.** ShadowManager::Render (0x6C9E50, main thread; once a frame
from the scene render), on its normal path:
- flips the buffer half (0x6CA1A4);
- zeroes the first 16 tile slots of both lists (0x6CA1B0);
- hands one of the half's 16 instances to every in-range tile entry (lists 0 and 1, so at most 2 per shadowed character):
  0x6D12F0(0), 0x6D12F0(1), 0x6D1530(1), each through GetASpare 0x6CC980;
- draws each entry from its slot (0x6CAB60..0x6CAD4E);
- frees all 32 flags (0x6CADD0).

The halves are for GPU double-buffering, not a session budget. The tile entries come from this pass's shadow requests.
Ch_DrawMain clears the 64 caster marks (+0xFE8) before loop 3 (0x82F1FF -> 0x6D1780), and each request
(0x830240 -> ... -> 0x6D3000) sets one (0x6D30F0). So **a render crashes when more than about 8 shadowed characters are in
the tiles at once**. It has nothing to do with session length: the 1.5 vs 3.7 minutes says where the owner rode, not how
long. Vanilla requests shadows only for visible characters, a handful. "Keep characters drawn" makes all ~25 Calm Lands
actors request one.

**B1, the f654547 guard, still crashed.** Its skip left the slot null, and the draw loop in the same render reads it
(0x6CACCE, then `push [eax+0x10]` at 0x6CAD03). **Fix:** on a null the stub now does what the loops' own "nothing to do" paths
do. It stores the default instance, manager+0x8B8, into the slot (`mov eax,[edx/ebx+0x8B8]; mov [edi],eax`, as at 0x6D15A9
and 0x6D1355), so the draw loop takes its default branch, as for an empty tile. Then it replays the loop end and continues.
- Those bytes are now in the verified set.
- The stub stride is 128 bytes.
- The cave is made execute-read after building.
- What the player sees: that shadow is missing for that one render. The next render starts with a free pool.

**B2, the f654547 budget, never fired.** It read the pool flags on the main thread, after the render had freed them, so
the count was always 0. **Fix: a caster budget from the marks**, counted in the same Ch_DrawMain pass.
- `sg_gate` (still replacing the call at 0x82F4BE) reads the marks before and after each request (+0xFE8, 64 dwords). A
  drop in the count means a new pass.
- Characters within `shadow_near` (300 units, about 26 m) always ask for a shadow.
- A farther one is skipped once this pass's far casters reach `shadow_budget` minus the near casters. The near count is the
  larger of this pass's and the last pass's, because the actor walk is in table order: far birds come first and Tidus on the
  bird is slot 28.
- `shadow_budget` counts characters, not instances: default 8 (at most 16 instances), range 2-8, shown in F8 as "characters
  with a shadow".
- If more than 8 near characters ask, the guard catches the rest.

**Re-review fixes.**
- The 0x644B30 job-join hook is now always installed (MF-1). `h7_join` calls `sg_join_install` on every join, past the
  main-thread check of `ffxvr_main_note`, so the guard is present even with `smooth_anchor=0` and `diag_build7=0`. Its
  snapshot work still runs only when those settings ask for it.
- The install line and the "not installed yet" warning have separate flags (SF-1), so a late first join still logs the
  install.
- The budget applies only while "keep characters drawn" is on. Without that patch the game's own visibility keeps the
  demand to a handful, so vanilla shadows stay untouched.
- A second shadow requester bypasses the gate: 0x830180, called from Sg_MainLoop at 0x821465 only when [0x12FBBB1],
  [0x12FBBB9] and 0x843210() say so. It requests for mode-1 actors with +0x198 & 0x10 without a visibility test. It is not
  budgeted or counted in the readout; the guard covers it.

**Install (S1, S2, S3).** Only on the main thread: in the job-join hook, past its main-thread filter, at Sg_MainLoop's
post-join, so never while the main thread runs these sites. The render-thread fallback is gone: the gate's rel32 at
0x82F4BF straddles a cache line, so writing it from another thread could be fetched torn. Without the hook, one warning
says it is not installed. The managed side logs from a flag written after the install result.

**Readouts (S4).** The F8 line and the `shadow pool:` log lines report:
- shadowed characters in the last pass (and how many were near);
- the peak in a pass;
- the far characters left without a shadow;
- the renders where the guard saved a shadow from crashing (it should stay 0 with the budget).

Gone: the instance counts per half, and the "missing until restart" warning.

**Tests that would have caught B1 / B2** (choco_chk/tests/sg_test.cpp). A model of Ch_DrawMain's marks and of the render
(the flip, the slot clear, GetASpare, the draw loop's slot read, the free) over the 22:20:16 Calm Lands positions. It
reproduces:
- the original crash (0x6D1736);
- B1 (the old stub: a null read at 0x6CAD03);
- B2 (the flag budget never skips).

It then shows the new stub and budget: no crash in 10 passes, no overflow after the first pass, Tidus shadowed every pass.
`sg_replay_check.py` checks that each stub's null path is the exe's own default store plus the loop end. `sg_check.py`
checks every verified byte array against the exe.

### (Superseded) The shadow pool: prevention (ABI 34, f654547)

Kept for the record. Its "never frees" premise and its budget are wrong: see the section above.

The owner: "Can we exclude shadows when we get too many away from the character's immediate area?" There are two parts, and
both are installed once, each only over the bytes it expects.

**Does the pool ever free?** No, not while the game runs. The in-use flags (ShadowManager+0xDC4, 16 per buffer half) are
cleared in exactly one place, the pool's setup 0x6D1B40. That setup is reached from the renderer's resource build
(0x6364D4 -> 0x6CE100: start-up, and whenever the renderer rebuilds its targets). It is not run per frame and not per map
load. The flip of the buffer half (0x6CA1A4) clears the slots' cached instances (+0x8BC, +0x9BC: 16 each) but not the
flags, so after a flip the slots ask for instances again. Whether vanilla play flips often enough to run out, I cannot tell
statically. In our setup the pressure builds over the whole session, across maps.

**1. The distance budget** (the owner's request): Ch_DrawMain's `call 0x830240(actor)` at 0x82F4BE (the per-actor shadow
request, reached only for actors that passed Ch_VisibleTest) now calls `sg_gate`.
- `sg_gate` skips the request for an actor whose drawn root (+0x200, which is right for riders too) is farther than
  `shadow_near` from the player, once the pool already holds `shadow_budget` instances (the fuller half).
- The skipped characters stay drawn, without a shadow. The near ones keep theirs, because the budget leaves 4 instances for
  them.
- Defaults: `shadow_near` 300 units (about 26 m at 11.4 units a metre), `shadow_budget` 12.
- Why 300: it covers talking distance and a chocobo's short dash, and a character's blob shadow at 26 m is a few pixels in
  the headset. Most of the Calm Lands actors in the dumps are 450-1800 units away, so they are the ones that go
  shadowless.
- `shadow_near` 0 turns the budget off.
- This is the decision point the game itself uses: tiles and list entries come from these requests each frame. Since an
  instance is never given back, skipping far requests once the pool is near full is what keeps the remaining instances for
  the near characters.

**2. The guard (always on): no crash when the pool is full.**
- Both `call 0x6CC980` sites (0x6D16F5 in the tile list 1 walk, 0x6D14AA in the list 0 walk) call a 64-byte stub in an
  executable page we allocate. The stub calls the pool scan. On a null it drops its frame and counts the hit. Then it
  replays the loop end without the two stores through the instance (the register reloads and the 5 or 4 x87 pushes the
  loop top expects) and jumps to the loop's continue point (0x6D174F or 0x6D14FA).
- The x87 stack is empty at the call: MSVC code keeps it empty across calls. Each loop's prologue pushes exactly as many
  values as its loop end, so the body pops everything first.
- **What the player sees when the pool is full:** that character's shadow is missing for the frame (in practice from then
  on, as nothing frees), instead of the crash.
- **The install** runs from the job-join hook on the main thread, between frames, so the main thread never runs code that
  is being rewritten. Without that hook it runs from the render thread after 600 frames.
- **Every byte is checked first:** the call, the loop end it replays, its continue point, the pool scan's prologue, and the
  gate's context. A mismatch is logged and left alone. Nothing is restored on unload, as with the culling patches.
- **Logs:** one `shadow pool: guard on (both tile lists), distance budget on (the main thread)` line; `shadow pool:` lines
  as the counts change, with the instances in use per half, far characters left without a shadow, and the guard's skips;
  and a warning the first time the guard fires. The same readout and both settings are in F8 > Debug > Character shadows.
- **Checks:**
  - choco_chk/tests/sg_check.py: every verified byte array equals the exe copy, and the calls decode to 0x6CC980 and
    0x830240.
  - sg_test.cpp: the stub builder and the budget rule.
  - sg_replay_check.py: each stub's skip path is the original loop end minus the stores, and jumps to the continue point.

## Hands hidden in battle (3 Nov 2026)

The owner: "our VR hands are amazing but I think we need to completely hide them in battle. They are distracting and look
bad when showing this off." Native `hands_hide_why` now returns `H2_BATTLE` ("battle" in the hands: counters) while the
game-gate bit 1 (Btl.battle_state != 0) is set; managed now reads the game gate whenever VR hands are on, not only while
the tap is on (the tap's own gate is unchanged: it still checks its own on/off first). A stale gate (managed not pushing
for 3+ frames) is not read as a battle. Battle transitions were already hidden (H2_BT). No option for now.

Parked with it (the owner): hands could come back for overdrives (see "Overdrives in VR"), but then they should belong to
whoever is acting, so **import hand packs for every playable character** (Tidus, Yuna, Auron, Wakka, Lulu, Kimahri, Rikku)
and tie each pack to its character's chr_id (the battle follow already knows whose turn it is: `cur_chr_id`). The importer
already handles a full-body T/A-pose OBJ; the per-character packs stay private (made from the player's own models). Needs:
a pack-per-character mapping in hand.json or the ini, the F8 list per character, and Kimahri's and Lulu's sleeves/claws
checked by the importer's limits.

## FMV depth converter (3 Nov 2026)

Branch `fmv-depth`. Stages S0 (quality check) and S1 (converter v0) of the FMV "relief screen" research; no runtime or
mod code yet. Setup and use: docs/setup.md, "FMV depth". Format: tools/fmvdepth/FORMAT.md. (Converter 0.2, below, changes
the windowing, the normalisation, the edges and adds a temporal filter; the bullets here describe 0.1.)

- **What the game has** (measured read-only on the owner's copy). The FMVs are WebM: VP8 + Vorbis, 29.97 fps
  (30000/1001), no colour metadata (treated as BT.601 limited range), inside `data\FFX_Data.vbf` under
  `ffx_data/gamedata/ps3data/video/` (95 .webm, flat). Per-region lists `video/<us|jp|asia|cn>/ffx_videolist.txt`, lines
  `<id>:,<file>,<file>` (both names always equal), mixed case (`OPN_us.webm`, `WithoutEvent_480.webm`). The US list: 61
  videos, 86,353 frames, 2,881 s, 2,624 MB; 6 of them 640x480 (ids 106-116), the rest 1280x720. END_us alone is 24,828
  frames. Bink is not involved: any FFmpeg decodes VP8 natively. `cn` names .mp4 files that are not in this archive.
- **Pipeline** (tools/fmvdepth/convert.py), per video, streaming: `vbf.py` inflates the 64 KiB blocks (raw, 0 = 65,536
  raw, or zlib) into ffmpeg's stdin -> `scale=WxH:flags=bicubic:in_color_matrix=bt601:in_range=tv`, rgb24, every
  decoded frame once (`-fps_mode passthrough`, `-vsync passthrough` on FFmpeg < 5.1) -> Video-Depth-Anything-Small in
  fp16 autocast, DINOv2 attention swapped for `scaled_dot_product_attention` (same maths; the plain version builds
  ~2.4k x 2.4k score matrices per head and frame at 518x924) -> area-downscale to 320x180 on the GPU -> optional edge
  filter -> per-shot normalisation -> left predictor -> XPRESS_HUFF -> `<name>.fvd.part`, renamed when complete.
  Model input as VDA's own Resize: 924x518 (16:9) / 686x518 (4:3) at 518, 700x392 / 518x392 at 392. Same size, but
  not the same resampler: upstream resizes float frames with cv2 INTER_CUBIC (no antialias); here ffmpeg's bicubic
  scaler (which widens its filter when shrinking) makes uint8 rgb24 directly from the YUV planes, so the model sees a
  slightly softer, requantised image. Expected to matter little; S0 is where that shows.
- **Windowing** (window.py): upstream `infer_video_depth` keeps all frames and full-size float depth in RAM (END_us:
  ~160 GB), so it is re-implemented as a generator with the same maths: windows of 32 (frames 0..31, then the previous
  window's KEYFRAMES [0,12,24..31] + 22 new frames, padding with the last frame, ceil(n/22) windows), a least-squares
  scale/shift per window from slots 0/1 against the reference depths of the same frames, the 8 shared frames
  cross-faded 0..1, clamps at 0. Slot 0 is video frame 0 for the whole video, as upstream. Frames leave 8 frames late;
  at most two windows of input frames are alive. One deliberate difference: the fit and cross-fade run on the 320x180
  maps (linear per pixel; only the fit's sample set changes). A test checks the generator against a batch
  re-statement of upstream (with copies of upstream's own fit and interpolation helpers) on 19 lengths (1-333 frames) to
  2e-5; another test checks that .fvd frame i comes from video frame i.
- **Normalisation**: `q = 255 * clamp((d - lo) / (hi - lo))` rounded to nearest, with a floor on `hi - lo` for flat
  frames (the exact rule: FORMAT.md, "The values"), 0 far ... 255 near (relative disparity), lo/hi = p2/p98
  EMA-smoothed (alpha 0.9 on the past) within a shot and reset at a cut. Cut = mean absolute luma change of 64x36
  thumbnails >= 28 levels and >= 3x the recent average (+1), so fades and pans do not flag every frame. Raw p2/p98
  and the lo/hi used go into the index; the runtime picks strength and pivot itself.
- **.fvd v1**: 128-byte header (magic `FVD1`, version, size, frame count, fps, codec, predictor, model, flags, input
  size, settings hash, index/thumbnail offsets, source size + VBF offset + name, the name lower-case as the VBF stores
  it), payloads, then the index (32 B a frame:
  u64 offset, u32 size, u16 flags cut/stored, f32 p2, p98, lo, hi) and a 16x9 (12x9) luma thumbnail per frame at the end,
  so the writer can stream; the header is rewritten with HF_COMPLETE last. XPRESS_HUFF in buffer mode (not
  COMPRESS_RAW), via ctypes to cabinet.dll; zlib (codec 1) for tests and non-Windows; a frame that does not shrink is
  stored (FF_STORED). The writer decodes every payload again and compares, so a codec fault on the owner's PC cannot
  produce a bad file silently. Reading needs one ReadFile + one Decompress per frame; END_us's index is 0.79 MB.
  FORMAT.md lists the checks a reader makes before using any offset or size (payload <= w*h, == w*h when stored) and
  the version rule (major version; compatible additions only in ignored flag bits, reserved fields or header bytes past
  128). Readers open with FILE_SHARE_DELETE so the converter's final rename works while the game runs.
- **Resume / manifest**: `manifest.json` per video (status, source size, VBF offset, model, weights SHA-256, settings and
  their hash, converter version, frames, seconds, bytes, cuts, peak VRAM). A video is skipped when the manifest says done
  with the same source and settings hash and the .fvd header agrees; `--force` redoes it. A crash leaves only a .part,
  deleted on the next attempt. Out of VRAM: a plain message suggesting `-InputSize 392`, exit 3.
- **Pins**: VDA code at 4f5ae23 (GitHub zip; the 17 imported files + LICENSE checked by SHA-256, and no unexpected
  .py file allowed in the imported packages: the known list is `git ls-tree -r 4f5ae23 -- video_depth_anything utils`,
  recorded in vda_model.py and tested against VDA_FILES; verify passes on a clone of that commit), Small weights only
  (Base and Large are CC-BY-NC-4.0 and are not offered), torch 2.6.0 / torchvision 0.21.0 (cu124 index), numpy 2.1.3,
  opencv-python-headless 4.10.0.84, einops 0.8.0, easydict 1.13, tqdm 4.67.1; no xformers. The weights' SHA-256 is not
  known yet (Hugging Face was unreachable while writing): recorded on the first download and checked on later runs.
  The launcher also records what Hugging Face says about the file (a HEAD request without redirect: X-Repo-Commit,
  X-Linked-Etag, X-Linked-Size, ETag, Location). Of those, only two are compared: a download whose SHA-256 differs from
  X-Linked-Etag (when that is a 64-digit hash) or whose size differs from X-Linked-Size is refused; the others are only
  recorded. A re-download that differs from the hash this PC recorded the first time is refused too (deleting the
  `.sha256` record is the explicit way to accept a changed file). While unpinned it prints a block on every run asking
  the owner to send those lines. `$WeightsSha256` (and a `resolve/<commit>` URL) is the TODO to pin.
- **Protection** (like the private hand packs): PRIVATE-do-not-share.txt in `fmvdepth\` and `fmvdepth\preview\`;
  package-release fails on any fmvdepth path, .fvd, .part, video or preview; install-mod never deletes it (-Purge keeps
  it and names it, and still refuses when the mod folder itself is a junction); build-mod reports it separately; run-ffx
  does not collect it (prefix from the normalised game path, plus a path-component test); .gitignore (`**/fmvdepth/`
  except `tools/fmvdepth/`, `*.fvd`, previews); the converter refuses a git checkout and the game's data folder as
  output, by the literal path and with junctions and symlinks resolved; the launcher pre-checks the literal cases in
  PowerShell before the install and asks the converter (`--check-out`) before its write test. A video name that is
  not `[A-Za-z0-9_]+.webm` (or is a Windows device name) is skipped with a warning. `$FfxvrFmvDepthDir` in paths.ps1 names the folder for all of them.
- **For the runtime later** (static analysis of FFX.exe, not yet confirmed in game): manager `*(void**)0xCDED3C`
  (FhUtil 0x8DED3C); playing = byte mgr+0x6D2 (corrected by the sync probe below: playing is +0x6D0, +0x6D2 is "open"); file name `char[0x80]` at mgr+0x4F0 (`OPN_us.webm`, mixed case as in
  FFX_VideoList.txt, "" when idle; the .fvd's src_name is lower-case: compare case-insensitively); id =
  int mgr+0x6DC; the player object is embedded at mgr+0x38 and its u32 +0x100 counts uploaded frames, so the frame on
  screen is that count - 1 = the .fvd frame index (clamp to the frame count). Depth file: `fmvdepth\<name lower-cased
  without .webm>.fvd`. If the decoder drops frames that count under-counts: the per-frame thumbnails allow a content
  check (+-3 frames). Ids 32/46/70/72 (MST_us, FAR_us, GGM_us, SNS_jp) have `<id>.dat` files and upload on the render
  thread: probably FMV backgrounds behind real-time 3D, where the relief does not apply.
- **Not verified here** (needs the owner's PC): CUDA torch install and import, VDA load with `strict=True`, fp16 + SDPA
  on Windows, VRAM and speed at 518 / 392, cabinet.dll XPRESS_HUFF through ctypes, the GitHub zip's file hashes, the HF
  download, every PowerShell 5.1 path of the launcher, Windows ffmpeg pipes, frame counts on the real files (the research
  table: e.g. OPN_us 4,750, END_us 24,828). Tested in the container: the .fvd round trip (zlib/stored), normalisation, cuts,
  the windowing against upstream and its constant memory, vbf.py on a synthetic archive, and end to end with synthetic
  VP8 clips in a synthetic VBF and a fake model (resume, --force, previews). vbf.py was also run read-only on the real
  archive's index: the US list resolves to 61 videos / 2,624.4 MB and the archive has 95 .webm, as in the research.
- **Next**: S0 exit = the owner judges SIN_us / BTS_us depth plausible and notes VRAM and frames/s; S1 exit = the whole US
  list converted with total time, peak VRAM and GB measured (pin the weights hash then); S2 = the read-only sync probe in
  the mod.

### Converter 0.2 (ALGO 2, branch `fmv-conv2`, 5 Nov 2026)

Item 7 of the FMV pipeline audit (research `fmv3d-pipeline.md`; techniques from `fmv3d-projects.md` T1/T2/T6/T9 and
`fmv3d-models.md`). The .fvd format stays version 1; the files carry header flag HF_SEGMENT_RANGE, which the runtime
must honour (fmv-relief 9812b55: no per-shot re-normalisation of such files; see "For the runtime" below); every video is
converted again once (ALGO 2 is in the settings fingerprint). The pipeline core is `convert.process_video()` (no I/O;
numpy only: the depth model is the one GPU step), wrapped by `convert_one()` with ffmpeg and the writer.

- **Cut-aware model windows** (`window.shot_windows`): VDA's 32-frame window chain restarts at every cut, so no window
  holds two shots' frames, slot 0 and the scale/shift reference are the shot's own frames, a short shot is padded with
  its own last frame. Cost: each shot rounds up to whole windows (~+5 % on the synthetic test, ~+20 % at an average
  shot of 3 s).
- **Uncertain cuts** (`depthproc.tag_frames`, `find_transitions`): a detector cut whose picture comes back within 4
  frames is a flash, not a cut (no window restart, its frames flagged FF_FLASH and left out of the range); a cut between
  two correlated (>= 0.9) non-blank pictures is a brightness change; a blank frame (64x36 luma std < 2.5 levels) is
  flagged FF_FLASH. Inside a shot, cross-dissolves (sustained change with a near-zero second difference, then checked
  as a least-squares blend of the end pictures, ends >= 20 levels apart) and dips through black (a blank run between
  uncorrelated pictures) start a new normalisation segment; fades to / from black do not. Dissolve and fade frames are
  flagged FF_BLEND (index flag bit 2; FF_FLASH is bit 3; v1 readers ignore both).
- **One depth range per shot segment** (`depthproc.shot_ranges`): each shot is buffered (RAM, then a temporary file in
  the output folder past 2,400 frames = 550 MB), and lo/hi = the 0.1 / 99.9 percentiles of all its reliable frames'
  pixels together, widened by 2 % each end, written as every frame's lo/hi (the v1 meaning d = lo + q/255 (hi - lo)).
  No plateau at the nearest 2 %; nothing sinks back when something enters; across a dissolve lo/hi move linearly
  between the two segments. A segment whose frames are all blank is written flat (lo = hi).
- **Centred temporal filter** (`depthproc.temporal_bilateral`, `--temporal on`): per pixel a weighted mean over t-3..t+3,
  2 passes, weights Gaussian in time (2 frames), in normalised depth (8 levels) and in the frame's luma (0.05; a
  320x180 guide from the model-size frame), inside the shot only. Zero phase: no lag; a pixel whose depth or luma
  changes keeps its own value, so motion passes. Flash / blank frames take their depth from their neighbours.
- **Edges before the shrink** (`--edge-filter refine`, replaces the old late `edge_snap`): the model hands over its maps
  at its own resolution (924x518), and per frame (numpy, `--threads`, default up to 8): `sharpen_edges`, a forced
  foreground / background weighted median after Kopf et al. 2020 (our formulation: pixels in the band a soft step
  leaves, near a step core where the 11x11 range is <= 2.1 x the 5x5 range so slopes and round surfaces are untouched,
  take the weighted median of their settled 5x5 neighbours, Gaussian weights in depth, in-between pixels weight 0,
  4 rounds so the two sides meet at the 50 % crossing); `grow_near`, the near side grown 2 px across and 1 px up/down
  across real steps (iw3's Edge Fix idea, reimplemented from its description); `edge_shrink`, a 320x180 texel over a
  two-valued step takes its larger group's mean. Edge threshold: 5 % of the frame's p2..p98, at least 8x the pixel
  noise.
- **Options**: `--input-size 714` (1274x714; the default stays 518, what VDA-S was trained at, until an A/B says
  otherwise), `--flip` (each window also run mirrored, fitted by one scale/shift and averaged), the settings
  fingerprint logged at the start (a video made with other settings or an older converter is converted again; the
  rest is skipped), converter 0.2.0 / algo 2 in the log and the manifest (which also records segments, dissolves, dips,
  fades, flash / blank / flat frames and the clipped share per video).

Measured on a synthetic FMV (tests/synth.py: a textured disc moving 3 px a frame in front of a textured far plane, two
hard cuts, a 30-frame dissolve, a fade out / black / fade in, a 2-frame white flash, a slow dolly, an object entering)
with a stub model that has VDA-like flaws (edge blur 2 model px, per-window scale / shift, a window's mean leaking into
its frames, a left/right bias, i.i.d. noise and smooth flicker). The audit's measures (fmv3d_exp/fvd_stats.py, e2, e3,
e4) on the stored 320x180 maps; "% step" = levels / the levels between the disc and the plane in that variant's own
mapping. The stub cannot show what real VDA does across a cut, so the windows' benefit shows only as less stride-22
pumping here; the rest transfers as ratios, not absolute values. Scripts: scratchpad/conv2_exp (run.py, metrics.py).

| measure | 0.1 (3d9aaa9) | 0.2 | 0.2 no temporal | 0.2 no edges | 0.2 whole-video windows | 0.2 + flip |
|---|---|---|---|---|---|---|
| noise, median 2nd difference (lv) | 1.54 | 0.07 | 1.35 | 0.07 | 0.07 | 0.06 |
| noise, mean 2nd difference (% step) | 1.28 | 0.33 | 1.23 | 0.24 | 0.33 | 0.31 |
| pumping, frame median (lv / frame) | 0.68 | 0.09 | 0.47 | 0.09 | 0.08 | 0.09 |
| stride-22 power (x mean bin) | 0.51 | 0.14 | 0.39 | 0.27 | 0.61 | 1.03 |
| subject (disc centre) p-p in shot 1 (% step) | 54 | 5.4 | 7.8 | 5.4 | 5.5 | 4.6 |
| disc interior clipped at 255 | 80 % | 0 % | 0 % | 0 % | 0 % | 0 % |
| pixels at 255 / at 0 | 2.5 / 3.1 % | 0 / 0 % | 0 / 0 % | 0 / 0 % | 0 / 0 % | 0 / 0 % |
| silhouette 10-90 % width (video px) | 9.4 | 3.2 | 3.3 | 8.8 | 3.2 | 3.2 |
| halo into the background (video px) | 3.8 | 4.1 | 4.1 | 3.5 | 4.1 | 4.1 |
| bite into the subject (video px) | 5.6 | -0.9 | -0.8 | 5.3 | -0.9 | -0.9 |
| edge crawl, std of the 50 % crossing (video px) | 1.0 | 1.65 | 1.64 | 1.0 | 1.64 | 1.63 |
| edge lag (frames) | -0.09 | -0.10 | -0.10 | -0.10 | -0.10 | -0.10 |
| dolly seen (% of the true change) | 9 | 85 | 88 | 85 | 85 | 92 |
| left/right bias (% step) | 4.1 | 3.4 | 3.4 | 3.4 | 3.7 | 0.8 |
| model calls | 20 | 21 | 21 | 21 | 20 | 42 |

Readings: the temporal filter takes the frame-to-frame noise down ~4x with no lag (the edge lag and the dolly do not
move); the per-shot range removes the subject sinking when the object enters (54 % -> 5 % of the step; the rest is
the stub's own window leak) and the clipped plateau; the edge path narrows the silhouette from 9.4 to 3.2 video px and
puts the whole subject on the near side (bite < 0; the grown 2 model px land in the halo). The cost of a sharp
shrink is that the step sits on the 320 texel grid: its crawl grows from 1.0 to 1.65 px (std; a texel is 4 video px).
Sharpening with a plain area shrink instead gives 5.7 px / 1.08 px; the runtime's guided upsample (audit item 5) is
the fix for the crawl. In the 0.1 column the dolly is "absorbed": the per-frame normalisation pins the plane.

**Time (review S-1).** Three things run at once: a producer thread reads, tags and windows the frames; the model
runs on its own thread one window ahead of the edge pass (window.windowed_depth submits window k+1 before it takes
window k; torch and numpy both release the GIL), which spreads each window's maps over `--threads` (default up to 8);
and the caller's thread finishes the previous shot (ranges, temporal filter, 8 bits, the writer). Padding slots (a
shot's last window is filled with copies of its last frame) get the area shrink, not the edge path, so the edge path
runs ~1.45 maps per output frame (the 10 keyframe slots of every window after the first are real and needed). With a
stand-in GPU (sleeping, on the reviewer's edge-dense maps at 18 % step pixels, 2 threads on this 2-core box): 173 ->
115 ms a frame when the CPU is the bottleneck (here), and 312 ms against 300 ms of GPU alone when the GPU is: the edge
pass is hidden, what remains is the last window of each shot: the next shot's first window is only submitted after
it, so the GPU waits for that window's edges (~30 real maps: ~0.3-0.4 s at 518, ~0.8-1.2 s at 714 on the owner's
CPU by the per-map figures below; round-2 review R2-N-3).

Edge-path cost per map, one core of this 2.1 GHz Xeon: 38 ms on the synthetic video (few edges); on edge-dense maps
88 / 148 / 339 ms at 8 / 18 / 35 % step pixels (924x518) and 190 / 416 / 665 ms at 1274x714 (the reviewer's
t_dense.py). How dense VDA-Small's maps of FFX's CG are is not known (10-20 % assumed). Model for the owner's PC (a
P-core ~2x this one, 8 threads ~6x one P-core; the 3080 Ti at 16.4 ms a frame in 0.1, +20 % for per-shot windows):

| settings | GPU a frame | edge pass a frame (10-20 % dense) | bound | shot-end gaps | 86,353 frames |
|---|---|---|---|---|---|
| defaults (518) | ~20 ms | 13-18 ms | GPU | ~5-7 min | ~35-45 min (up to ~1 h at 35 % dense: CPU-bound) |
| -Flip | ~40 ms | 13-18 ms | GPU | ~5-7 min | ~1.1-1.2 h |
| -InputSize 714 (model 2.2-2.7x) | 44-54 ms | 30-51 ms | mostly GPU | ~13-20 min | ~1.3-1.6 h |
| -InputSize 714 -Flip | 88-108 ms | 30-51 ms | GPU | ~13-20 min | ~2.3-2.9 h |

Assumptions, all estimates: a P-core ~2x this Xeon and 8 threads ~6x one P-core; 10-20 % of the pixels near a depth
step; VDA-S at 714 costing 2.2-2.7x its 518 time; **~1,000 shots in the US list** (86k frames / ~90 a shot: a guess,
the converter's log will give the real count), each costing one shot-end gap. The review's estimates for the
unoverlapped pipeline were 45-60 min and 2.5-3.5 h; the model factors are from fmv3d-models.md and the review; nothing
was run on a GPU. (Removing the gap would need the next shot's first window submitted before the last one's result
is taken, across the shot boundary: not done.)

**Ctrl+C (round-2 review R2-S-1).** On Windows a plain Queue.get() / Thread.join() is not interrupted by Ctrl+C before
Python 3.14 (cpython #74157, #139689), and the launcher accepts 3.10-3.13; so the caller's thread waits for the next
shot, and for the producer on the way out, in 0.25 s steps. The producer checks the stop flag after every depth map of
a shot (within one model window), and ffmpeg, which runs without a console (CREATE_NO_WINDOW) and never sees Ctrl+C,
is killed at once (FrameReader.kill). So Ctrl+C lands within a quarter of a second and the run is wound up within one
model window (a second or a few at 714 + flip); the executors' final shutdown waits for a model call in flight.

Left as it is (R2-N-4): a fade of 28+ levels a frame during a fast pan (8+ model px a frame) leaves the pan in the
affine residual, so same_picture says "different" and every step of that fade becomes a cut (1-frame shots). Rare in
FFX; if it shows up, accept a step whose brightness change has the same sign as the detector's previous step (a fade
is a ramp) at a lower correlation.

Not verified here: VDA with full-resolution output (the same forward, without the area resize: ~1.9 MB a frame over
PCIe), VRAM and speed at 714, real VDA edges (the stub's blur of 2 model px is a guess; sharpen_edges handles 1-4
px), dissolve detection on real FFX dissolves (tested on synthetic blends and pans), the threshold of the flash test
on real white flashes, the 2,400-frame spill on Windows (a TemporaryFile in the output folder).

**For the runtime (review B-1).** 0.2 files set header byte 27 bit 2, HF_SEGMENT_RANGE (0x04): lo/hi are already one
fixed range per shot segment, so the runtime must not re-normalise them per shot. The 2c4bd03 runtime's shot_norm (p10
of lo / p90 of hi per FF_CUT run, short shots folded into the one before) would map a short shot (its own VDA chain,
its own raw scale) through the previous shot's range, and squash or clip a shot whose dissolve split it into two
ranges. fmv-relief 9812b55 checks the bit and uses a = 1, b = 0 for such files; files without it (0.1) keep the
re-normalisation. Both must land together: reconverting with 0.2 while playing an older build shows both artefacts.
FF_FLASH frames' depth is interpolated from their neighbours. The temporal filter makes the runtime EMA largely
redundant (A/B it at 0.3 or off).

Known trade-offs (review): a 1-4 frame cutaway A -> B -> A counts as a flash, so B shows A's depth (N-9); the edge
path runs inside each model window, before the 8-frame cross-fade between windows, so where two windows put an edge a
pixel apart the cross-fade makes in-between values again for those 8 frames (N-4: small, and the temporal filter's
depth weight keeps most of it; worth knowing when judging edge width on real output). The "same picture, brighter"
rule (a detector cut that is no cut) now needs a correlation >= 0.98 and an affine fit to 10 % of the step: two
different sky-over-ground shots with a brightness change (r 0.95, a quarter of the step left over) stay a cut (S-2).
Blank-only segments are written as a constant 0; the temporal filter gives no weight across a dissolve / dip segment
start; leftover spill files older than 3 h are deleted at the start of a run.

### Optional high-quality mode: DA3MONO-LARGE detail on VDA-S (design only, not implemented)

A plan until it can be tried on the owner's PC (fmv3d-models.md, "Optional higher quality").

- **Licences** (from the LICENSE files and model cards, read 5 Nov 2026): Depth Anything 3 code is Apache-2.0
  (github.com/ByteDance-Seed/Depth-Anything-3, LICENSE and pyproject). Its weights differ per model: **DA3MONO-LARGE
  is Apache-2.0** (README model-card table; the Hugging Face page is tagged apache-2.0), as are DA3-SMALL, DA3-BASE and
  DA3METRIC-LARGE; DA3-LARGE, DA3-GIANT and DA3NESTED-GIANT-LARGE (and their -1.1) are CC BY-NC 4.0 and must never be
  offered. VDA-Small stays Apache-2.0 (VDA-Base/Large: CC-BY-NC-4.0, never offered).
- **Why**: DA3MONO-LARGE (ViT-L + DPT, per frame, relative depth plus a sky head) is the strongest permissive
  per-frame model found: Sintel (CG) delta1 82.3 vs DA2-L 77.2 (DA3 paper, Table 10). Per frame, so it must not decide
  the low frequencies (shot-to-shot scale drift, flicker): VDA-S does.
- **Fusion** (per window, inside DepthAtOutput, before the edge path): V_t = VDA-S at 518 as now (cut-aware windows);
  D_t = DA3MONO-L per frame at 924x518 (or larger), fp16, optionally with flip averaging, inverted to disparity (sky
  -> 0, the far end), resized to V's grid. Per frame a robust affine fit a_t D_t + b_t ~ V_t (trimmed least squares at
  320x180, edge pixels masked), a_t / b_t smoothed zero-phase within the window. Output = lowpass(V_t) +
  (a_t D_t + b_t - lowpass(a_t D_t + b_t)), the low pass a Gaussian of ~1/20 of the frame width: coarse from VDA,
  fine from DA3. Then the 0.2 pipeline unchanged (edge path, per-segment range, temporal filter: the latter also
  damps what flicker the fine band brings).
- **Cost** (estimates, fmv3d-models.md): ~6-8 frames/s at 518 with flip and VDA-S, ~3-4 h for 82k frames; at 728
  short side ~6-7.5 h; ~4-6 GB VRAM (the two models run one after the other).
- **Integration**: the launcher downloads DA3's code at a pinned commit (3d835ec was HEAD when read) and only the
  model files mono depth needs (dinov2, dpt, the da3 model, cfg and configs/da3mono-large.yaml; not api.py, which
  pulls open3d / trimesh / pycolmap), checked by SHA-256 like VDA's; the weights (model.safetensors, ~1.4 GB) from
  huggingface.co/depth-anything/DA3MONO-LARGE at a pinned revision with their SHA-256 recorded. xformers stays optional
  (SDPA). A `-Quality hq` switch; the fingerprint names both models and both weights' hashes.
- **Exit test**: an A/B on SIN_us / BTS_us with `-Preview` against the 0.2 default (fine-detail flicker; if it
  flickers, lower the fine band's weight), then the owner's eye in the headset.

## FMV sync probe (S2, 4 Nov 2026)

Branch `fmv-sync`. Stage S2 of the FMV depth plan: a read-only probe that knows which video plays and which of its frames
is on screen. Diagnostics only: no rendering change, no export change (ABI 40). Native: src/native/ffxvr_fmv.h (pure: the
fields, the tracker, the lines, the .fvd header check) and `fmv_frame` in ffxvr_native.cpp; managed: src/managed/fmv_sync.cs
(F8's line). Notes and test results: scratchpad fixes/fmv-sync-impl.md.

- **The fields** (re-verified in our exe's disassembly, VA with the image at 0x400000; ffxvr_fmv.h lists every writer):
  manager `[0xCDED3C]` (0x750 bytes, `push 0x750` at 0x6D715C; made once at init, freed only at shutdown); **playing =
  byte +0x6D0** (1 at the end of start 0x6D9159, and at 0x6DAB51 in 0x6DA9B0, an open-by-path with no name or id and no
  reference found: probably dead; 0 in the constructor 0x6D6A89, at the natural end 0x6D7884, at a skip 0x6D9559, in stop
  0x6D71C7, at load 0x6D9D83, and at 0x6DAC40 from the event command 0x76EA60, which clears playing alone: "stopped before
  its end"; the update 0x6D7680 uploads only while it is set, 0x6D77A2), +0x6D2 = open (the update and draw run only with it; 0 in close 0x6D91A3);
  name `char[0x80]` +0x4F0 (emptied at load, filled by `snprintf(.., 0x80, "%s", ..)` at 0x6D943E; not cleared at stop),
  id +0x6DC, frame number +0x6D8 (0xFFFE = ended), paused +0x6FC (the toggle 0x6DA4B0, and the update itself at 0x6D785D
  when +0x6FD is set and the skippable flag is 0, so "paused on P" may count frames the player did not pause),
  render-thread upload +0x570,
  skip prompt +0x74C, skip allowed +0x74D (0 for the 16 ids of the table 0xB49B08: 2 4 6 18 24 32 38 42 44 46 50 60 62
  70 72 84). The player is embedded at +0x38: its duration in ms at player+0xF8 (from the WebM's ns, 0xA28148), the
  uploaded count at player+0x100 (0 at open 0xA27F4B, `inc` after the Y/U/V upload 0xA28826, no other writer), ms per frame
  at player+0x108, the size at +0x14C/+0x150. New: **byte 0xCDED31 = the user skipped** (1 only at the skip 0x6D9548, 0 at
  every load 0x6D9D27); the game's frame number is `ftol(float(trunc((count + 1) * ms_per_frame)) * 29.97 * 0.001)`, which
  equals the count for every count 0..30,000 at 30000/1001 fps (checked by emulating the exact arithmetic), so the frame on
  screen is index count - 1 = frame number - 1. 0x76EFC0 returns 1 for event movie ids 0x20/0x2E/0x46/0x48 (= 32, 46, 70,
  72, the hybrid candidates; ten callers).
- **Cost.** Nothing in the Release logging profile (`diag_profile_on()` false: no read, no line; F8's line still works,
  it is managed's own). Otherwise every Present: the manager pointer (exe image) and its playing word (one 4-byte guarded
  read of the heap). Only while the playing byte is set or an FMV is being tracked (not on the skippable flag 0x112A008
  alone: it lingers after a movie): three guarded reads of the manager (0x80 + 0x5C + 0x84 bytes) and three small exe
  reads. No file I/O on the render thread: at a logged start a worker thread (one request / one result slot, the PDH
  thread's pattern, stopped at shutdown) checks the depth file: `GetFileAttributesW` first (an offline / recall-on-open /
  recall-on-data-access file is not opened: "not checked (cloud-only)"), then `CreateFileW` with `FILE_SHARE_READ |
  FILE_SHARE_DELETE`, one 128-byte `ReadFile`, closed at once. The lines are kept and written with `diag_try` (retried on
  later Presents when the log is busy).
- **The lines** (ffxvr_diag.log; at most 240 a session, a start keeps room for its end; a restart of the same video within
  1 s is not logged and is counted on the next start line):
  `fmv: start '<name>' id N (event movie id, skippable flag, skip allowed, render-thread / inline upload), view <v> | video
  WxH, ms a frame (fps), duration ms (frames at 29.97) | count at start | depth file: checking... (fmvdepth\<file>)`, then
  `fmv: depth file for '<name>' id N: present (fmvdepth\<file>: N frames WxH, model) / absent / not checked (cloud-only) /
  invalid (why) (checked off the render thread in ms)`.
  `fmv: end '<name>' id N: ended / SKIPPED by the user / stopped before its end / replaced / the movie player went away |
  s, Presents (per view), a 3D camera on K, paused on P, skip prompt on Q | uploaded a..b (changes seen, largest step,
  frames never presented), first upload after ms, longest gap ms | s from the first to the last upload vs s at 29.97
  (uploads/s) | frame number a..b (largest step, back), number - count range, at the end | expected: the game's duration
  ms = frames, the file N frames = s | final count EQUALS the file's frame count / SHORT by k / OVER by k` (without a file:
  the final count against the game's duration).
- **F8** (Debug header): `FMV: <name> id N frame F/total uploaded U, mode screen/3D, depth file: present (N frames) /
  absent`; total is the file's frame count, or `~` the game's duration at 29.97. Idle: `FMV: none playing (last ...)`.
  Managed's own guarded reads, only while the Debug header is drawn; the depth file only while a movie plays, on a pool
  thread (the same cloud-only rule), at most every 5 s per name, "checking..." until the first answer; the text is rebuilt
  only when something in it changed.
- **Exit criteria (S2)**: for OPN_us and END_us the end line says "final count EQUALS the file's frame count" (with the
  .fvd present; without it, the count against the game's duration, `+0` expected); a skipped FMV ends "SKIPPED by the
  user" with a count short of the file; for the hybrid ids the end line's views and "a 3D camera on K" say whether the
  game draws a 3D camera while the movie plays (K close to the Presents: a movie backdrop behind 3D; the relief must then
  stay off). If the count ends short of the file on a natural end, the player stops at the duration before the last frame
  (the update's `(count + 1) * ms >= duration` test, 0x6D76D2..0x6D76F4) and S3 clamps to the last frame.
- **Verified here** (container): ffxvr_fmv.h's unit test (offsets on a synthetic 0x750-byte manager, the frame number
  formula, the file name rule, 23 .fvd files written by tools/fmvdepth/fvd.py incl. damaged copies with the same verdicts as
  fvd.py's FvdReader, the tracker on a simulated OPN_us at 72 Hz / a 20 Hz monitor / skip / stop / replace / gone / hybrid /
  paused, the limits, truncation; mutations caught); FmvSync's C# test (the same files and verdicts, the F8 text); the
  native syntax check and the managed compile check unchanged. **Not verified**: anything in game.

Owner test steps (ffxvr_diag.log, lines starting `fmv:`, with the Dev or Verbose logging profile: Release writes none; F8 >
Debug shows the FMV line live in any profile):
1. The opening FMV (start a new game): watch it to the end without skipping. Expect a `fmv: start
   'OPN_us.webm' ...` line and an `fmv: end ... ended ...` line; note "final count ..." and the uploads/s (about 29.97).
   With F8 open during it, the FMV line's frame should run up to the total.
2. Skip one FMV (one that shows the skip prompt: the game's own two presses, the prompt button and then the confirm;
   in the code pad bit 0x800 opens the prompt, 0x80 confirms, 0x6D94A9 / 0x6D94E3). Expect `SKIPPED by the
   user` and the count where you skipped. If no prompt ever comes, its id is in the no-skip table (the start line says
   "skip allowed 0").
3. If you know a scene where an FMV plays behind real-time characters (ids 32 MST_us, 46 FAR_us, 70 GGM_us, 72 SNS_jp:
   which story scenes these are is not known here), play it with the headset on: the end line's views ("3D N" / "screen N") and
   "a 3D camera on K" tell how it was shown. Say whether it looked right in the headset.
4. Send the `fmv:` lines (and, if you converted any videos, which .fvd files you have).

## FMV relief screen (S3, 4 Nov 2026)

Branch `fmv-relief` (on `fmv-sync` f3baec5). Stage S3 of the FMV depth plan: an FMV that has a converted depth file plays on
the virtual screen as real geometry in the headset. Off by default (`fmv_depth=0`) until tested in a headset. Native:
src/native/ffxvr_relief.h (pure: the .fvd reader, the frame selection, the hold / cut ramp, the shape, the matrices),
`relief_present` and the relief worker in ffxvr_native.cpp, `xr_render_views` in ffxvr_xr.cpp; managed: src/managed/fmv_relief.cs
(the settings and F8's text). ABI 41 (+ `ffxvr_fmv3d_set`, `ffxvr_fmv3d_text`); 42: the 3D movie mapping's settings; 43:
the edge band. Notes and test results: scratchpad
fixes/fmv-relief-impl.md; the review fixes/fmv-relief-review.md (SF-1, N1-N8: done, below).

- **When.** Every Present with the headset on, in xr_tick right before xr_frame, with `fmv_depth=1`: the movie player is read
  as the S2 probe reads it (the manager pointer and the playing word; the state, the player's fields and the name only
  while playing). The relief is drawn only when all of these hold, in this order (the first that fails is F8's reason):
  the state is readable; the id is not 32 / 46 / 70 / 72 (the movie-behind-3D ids); the frame goes to the mono virtual screen
  (not the stereo screen, not the 3D view, no battle transition window); no real 3D camera this frame; Fahrenheit's save
  screen is closed; the depth file opened and checked (below); eye 1 is our backbuffer copy (`g_bb_copy`, the flat screen's
  own source); the relief pass (shaders, the eye images, located views with a sane field of view) is ready. Otherwise the
  frame goes out exactly as before (the screen quad), and F8 says why. While the relief is drawn the screen quad and the UI
  panel are not submitted (the head-locked ImGui layer still is). The depth file is asked for only once the movie could
  show a relief (review N3): not a hybrid id and the mono virtual screen (re-checked every Present, so a view that settles a
  few frames in still gets it; a battle transition window does not hold it back): a movie behind 3D, the stereo screen or
  the 3D view never opens or decodes anything.
- **The file** (all I/O on the FMV worker, the S2 thread, extended). Once the movie qualifies the render thread posts a
  request (a generation number, the name, the frame count by the game's duration); the worker opens
  `fmvdepth\<name>.fvd` once: `GetFileAttributesW` first (cloud-only: not opened), `CreateFileW` with share read + delete,
  the header through `fmv::fvd_parse` (FORMAT.md checks 1-5), then this runtime's own: 320x180 or 240x180, codec 0 or 2
  (XPRESS_HUFF via cabinet.dll, loaded from System32 only (`LoadLibraryExW(..., LOAD_LIBRARY_SEARCH_SYSTEM32)`, review N5),
  `CreateDecompressor` / `Decompress` looked up at run time on the worker; zlib, the tests'
  codec, is refused), not the fake model, the source name equal to the game's (ignoring case), 1..65,536 frames, and the
  frame count within 2 of the game's duration (else "stale: convert it again"). The whole index is read in 8 KB chunks and
  every entry checked (checks 6 and 7) at open, kept as 16 B a frame (offset, size, stored / cut / flat), plus a cut bit map
  and a flat bit map (hi - lo below FORMAT.md's eps, or not finite) of 8 KB each. The handle stays open until the movie ends.
- **Decoding.** The render thread posts the depth frame on screen (the uploaded count - 1, `+ fmv_depth_offset`) when it
  changes; the worker keeps a ring of 8 decoded frames: that frame and the 7 after it (from frame 0 before the first
  upload), one per loop so a new request is seen at once (one `ReadFile`, one `Decompress`, check 8, the left predictor's
  inverse, the frame's median by histogram). A slot is written with its index at -2 (set under the lock); the render thread
  copies the wanted slot out under the lock only when it will upload it, then `UpdateSubresource`s the R8 depth texture.
  A new generation (or none: the movie ended) closes the file and frees the index and the ring.
- **Memory** (32-bit): per open video the index (16 B a frame: END_us 0.40 MB, at most 1 MB by the frame cap), the ring and
  the payload buffer (9 x 57.6 KB = 0.52 MB), the render thread's upload copy (57.6 KB); the two 8 KB bit maps are static.
  At most ~1.6 MB, END_us ~1 MB, all freed at the movie's end. Kept from the first relief frame until the headset goes
  off (a session restart included) or fmv_depth is turned off (review N4: no per-movie churn): the upload copy (57.6 KB of
  CPU memory) and the GPU objects: two eye images at the eye swapchain size in the backbuffer's format and a D32 depth
  buffer of that size (at 2496x1404: 2 x 14 MB + 14 MB = ~42 MB of VRAM), the 320x180 R8 texture (57.6 KB; re-made for a
  240-wide 4:3 file); no vertex or index buffer (the grid comes from SV_VertexID).
- **The relief** (k_rel_hlsl; relief::weight / map_point / edge_fill are its CPU copy, tested). Our two eye images are cleared black
  (the room colour) with a depth buffer; each eye draws 160 x 90 cells (120 x 90 for 4:3) x 2 triangles with
  MVP = P(that eye's field of view, below; near 0.05, far 50) * V(that eye's located pose) * M(the virtual screen's pose),
  in the screen quad's own frame (x right, y up, +z toward the viewer, W x W * 9/16 for the whole backbuffer, exactly as the
  flat screen shows it). The grid covers the video rect inside the backbuffer (fit and centred: 1280x720 / 640x480 by the
  player's own size into the backbuffer; a 4:3 video is pillarboxed, the bars stay black). The vertex shader samples the
  depth (bilinear: a 2x2 box at the 2-texel cell spacing) and places the vertex by the "3D movie" mapping (below; it
  replaced the first build's linear relief, z = s W (d - p) along the normal with s at most 0.25, which the owner found
  very subtle and noisy at its maximum), with the window rule at the edges (below). The pixel shader samples eye 1's
  backbuffer copy (subtitles and the skip prompt are in it and bend with the relief: the overlay
  split is S4). **The tight field of view** (review SF-1; `fmv_depth_tight_fov`, default 1): the eye images are 16:9 (the
  eye swapchains) while an eye's field of view is nearly square, so drawn over the whole view the video got only about 0.56
  of the horizontal density vertically (~360 rows for a 2.5 m screen at 2.5 m, against ~565 display rows that the quad
  reaches). Each eye's projection is therefore narrowed to the box of the video rect pushed in and popped out by this
  frame's relief (the frustum from the nominal eye through the rect's corners, cut at the nearest and farthest distance
  of this frame's mapping: its 8 corners projected into the eye's located view; a convex body's projection is bounded by its corners),
  widened by 3 % of the span plus 0.004, clamped to the eye's own field of view (relief::tight_fov); the eye is drawn and
  submitted with that field of view, so the same image covers the screen with all its rows. Outside it the compositor shows
  black, the room colour anyway. A corner behind the eye or a screen out of view keeps the eye's whole field of view. The
  figure (eye-image rows per video row, eye 0, tight and whole) is logged at each movie's first relief frame and shown on
  F8. A projection view whose field of view differs from the located one is legal OpenXR; how Virtual Desktop reprojects
  it is the A/B the toggle is for. xr_frame gets the two images, `xr_render_views`' poses and the field of view they were
  drawn with as a projection layer: the pose the frame was drawn with is the one it is submitted with (the views located at the last wait, an extra frame's included),
  so it is world-locked like the quad was; Home re-places the screen as before. Stickrep's extras re-send it unrotated
  (a screen frame records no stick heading).
- **The "3D movie" mapping** (owner test, 4 Nov 2026: "very subtle even at 0.25, and max strength accentuates noise").
  Video-Depth-Anything gives relative inverse depth (disparity) up to an unknown affine map, and the converter stores it
  normalised per shot: n = q / 255 between the shot's lo and hi (p2 / p98, smoothed within the shot; OPN_us frame 600:
  0.70 / 4.56). Choosing the affine map is the stereographer's job, as in a 3D film: n = 1 (the shot's nearest) goes to
  Zn = Ds - pop, n = 0 (its farthest) to Zf = Ds + range, linear in disparity in between, so depth is proportional to
  1 / disparity:
  `1/Z(n) = 1/Zf + n (1/Zn - 1/Zf)`, then the amount k (0..1 x the ramp level) blends in disparity toward the screen:
  `1/Z = 1/Ds + k D'` with `D = 1/Z(n) - 1/Ds` and `D' = D x (D > 0 ? w_edge : 1) x w_sub` (k = 0: the flat screen; the
  weights: the window rule below). Ds is the viewer's distance to
  the screen (`screen_distance`, 2.5 m by default: the screen is placed that far ahead of the reference). A pixel is moved
  along the ray from the nominal eye E through its own position on the screen. E is the midpoint between the eyes at the
  recentre (review3 N-1): the screen is placed Ds ahead of the left eye (the reference), so in the screen's frame E =
  (ipd / 2, 0, Ds), ipd the located eye separation (63 mm when out of 45..85 mm); the parallax is then split evenly between
  the two eyes (with E at the left eye the right eye carried all of it, twice as wide). t = Z / Ds,
  `P = E + t (S - E) = (ex + t (x - ex), t y, Ds (1 - t))`. Seen from E the picture is exactly the flat
  screen's (no pixel moves, no hole opens); far content is drawn larger in proportion to its distance (at 5.5 m, 2.2 x),
  so it keeps its angular size, as in a cinema. The two eyes' parallax gives the depth; looking from off E (head motion)
  shows the parallax a real scene would. Defaults (fmv_depth_amount 1, fmv_depth_range 3 m, fmv_depth_pop 0.15 m), a
  2.5 m screen at 2.5 m, eyes 63 mm apart, angular disparity against the screen plane `eta = ipd (1/Z - 1/Ds)`:

  | | distance | disparity | |
  |---|---|---|---|
  | the nearest (pop 0.15 m) | 2.35 m | +0.09 deg (crossed) | slider end 0.5 m: 2.0 m, +0.36 deg |
  | the middle (n = 0.5) | 3.29 m | -0.35 deg | |
  | the farthest (range 3 m) | 5.5 m | -0.79 deg (uncrossed) | slider end 10 m: 12.5 m, -1.16 deg; infinity would be -1.44 deg |
  | the first build at s = 0.25 | 2.34 .. 2.81 m | +0.10 .. -0.16 deg (at p = 0.5: 0.31 m in, 0.16 m out) | |

  So the default spans 0.88 deg of disparity (3.15 m of depth) against the old maximum's 0.26 deg (0.47 m), and it does so
  without scaling the noise along with it: the noise filters below act on the map first. Comfort: the usual cinema rule keeps crossed (pop-out) disparity within about 1 deg; here it is 0.09
  deg (0.36 deg at the slider's end) at 2.5 m. **At every screen distance** (review3 SF-2: at screen_distance 1 m the
  sliders reached +3.6 deg crossed and the defaults -2.7 deg uncrossed) make_map caps the pop-out so the nearest content's
  crossed disparity is at most 1.0 deg (and the nearest at least 0.5 m away), and the range so the farthest's uncrossed
  disparity is at most 1.5 deg (63 mm): Zn >= 1 / (1/Ds + 0.277), Zf <= 1 / (1/Ds - 0.416) when 1/Ds > 0.416. At Ds >= 2.41 m
  even infinity is within 1.5 deg, so at the default 2.5 m neither cap binds for any slider value; at 1 m the defaults' range
  is capped to 0.71 m and the pop-out to at most 0.22 m (the default 0.15 m stays). The uncrossed figure is looser than the crossed one because behind the screen
  nothing diverges in a headset (below) and 1-1.5 deg of uncrossed disparity is within the usual 3D-film budgets; it bounds
  the depth spread (nearest to farthest at most 2.5 deg) at close screens. F8's row and the Debug line say when either
  is limited ("pop-out limited to 0.22 m: 1 deg at this screen distance", "(limited: 1.5 deg)"), as does the fmv3d: line. Behind the
  screen there is no divergence in a headset: the content is real geometry at 2.5 .. 12.5 m, so the eyes converge on it as
  on any scene (unlike a cinema, where screen parallax beyond the eye separation diverges); the headset's fixed focus
  (about 1.3-2 m on a Quest 3) is the same for the flat screen. Per shot the nearest content always lands at the pop-out
  plane (the floating window of 3D films). The range and pop-out follow their sliders at 2 m/s and 0.5 m/s, the amount at
  0.5 a second.
- **The window rule at the edges** (owner test round 3: "the top edge makes a 'shelf' the whole time"). The first mapping
  build feathered the whole offset to the screen plane over the outer 4 %; at the top of most shots the content is far
  (sky, a stadium roof: 3 m behind), so the feather stood a steep wall from 3 m back up to the screen plane along the top
  edge, in every shot; at the bottom the content is usually near the screen, so nothing showed there. Content behind the
  screen needs no feather: on its ray from the nominal eye it stays inside the pyramid through the screen's outline, as
  when looking through a window, and the black around the frame is the window's frame. Only content in front of the
  screen at the border breaks the window (the floating-window problem of 3D films). So the edge weight w_edge
  (smoothstep from 0 at the border to 1 at `fmv_depth_edge_band` in, default 0.04 of the rect, Debug slider 0..0.15) scales
  only a positive D (in front); behind the screen nothing is feathered, up to the border. D' is continuous (D x w and D
  agree at D = 0). With `fmv_depth_flat_subs` the subtitle weight w_sub (0 in the bottom 15 %, ramping from 1 at 80 %)
  scales all of D: the bottom band lies on the screen plane, readable; at its ramp the far content comes forward over 5 %
  of the height, which is that option's price. Tested: every vertex (a 41 x 41 grid, 9 disparities, pop-out 0 / 0.15 /
  0.5 m, range 0.5 / 3 / 10 m, the band on and off) stays inside the pyramid from E through the rect and never nearer than
  the pop-out plane; at the border nothing is in front of the screen and the far content keeps its full depth; the band
  puts everything on the screen plane; no step larger than 1 cm along the outer 10 % in steps of 0.005 %, none through
  the screen plane; and the tight field of view still bounds every vertex from an eye 3.2 cm off.
- **Noise.** (a) The worker runs a 3x3 median on every decoded frame (fmv_depth_median, default 1): it removes the
  model's speckle and keeps depth edges (a 19-exchange network, ~57,600 pixels, about 1 ms on the worker). (b) At each
  upload, in display order, a per-pixel temporal filter (relief::ema, 8.8 fixed point): the depth follows (1 - smooth) of
  the gap per video frame (fmv_depth_smooth, default 0.6: 40 %), but a change of more than 24 of 255 is taken at once
  (motion, not noise: no ghost behind a moving edge); reset at every cut and after a flat stretch. 115 KB with the upload
  copy. (c) The rubber sheet and the edge fill (**Debug, off by default since round 6**, ini `fmv_depth_edge_fill`; the old
  key `fmv_depth_edges`, saved as 1 by every earlier build, is ignored). A triangle across a depth edge stretches from the
  near to the far side; from an eye a few centimetres off E it shows a smear. The first fill (rounds 3-5) took each
  triangle's slope from ddx / ddy (constant over the triangle) and recoloured the whole triangle from two cells toward the
  far side. It first ran in the owner's build of 636c7f7 (review3 B-1 had left its constants unbound) and drew "a tetris
  game" around faces: seen from E every triangle covers exactly its own cell of the picture (each vertex is on its
  pixel's ray), so the fill repainted the picture itself, cell-sized triangle by triangle, switching on and off between
  neighbours and biting into the face. Reproduced in relief_test (test_fill_blocks: his settings, a face disc on a far
  background, the worker's median): 235 triangles filled over half, 163 of them touching the face, 294 hard steps between
  neighbouring triangles; noise was not the cause. The rework (relief::fill_vertex, the VS): per vertex, the slope of 1/Z
  as a central difference over the four neighbours one cell away (1/m per m: the same at any range; thresholds 0.369 /
  1.153 with a smoothstep ramp, the round-5 one-cell 0.738 / 2.305 halved for the two-cell difference: at range 3 m a
  step of about 12 levels starts it, one level never does), gated by where the vertex lies in its neighbourhood's depth
  (0 at the near side's own depth: the near object is never recoloured; 1 on the far side), and a sample offset toward the
  far side of up to two cells (shrinking with the slope below the ramp; cells now from the video rect, so 4:3 is right too,
  review3 N-2). Both are vertex outputs, interpolated: the weight is continuous across every triangle edge by construction,
  and the pixel shader only lerps. Same test: 104 vertices over half, 0 face vertices over 0.25, 0 at the face's own depth.
  It still alters the picture seen from E in the cell beside a silhouette (the background there is drawn from up to two
  cells further out), which is why it stays an experiment for an A/B in the headset (review5 SF-1 measured it eroding the
  near object's outer ~1.5 cells by up to 0.64). **Planned replacement (not built):** a per-pixel gate on the stretch as
  this eye sees it (from the uv's ddx / ddy in that eye, so exactly 1 from E: the picture from E is never touched) with a
  far-side sample from a blurred mip (research fmv3d-projects.md T4). (d) Debug, session-
  only: `depth as colour` (red near, green middle, blue far) to judge the depth itself in the headset.
- **Would converter changes help?** (not done; estimates)
  - Per-shot constant lo / hi (a second pass over each shot, ~35 MB of float16 maps for a 300-frame shot): with the depth
    now in metres, any drift of the causal lo / hi EMA within a shot becomes the whole scene breathing in depth. Removes it
    entirely; the most valuable change, small cost (one shot buffered, no extra model time).
  - Zero-phase temporal smoothing within a shot (+-2..4 frames, edge / motion gated): about 2-3x lower frame-to-frame noise
    on static regions with no lag (the runtime filter's time constant is about 2.5 video frames at 0.6, for changes under its gate). Cheap at 320x180.
  - Joint-bilateral upsampling guided by the video frame (edges of the depth snapped to colour edges): crisper silhouettes,
    smaller rubber sheets; moderate gain, a few ms a frame.
  - 480x270: 2.25x the storage (~3 GB for the US list) and decode, and only useful with a 240x135 grid; depth edges ~1.5x
    sharper (a cell would be 1 cm instead of 1.6 cm). Moderate gain, lower priority than the three above.
  - 16-bit storage: none visible. One 8-bit step is 0.0035 deg of disparity at the defaults (0.005 deg at 10 m), below
    stereo acuity (~0.006-0.01 deg) and far below a Quest 3 display pixel (~0.04 deg); the noise is the model's, not the
    quantisation's. Doubles the files.
- **Time** (relief::step, per Present). The depth changes only when the video frame does and is held in between (no
  interpolation). A depth level 0..1 (the strength is s x smoothstep(level)) moves over 150 ms both ways (review N1): up
  while the wanted frame's depth is there, down (on the last depth uploaded) while it is not: a decode lag past one frame,
  a flat frame in the file, a decode error, past the file's end. It is also capped to reach 0 at the file's last frame
  (cap = frames left x 33.4 ms / 150 ms), so a movie's natural end and a file a frame or two short fade out. Instant flat
  only where no depth can stand in: nothing uploaded yet, a new shot (a cut) not decoded yet, and the Presents that fall
  back to the flat screen (the frame source is not the relief's; review N2: the unreadable-player Present too), after which
  the next relief frame ramps in from 0. A cut (the index's flag; any cut in (shown, wanted]; a jump of more than 64 frames
  or backwards), and the first depth after a flat Present, resets the level to 0 and the pivot to that frame's median;
  otherwise the pivot follows the median by 15 % of the gap a video frame, at most 0.02. The strength setting is followed
  at 0.5 a second at most (the slider never pops). A frame not decoded in time gets the previous frame's depth once at full
  strength (one frame late, never across a cut), then fades: while fading the depth shown is up to ~5 video frames old, at a
  falling strength. A count that goes back with the same name and id while playing stays set (a loop; review N7) keeps the
  file open: the step sees the jump back as a cut.
- **F8.** Main window, View, after "2D screens": `FMVs in 3D (needs converted depth)` with `amount` (0..1) and
  `flatten the bottom 15% (subtitles)`; `depth range: the farthest, behind the screen` (0.5..10 m), `pop-out: the nearest, in
  front` (0..0.5 m), `smoothing (less shimmer, within a shot)` (0..0.95) and the disparity they give (`disparity +0.09 deg in
  front, -0.79 deg behind (comfortable within about 1)`); then a one-line note while a movie plays: `OPN_us.webm in 3D (depth ok)` (`(fading to
  flat (decode lag))` while fading) or
  `OPN_us.webm on the flat screen: no depth file (fmvdepth\opn_us.fvd)` (afterwards: `last movie OPN_us.webm: in 3D on N of
  M Presents` / `... flat screen: <reason>`). Debug header, after the FMV line: `FMV 3D: OPN_us.webm frame 1234/4750, depth ok,
  amount 1.00, 3.0 m behind / 0.15 m in front, 1.80 rows a video row (tight fov) | decode avg 0.31 / max 2.10 ms (1241 decoded, 0 errors),
  ring 8/8 | in 3D on 2900 of 2950 Presents` (or `, flat screen (<reason>)`; `FMV 3D: off (fmv_depth 0)`; `FMV 3D: no movie
  playing (last movie ...)`), the checkboxes `FMV 3D: depth as colour (red near, blue far)` (session-only), `edge fill`
  and `3x3 median` (ini fmv_depth_edge_fill (default 0) / fmv_depth_median), `edge band (pop-out only)` (ini fmv_depth_edge_band, 0.01..0.15 (review3 N-5: never 0),
  default 0.04), `FMV 3D: tight field of view (sharper; off = the eye's whole, A/B)` (ini
  `fmv_depth_tight_fov`) and `depth frame offset (sync test, not saved)` (-3..3: session-only, 0 at every start, not in the
  ini; review N6).
- **Log** (`fmv3d:`, diagnostic profiles only, 200 a session, through the S2 pending buffer): one line per movie at its end:
  `fmv3d: '<name>' id N: <how it ended> | <the file: frames, size, cuts, flat frames, codec / the reason it was refused> |
  P Presents: relief R (depth ok a, one frame late b, flat (decode lag) c, ...), flat screen F (<reason> k, ...) | depth
  uploads, ramps | decoded n frames, avg / max ms, errors; ring fill avg | opened in ms | video WxH in the backbuffer WxH:
  crop u, v | eye-image rows per video row (tight / whole) | fade-outs, loops | strength, flatten the bottom, tight field of
  view, offset` (now: `amount, range m behind / m in front of the screen at Ds (disparity + / - deg at 63 mm), smooth,
  flatten the bottom, tight field of view, edge fill, median, depth colour, offset`); and at each movie's first relief frame `fmv3d: '<name>' id N: first relief frame: eye image WxH, video WxH
  in the backbuffer WxH | eye 0: R eye-image rows per video row with the tight field of view (l r u d), R' with the eye's
  whole (l r u d)`. Also `fmv3d: relief pass ready / FAILED` once.
- **Cost.** fmv_depth 0: one branch. On, no movie: two guarded reads a Present. A movie: three more guarded reads, two
  short locks a Present (three when the frame changes), the 57.6 KB upload 30 times a second, two passes of 86,400 / 64,800 vertices
  and the eye images' fill; the shaders compile once at the first Present with fmv_depth on and the headset on, the GPU
  objects at the first relief frame (then kept).
- **Verified here** (container): ffxvr_relief.h's unit test (fixes/fmv-relief-impl.md: 1025 checks, 0 failed; round 5's
  set of 22 mutants, one at a time: 22 / 22 caught): the reader on 25 cases written by tools/fmvdepth/fvd.py with the zlib and stored codecs (16:9 / 4:3,
  stored frames inside a zlib file, the left predictor in both codecs; every frame equal to fvd.py's FvdReader and its
  median; damaged headers, entries and payloads; stale, other video, fake model, sizes, codec 2 with no decoder, the frame
  cap), the predictor, the selection, the hold / lag / cut / flat / error / strength-rate logic at 72 Hz, the shape and
  weight, the matrices (centre, edges, depth range, an asymmetric field of view, view * pose = I, a yawed eye), the video
  rect, the tight field of view (the exact bound of a centred screen, pop-out widening it, every point of the relief's box
  inside, the clamps both sides, the fallbacks, the rows figure), the fades (lag, flat frame, error, past the end, the end
  cap, recovery mid-fade, instant across a cut), the 3D movie mapping (the defaults' distances, linear in disparity, the
  amount, the ray from E, the pop-out cap, the comfort figures), every point of the mapped relief inside an eye's tight
  field of view, the window rule (above), the 3x3 median against a sort, the temporal filter (reset, the gate, rounding, convergence) and the
  shader's grid on the CPU; the HLSL compiled with dxc (vs_6_0 / ps_6_0; the game compiles vs_4_0 / ps_4_0
  with D3DCompile at run time); the native syntax check (warnings identical to the base) and the managed compile check (the
  same 11 pre-existing errors; a mutation in fmv_relief.cs is reported). **Not verified**: anything at run time (XPRESS
  through cabinet.dll, the worker's file and ring protocol, the D3D objects, the look, the comfort, the sync, the crop, the
  cost, Virtual Desktop with a narrowed projection view), and the shaders under fxc's SM4 rules.
- **Review 3** (fixes/fmv-relief-review3.md). B-1: the pixel shader's constants (edge fill, depth as colour, the screen
  size) were never bound: rel_draw now binds the relief's buffer to the PS too (uid_save / uid_restore keep the game's PS
  b0); a static check (scratchpad relief_chk/test/static_check.py: every stage that reads the cbuffer has it bound in
  rel_draw) fails on 35fad09 and passes now. SF-1, SF-2, N-1, N-5: above. N-3: the range / pop-out followers snap to the
  settings when a movie starts. Left as notes: N-2 (the fill's two-cell offset is 2.67 cells for 4:3 movies), N-4 (the
  median costs ~2 ms a frame on the worker), N-6 (the subtitle band's ramp is a steep step at large ranges, which the fill
  treats as an edge). The shaders compile as vs_4_0 / ps_4_0 with vkd3d-compiler 1.12 (built by the reviewer; not fxc).
- **Headset check (review N8): world-lock under head motion.** The relief goes out as a projection layer with the pose it
  was drawn with and no depth layer (no XrCompositionLayerDepthInfoKHR); between fresh frames VDXR can only reproject it by
  rotation, while the quad was recomposited exactly every display frame. At the game's Present rate during FMVs expect a
  little positional swim on head translation that the flat screen does not have (the 3D view already behaves so). Check it
  in step 5 below; if it shows, a depth layer for the relief is the next step.

- **Round 7** (runtime items 1-4 and 9 of the audit's ranked plan, research fmv3d-pipeline.md; no reconversion, ABI 43):
  - *A vertex per depth texel.* The 160 x 90 grid had its vertices on texel boundaries of the 320 x 180 map (each read
    the mean of 2 x 2 texels). Now a vertex at every texel centre plus one on each edge of the rect (relief::grid_coord,
    321 x 181 cells, 241 x 181 for 4:3; 348,606 vertices an eye, no buffers): the audit's silhouette transition goes from
    11.5-14 to 6.8-10.6 video px. The pass is timed (D3D11 timestamps, both eyes): F8's FMV 3D line and the fmv3d: end
    line give it (the audit expects well under 0.3 ms an eye).
  - *One fixed range per shot.* relief::shot_norm at open: per shot the 10th percentile of the index's lo and the 90th of
    its hi (flat frames left out; a "shot" under 6 frames, a flash, folded into the one before), and per frame n' = a n + b
    against them. So the subject no longer sinks back when something nearer enters (the audit's 0.25 deg / 6 px swings at
    the defaults) and nothing breathes. The worker now outputs 16-bit n' (the depth texture is R16_UNORM). Index entries
    are 16 B (a 32-bit offset), at most 32,768 frames. Debug: `each frame to its own range (old; A/B)`.
  - *Cuts.* A cut whose depth is ready switches at once at the level it was at (the old reset to flat and 150 ms inflate
    pulsed at every cut and at a flash that tripped the cut flag twice mid-shot); the 150 ms ramp stays for returns from
    flat, and a cut whose depth is not decoded yet is still flat at once. Debug: `old cut ramp (flat at every cut; A/B)`.
  - *A centred temporal filter on the worker* (relief::tfilter): each output frame is a depth-only temporal bilateral of
    the decoded frames -3 .. +3 (time sigma 3 x `smoothing`, depth sigma 7 levels; nothing across a cut, a flat or failed
    frame), on each frame's own per-shot n'. No lag (the EMA's was ~1.6 video frames), less flicker, no ghost under a gate.
    The worker keeps 12 decoded and 6 output frames (1.44 MB; with END_us's index 1.84 MB) on a tested schedule
    (relief::next_unit); ~2.4 ms a frame on the worker. Debug: `old causal filter (EMA, lags; A/B)`.
  - *Clean-ups:* the dead pivot and the per-frame median that fed it are gone; the colour's uv stays half a backbuffer texel
    inside the rect (no dark rim on 4:3 videos).
  - *Tools:* tools/fmvdepth/fvd_stats.py (read-only figures of the owner's files; setup.md has the command).
  - Checks: relief_test 941 / 0, 25 / 25 mutants (one at a time), nchk 14 / 2 / 0, vkd3d vs_4_0 / ps_4_0, the static
    check; fvd_stats' unit test. Not verified: anything at run time (the GPU time, the worker's cost in 32-bit MSVC, the look).
  - Review 7 (fixes/fmv-relief-review7.md, no blockers): SF-1, a cut onto a flat frame faded on the previous shot's depth
    over the new shot: a flat frame now fades only within its shot and is flat at once after a cut (the reviewer's repro is
    a test). N-1: the Debug causal filter starts over when it is switched on. N-5: past the file's end the worker no longer
    refills from frame 0. Left as notes: N-2 (the timestamp queries live for the process, like the shaders), N-3, N-4.
  - Converter v2 (fmv-conv2, review fmv-conv2-review.md B-1): a v2 file sets header flag bit 2 (HF_SEGMENT_RANGE, 0x04
    in byte 27): its lo / hi are already one fixed range per segment (the model restarted at every cut, dissolves split
    into segments). Then the runtime uses them as they are (a = 1, b = 0: no short-shot fold, no per-shot percentiles,
    which would map a 1-5 frame shot through another shot's range and squash or clip a dissolve's segments); the fmv3d:
    open line says "ranges per segment from the converter". Unflagged (v1) files are re-normalised as above.
  - Owner A/B (F8 > Debug, the FMV 3D line's checkboxes; all session-only): `each frame to its own range` on and off in a
    shot where something enters or leaves (a hand, a sword, a creature): with it off the rest should stay put; `old cut
    ramp`: watch shot changes for the "inflate" pulse; `old causal filter`: depth trailing moving edges, shimmer on still
    backgrounds; `smoothing` 0 / 0.6 / 0.9 with the centred filter (0.9 should not lag); silhouettes against the round-6
    build (finer depth edges, less halo).
- Round 10 (6 Nov 2026), the audit's item 5: sharp silhouettes. The rubber sheet is the depth transition across a
  silhouette (the mixed texels the model's softness and the area downscale leave); three parts fix it where the picture
  has an edge to follow:
  - *The edge snap* (worker, after the temporal filter, relief::snap_out): each texel on a depth edge goes to the nearer of
    its 5 x 5 min and max, ties to the far side. On an edge: 3 x 3 range over 12 levels and 5 x 5 range at most 15/8 of
    it, so a soft ramp of up to ~3 texels becomes a step while a linear slope (ratio 2) stays; the texel or two next to a
    steep kink move up to 1.5 steps; the outer 2 texels are never snapped. It never grows the near side (converter v2's 2
    model px growth is kept as it is: over 32 sub-texel positions the 50 % point moves by 0 on average, at most half a
    texel). The worker keeps the snapped copy beside each output frame; both are uploaded (R16 each).
  - *The guided upsample* (GPU, at each depth upload and the Present after, k_jbu_hlsl): a guide pass (each depth texel's
    footprint luma from the eye-1 backbuffer copy) and the upsample to 640 x 360 (480 x 360 for 4:3, R16): for each output
    texel the 5 x 5 snapped texels split into a near and a far layer at the middle of their range; each layer's depth
    (spatially weighted, sigma 1 texel) is taken by how close the output's own luma (2 x 2 video px) is to the layer's mean
    luma (sigma 0.05). A plain per-texel joint bilateral (the audit's) leaves v2's grown near side, a near texel showing
    the background's colour, in the background: 3.5-5 px of halo at contrast 1-0.3; the layer means keep it at 1.0-1.2.
    Where the 5 x 5 depth range is under 0.05 or the layers' lumas differ by less than 0.05-0.15 it fades to the original
    map, bilinear: there is no edge to follow, and the snapped step would crawl by a texel (4 px) as edges move.
  - *The fine grid:* a vertex per output texel (641 x 361 cells, 232k vertices an eye) as indexed triangle strips (one
    strip a row, cut by 0xFFFFFFFF, R32 indices, 1.86 MB); the VS takes SV_VertexID (the index) apart. Same diagonal as
    round 7's list.
  - Measured on the audit's cross-section (1280-px video, edge swept over 32 sub-px positions; 10-90 % width / crawl p-p in
    video px), round 7 -> round 10: v1 model softness 1.0: 6.8 / 0.9 -> 1.6 / 1.9; softness 2.5: 10.6 / 0.6 -> 1.7 / 2.0;
    2.5 at contrast 0.3 + noise: 10.6 -> 1.9; v2-like (sharp, near side +2 model px): 6.2, halo 6.1 -> 1.6, halo 1.0, crawl
    1.6; v2-like at contrast 0.3: 6.2 -> 1.9 (halo 1.2); contrast 0.2: 6.2 -> 3.9; contrast 0.1 (the fade): 10.6 / 0.6 ->
    8.3 / 3.0; no colour edge: as round 7 (10.9 / 0.6, v2 6.7 / 0.6). The crawl at a contrasty edge, 1.5-2 px, is the
    640 grid's own (2 video px a cell; the reviewer's 1.65 px on v2 is not reduced, only kept): a 1280 grid would halve it.
  - Cost (not measured here; F8's FMV 3D line and the fmv3d: end line time both): the upsample ~77 fetches an output texel
    (230k), estimated ~0.05 ms a run, 2 runs a depth frame; the mesh 232k vertices and 463k triangles an eye. The worker's
    snap is two separable min / max passes over 57.6k texels.
  - Debug `guided upsample (A/B; off = round 7's 320 x 180 grid)` (session-only, flag 256): off draws exactly round 7's
    grid on the unsnapped map (the worker then skips the snap). On by default. Until a snapped frame is up (just switched
    on) round 7's grid draws.
  - The edge fill stays off: with 2-px cells the stretched triangles at a silhouette are the disocclusion itself, at most
    ~2 px an eye at the owner's settings; the fill is not needed for the rubber sheet any more.
  - Review 10 (fixes/fmv-relief-review10.md, no blockers): a flat relief (k 0: a ramp's first Present, a fade's tail, a
    not-ready cut, amount 0) draws round 7's list, not the fine mesh (same picture; the upsample still runs at uploads);
    the fmv3d: end line counts the Presents on the fine grid. Tests now run the flat-window guard (0 / 0 -> NaN -> the far
    plane on an R16_UNORM target) through the shader's text and pin the edge-range fade (0 at 0.05, 1/2 at 0.075, 1 from
    0.1). Left as notes: a failed constant-buffer Map in the upsample (removed device only) keeps the previous output for
    that Present; the worker's snap scratch (460 KB) stays allocated for the process; with the Debug causal filter the
    snapped copy is unfiltered; k_jbu_hlsl's one-time fxc compile time is in the "relief pass ready" diag line.
- Round 10b (6 Nov 2026), the owner's first look (v2 files, amount 1, range 4.7 m, pop 0.2 m): the guided grid "streaks and
  distorts"; round 7's grid has "a blocky appearance around edges". So the grid is a Debug choice, round 7's by default:
  - `320 grid (round 7)` (default), `640 grid, smooth depth` (the fine grid on the plain depth map, bilinear at each vertex:
    no snap, no luma pick), `640 grid, guided (round 10)`; and `edge softening (depth texels)` 0-1.5 (default 0): the vertex
    shader's depth fetch averages 4 bilinear taps at +- half that (relief::depth_at), in every mode. On the 640 grid
    anything up to 0.5 changes nothing (its vertices sit a quarter texel from the texel centres, where bilinear is
    linear); 1.0 is the useful setting. All session-only (ffxvr_fmv3d_set flags bits 8-9 and 10-13: no ABI change).
  - Measured (relief_test's test_grid_modes: a v2-like silhouette at 20 deg, near side grown 2.8 px, texels binary as the
    worst case of v2's sharpening / area means; staircase = the 50 % line's wobble about the true edge, rms; 10-90 width;
    stretch = how much each eye's image of the transition is stretched by the disocclusion, which at the owner's settings
    is 7.2 video px an eye for a 0.2 -> 0.8 step, 11.9 for the full range):
    | mode | staircase rms (binary / area) | 10-90 px | stretch an eye | fg flips on texture |
    |---|---|---|---|---|
    | 320 grid | 1.22 / 0.43 | 4.4 / 6.1 | x2.6 / x2.2 | 0 |
    | 320 grid, softening 1.0 | 0.52 / 0.13 | 9.7 / 10.1 | x1.7 | 0 |
    | 640 smooth | 0.87 / 0.25 | 5.2 / 6.3 | x2.4 / x2.1 | 0 |
    | 640 smooth, softening 1.0 | 0.51 / 0.02 | 7.8 / 8.5 | x1.9 / x1.8 | 0 |
    | 640 guided | 0.51 | 2.4 | x4.0 | 25 % |
    The guided grid is the only one that undoes v2's 2.8 px growth (bias +0.4 against +2.8), but it pays with the stretch
    and the flips. Ranked causes of its streaks (round 10 notes): (1) the disocclusion concentrated in one 2-px cell: each
    eye stretches ~2.4 px of picture over ~9.6 px, a horizontal smear of one or two source pixels per row (with the
    triangle diagonal, a comb) along every silhouette, where the old ramp spread it into a blur; (2) the per-pixel layer
    pick on textured foregrounds (hair, cloth, foliage) whose dark or light details resemble the background: 25 % of the
    foreground vertices within 12 px of the edge flip to the far side at +-0.2 texture, spikes the mesh tears around;
    (3) no temporal filtering after the pick: compression noise changes the flips every frame (shimmer); (4) in fast
    motion the depth frame and the picture disagree beyond the pass's +-2 texels; (5) the snap's plateaus (measured
    harmless on slopes: the pass falls back to the bilinear map there); (6) the tight field of view: not a cause (the
    guide reads the game's backbuffer copy, not the eye images).
- Round 10c (6 Nov 2026), the owner's settled choice ("looking quite impressive now"; v2 files, screen 5.2 m, amount 1,
  range 10 m = the old slider's maximum, pop 0.5 m = its maximum, smoothing 0.36, the 640 smooth grid, softening 1.0; the
  mesh 0.197 ms for both eyes):
  - The grid (`fmv_depth_grid`: 0 the 320 grid, 1 the 640 smooth grid, 2 guided) and `fmv_depth_soften` are ini settings
    in the 3D FMV section (F8 > View) next to range and pop-out; the guided grid only from F8 > Debug ("experimental:
    streaks at silhouettes"). New defaults: grid 1, softening 1.0, range 10 m, pop-out 0.5 m. An ini without the keys gets
    them; a saved range / pop-out is the user's own and is kept.
  - The sliders: range 0.5-20 m (was 10), pop-out 0-2 m (was 0.5). The caps (+1 deg crossed, -1.5 deg uncrossed, review3
    SF-2) keep every position comfortable at every screen distance (1-6 m; relief_test sweeps them in 1 cm / 5 cm steps).
    Disparity against the screen plane (63 mm):
    | screen | defaults (range 10, pop 0.5) | maxima (range 20, pop 2) |
    |---|---|---|
    | 1.0 m (the slider's minimum) | capped: range 0.71 m, pop 0.22 m: +1.00 / -1.50 deg | the same |
    | 2.0 m | pop +0.60 deg; range capped at 9.84 m: -1.50 deg | pop capped 0.71 m: +1.00; range 9.84 m: -1.50 |
    | 2.5 m (the default screen) | +0.36 / -1.16 deg | pop capped 1.02 m: +1.00 / -1.28 deg |
    | 5.2 m (the owner's) | +0.07 / -0.46 deg | +0.43 / -0.55 deg |
    | 6.0 m | +0.05 / -0.38 deg | +0.30 / -0.46 deg |
    At 2.5 m and beyond the far side never reaches the cap (infinity is 1.44 deg at 2.5 m, 0.69 deg at 5.2 m): the range
    slider's top only matters for a far screen, where it is still well inside. Pop-out is what a far screen lacks: at
    5.2 m the old 0.5 m maximum was 0.07 deg; 2 m gives 0.43 deg (the cap there is 3.07 m). The readout next to the sliders
    and F8 > Debug's FMV 3D line say "limited" whenever a cap binds. The range / pop followers are 4 m/s and 1 m/s (were 2
    and 0.5) so a slider move over the wider ranges still settles in a few seconds.

Owner test steps (the Sphere Theater in Luca replays FMVs; convert first: `tools\convert-fmv-depth.ps1 -Only OPN_us,SIN_us,BTS_us`
or the whole list; the Dev or Verbose logging profile for the `fmv3d:` lines):
1. F8 > View: tick `FMVs in 3D (needs converted depth)`. F8 > Debug shows `FMV 3D: no movie playing`.
2. In the Sphere Theater pick a converted FMV (OPN, SIN or BTS). Expect the note `<name> in 3D (depth ok)` and, in the
   headset, the screen in relief where it was (same size and place; black around it). An unconverted one says `on the flat
   screen: no depth file (...)` and looks as before.
3. A / B: untick the checkbox while the FMV plays: the flat screen returns at the next frame; tick it: the relief ramps back
   in over 150 ms. Or set the amount to 0 (flat geometry) and back.
4. Depth: the defaults (amount 1, range 3 m, pop-out 0.15 m) should read as clearly 3D (the farthest content 3 m behind the
   screen). Try range 1.5 and 6 m, pop-out 0 and 0.3 m, and say what is comfortable over a whole movie. F8 > Debug `depth as
   colour` shows the depth itself (red near, blue far): say whether it follows the objects, and whether it shimmers;
   compare `smoothing` 0, 0.6 and 0.9, and `3x3 median` and `edge fill (experimental)` off and on (it is off by default; look at silhouettes from a
   little to the side). With subtitles on, try `flatten the bottom 15% (subtitles)` (the bottom 15 % lies flat).
   The top edge (round 3's "shelf"): far content should now go back right up to the frame's edge, like a window; say
   whether anything near the edges looks cut or doubled, and try `edge band (pop-out only)` 0 and 0.1 in F8 > Debug.
5. Sharpness and world-lock: compare the relief at amount 0 with the flat screen (untick) for sharpness, subtitles
   included; then F8 > Debug `FMV 3D: tight field of view` off and on (expect the tight one sharper; say if Virtual Desktop
   shows black edges, a shimmer or a jump with it on). Move your head sideways and forward / back: say whether the relief
   swims against the room compared with the flat screen (review N8).
6. Watch for: the depth lagging or leading the picture (moving edges with a depth "ghost": try `depth frame offset` -1 / +1
   in F8 > Debug and say which looks right), a pop at shot changes, the frame's border bending, comfort at the default,
   and F8 > Debug's decode max and ring (expect ring 8/8, decode under a few ms). The end of a movie should fade to flat
   over its last ~5 frames rather than snap.
7. Send the `fmv3d:` and `fmv:` lines (the first-relief-frame line has the rows per video row), the range / pop-out /
   amount / smoothing you liked, and whether subtitles were on.

## Slow-GPU guard and overdrive freeze diagnostics (3 Nov 2026)

Branch `od-guard` (ABI 36). Research: overdrive-freeze.md (scratchpad). Revised after review (od-guard-review.md: B1-B3,
SF1-SF3, N1-N4, N6-N8) and review 2 (od-guard-review2.md: SF1, SF2, N1-N4).

- **What happened.** At Tidus's Slice & Dice (the timing gauge) the headset view fell to about 15 fps and never came
  back until the game was quit: the GPU span (the game's first clear to our Present hook, GPU timestamps) went from ~12
  to ~60 ms and xrEndFrame from ~0 to ~55 ms, on the flat virtual screen too (61 ms with 100 draws; a normal screen
  frame is ~1 ms with 400). The onset brackets the game loading `magic_0391.dll` (first seen in any log; probably the
  overdrive's effect program) and it stayed after the unload. None of our counters moved (twins, buffers, VRAM, eye-2
  re-issue CPU, MSAA targets, line types), the session stayed FOCUSED, and rumble is not the trigger (it started after
  the onset). A 10-26 case with the same signature (span x9 with the game paused, our work unchanged) ended at a session
  state change. Favoured (medium-low): something else occupies the GPU (Virtual Desktop's streamer / encoder, DWM, the
  driver), so our span and VDXR's xrEndFrame both stretch. Not excluded: a per-frame cost the overdrive latched inside
  our span that our counters do not see. The cause is undetermined; this round adds a guard and the data to tell.
- **The guard** (ffxvr_slow.h, the decisions, pure and unit-tested; ffxvr_native.cpp "slow-GPU guard", at every Present
  on the render thread). Slow = span > max(3 x the view's baseline, 33 ms) and xrEndFrame > 20 ms. Baselines, one for 3D
  (and the stereo screen) and one for the mono screen: the median of the measured spans while watching, from a
  0.5 ms-bin histogram that halves at 8192 frames, so it holds the last 4096-8191 measured frames (34-68 s at 120 Hz,
  45-90 s at 90 Hz, 4.5-9 min at 15 fps); learnt after 600 frames; no 25 ms cap (review B2), so a setup that always
  runs at 36 ms learns 36 and is never "slow" against itself. Review 2 SF2: nothing is learnt during an episode or the
  cool-down after it (a 5-min cool-down at 15 fps would otherwise learn the stuck state and silence the guard), and once
  learnt, spans over 2 x the baseline go to a shadow histogram instead; it replaces the baseline's only when such spans
  make up 75 % of every 10 s of watching for 10 min (a heavier map, new settings: a `slow: the 3D baseline is now ...`
  line). The exception is the cool-down after a restart that did not help: that state is learnt as it is, so the guard
  stops there. Each span is judged against the view of the frame it timed (5 frames earlier), and a span whose frame was
  excluded (review 2 N2: tracked per query slot) is neither measured nor learnt. Trigger: 120 slow frames (about 8 s at 15 fps), 6 normal
  frames tolerated inside the run. Without a learnt baseline for that view the run is only logged (one line, 10 min
  apart) and nothing is switched off. Not counted (the run restarts): a battle transition window, a load phase, the pick
  tool's slow motion, a session that is not FOCUSED (the Quest / VD menu throttles by design), and a Present gap over
  250 ms (a stall, a load hitch: 2 s of quiet after it). Then:
  1. a `slow:` line (span, its baseline and threshold, xrEndFrame, frame time, the canary, draws, battle_state, map), a
     frame census (ffxvr_frame_auto.log; 2 a session), the last 600 frames (1 a session, 3 with a diagnostic on), the PDH
     snapshot;
  2. after 1 s, each optional item that is on goes off for about 2.5 s (0.5 s settle + 2 s measured): `ui_capture` (the UI
     pass split), `ui_depth` (the adaptive distance and the plate), `vr_hands`, `screen_mono` (the mono virtual screen,
     eye 2 not re-issued; the skip stops one Present before the forcing, so the first 3D frame after it has an eye 2). The
     ImGui layer is never an item (review SF1): F8 and Fahrenheit's save screen are on it. Only measured frames count
     (a span read, not excluded); a window needs 8 of them. One with fewer (all excluded: a menu, a load, a transition
     window) is repeated once and then skipped; it never counts as "no change" (review B1). An item helped when 75 % of
     the measured frames are fast (span <= max(2 x baseline, 25 ms) and xrEndFrame <= 10 ms); one that did not is back on
     at once. One that helped stays off 10 s, comes back on and the frames are checked 2.5 s: still fast = the cause had
     passed; slow again = it stays off for 5 min (F8 shows it). A re-check without 8 measured frames is repeated once and
     then ends the episode with the item on: latching needs a measured result (review B3);
  3. nothing helped: everything is back on and the slow state is measured in 3 s windows until it is 60 s old (review 2
     SF1: longer than a heavy summon or overdrive effect; a fast window on the way = "recovered ... by itself"). The
     session restarts only if at least 2 steps were measured, the last window is 75 % slow, and from a FOCUSED frame
     outside a transition window or load phase (waited for up to 5 s). The restart is the stuck-swapchain
     recovery's (xr_stop under InEye2, the flat state; managed's ffxvr_xr_set starts it again after 1 s, not counted as a
     failed start). At most one per 5 min and 3 a session (slow_guard_restart=0: never). Fast 1.5 s after the session is
     back = "recovered after the session restart (the cause may have ended by itself ...)"; else it gives up. Every refusal is one `slow: gave up ...` line.
  Every episode ends in a cool-down (30 s; 5 min after giving up); 6 episodes a session, then it only logs (one line a
  minute). The headset going off (or unticked while a restart is pending) or slow_guard=0 ends an episode with everything
  back on, latched items included. The effective settings are the user's values with the guard's items off (the setters
  and slow_apply), so managed's every-frame push cannot undo a step.
- **Diagnostics.** (a) every ffxvr_diag.log line starts with the local time `hh:mm:ss.mmm`, every line of a multi-line
  text too (core.log and the module log are UTC: compare minutes and seconds). (b) `dll:` lines for DLL loads / unloads
  after init (LdrRegisterDllNotification: the callback copies into a fixed 64-slot ring under the loader lock; the render
  thread logs): magic_*.dll up to 300 a session, others 20 (150 with a diagnostic key on). (c) a 600-frame ring (frame,
  QPC, frame ms, span with its frame's view, canary, xr wait / end, draws, eye-2 draws, view, battle_state, excluded),
  dumped at the trigger. (d) the census at the trigger; with a diagnostic key on, also a baseline census at the session's
  first live battle frame in the headset (battle_state 1 or 2). The first automatic census of a session starts
  ffxvr_frame_auto.log fresh (the previous one is ffxvr_frame_auto.prev.log); one automatic census can wait, and the
  baseline gives its place to the first shatter or a slow onset. (e) the GPU canary: two timestamps back to back at the
  Present hook's start, read with the span (perf: `canary avg max, over 2 ms N`; the slow: line has it over the run).
  Weak evidence (review SF3): with no flush between them another process shows up only if the scheduler switches exactly
  there. Over 2 ms suggests the GPU was busy with other work at that moment (another process, or this one's own paging
  if the driver split the buffer there); a low canary proves nothing. (f) with a diagnostic key on,
  `rt:` lines for render-target / depth textures (any sample count) the game creates in a battle or an episode: the first
  40 a session, then new size / format / samples / bind kinds (64); ours are left out by the caller's return address.
  (g) during an episode only, at its start and every 5 s, a PDH line with the top 5 processes by 3D and by VideoEncode
  engine use (`\GPU Engine(*)\Utilization Percentage`, summed per process; a thread of its own, pdh.dll loaded there).
- **Logging profiles.** tools\set-logging.ps1 -Profile Release turns every diagnostic key off and leaves slow_guard (not a
  diagnostic, default 1): release users get the guard with a quiet log (no baseline census, no rt: lines, 20 dll: lines
  besides magic_*.dll, one 600-frame dump; slow: lines only when it is slow).
- **F8** (Debug): the two settings and one line: the state, the baselines ("learning" until learnt), any item kept off,
  and the last trigger (time, view, span and baseline, xrEndFrame) with what fixed it.
- **Reading the next log.** The PDH line names the processes using the 3D and VideoEncode engines during the episode
  (another process taking the GPU = H1). An item fixed it (slow again with it back on) = our work (H2, that item). Fast
  after a restart suggests the runtime's state, but the cause may have ended by itself at the same time (the restart
  comes 60 s or more after the onset). A canary over 2 ms in many frames suggests H1; a low one is no evidence.
- **A restart that did not help** (review 2 Q3): no more restarts for the rest of the session; the steps and the lines go
  on, later episodes end "restarts are off for this session (a restart did not help)", and F8 says so. A map that is
  legitimately 3x heavier than the previous minute and over 33 ms for minutes therefore costs one restart a session at
  most (60 s in). A restart that did help keeps the usual limits (one per 5 min, 3 a session). The
  `dll:` and `rt:` lines around the onset show what the game loaded and created.
- **Not verified here** (container only): MSVC build, the PDH counter names on the owner's driver, the restart under
  VDXR mid-battle, whether the census frame's hitch is noticeable.

## NPCs vanishing in first-person cutscenes (4 Nov 2026)

- **What the owner saw.** In first person a nearby NPC sometimes vanished during an in-game cutscene: Auron in Luca
  (lc/lchb0500, his guardian scene), Yuna on lu/luca0600 (the laughing scene). In both the followed actor is re-picked (actor 0
  hidden by the game -> the event Tidus, c001 then c101), and the Auron scene ran at 30 fps.
- **The mechanism (research, code-supported; not proven from those logs).** The on-foot body hide hid any skinned draw within
  hide_radius (0.75) + 1.5 x step_hide of the hide centre, in every pass. The step margin had no cap: a scripted move under the
  50-unit warp rule, a re-pick of the followed actor (a jump counted as a step) and 30 fps logic (a full step on every call,
  so step_hide = two steps) all widened it to several units, and an NPC whose root was inside went whole. Riding already used
  a better rule (a draw nearer the mount's root than the rider's is the mount's).
- **The fix: nearest-root ownership on foot.** Every frame push_player sends native the followed actor's drawn root and a
  re-pick key (ffxvr_fp_self_set) and, in the field in first person on foot, the drawn roots (+0x200, render space) of the
  other live actors within 30 units, at most 32, nearest first (ffxvr_fp_others_set). Left out: the followed actor, actors
  the game script-hides (+0x180 & 0x13: not drawn) and unbuilt roots (about 0); nothing is sent while riding (the mount has
  its own rule). A draw is the player's only if it is inside the radius AND not both more than 0.75 from the player and
  nearer another root than his by more than 0.5 units (`ffxvr_fpown.h`; ties and the tolerance go to the player, so his own
  parts within 0.75 of his root are never refused, whatever stands next to him).
- **The player's side.** His parts sit on his drawn root, +0x200 = +0x0C + chr_offset (+0x6C). The hide centre g_fp_pos is
  that root minus chr_offset when the smooth anchor's post-join snapshot is used, else the 30 Hz +0x0C: chr_offset moves the
  centre off his parts, never the root (review N5). So with the snapshot (root_ok 2, what this frame draws) the player's
  distance is to his drawn root alone (review N2: a large chr_offset would otherwise make a "phantom" centre that hides an NPC
  standing by it). Without it (root_ok 1: +0x200 read at Present, the lerp the last frame drew or the canonical tick) it is the
  smaller of the distances to that root and to the centre, whichever is nearer what is drawn.
- **Never refused near him (review N3).** A draw within 0.75 of the player (his distance, above) is his, whatever stands next
  to it, so his body does not flash on while his root is known to within 0.75. With both his roots a tick off what is drawn
  (no snapshot: smooth_anchor 0, no join hook, a script matrix +0x10C, the re-pick frame, a stale snapshot) and running, a root
  under 0.5 + that tick from his parts still refuses them; all his parts share one translation, so his whole body (in body mode
  1 / the VR hands also the head) shows for that frame. The safer failure: in the headset a one-frame body flash means this.
- **The margin.** min(1.5 x step_hide, 4) on top of hide_radius (4 covers a 2.7-unit step; the smooth anchor needs about 0).
  A new re-pick key zeroes it, as ffxvr_ride_set's N4 does for a mount: the key is the table index << 16 | chr_id, so a model
  swap in the same slot (lc/lchb0500: actor 3 c001 -> c101) counts too (review N1). No roots sent this frame or the one before
  = the radius test alone, as before. Riding is unchanged.
- **Limits.** Other actors' roots are read at Present: the lerp the last frame drew or the canonical tick, up to a step off
  (about 0.5 units at a walk; inside the tolerance). An NPC whose root is read more than 0.5 units nearer the player's part
  than its own parts, or one whose parts are within 0.75 of the player, can still be taken for his (the base's radius always
  took those). An object attached to the player as an actor of its own is no longer hidden with him (none known on foot).
- **Cost (review N4).** Managed walks the table only in first person on foot with the body hide on (native ignores the roots
  elsewhere), reading chr_enabled and chr_name (live()'s rule), then +0x180 and +0x200 of the live ones: about 48 x 8 bytes
  plus a few hundred, not 48 whole actors.
- **Diagnostics.** `body hide:` (ffxvr_diag.log), first person on foot, at most once a second and 200 a session, only on
  frames with a draw refused as another actor's or a match more than 0.75 from the player (review SF1: not on the radius, which
  running alone takes over 1.5 on every frame and which spent both budgets before a cutscene): the matched draws grouped by place
  (count at distance from the centre), the radius with its margin and the two per-call distances, the other roots sent and the
  nearest, the player's drawn root's distance from the centre (snapshot or read at Present), and the draws refused. Every 10 s,
  when anything was refused: `body hide (10 s, first person on foot):` with the frames and draws kept visible and the frames
  with a radius over 1.5 (100 a session). F8 > Debug > First person / follow: `body hide: radius N, NPC parts kept visible
  last frame M`.
- **ABI 37:** + ffxvr_fp_self_set, ffxvr_fp_others_set.
- **Tests (container):** fpown_test (the header and the native glue extracted verbatim, against the base's fp_part_of): an NPC at
  1 unit with a big margin stays visible; the player's parts 0-0.5 from his root stay hidden (200,000 random cases), 0-0.75 from
  the snapshot root (200,000); riding identical to the base (200,000); no others or stale ones = the base; an NPC exactly
  between; stale roots off by a step; the 30 fps margin, its cap and the re-pick reset; N1 (a model swap in the same slot), N2
  (the phantom centre), N3 (and its documented limit). sf1_test drives the extracted diagnostics: ten minutes of running write
  no line; refusals write about one a second and the 10 s lines. Not verified: MSVC build, the headset.

### Shared scene anchors: ties resolved by model identity (branch `npc-anchor`, 4 Nov 2026)

- **What the owner saw.** mi/mihn0100, an in-engine cutscene on the Mi'ihen Highroad, first person following actor 0 'c101'
  (the event Tidus): Yuna, sitting on the ground, was invisible in first person only and came back with "hide the player's
  body" unticked; F8's "draws hidden" kept growing.
- **The logs.** The actor table (18:04:51Z): actor 0 c101 and actor 1 c102 both at (-188.0, -18.8, -176.0), the scene anchor;
  c003 and f051 far off. The motion lines: the drawn root = logic = the snapshot = (188, 18.8, -176) render space, "rendered
  ... d 0.0". No `body hide:` line at all (nothing refused, every match at 0 from the player: the line's triggers never fired).
  perf: hidden grew by 68,400 per 30 s = 38 draws a frame, steady. The censuses (12:04:26, :31, :32, :34) cannot list hidden
  draws: the body hide returns before the census's scene-draw line, so they show only as gaps in the frame draw numbers (25
  before the first visible character, 13 more among the blended draws in every census). They do show that every part of one
  character samples one PS slot-0 texture (Auron's 13 draws 28B1EDA4, an NPC's 5 draws 407A9924; nagi0000's Tidus 13 draws
  3DFB9564; the Luca NPCs each their own), and that the VS constant buffers and shaders are shared between models.
- **The mechanism.** Both event models stand on the anchor; their animations place the bodies through the bone palette. The
  skinned buffers' world translation (+640) is the root for every part of both, so every draw of either is 0 from the player
  and 0 from the other root: a tie, and ties go to the player (the safe side for his own body). Yuna's whole model was hidden.
  The first-person eye was right all along: it comes from the head sub-position (chr_optpos, built by the game from c101's own
  bones x root, 0x8350F0; the highest one is the head bone's origin), so it sits in Tidus's head, wherever his animation puts it.
- **The discriminator: model identity, by texture and head.** A matched draw tied with another actor's root (within the 0.5
  tolerance, not refused) is resolved by the model it belongs to (`ffxvr_fpown.h`, `fp_tie_resolve`):
  - the model key is the draw's PS slot-0 texture (the atlas); identical NPC models share one, which is harmless;
  - the draw's palette is world space (M = W_joint x Bind^-1). Its head points (the bind axis at y = -14.5, -15.8 (Tidus's
    neck joint), -17.0, -18.2) are compared with the player's head sub-position and with the tied actor's (managed now sends each
    other actor's head with its root, ABI 39). The frame's nearest distances per texture step a score at Present: +1 when the
    player's head is within 3 of the model's head points and 1.5 nearer than the other's; -1 when the other's head is, AND the
    player's is more than 4.5 away (`k_head_far`: a wrong -1 would show his body);
  - score >= 2: the player's model; <= -3 (three frames from nothing): another actor's; a draw clear of every other root by 3
    units (normal play) makes its texture the player's (every 16th frame);
  - a tied draw of another actor's model is kept visible; of the player's, or of a model not yet known, hidden as before.
  The identity is used only to turn a hide into a show: nothing the old rule showed is hidden. The table is cleared with a new
  re-pick key (the Luca c001 -> c101 swaps) and entries expire after 600 frames unseen. The light-camera hide (fp_self_shadow 0)
  uses the same verdict. bones_capture / bones_record (the palette work in the upload hook, which has no texture) keep the
  positional rule.
- **Why not the alternatives.** A signature learned only on frames with no other root near (the owner's suggestion) cannot
  start here: the two share the anchor from the scene's first frame, so it is kept only as a second source of evidence. Vertex
  / index buffers or the actor's model handle (+0x830) would need the Phyre mesh structures mapped to D3D objects (not done;
  nothing in the logs to check it against). The draw order follows the actor table for the opaque pass only.
- **The engine's model chain (debug-revival.md section 3, checked in the disassembly, not at runtime).** actor +0x830 -> a
  handle {count | 0x80000000, instance pointers}; an instance's [0] = segment records (0x6C bytes: +0x18 part id, +0x14 the
  visible byte, [+0] a material index into the instance's +0x10 list); a material's parameter block carries "MaterialColour"
  (0x63BC80 writes it, then 0x651400). None of it reaches a D3D object in what was read: the draw side is the render-command
  interpreter (0x615550 type 1 -> 0x61C400 -> 0x5934C0), whose records would have to be traced to the ID3D11 buffers and views
  the hook sees. Not done; an exact draw -> actor map stays a lead (a runtime test: flip one segment's visible byte of actor A
  and see which skinned draw goes). The MaterialColour value is normally the same for every actor, so the uploaded constant
  buffers cannot tell actors apart by it.
- **Shared roots in the module log.** When another live, drawn actor's root is within 0.05 of the followed one's, both actors'
  attach fields (+0x19C parent as a table index, +0x1A0 bone, +0x1A4.. offset, +0x1B0 scale, +0x1B8), the override matrix
  (+0x10C), +0x180, +0x0C and +0x6C, and the model chain's head (+0x830: instances, segments, materials) are logged once per
  pairing (40 a session): "body hide: actors 0 'c101' and 1 'c102' share a root ...". The owner's next repro says whether one is
  attached to the other.
- **Limits.** Heads closer than about 4.5 units are often unresolved (hidden, the old behaviour): the scratch test resolves
  every tie with heads 6+ units apart in 97% of scenes, 4-6 units in 60%, under 4 rarely. A first tie shows the other model
  after three to five frames. The test models the head sub-position as within 3 units of the bind axis; Tidus stays hidden with
  it up to 6 units off (without `k_head_far` only up to about 4). Untested in the headset.
- **Diagnostics.** The `body hide:` line (1/s, 200 a session) now also fires on a tie: "... | N draws tied with another actor's
  root (same anchor, within 0.50): resolved by model identity (...): K kept visible as another actor's, P hidden as the
  player's, U unresolved (hidden, the tie rule) | the player's head sub-position D from the centre | models: <texture> player /
  other / ? (score, head p / q)". The 10 s line adds the tie frames, draws and those kept visible. F8's "NPC parts kept visible"
  counts them. A frame census lists every body-hidden draw ("body hide: frame draw N HIDDEN as the player's part | world ... |
  s0 ... | from the player ..., the nearest other root ... (tied) | model v") and every tie kept visible.
- **ABI 39:** ffxvr_fp_others_set(xyz, heads, n).
- **Tests (container):** anc_chk/tests/anchor_test (the header and the native glue and draw-site block extracted verbatim): the
  owner's scene (Tidus standing, Yuna sitting, one anchor: Tidus hidden on every frame, Yuna shown from the fifth, nothing hidden
  that the old rule showed), 3000 random shared-anchor scenes (two or three models, sitting / standing, 0-15 units apart: Tidus
  never shown), no heads sent = the old rule, the Luca double (one pose on one root: both hidden), the re-pick reset, normal play
  (equal to the old rule; his texture learned), riding (200,000 cases equal to the old rule). Mutations caught: no margin, a
  one-frame verdict, the player's head alone, every matched draw counted as tied, the neck point alone. The npc-hide fpown and
  SF1 tests pass unchanged against the new code; the cpu-lean CpuTests check the heads (live and snapshot walks).

### Round 2: the exact owner by index buffer, and the review fixes (4 Nov 2026)

- **The chain to D3D** (research/fh-structs.md 1a; checked in the disassembly, not at runtime). actor +0x830 -> model object
  {+0 count | 0x80000000, +4 PMeshInstance*[]} (0x63C540 fills it with what 0x66F7D0 returns: the PMeshInstance clone) ->
  instance +0 PMesh (shared per model) -> {+0 count, +4 segments, 0x6C each} -> the index block inline at segment +0x30:
  +0x08 index count, +0x14 buffer count (inline entry when 1, else a pointer at +0x18; entries 0x14 bytes, ID3D11Buffer* at
  +0), +0x2C byte offset, +0x30 bit 0 "has a GPU buffer". 0x592C60 binds exactly that: IASetIndexBuffer(entry k's buffer,
  R32 if +0x0C == 8 else R16, +0x2C) and DrawIndexed(count, 0, 0). So a draw's bound index buffer + offset name its segment,
  hence its model; per model, not per actor (two actors of one model share the PMesh).
- **The rule.** Managed (body_hide.cs `body_hide_ib`, `IbChain.Walk`) walks the followed actor's chain and those of the live,
  drawn actors within 6 units and sends ffxvr_fp_ib_set (ABI 40) (buffer, offset, count, owner) entries every frame; rebuilt
  when the set of actors, an actor's +0x830 or +0x1C4, the pick or the scene changes, and every 30 frames while a chain did
  not read whole (+0x830 is 0 for a few frames after a load: that actor adds nothing until set). Native, for a matched draw on
  foot only: IAGetIndexBuffer; in the table, the owner decides (the player's: hidden; another actor's: shown); a key with
  both owners (identical models) or not in the table: the texture + head rule, then the positional rule. The table is used
  only in the frame after a call. Still only a hide turned into a show: refused or unmatched draws are never looked up.
- **Review fixes.** SF1: a negative texture score gets +1 on any frame with the player's head within 3 units of the model's
  head points, and is clamped at -4 (a transient no longer latches Tidus visible). SF2: the re-pick key carries a scene
  generation (map name, event id, the followed actor's +0x830): a map change or load clears the texture table. N3: an
  attached actor's head is never folded onto its +0x0C. N4: tie-only lines have their own budget (120, one every 2 s).
- **Diagnostics.** The `body hide:` line: "IB table (N entries, build B): matched draws decided D (the player's P, another
  actor's O), a key of both (identical models) K, not in it M, count differs C" and, in a tie, "(T by the IB table ...)". Each
  build: "body hide: IB table build B on map M: N entries | actor 0 'c101' the player's: +0x830 ..., +0x1C4 ..., S segments,
  K keys ib+offset/count ... | actor 1 'c102' another actor's ..." in the module log (30 a session). The census lists every
  body-hidden or kept draw with "ib XXXXXXXX +offset fmt F, DrawIndexed n from start base b | IB table <owner>": one census
  proves whether the table's keys are what the draws bind (SF3: the texture-per-model assumption is then no longer needed).
- **Not verified.** The chain at runtime (layout, the buffer index k, whether every character draw goes through 0x592C60
  with start 0); a "count differs" figure above 0 would say the caller draws a different count than the block's.
- **Tests.** anchor_test 50 checks: the IB table with heads too close for the head rule (Tidus hidden, Yuna shown from the
  first frame; without the table she stays hidden), a table without the player's keys, a table of neither model (the same
  decisions as none), identical models (nothing decided), a stale table (unused from the second frame without a call), a new
  build replacing a wrong owner, refused NPCs never looked up, SF1 (a 4-frame head transient beside Yuna's head: 0 of 100
  scenes latched), SF2 (a new scene generation clears the table). CpuTests: IbChain.Walk on a fake chain (inline and array
  entries, no-GPU segments, the cap, a failed read, no model yet) and HeadOf for attached actors.

### Round 2 review fixes (4 Nov 2026)

- **B1: a listed model is the player's.** A slot whose model is in the fp_actor list (c001, c101, ...) goes into the IB table
  as the player's, never "another actor's" (IbChain.Owner): a Tidus double near him (the c001 -> c101 handover before the
  script hides c001, the field model back before the event model leaves) stays hidden, also when it is drawn on his root with
  its own root reported elsewhere.
- **SF1: no stale table.** Managed rebuilds after any gap in its calls (a load, a menu, no followed actor: buffers can be freed
  and reused at the same addresses behind an unchanged signature) and every 60 frames regardless; native never re-arms a
  table after a gap with the same build number (only a new build fills it). A periodic rebuild whose keys changed says so in
  its log line.
- **N1:** when the followed actor's own walk gives no keys (or a read fails), no table is sent (n = 0): a same-model actor's
  keys can never stand alone as "another actor's".
- **N2:** a tied draw still feeds the texture rule when the table decides it, so the fallback is warm if the table drops out.
- **N3:** a build is logged only when its key set differs from the last one logged (30 a session).
- **Left as they are (review 2 N4, N5).** N4: the light-camera path shares the scene's IB counters (F8's "decided by it"
  includes shadow casters); a frame that is "wide" while the 200-line budget is spent writes no line even with tie budget
  left; an NPC part kept visible by the table near the player in normal play spends the shared 200 at 1/s; light-camera hides
  have no census line. N5, for the census to confirm: that the draw packet's index block (+0x34) is the PMesh segment's;
  that the buffer index k is 0..3 (a 5th buffer falls back); that no character draw reaches DrawIndexed without its own
  IASetIndexBuffer (the vtbl +0x0C branch at 0x614575, packet +0x04 != 0, is not 0x592C60: such a draw would inherit the
  previous draw's buffer and its owner). One shared-anchor census answers all three: "count differs" 0, and every kept draw's
  ib/offset in the build line.

### Round 3: the eye on a body drawn away from its root (4 Nov 2026)

- **The owner's run (grid-timer d2c20d1 = npc-anchor 7cf1e3e + grid timing).** Yuna visible, Tidus hidden, but the eye
  floated above Yuna. The census (14:00:27 and :31) confirms the IB table: Tidus's 13 draws per pass "HIDDEN ... IB table the
  player's" (textures 405420E4 and 40535B64: c101 uses two, review SF3 was right), Yuna's 25 "kept visible: another actor's
  segment" (one texture, 406F4CA4); every segment its own index buffer at offset 0; "count differs 0", "not in it 0". The
  body hide line: 38 tied draws a frame, all decided by the table; the head rule agreed (Tidus's textures dp 0.1, Yuna's dq 0.4).
  The module log: c101 and c102 not attached, no override matrix, chr_offset 0, both +0x0C on the anchor.
- **Why the eye was wrong.** The head sub-position was right: "the player's head sub-position 67.49 from the centre", and his
  palette's head points within 0.1 of it. But fp_head_anchor 1 (the owner's setting, and 0) takes only the head's HEIGHT and the
  horizontal position from the actor's root (g_fp_pos), by design (no lean when swimming or crouching). The root is the anchor,
  where Yuna sits. (My round-1 note that "the eye sits in Tidus's head" held for fp_head_anchor 2 only.)
- **The fix.** Managed (EyeLogic, body_hide.cs) measures the horizontal distance from the drawn root (+0x200) to the NEAREST of
  the followed actor's 26 sub-positions: in normal play one of them (feet, hips) is always near the root, also prone or
  crouching; on a displaced body all are far. Over 6 units for 10 frames in a row: on; under 3 for 10 frames: off; an unknown
  or implausible value (none set, over 400) breaks a run. On foot in the field only (riding and battle unchanged), reset on a
  re-pick. It sets bit 1 of ffxvr_player_head_set's `valid` (the signature unchanged; old managed sends 0 / 1). Native then
  takes the eye's horizontal position from the head sub-position in fp_head_anchor 0 and 1 too (fp_eye_base; mode 2 always
  did), and while it does, a head that jumps more than 8 units from the smoothed one is held for up to 3 frames (a one-frame
  bad value never moves the view; a real move is taken on the 4th). The switch itself is a cut, not a glide.
- **Unchanged.** The body hide (its centre is still the root: the draws' world translation is the root), the shadow (drawn by
  the game at the body), the hands (relative to the eye). Without the bit every mode's eye base is bit-identical to before
  (test). The head sub-position steps at 30 Hz (not interpolated), as in mode 2: while the displaced body walks, the eye may
  step with it.
- **Diagnostics.** ffxvr_diag.log: "eye: from the drawn head: the followed actor's body is drawn away from its root (head
  sub-position N units from the root horizontally; the palette's head points X units from it) ..." and "eye: back to the
  root's horizontal position ..." (60 a session). Module log: "eye: from the drawn head / back to the root: the nearest of
  'c101''s sub-positions is N units from its root horizontally ..." (40 a session).
- **Tests.** eye_test (fp_eye_base and ffxvr_player_head_set extracted): without the bit, 200,000 random heads in all three
  modes equal the old formula; with it, modes 0 / 1 at the head, mode 2 unchanged; a 30-unit one-frame spike held; a real
  15-unit move taken on the 4th frame; the switch back; no head. CpuTests: EyeLogic never switches over 240,000 frames of
  normal play (standing, running a tick off, prone); a body 67 units away switches after 10 frames; one frame on the root or an
  isolated spike does not switch; an unknown distance breaks a run.
- **Not verified.** Whether some normal-play pose (swimming) leaves every sub-position 6+ units from the root (then the eye
  would follow the head there too); the headset.

### Round 3 review fixes (eye-head-review.md, 4 Nov 2026)

- **SF1:** a displaced head is never folded onto +0x0C (the "looks relative" rule for heads 100+ units off): push_player
  decides the flag before the fold and skips it while the flag is on; the post-join snapshot marks a folded head (D7Join flags
  4) and sa_apply undoes the fold for a displaced body. (Before: a body 75 units out on a diagonal put the eye ~257 units off.)
- **SF2:** the flag can turn on only while another live, drawn actor's root is within 0.5 of the followed actor's root (the
  shared-anchor signature itself), in first person, on foot, in the field, not in blitzball. Normal play cannot turn it on,
  whichever joints the 26 sub-positions are.
- **SF3:** the spike hold compares with the last accepted raw head, not the smoothed one (heavy smoothing no longer runs away).
- **N1:** one or two frames without a head while displaced keep the last eye. **N2:** the switch in and out blinks (4 frames,
  the camera-cut blink). **N3:** the flag's state key carries the scene generation.
- **Round 3b (the owner's retest, 14:28).** The eye switched on (body 66.9 units out) and then back 11 s later: Tidus's feet
  walked back to 1.4 units from the anchor while his head was still 8.4 units out, and the distance off-switch put the eye on
  the anchor (above Yuna) again. The rule is now the shared root itself: while another live, drawn actor's root is within 0.5
  of the followed one's for 10 frames, the eye follows the drawn head whatever the distance; it goes back after 10 frames
  without the shared root (or on the exclusions). The body's distance only guards plausibility (none set, over 400, breaks a
  run). The fold skip, the spike hold, the missing-head frames and the blink are unchanged.
- **Left (eye review N4-N8).** N4: a root jump while displaced moves the eye with the root for up to 3 frames, then it glides
  back (offsets are relative to the root); real 8-50 unit moves glide over ~10 frames at the default smoothing. N5: during a
  hold the raw head is stale for the tie rule and the census (harmless: a stale dp gives no "other" evidence). N6: without the
  flag the eye base is equal in value, not always in bits (-0.0 + 0.0). N7: with a +0x10C script matrix the metric (+0x200)
  and the eye base (+0x0C) use different origins (pre-existing). N8: k_frames counts Presents (10 = 167 ms at 60 fps).

### Round 3c: not for a sphere grab (7 Nov 2026)

- **The owner's report.** In a cloister, taking a sphere from a wall socket in first person: Tidus's body moved abruptly
  during the grab animation and the view went up close into the wall. Suspected after-effect of round 3b.
- **The logs (Djose, not Kilika: dj/djyt0000 at 07:26:44-52 local and dj/djyt0600 at 07:43:24-32).** The rule switched on
  and off around each grab (about 8 s each). The module log names the actor on the root: actor 13 `f010` (an object model,
  chr_id type 5, 2 segments; +0x0C on Tidus's, not attached, no script matrix). Its root was 0.01-0.09 units from his for
  421 and 428 frames. Round 3b's rule (the shared root alone) was met by the sphere.
- **But the rule did not move the eye there.** c001's head pick (push_player's rule, the highest sub-position) is index 25,
  a helper point exactly 18 units over the root (every optpos dump: `25*: (root x, root y - 18, root z)`). The census during
  both grabs: "the player's head sub-position 18.00 from the centre" in every line; the drawn palette head 0.0-0.2 units from
  it (texture 3B3F1FA4) and the root still (step 0) through 07:26:44-50. With the bit on, the eye's horizontal offset was the
  smoothed head offset, 0. So the switch only gave two 4-frame blinks; the close-to-the-wall view most likely has another
  cause: the game standing Tidus's root right at the socket (the eye is at the root horizontally in fp_head_anchor 1), or the
  drawn body moving off the root during the grab (which the head pick of c001 cannot show). Not settled by these logs.
- **The fix (EyeLogic.Step, body_hide.cs).** Entering needs, on each of the 10 frames, the shared root AND the head
  sub-position more than `k_enter` = 12 units (1.05 m) from the root horizontally. A body drawn at its root does not put its
  head that far out with a lean or a reach (a bend or a kneel: 6-9 units); mi/mihn0100's head was 67 units out. Once on, it
  stays on while the root stays shared, whatever the distance (round 3b's walk-back to 8.4 / 1.4 is unchanged). Leaving is
  unchanged. The measure is the head pick itself (what the eye would follow), not the nearest sub-position (index 0 of c001
  is the root itself, always 0.0).
- **Not chosen: a gate on the sharing actor's kind.** The type in chr_id (0 `c`, 2 `n`, 4 `w`, 5 `f`) does not say "has a
  body": the chocobo `f051` is an `f` model with a skeleton, and an event can anchor Tidus on a prop. The displacement is
  the property that matters (would following the head move the eye a lot?), so it is the only gate; the kind is logged.
- **The eye while on (checked, unchanged).** fp_eye_base in fp_head_anchor 0 / 1 takes only the horizontal part of the
  smoothed head offset (g_fp_head_s, 0.35 a frame; spikes over 8 units held for up to 3 frames) and adds no head-length
  along the lean (mode 2's term); the height is the mode's own (mode 1: the head's height, as without the bit; mode 0: the
  fixed eye height). A lean moves the eye only while the rule is on, which now needs a body drawn 12+ units off its root.
- **Diagnostics.** Module log: a shared run that the rule declined (10+ frames) gets one line at its end: "eye: stays on the
  root: actor N 'f010' (chr_id 0x5..., an object (chr/obj)) shared 'c001''s root for F frames, but his head sub-position
  was at most X units from it horizontally (last Y; ...); his farthest sub-position Z units out (index K)". The on line
  names the sharing actor too. These lines have their own budget, 20 a session (review N1: a cloister-heavy session cannot
  silence the on / off lines, 40 a session). The slot and model are the hook for the carried-sphere item (Backlog).
- **Known (review N3).** If the state's key resets inside a displaced scene (a load or map change: a new scene generation)
  while the head is under 12 units out, the rule re-enters only once the head goes past 12 again (before: after 10 frames).
  Meanwhile the eye is under 12 units from the head. If the owner sees the eye "stuck at the anchor" after a load inside
  mi/mihn0100, this is it.
- **Tests.** scratchpad eg_chk/eye_grab_test (EyeLogic extracted from body_hide.cs and transpiled mechanically): normal
  play never on; the Djose grab (shared 421 frames, head 0-1.7) never on and declined every frame; a grab with a lean to 8,
  a one-frame pop to 30 and 9 frames at 14 never on; mihn0100 on after 10 frames at 70 and on through the walk-back to 8.4,
  1.4 and 0, off 10 frames after the shared root ends; a scene starting 5 out that moves to 40 switches at its 10th frame
  past 12; the threshold (exactly 12.0: no entry, and the decline line's condition; review N2), run breaks, unknown / NaN /
  over-400 heads. 13 of 13 mutants caught.

## Battle-start banners in double (4 Nov 2026)

- **What the owner saw.** In battles that start with a message ("Preemptive Strike!", "Ambushed!") the 2D UI is not on the
  VR panel yet: the banner is doubled, impossible to fuse.
- **The mechanism (EXE and log; high confidence).** The banner is drawn at battle_state 0x16, the end of the load chain,
  where the battle is already drawn with its camera. MsBattle's state machine (0x783020) sets state 0x15, registers the cue
  from Btl.ambush_state [0x112C9DC] (1 -> MsMessageCueRegist type 2 PREEMPTIVE, 2 -> type 3 AMBUSH; 0x783569), sets 0x16
  (0x7835AC), and leaves 0x16 for 1 only once the cue slot [0x112C910] is clear (0x7835B3). The battle main (0x790C10)
  processes the cue in the same tick for any non-zero state (0x790D83 -> 0x79CDF0 -> TOBtlDrawFirstStrike*MessageWindow
  0x8934F0 / 0x8934D0), and the battle UI draw puts the message window in the UI pass. BT-3 counted 0x16 as the load chain,
  so the transition window stayed open through it, with no UI split (bt_no_split): the UI pass went into both eye images.
  The doubling was there in every random battle (4 Nov log: 60-231 frames per battle at state 22 with `cam3d=329 dclr=1
  uiframe=0`; the one script glass break, window 1, had `dclr=0` there: no UI pass), but only the banner is visible
  there: the command, CTB and party windows come at state 1.
- **The glass break overlaps it.** State 22 starts while the shatter's last pieces still fly (23-45 frames with shard draws
  in 11 of 13 random windows), and the handle stays set ~50 frames after the last shard. So "AFTER at 0x16" alone would fix
  little: in most battles the banner is up in the shatter phase.
- **BT-B-1, the fix (native, bt_tick).** `bt_intro_live`: an encounter window in SHATTER or AFTER counts state 0x16 with a
  3D camera (BT-3's 2 frames) as live when the glass break has stopped drawing: AFTER with handle 0 (the effect completed),
  or real shards seen this window (`g_bt_real_seen`) and 6 still Presents in a row (`g_bt_still_n`: no shard draw, no
  capture of capturedFrame's size, +5 clear), the handle's tail set or not. Real shards are required for the second case,
  so a window whose shard detection failed does not end while the handle is set; it ends at the intro once it is in AFTER
  with handle 0 (the effect completed), not at state 1 as before. ARMED (a haze, BT-H-1) and every other state are unchanged.
  After such an end the re-arm block (`every signal clear`) waits for the handle's tail, as after BT-H-1. In mode 2 the freeze
  and the AFTER hold (the pinned frame over the load black) also end at the intro instead of at state 1. No ABI change.
- **Effect (replayed on the 4 Nov log).** Doubled frames at state 22 go from 60-231 per battle to 2-52; e.g. window 5 ends at
  f=26520 instead of 26703. What is left is the 6 still + 2 live frames, plus the frames where the glass still flies.
- **Not fixed: the banner over the flying glass (0.4-0.75 s).** While shards are drawn, the shatter's fade pass has the UI
  pass's signature (battle-transition.md 2.2), so the split stays off. A follow-up could open the UI pass at a depth clear
  that comes after this frame's shard draws (the second clear: `dclr=2` on those frames). That changes the UI detector
  inside the glass break, so it is not part of this fix.
- **Known limits (review N4).** MsMessageCueRegist only queues the cue; the slot [0x112C910] is written by the process call
  later in the tick. 0x14 -> 0x15 -> 0x16 -> its test run in one call of 0x783020, so if 0x7A04C0, 0x784220 and [0x112CA64]
  are all clear in that tick, the game goes to state 1 with the banner still queued, and the banner shows at state 1. If the
  handle's tail is still set then, the window stays open (BT-3 needs handle 0) and the banner is doubled as before. Never
  seen: state 22 lasted 39+ frames in every logged window. Haze windows (ARMED) at state 22 keep the split off until state 1
  as before; whether a scripted battle ever carries ambush_state 1 or 2 is unknown.
- **Diagnostics.** `bt: f=` lines end with `amb=N cue=M` (ambush_state; the cue slot, non-zero while a battle message is up;
  -99 = not read; the AFTER-phase thinning never hides a cue change). The new release: `bt: frame N: shatter -> idle: the
  battle intro in the shatter phase (battle_state 22 with a 3D camera, 2 frames; no BrokenScreen drawing for K Presents,
  handle H; ambush_state A, message cue up|none)`, or `...; the effect completed (handle 0); ...` on the AFTER path. The
  summary's window part gets `ambush_state N at the end` (`1 (preemptive)`, `2 (ambushed)`); a window that ends in the
  shatter phase records its end state (`battle_state a..b, c at its end`) and says `no after phase (ended in the shatter
  phase)` instead of `after 0 ms` (review N1).
- **Tests (container):** bt_intro_test (bt_intro_live and bt_running_state extracted verbatim): every state and phase, the
  handle's tail, a replay of window 5 (release at 26520), no shards detected (26570, AFTER with handle 0), a stray
  capture-sampling draw, a capture candidate every frame, no camera, a non-encounter window. Not verified: MSVC build, the
  headset, which logged battles had a banner.

## Sphere grid frame pacing and CPU timings (4 Nov 2026)

- **What the owner saw.** The sphere grid no longer holds 60 fps in the headset (it did on 2 Nov). The GPU span there is 2-3 ms;
  the render thread reaches its ring wait after the headset slot has passed (`sr:` "skipped by us: past the deadline" 30-80/s),
  so frames come out at 3-5 slots of 8.33 ms. The main menu (also the virtual screen) holds in the same session. The culprit
  is not proven: this build makes the render thread's CPU time visible and removes the per-frame work found in the window
  that was plainly wasted (research grid-cpu.md). Nothing else changes.
- **The `cpu:` line.** Logged with every `perf:` line (the same `diag()` call, so the same cadence and the same logging
  profile: every 30 s, 5 s while slow, also in the release profile). Every field is avg/max per frame over the window since
  the last line (not the `perf:` line's last 60 frames). QPC only, fixed storage, nothing logged per frame, always on.
  - `begin_frame` and its sections: `head` (the stamps, the frame clock, pick, C3, the hands' frame end), `pre` (the capture
    lock, tick, the camera read, the cache sweeps, the GPU span's end), `xr wait` (our xrWaitFrame with the load-lock release
    and re-take around it: the pacing wait, not work), `xr rest` (the rest of xr_tick: copies, submit, xrEndFrame, Begin,
    sync, locate, input), `perf` (C2, the pointer sprites, perf_tick), `slow` (slow_frame), `tail`.
  - `managed` (ffxvr_cpu_report, once a frame at the end of render_imgui): `total`, `before begin_frame`, `begin_frame call`
    (includes the native sections above), `mid` (hotkeys, hover, gestures), `window` (F8), `push_all` = `player` [`body
    hide`, `ride`] + `hands` [`tap gate`] + `rest`, `after`; `gc 0/1/2` = collections in the window.
  - `safe_read` calls, `VirtualQuery` calls, `cache hits` and `range_readable` ms per frame (render thread).
  - `after our hook to the ring wait`: from the end of our managed hook to the render thread's next ring wait (Fahrenheit's
    later ImGui work, the real Present, the rest of the Present list); how many of those entries were past the open slot's
    deadline and by how much, how many by more than the margin (those slots are lost), and `skipped by us` (the `sr:` line's
    skip_ours) over the same window, with a per-second rate to compare with the `sr:` line's 10 s counts. "not measured"
    when stick reprojection's ring wait is not installed.
- **The savings (each keeps the result; scratch tests in the cpu-lean notes).**
  - (a) range_readable: on the render thread, a range inside a page run of FFX.exe's own image (MEM_IMAGE, the exe's
    allocation base) that VirtualQuery reported readable earlier in the frame skips the query (4 runs, cleared at every
    Present). Only the exe is cached because it is the one region that cannot be freed mid-frame; heap, mapped and other
    modules' regions (the game's magic_*.dll unload mid-session) and every other thread query on every read, as before.
    The page rule is unchanged. Assumption: nothing makes an exe page PAGE_GUARD / NOACCESS mid-frame (a debugger's memory
    breakpoint would).
  - (b) the tap gate: `ffxvr_hands2_set` returns whether the coming frame can draw the hands (on, headset wanted, not the
    virtual screen, the stereo screen or a backdrop room: hands_hide_why's rules before the battle rule, fixed from push_all
    to the next Present). Managed reads every game bit only when the tap is on in first person, only the battle bit
    (`| 0x400`, gate shut) when the hands can be drawn otherwise, nothing (`0x200`: hidden as in battle, gate shut) when they
    cannot. The hands stay hidden in every battle. Side effect: no `hands: tap gate (game)` line from the virtual screen (the
    sphere grid's "sphere grid" line goes; the gate is shut there anyway), and F8's tap `game:` field says "not read this
    frame (hands can't show)" there ("... (no tap outside first person)" in the other views). The first line after such a
    stretch compares with the last full read.
  - (c) the body hide: `ffxvr_fp_self_set` returns 0 when the frame goes to the virtual screen (g_ovr_suspend: the menus,
    the sphere grid); every user of the other roots (the two body-hide draw sites and maybe_override's bone record) needs the
    override live, so nothing is walked. Elsewhere the walk runs every frame as before, but from one guarded read of the whole
    table (48 x 0x880 bytes), else slot by slot.
    The owner asked for a short list refreshed every 8-10 frames, with a full walk on (1) a match beyond 0.75 last frame, (2) a
    radius over 1.5, (3) a re-pick, map change, battle end or reload, (4) a live-count change. The snapshot makes that
    unnecessary: a full walk from it costs no extra reads and nothing in it is stale. It also closes a gap the triggers leave:
    (1) is the previous frame's, so an NPC placed by script 0.75..1.5 from Tidus between refreshes (inside the radius, outside
    the 0.75 never-refused core) would be hidden for its first frame. The scratch test replays 16 000 such placements: kept
    visible from the first frame, and 1.95 M draws get the same hide decision as the walk of 521d237.
  - (d) diag_on(): log_push built a List and an interpolated string every frame; diag_any() is the same test without them.
  - (e) the follow pick: the rescan (every frame for the actor-0 last resort, a hidden listed model and a hidden party model)
    reads the same snapshot, and a slot's name rank is kept while its name pointer, chr_id and fp_actor are the same (read
    again every 30 scan ticks). Same result, cadence and tie rule; the hidden-party fallback no longer costs 48+ reads a frame.
    Assumption: the name under an unchanged pointer and chr_id does not change (the name is the model's).
  - Untouched: "keep characters drawn", the distance culls and every cull patch, the pacing (stickrep, lock release).
- **Reading the maxima (review N1).** The `cpu:` and `perf:` lines are written inside perf_tick, so the frame that writes
  them (two log appends) lands in the next window's `perf` and `begin_frame`: their maxima can be the log write itself,
  not our work. The averages are not affected.
- **ABI 38:** + ffxvr_cpu_report; ffxvr_hands2_set and ffxvr_fp_self_set return int; tap game bits 0x200 / 0x400.
- **Reading the A/B.** The savings have no switch: the A/B is this build against bt-banner (521d237) at the same save,
  each in the grid for a minute (fps60.log PERF-002, the `sr:` line's fresh and skipped-by-us counts). On this build the
  grid's `cpu:` line says where the render thread's time goes: our own work is `begin_frame` minus `xr wait`, plus `managed
  total` minus `begin_frame call`; `after our hook to the ring wait` is everything after it. If the grid still misses slots
  while our own work is well under a millisecond and the ring wait is entered late, the time is outside our hook
  (Fahrenheit's ImGui, the real Present, the game's list, or the slot clock itself), and the next step is there.
- **The limiter and its timer (grid-timer, 4 Nov 2026).** A lead from UnX: the game's main loop (SKS_WinMainLoop 0x62EE50)
  runs a frame when due (`call 0x62E340` at 0x62F350) and otherwise sleeps the rest, truncated to whole milliseconds
  (`Sleep` at 0x62F3BB; the x87 rounding is set to chop), on the process's timer (the game asks 5 ms at 0x42FE10; FFX-60fps'
  PERF-001 asks 1 ms and opts the process out of timer-resolution throttling). Windows 11 may coarsen the timer of a process
  whose windows are minimized or covered, likely in the headset, unless it opted out. A light scene leaves the most sleep,
  so a coarse timer would show there first (frames of 3-5 slots with the GPU idle). The first cpu-lean sessions never
  opened the grid. They do show FFX-60fps' opt-out and 1 ms request succeeding (fps60.log PERF-001), on 2 Nov as today, so
  this is a lead to measure, not a known cause.
  - The `cpu:` line's new part: `timer` (NtQueryTimerResolution: now, the window's range, finest / coarsest; it is the
    system's resolution, the finest any process holds, so the limiter's own Sleep is the real test), `timer_fix`'s state;
    `game window` sampled every 30 Presents (foreground, minimized, DWM-cloaked, hidden, five client points covered by
    another window or off-screen); `main loop` (limiter_probe=1): frame-start intervals against the limiter's period (the
    game's divider x FFX-60fps' K: 16.68 ms at 60, 33.37 at stock), late by > 1 / > 2 ms / > a slot, early by > 2 ms, gaps
    over 250 ms kept out, each frame's run time (0x62E340, its waits on the render thread included), frames that started
    without a sleep (already due); `limiter Sleep`: calls, Sleep(0) count, asked vs took, oversleep (> 1 / > 2 ms).
    The job join could not give the frame starts: it does not run in the grid (no join calls there).
  - How it is measured: FFX.exe's import slot of kernel32!Sleep (a data write) points at a wrapper that times only the call
    returning to 0x62F3C1 and passes the slot's other users straight to its previous value (7 more call sites, a jmp thunk,
    and two places that copy the pointer into edi: a copy taken after the swap is our wrapper, whose return address never
    matches). The frame call's rel32 is redirected to a plain cdecl wrapper (0x62E340 takes no arguments and returns in eax)
    with one aligned 8-byte compare-exchange. That exchange is done by the main thread itself, inside the first limiter
    Sleep after the bytes were checked, where it provably is not at 0x62F350: self-modifying code on one thread, as
    sr_patch's (cross-modifying it from the render thread would need the executing core to serialize). A `timer:` line
    says when it happened. Both sites are checked against the exe's bytes first (refused and logged otherwise). Never
    removed (the DLL stays loaded); limiter_probe=0 later only stops the recording and the window sampling (its off
    switch). FFX-60fps writes only K (data) there.
  - Reading it: with a 1 ms timer the oversleep stays under about 1 ms and frame starts within about 1 ms of the period;
    the game's 5 ms gives oversleeps up to 4-5 ms and frames up to 20 ms at 60; the 15.6 ms default gives oversleeps up to
    15 ms and frames of 25-31 ms (a scratch model of the loop, grid-timer notes). Lateness with small oversleeps and long
    frame runs points to the frame itself (or the render thread it waits on); covered / minimized samples together with
    big oversleeps point to the timer.
  - `timer_fix` (ini, default 0): SetProcessInformation(ProcessPowerThrottling) adds the timer-resolution and execution-
    speed opt-out to whatever policy is set (read first with GetProcessInformation where Windows allows it), plus
    timeBeginPeriod(1) for the session; all looked up at run time, harmless where missing (if Windows refuses the
    timer flag, as older builds may: the execution-speed opt-out alone), logged once (`timer:` line). Off by default: it
    duplicates PERF-001 when FFX-60fps runs it, and without FFX-60fps it would move the game's limiter from its own 5 ms
    timer to 1 ms, a pacing change; it is there for the A/B with PERF-001=0. 1 -> 0 at run time releases our 1 ms request only; the opt-out
    stays until the game restarts. The game's Sleep itself is not replaced.
  - The settings ride on ffxvr_cpu_report (two values after the 15 timings): no export changed, so the ABI is unchanged (40 with npc-anchor).
- **VirtualQuery cost (vq-lean, 4 Nov 2026).** The owner's 4 Nov session (14:34-14:56) shows the guarded reads, not their
  number, as the cost: in the 3D field 13 queries a frame took 0.02 ms, but in the two random battles in mi/mihn0800 and on
  the mihn0300 virtual screen 9-73 queries took 1.4-8.9 ms a frame (100-160 us each), nearly all of push_all's `player`
  part. VirtualQuery scans the page table from the queried page to the first page with other attributes, so a read into
  one of the game's large committed blocks (the 128 MB VirtualAlloc reservations on the `va:` lines) pays for the whole rest
  of the block, every time.
  - **What we do (`fast_reads`, ini, default 1).** The render thread's `ffxvr_safe_read` keeps every run a query answered
    readable (any type) until the next Present, at most 50 ms, and drops them all after a failed copy (ffxvr_cpu.h
    `FastReader`). A miss below a kept run of the same allocation queries once from the allocation's base (VirtualQuery's
    answer starts at the queried page; reads in random or falling order would otherwise query at each new low address).
    Bytes only in FFX.exe's image are copied as before (memcpy; the exe is never freed). Anything else is copied by the
    kernel (ReadProcessMemory on our own process): a page freed, decommitted or made no-access since its query makes the
    copy fail, without a fault and without an exception in the process, so nothing reaches the stage-0 debugger. Reads up
    to 256 bytes go through a bounce buffer so a failed one leaves the destination untouched (some managed callers ignore
    the result); the larger ones all check it. `range_readable` (whose callers read with plain loads) and the other
    threads' reads are unchanged, and `fast_reads=0` is the old path exactly. The setting rides on ffxvr_cpu_report as a
    third value after timer_fix and limiter_probe (same export, ABI 40). Review SF1: native starts with it off and managed
    sends the three settings alone (`ffxvr_cpu_report(v, -3)`; natives before vq-lean ignore n <= 0) right after the native
    loads in init, before any frame reads, and on any frame whose report is not sent, so `fast_reads=0` holds from the
    first read and does not depend on render_imgui finishing. An older ffxvr.dll never sends it: the old path.
  - **Why not SEH.** Under alpha12's stage 0 an access violation is fatal even inside `__try` (section 2 of
    exe-analysis.md); upstream's #388 passes first-chance exceptions on but writes a minidump and two stack walks for each
    one above 0xC0000000 first. A vectored handler comes after the debugger too. Faulting on purpose stays out.
  - **Why the cache alone was not enough.** The main and loader threads run while our hook does (load lock, `va:` lines at
    zone changes and battle starts and ends), so a block can be freed in the middle of our frame; a kept run copied with
    memcpy would then fault. The kernel's copy is what makes a stale entry harmless. It is also stronger than the old path
    on a miss: VirtualQuery then memcpy had a (microsecond) window of its own, which the kernel copy closes.
  - **The one residual (review N1: left as documented).** The kernel's copy consumes a guard page as a user-mode read would (the guard bit is cleared; the
    copy fails, no exception). A kept run was not a guard page when queried; it can become one within the frame only by
    VirtualProtect(PAGE_GUARD) on committed memory (a debugger's memory breakpoint) or by a free and a new thread stack
    reserved at the same address in the same frame. FFX.exe imports no VirtualProtect, and its seven VirtualAlloc calls ask
    for PAGE_READWRITE (PAGE_NOACCESS for one reserve), never PAGE_GUARD; Fahrenheit's sources set no PAGE_GUARD (its
    VirtualProtect scope is for code patches); the CLR uses guard pages for thread stacks only. Drivers were not checked. A stack's own growth never turns committed pages into guard pages. This build cannot notice new threads
    (DllMain disables thread notifications), hence the 50 ms bound as well as the Present.
  - **The cpu: line.** `VirtualQuery` and `cache hits` keep their meaning (all queries, base probes included; reads with no
    query); `range_readable` is the walk's time alone, without either copy (review N8); new: `fast_reads on: kernel copies
    N/M a frame x/y ms (over 4 KB n/m, x/y ms), F failed, base probes N/M` (or `fast_reads off`). Failed copies above zero
    outside loads would be worth a look.
  - **The actor table snapshot (review N7).** body_hide's one 104 KB read a frame (Globals.actors, likely heap) is a kernel
    copy above the kernel's small-copy size, so it goes through its locked-page path and may cost more than the small
    ones; the `over 4 KB` figures show it alone. Routing it to a fresh query plus memcpy instead was not done: that query
    is the expensive kind in a large block (100+ us measured) and brings back the old path's window for the largest read.
    If the figure turns out large, the choice can be made from the owner's numbers.
  - **Expected.** A scratch model of the battle shape (three 112 MB blocks, 70 heap and 25 exe reads a frame in random
    order): 73.6 -> 9.5 queries a frame and 87% fewer pages scanned. The kernel copy costs a system call (a few us under
    WoW64) for each heap read, about 0.1-0.3 ms a frame at 70 reads: measured on the line. Option (c), fewer reads (bulk
    copies of the structures push_player reads field by field), is managed work for later.
  - **Tests (container):** vq_chk/tests/fastread_test.cpp, a page-level address space whose VirtualQuery scans like the
    real one: readability identical to range_check over 600k reads; 720k reads under churn on another "thread" (decommit,
    release, no-access, commit) with no plain copy meeting an unreadable page and no read returning bytes from one; guard
    pages met only when PAGE_GUARD was set mid-frame; the probe, the age bound and the clear after a failed copy; nine
    mutations of ffxvr_cpu.h caught.
- **VirtualQuery cost, round 2 (lean2, 4 Nov 2026).** With fast_reads (session 17:22-17:39) battle went to 13-20 queries
  and 0.2-0.6 ms a frame, but the mihn0300 virtual screen still took 0.9 ms with 4 queries a frame (11 reads, 7 hits, one
  base probe, the 104 KB table copy): about 225 us a query, every frame. The runs are kept for one frame only, so each frame
  pays one long scan per big block it reads again, however stable that block is.
  - **Cross-frame validity was considered and dropped.** Keeping a run across frames when one VirtualQuery of the
    allocation's first page shows the same base and size: that query scans from the first page to the end of the first
    run, which in a uniform committed block is the whole block, as expensive as the probe it would replace (scratch model:
    959 us a frame in the virtual-screen shape, against 900 us now). One query of the run's last page is cheap (1-3 pages)
    but proves nothing about the pages before it: a guard page set mid-run, or a release and a new reservation with the
    same base and end, passes it.
  - **What we do: QueryWorkingSetEx first where the runs are long.** It answers per page (one page-table look-up each),
    not per run. Valid = 1 means the page is resident, so a read of it cannot fault, and a guard page (an invalid PTE until
    its first touch) never reports it. Its Win32Protection is then held to range_readable's rule. A page that is not
    resident (paged out, never touched, guard, reserved, free) reports Valid = 0, which is "unknown", and the walk asks
    VirtualQuery as before. So the check answers "readable now" or "ask VirtualQuery", never a refusal. ffxvr_cpu.h
    `ws_check`, up to 32 pages a read (the actor table is 26-27). Where VirtualQuery answered a run of 1 MB or more (outside
    the exe's image), the range [allocation base, end) is kept as a hint (`BigHints`, 8, across frames; a cost guess, never
    a safety claim: a stale one costs one extra call). On the render thread a read starting in a hinted range, outside the
    exe's image and not covered by this frame's kept runs, gets the check first; resident and readable, its pages are kept
    for the frame as non-image (copied by the kernel only) and the kernel copies it. The other threads (the main thread's
    post-join and battle-cursor snapshots in diag7.cs, once per game frame, never counted before; the loader) use the same
    check before range_readable's walk and keep the plain copy; the window between check and copy is the old path's.
    `fast_reads=0` turns all of it off. Review SF1: the "Valid = 1 is never a guard page" rule is the memory manager's
    (the WRK removes a page from the working set when VirtualProtect sets PAGE_GUARD or PAGE_NOACCESS), not a documented
    sentence, so it is checked on the machine itself before use: at the first frame roll with fast_reads on, a page of our
    own, written, must report Valid with PAGE_READWRITE, and the same page made PAGE_READWRITE | PAGE_GUARD (never read,
    released at once) must report Valid = 0 or the guard bit. Until it passes no thread uses the check; if it fails, the
    check is off for the session (`fast reads: QueryWorkingSetEx ...` line, and the verdict on every cpu: line). The render
    thread also turns it off after 64 answers in a row with no page Valid for ranges VirtualQuery then found readable (Wine
    and CrossOver answer TRUE with Valid = 0 for every page: cost only, but every such read would pay two calls).
  - **The cpu: line.** `working-set checks N/M a frame (answered N/M, x/y ms; big-run hints H)`; `reads that queried or
    checked, by caller (render thread, top 4 by time)`: return address (`ffxvr_native+RVA` from the pdb, `ffx+RVA`, or
    `JIT 0x...` for a managed call site; read<T> is one site per T; ffxvr_safe_read is noinline so its native callers are
    named, review N6), calls / queries / checks a frame, ms a frame, the last
    read's length and address, and the allocation and answer length of its last query; `other threads' reads`: main and
    others, with checks, queries and time. safe_read = hits + queried + checked reads.
  - **Expected (scratch model, c_page calibrated to the 0.9 ms):** virtual screen 900 -> 9 us a frame (1 query, 3 checks);
    battle shape 2026 -> 161 us (70 checks a frame for 70 reads at random pages of three 112 MB blocks; real reads cluster,
    so fewer). Unverified: the real per-call cost of QueryWorkingSetEx under WoW64 (the line shows it).
  - **Tests (container):** lean2_chk/tests/lean2_test.cpp (vq_chk's address space plus residency; its QueryWorkingSetEx
    reports Valid only for committed, resident, non-guard pages): the check never says yes where range_check says no (300k
    reads), FastReader's readability identical to range_check with the plain copy iff image only (450k), churn with page
    trims (540k reads, 0 faults, guard pages met only with PAGE_GUARD set mid-frame, as vq-lean), the cost model, and 13
    mutations caught; vq-lean's tests and its 9 mutations unchanged.

## Direct game-state gates and overdrive minigame state (4 Nov 2026)

Branch `gates` (on grid-timer 839d7e5, ABI 40: no export changed). Notes: scratchpad fixes/gates-impl.md.

- **Sources, credited, nothing copied.** The facts below come from three community projects, read for the addresses and
  the meaning of the game's own variables, and each was re-checked in our exe's disassembly before use (the evidence is
  in the table): **Debug Revival** (the player actor `Sg_GetPlayer` = [0x1300788], 0 while control is off), **FFX-TAS /
  FFX_TAS_Python** (GPL-3.0; its whole-game gates: control, the field menu flag, the text box byte, the choice cursors
  and the slot each screen uses, the skippable-movie flag, and its use of the overdrive state words), and the **FFX
  screen reader** (the message window and cursor pointer tables, the cursor's +0x01 / +0x02, the Tidus / Auron / Lulu
  words in its translation table). Addresses, offsets and state values are facts about the game; no code, script or DLL
  of theirs is in FFX VR (MIT).
- **Builds.** FFX-TAS, FFX Project Editor and Zanarkand Workshop read the other Steam build (the screen reader's
  "Reference" exe, sha256 3d13e5ed...); ours is the "Installed" one (0537b2a1..., exe-analysis.md). Fahrenheit alpha12
  and Debug Revival match ours. The shift is per range, not one number: 0 for 0x85A03C / 0xD2A008 / 0x84DED0, +0x8 for the
  chr lists, +0x10 for the save screen block, +0x40 for most of .data from 0xF01A44 up to 0x1471528, +0x48 for the player
  block (their 0xF00740 is our 0x1300788 - 0x400000; +0x40 would land on a float). Their numbers are module offsets:
  their 0xF3D6F4 is Tidus's word, and our module offset 0xF3D6F4 (VA 0x133D6F4) is Auron's. So never paste a number
  from them: translate, then verify in our disassembly (this explains zanarkand.md's two "contradictions" too).
- **The addresses (our VAs; the code reads them as module offsets, VA - 0x400000), with the evidence:**
  | What | Where | Evidence in our exe |
  |---|---|---|
  | Player actor (0 = no control) | dword 0x1300788 | `Sg_GetPlayer` 0x82D860 `mov eax,[0x1300788]; ret`; the only writer is `Sg_SetPlayer` 0x82DAD6 (`mov [0x1300788],eax` from its argument) |
  | A field (TkMenu) menu is up | dword 0x1340824 | set 1 at 0x8A9D8A when the TkMenu loop opens, cleared at 0x8A9FCB; 34 readers (Sg_MainLoop 0x820BD8.., the renderer 0x668EC0) |
  | A message window reached "shown" | byte 0xC5A03C | written only by 0x8B6B70(i): 0 unless window i's u16 +0x2C == 2, then 1 if it was 0; 0x8B5680 turns 1 into 0xFF. Called per window from 0x8AB969 only for windows whose other-table +6 == 1, so it keeps its value when none runs, and **its image value is 0xFF** (.data): it counts only with a window open and shown (below) |
  | Message windows | pointers [0x18676F0 + i*4], i < 8 | the pool initialiser 0x8B86F2 / 0x8B87B9: windows at 0x18663F0 + i*0x130 or 0x1866D70 + i*0x130. 0x8B8560 opens one (+0x00 = 1, +0x2C = 0), 0x8B5810 closes it (+0x2C = 3), 0x8B5850 runs it only while +0x00 == 1 |
  | Choice cursor, row | pointers [0x1868A90 + i*4]; +0x01, +0x02 | the same initialiser: cursors at 0x1867710 + i*0x138 or 0x18680D0 + i*0x138. 0x8B5FB0(i): +0x01 = 1 only when window i's +0x2C == 2 (0x8B6004), draws while +0x01 == 1 with the row `movsx [cursor+2]`; confirmed = 2 (0x8B6483, 0x8B653C); reset to 0 by the text setup 0x8B88A2 |
  | A skippable movie was started | dword 0x112A008 | 1 at 0x76EB62 / 0x76EBC0 (the skippable starts), 0 at 0x76E968 / 0x76E9D2 / 0x76EA9C / 0x76ECE9. **It lingers** (review B1): the skippable play's own wait (0x76EBE0) never clears it, 0x76ECE9 runs only from the map load 0x88E070 and 0x909A71, and the reti 0x76EB60 sets it without starting a movie; so a scene that plays a skippable FMV and goes on on the same map keeps it 1. A non-skippable movie leaves it 0 |
  | The movie player is playing | byte [[0xCDED3C] + 0x6D0] | the player object: made at 0x6D717E, 0 at 0x6D738B; +0x6D0 = 1 at 0x6D9159, 0 at 0x6D6A89 / 0x6D7884 / 0x6D9559. Every reader of 0x112A008 pairs it with this byte (0x6D947D, 0x6DA4B3, 0x6DAC06, 0x668CDF) or with 0x76EFC0; none reads the flag alone. **So "a movie" = the flag and this byte** (a null player = none) |
  | Movie id (F8 only) | dword 0xC40E28 | set at 0x76E937 / 0x76EBA4, -1 at 0x76EA1F / 0x76EA92 (the stops); the image's value is -1 |
  | Overdrive minigame up | s16 0x133D734 Tidus, 0x133D6F4 Auron, 0x133D708 Lulu, u8 0x133C93F Wakka | the battle HUD's own test 0x89B38D..0x89B3B2 (any non-zero draws the overdrive bar 0x8A1CE0); handlers 0x892320 (`mov cx,[0x133D734]`, table 0x89255C), 0x890F70, 0x891BE9; Wakka 1 at 0x898DD9, 0 at 0x890F24 / 0x898249 |
  | The shared timer | dword 0x133F77C (0 off, 1 armed, 2 running), f32 0x133F790 seconds left | TOBtlCtrlLimitTimer 0x891AC4 `cmp [0x133F77C],2`; 2 at 0x89AB5E, 1 at 0x89AB25 and the initialisers, 0 at 0x890E75 / 0x897F3F / 0x89B7F8; Tidus's state 4 compares 0 with [0x133F790] at 0x8923AA (time out -> 7) |
  | Tidus's cursor and hit zone | s16 0x133D742; zone s16 0x133D73C .. 0x133D73E | state 4: a press with lo <= cursor <= hi -> 5 (0x8923CD..0x8923EA), outside -> sound 3 and it runs on; the cursor moves by 0x133D744 within [0x133D736, + 0x133D73A] |
- **The tap gate, stricter and simpler** (revised after review: gates-review.md B1, SF1, N2, N3-N6). Managed (game_gates.cs, hands.cs) sends five new game bits, native
  (ffxvr_gates.h `tap_game_gate`, pure and unit-tested) maps them to a reason; the older gates are kept and ORed (any
  shut reason shuts). Order: not readable, battle, sphere grid, save screen, blitzball, riding, **a movie** (0x2000: the
  skippable flag with the movie player playing; the flag alone never shuts),
  **a field menu** (0x80), **a choice box** (0x800: always, whatever "also advance dialogue" says; a press in flight is let
  go), **no player control** (0x40), a text box (8, or 0x1000 = a render-side window open in state 2, the typewriter done,
  with the byte 0xC5A03C set). "Also advance dialogue" lets a text box, and the no-control reason, pass only when the
  line is fully shown: bit 8 (the Atel bank has a window) with 0x1000 (a render-side window in state 2), and no choice
  box, with or without control (SF1: a line still typing shuts, so a Yes/No box still typing cannot take a tap before its
  cursor appears). The byte adds little to 0x1000: it is overwritten per window and left at 0xFF, so it reduces to "some
  window is in state 2"; the two are not independent confirmations. Choice = a cursor with +0x01 == 1 on a window
  in state 1 or 2 (closing / confirmed windows do not count, so a stale cursor cannot kill the tap). F8's gate names
  them ("a movie", "a field menu", "a choice box", "no player control"), and so do the `hands: tap gate (game): ...` lines,
  with the render-side windows (`windows open 0x.. shown 0x.., byte 0x..`, the choice slot and row). The gestures readout
  has a "direct gates" line (the player actor, the menu flag, the movie flag and id, the windows, the choice box).
- **The hands' hide reasons**: + `menu` (the game's flag, bit 0x80, read with the battle bit on every frame that can draw
  the hands): the owner's "never in battle, menus or FMVs". The menu flag cannot outlive its menu (review N1: one setter and
  one clearer in the TkMenu tick, which runs every frame while it is set); the 3D frames it hides are the menu's fades.
  **No hide for a movie** (review N6), by choice: the frame that *is* the movie is already hidden by the screen / FMV
  detection (a screen frame, H2_SUSPEND), and a playing movie with no 3D camera has nothing to draw the hands into
  (H2_NO_TICK / H2_NO_PROJ). What is left are the movie-camera scenes (0x76EFC0: movie ids 0x20, 0x2E, 0x46, 0x48, the camera
  driven from the movie by 0x76EDC0) that render 3D over a playing movie: those are in-engine scenes to the player, so the
  hands stay, as in any cutscene. The tap is still shut there (a movie, and no control). No hide for "no control": the
  hands stay in cutscenes and dialogue.
- **Reads and cost** (cpu-lean's plan kept: nothing on frames that cannot draw the hands). View bits plan (hands drawable,
  no tap): battle, menu, movie (3 dword reads, was 1; + the movie player's pointer and byte while the movie flag is set). Full plan (first person, tap on): those + the sphere grid, the
  player, the byte, one 9.7 KB block (VA 0x18663F0..0x1868AB0: both window layouts, both tables, all 8 cursors), the
  bank, the bank's 8 TOMesWinWork windows as one 352-byte read (was 16), the Atel controller (2): 11 calls, was about 21.
  All in FFX.exe's image, so range_readable answers from its region cache.
- **Overdrive minigame diagnostics** (for the Slice & Dice freeze, whose onset is bracketed only to a 10 s window).
  slow_frame, every Present, in battle only (battle_state > 0): the state block 0x133D6F2..0x133D745 (one read: the three
  words, the result bytes, Tidus's zone and cursor), the Wakka byte, and the timer block 0x133F77C.. while a minigame is
  up (or the timer was not off last frame): 2 reads, 3 during a minigame; none outside battle. Output: an `od:` line per
  change of the words or the timer's state with battle_state and the frame (diagnostic profile only; 24 per 5 s, 400 a
  session, the dropped ones counted in the next line; one line when the battle ends with a minigame state last seen; a failed
  read in battle logs nothing and keeps the tracker's state, review N3);
  `overdrive minigame: ...` in the `slow:` onset line; an `od` column in the 600-frame dump (`T4/2` = Tidus state 4, timer
  running; `-` none; two bytes in SlowFr's padding). Tidus's states: 1 loading, 2 waiting for the asset, 3 arming the
  timer, 4 the gauge runs, 5 hit, 6 hit / unloading, 7 timed out, 8 timed out / unloading, 9 done, 10 the result to the
  effect (0x7B0470, then the effect program). `g_od_active` (native, file-static) = the game's own "a minigame is up" test, for later
  users (a VR minigame panel, the parked Auron sequence idea).
- **The follow pick, cross-checked** (diagnostic only): in the field in first person, every 10th scan tick, is the
  picked actor the one at [0x1300788]? A mismatch seen twice in a row is logged with a diagnostic key on (review N5;
  `first person: the follow pick ... is not the game's player actor ...`; one line per 10 s, the same pair again after
  2 min, 50 a session); counts and the last mismatch in F8 in every profile. The
  pick is unchanged until play shows whether the game's actor is the better rule. Riding is not a mismatch (lean2): on the
  chocobo [0x1300788] is the bird (f051) and we follow the rider, attached to it (+0x19C = the bird); such a check counts as
  `riding` and logs nothing (the owner's 4 Nov logs had 9 such lines from the Mi'ihen ride). A pick attached to another
  actor still counts as a mismatch.
- **Not seen in play yet**: a Yes/No box with the tap (the gate should read "a choice box"), the field menu flag over a 3D
  frame, the movie flag with the playing byte (F8: "movie skippable 1, playing 0" after a skippable FMV is B1's case), an `od:` sequence for a real overdrive, the pick check's counts. The exe-analysis.md mislabel
  ("not located") and research exe-battle-ui.md:296 ("message queues") are corrected by the table above.

## Eye jumps: the blink in battle, and facing the enemies (branch `eye-blink`, 5 Nov 2026)

Roadmap 4.1 (scratchpad research/roadmap-design.md), review fixes from eye-blink-review.md. Logic in `ffxvr_eyejump.h`
(pure, unit-tested), managed in `eye_jump.cs`, ABI 44 (`ffxvr_eye_jump_set`, `ffxvr_eye_event`).

- **The blink now works on frames with a UI pass.** It used to blank only the plain backbuffer copy, so every battle frame
  and every field frame with a text box skipped it. It now also clears eye 1's final image (both eyes black); the UI panel
  (command window, text box) stays. **`cut_blink` therefore now also blinks in battle** (seat view, battle camera cuts),
  and the first-person head-switch blink ("eye: from the drawn head") shows with a text box up. Cut blinks are limited to
  one start per 30 frames, and no blink is drawn on frames shown on the virtual screen or the stereo screen.
- **Eye jumps** (first person and third person): a move of the eye base of more than `eye_jump_units` (8, about 0.7 m)
  in one frame blinks for `eye_jump_blink` frames (6; 0 = off, and then nothing else changes either). Not while
  dragging, on the heading's latch frame, after a gap of 3+ frames, on the first frame or on NaN. One start per 30
  frames; a jump inside the window extends a running blink by at most 2 frames. A jump snaps the head smoothing (at the
  next head update) and resets the stick reprojection's heading. `eye:` lines (40 a session) give the move, the frame
  before's move and field / in battle; counts on the `view:` line (diag_verbose=1).
- **Facing the enemies** (`battle_face` 1 body forward (default), 2 where you look, 0 off): on a battle follow change
  (start, swap, summon, dismissal, manual) the heading turns to the centre of the monsters (no turn under 15 deg; with the
  blink). The request waits through the load chain (battle_state 4..0x15, 0x22; the first follow change comes at state
  4, about 2-3 s before the battle is drawn) and is then sent once a monster counts: 11 monster slots (the allocator
  0x793530), Chr+0x10 set (0x79F380's per-Chr gate), actor live, stat_visible and the show vote Chr+0x17 (Chr+0x16..0x19
  are four show votes, 0x797080 / 0x7966C0: the actor is drawn only when all are non-zero; 0x17 = 0 for parts that appear
  later and script-hidden monsters), alive by the game's own target test
  (stat_death Chr+0xDCC == 0, 0x795780 for the target window's masks 0x898330), HP > 0, not at (0, 0, 0), within 400
  units of the followed character. (Until 7 Nov it read Chr+0x17, Fahrenheit's `stat_hide`, the wrong way round: it
  counted only monsters whose show vote was 0: 0 of 10 requests turned on 6 Nov, 4 of 11 on 7 Nov, each toward one monster of three.) Native applies it at the next first-person frame (aged in such frames).
- **The field heading** is kept over a battle (battle_face on only) with the game camera's yaw, and restored at the first
  first-person frame after the battle in the same view and under the same field camera (30 deg). Another camera waits
  (the battle's end can still be drawn with the battle camera for a few frames); the heading re-latches from the game
  camera after 20 such frames in a row, at once after a first-person gap of 3+ Presents (a load passed: a battle ending
  into a cutscene), or after 600 frames.
- Not done (review notes): the manual yaw offset (`g_ovr_yaw`) is not folded into the facing (N4, default 0); the ini
  ranges differ (managed 0-15 / 4-40, native clamps 0-30 / 2-100, N8); third person with anchor smoothing above 0 glides
  past the blink (N9, default 0); a run of over-threshold frames (a battle dash, if one exceeds 8 units a frame) is not
  yet treated as motion (S5: measure first from the `eye:` lines).

## Battle swaps hand the eyes on where the two cross (5 Nov 2026)

The owner: on a swap of the character who has the first-person view, "our character runs far off in the distance with me
watching for a bit, then the view switches"; wanted: "the view switches when they kind of cross paths, almost like they
are tagging in / out". The follow waited for the incoming character to run in and stand still (15 frames), or 300 frames:
about 2 s. Logic in `ffxvr_tag.h` (pure, unit-tested), managed in `bt_tag.cs`, ABI 46 (`ffxvr_bt_tag_step`,
`ffxvr_bt_tag_hold`).

- **The tag.** While a swap-in is pending ("Tidus joined the field", "N left the field"), the horizontal distance between
  the outgoing character (the one followed) and the incoming one is measured every frame. Tag when it is under
  `bt_tag_dist` (6 units; party models are 15-22 tall), or when it has closed in by 2 and then grown 2 past its minimum
  (they passed further apart). The first 4 frames, and any frame before both have moved 1 unit from where they stood,
  are ignored. No tag (the wait as before) when the followed character stays on the field (Tidus joining for someone
  else), when either cannot be read or the outgoing one is already gone, or when they have not crossed by the arrival.
- **`bt_tag`** (F8 > First person / follow, "a swap hands the eyes on:"): 0 "wait until he arrives" (as before; the
  crossing is still measured for the log), 1 "tag and wait on the spot" (default), 2 "tag and ride in with him".
- **Tag and wait on the spot.** At the tag the follow goes to the incoming character and the outgoing one shows again.
  In first person the eye stays on the outgoing character's spot (his position when the swap began: the slot the incoming
  one runs to; a swap that begins during a hold keeps that spot) at the incoming one's eye height, until he arrives and
  stands still (the same arrival rule, one set of constants in bt_tag.cs) or 300 frames from the swap's start; then it
  follows him. The hold is native: `ffxvr_player_set` stays his position and only the eye base is moved, by spot - his
  position (his head by the shared rule, his smoothing). Refreshed every frame; it lapses after 2 frames without one. In
  third person a tag just switches the follow.
- **You see him run in** (review of 1442290, SF). The battle body hide is the radius test around `ffxvr_player_set`'s
  position alone (no other roots are sent outside the field), which hid him all the way in. While the eye is held its
  centre is the spot (nothing stands there: the outgoing one has left), and his own position again once he is within the
  usual hide radius + step margin + 2 units of the spot, horizontally (`tag::hide_centre`). The 2 units cover where he
  stops against the recorded spot and his body's width, so his head is never drawn through the held eye.
- **A tag blinks** through the eye-jump blink (`eye_jump_blink` frames; 0 = none) and snaps the head offset, whatever
  the distance (the jump at a tag can be under `eye_jump_units`). The enemies are faced through the usual battle follow
  change (`battle_face`), measured from the spot.
- **Log:** one `battle tag:` line per swap (100 a session): the mode, the frames until the tag, why (distance or passed)
  and the distance then, the closest distance, and how it ended (held N frames until he stood still, the 300-frame
  limit, interrupted by a summon or a manual pick, ...). Native: `tag:` lines (40 a session) at each tag.
- With `eye_jump_blink=0` a tag does not blink (the eye snaps from the crossing to the spot), as for every eye jump.
- Not done: the held eye bobs with his running head height (smoothed); the release, when he stands a unit or two off
  the recorded spot, is a small step without a glide; while he is away from the spot the body hide's bone capture (the
  VR hands' bones) finds none of his.

## The first-person head rule in one place (5 Nov 2026, review of 9b406b9)

9b406b9 left out sub-positions more than `fp_head_reach` (8) units sideways from sub-position 0 (Wakka's battle model
carries the ball he holds up 14.5 units out, 8-12 units above his head). But the smooth anchor's post-join snapshot kept
its own copy of the old rule and replaces push_player's head on almost every frame, so the view did not change. The rule
is now one function, `FphWalk.HeadPick`, for push_player, the optpos log and the snapshot (the main thread reads the
reach push_player published that frame). The reach applies in battle only: the field, riding, cutscenes and blitzball
keep the old rule exactly; `FphWalk.HeadOf` (the npc-anchor ties, field only) uses it with reach 0. A `head: skipped
index N, X units out, for model cNNN` line (a new model / index pair, 6 a battle) shows it live. The F8 slider ("head,
in battle: ignore points further out than") shows in every eye-height mode.

## 3D damage numbers (branch `dmg3d`, 6 Nov 2026; roadmap 4.2)

Off by default (`dmg3d_show=0` since 8 Nov; on by default from the play2 test until then, key `dmg3d_on`, now ignored): with the panel's own numbers still shown and the words left on the panel, the owner found they added little. A replacement (the flat numbers hidden properly, the words carried too) is on the backlog. F8 main page, under "battle start (glass break)": "3D damage numbers". The damage, heal and MP digits are drawn at each target in the world, in both eyes, with the game's own hop, fade and colour. "Miss" and the other three words (texture 600) and the Doom countdown stay on the panel (v1).
- **Source.** The game's damage-number nodes (pool `[0x11360B8]`, 128 x 0x14 B; list `[0x11360BC]`, link +0x10), read on the main thread at the first job join after the frame counter moved (`h7_join`, always installed). Sg_MainLoop runs the battle ticks (the node update 0x79F5E0), then the battle UI (the queue push 0x79F660 and the draw 0x893290), then the counter, then that join: the pool there is exactly what the game drew this frame. The push's own gates are kept: battle state != 0 and `[0x133C948]` != 0 (0x780CC0 / 0x89B360). 0x79F660's test of battle state 0x1F-0x21 jumps to the head of the node loop (0x79F845): it skips only the Doom countdown, the digits go on being drawn while a battle ends (the design and exe-battle-ui.md had read it as stopping both; review S1). The main loop's three UI-draw gates are kept too (`[0x12FB8FB]` == 0, `[0x12FBBB4]` != 0, `[0x12FB7D0]` == 0 at 0x8214F4..0x821533; review S3): on a frame without the UI nothing is published, the last snapshot holds as the game's panel image does, and the live rule clears it after 3 such frames. No new hook for this.
- **Placement.** `ffxvr_dmg3d.h` (pure, unit-tested against a transcription of 0x79F86A..0x79F973): step = counter - 1, j = step - digit index; damage hops by the table at 0xC424B0 (12 px, then a 3 px bounce), heals rise 1 px per 2 frames, MP 1 px per frame; the fade formula (len - step) << 5 takes over at step 26 (still 0x80 there) and shows in the last 3 drawn frames (0x60, 0x40, 0x20). **Correction to the design:** offsets are PS2 px (512 x 416 over the whole screen), the digit is 34 x 45 UI px (0x8FB840 converts both sizes down to PS2 px), so offsets are scaled by 3.75 / 2.596 to UI px before C3's 14 px per unit.
- **Anchor.** optpos[Chr+0x442] (d7_anchor's rule, with the 30 Hz correction) lifted by Chr+0x4E8, as 0x79F380; Chr+0x442 = 0xFF: the game's 0x794AB0, ported (7 Nov, below); Chr+0x442 above 25, d7_anchor's fallback for an empty sub-position, or a failed port read (optpos[1]): stand-ins, counted, drawn, never hidden from the panel (review N2). Then `dmg3d_lift` units up, pulled toward the head by C3-13's rule, and (7 Nov) pushed out along the ray to 0.6 m from the head when nearer (not in the diorama).
- **0x794AB0 port (7 Nov, logs finding 3).** All 80 numbers logged on 6 / 7 Nov had Chr+0x442 = 0xFF, and optpos[1] lifted by Chr+0x4E8 put them 30-490 PS2 px above the game's. 0x79F380 calls `0x794AB0(chr, &v)` for them and subtracts Chr+0x4E8 from y after either branch (0x79F4B8..0x79F505). 0x794AB0: actor+0x10C != 0 (the override matrix): its translation (+0x30..0x38), done; else 0x796EA0: Chr+0x43F != 0xFF with an actor (0x795080): optpos[Chr+0x441] of that actor (0x8354F0); else the actor's position +0x0C..0x14 (0x82AC90: the root on the ground). Then `[0x112C9DD]` and stat_death (Chr+0xDCC) == 2: y = Chr+0xF70; Chr+0x505 (flying) or `[0x112C9CE]` (a battle-wide flag from the btl file, 0x783E20): y += actor+0x4F0 x Chr+0x3A4 x 0.5 (`[0xB92220]`). Managed reads (`dmg_ab0`), native decides (`ffxvr_dmg_anchor`, ABI 50; `dmg::anchor_794ab0`, unit-tested). Replay of the 80 lines (scratchpad fixb_t/dmg_replay.py; no camera in the logs, so the screen point is estimated from the cursor lines' projections of the same chr): 15 of the 62 non-attached numbers land within 3 px of the game's point (the two whose cursor sample provably had the number's camera: 0.8 and 0.9 px, from 72 and 50), where today's anchor has 4 of 70; the rest scatter by the camera that moved between the cursor sample and the number (median |dy| 92 -> 21 px). The next log's per-number lines measure it exactly ("0x794AB0 port, the actor's root").
- **First person** (C3's rule, `c3_is_self`: fp_mode 1, headset on, the actor we look out of): that actor's digits go to C3's self spot, 0.5 m ahead along the view's horizontal forward and 0.35 m below the eyes (`bb[6..8]`, no lift, no pull), instead of our own eyes (review S2). With the hide on they leave the panel like every other target's: they are drawn, just in front of you, and one rule for all targets keeps the panel from showing half of a hit's numbers.
- **Size.** C3's 14 UI px per unit times `dmg3d_scale`, with the digit's height kept between 1 and 4 degrees. The base size is 3.2 units tall: about 1.5-2 degrees at the diorama's usual 100-140 units (94 units/m), close to the monitor's; in first person (11.4 units/m) it is 28 cm, which would pass 4 degrees nearer than 46 units (4 m), so it is capped there instead of filling the view.
- **No depth test** (as every marker): the game draws its numbers over everything, and the anchor is on or inside the body.
- **Drawn** in the marker pass at Present (`dmg_build` once per Present from eye 1's billboard frame, appended to each eye's marker draw; battle.dds from C3's held atlas, whose search now also runs in battle while dmg3d is on).
- **Lifetime.** C2's live rule on the snapshot's game-frame stamp (within 3 frames, not older than the switch-on or the battle start, 180-Present backstop): the digits come and go with the game's nodes.
- **The flat numbers** stay on the panel by default. `dmg3d_hide2d=1` (ini, its hook installed at launch only then; off by default) hooks the queue draw 0x893290 (single call site 0x89B3CB; prologue `57 E8` checked first): the detour records there and, while native draws the digits (`ffxvr_dmg_skip`, decided beside C2's skip), takes exactly those entries off the queue, only when every entry of this frame matches the placement predicted from the nodes; on any doubt nothing changes. Digits at a stand-in anchor, or past the 64 we draw, stay on the panel.
- **Logs.** One `dmg3d:` line per new number for the first 40 (value, kind, target, the game's anchor and screen point against ours through the game's camera, where the quad went, its distance and size); a 10 s `dmg3d:` line in battle (snapshots, cut walks, stand-in anchors, the queue check: our predicted entries against the game's own, still in memory after the draw).
- ini: `dmg3d` (0), `dmg3d_scale` (1.0, 0.3-3), `dmg3d_lift` (0, -10..20 units), `dmg3d_hide2d` (0). ABI 47: `ffxvr_dmg_walk`, `ffxvr_dmg_queue`, `ffxvr_dmg_set`, `ffxvr_dmg_cfg`, `ffxvr_dmg_skip`, `ffxvr_dmg_status`; ABI 50: `ffxvr_dmg_anchor`.

- ini: `dmg3d` (0), `dmg3d_scale` (1.0, 0.3-3), `dmg3d_lift` (0, -10..20 units), `dmg3d_hide2d` (0). ABI 47: `ffxvr_dmg_walk`, `ffxvr_dmg_queue`, `ffxvr_dmg_set`, `ffxvr_dmg_cfg`, `ffxvr_dmg_skip`, `ffxvr_dmg_status`.

## Render and performance fixes from the 6-7 Nov logs (branch `fix-render`, 7 Nov 2026)

Sources: scratchpad research/logs-1107-perf.md and logs-1107-behaviour.md. Pure parts in `ffxvr_render.h` (and `ffxvr_cpu.h` for the
reads); scratch tests in render_chk/tests.

- **The flat-screen rule needs the painted layers to dominate (behaviour E1/E2).** A 3D cutscene in kino0400 (s2 07:25:17) went to
  the virtual screen for 9.3 s on 2 painted-layer (dummy camera) uploads beside 349 real 3D camera uploads: any layer upload made
  the frame a "painted-backdrop room". Now (`rnd::ModeRule`) a room is entered when there are at least 4 layer uploads, or the
  layer uploads x 4 >= the real 3D uploads (or there is no real 3D upload), and kept while 4+ layers or layers x 8 >= real (the
  hysteresis band); 10 frames in a row outside the band, or a frame with no layer at all, end it. The 2D verdict still needs 2
  frames before the screen and 3D comes back at once. Expected: E1 stays 3D (2 layers, 2 x 4 < 349); every room of 4+ layers
  (the title-like frame's 28, a hut, the Djose side rooms if they are painted) stays flat whatever its character count.
  - Review B1: without the 4-layer floor, a painted room with many 3D characters (real > 4 x layers, plausible in the Djose side
    rooms) would have come up in 3D with the painting glued to the face. Replaying all 141 logged mode lines of both sessions
    through the new rule (render_test): only E1 changes.
  - First person, the hands, the stick remap and the stereo screen stay off whenever any layer is drawn, this frame or the one
    before (`ModeRule::any_layers` -> `g_backdrop_room`), as before the change: only the screen decision uses the ratio. So
    E1's scene is 3D now, but in the seat view while its 2 layers are drawn.
  - **The mode lines say why.** `mode -> SCREEN|3D at frame F: ... | why: <reason with the counts and the rule>`; reasons: a real
    3D camera, 3D with layers that do not dominate, no real 3D camera, too few scene draws, a painted room, screen_mode 1 / 2,
    a battle transition's pin, Fahrenheit's save screen, the slow-GPU guard. While the mode stays, a new reason held for 30
    frames gets `mode: still SCREEN at frame F, now because: ...` (40 a session per reason): a room entered through a load is first
    logged as "no real 3D camera" (the load frame), then as the room with its counts, which is what confirms or rules out the
    Djose side rooms' 126 s (E2).
  - If a painted room with many characters now shows in 3D with the painting stuck to the face, the line gives its counts
    (`k_bd_enter` / `k_bd_stay` in `ffxvr_render.h`).
- **The exe's image runs are kept across frames (perf finding 5).** s2 paid about 0.4 ms of render-thread time a frame on two
  4-byte reads into one 25 MB run of FFX.exe's image (0x0116B994 through ffxvr_safe_read, 0x0117ED3C through range_readable):
  both caches were cleared at every Present and kept the run from the *queried* page, so the lower read never hit the higher
  one's run. Now (`cpu::ImageKeep`, render thread, fast_reads on and the working-set check on): every image run a query answers
  is kept across frames (a lower query of the same run supersedes it), and a miss in the image first queries its PE section's
  start, once a section, so the run is learnt from its start where it starts there. **Safety:** a run kept from an earlier
  frame is never trusted as it stands. A read in it is answered only when QueryWorkingSetEx says every page of the read is
  resident and readable now (lean2's rule: Valid, not bad, range_readable's protections; nothing uses it before the start-up
  self-test passed), which a page freed, decommitted, made no-access or turned into a guard page since cannot pass; the proven
  pages last as long as a query's answer did (the frame, or the 50 ms bound); a failed plain copy through a kept run drops it.
  The start-up self-test has two more steps (review S3): the test page made PAGE_NOACCESS must not answer Valid with another
  protection (else the whole working-set check is off), and the exe's header page, read first, must answer resident and
  readable, read and asked once more if a trim came in between (else only the kept runs are off, `kept exe runs ... OFF: <why>`
  on the cpu: line). The pages read through kept runs are remembered,
  so the next frame proves them all in one call: about one QueryWorkingSetEx call a frame instead of two 25 MB scans. A refused
  proof drops the run and the read walks as before; a failed kernel copy, fast_reads=0 or the check off drop every kept run.
  Only FFX.exe's own image is kept (heap and other modules keep the per-frame rules). Model (render_chk keep_test, s2's shape
  and the owner's measured costs): 350-366 us -> 1.6 us a frame. The `cpu:` line adds `kept exe runs N (of S sections; safe
  reads through them, proofs a frame over P pages, pages refused a frame, calls unanswered, section probes)`.
- **The second 3D camera (behaviour G).** kino0800 (s1 16:27:41-51) had about 70 uploads a frame whose view differs from the
  frame's main (tick) camera, and the aeon in battle 1 about 0.8 a frame. What the code shows:
  - Mirrored (reflection) cameras are already rejected (the inverse view must agree), the ShadowManager's light cameras are left
    alone (`light_proj`, shadow_cam 1), the painted layers' dummy camera has its own path. What is left is a perspective camera
    whose inverse agrees: a scene camera. The engine has one: renderer+0x654, the frustum for node list 3 (probably the dynamic
    / skinned objects; exe-analysis.md). In a moving cutscene (kino0800's projection zooms from P0 2.87 to 1.93 over 15 s on the
    pan-scan lines) the two can differ by a frame.
  - The override is relative to each upload's own view (seat: its own position under the seat heading; first person: the
    player's eye for every camera), so those draws need the head pose: leaving it off would put about 70 draws a frame (the
    characters, if node list 3 is them) in the game's flat view, head-locked, with no stereo. That is not done.
  - What *was* wrong: `build_override_view` wrote the frame's main-camera state on every eye-1 call, so the last camera of the
    frame won: `g_yaw_cam` / `g_move_delta` (the first-person stick remap's heading), the VR hands' base (`g_hb_*`) and the
    first-person heading latch. Now only a call with `g_ov_publish` set writes it: write_camera for an upload of the frame's
    main camera, and hands_draw_frame for the main view. ffxvr_project, the markers and eye 2 only read (review S1). The
    per-frame filter steps (eye jumps, the stickrep record) stay on the frame's first 3D upload (`advance`), whichever camera.
  - **Which camera is main (review S2, round 2 R2-1):** tracked by its view, not by its upload order. `rnd::MainPick` gives each
    distinct view of the frame a slot; an upload's slot is main when its view lies within a tracking gate of the previous
    frame's main view (4 units and 4 degrees, widened by twice the camera's recent per-frame motion, each frame's motion capped
    at 25 units / 10 degrees), and a later slot nearer that view takes over (the nearest wins). With no slot in the gate yet (a
    cut, the first frame), the previous frame's busiest slot (most uploads into the scene target, then most uploads) is main
    from its second upload, or else the first slot to reach 8 uploads, so a one-upload stray camera never qualifies. The next
    reference is the gate-picked main view, else the busiest camera's. The reviewer's simulation (a stray uploading first in 11
    of 16 frames) now gives no stray pick and no frame without a main upload. hands_draw_frame uses the main view (`g_main_v`).
  - **The log line that confirms it:** every 10 s with another camera, `view: another 3D camera this window: N uploads (bound
    to the scene target K, orthographic near projection L), up to X units and Y deg from the frame's main camera, first B bytes
    with P0 (main P0) -> verdict | the main camera's own uploads M (per frame); main = a later camera than the frame's first in
    F of T frames, no main upload in K [(the pick flipped within the window)]` (120 a session, not verbose-only). "Within 5 units / 3 deg, into the scene target" = the
    same camera a moment apart (head pose right); "partly into another render target" = an off-screen pass (then the head pose
    is wrong for it: a follow-up would leave those uploads alone); "LIGHT-LIKE" = a shadow light the light-camera rule missed
    (e.g. under 1872 B; same follow-up). The render target is the one bound when the constant buffer was uploaded.
- **The GPU budget in F8 (perf finding 2).** The heavy scenes are GPU-bound: above a span of about 13.5 ms the 120 Hz extra
  frames collapse. The planned drop (perf-plan S1: the game at 2560x1440, then S3 `msaa=2`) is a game setting
  (`tools/game-settings.ps1 -Profile vr`, GameSetting.ini) plus the existing `msaa` key, so nothing new is stored and no default
  changes; setup.md "GPU-bound scenes" gives the steps. `ffxvr_perf_status` (same export, ABI 49) now returns a second line:
  `GPU budget: span X ms (max Y) against the ~13.5 ms knee[: OVER] | extra frames N/s, dropped (slots repeated) N/s, fresh
  frames late N% | game renders WxH, scene MSAA Nx (msaa=M), eye image WxH`, and when over the knee a third line naming the two
  settings to try. Rates come from the last 10 s sr window (`rnd::gpu_budget_rates`), the span from the perf window. The overlay
  reads 768 bytes (an older ffxvr.dll's 256 bytes show the first line and the start of the second).
- **Virtual Desktop's late-session slowdown: logging only (perf finding 3).** VDXR's xrEndFrame grew from 0.1-0.2 ms to 4-8 ms
  after 25-37 minutes in both sessions, flat screen and FMVs included (its `waitForAsyncSubmissionIdle`: the previous frame's
  hand-off to the streamer). No fix here; what was added so the next session can tell the causes apart:
  - `rnd::EndTrend` per headset session (this mod's view: the headset active): the extra frames' End time per 10 s sr window
    (an extra frame does nothing of ours but xrEndFrame), its average over the session's first 5 minutes, the session's maximum.
  - The first time a window passes 1 ms, and again at 3 ms, one line (not verbose-only): `xr: the runtime's xrEndFrame is
    slowing: the extra frames' End X ms ... (Y ms in this headset session's first 5 minutes), a fresh frame's End, extras/s,
    photon latency, display Hz, headset session N min (session K of this run)` with the owner's next steps. With every `perf:`
    line: `xr: End trend: headset session N min, extra frames' End X ms (first 5 min Y, max Z)`. F8 shows the same as a third
    line, so the onset can be spotted in the headset.
  - What the runtime does not give: VDXR exposes XR_FB_display_refresh_rate only as a mock (the fixed refresh), no performance
    metrics extension; its ASW state only doubles the predicted display period (already shown as "half rate"). The streamer's
    encode / network / decode figures are only in Virtual Desktop's own overlay; VDXR's ETW provider (its
    `scripts/Capture-ETL.bat`) splits the wait into `OVR_WaitToBeginFrame` and `OVR_EndFrame` on its submission thread.
  - Owner steps: fixes/fix-render-impl.md (scratchpad), section 6.
- **Spell hitches are FFX-60fps' (perf finding 1): nothing changed here.** Every spell end with a hooked magic overlay freezes the
  picture 0.1-0.6 s (on the game's main thread, so no Present and no extra frames: the flush gate is held), and a hooked overlay's load
  0.1-0.5 s. fps60.log's own lines show it: `magic_0605.dll unload: 9 callbacks 563.40 ms (slowest MAG-022 412.36 ms)`; hookless
  unloads take 0.00 ms. Cause: the MAG-016 / 017 / 022 rows hook and unhook the overlay through MinHook, and each `MH_EnableHook`
  / `MH_RemoveHook` runs `Freeze()` (a system-wide Toolhelp thread snapshot, then suspend + context of every thread of the
  process): ~100-150 ms a call on this PC, 1-4 calls per DLL event. Suggested upstream: no freeze for magic DLLs (no thread can be
  in an overlay at its load or unload notification; at unload nothing even needs restoring), or one `MH_ApplyQueued` per event
  (queue the enables / disables), or a per-process thread walk in MinHook. FFX-60fps is Apache-2.0 (MinHook BSD-2-Clause): a
  local patch is allowed (modified files marked; licence kept when distributed). The issue draft with the evidence:
  scratchpad research/fps60-spell-hitch.md.

## Bushido: strike the targets (branch `od-targets`, 7 Nov 2026; ABI 52)

A second scheme for Auron's Bushido beside the slashes and punches: `od_gest_auron` = 0 off / 1 slashes and punches (unchanged)
/ 2 strike the targets, an F8 radio row ("Auron's Bushido:"). The logic is the pure header `src/native/ffxvr_odtarget.h`
(its unit test and mutants are kept outside the repo, as the other overdrive suites).

- **The game's Bushido row and our targets** (the owner, 10 Nov 2026: "it kind of clashes with the 2d original bushido ui").
  Why: the game's UI pass is captured into its own texture and submitted as a head-locked OpenXR quad layer (the UI panel)
  above the projection layer, and our targets are drawn into the eye images (the projection layer). So wherever the panel's
  Bushido row and a plate overlap, the row covers the plate, at the panel's own distance. What the game draws (our exe): the
  battle UI 0x89B360 calls 0x892740 at 0x89B411 every UI frame; while Auron's state 0x133D6F4 is 3 or 4, 0x892740 draws one
  button icon per remaining input (from the progress 0x133D6FC to the length 0x133D700, the sequence at [0x133D704]; 0x9029B0,
  30 units apart) and a label (0x78F8F0(0x3011), 0x905290 / 0x9016B0). The shared countdown is drawn elsewhere (0x8955E0, while
  the timer word is not 0). The fix: a detour on 0x892740 (managed, main thread; ini `od_targets_hide2d`, on, hooked at launch,
  its first bytes checked) skips the call while native says a strike-the-targets window's targets are drawn in the headset
  (`ffxvr_odt_hide2d`, decided each Present, published interlocked, latched per game frame). The countdown stays; no other
  overdrive's UI is touched; slashes and punches (mode 1) keep the game's row.
- **Arrows only** (the owner, 10 Nov 2026: the symbols were "less than ideal"; every plate has a cut direction now). Each plate
  shows one big bold arrow pointing its cut direction (A down, B right, X left, Y up, L1 left, R1 right, the d-pad itself),
  as large as fits in the face (the medallion's hole), with the colour's glow, a strong dark outline and a bright core; the
  glow pulses with the chevrons, which stream along the arrow (at full strength) and on along the guide beyond the plate. The
  spot and the colour still tell the kind. F8 "show the button letter (small)" (ini `od_targets_badge`, off) adds the button's
  letter on a dark disc at the rim's bottom (A / B / X / Y, L1 / R1; none for the d-pad). The art pack may hold `arrow.png` (a
  white or grey glowing arrow pointing up, 1024 x 1024; `target_art.py --arrow`): kept in a texture of its own (512 with mips,
  1.3 MB), turned to each direction, multiplied by the colour (so it must be white or grey: a coloured arrow would darken or
  shift each symbol's colour), over a black silhouette of itself (1.08 x, 0.7) and the colour's glow; without it, or
  odd-sized, the procedural arrow.
- **The row's first frames** (review R9-N3): the game draws the row in Auron's states 3 and 4, and a round opens only at state
  4 with the timer running. So the row is also hidden from state 3 whenever a targets round is coming (the targets scheme on,
  a battle, the headset and VR pad live, the 3D view, no other overdrive; `odt::hide_row`). In state 4 without a round (the
  gestures cannot inject) it stays, as it does in practice, in slashes and punches and in the other overdrives. The detour's own
  work is wrapped (R9-N4): a failure there draws the row (counted as "faults" in the F8 tooltip).
- **The row still showed (the owner, 461b1bf), and two ways to hide it now.** The investigation ruled out native's answer and
  any other drawer of the row, which leaves the detour on 0x892740 not running. Nothing in core.log says why, and every
  address we use is base-relative: FFX.exe loads at 0x10000 there, not 0x400000. FhCallLocation(module, offset), FhUtil.get_at
  and native's GetModuleHandle + (VA - 0x400000) all use the real base.
  - **The detour, checked.** 30 frames into the session (after Fahrenheit's commit) the managed side asks Fahrenheit for the
    hook's chain entry (an inserted hook has one) and reads 0x892740's first bytes (MinHook's jmp, E9). It logs one line,
    `odt: Bushido row detour: inserted at 0x... (bytes E9 ...)` or `NOT inserted ...`. If the hook is not in, it tries one late
    insertion, which Fahrenheit allows on a function with no hooks applied, and logs the result.
  - **A call-site patch of our own.** At init (ini `od_targets_hide2d`, `ffxvr_odt_row_patch`), native checks the battle UI's
    5-byte `call 0x892740` at 0x89B411: the byte E8, and a target of 0x892740 at the real base. It then points the call at a
    native stub, which skips the call while the row is to be hidden and otherwise calls 0x892740 (and so the detour too, when
    inserted).
    - Only the rel32 is rewritten, in one interlocked store inside a cache line, under VirtualProtect. This happens at module
      init, and never while the battle UI is alive ([0x133C948] non-zero): the patch is race-free only because no battle has
      started.
    - The DLL is pinned (GetModuleHandleExW PIN), so the stub outlives every call. Nothing is restored at unload: a restore
      could not cover a thread already inside 0x892740.
    - Other bytes there (another mod, another exe) leave it alone, with a log line.
    - It does not depend on Fahrenheit's hook commit. Either mechanism alone hides the row.
  - **Why not the 2D draw skip.** The row's icons go 0x892740 → 0x9029B0 → 0x8E6030 → 0x8E6070 → 0x903EE0 → the 2D quad
    submit 0x63EEE0, all on the game's main thread. There they are recorded into the command lists that the game's render
    thread replays later. So at the D3D draws, where our skip ranges work, no return address inside 0x892740 is on the
    stack. The sprite signatures (the button atlas, the quad sizes) are shared with every other button prompt.
    - Skipping at 0x63EEE0 by its caller would need another detour, of the same kind as the one that failed, plus a stack walk
      per quad.
    - The call site is the narrowest point that is ours alone.
  - **Diagnostics.**
    - Native counts each question by Auron's state (3, 4, other) and answer, the stub's and the detour's apart.
    - Managed pushes the detour's install state and counts each frame (`ffxvr_odt_row_stat`).
    - Each Bushido window's `odt:` line in ffxvr_diag.log ends with `; row: hook <state>, detour calls N, hidden H, faults F;
      asked st3 a/b st4 c/d other e/f; call-site patch <state>, calls P, hidden Q, asked ... (answers 0/1)`, with counts since the last
      window line. "NEITHER RAN this window" flags a window in which neither path was called.
- **Textured medallions** (the owner's concept, 10 Nov 2026: engraved silver medallions with a swirling glowing disc and a
  bright symbol, each symbol its own colour). From a local art pack when present (next section); without it the flat plates as
  before. Each plate, back to front: a soft halo ring (tinted, pulsing, additive), the swirl disc in the frame's hole (tinted,
  turning 0.15 turn/s, darkened to 40 % at the centre under the symbol), the engraved frame (its own metal colour), its glow
  channel (tinted, pulsing a little, additive; red on a deflect), the arrow in the hole (arrows only, above: the pack's
  `arrow.png` when present, else the procedural arrow, near-white over a dark outline over the colour's glow, scaled to fit
  inside the hole). The chevrons and guide line stay (they stream over the arrow at full strength). The frame's outer edge is the plate's edge (24 cm at size 1), its hole 0.48 of the radius. A cut: the
  halves are the medallion's own layers clipped by the cut (the same UVs), the back face once a half has turned away (with the
  current tumble, at most about 63 degrees in its 0.45 s, it rarely does), the rim's edge strip on the side, the cut's face hot
  white; a burst (0.2 s) and sparks (0.3 s), tinted, expanding, additive. The preview: the textured look at 40 %, dimmed, no
  halo. Colours per symbol (the flat plates use them too): Up blue, Down teal, Left green, Right cyan, A blue, B orange, X cyan,
  Y gold, L1 violet-blue, R1 sea-green. The comfort rules are unchanged (everything in the plate's plane or behind it; the
  halves' motion as before).
- **The targets' own draw.** The marker pass's shader gained a second pixel shader (`pst`, compiled at run time from the same
  source with D3DCompile, ps_4_0, as the rest of the pass) and two texture slots (t1 atlas A, t2 atlas B, sampler s1: trilinear,
  clamp, mips 0-6). The markers, damage digits and overdrive visuals draw exactly as before (ps, straight alpha); the targets
  follow in their own Draw with pst and a premultiplied blend (ONE, INV_SRC_ALPHA): a vertex's t.z picks plain (0), atlas A (2)
  or B (3), + 4 additive (alpha 0 after premultiplying). Without pst (a compile failure, logged) the targets draw with ps in the
  same draw as before and the art is off.

## Bushido target art (local pack)

The textured medallions use an art pack. **The art is not in the repository and not in the mod's zip**: it is the owner's
own AI-generated art (Google Gemini, upscaled with Upscayl), offered with releases as a separate optional download,
`ffxvr-target-art-<version>.zip`, laid out like the mod's zip (`ffx-vr-<version>/mod/ffxvr/art/targets/`): extracted into the
same folder as the mod's zip, `tools\install-mod.ps1` installs it with the mod (docs/release.md). The repository
has only the loader, the cutting script and these notes; `assets/art/` is ignored, and the release script refuses any `art\`
folder in the mod's zip. To make your own pack:

1. **The sheet.** Generate one image, a 3 x 3 grid of cells on pure black, straight-on, no text, at 2048 px, then upscale 2x in
   a faithful (not creative) mode; save as PNG. The cells, row by row:
   1. an ornate engraved silver medallion frame, no glow, an empty black centre;
   2. the same frame with a white glow channel;
   3. a soft white halo ring;
   4. a white or grey swirling energy disc;
   5. a second swirl (unused);
   6. the medallion's back face;
   7. a seamless side strip of the rim;
   8. a white burst;
   9. white sparks.
2. **Cut it:** `python tools/target-art/target_art.py sheet.png out` (Python 3 with numpy and Pillow). It writes eight PNGs:
   frame, glow, halo, swirl, back, burst, sparks (1024 x 1024 RGBA) and edge (1024 wide), and prints the frame's measured hole
   and edge (the mod assumes 0.465 and 0.960 of the half-size) and the swirl's disc (0.925).
3. **Optional, the arrow:** generate one image of an ornate glowing arrow pointing UP on pure black (a metal outline, a white
   glowing fill; white or grey only, since the mod multiplies it by each symbol's colour), upscale it, then `python tools/target-art/target_art.py --arrow arrow-image.png out`: a square on its content
   with a 4 % margin, 1024 x 1024, alpha from the brightness, the colour kept (the metal stays grey). Without it the mod draws
   its own bold arrow.
4. **Put them** in `<the mod folder>\art\targets\` (eight files: frame.png, glow.png, halo.png, swirl.png, back.png, edge.png,
   burst.png, sparks.png; and arrow.png if made). The mod's build and install never touch that folder.
5. **F8 > Controllers:** "target art (local pack)" (ini `od_targets_art`, on by default) and its status line, e.g. `art pack:
   loaded 8 of 8 + arrow.png (atlas 2048 + 1024 + arrow 512, 28 MB; ...)` or `art pack: frame.png not found at ..., flat plates`.

How the mod loads it: at the first targets drawn, a worker thread (as the hand packs' loader) reads each file, decodes it
with WIC (Windows' own codecs), checks its size (1024 x 1024; the edge strip 1024 x 32..512), packs atlas A (2048: frame,
glow, swirl, back at 1024) and atlas B (1024: halo, burst, sparks, edge at 512, shrunk 2:1) and builds each one's full mip
chain (alpha-weighted), and arrow.png, when present, its own 512 texture (shrunk 2:1, with its chain); the render thread then
makes one immutable texture a Present (26.7 MB of VRAM in all, 28.0 MB with the arrow) and draws the flat plates until A and B
are made (the arrow's texture failing costs the arrow image only). A load the box was turned off during is dropped when it ends. Anything missing, odd-sized
or failing: nothing is kept, the flat plates, and the reason in one `odt:` log line and in F8; the success line gives the
background time and the textures' time. Turning the box off releases the textures.

- **Two places** (the owner, 9 Nov 2026: "two places where the icons can appear"). The single spot became two, at the same
  distance and height, 0.36 m apart (`od_targets_spread`, F8 "target spacing (m)", 0.25-0.6 between the centres, never less
  than the plate's width + 2 cm, so the plates never overlap from either eye at any setting): the LEFT spot holds the four d-pad
  arrows, the RIGHT spot A / B / X / Y and L1 / R1 (L1 and R1 were first planned for the left spot; the owner moved them to the
  right with the face buttons). Every symbol is now cut in a direction with the same 65-degree path rule and deflect handling
  as the arrows: the face buttons as they sit on the Xbox pad (A down, B right, X left, Y up; the glyph follows the map in use,
  so the direction follows the button the game wants), L1 left, R1 right. A letter or the L / R tab stays the main symbol, with
  the same sliding chevrons and guide line in the plate's colour; so the letter stays legible, a chevron is at 20 % alpha and
  65 % size while any part of it is over the face, and full again by the rim's middle and beyond the plate (a lane around the
  letter); on an arrow the chevrons run over it at full strength (they point the same way). Stabs no longer cut anything. Only
  the current plate is hittable, as before; the preview (one, the next symbol) sits just above its own spot, so the player sees
  which side is next. The pips and bar sit between the spots, pushed back to the spots' distance (never nearer than a plate).
  A follow-through from one spot into the other must not cut a plate that has just appeared there: the hand that cut cannot
  cut at the OTHER spot for 0.25 s (its same-spot re-arm is unchanged: out of the sphere it cut, or 0.2 s), the other hand can at
  once. ABI 54 (`ffxvr_odt_set` gains the spread).
- **What the player sees** (the "one big target" redesign, 8 Nov 2026; the first build's fly-in and two lanes failed the
  owner's play test: hard to see and unclear what to do). One plate at a time in one fixed spot, 0.50 m ahead and 0.30 m below
  the eyes, latched from the head's yaw at the open (blended toward the body). The plate is an octagon prism about 24 cm across
  (the size slider scales it), with a soft dark halo behind it (about 1.4x the plate, alpha 0.6), a bright saturated face
  (arrows cyan, A / B / X / Y amber, L1 / R1 violet), a thick white rim and a thick near-black symbol, and a gentle pulse
  (scale 1.00-1.04). The arrow points the way to cut: three chevrons slide along it, looping every 0.9 s, over a faint guide
  line reaching 12 cm beyond both edges; letters and L / R had no chevrons (any direction) until the two places (above). The next input shows small (40 %)
  and faint above and behind the plate, never hittable. When a plate is cut the next materialises in the same spot in 0.12 s
  (scale and alpha, no travel); the first materialises at the open. A wrong direction flashes the rim red and enlarges the
  chevrons. Thin progress pips (the game's own progress; a pip flashes when it moves) and a timer bar sit under the plate.
  Each hand holds a single-edged, slightly curved blade with a small round guard (left gold, right steel-cyan) that brightens
  above the speed threshold and leaves a short ribbon trail. A cut splits the plate along the stroke; the halves part at
  0.8 m/s, fall, spin and fade (450 ms), with sparks and a bright line along the cut; they never move toward the eyes (the
  near-eye fade still applies). Nothing is drawn nearer the eyes than the plate. The look is this mod's own: no game assets,
  no console colour scheme.
- **Contrast** (the owner's second test, 8 Nov 2026: "an improvement"; then more contrast, bolder and brighter symbols). The
  plate's face is near-black (alpha 0.92) inside a thick bright rim in the plate's colour (arrows cyan, A / B / X / Y amber,
  L1 / R1 violet; 0.36 of the radius, twice the first build's) with a soft glow ring outside it (to 1.22x); the dark halo stays
  behind. The symbol is the brightest thing in view: near-white tinted toward the plate's colour, full alpha, strokes 1.6x the
  first build's, each symbol scaled as large as fits inside the rim (its extent measured once), over a slightly larger, dimmer copy
  of the same strokes in the plate's colour as a glow. The chevrons take the symbol's tint, thicker (0.08 of the radius), bright
  for longer in their loop. The preview is brighter (0.65) with the same look, still small and dimmer. The cut halves have the
  plate's look (a dark face, bright coloured edges). The marker pass blends straight alpha only (no additive or premultiplied
  output), so the glows are dim layers drawn under what they light. Worst frame (any symbol and preview, a deflect, six cuts
  with flash rings, both blades): 3150 of the 4608 vertices.
- **More time** (`od_targets_time`, F8 "Bushido time (x, strike the targets)", 1-3, default 1). The game's countdown (our exe):
  each logic frame while the timer word 0x133F77C is 2, 0x891AC0 adds 1.0 to the f32 frame count 0x133F788 and recomputes the
  seconds left 0x133F790 = total 0x133F78C - count / 30 (25 in the 50 Hz mode), clamped at 0; Auron's state 4 (0x890FE5)
  then goes to 7 when the seconds left are not above 0. The seconds left are derived every frame (a write to them would be
  overwritten before the test), so the lever is the frame count: each Present with a fresh read, in an open strike-the-targets
  window with the timer running, the count is written back as prev + (now - prev) / mult (prev: what we wrote or saw at the last
  such Present), so a logic frame counts 1 / mult. A count that went down (the game resets it at a window's end and a battle's
  start) or a Present without a fresh read starts afresh with no write; x1 never writes; nothing is written in mode 1, another
  overdrive, practice, or once the window closed (state 5 / 7, the battle over, the headset off or the gate past its grace end
  the round). The only other writers of the count are that reset (0x897F00) and the battle's start (0x89B780); its only reader
  is the tick itself. The game compares the count after its own +1, before our write, so a window is up to mult - 1 frames
  short of total x mult (67 ms at x3). The bar under the plate shows the stretched seconds; practice's bar is 4 s x mult.
  **Threads (review R6-S1):** the game's logic runs on its main thread and the Present on the render thread (the first build
  wrongly assumed one thread), so the game may tick, or reset the count at a window's end, between our read (od_frame) and our
  write. The write is an interlocked compare-exchange of the float's 32 bits against the exact bits read: it lands only if the
  count is unchanged; otherwise nothing is written and the stretch starts afresh at the next fresh read. So a reset is never
  overwritten with a window's old count (which no window's open would clear: the next overdrive's timer would start short), and
  a tick in between is not lost. The count is 4-byte aligned in .data; a read torn across a tick can only fail the exchange.
  This is the mod's first write into the game's data (one f32 in .data, guarded).
  **Damage (review R6-S2):** the game turns the time left into Bushido's damage bonus: at the close (state 10) it saves left and
  total for Auron, and the damage path multiplies by (left + 2 x total) / (2 x total), up to x1.5 with the whole time left. The
  stretch leaves more of the game's seconds at a given real finish, so the same play hits harder (a 4 s window finished in 2.5 s:
  x1.19 at x1, x1.29 at x1.5, x1.34 at x2, x1.40 at x3). The F8 tooltip says so; whether to keep it is the owner's call. The
  window line logs the game's count against the real frames (the stretch's effect), the writes landed and refused, and the
  game's left and total at the close with that damage factor.
- **What counts.** A swept test of the blade (8 points, sub-stepped by rotation) against a 15 cm sphere at the plate; 1.2 m/s
  at the contact. Only the current plate can be cut (no lanes, nothing ahead of time). An arrow is judged on the stroke's path
  in the plate's plane over the pass (at least 7 cm of it, from the first contact sample or the last turn back), which must lie
  within 65 degrees of the arrow; a new pass starts afresh, and so does a turn back inside the sphere (a wind-up from a ready
  pose already in the 30 cm sphere would otherwise have to be cancelled by the chop first: review R5-S1). Across a 30 / 60 Hz test of noisy strokes, a stroke at
  right angles is accepted 3 % of the time (bound 5 %), the opposite way never, the right way always. Buttons took any
  direction (stabs too) until the two places; now they follow the same rule in their own direction (right angles 3-4 %). One hit per plate; the striking hand re-arms when its blade leaves the sphere or after 0.2 s, the
  other hand can cut the next plate at once. A wrong direction deflects and sends nothing; a deflect shows 0.3 s after its
  pass ends and is dropped if the plate is cut (or done) meanwhile (a raise through the plate before the chop), and a pass that
  began less than 0.15 s after its plate became current never deflects (a two-handed grip: the second blade is already in the
  sphere when the next plate appears) (review R5-S2). One stroke may cut two plates that way (correct input): the second's
  halves show with a ring flashing out from the spot (R5-N1). Pose snaps and spikes (rest around
  a big step, or a step straight back) and jumps over 8 m/s at the grip are skipped.
- **What is sent.** Closed loop on the game's progress: a press goes out only when the slot is free, the plate at the game's
  progress was struck, and the player's last new button press has settled: 80 ms AND two Presents with a fresh read of the game's
  state, counted from the poll whose new packet carried the press to the game (the game compares at its next logic frame and the
  render reads it at the Present ending that frame, so a slow frame stretches the time, not the count); its value is always
  seq[progress] by the map in use (the game's own bindings read at the window's open, else the built-in map). One resend
  150 ms and two fresh Presents after a press the game did not show (and settled as above); a second miss, or a reset right
  after our press with no word of the player's since and no press of theirs noticed at an earlier Present, stops injecting for
  the window (the plates turn grey, the
  buttons still work). Any other reset (a wrong button) rewinds the plates. The round lives with the driver's window: the
  injection gate (the VR pad's sync, the headset's focus) may drop for 0.25 s without ending it (presses simply wait).
- **Buttons racing ours** (odg_input): a new press by the player of any button the game reads (the d-pad, A B X Y, LB / RB,
  Start, Back, the stick clicks: Bushido resets on any non-zero new press) cancels our press while no new packet has carried
  it, whether the press is new at this poll or new to the game with this packet (one seen during a render stall reaches the
  game only with the next packet, as the packet number follows the render frames) (the render thread re-decides from the
  progress); our press also waits while the game still holds that same button from
  the player's last packet (else it would make no new-press edge), and its 400 ms cap counts from the end of the wait. During a
  targets round the triggers are held back, a quick squeeze is not L1 / R1, the grips do not grab and do not pass as LB / RB
  (also with `grip_lbrb=0`).
- **Haptics:** a hit 0.7 / 15 ms on the striking hand, a deflect 0.3 / 20 ms (on by default), success three 0.5 / 30 ms
  pulses; the window's open and a reset as before.
- **Practice targets** (F8, under the overdrive gesture rows): any time with the headset's 3D view and no overdrive window open,
  outside battle too: a random 3-8 symbols (or "Replay last Bushido": the last sequence seen this session), a 4 s bar, the full
  test and visuals, nothing sent. It ends with `practice: 8 of 8 in 2.31 s (first 0.57 s, then 0.25 s an input; deflects 2)` in
  F8 and the log. Sliders: blade pitch, target distance, target height, target size (a multiplier on the 24 cm plate), target
  spacing, Bushido time (ini `od_targets_blade_pitch`, `od_targets_dist2`, `od_targets_height2`, `od_targets_size`,
  `od_targets_spread`, `od_targets_time`; the first build's `od_targets_dist` /
  `od_targets_height`, defaults 0.6 / -0.2, are read and ignored so the new 0.5 / -0.3 apply; one log line says so).
- **Drawing:** the plate, preview, cuts, pips, bar and blades are built once a Present in the controllers' XR space and drawn last in the
  3D marker pass with the VR hands' map (as the overdrive visuals), in their own 4608 vertices; the blades and trails are drawn
  last with a reserve, anything else that does not fit is dropped and counted in the window's `odt:` line. The first scheme's
  glyph row does not draw under the targets. A 3D frame without the game's UI pass (most field frames) has no marker pass, so
  there the targets alone are drawn into the frame copy (eye 1) and a copy of the eye-2 twin, once per eye, on 3D scene frames
  only (not in a battle transition, under the save screen, or on a frame without a real 3D camera); UI frames draw into the
  finals as before (never both). The eye-2 copy is released after 10 s unused. Fahrenheit's F8 panel is its own composition layer above the eye images, so the targets
  cannot be drawn over it from inside the eye image: the practice tooltip says to close F8.
- **Log:** `odt:` lines for each cut, a cancel, a resend, a rewind, a stop, and one summary per window or practice round.
- **The game's bindings:** the owner's first test read `LB: 0x8000004 (not one of the ten game bits)` and fell back to the
  built-in map (which was right). The low 16 bits are 0x0004 (L1), so the high bit may be a flag in the binding word; masking to
  16 bits before the check would probably accept the game's own map. Left as it is until another binding confirms it.
- **Not verified (nobody here can run the game):** the feel and the defaults (practice is the place to tune them), whether the
  blade's direction suits the Quest grips (the pitch slider), the draw path's placement in every view, and the game facts the
  first scheme also rests on.

## The F8 menu reorganised (vr-menu S1, branch `vr-menu`, 8 Nov 2026)

Design: research/vr-menu-design.md (session scratchpad), sections 2 and 6, with the owner's decisions of 8 Nov (developer
mode = `diag_verbose`; tabs and labels as proposed, judged in the headset). Managed only; ABI unchanged (57).

- **Layout** (`menu.cs`): a header that stays in view (Headset, Recentre, the status in one line with the whole text in
  its tooltip, frame / GPU / VRAM with the full perf text in its tooltip, the version, warnings only when present,
  **Developer** on the right), then one tab bar: View, Controls, Battle, Overdrives, Movies, Hands, Developer. The selected
  tab draws in its own child window (`NavFlattened`), so the header never scrolls away; the tab is remembered in the new
  ini key `menu_tab`. A new install's window opens at 30 x 34 em; an existing imgui.ini keeps its size.
- **Developer mode is `diag_verbose`**, live: the tab appears or goes the same frame, `log_push` sends the log settings in
  that frame's `push_all`, and only the camera trace's fresh file waits for the next launch (a note says so). While the
  Developer tab is the selected one, `ffxvr_diag_keepalive(2)` keeps the native diagnostics armed, as the Debug header did.
  The Developer tab holds all of the old Debug in eight groups, plus the live readouts that sat beside the player
  settings (a player tab shows status only when it is actionable).
- **Widget helpers** `ui_check`, `ui_radio`, `ui_slider`, `ui_slider_int` take the ini key first; the tooltip ends with
  `ini: <key>`, and the tags are drawn after the widget: `UiTag.Experimental` (small amber `experimental`; experimental
  items come last in their tab), `UiTag.NextLaunch` (`next launch` while `NextLaunch.Pending` lists the key: the launch
  snapshot taken in `init` before the hooks are queued, against the values now), `UiTag.Dev`, and `UiTag.Common`, which
  draws nothing and marks the later simple page's controls. `msaa` shows `next scene` until the game makes new scene
  targets. With developer mode off, the header counts the developer-only keys (`k_dev_keys`) whose ini value is not the
  code default (taken from `ini_text()` before the file is read).
- **No duplicates:** the 12 keys the old menu drew twice (heat_haze, stick_reproject, ui_adaptive, ui_backing, shadow_cam,
  fp_self_shadow, load_lock_release, screen_width, screen_distance, fmv_depth_grid, seat_height, seat_forward) are drawn
  once. `diag_verbose` is the one key drawn twice on purpose (the header and the Developer tab). The Hands tab's body
  warning has a **Fix it** button (fp_body_mode=0 and the session's "hide the player's body").
- **Settings still reach native only through `push_all`**, every frame, drawn or not. What stays tied to drawing is what
  was tied to the Debug tree before: the keepalive, and in Developer > First person & body (with `hands_dev=1` only) the
  old bone hands' calibration countdown, `ffxvr_hands_method` and the bone finder's lift; `hands` is forced off there
  without `hands_dev` (native ignores it anyway: `push_hands` sends `hands_dev && hands`).
- **Check:** vrmenu_chk/check.py (session scratchpad) reads the old (799852a) and new sources: every one of the old 390
  widgets has a widget (ini keys by key and radio value, session controls by variable, buttons and headers by label, with
  the renames listed), every written key has a widget or is hand-edit only (design 1.2, plus `bt_tag_dist`), no key is
  drawn in two places, the helper keys agree with the parse switch, and the Common / NextLaunch tags match the design and
  `NextLaunch.Pending`. The C# is not compiled here.
- **Not verified (nobody here can run the game):** the layout at the owner's 35 px text, the tab restore on the first
  frame, the header's right-aligned checkbox in a narrow window, and the tags' small font.
- **Review fixes** (vrmenu-s1-review.md, S1-S5 and five nits): `dmg3d_hide2d` is `next launch` only when turned on
  without its hook (hooked at launch it is live both ways); the log cap is never lowered during a session (native closes a
  log already past a lowered cap until the next start), and the Developer tooltip names the automatic captures it turns
  on; with Developer off the header names the session-only developer states still running (F10 slow motion, F11
  highlight, a draw range, the frozen camera, eye 2 without the headset, the bone lift) with a **clear** button, never
  clearing them by itself; `menu_tab=developer` is never restored (View opens), so the diagnostics are armed only after
  a click on the tab; the player docs (README, INSTALL-ALPHA, the zip's README, CHANGELOG, release.md) follow the new
  menu. Nits: tooltips are built only while hovered, the tab bar uses the Mixed fitting policy, a radio row shows
  `next launch` once, the guided FMV grid's radio shows whenever it is the value, stale "Debug" comments renamed.

## The F8 menu as a panel in the room (vr-menu S2, branch `vr-menu`, 9 Nov 2026; ABI 58)

Design: research/vr-menu-design.md (session scratchpad), sections 3 and 6 S2, with the owner's decisions of 8 Nov (grab
anywhere on the panel; F8 stays the only way to open it, no chord; Practice targets slides it aside for the round; it
stays visible during movies). No laser yet (S3), so no beam and no dot. Reviewed in vrmenu-s2-review.md (session
scratchpad); the fixes are folded in below and listed at the end.

- **Managed** (`vrpanel.cs`): while the headset is on, `imgui_layer=1`, `vr_menu_panel=1` and native reports the panel
  available, the F8 window is drawn as its own window `FFX VR###ffxvr_vr` at the right edge, 30 x 34 em, set every frame
  (`NoMove | NoResize | NoSavedSettings | NoDocking`, and no title bar, background, border or scrollbar of ImGui's own:
  those are decorations clipped to the whole display), its opaque background drawn first in its own draw list clipped to
  itself (`vrp_fill`), FramePadding x1.5 and ItemSpacing x1.25; the flat window keeps its imgui.ini place and size. Its
  combo lists (`vrp_begin_combo`) and tooltips (`vrp_tooltip`, under the item, inside the window's width, wrapped) hide
  ImGui's popup background and border and draw their own the same way (`vrp_union_popup`, skipped on a popup's first
  frame, when ImGui hides it at its last place). Our font is clamped to 28..45 px there (`vrp_font`). Every frame after the window
  `vrp_push` reads `ffxvr_panel_state` (a grab that ended writes `vr_menu_distance` / `_height` / `_scale`, saved by the
  debounced ini save) and then sends `ffxvr_panel_set`: the window plus the combo lists and tooltips it opened, the window
  alone, the em, DisplaySize and the three settings. `push_all`'s `force_show` is no longer
  set by the open F8 window while the panel showed (the save screen still forces the ImGui layer).
- **Native, pure** (`ffxvr_panel.h`, namespace `vpanel`): placement (`place_front`, `keep_pose`: 50 degrees / 3 m, `remember`),
  face-the-eye (yaw then pitch, no roll), the size model (`m_per_px`: one em is 2 tan(0.45 deg) m at scale 1, so the size is
  physical, not angular, when the panel is moved), the crop (outward rounding, clamped, cut to 1536 from the top-left),
  `crop_offset` (the quad's centre against the window's, so the window part stays put when a list widens the crop),
  `scissor_inside` (which ImGui draws are the panel's), the ray / reach proxy, the grab state machine, and a targets round's
  `slide_plan` / `slide_step` with `targets_half_deg`.
- **The panel's own capture** (review S1). ImGui's D3D11 backend sets one scissor rectangle per draw command, the clip of
  the window that draws it. `ffxvr_panel_set` turns this frame's rectangle into backbuffer pixels; Fahrenheit's
  RenderDrawData follows in the same Present, and `ffxvr_on_draw` sends each ImGui draw whose scissor lies inside it (4 px
  slack) to the panel's capture (`vpanel_route_draw`; 1536 x 1536, the backbuffer's format, made while the panel is on in
  a running headset, freed when it is turned off or the headset stops). `ffxvr_eye2_begin` binds it and moves the viewport
  and the scissor by the rectangle's origin, so the window lands at (0, 0); `ffxvr_eye2_end` puts both back. Every other
  draw goes to the ImGui layer's capture as before and only those count toward its `min_draws`, so the layer keeps the
  save screen and F7 whole under the window (no hole) and is not composited at all with F8 alone (review N5).
- **Native, glue** (`ffxvr_native.cpp` `vpanel_*`): the panel shaders (`ps_crop`, `ps_crop_srgb`); `vpanel_copy_crop`
  copies the capture 1:1 into swapchain 6 (the viewport's top-left at minus the crop's origin, so each target pixel
  samples one texel centre). `vpanel_frame` runs in `xr_tick` before `xr_frame`: it arms the routing for the draws that
  follow, makes the swapchain at the first open (`xr_panel_ensure`, review N6: none for a player who never opens it),
  places the panel on open or re-place (Home, the headset's recentre, a space change, Bring here; deferred while a grip
  moves it or while slid aside), checks that the capture holds the last frame's routed draws, and builds the quad.
- **Frame pairing.** Managed calls `ffxvr_begin_frame` (g_frames++, then `xr_tick`) and then draws and calls
  `ffxvr_panel_set` stamped with that g_frames; RenderDrawData follows, so the routing uses the very rectangle of the
  draws it routes (stamp == g_frames). The next `xr_tick` shows the capture when its draws were routed in the frame before
  (`g_vp_cap_frame + 1 == g_frames`), else it counts a miss and shows no panel that frame (`panel:` line,
  `ffxvr_panel_state[7]`). A miss no longer flashes F8 on the head-locked layer: its draws are in the panel's capture.
- **The grab without a laser.** At a grip's press edge (squeeze > 0.5), the grip latches to the shown panel when its aim
  ray hits the window's rectangle widened by 3 cm, or the grip pose is within 15 cm of it. Not inside an injecting Bushido
  window (`odg_grips_are_gestures()`), not during a targets round, not while aside (a latch from before the round stays,
  masked, but stops moving the panel). One hand: the panel's centre
  rides rigidly in the grip's frame. Two hands: scale = scale0 x d1 / d0 (0.6..2; hands closer than 5 cm do not scale,
  and a pair that formed closer starts from where they first are 5 cm apart, review N8), the centre at the midpoint plus
  the offset when the pair formed; a change of hands re-anchors without a jump. Then the
  distance from the eye is clamped to 0.4..2.5 m and the panel turned to face the eye. Haptic 0.3 / 40 ms at a latch.
- **A latched grip is hidden by zeroing its `squeeze`** in the input copy before it becomes `g_vrpad`. `vrpad_take` is
  the one writer of `g_vrpad` (review B1: stickrep's extra frames wrote the raw input into it between two game frames),
  called by `xr_tick` and by `sr_extra_inner`, so the grab also runs at display rate. Every other grip use reads that copy,
  so the world grab, GripTap (quick squeeze, LB / RB), the LB / RB pass, the grip chords, the ImGui pad feed and the hands
  tap all see a released grip, without a gate in each of them. The mask holds until the grip is let go, even if the panel
  closes. The holding hand's finger curl takes the grip's own squeeze (`vpanel_curl_squeeze`, review N1), so it stays
  curled. vrmenu_chk/s2/probe_vrpad.py fails on any other `xr_input_get` or `g_vrpad` write.
- **A Bushido targets round** (Practice targets or a real one, `g_odt.r.on`; review S2 and N7). At its start `slide_plan`
  takes the panel's azimuth against the round's forward, its angular half-width and the targets' extent: each plate's
  glow ring is a cone of angular radius rho around its centre (az_c, el_c), reaching az_c + asin(sin rho / cos el_c) in
  azimuth (35.7 degrees at the default layout, `targets_half_deg`). A panel not clear of that by 4 degrees turns to its
  own side about the eye until its inner edge is (52.9 degrees for the default panel); past 60 degrees, or not placed
  yet, it hides for the round instead. 120 degrees a second both ways.
- **Layers.** Base, [stereo right], UI panel, ImGui layer, menu panel: at most 5 (`layers[8]`); the
  runtime's `maxLayerCount` is logged once. The panel quad is in `g_space`, premultiplied, with `imageRect` the crop's
  extent; held frames and stickrep's extra frames re-send it as last sent.
- **Exports (ABI 58):** `ffxvr_panel_set(flags, v, n)` (flags 1 drawn and open, 2 `vr_menu_panel`; v: the rectangle with
  its popups, em, DisplaySize, distance, height, scale, then optionally the window alone, n >= 14),
  `ffxvr_panel_state(out, n)` (12 values), `ffxvr_panel_place(1)` (in front at the next frame).
- **Tests:** vrmenu_chk/s2/panel_test.cpp (session scratchpad), 217 checks under g++ ASan / UBSan and clang++, the
  slide checked against ffxvr_odtarget.h's own default layout, the grab at display rate; 56 mutants of the header, all
  killed; probe_vrpad.py passes (and fails on a79aca0). vrmenu_chk/check.py passes with the four new keys.
- **Not verified (nobody here can run the game):** the negative viewport offsets on the owner's driver (the copy, and the
  routed draws' shifted viewport), that every draw of the window clips inside it (a draw that does not stays on the
  head-locked layer; the `panel:` line counts the routed ones), the layout at 35 px, how the reach proxy feels, and VD's
  handling of five layers. At a 1080p backbuffer and 35 px the 34-em window is taller than the screen and is cut at the
  bottom; the owner's 3840 x 2160 capture holds it. Another window lying wholly inside F8's rectangle would be routed to
  the panel with it (drawn over it there, as on the TV).
- **Review fixes** (vrmenu-s2-review.md): B1 one writer of `g_vrpad`; S1 the panel's own capture instead of a hole; S2 the
  slide clears the targets or hides; N1 the curl; N2 a popup's first frame; N3 tooltips on the panel; N5 the layer not
  composited for F8 alone; N6 the swapchain at the first open; N7 every targets round, and no moves while aside; N8 a close
  pair; N9 the "Overdrives always ready" label, the scissor restore (none bound stays none), the docs.

## The laser pointer on the menu panel (vr-menu S3, branch `vr-menu`, 9 Nov 2026; ABI 59, 60 after the review)

Design: research/vr-menu-design.md (session scratchpad), sections 3.5, 4 and 6 S3. Pure logic in `ffxvr_pointer.h`
(namespace `vptr`), unit-tested in vrmenu_chk/s3 (session scratchpad).

- **The ray.** `vpanel_pointer` runs inside `vrpad_take` (every game frame and every stickrep extra frame, render thread) on
  the raw input, before any mask. Each controller's aim pose (its -Z) meets the quad last shown (`g_vq`: the union's pose with
  the crop offset, its size, the crop in backbuffer pixels); `hit_quad` gives u, v across the image and the point in metres.
  The pointer never catches the panel while it is aside or hidden for a targets round, nor without head tracking.
- **Which hand** (`vr_menu_hand`: 0 either, 1 right, 2 left; `owner_step`). Either: the owner keeps it while its ray is on, for
  150 ms after it leaves, and however long its trigger is held (a drag); a hand that comes onto the panel (after 150 ms off,
  so an edge flicker does not count) or pulls its trigger on it takes it over; both at once, the right. A hand the setting leaves
  out has no ray, no beam and no mask.
- **The mouse.** The owner's hit, 1-euro filtered in the panel's plane (1 Hz at rest, + 20 Hz per m/s) and press-locked (100 ms
  or 1.5 degrees, so a click does not slip but a slider drag starts), goes back across the image and through the crop to ImGui
  coordinates (`uv_to_imgui`). Its trigger (30 / 255 to press, 20 to release, as `pad::k_trig_on / off`) is the left button
  when pressed on the panel, kept until released (a drag past the edge keeps feeding). Its stick y over the panel is the wheel
  (dead zone 0.2, 4 notches a second at full deflection), summed across calls until managed reads it.
- **Feeding ImGui** (`vrp_pointer`, managed `render_imgui` after the window, so the events apply at the next NewFrame):
  `io.MousePos` written directly with `io.WantSetMousePos` (below; no queued position while feeding, review S1),
  `AddMouseButtonEvent(0)` on changes, `AddMouseWheelEvent`, and one `(-FLT_MAX, -FLT_MAX)` at the end of the hand-back.
  **The real cursor:** in ImGui 1.92.9b (Fahrenheit's cimgui and ImGuiImpl, both checked) the Win32 backend polls
  `GetCursorPos` at every NewFrame while the physical mouse has not moved over the window, and queues it after ours. We also
  write `io.MousePos` and set `io.WantSetMousePos`: the backend then moves the OS cursor to our point (only with the game in front)
  and skips the poll, and `UpdateInputEvents` drops every queued position event while the flag is set, so the direct position
  stands whatever is queued (WM_MOUSEMOVE from the cursor move included). ImGui's NavUpdate clears the flag each NewFrame; we set
  it again each frame we feed. Logged once (`menu pointer: ... via io.MousePos + io.WantSetMousePos`).
- **Handing the mouse back** (review B1; `vptr::Feed`, run by native in `ffxvr_panel_pointer`, which managed calls with
  `io.MousePos`, DisplaySize and the F8 window's rectangle). Feeding moves the OS cursor onto the F8 window (the backend's
  SetCursorPos), so a single "no mouse" on leaving loses: the WM_MOUSEMOVE that cursor move posts is handled on the message-pump
  thread after our event, and the backend's GetCursorPos poll (with the mouse untracked) adds the parked position every frame;
  either puts ImGui's mouse back over F8, `WantCaptureMouse` stays on and Fahrenheit withholds the game's input. Now, on leaving,
  `io.MousePos` is written at a park point with the flag kept for 3 more frames (ImGui drops every queued position meanwhile and
  the backend moves the OS cursor there), then the flag goes and one "no mouse" is queued. The park point is where ImGui's mouse
  was when pointing began, unless unknown or within 8 px of the F8 window; then (8, DisplaySize.y / 2). A model of these sources
  in pointer_test.cpp (`t_imgui_model`) shows the old way never gives the input back and the new one within 5 frames, either with
  the mouse tracked or with the backend polling.
- **The edge and gaps** (review N1, N2): a ray comes on inside the quad and goes off 1.5 cm past an edge (`on_step`); a drag
  begun on the panel keeps its button and last position through up to 100 ms without the panel (`drag_grace`: a pairing miss, a
  head-pose drop). The hover tick needs the new item held for a frame and differs from the last one that ticked.
- **What the game and the mod do not get** (`mask_step`): a hand whose ray is on the panel has its trigger, stick and stick
  click zeroed in the copy that becomes `g_vrpad`, so XInput, the stick turning, stickrep's turn, the chords, the hands tap and
  the ImGui pad feed all see nothing. A trigger pressed there stays the panel's until released, a stick until back in its dead
  zone. The finger curl takes the masked trigger's own value (`vpanel_curl_trigger`). On top of that, while the pointer is on the
  panel Fahrenheit skips the game's `PInput::Update` entirely (`h_pinput`: `WantCaptureMouse || WantCaptureKeyboard`), as it
  does for the real mouse; and a click focuses the window, which holds `WantCaptureKeyboard` (keyboard nav) and so the game's
  input until the focus goes. A trigger pulled while no ray is on the shown panel (`ffxvr_panel_pointer[6]`) therefore calls
  `ImGui.SetWindowFocus(null)` when F8 has the focus (design 4.5's proposal): the panel stays, the game moves again, and that
  pull reaches the game. Not with `imgui_vrpad=2` (review N4: the controllers navigate the menus and the focus is theirs).
- **The grab** now needs the ray on the window itself (S2's 3 cm proxy is gone; the beam shows where it points) or the
  controller touching it (15 cm). Two-hand resize is unchanged.
- **The dot** is drawn into the panel image by the crop copy: a constant buffer (dot x, y in capture pixels, radius 0.3 em or
  0.22 em pressed, colour light blue or amber when pressed), a dark ring for contrast, antialiased; only for the panel's capture.
- **The beams** are their own quads, last in the layer list: a 16 x 128 premultiplied gradient (`g_vp_beam_tex`; left half the
  owner's, right half at 35 %) written once into swapchain 7 (made with the panel's at the first open), then only referenced.
  A beam shows while its ray meets the quad widened by 25 %, or while its grip holds the panel; `beam_pose` puts its length along
  the ray and its face toward the eye; 4 mm wide. Extra frames re-send them as last sent. At most 7 layers, and never more than
  the runtime's `maxLayerCount` (review N3: the last layers, beams then the panel, go first).
- **Haptics** (`vr_menu_haptics`, managed): 0.08 / 6 ms on arriving, 0.12 / 8 ms on each new hovered item
  (`ImGuiP.GetHoveredID`), 0.25 / 12 ms on a press over an item.
- **S2 round 2 nits folded in:** a draw is routed only when its rasterizer state has scissoring on (N1); a DisplaySize that is
  not the backbuffer's is logged once (N2); the tab bar has `NoTooltip` (N3); routed draws are counted in `eye2_begin` (N4).
- **Exports:** ABI 59 `ffxvr_panel_pointer(mode, out, n)`; ABI 60 (the review's hand-back) `ffxvr_panel_pointer(mode, in, nin,
  out, n)`: in 8 values (io.MousePos, DisplaySize, the F8 window), out 11 (owner, on the panel, write + x + y, button, wheel,
  no-mouse, presses off the panel, masked hands, beams).
- **Tests:** vrmenu_chk/s3/pointer_test.cpp, 168 checks (g++ ASan / UBSan, clang++ -Werror), 65 mutants of `ffxvr_pointer.h`
  and the grab's ray, all killed; the S2 suite, probe_vrpad.py and check.py stay green. Reviewed in vrmenu-s3-review.md
  (session scratchpad): B1 the hand-back, S1 no queued positions, N1 the edge and the hover tick, N2 the gap, N3 the layer
  limit, N4 the focus with stick navigation, N5 an owner-test note; N6 (beams a call behind) left as it is.
- **Not verified (nobody here can run the game):** the feel of the filter, the press lock and the wheel rate; the OS cursor
  following on the TV; whether `PInput::Update` really holds the game's XInput poll (design 4.5's guess; the native mask covers
  the mod's own readers either way); the beams' look; the unfocus on an off-panel pull (an owner decision in the design).

### To raise upstream: ImGui's input queue across two threads (Fahrenheit)

Facts only, from Fahrenheit at the version the mod targets (`src/runtime/imgui.cs`) and ImGui 1.92.9b:

- The game renders on its own thread. `h_present` runs `ImGuiImplWin32.NewFrame`, `ImGui.NewFrame`, every module's
  `render_imgui`, `ImGui.Render` and `RenderDrawData` there; the window's message pump runs on another thread (Fahrenheit's
  own comment above `h_implw32_setwindowfocus` says so).
- `h_wndproc`, on the message-pump thread, calls `ImGuiImplWin32.WndProcHandler`, which queues ImGui input events (mouse
  position, buttons, wheel, keys, focus) into the context's `InputEventsQueue`.
- `ImGui.NewFrame`, on the render thread, reads and clears that same queue (`UpdateInputEvents`); modules that call
  `io.Add*Event` from `render_imgui` push to it on the render thread too. `InputEventsQueue` is an `ImVector` with no lock,
  so a push on one thread during a push or the clear on the other can lose an event, and a push that reallocates during the
  other thread's access touches freed memory.
- This exists without the mod whenever the physical mouse or keyboard is used while frames are drawn. The mod's pointer adds,
  from the render thread, a button event per press and release, a wheel event per frame while the stick scrolls, and one
  position event per hand-back; and while it points, the backend's SetCursorPos makes Windows post one WM_MOUSEMOVE per frame,
  which the pump turns into a queued event.

## The menu from the controller: left grip + left menu button (vr-menu S4, branch `vr-menu`, 10 Nov 2026; ABI 62)

The owner's request after testing S3: open and close the F8 window from the controllers. **First try (ABI 61, 193b196):** a long
press of the left menu button alone. The owner's test: Virtual Desktop reads that button on the headset itself and takes a long
press (and a double tap) for its own "switch to desktop", before anything reaches the game or the mod, so it cannot be masked. Now
the left grip held + a press of the left menu button.

- **The toggle is F8's own path.** F8 is the mod's key, not Fahrenheit's (F7 is): `handle_hotkeys` flipped `_window_open` and
  cleared `_window_open_forced`. That is `toggle_window()`, called by F8 and by the chord, so both act the same (the window, the
  saved `overlay_open`, the headset panel, everything that reads `_window_open`). No key press is faked.
- **Where the chord is seen** (`ffxvr_menuchord.h`, namespace `vmc`, unit-tested in vrmenu_chk/s4): `vpanel_input`, inside
  `vrpad_take`, the one writer of `g_vrpad`, on the raw input (the grips before any panel mask) at every game frame and every
  stickrep extra frame (time from QueryPerformanceCounter; one call counts at most 0.25 s). The left grip must have been down at
  least 150 ms (`k_grip_min_s`; a squeeze's first call counts no time) when the left menu button's press edge comes, and the
  right grip up; then one toggle (`g_vmc_toggles`, read by managed through `ffxvr_menu_chord`). A press without that is Start as
  before (`pad::compose`: `L.menu` -> START), to the game and to ImGui's pad (`ffxvr_imgui_xstate`).
- **The press is hidden:** from the chord's press until the button's release `hand[0].menu` is cleared in the copy that becomes
  `g_vrpad`, so the game (no pause) and ImGui's pad never see that press.
- **The grip is the chord's** from the press until the grip's release (`Out.owned`), by the existing mechanisms:
  - the quick squeeze: the existing chords (L grip + R stick click and the others) stop a squeeze from being LB through
    `GripSample.any_click`, which marks the squeeze `GripTap.chord` and gives it the verdict GV_CHORD on release. The menu chord
    marks the left squeeze the same way (`g_vmc_veto`, applied in `grip_tick` before `grip_step`), so it is never sent;
  - the world grab: the existing chords do not touch the grab, so for this one `gesture_tick` leaves the owned grip out of
    `grab[0]` (the other hand keeps grabbing; a hand leaving the grab re-anchors with no move that frame, so nothing jumps), and
    at the chord undoes the grab's nudge since the squeeze began with the snapshot a quick squeeze uses (`g_grip_snap`, the
    GT_RESTORE condition: begun in a grab view, within `grip_tap_ms`, no two-hand grab, the same view). A grip held longer than
    that (a real grab under way) is only left: the world stays where the grab put it.
  - In views where the grips are LB / RB at once (field first person, `grip_lbrb=1`), the grip's LB was already held by the game
    from the squeeze, as with any grip; the chord adds no LB edge.
- **Fahrenheit's soft reset** (`runtime/reset.cs`: ImGui's GamepadL1 and GamepadR1 down and a GamepadStart press): with the right
  grip down the chord does not fire, so both grips + the menu button is Start, and the soft reset, as before.
- **The gate:** `vr_menu_chord` (default on; `vr_menu_hold` and `vr_menu_hold_ms` from the first try are read and ignored, and
  no longer written), the headset running (`g_xr_active`), the controllers as the gamepad (`vrpad`), the left controller active.
  A mask already on stays until the button's release.
- **Haptics** (managed, left hand): opening two pulses of 0.55 / 30 ms 90 ms apart, closing one of 0.4 / 80 ms.
- **Export (ABI 62):** `ffxvr_menu_chord(on)` (it replaces 61's `ffxvr_menu_hold(on, ms)`) sets the setting and returns the
  toggle count.
- **Tests:** vrmenu_chk/s4/menuchord_test.cpp, 28 checks with the real `pad::grip_step`: grip then button toggles once (two
  presses in one grip, twice), the button then the grip and a squeeze and press within 150 ms do not, Start hidden from the press
  until the release, no quick-squeeze LB from the chord's grip (and the same grip without the chord IS one), the right grip
  (soft reset), extra frames and stalls, the gate; 15 mutants, all killed.

## The event hold through a sphere grab's own camera cut (branch `eye-hold2`, 10 Nov 2026)

**The event hold** (fix-eye, `ffxvr_eyejump.h` `ej::EvHold`, `ffxvr_fp_event`; F8 > View > First person > "Steady view during
scripted moves", ini `fp_event_hold`, default off, on since 10 Nov 2026 after the owner's Djose test; `fp_event_leash`, "slack (units)", default 12, 5..40 since review round 2). In first person on foot, when
player control goes off, the eye's horizontal position stays where it was while the game moves Tidus's root, on a leash: further
than the leash, the eye trails at that distance; a single step over twice the leash (a staging) restarts the hold at the new root.
The height follows the head as usual. It ends at control's return, a map change / load / re-pick, the view leaving first person,
the setting off, or 45 s; then the eye glides back over 0.3 s (under half the leash off) or blinks and snaps.

**The owner's test (Djose cloister, `fp_event_hold=1`), measured.** The motion log with the eye columns
(`logs8/djose_grab_1300.log`, 13:00:22-27) shows one sphere take:
- frame 1093 (the motion lines' numbering, used here and in the tests; a diag `eye hold:` line names the same step one less, 1092):
  control goes off at the root (-31.78, 10, -218.25), and the hold starts there;
- frame 1097: the game cuts its camera and, in the same step, moves the root to (-33, 10, -222), 3.9 units (about 32 cm) toward the
  socket, split over two frames by the smooth anchor (1.97 + 2.08);
- the hold ended on that cut, "ended (a camera cut) after 0.1 s", and the eye glided onto the new root in 0.3 s;
- the root stayed at the socket for the 1.8 s of the take, with the eye at the root: the face in the wall;
- frames 1203-1204: the root moves back to (-30, -220); 1205: control is back.

The earlier session's grabs had all ended the same way, "after 0.0 s". The rule "a camera cut ends the hold" was added because a
cut usually means a new scene. But the grab's own cut comes with the scripted snap, so it defeated the hold every time.

**The rule now (`EvCfg::cut_win`, revision 8).** A camera cut ends the hold only when the root (the eye base) moves more than the
leash between `cut_win` = 4 frames before the cut and 4 frames after it: a new scene (a staging, a teleport, another place).
- **The window.** It covers a cut and a move that come a frame or two apart in either order, and a snap split over two frames. A
  second cut inside it keeps the first one's reference. `EvHold` keeps the last 8 bases for it (no allocation).
- **A cut that keeps the hold** leaves the root within the leash of the eye, so holding on stays safe. The 45 s cap and control's
  return still bound it. The map change is managed's release bit, as before.
- **The proposal was a 1.0 s grace after the start** (cuts ignored), then the jump test. The jump test alone is used, at any age
  of the hold. A longer interaction (djyt0000: 27 s with control off and text boxes) can cut again later, still at the socket,
  and a fixed grace would release into the wall there. A cutscene that stages him far away in its first second should release
  at once, not restart the hold at the staging mark.
- **4 frames**, because a scripted run at 1.02 units a frame across a cut (djyt0000) covers 8.2 units in the 8 frames, under the
  leash. The chocobo's 2.5 a frame would count as a jump, but the hold is not eligible while riding.
- **The cost, and the review's B1.** S1's block covers only a cut on the control-off edge or in the 2 frames before it. A
  cutscene whose first cut comes 1 or more frames after control goes off starts a hold. If it never stages Tidus elsewhere and
  walks him, the jump test alone kept the eye up to the leash (about 1 m) off him, through every cut, until control or 45 s
  (the review measured 25 s at 12 off). Before, its first cut released it. The two rules below bound it.
- **Control back** goes from the held point to the root at that moment. In the trace that is (-31.78, -218.25) to (-30, -220),
  2.5 units, under half the leash: the existing 0.3 s glide (18 frames, at most 0.15 units a frame). Unchanged.

**"Engaged" and the logs.** `EvHold::engaged` is set when the hold first keeps the eye `EvCfg::engage` = 1 unit off the root and
the root has moved 1 unit from where control went off (review N1: a hold starting inside the last glide is not engaged by the
glide's leftover): the game moved him. Text boxes and NPC talks start a hold that never engages.
- `eye hold:` diag lines now have two budgets. The quiet ones get 30 a session: every start, a cutscene's blocked start, the
  first cut held on in a hold that never engaged, and every end of a hold that never engaged (review S2: the deadline, a staging
  cut, a release, the setting off included). The working ones keep their 60: engaged, the first cut held on in an engaged hold, a
  pull, a staging, and the end of an engaged hold. Each line takes its own count (review N6). A session full of text boxes and
  road cutscenes no longer silences the cloister.
- New lines: "engaged N s after control went off: the game moved the root N units ..."; and, for a hold's first kept cut only,
  "a camera cut with the root N units from where it was 4 frames before (not over the leash ...): held on". The end line adds the
  camera cuts, how many it held on through, and the largest root move around one; a quiet end says "never engaged" with its cut
  count. A cut that releases says "a camera cut with a jump of the root over the leash: a new scene".

**The automatic capture's budget** (`FpCap`, diagnostic profile only). The owner's 6 motion-log runs went to ordinary
control-off moments (text boxes, road cutscenes) before the cloister.
- Now the 6 runs (`k_runs`) start when the hold engages. Managed reads `ffxvr_fp_event` out 0 = holding and out 2 = the eye
  `k_engage` or more off its base: one frame after native, at the push. One run per episode, with no 30 s spacing (grabs come a
  few seconds apart).
- A plain control-off edge gets its own `k_runs_plain` = 2 runs, 30 s apart, as before. When the hold engages inside this
  episode's plain run, it takes the run over: no restart (the log already runs from control off), and the plain budget is given
  back.
- The summary line at control's return is also written for an engaged episode without a run (30 a session).
- Review S2: a cutscene can still engage (it moves him in its first 1.5 s). Its hold then ends while control is still off (a
  staging cut, a late cut, a release, the cap), unlike an interaction's, which ends at control's return. So a hold run whose
  hold ends with control still off is given back (`FpCapState.refunds`, at most 6 a session; the motion log runs on).
- No export changed (the out layout and flags are as before): the ABI stays 62.

**Review fixes** (fixes/eyehold2-review.md, `FFXVR_EJ_REV` 9):
- **B1, the engagement deadline (`EvCfg::arm_s` = 1.5 s).** A hold not engaged 1.5 s after control went off ends (`EVW_ARM`,
  the usual glide, under a unit: unseen).
  - Every known interaction moves him at once: the cloister grab at +4 frames, djyt0600's 9.1 set "the moment control went off",
    djyt0000's run from the edge. djyt0600's second set 11 s later is inside a hold that engaged at its start.
  - A talk, a text box, or a cutscene that talks before it walks gets no hold.
  - The cost: an interaction that first moves him more than 1.5 s after control goes off is not held.
- **B1, the late cut (`EvCfg::late_s` = 3 s).** In an engaged hold, a camera cut 3 s or more after the engagement ends it, jump
  or not (`EVW_LATE`: a glide under half the leash, else a blink and a snap). This answers the question: should a cut long after
  the engagement still end it?
  - The measured grab's own cut comes at its engagement, and the whole engaged grab lasts 1.8 s, so it keeps with margin.
  - A cutscene that moved him in its first 1.5 s goes back to him at its next shot change 3 s on.
  - The residual is the time to that cut: the review's probe (a walk from 1 s, cuts every 3 s) is held 6.0 s instead of 25.
  - The cost: a long interaction (djyt0000: 27 s with control off) that cuts its camera again 3 s or more after it moved him
    glides the eye onto the root at the mark at that cut.
  - The capture shows only the grab's one cut, so this takes the conservative side of the unknown (a cutscene a metre off him
    is the failure that was reported). A capture of a long interaction would tell; the motion line has no cut column yet (the
    review's suggestion, not done here).
- **S1, a staging over two Presents.** FFX-60fps blends a tick step of up to 30 units over two Presents (13 + 13 for a 26-unit
  staging), and the smooth anchor accepts the halves.
  - So a staging is now a single step over twice the leash, or the last two steps together over it, as the body hide's SM-2
    margin already counts "half + half". The second half of a restart is not another.
  - The single-step pull for 12-24 stays.
- **N1** (above, "engaged"). **N3:** a release (managed's, or a gap in the steps) clears the ring and the open window, so a cut
  just after re-entering first person is not measured from where he was before. **N4:** "held on through N" counts every cut
  of the kept windows, not the windows. **N6:** one count per line.
- **Round 2 (`FFXVR_EJ_REV` 10).**
  - **R2-S1: the late cut only with the eye pinned near the slack** (`EvCfg::late_pin` = 0.75 x leash off the root at the cut).
    A grab-style hold within the slack (the cloister's 3.94, djyt0600's 9.1 / 10.5) now keeps through any later cut until control
    returns. A longer take at the socket with a second cut 3.5 s on (the reviewer's L1) stays out of the wall. Round 1's
    cutscene probe (walked to the leash, 12 off) still ends at its first late cut, 6.0 s.
    - Costs: a run to a mark beyond the slack (djyt0000-like, the reviewer's L2) that cuts again 3 s on blinks onto the root at
      the mark. A cutscene that moved him under the slack in its first 1.5 s keeps the eye under 1 m off him until control, a
      staging or 45 s: the view a grab gives.
    - With the gate, a grab never logs "a camera cut 3 s or more after the hold engaged": such lines come from cutscenes or runs
      over the slack.
  - **R2-N4: the two-step staging needs two real halves** (each step at least half the leash; a blend's halves are, even at
    1:2). A 23.5 set followed by a walking step pulls instead of restarting.
  - **R2-N1:** managed's engaged test also needs the eye's offset to have grown by `k_engage` since the control-off edge, which
    matches native's "the root moved" for a hold starting inside a glide (no run, no give-back). No ABI change.
  - **R2-N2:** the slack's minimum is 5 (slider and ini). Under it a scripted walk pulls and blinks every few frames; native
    still clamps 2..40.
  - **R2-N3:** a given-back hold run that had taken over the plain run makes it a plain run again. Not done: telling a
    setting-off or drawn-head end (`EVW_OFF`) from a cutscene's, which would need the end reason in the export. It can give back
    a real grab's run, bounded by the 6 refunds.
- **Left as notes.**
  - N2: the window and S1's block count Presents. At a 30 Hz present rate (no FFX-60fps) the window spans 4 ticks, and a
    djyt0000-speed run across a cut can release. That degrades to f12e7db's behaviour, not worse. Keying both to time would fix it.
  - N5: an engaged episode's summary line in an ordinary profile (up to 30 a session), and its "on his root" says nobody because
    the share is tracked only with a run.

**Tests** (scratchpad `eh2_chk`, `run_all.sh`):
- **The trace transcribed** (`gen_trace.py`: 360 motion frames, the root, control, the cut frame) and replayed through
  `ej::ev_step`. Against the old header the replay reproduces the logged eye base within 0.017 units (the transcription is
  faithful). Against revision 9, the eye stays at the held point from 1093 to 1204 (1.87 s, 0.0000 off), 3.94 units back from
  the root at the socket, and glides 2.5 units back at 1205.
- **Synthetic scene cuts.** All released: a 30-unit teleport 4 frames in, a 16-unit jump split 8 + 8, a jump 2 frames before the
  cut, a jump over twice the leash with a cut, a cut 3 s after the engagement. All held on: a cut with no jump, a cut 6 frames
  before a jump, a scripted run across a cut, the grab's snap and cut 1.2 s in, a cut 2.1 s after a late engagement.
- **The review's probes as regressions.**
  - P1: the cutscene walk ends late at 6.0 s, not 25 s; talking first ends at 1.5 s, never off him.
  - P2: 13 + 13 restarts, and the eye is on the new mark. P3: a hold inside a glide does not engage. P6: the ring is cleared by
    the release.
  - The deadline and late-cut boundaries, and both set to 0 giving rev 8's behaviour.
- **The glue** (evh_tick verbatim, `glue2_test`): the line budgets, the lines from the trace, the blink. **The capture**
  (`cap2_test`, `FpCap` transpiled): the budgets, the takeover, the trace through native into managed (the run starts at frame
  1098, the push after the engagement).
- **Round 2 tests.** The reviewer's probe2 L1 (the grab with a second cut 3.5 s on: held to control's return), L2, T1 and a
  1:2 blend; the pin's boundary (8.9 / 9.1 off); the late count from the engagement while pinned; a glide-start driven through
  native into managed (no run).
- **Mutants.** 70 for this change, all killed. The fix-eye suite still passes, and fix-eye's own mutants are killed except the 4
  whose lines this change rewrote (their equivalents are in the 70).

## Weapon hands, stage 0: the log-only capture (branch `weapon-hands`, 10 Nov 2026; ABI 65)

**The feature (not built yet):** in a first-person battle the followed character's weapon is drawn in the VR hand while he
stands idle, and goes back to the game's animation while he acts. This stage only records what the build needs. Nothing in the
game changes: no game write, no hook, and the draw probe hides, moves and uploads nothing.

**What the game does** (checked in the disassembly):
- A battle weapon is its own actor, a `w` model allocated in the actor table by `Ch_Allocate` (0x824F90).
- The battle keeps a table of weapon records at `[0xD3446C]`, 0x390 bytes each, index `chr + 0x1F * hand` (0x795B10). A record
  holds the weapon's actor (+0x00), its model id (+0x04) and the attach point on the owner (+0x0D): 14 for hand 0 (the weapon),
  15 for hand 1 (the armour's model). The Chr initialiser 0x79C110 sets the points.
- 0x793330 attaches the weapon to the owner's actor at that point (`0x832630`: parent +0x19C, bone +0x1A0). 0x793F10 detaches it
  for a throw and sets Chr+0x4B6 / +0x4B7. Wakka's ball (chr 0x1C), Lulu's doll (0x1D) and Yojimbo's companion (0x1E) also have
  a Chr record of their own (0x795BB0).
- The standard battle motion a character plays is the byte Chr+0x3F0, written by the battle's motion setters (0x7882C0,
  0x7A8FC0). Chr+0x3F1 is the idle in effect. The actor's own motion id is at actor+0x72C (`Ch_SetMotion` 0x837AC0).

**The capture:** F8 > Developer > Inspect > **Weapon research log** (ini `diag_weapon`, default 0), in battles, `wpn:` lines in
ffxvr_diag.log (`weapon_log.cs`).
- A full snapshot at the battle's start, on a change of the followed character or the view, and every 10 s.
- An event block on every change of a party member's motion.
- After a snapshot or an event of the followed character, the next frame's draws of his weapon models.
- A compact line every 0.5 s.
- Bounded: 8000 lines, 300 snapshots and 2000 event blocks a session.

**Native (ABI 65):**
- `ffxvr_wpn_probe` / `ffxvr_wpn_report`: the weapon models' index-buffer keys are watched for one armed frame. Each matched
  draw is recorded with its constant buffer's size, world matrix and first palette slot.
- `ffxvr_wpn_hands`: both VR hands' frames in render space, drawn or not (the hands are hidden in battle).
- `ffxvr_wpn_phase`: a battle motion's class and the candidate "weapon in the hand" rule.
- The pure parts are in `ffxvr_weapon.h` (the probe, the motion classes, the grip maths prototype), unit-tested in scratch
  (`wpn_chk`: 786 checks, 37 mutants killed).
- ABI 64 is left for the sphere-hands branch.
