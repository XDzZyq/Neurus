# Debug Objects

`DebugLine`, `DebugPoints` and `DebugMesh` are retained, stateful, first-class scene objects that visualize data in the viewport - pooled, serialized and selectable like any Mesh or Light, not per-frame commands.

## Retention model

Viewport debug geometry is retained and stateful, matching the Qt widgets it sits next to. Something is drawn because an object exists and says so, never because a function was called this frame.

- Each debug object is a first-class Scene object: it is pooled in the `ResourceManager`, serialized, selectable and undoable like any other object.
- `DebugDrawBuilder` (owned by `EditorViewport`) is the single flattener: it walks the Scene's three debug pools into a `DebugDrawList` on a dirty flag, never per frame.
- Hiding a debug object means not flattening it. `DebugDrawList` has no per-primitive enable bit, so the builder drops objects whose `is_viewport && is_rendered` is false, before the x-ray partition is computed.
- Smoothing is derived, never authored, and on by default: nothing stores a smooth bit. The builder decides it per primitive kind - a solid line and every cube edge get `DebugFlag::Smooth`; a stippled line does not (antialiasing would soften the hard ends a dash is made of); point sprites always do; a `DebugMesh` wireframe never does.

See [Overlays](overlays.md) and [Viewport](../editor/viewport.md).

## DebugLine (`GO_DL`)

`scene/DebugLine.h` stores a collection of vertex positions interpreted as line-segment endpoints, plus rendering properties. It inherits `ObjectID` and `Transform3D`.

- Vertex storage is `std::vector<glm::vec3>` - pure CPU data, no GPU resources or shaders.
- `PushDebugLine(start, end)`, `PushDebugLines(vertices)` (interpreted in pairs; an odd count leaves an incomplete last segment), `ClearVertices()`, and the absolute setter `SetVertices(vertices)` that the property editor and the undoable op carry.
- `GetVertexCount()` and `GetVertices()` read the storage back.
- Properties: color, width (pixels), opacity, stipple (dashed flag), and x-ray (skip the depth test so the lines are never occluded).
- The line style is solid or stipple; antialiasing is not an authored property but follows that style, applied to solid lines only.
- Vertices are stored in the transform's local space and the builder bakes `GetModelMatrix()` into the world-space overlay payload, so the whole line can be moved, rotated and scaled as a unit. `GetTransform()` is overridden to make the object reachable by the gizmo, the property panel and the transform update pass.

## DebugPoints (`GO_DP`)

`scene/DebugPoints.h` stores point positions with their rendering attributes. It inherits `ObjectID` and `Transform3D`.

- `PointType` selects the sprite shape: `SQUARE`, `RHOMBUS`, `CIR` (circle), `CUBE` (volumetric). The shape ids are kept numerically in sync with `OverlayPointShape`.
- `PushDebugPoint(point)`, `PushDebugPoints(points)`, `ClearPoints()` and the absolute setter `SetPoints(points)`; positions are local-space, relative to the transform (baked in by the builder like `DebugLine`).
- Properties: point type, color, scale (sprite diameter - pixels when projection mode is 0, world units otherwise), opacity, projection mode (`0` = screen space/constant pixel size, `1` = world space/shrinks with distance), and x-ray.
- Point sprites are always smoothed, because their shape mask is an analytic distance; a rhombus or circle is visibly stepped without it. A cube has no sprite form and the producer decomposes it into 12 segments.
- `GetTransform()` is overridden like `DebugLine`.

## DebugMesh (`GO_DM`)

`scene/DebugMesh.h` draws pooled geometry as a wireframe overlay instead of a shaded surface, completing the debug-object family. It inherits `ObjectID` and `Transform3D`.

- References a pooled `MeshData` exactly like `Mesh` does (`o_mesh` plus `o_meshDataId`), so it reuses the existing `MeshGPU` upload path and the `ResourceComponent` re-wiring on project load.
- Has no `Material` and no per-object `Shader`; its appearance is fully described by color, opacity and x-ray.
- Line width is deliberately absent: the wireframe is rasterized with `polygonMode = eLine`, whose width is fixed at 1 px wherever the wide-lines feature is unavailable (notably MoltenVK/Metal, which clamps `lineWidthRange` to `[1, 1]`). Use `DebugLine` when thickness is required.
- `SetMeshData(...)` points it at pooled geometry and keeps `o_meshDataId` in sync; `HasGeometry()` reports whether anything will be drawn.
- A `DebugMesh` wireframe never gets the `Smooth` flag, so its fragment stage has no branch on it.

## Upload

A `DebugMesh` wireframe still needs a `MeshGPU` in the `RenderCache`, uploaded by the Editor through `UploadManager::UploadMeshData()`. `DebugMesh` is not a `Mesh`, so it cannot reuse `UploadMesh()`; without the upload the pass skips the mesh silently.

Upload of the flattened list is revision-gated: an unchanged list re-uses the buffer a frame slot already holds and skips the memcpy entirely. `CPUBuffer` is the permanent host-visible buffer type for this data; `StagingBuffer` is for uploads and downloads only and must never be retained as a permanent buffer. See [Buffers](../render/buffers.md).

## Rendering

`DebugPass` is the single consumer and runs last, after post-AA: it draws over the final shaded image with `LOAD_OP_LOAD` (the FXAA output when FXAA is on, the composed output when it is off), reads but never writes the G-Buffer depth, and flips `eDepthTestEnable` as dynamic state to serve x-ray. Segments and points are covered by four draws (two depth-tested, two x-ray) regardless of primitive count or authoring order, with the wireframe drawn separately; the overlay convention caps the pass at at most 6 draws per frame. Every debug primitive is already antialiased analytically, so handing FXAA the scene alone keeps both correct.

The frame driver publishes; passes read. `DeferredRenderer::recordFrame()` writes the `RenderCache`'s camera and debug/gizmo caches once per frame before any pass records, and `DebugPass` only binds them. A test driving a pass directly stands in for that driver.

## Serialization

Each debug object serializes its `ObjectID` and `Transform3D` bases directly, then reads its value fields with `archive::Optional` - including `m_vertices` / `m_points` and `m_meshDataId`. `DebugMesh` stores the pooled `MeshData` id rather than the pointer, which is re-wired on load. See [Serialization](../core/serialization.md).
