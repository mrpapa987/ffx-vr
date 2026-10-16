# Finding the camera

Research notes, 16 October 2026. FFX runs on PhyreEngine, and Fahrenheit has
already reverse-engineered most of what we need.

## The layout is known

`src/core/petypes.g.cs` documents the engine camera structures with field
offsets. Everything a VR renderer needs is there:

| Struct | Offset | Field |
| --- | --- | --- |
| `PCamera` | `0x00` | `m_viewMatrix` (`PMatrix4x3`) |
| `PCamera` | `0x30` | `m_projectionMatrix` (`Matrix4`) |
| `PCamera` | `0x70` | `m_viewProjectionMatrix` (`Matrix4`) |
| `PCamera` | `0xB0` | `m_localToWorldMatrix` (`PWorldMatrix`) |
| `PCamera` | `0xB4` | `m_nearPlane` |
| `PCamera` | `0xB8` | `m_farPlane` |
| `PCameraProjection` | `0x150` | `m_aspect` |
| `PCameraPerspective` | `0x154` | `m_FOV` |

**Caveat:** these are all commented out in the generated file. They document the
layout rather than declaring usable types, and the offsets should be validated
against the live game before being trusted.

## Class descriptors are known

`src/core/peinterop.cs` maps class descriptors for the camera types (FFX column,
RVA from image base `0x400000`):

| Type | Class descriptor |
| --- | --- |
| `PCamera` | `0x8922C0` |
| `PCameraProjection` | `0x892358` |
| `PCameraOrthographic` | `0x8930E8` |
| `PCameraPerspective` | `0x893608` |
| `PCameraControllerComponent` | `0x8AFF10` |
| `PPhysicsCharacterCamera` | `0x8BE940` |

## The iteration machinery exists

`FhPFilteredInstanceList<T>` and `FhPFilteredInstanceListIterator<T>` enumerate
every live PhyreEngine object of a given type, which is how we would reach the
active camera without hardcoding a pointer.

## The single missing piece

`FhPhyreUtil.get_instance_list_filter_for<T>()` only knows two filter functions:

| Type | Filter function |
| --- | --- |
| `PTexture2D` / `PTexture2DD3D11` / `PTexture2DBase` | `0x64F30` |
| `PTexture3D` / `PTexture3DD3D11` / `PTexture3DBase` | `0x64FA0` |

Everything else throws `NotImplementedException`. So to enumerate cameras we need
the address of the instance-list filter function specialized for `PCamera`.

This is upstream issue
[#209](https://github.com/fahrenheit-crew/fahrenheit/issues/209) — "Instance list
filter functions need to be mapped for all remaining `P*` types of interest."

## How to find it

The two known filters sit at `0x64F30` and `0x64FA0`, 0x70 apart, which suggests
a family of generated per-type functions in the same region. Two approaches:

1. **Scan for references to the class descriptor.** A filter specialized for
   `PCamera` should load `0x8922C0` (absolute `0xC922C0`). Searching `.text` for
   that immediate gives a small candidate set; the one shaped like the texture
   filter at `0x64F30` is ours.
2. **Disassemble the known filter** at `0x64F30` to learn the pattern, then look
   for the same shape nearby.

Approach 1 is cheap and can be done by parsing the executable directly.

## Then what

Declare `PCamera` with the offsets above, enumerate instances, and display the
live view and projection matrices in the mod's ImGui window. If those numbers
move sensibly while walking around Besaid, we have the camera — and the VR
renderer has something real to attach to.

## Also worth knowing

- `MsCameraMoveAcc` and `MsCameraMoveFrame` are named FFX camera functions in the
  call tables; they may be useful for detecting camera transitions and cuts.
- `src/core/ffx/ids/camera.cs` holds FFX's battle camera target ids
  (`CAM_CHR_ACTIVE`, `CAM_CHR_TARGET`, party slots and so on), sourced from
  `camera.ath`. Relevant later for the battle viewpoint work.
