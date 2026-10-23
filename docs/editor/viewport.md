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
`EditorViewport` decides what that pointer is, in `SyncCamera(Scene&)`:

```
EditorViewport::SyncCamera(scene):
    p_camera = scene.GetActiveCamera()     // the activated scene camera
               ?: ViewportCamera()         // otherwise the viewport camera
EditorViewport::GetCamera()  =  p_camera   // published, never recomputed
```

The Editor owns no camera and exposes no camera API. It holds the viewport
(`GetViewport()`) and contributes only the timing: `Editor::Edit()` calls
`SyncCamera(GetScene())`, and the frame snapshot's camera is built from
`GetViewport().GetCamera()`. Everything about which camera the frame belongs to,
how it is framed, and how long it lives is the viewport's business. That is also
why `GetCamera()` is a published pointer rather than a fresh choice per call: the
gizmo's projection math and the renderer's `CameraGPU` read the same one pointer,
so they cannot disagree about which camera the frame belongs to. A test that
calls `Scene::ActivateCamera`/`DeactivateCamera` by hand must therefore re-sync
where a frame would (`test_viewport_camera.cpp`'s `SyncFrame` helper).

### A scene with zero cameras is legal

A scene containing zero cameras is a legal document, and deleting every camera in
one is a legal gesture. There is no last-camera guard and nothing is injected on
load to repair a camera-less project.

### The viewport camera is pooled but not scene content

The viewport camera lives in the `ResourceManager` so
`CameraController::ResolveCamera` can resolve and orbit it by UID like any other
camera, and it is absent from `cam_list`, so it cannot be selected, deleted, or
activated. `Scene::ActivateCamera` validates membership in `cam_list` and refuses
it. Orbiting to inspect a model therefore no longer edits scene content.

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

`EnsureCamera` frames the camera it establishes, so no caller replays the extent
by hand.

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

### Edit() syncs the view camera twice per frame

`Edit()` syncs the view camera twice per frame, before and after
`ed_eventBus.Process()`. Activating or deactivating a scene camera, and deleting
the activated one, all land inside `Process()`, and the gizmo rebuild that
follows projects its geometry through this viewport at most once per dirty flag.
Rebuilding against the camera the frame started with would leave the handle at
the previous camera's scale and screen position. A third sync lives in the
`SceneObjectDeleteRequested` subscription, registered after the controllers so it
runs once the object is actually gone, which is what keeps the pointer from
dangling for the rest of that `Process()`.

## Resize and framing

The viewport extent outlives the scene and reaches both cameras.

`EditorViewport::HandleResize(logical, renderExtent)` caches both extents before
anything camera-related: the extent is a property of the viewport, not of
whichever scene happens to be loaded, and it arrives once at startup, long before
any File > New. A camera seeded later, or restored from a project saved on a
differently sized window, carries an aspect ratio unrelated to the current
viewport (a fresh `Camera` is 1x1), and only a window resize ever corrected it,
so New rendered a squashed frame until the user dragged the window. A zero extent
is the one thing dropped rather than stored: it is what the window reports before
it is first shown, and it would divide by zero in the projection, so the previous
extent is a better answer than none.

There is one framing mechanism, not two. `HandleResize` frames the viewport
camera immediately (no `CameraResizeEvent` is enqueued on the resize path any
more) and marks the gizmo dirty, because the guide is a fixed pixel budget and
the extent is an input to the picture. `SyncCamera` then frames both view
candidates behind a change-guard (it runs twice per frame, so re-framing
unconditionally would recompute matrices for nothing). Both are re-framed, not
just the one currently in use: activation can switch the view with no resize in
between, and a camera that missed a resize would present a stretched image the
moment it took over.

`SceneController::OnActiveCameraChanged` still enqueues a `CameraResizeEvent` on
activation, and `CameraController::OnCameraResize` is untouched. That path exists
for the camera becoming the view, not for the window changing size. The event
carries a UID and `CameraController` resolves it through the pool, which is why
the viewport camera is reachable by the ordinary event despite not being scene
content.

`FrameCamera()` is the one place the correction is written. It no-ops while the
extent is still 0x0 (startup, before the window is shown) rather than pushing a
degenerate aspect into the projection. Both branches are pinned by
`test_editor_scene_lifecycle.cpp`;
`test_viewport_camera.cpp::ResizeReframesBothTheViewportAndTheActivatedSceneCamera`
pins the two-camera half and the two different moments at which each is framed,
and `ZeroResizeIsIgnored` pins the dropped extent.

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
