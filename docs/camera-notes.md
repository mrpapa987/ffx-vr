# Finding the camera

Research notes, 16 October 2026. FFX runs on PhyreEngine, and Fahrenheit has
already reverse-engineered much of what we need — but not all of it is reachable
from a mod.

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

**Caveat:** all commented out. They document the layout rather than declaring
usable types, and should be validated against the live game before being trusted.

## Class descriptors are known

`src/core/peinterop.cs`, FFX column, RVA from image base `0x400000`:

| Type | Class descriptor |
| --- | --- |
| `PCamera` | `0x8922C0` |
| `PCameraProjection` | `0x892358` |
| `PCameraOrthographic` | `0x8930E8` |
| `PCameraPerspective` | `0x893608` |
| `PCameraControllerComponent` | `0x8AFF10` |
| `PPhysicsCharacterCamera` | `0x8BE940` |

## The instance-list filters are type-generic

Upstream issue [#209](https://github.com/fahrenheit-crew/fahrenheit/issues/209)
treats the instance-list filter functions as something to map per type.
`get_instance_list_filter_for<T>()` knows only two and throws for everything else:

| Type | Filter |
| --- | --- |
| `PTexture2D` / `PTexture2DD3D11` / `PTexture2DBase` | `0x64F30` |
| `PTexture3D` / `PTexture3DD3D11` / `PTexture3DBase` | `0x64FA0` |

**Analysis of the binary says the per-type mapping is unnecessary.** The two
filter functions are byte-identical apart from three relative call displacements,
and those resolve to:

- a type-check predicate (`0x659E0` for the first, `0x65A10` for the second)
- the same shared helper `0x41DC0` in both

The two predicates are themselves **byte-identical**. Disassembled:

    mov edx, [ecx+0x18]   ; list->ptr_instance_list_next
    mov eax, [edx+0x2C]   ; instance_list->ptr_class_descriptor
    mov ecx, [ecx+0x1C]   ; filter->ptr_class_descriptor   <-- from the struct
    cmp eax, ecx
    mov eax, [eax+0x40]   ; walk the base-class chain
    ...

The type test reads the descriptor **out of the `FhPFilteredInstanceList` struct**,
which C# already populates via `get_class_descriptor_for<T>()`. Nothing is
compiled in. These are duplicate template instantiations that behave identically,
so **`0x64F30` works for any type, including `PCamera`.**

This is worth reporting upstream: #209 may need no address hunting at all, just a
fallback to a known-good generic filter.

## What blocks us

Two things.

**1. The interop is `internal`.** `FhPDoubleListIterator`, `FhPFilteredInstanceList`,
`FhPFilteredInstanceListIterator` and `FhPhyreUtil` are all `internal` to the
Fahrenheit core. `FhApi` — the mod-facing surface — exposes Mods, Localization,
Resources, Gui, Audio, Input, Events and Saves, but nothing for PhyreEngine
objects. A mod cannot enumerate engine instances using Fahrenheit's helpers.

**2. We don't have the live scene's instance-list root.** The one call site
(`src/runtime/resources.cs`) walks a cluster it loaded itself by path:

    using FhPClusterScope cluster_scope = _plm.cluster_load(tex_path);
    cluster_scope.enter(out PCluster* ptr_cluster);
    FhPDoubleListIterator<PInstanceList> iter = new(&ptr_cluster->_0x1C_instance_lists);

That finds objects in a *loaded asset cluster*, not the live rendering scene. We
would still need wherever the active scene's cluster or camera lives.

## What is reachable from a mod

`FhCall` **is public**, so a mod can hook any named game function. Two camera
functions are mapped:

    d_MsCameraMoveFrame(uint camera_id, uint, uint, uint frame_count, uint)  @ 0x3BDE50
    d_MsCameraMoveAcc  (uint camera_id, uint mode_non_ref, uint mode_polar, ...) @ 0x3BD860

Note they take a **camera id**, not a pointer — these are the ATEL script layer.
Hooking them tells us when the camera moves and which camera is addressed, but
not where its matrices live.

## Options from here

1. **Reimplement the Phyre interop inside our mod.** Declare our own
   `PInstanceList` / `PClassDescriptor` / iterator equivalents and call `0x64F30`
   directly. Perhaps 200–300 lines. Still needs the live scene root.
2. **Hook the D3D11 constant buffers instead.** Intercept the view-projection
   matrix on its way to the GPU. Engine-agnostic, the standard approach for VR
   mods, and independent of all the above. Needs the native DLL we were going to
   write anyway.
3. **Contribute upstream.** Implement #209 (with the generic-filter finding) and a
   public API for engine objects, then consume it from the mod. Slower, helps
   everyone, and we already build Fahrenheit from source.
4. **Ask the Fahrenheit Discord** where the live camera lives. Cheapest if they
   know; they built all of this.

## Also worth knowing

`src/core/ffx/ids/camera.cs` holds FFX's battle camera target ids
(`CAM_CHR_ACTIVE`, `CAM_CHR_TARGET`, party slots), sourced from `camera.ath`.
Relevant later for the battle viewpoint work.
