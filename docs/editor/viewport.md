# Viewport

`EditorViewport` is the composition root for the editor's 3D view: it owns the
viewport camera, the world-to-screen conversions that camera defines, and the
overlay builders that draw over it. Projected coordinates follow the codebase's
[Coordinate System](../core/coordinate-system.md) (Z-up, +Y forward,
right-hand).

## The composition root

`src/editor/viewport/EditorViewport.h/cpp` is the viewport composition root. It
owns:

- the viewport camera
- the `DebugDrawBuilder`
- the `TransformGizmo` state machine
- the `GizmoDrawBuilder`

It handles the resize and provides the world-to-screen service: `Project`,
`RayThrough`, `ProjectBox`, and `PixelsPerWorldUnit`. It is Vulkan-free and
Qt-free, and layer-wise it belongs to the editor.

The class is the composition root for its collaborators, not their
implementation. `DebugDrawBuilder`, `TransformGizmo` and `GizmoDrawBuilder` all
compute against this camera and this extent (a fixed pixel budget means nothing
without them), so they are members here, and the surface this class adds for them
is four forwarding calls and two list accessors.

The one thing it does not own is the `Scene`. The scene arrives as a parameter to
the two per-frame calls that need it, so nothing here dangles across File > New
or File > Open and the class stays ignorant of what a scene contains.

Object queries (screen position, screen bounding box) are free functions in the
same header rather than members, so the class stays ignorant of scene types and
is testable against a bare `Camera`. The mutators (extent, camera, cursor,
overlay dirty flags) are the Editor's alone, which is why controllers receive a
`const EditorViewport*`.

## The view camera

Every view-projection pass reads the frame's camera (`ctx.Frame().camera`)
(`ShadowDepthPass`, `ShadowIntensityPass`, `SSAOPass`, `LightingPass`, plus
`GeometryPass` through the shared `CameraGPU`), and no pass reads the Scene.
The Editor chooses that pointer:

```
Editor::ActiveCamera()       = scene.GetActiveCamera()  // activated scene camera
                               ?: viewport.ViewportCamera()
Editor::PublishActiveCamera() -> viewport.SetCamera(active)
EditorViewport::GetCamera()  = published pointer
```

This is selection, not synchronization. The viewport camera and an activated
scene camera stay independent: no pose or aspect is copied between them. When a
scene camera is active it is the camera the frame renders through; when it is
deactivated the viewport camera takes over at the pose and aspect it already had.

`Editor::Edit()` publishes before and after `ed_eventBus.Process()`, because
activation, deactivation, deletion and undo/redo all land inside the drain and can
change which camera the frame belongs to. A `SceneObjectDeleteRequested`
subscription republishes earlier, right after the controllers remove the object,
so nothing in the rest of that dispatch reads a camera that is no longer the
view. Publishing is a pointer comparison, not per-frame camera work.

That is also why `GetCamera()` is a published pointer rather than a fresh choice
per call: the gizmo's projection math and the renderer's `CameraGPU` read the same
one pointer, so they cannot disagree about which camera the frame belongs to. A
test that activates or deactivates by hand must therefore name the frame
boundary itself (`test_viewport_camera.cpp`'s `PublishFrame` helper).

### A scene with zero cameras is legal

A scene containing zero cameras is a legal document, and deleting every camera in
one is a legal gesture. There is no last-camera guard and nothing is injected on
load to repair a camera-less project.

### The viewport camera is pooled but not scene content

The viewport camera lives in the `ResourceManager` so
`CameraController::ResolveCamera` can resolve and orbit it by UID like any other
camera, and it is absent from `cam_list`, so it cannot be selected, deleted, or
activated. `Scene::ActivateCamera` validates membership in `cam_list` and refuses
it.

### It cannot be created once in the constructor

`m_resources->Clear()` runs in both scene-swap paths and drops every pooled
object, so `EditorViewport::EnsureCamera(ResourceManager&)` (resolve the stored
uid through the pool, else `Load<Camera>()` at `kDefaultEye` looking at
`kDefaultTarget`) runs on each of them. The `ResourceManager` is a parameter, not
a stored reference, so the viewport stays default-constructible for
bare-construction test sites:

- `NewScene()`: after `Clear()`, `ForgetCamera()`, then `EnsureCamera()`. The uid
  therefore changes on every File > New, by design. This is also what makes an
  empty scene renderable at all.
- `BeginLoad()`: after `Clear()`, `ForgetCamera()` drops the reference and the
  uid.
- `FinishLoad()`: `EnsureCamera()`. A uid restored from the file resolves to the
  serialized camera; a missing one mints a fresh camera.

`EnsureCamera` frames the camera it establishes, so a freshly created viewport
camera never renders with its default 1x1 projection.

### Identity is serialized, so the viewpoint survives a reopen

The camera object rides along in `ResourceComponent`'s wholesale pool
serialization. `project::EditorComponent` (key `"m_editor"`, field
`viewportCamUid`, registered last in `Application::BuildProject` because
registration order is archive read order) records only which pooled camera the
viewport claims as its own. It binds to the `EditorViewport`, not to the Editor,
the Editor has no camera API left to ask, which also narrows the sanctioned
reverse asset-to-editor dependency to `editor/viewport/EditorViewport.h`. Without
that uid, `FinishLoad()` would mint a fresh camera at the default framing and the
view would snap back on every open. A project written before the component
existed simply lacks the node; `Load()` falls back rather than throwing, and a
project written before the rename carries the field as `editorCamUid`, which
`Load()` still reads (an explicit migration, spelled as a second `Optional()`).

## Resize and framing

The viewport extent outlives the scene and reaches exactly the camera the frame
renders through.

`EditorViewport::HandleResize(logical, renderExtent)` caches both extents and
dirties the gizmo guide, whose geometry is a fixed pixel budget against the
viewport height. It writes no camera at all. The extent is a property of the
viewport, not of whichever scene happens to be loaded, and it arrives once at
startup, long before any File > New. A zero extent is the one thing dropped rather
than stored: it is what the window reports before it is first shown, and it would
divide by zero in the projection, so the previous extent is a better answer than
none.

The resize reaches a camera through the existing controller path:

```
Application::ResizeViewport
  -> Editor::HandleViewportResize(logical, render)
       -> EditorViewport::HandleResize(...)          // cache extent, no camera write
       -> CameraResizeEvent{ activeCameraUid, logical }
  -> CameraController::OnCameraResize
       -> ChangeCamRatio(...)
```

`PublishActiveCamera()` emits the same `CameraResizeEvent` whenever the active
camera changes - activation, deactivation, or load - so a fresh 1x1 scene camera
gets the current aspect once on becoming the view, and the viewport camera gets
the current aspect back on deactivation. One event type covers both resize and
camera handoff, and `CameraController` is the only thing that writes a camera's
aspect. It still receives only a UID, so it never needs to know whether the camera
is scene content or the viewport's own.

The inactive camera is never touched, whichever kind it is. `FrameCamera()` remains
the initial framing used by `EnsureCamera()` (a freshly created viewport camera is
1x1); it no-ops while the extent is still 0x0 at startup.

`test_editor_scene_lifecycle.cpp::NewProject_SeededCameraAdoptsTheViewportExtent`
pins the seeded-camera half.
`test_viewport_camera.cpp::ResizeFramesOnlyTheActiveViewportCamera` and
`ResizeFramesOnlyTheActivatedSceneCamera` pin the one-camera half in both states,
`ActivationFramesTheSceneCameraToTheCurrentViewport` pins the handoff, and
`ZeroResizeIsIgnored` pins the dropped extent.

## Screen-space queries

The class is one object answering every "where is this on screen" and "what is
under the cursor" question in the editor. Before it existed, that knowledge was
spread across the Editor's viewport width/height members, an open-coded
device-ratio multiply in the Application's pick path, and whatever each caller
derived from `Scene::GetActiveCamera()`, three places that could disagree.

```cpp
struct ScreenPoint { glm::vec2 pixel; float viewDepth; bool visible; };
struct ScreenRect  { glm::vec2 min; glm::vec2 max; bool valid; bool clamped; };
struct ScreenRay   { glm::vec3 origin; glm::vec3 direction; bool valid; };
```

- `Project(world)` projects a world point to a logical pixel. `viewDepth` is the
  clip-space w the perspective divide used, which is the quantity
  `PixelsPerWorldUnit()` needs. `visible` is true when the point is in front of
  the near plane; it does not mean "inside the viewport", so an off-screen point
  in front of the camera is visible with pixel coordinates outside the viewport.
- `RayThrough(logicalPixel)` builds the world-space ray through a pixel, the
  exact inverse of `Project()`. Its origin is on the near plane rather than at the
  eye, because that falls straight out of unprojecting NDC z = 0.
- `ProjectBox(model, localMin, localMax)` returns the screen-space bounds of a
  transformed local box, near-plane correct. Exact rather than conservative: the
  projection of a convex box is the convex hull of its projected vertices. The
  box is taken explicitly instead of read off a mesh, so it is testable before any
  AABB exists in the asset layer. The rect is deliberately not clipped to the
  viewport ("this object extends off-screen to the left" is information callers
  need), but coordinates are capped a few viewports away by the guard band
  (`kGuardBandViewports`, capped to `[-4*size, +5*size]`) so a box straddling the
  near plane stays finite, and `clamped` records when that happened.
- `PixelsPerWorldUnit(vp, viewDepth)` is the closed form
  `|proj[1][1]| * height * 0.5 / viewDepth`. It is aspect-independent by
  construction, which is what makes constant-pixel gizmo geometry come out the
  same size on every window shape.
- Free functions `ProjectBounds` (thin wrapper over `ProjectBox`) and
  `ScreenPosition` (screen position of any scene object that carries a transform)
  are kept out of the class so `EditorViewport` needs no knowledge of scene types.
  `ScreenPosition` returns an invisible `ScreenPoint` for an object with no
  transform (an Environment, say) rather than guessing an origin. A
  `ScreenBounds(ObjectID)` is deliberately absent.

## Everything is in logical pixels

`Camera::cam_w/cam_h` are logical Qt pixels; the renderer's swapchain extent is
physical. This is not a discrepancy that needs a DPI factor sprinkled through the
math: `Camera::RecomputeMatrices()` puts aspect in `proj[0][0]` and the FOV term
in `proj[1][1]`, so `|proj[1][1]|` is identical whichever extent you build the
camera from, and a world length therefore maps to the same fraction of screen
height either way. Pixel results simply come out in the same unit as the size you
were given. `ToRenderPixels()` is the one deliberate exit from that world, for
the ID-buffer readback that must address physical texels.

No Y flip lives here. `Camera.cpp` applies `proj[1][1] *= -1` for Vulkan NDC
(`y = -1` at the top). Mapping NDC to a top-left-origin pixel with
`(ndc * 0.5 + 0.5) * size` therefore needs no flip of its own, and adding one
would silently drag a gizmo the wrong way while passing every round-trip test.
There is exactly one flip in the whole chain and it is in the projection matrix.

## Validity is data, not an exception

A null camera or a zero-sized viewport is normal during startup and teardown, so
every query returns a result carrying its own validity flag and nothing ever
throws or dereferences null. Callers check the flag. `IsValid()` is true when a
camera is set and the logical size is non-zero.

## Overlays

The viewport owns two overlay systems, each documented on its own page:

- [Debug overlays](debug-overlays.md): the retained `DebugLine`/`DebugPoints`/`DebugMesh`
  scene objects, flattened by `DebugDrawBuilder` into `DebugDrawList`.
- [Transform gizmo](gizmo.md): the transient modal G/R/S handle driven by
  `TransformGizmo` and flattened by `GizmoDrawBuilder`.

Both are rebuilt on a dirty flag, not per frame, by `RebuildOverlays()`. It
returns `true` when at least one payload was actually rebuilt - the only way the
Editor can tell that this tick's overlays differ from the ones the renderer last
saw, and therefore whether it has to post a `RedrawOverlaysRequest` for a
converged renderer to draw them.
