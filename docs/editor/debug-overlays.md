# Debug Overlays

Viewport debug geometry in Neurus is retained and stateful, not immediate-mode:
because Qt is stateful, debug objects are too.

## First-class scene objects

`DebugLine`, `DebugPoints` and `DebugMesh` are first-class Scene objects: pooled,
serialized and selectable like any Mesh or Light, not per-frame commands.

`DebugDrawBuilder` (owned by `EditorViewport`) is the single flattener: it walks
the Scene's three debug pools into a `DebugDrawList` on a dirty flag
(`MarkOverlaysDirty()` from `RenderResetEvent`, `RebuildOverlays()` at the end of
`Editor::Edit()`), never per frame.

`DebugPass` (renderer) is the single consumer and runs in the overlay graph, over
the converged scene image: it draws into `OverlayOutput` with `LOAD_OP_LOAD`
(seeded by `OverlayCopyPass` from `ComposedOutput`, or `FXAAOutput` when FXAA is
on), reads but never writes the G-Buffer depth, and flips `eDepthTestEnable` as
dynamic state to serve x-ray, at most 6 draws per frame regardless of primitive
count. Every debug primitive is already antialiased analytically, so handing FXAA
the scene alone keeps both correct. The transform gizmo's `GizmoPass` then runs
after `DebugPass`; see [Transform gizmo](gizmo.md).

Because the renderer stops drawing once its sampling budget is spent, a rebuilt
overlay payload does not reach the screen by itself. `RebuildOverlays()` reports
whether it rebuilt anything, and `Editor::Edit()` posts a
`RedrawOverlaysRequest` when it did - the one request that redraws the overlay
graph over the converged image instead of restarting accumulation. An idle tick
posts nothing. See [Temporal](../render/temporal.md) and
[Render Request Path](../architecture/data-flow.md#render-request-path-editor-to-renderer-gpu-work).

Upload is revision-gated: an unchanged list re-uses the buffer a frame slot
already holds and skips the memcpy entirely.

## DebugDrawBuilder

`DebugDrawBuilder` is the only place that walks `Scene::dLine_list`,
`dPoints_list` and `dMesh_list` and flattens them into the `DebugDrawList` that
the frame snapshot carries to `DebugPass`. It is a plain member of
`EditorViewport` (`m_debugDraw`), not a controller: it has no events of its own.
The Editor reaches it only through `MarkOverlaysDirty()` /
`RebuildOverlays(scene)`, and never sees the builder itself.

Lifecycle, dirty-flagged, never per-frame polling:

```
RenderResetEvent  --> viewport.MarkOverlaysDirty()  // O(1), the existing
                                                    // "something visible changed" broadcast
Editor::Edit()
  |- EventQueue::Process()
  |- m_viewport.RebuildOverlays(*m_scene)  // after the queue drains, so a scene
  |                                        // change and its overlay consequences
  |                                        // land in one frame
  `- if it rebuilt anything: Post(RedrawOverlaysRequest{})
```

`RebuildOverlays()` answers `true` when at least one payload was actually
rebuilt, which is the only answer the Editor cannot work out itself - the dirty
flags belong to the builders. Posting inside `Edit()` matters: the request
lands before this tick's snapshot is published, so the redraw happens on that
tick's frame rather than costing one.

`Rebuild()` returns immediately when clean, which is the common case: debug
objects are stateful and rarely move. `m_dirty` starts `true` so the first
`Edit()` populates the list. `BeginLoad()` / `FinishLoad()` / `NewScene()` /
`CreateDefaultScene()` all `MarkDirty()`, because a scene swap invalidates the
whole flattened overlay and new content is not in it yet.

## Four contracts the renderer trusts

These are pinned by `test/editor/test_debug_draw_builder.cpp` and cannot be
checked by a GPU test:

1. Partition. Every depth-tested primitive precedes every x-ray one;
   `xraySegmentStart` / `xrayPointStart` mark the boundary exactly. X-ray
   primitives are staged in member scratch vectors (`m_xraySegments` /
   `m_xrayPoints`, members so their capacity survives across frames, cleared at
   the top of every `Rebuild`) and concatenated at the end, so no sort is ever
   needed. `DebugPass` turns those two integers straight into draw ranges, so an
   off-by-one draws x-ray geometry with the depth test still on while every draw
   and pixel count still looks plausible. Wire meshes are deliberately not
   partitioned: there are few of them and `DebugPass` already switches pipeline
   per mesh.
2. Revision. Exactly one `Touch()` per rebuild. `DebugDrawList::Clear()`
   deliberately does not `Touch`, so a clear+refill counts as one revision. A
   missing `Touch` freezes the overlay; a double `Touch` re-uploads every frame.
3. World-space baking. `DebugDrawList` carries no matrix for segments or sprites,
   so the builder folds each object's `GetModelMatrix()` in while copying. Only
   `DebugWireMesh` keeps a `model` matrix, because its geometry is never copied:
   `DebugPass` draws it straight from the mesh's `MeshGPU` with the transform in a
   push constant.
4. Visibility filtering. A hidden object is not flattened (see below).

## Hiding a debug object

Hiding a debug object means not flattening it. `DebugDrawList` has no
per-primitive enable bit (`DebugPass` draws whole ranges), so the only way to hide
a debug object is to leave its primitives out of the list, and the filter has to
run before the x-ray partition so the two boundary integers count only surviving
primitives. `IsVisible()` ANDs `is_viewport` and `is_rendered`, the same way
`GeometryPass`, `ShadowDepthPass` and `UploadManager` AND them: the overlay is
editor-only so `is_viewport` is the flag that obviously applies, but honouring
only one of the pair would make the Outliner's two toggles mean different things
for a debug row than for a mesh row. Nothing extra is needed to notice a toggle:
`SceneController::OnVisibilityChanged` already calls `Mutated()`, which enqueues
`RenderResetEvent` to `MarkDirty()`, and undo replays the same event through
`SetVisibilityOp`.

## Flattening rules

- `DebugLine` vertices are endpoint pairs; a trailing odd vertex is dropped and a
  lone vertex emits nothing.
- Smoothing is derived, never authored, and on by default. The Scene stores no
  smooth bit, `DebugProperties` shows no checkbox, and there is no event or op for
  it; the builder decides per primitive kind:
  - solid `DebugLine` gets `Smooth`; stippled does not
    (`flags |= GetStipple() ? Stipple : Smooth`, so the two are never both set).
    Antialiasing fades alpha towards the quad edge, which would soften the hard
    ends a dash is made of.
  - point sprites always get `Smooth`, in both projection modes. `DebugPoints` has
    no line style to protect, and `overlay_point.frag`'s shape mask is an analytic
    distance (Chebyshev / Manhattan / Euclidean), so a rhombus or circle boundary
    is visibly stepped without the fade.
  - CUBE edges get `Smooth`, like any solid segment.
  - `DebugMesh` wireframes never get `Smooth`. They rasterize through
    `PolygonMode::eLine`, which hands the fragment stage no distance-to-edge, so
    `debug_wire.frag` has no branch on the flag; setting it would advertise a
    behaviour no consumer implements.
- Opacity is folded into the packed alpha (`PackTinted`), because the GPU only
  ever sees one alpha; opacity is an authoring convenience, not a second channel.
- `DebugPoints::PointType::CUBE` has no sprite form (a screen-aligned sprite cannot
  show a cube's orientation), so it decomposes into 12 axis-aligned wireframe
  segments of width 1.0 with half extent `GetScale() * 0.5f`. `ScreenSpaceSize` is
  deliberately not set on them: their size is world-space by definition.
- `ScreenSpaceSize` is set for sprites when `GetProjectionMode() == 0`. It is
  meaningless for segments, whose width is always pixels.
- A `DebugMesh` without geometry (`!HasGeometry()`) is skipped: the wireframe is
  drawn from its `MeshGPU`, which does not exist until the geometry is uploaded.

Ordering caveat for tests and callers: `Scene::ResPool` is an `unordered_map`, so
the order of primitives coming from different objects is unspecified. Never depend
on an absolute index; assert over counts, boundary values, or predicates keyed off
the boundary itself.

## MeshGPU upload for DebugMesh

A wireframe needs vertex/index buffers in the `RenderCache`, keyed by the object
id `DebugWireMesh::meshObjectId` carries. Without them `DebugPass` silently skips
the mesh, so this is the difference between a wired feature and an inert one. The
Editor posts an `UploadMeshRequest{objectId, shared_ptr<const MeshData>}` rather
than uploading itself; `DebugMesh` and `Mesh` carry the same `MeshData` and land
in the same cache slot, so one request type serves both and the renderer's
executor runs it skip-if-cached (`UploadManager::UploadMeshData(const MeshData&)`
behind the scenes). Three places must stay in step:

| Site | Role |
|------|------|
| `PostMeshUpload()` (Editor.cpp anon namespace) | the single post path, shared by `Mesh` and `DebugMesh`; residency is the renderer's fact, so it posts unconditionally |
| `Editor::UploadSceneResources()` | `dMesh_list` loop, for objects present at project load |
| `Editor::OnSceneObjectGpuUpload()` | `Get<DebugMesh>` branch, for on-demand upload of an object entering the scene |

`ResourceComponent::Load` needs a matching `ForEach<DebugMesh>` block to re-wire
`o_mesh` from `o_meshDataId`: the `ForEach<Mesh>` block above it does not reach a
`DebugMesh`, so without it a reloaded project leaves the wireframe geometry-less
and the upload is skipped for a reason that looks like a rendering bug.
