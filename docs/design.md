# Design notes

Decisions made before any code was written. The full feasibility brief lives in a separate
document; this file is the short version that travels with the repo.

## Why a mode-based design

FFX was built around authored camera angles. Sets are only dressed for what the authored
camera can see, so a freely movable viewpoint exposes missing geometry, culled objects and
flat backdrops. Rather than fight that everywhere, each game state gets the most freedom it
can support.

## Viewing modes

**Overworld — fixed position, free look.** The viewpoint sits where the game's camera sits,
but follows only its *position*, never its rotation. Rotation the player did not cause is
the fastest route to motion sickness, and FFX cuts between angles constantly. Hard cuts are
covered by a brief fade. Head lean is clamped, with a soft fade at the edge of the allowed
range. Keeping the position authored also means the existing camera-relative controls still
work unchanged.

**Battles — free viewpoint.** Battle cameras already orbit the party, which suggests arenas
are built more completely than overworld sets. This needs confirming early: if an arena has
a missing back wall, no amount of culling work will fix it.

By default the mod ignores the battle camera entirely, so attacks, overdrives and summons
play out in front of a fixed spot like a stage show. An optional toggle can show the
authored cinematic camera on a floating screen for moves that look wrong from a static
viewpoint.

**In-engine cutscenes — authored position, shot by shot.** Same treatment as the overworld.

**Pre-rendered FMVs — a movie screen.** Video cannot gain depth, so it plays on a screen
placed in an environment of its own.

**Blitzball — panel first.** The Luca match is mandatory, so the first release needs it
playable at minimum: the game's own camera on a large panel. A tabletop sphere or a seat in
the stands comes later, and will need the stick remapped to head- or sphere-relative
directions.

## Architecture

Two halves talking over P/Invoke:

- **Managed (C#, Fahrenheit module):** camera hook, game-state detection, settings UI.
- **Native (C++ DLL):** D3D11 hooks, OpenXR session, per-eye rendering, UI panels.

The camera hook picks a viewpoint per game state and widens the culling frustum so head
movement does not expose missing objects. The 2D layer (HUD, menus) skips stereo entirely
and is composited onto panels.

## UI

The game keeps running every menu; the mod only changes where they appear. Panels are fixed
in the world, never attached to the head. The gamepad works exactly as it does now.

A native 3D sphere grid is a later stretch goal, drawn from game memory while the game still
owns input and logic.

## Effect-heavy battle moves

Deferred to later polish. Expected test order, likely easiest to hardest:

1. Physical overdrives (Blitz Ace, Bushido)
2. Basic spells
3. Flare, Ultima
4. Summon entrances
5. Summon overdrives (Anima's Oblivion)

Known risks: camera-facing sprites, screen-space effects, and sequences that rely on a
camera cut to hide a model swap or backdrop change.
