
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
- **Still open (for the R8b build):** hold the last frame on swapchain timeouts
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
