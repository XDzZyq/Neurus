# Render Passes

Neurus renders deferred PBR through a fixed chain of compute and raster passes, from shadow depth through lighting and IBL to the scene image, then a separate overlay graph over that image.

## Pipeline order

The pipeline is orchestrated by two RenderGraphs. `DeferredRenderer::m_mainGraph` is the scene graph - a compile-once DAG from shadow depth to the composed scene image. `OverlayEngine` owns the overlay graph, which draws the selection outline, debug primitives and gizmo over whatever image the scene graph ends with. Each pass declares its image reads and writes via `Pass::GetIO()`, a graph wires producer to consumer edges by `AttachmentName`, topologically sorts once, and `recordFrame()` executes the graph(s) the frame's work needs.

The scene graph is a projection of `RenderConfig`: `RebuildMainGraph()` is re-run only when the config-derived `PipelineSignature` changes (e.g. FXAA toggled), so the DAG always contains exactly the passes that will run. The overlay graph has a fixed shape: only its copy source changes with the tail of the scene graph (`ComposedOutput`, or `FXAAOutput` when FXAA is on), and `OverlayEngine::SetSceneTail()` re-wires it then. Passes still own their barriers and descriptor writes internally; any valid topological order is correct.

The two graphs exchange data only through `RenderCache` attachments, never in place: the overlay copy reads the scene tail, so the converged scene image survives every overlay redraw.

Shadow resources are logical `AttachmentName` members resolved by their owning pass (not `GetAttachment`): `ShadowDepth` (per-light bundle in `LightGPU`) and `ShadowIntensity` (2D array via `GetShadowIntensityArray`).

```
Scene graph (DeferredRenderer::m_mainGraph)
ShadowDepthPass (per-light depth -> RenderCache via GetShadowMap(uid, lightType))
    |-- Point light: cubemap geometry pass (6 faces, multiview)
    `-- Sun light:   2D orthographic geometry pass (2048x2048, single view)
    v
GeometryPass (G-Buffer MRT: Position, Normal, Albedo, MetallicRoughness, IDBuffer, Depth)
    v
SSAOPass (compute: reads G-Buffer, writes AO to R8 attachment)
    v
ShadowIntensityPass (compute: per-light shadow eval -> layered R8_UNORM 2D_ARRAY)
    |-- Point light: samplerCube depth comparison, PCF via cubemap sampling
    `-- Sun light:   sampler2D depth comparison, ortho PCF, NDC Z in [0,1]
    v
LightingPass (compute: reads G-Buffer + AO + shadow intensity array,
              reads LightingCache SSBOs from RenderCache, writes HDRColor)
    |-- Binding 5: PointLight SSBO (from RenderCache::GetLightingCache())
    `-- Binding 6: SunLight SSBO   (from RenderCache::GetLightingCache())
    v
IBLPass (compute: reads G-Buffer + HDRColor, applies diffuse+specular IBL, writes HDRColor)
    v
ComposePass (compute: tone maps HDRColor, applies gamma, writes ComposedOutput)
    v
FXAAPass (compute: reads ComposedOutput, luma-based edge detection + full-iteration edge search, writes FXAAOutput)
    |  (conditional: only when AA == AAAlg::FXAA)
    v
scene tail: ComposedOutput (no FXAA) | FXAAOutput (FXAA)

Overlay graph (OverlayEngine, its own RenderGraph)
SelectionOutlinePass (compute: reads IDBuffer, 3x3 edge detection for activeObjectId, writes R8 mask)
    v
OverlayCopyPass (compute: copies the scene tail into OverlayOutput and blends the
                mask onto it, one dispatch - overlay_copy.comp)
    v
DebugPass (raster: draws DebugDrawList over OverlayOutput with LOAD_OP_LOAD,
           depth-tested against the G-Buffer Depth it only reads)
    |-- lines      -> screen-space quads (6 verts/segment, SSBO-indexed)
    |-- points     -> ePointList sprites (square / rhombus / circle mask)
    `-- wireframes -> PolygonMode::eLine over the DebugMesh's existing MeshGPU
    v
GizmoPass (raster: draws GizmoDrawList over OverlayOutput with LOAD_OP_LOAD -
           no depth attachment at all, so every guide is unconditionally x-ray)
    |-- lines      -> screen-space quads (the shared overlay_line pipeline)
    `-- points     -> ePointList sprites (the shared overlay_point pipeline)
    v
Blit (OverlayEngine::OutputAttachment() == OverlayOutput) -> Swapchain (vkCmdBlitImage)
```

A **Sample** frame executes both graphs; an **Overlay** frame executes the overlay graph alone over the image the last sample left; an **Idle** frame does neither and acquires no image. See [Temporal Accumulation](temporal.md) for the budget that decides which.

The pass base types live in `passes/Pass.h` and `passes/ComputePass.h`; the overlay graph and its copy pass live in `overlay/OverlayEngine.h`. Each pass reports its own draw/dispatch counts by returning `PassStats` from `Record()`.

### Overlay ordering

The overlays are not part of the scene graph, and that is the point: they draw after the scene's tail, so an overlay-only change never re-runs a scene pass, and no overlay is touched by FXAA. Every overlay primitive that can be antialiased already is, analytically and from real coverage (`overlay_line.frag` fades one pixel in from the quad edge using the true width, `overlay_point.frag` fades an exact p-norm distance through `fwidth()`). A luma filter can only re-blur a gradient that was already correct, drag overlay color into neighbouring scene pixels, and soften the hard dash ends that the debug builder deliberately leaves un-`Smooth`ed on a stippled line. The cost is wireframe, whose `PolygonMode::eLine` edges have no distance-to-edge to fade and so were the only primitives FXAA genuinely helped - accepted, since a `DebugMesh` overlay is a diagnostic, not a shipped image. The selection outline is in the same position now: it used to be blended by `ComposePass` and therefore anti-aliased by FXAA, and it is now blended by the copy *after* tone mapping and gamma, like the other overlays.

`OverlayEngine` fixes the `DebugPass` and `GizmoPass` targets at construction (`OverlayOutput`); only the copy's source follows the config, through `OverlayEngine::SetSceneTail()` from the tail `RebuildMainGraph()` chose. The two scene tails stay concrete attachment names rather than an alias shared by both, because `RenderGraph::Connect()` matches producer and consumer sockets by `AttachmentName` and an unwired input is treated as external - which would silently leave the copy unordered. `GetIO()` therefore always declares concrete names the graph can validate. The blit source is `OverlayEngine::OutputAttachment()`, not a pass accessor: the overlay image is the end of the frame whichever overlay pass drew last.

## Shadow depth pass

The shadow depth pass renders per-light depth into `RenderCache` via `GetShadowMap(uid, lightType)`:

- **Point light**: a cubemap geometry pass (6 faces, multiview).
- **Sun light**: a 2D orthographic geometry pass (2048x2048, single view).

### Sun light shadow projection

- **Projection**: orthographic (`glm::ortho()`) with configurable left/right/bottom/top planes and near/far planes.
- **Depth range**: NDC Z in `[0, 1]` (Vulkan convention, requires `GLM_FORCE_DEPTH_ZERO_TO_ONE`).
- **Frustum parameters** (`Light.h` static constexpr):
  - `sun_shadow_field = 2.5` - XY half-extent (5x5 unit area centered on camera target)
  - `sun_shadow_near = -10` - near plane (view-space, behind eye)
  - `sun_shadow_far = 30` - far plane (view-space, extended past geometry so depth is not crammed at NDC Z=1.0)
  - `sun_depth_range = 10` - eye distance from ortho center (decoupled from far plane so geometry sits at mid-depth)
- **Eye placement**: `eye = center - sunDir * sun_depth_range` (NOT `* farPlane`, which would place geometry at the far boundary).
- **Push constant**: `SunShadowPushConstants` carries `mat4 lightViewProj` (view x projection).
- **Shadow map resolution**: 2048x2048, `VK_FORMAT_D32_SFLOAT`.
- **PCF**: percentage-closer filtering via `sampler2DShadow` with a UV offset kernel, ortho depth comparison.
- **Shadow intensity**: written to the per-light layer in the `ShadowIntensity` 2D array via the `SunShadowIntensityEval` compute shader.
- **Shadow bias**: depth bias read from `RenderConfig::r_shadow_bias` (default 0.02) via `ctx.Frame().Settings()`. The pass states no fallback of its own - it used to hardcode `0.0005f` when no config was published, 40x off the declared default.
- **Shadow mode**: supports `HARD`, `SOFT_PCF_16`, `SOFT_PCF_64` modes matching the point-light shadow pipeline.

## SSAO pass

- **AO value**: 1.0 = fully occluded (black), 0.0 = no occlusion (lit).
- **Sampling**: hemisphere samples in view-space, random rotation via a 16x16 noise texture.
- **Output**: `VK_FORMAT_R8_UNORM` attachment (`AttachmentName::SSAO`).
- **Lighting**: the ambient term is multiplied by `(1.0 - ao)` so occluded areas receive less ambient light.
- **Radius**: default 0.15 (appropriate for the [-1, 1] scene scale).

## Selection outline pass

- **Where it runs**: first in the overlay graph, over the converged scene. It reads the `IDBuffer` the last sample wrote, so it needs no scene pass to redraw an outline.
- **IDBuffer**: reads the `VK_FORMAT_R32_UINT` attachment at binding 0, written by `GeometryPass` (stores `objectId` as a 32-bit unsigned integer per pixel).
- **Edge detection**: 3x3 Laplacian-style kernel over neighbouring IDBuffer pixels. A pixel is highlighted when at least one neighbour has a different `objectId` AND the center pixel's `objectId` matches `activeObjectId` (push constant).
- **Output**: `VK_FORMAT_R8_UNORM` attachment (`AttachmentName::SelectionOutline`) at binding 1. Highlighted pixels = 255 (edge of selected object); all others = 0. `OverlayCopyPass` consumes it.
- **Push constant**: `uint32_t activeObjectId` - read inside `Record()` from `ctx.Frame().activeObjectId`, the value the producer resolved from the scene's `selections.GetActiveObject()`. This was the renderer's last live dereference of a scene `ObjectID` pointer; the snapshot carries the id as a value instead. When `activeObjectId == 0` (nothing selected), the shader early-outs (no highlight is written).

## Overlay copy pass

`OverlayCopyPass` (`overlay/OverlayEngine.h`, shader `compute/overlay_copy.comp`) seeds `OverlayOutput` from the scene tail and blends the selection highlight onto it. It is the pass that makes an overlay-only frame possible: the converged scene image is never written in place, so a redraw always starts from a clean copy rather than from whatever the previous overlay frame left on screen.

- **Bindings**: scene tail at binding 0 (combined image sampler), the R8 outline mask at binding 1, `OverlayOutput` at binding 2 (storage image, `R16G16B16A16_SFLOAT`).
- **One dispatch, not two**: copy and blend are the same pixel-wise operation, and splitting them would mean sampling `OverlayOutput` while storing to it - a feedback loop Vulkan leaves undefined. The shader is a no-op `mix` for mask value 0, so unselected pixels copy exactly.
- **Source**: `SetSource()` picks `ComposedOutput`, or `FXAAOutput` when FXAA is on. `OverlayEngine::SetSceneTail()` sets it from the tail `RebuildMainGraph()` chose and re-registers the pass, because the source name is part of `GetIO()`.
- **Ordering**: transitions the tail and the mask to `ColorShaderRead` and `OverlayOutput` to `ShaderWrite`, then dispatches `ceil(w/16) x ceil(h/16)` groups. It deliberately leaves `OverlayOutput` in `ShaderWrite`: `DebugPass` transitions it itself, which is what gives its `LOAD_OP_LOAD` read a write dependency on this dispatch.
- **Highlight**: orange `rgb(1.0, 0.55, 0.0)` at 50% alpha, blended after tone mapping and gamma - so the outline is not anti-aliased by FXAA and can be redrawn without a scene pass. This is the blend `ComposePass` used to perform.

## Compose pass

- **Inputs**: reads `HDRColor` (binding 0, `R16G16B16A16_SFLOAT`). The selection outline is not an input; it belongs to the overlay graph.
- **Tone map / gamma**: the only tone mapping today is gamma correction -
  `pow(color, 1.0 / gamma)` where `gamma` is read from `RenderConfig::r_gamma`
  (default 1.0) via the push constant `float gamma`. A filmic curve, if added,
  belongs here.
- **Output**: `ComposedOutput` (`R16G16B16A16_SFLOAT`) at binding 1, consumed by `FXAAPass` when AA is on, and by `OverlayCopyPass` as the scene tail when it is off.
- **Not the end of the frame**: Compose is the last scene pass. Everything layered on top of the shaded image - outline included - is the overlay graph's job.

## Debug overlay pass

`DebugPass` is a raster pass that draws the scene's `DebugLine` / `DebugPoints` / `DebugMesh` objects (issue #22). It is not to be confused with the selection outline pass above, which is a compute pass that outlines the *selected* object from the IDBuffer.

- **Gate**: `RenderConfig::r_debug_draw`. The pass stays in the overlay graph unconditionally; the producer leaves the frame's `debugDraw` empty with `HasDebugDraw()` false when the toggle is off and `Record()` returns before touching any image. Toggling the overlay therefore costs **no graph rebuild** (unlike FXAA, which changes the `PipelineSignature`).
- **Target**: `p_target`, set by `SetTarget()` and read back by `GetTarget()` - `OverlayOutput` under `OverlayEngine` (what production does), `ComposedOutput` by default so a pass constructed directly still has a valid target. Drawn with `LOAD_OP_LOAD` so the copied scene image survives underneath. The pass transitions it from the `ShaderWrite` state `OverlayCopyPass` leaves it in - that barrier is the only thing ordering the overlay against that dispatch - and leaves it in `ColorAttachment` for the next overlay pass and the blit. Getting this wrong tears the overlay per-tile on MoltenVK with zero validation errors. `SetTarget()` must be called **before** `RenderGraph::AddPass()`, which caches `GetIO()`; `OverlayEngine` is the only caller that matters. Any color attachment sharing `ComposedOutput`'s format and usage works, so one set of pipelines serves either and no second pipeline variant exists.
- **Depth**: reads the G-Buffer `Depth` with `LOAD_OP_LOAD`, `depthTestEnable` on and **`depthWriteEnable` off**. Debug geometry is occluded by solid objects but never occludes anything, including other debug geometry.
- **Three pipelines, one layout** (`p_pipelines[0..2]`): lines, points, wireframe.
  - Lines are `eTriangleList`: 6 vertices per segment expanded into a screen-space quad by `overlay_line.vert`. Wide-line rasterization is deliberately *not* used - Metal caps `lineWidth` at 1.0, so `VK_EXT_line_rasterization` would silently degrade on macOS.
  - Points are `ePointList` with `gl_PointSize`; the shape is masked from `gl_PointCoord`. `gl_PointSize` is clamped to the device's `pointSizeRange[1]`, queried once and pushed as `DebugPushConstants::maxPointSizePx` (exceeding it is undefined behaviour).
  - Wireframe is `PolygonMode::eLine` over the DebugMesh's **existing** MeshGPU vertex/index buffers, so wireframe geometry is never duplicated or re-uploaded. That MeshGPU has to exist first: the pass skips any `DebugWireMesh` whose `meshObjectId` resolves to no cached buffers, so the Editor must upload it. A wireframe that simply does not appear is almost always a missing upload, not a pipeline bug.
- **X-ray is dynamic state, not a fourth pipeline**: `vk::DynamicState::eDepthTestEnable` is flipped between the depth-tested and x-ray halves of the partitioned `DebugDrawList`. The whole frame is at most **6 draws** (3 kinds x 2 depth modes), independent of primitive count - that is the structural half of issue #22's budget and `test_debug_pass.cpp` asserts it exactly (10,000 segments means 2 draws).
- **Blending**: alpha-over, no culling. Debug geometry is two-sided by nature.
- **Upload is revision-gated, and the pass does not do it**: debug producers are stateful, so the list is usually byte-identical to last frame's. The geometry lives in `RenderCache`'s `DebugCache`, not in the pass: `DeferredRenderer::recordFrame()` calls `RenderCache::UpdateDebugDraw(frameIndex, list)` once per frame, before any pass records, and a slot whose `uploadedRevision` already matches `DebugDrawList::revision` **skips the memcpy entirely**. `uploadedRevision` starts at `UINT64_MAX`, not 0, because a list legitimately at revision 0 would otherwise never upload.
- **Capacity grows with the geometry**: `DebugCache` sizes each slot's `CPUBuffer` pair in powers of two from 4 KiB, so nothing is clamped or dropped. Growth swaps the `VkBuffer` handle, which is why `DebugPass::Record()` re-writes bindings 0-2 from the cache **every frame** - the same thing `LightingPass` does for the light SSBOs, and legal for the same reason: the frame being recorded has already waited on its fence. A slot is only ever written for a frame that is not in flight.
- **Camera**: the pass binds `RenderCache::GetCameraGPU()`, the renderer's single camera UBO (`{viewProj, view}`, the block `gbuffer.vert` declares), written once per frame by `recordFrame()`. `GeometryPass` binds the same buffer; neither owns one. `CameraGPU` also keeps the **projection matrix on the CPU** (`GetProjection()`) without uploading it: `DebugPass` converts a world-space size to pixels with `proj[1][1]`, and that factor cannot be recovered from `viewProj` once the view rotation is folded in (column-major: `(proj*view)[1][1]` sums over `k`).

### Smoothing

Smoothing is derived, never authored - and on by default. Nothing stores a smooth bit: no scene property, no event, no op, no checkbox. The debug builder is the one place that decides it, per primitive kind: a solid line and every cube edge get `DebugFlag::Smooth`, a **stippled** line does not (antialiasing would soften the hard ends a dash is made of), point sprites always do (their shape mask is an analytic distance, so a rhombus or circle is visibly stepped without it), and a `DebugMesh` wireframe never does - `PolygonMode::eLine` gives the fragment stage no distance to fade, so `debug_wire.frag` has no branch on the flag.

Hiding a debug object means not flattening it. `DebugDrawList` has no per-primitive enable bit, so the builder drops objects whose `is_viewport && is_rendered` is false, before the x-ray partition is computed.

## Gizmo pass

`GizmoPass` draws the modal transform handle's guides, which are not Scene objects at all - the Editor synthesizes them from `TransformGizmo` state into a `GizmoDrawList` every frame. It is not to be confused with `DebugPass` above, which draws the scene's `DebugLine` / `DebugPoints` / `DebugMesh` *objects*. The name used to belong to the selection-outline compute pass, renamed to `SelectionOutlinePass` so that "gizmo" means exactly one thing in this renderer.

- **Position: last in the frame**, after `DebugPass`, into the same overlay image with `LOAD_OP_LOAD` (`OverlayOutput` under `OverlayEngine` - the same `SetTarget()` contract, defaulting to `ComposedOutput`). `OverlayEngine::RebuildGraph()` therefore adds an **explicit** `Connect(debugNode, OverlayOutput, gizmoNode)` edge: this pass declares no depth socket, so unlike `DebugPass` it shares no other resource with the chain and its topological position would otherwise be unconstrained. Two consecutive raster passes writing one attachment need no new barrier code - `Barrier::Transition` does not early-out when `before == after`, so transitioning a target `DebugPass` already left in `ColorAttachment` still emits the write-after-write dependency.
- **The blit source is `OverlayEngine::OutputAttachment()`**, not `GizmoPass::GetTarget()`: `OverlayOutput` is the end of the frame whether or not either overlay pass drew anything. `Record()` either leaves the target in `ColorAttachment` or does not touch it, and the blit transitions it either way.
- **Three deliberate simplifications against `DebugPass`:**
  1. **No depth attachment at all.** A constraint line the object hides is a line that cannot be read, so every guide is unconditionally x-ray. That removes `SetDepthFormat`, `SetDepthStencil` and the `eDepthTestEnable` dynamic state, and `beginRendering` passes a null depth attachment. `GetIO()` names only the target, read and written in place - the one place this pass visibly diverges.
  2. **No x-ray partition.** X-ray is the only thing `DebugDrawList`'s partition exists for, so `GizmoDrawList` stores no `xraySegmentStart` and the pass issues exactly **two** draws (segments, then points) regardless of primitive count. Draw order alone decides what lands on top, with depth off and alpha-over blending.
  3. **No wire pipeline.** A guide is lines plus one point sprite; there is no `MeshGPU` in the picture, hence no vertex layout, no `drawIndexed` and no 72-byte push block.
- **Shared shaders, separate payload**: the two pipelines are built over the same `overlay_line` / `overlay_point` pair `DebugPass` uses, because the hard parts - screen-space quad expansion for a constant *pixel* line width (MoltenVK caps `lineWidth` at 1.0) and analytic edge antialiasing - are already solved and tested there. The descriptor layout and the 16-byte push block are copied unchanged; `ShaderLibrary`'s first argument is a log tag rather than a cache key, so `"GizmoLine"` and `"DebugLine"` coexist over one file.
- **The pass uploads nothing.** Geometry lives in `RenderCache`'s `GizmoCache` and the camera in its `CameraGPU`, both written once per frame by `DeferredRenderer::recordFrame()` before any pass records. `Record()` re-points this frame's descriptor set at the cache's current handles every frame, since growth swaps the `VkBuffer`.
- **Always in the graph, no `RenderConfig` gate.** It returns early on a null or empty payload, so entering and leaving a modal never recompiles the DAG - the precedent `DebugPass` already sets.
- **All three simplifications are pinned by tests** (`test/render/test_gizmo_pass.cpp`), and the sharpest one is a deliberate mirror: `AxisLine_DrawsThroughNearDepth` submits the same segment, camera and near-primed depth buffer as `DebugPassTest.DepthTestedLine_OccludedByNearDepth` and expects the opposite result, with no `XRay` flag to explain it away. Its reference PNG is byte-identical to `DebugPass`'s, which is the shared-shader claim made falsifiable rather than a copy-paste accident.

## FXAA pass

- **Algorithm**: NVIDIA FXAA 3.11, ported from the cuda-vision full-iteration approach.
- **Pipeline**: two compute dispatches per frame: edge detection (Sobel gradient, subpixel offset) then edge-line iteration (march along the edge, check 3 rows per step: center/upper/lower) then endpoint interpolation then bilinear resample.
- **Direction**: the Sobel gradient magnitude determines horizontal (`abs(gy) >= abs(gx)`) vs vertical edge.
- **Offset**: subpixel offset in `[-0.5, 0.5]` computed as `(neighborLo-center)/(neighborLo-neighborHi) - 0.5`.
- **Edge march**: iterates along the edge line (left/right for horizontal, up/down for vertical), up to 32 steps per direction. FLIP: center pixel luma differs from the starting luma (crossed edge). END: off-axis neighbour has the same luma as the starting luma (left edge region). BOUND: out of image bounds.
- **Endpoint interpolation**: endpoint offsets assigned by stop code (END -> 0, FLIP -> +/-0.5, otherwise -> original offset). Final offset = `mix(leftOff, rightOff, leftSteps/(leftSteps+rightSteps))`.
- **Resample**: bilinear `texture()` at the subpixel coordinate offset by `finalOffset * rcpFrame` along the edge normal.
- **Sampler**: bilinear (`VK_FILTER_LINEAR`) for sub-pixel accuracy; falls back to nearest if the `R16G16B16A16_SFLOAT` format does not support `VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT`.
- **Config**: `RenderConfig::r_fxaa_subpix` (strength, default 0.75), `r_fxaa_edge_threshold` (default 0.166), `r_fxaa_edge_threshold_min` (default 0.0833).
- **Gating**: `RenderConfig::RequiresFXAA()` - only records when the AA algorithm is set to FXAA in the RenderConfigPanel.
- **Input is the scene alone**: FXAA sits at the end of the scene graph and hands its output to the overlay copy, so it never sees the selection outline, the debug overlay or the gizmo. That is deliberate - see "Overlay ordering" above. It also means FXAA presence changes the copy's source, which is why `PipelineSignature::fxaa` drives the scene graph's tail and `OverlayEngine::SetSceneTail()` follows it.

## Configuration

`RenderConfig` (`src/core/RenderConfig.h`) is user-facing config owned by the Editor, reached by passes through `ctx.Frame().Settings()`:

- **Algorithm selection**: `r_pipeline` (Forward/Deferred), `r_aa`, `r_ao`, `r_shadow`, `r_ssr`.
- **Quality parameters**: `r_gamma`, `r_ao_ksize`, `r_ao_radius`, `r_shadow_bias` (0.02), `r_sample_pf`, `r_sampling_count` (64 - scene samples accumulated after a restart before the renderer stops; clamped to at least 1).
- **Overlay toggle**: `r_debug_draw` / `RequiresDebugDraw()` - gates *publication*, not graph topology (see the Debug overlay pass above). It is written **last** in `serialize()`, and like every other `RenderConfig` field it is read through `archive::Optional`, because a project file saved before the field existed would otherwise make `ConfigComponent::Load` discard the whole config and reset the user's gamma, AO and shadow settings. Absence leaves the member at its in-class `= true`; nothing in the load path restates that value. **Any field added to `RenderConfig` from now on goes in the same `Optional` call, with its default only in the member initializer.**
- **Serialized** via cereal for project save/load.
- **Live-update**: passes read `ctx.Frame().Settings()` each frame - no cast, and never null: with no published config it resolves to `DefaultRenderConfig()` (`src/core/RenderConfig.h`), so the in-class member initializers are the only place a default is written. **A pass must not spell a fallback literal of its own** (`cfg ? cfg->x : 0.0005f` is how the shadow bias came to differ 40x from the declaration). Scalar param changes take effect on the next `DrawFrame()`.
- **Shadow bias flow**: `RenderConfigPanel` slider -> `configValueChanged` -> `Editor::SetRenderConfig` -> copied into the frame by `BuildSnapshot()` -> `ShadowIntensityPass` reads `ctx.Frame().Settings().r_shadow_bias`.
