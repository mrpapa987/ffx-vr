
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
