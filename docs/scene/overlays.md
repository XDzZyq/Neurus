# Overlays

Screen-space overlays share one set of vertex records but travel in two separate payload containers, because the debug overlay and the transform gizmo have opposite lifetimes and authoring rules.

## Shared records (`render/overlay/OverlayGeometry.h`)

`OverlayGeometry.h` defines the primitives consumed verbatim by the overlay shaders as std430 SSBO arrays. It lives in the Vulkan-free `render/overlay/` tier, beside the payload containers and the passes that read them; the editor *authors* the records (it may include render, never the reverse) and hands the containers to the renderer through the context, so neither side includes the other.

- `OverlaySegment` - one line segment, expanded into a screen-space quad by the vertex shader. Fields: `a` and `b` (world-space endpoints), `width` (pixels), `rgba` (packed color), `flags`, and `_pad[3]`. 48 bytes, std430-aligned. Thick lines are quads rather than wide lines because Metal/MoltenVK caps `lineWidth` at 1.0 and exposes none of `VK_KHR_line_rasterization`'s modes; quad expansion gives arbitrary width plus shader-side smoothing and stipple on every platform.
- `OverlayPointSprite` - one point drawn as a native point-list primitive. Fields: `p` (world-space position), `size` (sprite diameter), `rgba`, `shape`, `flags`, `_pad`. 32 bytes. `size` becomes `gl_PointSize` (requires the largePoints feature) and `shape` masks `gl_PointCoord`. A cube has no sprite form and is decomposed by the producer into 12 segments.
- `OverlayFlag` - plain `uint32_t` bit constants (written straight into an SSBO field and tested with bitwise AND in GLSL): `None`, `XRay` (draw on top, depth test disabled), `Stipple` (dashed line), `Smooth` (anti-alias edges by fading alpha), `ScreenSpaceSize` (size in pixels rather than world units). Point sprites honour both size settings; segments do not - segment width is always pixels and this bit is ignored for them.
- `OverlayPointShape` - sprite shape ids (`Square = 0`, `Rhombus = 1`, `Circle = 2`), kept numerically in sync with `DebugPoints::PointType`.
- `PackOverlayColor(color)` - packs a linear RGBA color into `0xAABBGGRR` (matching GLSL `unpackUnorm4x8`). Overlay geometry is authored display-referred and drawn after tonemapping, so no gamma conversion happens here.

Positions are always world-space: producers bake their object's transform in while flattening, rather than passing a matrix index the shader would dereference. Field order and padding must stay in sync with `overlay_line.vert` and `overlay_point.vert`; the `static_assert`s guard sizes, not field order.

## DebugDrawList (`render/overlay/DebugDrawList.h`)

`DebugDrawList` is the single transport for all viewport debug geometry, and there is no immediate-mode API behind it: everything is a retained, stateful object that lives as long as what it visualizes.

- Produced by scene debug objects (`DebugLine`, `DebugPoints`, `DebugMesh`) and editor-owned visualizers (camera frustum, light frustum, selection bounds).
- Holds `segments` (`DebugSegment`), `points` (`DebugPointSprite`) and `wireMeshes` (`DebugWireMesh`).
- `DebugWireMesh` describes a wireframe drawn from an already-uploaded `MeshGPU` (using `VK_POLYGON_MODE_LINE`), carrying only `model`, `rgba`, `flags` and `meshObjectId` - no geometry is duplicated or re-uploaded.
- Segments and points are partitioned during assembly so all depth-tested primitives precede all XRay ones; `xraySegmentStart` and `xrayPointStart` record the split.
- `revision` is bumped by `Touch()` whenever the contents change. Debug objects are stateful, so on most frames the list is identical to the previous one; the renderer remembers the revision it last uploaded per frame-in-flight and copies only on a mismatch. The revision is never reset, so it cannot repeat a value the renderer has already seen.
- `Empty()` lets the pass skip entirely; `Clear()` drops geometry but keeps capacity and does not `Touch()` (the caller refills and touches once).

The debug-domain names `DebugFlag`, `DebugSegment`, `DebugPointSprite` and `DebugPointShape` are aliases of the shared records, so `DebugCache` and `GizmoCache` share a memcpy shape.

`DebugDrawBuilder` (owned by the viewport) is the single flattener; see [Viewport](../editor/viewport.md) and [Debug Objects](debug-objects.md).

## GizmoDrawList (`render/overlay/GizmoDrawList.h`)

`GizmoDrawList` is the interaction counterpart to `DebugDrawList`: transient feedback for a modal transform gesture, published while a gesture is active and rebuilt on every cursor move and camera change.

- Holds `segments` (`OverlaySegment`, constraint axis lines and rotation-arc chords) and `points` (`OverlayPointSprite`, the pivot marker and handle dots), all world-space.
- Deliberately smaller than `DebugDrawList`, and each omission is a decision: no `revision` / `Touch()` (the peak payload is tiny, so the cache writes it unconditionally); no `xraySegmentStart` / `xrayPointStart` (every guide is unconditionally x-ray, so there is nothing to partition); no wire meshes (a gizmo guide is never a mesh).
- Draw order matters: with depth testing off and alpha-over blending, whatever is submitted last lands on top, so segments are drawn before points and the pivot dot sits above the axis line.
- `Empty()` lets the pass skip; `Clear()` keeps capacity.

The list is published **unconditionally** through `RenderContext::gizmoDraw` - interaction feedback is not hideable by a debug toggle - so during normal operation the pointer is non-null and *empty* rather than null when nothing is armed, and the pass early-outs on `Empty()`.

## Why two containers

The two have opposite lifetimes (scene-object lifetime versus one gesture) and opposite authoring rules (retained, revision-gated, toggleable versus rebuilt every move and never hideable). Sharing one payload would mean one revision counter and one enable flag serving both, which is exactly the coupling that made a drag re-upload every scene debug primitive. What they legitimately share is the record layout and therefore the shader program. Both containers live in the Vulkan-free `render/overlay/` tier; `RenderContext` forward-declares them.

## Publication and null policy

`RenderContext` (`render/RenderContext.h`) carries `const DebugDrawList* debugDraw` and `const GizmoDrawList* gizmoDraw` as typed forward-declared pointers, produced once per frame by `Editor::UpdateRenderContext()`:

- `debugDraw` is null when `r_debug_draw` is off or when no scene is published; consumers must check it. See [Render Config](../core/render-config.md).
- `gizmoDraw` is published unconditionally and checked the same way.
- Both pointers are borrowed and frame-scoped: valid only for the frame that produced them, never stored across frames.

See [Data Flow](../architecture/data-flow.md).
