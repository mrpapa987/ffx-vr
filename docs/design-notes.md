
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
with a repeated id is ignored.

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
damage numbers / status text (scene-pass draws).

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

All of it hangs off one ini switch, `diag_build7` (default 1). With `diag_build7=0` and a restart:
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

- **`diag_load=1`** (default; hooks at the next launch): log-only detours, queued at init before build 7's job join (which stays last in Fahrenheit's commit).
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
  - **2, frozen world (default).** No UI split, and the view stays in the mode the window started in. The override's head pose is frozen: `xr_apply_poses`, stick turning and the world grab are skipped. The projection layer is submitted with the poses and FOV of the frame the pin was taken on, so the player's own view breaks apart world-locked where it was. In the after phase the eye image is withheld, so the pinned last frame is re-sent with no 30-frame cap over the game's load black until the battle is live.
  - **1, head-locked.** No UI split and the mode held only.
  - **0, old behaviour.** The phases still run for the diagnostics.
  - A recentre or a pin dropped by the runtime ends only the freeze; the window goes on head-locked.
  - Frames without a camera inside a window are submitted with the headset's FOV, because their image is the capture of a headset-FOV view.
- **Diagnostics.**
  - `diag_transition=1` (default for this test build) writes one `bt:` line per Present inside a window: phase, states, handle, +4/+5, mode, UI frame, scene draws, camera uploads, depth clears, late draws, capture and shard draws, held frames, eye source and the Present gap. The lines are batched (one file open per ~30 lines) and capped at 900 per window and 6000 per session.
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
- **`diag_load_prof`** (ini; 2 in this test build; the probes hook at launch, the sampler follows the setting at once; needs `diag_load=1`):
  - A watcher thread samples the loading thread every ~1 ms. It runs while a battle load phase is open (the entry: from MsBattleExe's wait to the entry's `load:` line; the way back: from the teardown to the way-back line; at most 60 s) or while a `loadPCluster` call is in progress. With `diag_load_prof=2` it also samples the render thread on the ticks where the loader waits for the lock.
  - Each sample is SuspendThread, GetThreadContext, one ReadProcessMemory of the stack (8 KB, 4 KB for the render thread), plain reads of the lock words (and the render thread's XR stage), then ResumeThread. Nothing else runs in between: no heap, no CRT, no log, no lock, no module or VirtualQuery call. Classification, the module table, formatting and the write happen with nothing suspended, into fixed buffers.
  - The return addresses come from the EBP chain inside the stack copy, in chain order. Scanned FFX return addresses (values after a call instruction) go first only within 0x40 bytes of ESP, where a frameless leaf's caller sits; without a chain, the scan in slot order is used. This list gives the innermost FFX function in a range table. That decides the bucket: LOCK_WAIT (with the owner read while suspended), LOCK_REL, MGR_MUTEX, ALLOC_WAIT, ALLOC_FILL, TEX_CREATE (by leaf: d3d11, driver, ntdll), SRV, READ, OPEN, CLOSE, FIXUP, PLAT_CPU, MANAGED (JIT code), POST, IDLE, OTHER. Every tick also records the lock's owner (render, loader, main, other, free).
  - The two probes (managed, last in the hook queue) time the texture section and its creates inside each window. "Before the first mip read" is the lock wait plus the 2x temporary buffer, and the misses are split by file size.
  - One `prof:` block follows each `load:` line: the header, buckets in and out of window, the render thread while the loader waited, the owner per tick, the lock release's counts, the probes, the top exclusive addresses (FhUtil, mapped with `re/out/functions.tsv`) and the field loads in between.
  - The `load:` line's "GPU create" (0x441620) segment is now called "post fixups": the textures are created inside parse.
- **`load_lock_release`** (ini and overlay, live, default 0, **never saved**: every launch starts at 0):
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
