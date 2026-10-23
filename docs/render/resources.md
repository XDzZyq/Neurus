# GPU Resources

`RenderCache` owns the renderer's cross-frame mutable GPU resources, keeping GPU ownership separate from the Vulkan-free scene and asset layers.

## RenderCache

`RenderCache` owns cross-frame mutable GPU resources beyond framebuffer attachments. These resources separate GPU ownership from the Vulkan-free scene and asset layers.

**Scene-scoped eviction.** Mesh, environment and light entries - plus the shadow-layer / SSBO-index maps and the shadow intensity array derived from them - are dropped together by `RenderCache::RemoveSceneResources()`. The renderer calls it when it executes a `DropSceneResourcesRequest`, which the Editor posts whenever it replaces the scene (from `BeginLoad()` and `NewScene()`); the executor drains the device first, since the entries own `vk::raii` resources the last frame may still read. Keying by object UID makes this necessary rather than tidy: `UID::serialize()` *restores* `o_id` from the project file, so an entry left behind by the previous scene shadows whatever returns under the same id. FIFO ordering is what makes the drop safe across a replacement - the Editor posts the drop before the new scene's upload requests, so the old cache is evicted before the new scene uploads. Screen-space attachments, the pipeline cache and the `LightingCache` survive it - they are recreated lazily, versioned, or owned by `UploadManager` rather than by the scene.

## Render request path

The renderer is the sole GPU submitter: the Editor touches no `VkQueue`, no
device-wide wait and none of the `RenderCache` resource maps. Everything it used
to do to them directly - upload a mesh, allocate a light's shadow maps,
(re)generate IBL, rewrite the light SSBOs, warm a per-mesh pipeline, drop the
outgoing scene, reset accumulation - is posted instead as a typed
`RenderRequest` value and executed by the renderer on the thread that submits.

- **Queue and drain.** The Editor holds a `RenderRequestSink*` (Post only).
  `DeferredRenderer::Post(RenderRequest&&)` pushes into a mutex-guarded FIFO
  (`RenderRequestQueue`, a `BatchQueue<RenderRequest>`). `DrawFrame()` drains the whole batch
  with `TakeAll()` *after* the frame-slot fence wait and *before* swapchain
  acquire (and before the no-camera early-out). `RenderRequestExecutor` runs each
  request against the device, cache, `UploadManager` and `GeometryPass`;
  `ResetAccumulationRequest` is the one request `DeferredRenderer` keeps for
  itself, because it writes renderer state (`m_iteration`) the executor does not
  own. Requests posted inside one `Editor::Edit()` execute in that same tick's
  `DrawFrame()`, so there is no one-frame delay.

- **Why that drain placement.** With `kMaxFramesInFlight == 1` a signaled fence
  means the GPU has finished every submitted frame, so a handler that frees or
  replaces a resource cannot pull it from under an executing frame. That argument
  holds only for one frame in flight, which is why the destroying handlers
  (pipeline replace, environment regenerate, scene drop) still call
  `device.waitIdle()`.

- **FIFO ordering is a contract.** Within a batch, append order is execution
  order. An upload posted after `DropSceneResourcesRequest` lands in the new
  scene; one posted before it is discarded with the old one. This is exactly what
  makes a scene replacement safe (see scene-scoped eviction above).

- **The payload rule.** A payload is a value, or a `shared_ptr<const T>` to data
  nobody mutates after creation. It is never a `Scene`, `Mesh` or `Shader`
  pointer - the Editor may edit or delete those before the request is drained -
  and never a callable, because a closure capturing the Editor would run Editor
  code wherever the renderer runs. `EnsurePipelineRequest` carries its own copy
  of the SPIR-V (via `MakeEnsurePipelineRequest(const Shader&)`) rather than the
  `Shader`, so a recompile landing before the drain cannot change the code under
  a version it no longer belongs to.

- **Ordering against frames on the render thread.** With `RenderThread` driving
  the renderer, two rules keep "Edit -> Upload -> Reset -> Render" intact
  without the main thread ever waiting: `RenderThread::Post` always enqueues and
  *then* wakes (a pending wake outranks a pending frame in `FrameMailbox`, so the
  loop drains once more before it draws), and every `Post` of a tick happens
  before that tick's frame is published. A request posted after the publish is
  only guaranteed to run before the next frame. The loop waits the frame fence
  itself before draining, because an interrupted iteration does not draw.
  Requests are never bundled into the frame message: the mailbox is
  latest-wins, so a dropped frame would drop its requests.

> The `UploadManager` is owned by the Application and borrowed by the executor
> via `DeferredRenderer::SetUploadManager()`. Because the executor is built in the
> renderer's constructor, before the `UploadManager` exists, a mesh/light/environment
> request posted before that call is logged once and skipped.

## Attachments

All screen-space attachments (Position, Normal, Albedo, MetallicRoughness, Depth, HDRColor, SSAO, SelectionOutline, ComposedOutput, FXAAOutput) are created lazily via `RenderCache::GetAttachment(name, extent)` on first use.

Per-light shadow maps are managed via `RenderCache::GetShadowMap(lightUID, lightType)`:

- `LightType::POINTLIGHT` - cubemap (6-layer 2D_ARRAY, D32_SFLOAT, 1024x1024 per face)
- `LightType::SUNLIGHT` - 2D orthographic (D32_SFLOAT, 2048x2048)

Spot-light shadows follow the point-light cubemap path and share the same shadow-map atlas.

The shadow intensity array (R8_UNORM, layered 2D_ARRAY) is created via `RenderCache::GetShadowIntensityArray(extent)` with per-light layer indices assigned via `RenderCache::GetShadowIntensityLayer(lightUID, extent)`.

| Attachment | Format | Clear Value | Purpose |
|---|---|---|---|
| Position | R32G32B32A32_SFLOAT | (0,0,0,0) | World-space position, w=1 for rendered pixels |
| Normal | R32G32B32A32_SFLOAT | (0,0,0,0) | View-space normal |
| Albedo | R8G8B8A8_SRGB | (0,0,0,0) | Base color |
| MetallicRoughness | R8G8B8A8_UNORM | (0,0,0,0) | R=metallic, G=roughness |
| Depth | D32_SFLOAT | 1.0 | Depth buffer |
| HDRColor | R16G16B16A16_SFLOAT | (0,0,0,0) | Lighting output |
| SSAO | R8_UNORM | 0 (no occlusion) | Screen-space ambient occlusion |
| SSR | R16G16B16A16_SFLOAT | (0,0,0,0) | Screen-space reflections (planned) |
| SelectionOutline | R8_UNORM | 0 | Selected-object edge highlight (SelectionOutlinePass output) |
| ComposedOutput | R16G16B16A16_SFLOAT | (0,0,0,0) | Tonemapped scene (ComposePass output); FXAA's input, or the overlay target (DebugPass then GizmoPass) when FXAA is off |
| FXAAOutput | R16G16B16A16_SFLOAT | (0,0,0,0) | FXAA anti-aliased scene (FXAAPass output); the overlay target (DebugPass then GizmoPass) when FXAA is on |
| FXAAOffsets | R16G16_SFLOAT | (0,0) | FXAA edge subpixel offsets (RG16F, 2-channel, sampled+storage) |
| ShadowMap | D32_SFLOAT | 1.0 | Per-light shadow depth (RenderCache-owned). Cubemap (6-layer 2D_ARRAY, 1024x1024) for point lights; 2D (2048x2048) for sun lights |
| ShadowIntensity | R8_UNORM | 0 (no shadow) | Layered 2D_ARRAY, one layer per shadow-casting light (RenderCache-owned) |

## MeshGPU

`MeshGPU` (`src/render/resources/MeshGPU.h`) holds device-local `VertexBuffer` + `IndexBuffer` for a mesh, plus vertex/index counts.

- Created lazily via `RenderCache::GetMeshGPU(objectId, meshData, device, pd, queue, qfi)`, which uploads geometry from `MeshData` to the GPU.
- Subsequent calls return the cached `MeshGPU` immediately.
- Destroyed via `RenderCache::RemoveMeshGPU(objectId)` or `RenderCache::Clean()`.
- Scene `Mesh` objects call `RenderCache::GetMeshGPU()` through `Mesh::UploadToGPU()`; the scene layer never owns GPU buffers directly.

### MeshPushConstants

`MeshPushConstants` (`src/render/resources/MeshGPU.h`) is the per-mesh push-constant block sent to the vertex shader (128 bytes total). It carries two mat4s: `model` (local-to-world transform) and `normalMatrix`.

## EnvironmentGPU

`EnvironmentGPU` (`src/render/resources/EnvironmentGPU.h`) holds diffuse irradiance and specular prefiltered cubemap `Texture` objects (each wraps `Image` + sampler + descriptor).

- Created lazily via `RenderCache::CreateEnvironmentGPU(envId, device, pd, queue, qfi, env)` from an `Environment` scene object.
- Read per-frame by `LightingPass` via `RenderCache::GetEnvironmentGPU(envId)`.
- Destroyed via `RenderCache::RemoveEnvironmentGPU(envId)`.

## LightingCache

`LightingCache` (`src/render/resources/LightingCache.h`) manages point light and sun light SSBOs (device-local `GPUBuffer`s).

- Built by `UploadManager` (it needs a queue) and handed over with `RenderCache::SetLightingCache(std::unique_ptr<LightingCache>)`, so `RenderCache` never has to store a queue/qfi of its own.
- Updated via `RenderCache::UpdateLighting(variantDict)` - accepts a map of `variant<PointLightStruct, SunLightStruct>` keyed by light UID.
- `RenderCache::GetLightingCache()` returns the `LightingCache` for per-frame SSBO binding by `LightingPass`.
- Also defines `PointLightStruct`, `SunLightStruct` (std140-compatible, 48 bytes), and `LightingPushConstants` (176 bytes) - byte-for-byte matches with the GLSL shaders.

Per-member std140/std430 offset tables for the light structs were dropped; only the total sizes are `static_assert`-guarded, so member-by-member offsets now live only on the GLSL side (worth re-documenting).

## CameraGPU

`CameraGPU` (`src/render/resources/CameraGPU.h`) is the renderer's **single** camera UBO: `CameraUBOData{viewProj, view}` (128 bytes, matching the `CameraUBO` block in `gbuffer.vert` and `debug_*.vert`).

- Constructed **eagerly** in `RenderCache`'s constructor - it needs no queue - so `GetCameraGPU()` returns a reference and is never null.
- Written once per frame by `DeferredRenderer::recordFrame()` via `RenderCache::UpdateCamera(proj, view)`, *before* any pass records. Passes only bind it; a pass that wrote it would make the result depend on pass order.
- Keeps `proj` on the CPU behind `GetProjection()` without uploading it (see the Debug overlay pass in [Passes](passes.md) for why `viewProj[1][1]` is not a substitute).
- `IsValid()` is false until the first `UpdateCamera()`. `GeometryPass` and `DebugPass` both bail out of `Record()` on a false, which is also why a **test** that drives either pass directly must publish a frame first - use `VulkanTestShared::PublishFrame(cache, ctx, out, scene)`, which writes the camera UBO as part of publishing the frame.
- `CameraUBOData` must be declared identically in `gbuffer.vert`, `overlay_line.vert`, `overlay_point.vert` and `debug_wire.vert` - they share the one UBO.
- `Update` keeps `proj` separate from `view` because a consumer needs `proj[1][1]` to convert a world-space size to pixels; that value is unrecoverable from `viewProj`.

## DebugCache

`DebugCache` (`src/render/resources/DebugCache.h`) is a per-frame-slot `CPUBuffer` pair (segments + point sprites) holding the flattened `DebugDrawList`, plus the partition boundaries and counts `DebugPass` draws from.

- Also eager in `RenderCache`'s constructor, but allocates nothing until the first `UpdateDebugDraw()`; the slot vector self-sizes on `frameIndex`, so no `framesInFlight` count is threaded through the renderer.
- Written once per frame by `recordFrame()` via `RenderCache::UpdateDebugDraw(frameIndex, list)`; revision-gated, so an unchanged list copies nothing.
- `GetFrame(frameIndex)` returns a `FrameView` of borrowed pointers + counts. Null buffers mean no `UpdateDebugDraw()` has run for that slot, and `DebugPass` returns without touching an image.

## GizmoCache

`GizmoCache` (`src/render/resources/GizmoCache.h`) is a structural sibling of `DebugCache`: the same per-frame-slot `CPUBuffer` pair (segments + point sprites) and the same `EnsureCapacity` with its 4096-byte floor, holding the flattened `GizmoDrawList` that `GizmoPass` draws from. The floor is load-bearing, not incidental - it is what guarantees both buffers are non-null once `Update()` has run, and a null `VkBuffer` is not something a descriptor may name.

- Written once per frame by `recordFrame()` via `RenderCache::UpdateGizmoDraw(frameIndex, frame.gizmoDraw)`, unconditionally: the snapshot always carries a gizmo list (empty when nothing is armed), so there is no pointer to guard on.
- **No revision gate** (unlike `DebugCache`): a live gesture rebuilds its guide on every cursor *or camera* move, and when no gesture is live nothing is drawn at all, so `GizmoDrawList` carries no revision counter and `Update()` copies unconditionally - a few dozen arc chords is less than the compare is worth reasoning about.
- **No x-ray partition**, for the reason in the Gizmo pass section of [Passes](passes.md): with depth off for the whole pass there is no `xraySegmentStart`/`xrayPointStart` to record or clamp.
- The unconditional copy is load-bearing and tested two ways: an **empty** list - the frame a gesture ends on - must still update and retire the counts to zero, or the finished guide stays resident and keeps drawing; the pass test also drives a **null** payload through its fixture (which skips the update, standing in for a slot never uploaded) to prove the ring allocates nothing until `Update()` runs.

## Image state and barriers

### ImageState and Barrier convention

All image layout transitions **MUST** go through `Barrier::Transition()`, never raw `vk::ImageMemoryBarrier` / `vk::ImageMemoryBarrier2`:

```cpp
// DO: use Barrier
Barrier::Transition(cmdBuf, myImage, ImageState::ColorShaderRead);

// DON'T: raw Vulkan barriers on Image objects
// vk::ImageMemoryBarrier2 barrier(...); cmd.pipelineBarrier2(...);
```

- `Image` tracks its logical state via `ImageState m_state`.
- `Barrier::Transition(cmd, image, after)` reads `image.State()` as the "before" layout, emits a `vkCmdPipelineBarrier2`, and updates `m_state` to `after`.
- `Barrier::Transition(cmd, image, after, subresourceRange)` does the same but with an explicit subresource range - it does **not** update `m_state` (the caller must manage state for partial transitions).
- Raw `vk::ImageMemoryBarrier2` is acceptable **only** for:
  - Raw `VkImage` handles not wrapped in `Image` (e.g. swapchain images)
  - Same-layout memory barriers (`eGeneral -> eGeneral`) within compute passes

**Access masks carry the read bits, not just the write bits.** `ColorAttachment` and `DepthAttachment` map to `...AttachmentWrite | ...AttachmentRead`, because an attachment reached with `VK_ATTACHMENT_LOAD_OP_LOAD` (DebugPass draws over its target) or consumed by the depth test without depth writes (DebugPass again) *reads* the image through the attachment stage - a write-only `dstAccessMask` leaves the previous writer's data unavailable to the load, which on a tiler shows up as stale tiles blended into the frame. Passes that only clear-and-write are unaffected by the wider mask.

**A pass transitions the images it is about to *use*, and leaves its own outputs in its own write state.** A producer must never pre-transition an output into the state it guesses the next consumer wants. `Barrier::Transition` derives the `srcAccessMask` from the state the image is *currently* in, so a producer that hands over e.g. `TransferSrc` gives the consumer's own barrier a src scope of `eTransferRead` - which names no writes, makes none available, and so creates a memory dependency covering nothing. `ComposePass` used to end with `Transition(ComposedOutput, TransferSrc)` "ready for the blit"; `DebugPass`'s `TransferSrc -> ColorAttachment` barrier then failed to order its overlay against `ComposePass`'s compute writes, and on MoltenVK the compute and render encoders overlapped: every frame kept a different random subset of the overlay's tiles with `ComposePass`'s output in the rest - a per-frame-random tear that no amount of extra serialization *after* `DebugPass` could fix. The rule now: `ComposePass`/`FXAAPass` leave their output in `ShaderWrite`, `DebugPass` and `GizmoPass` leave their (shared) target in `ColorAttachment`, and the swapchain blit in `DeferredRenderer::recordFrame` transitions its own source to `TransferSrc`. The same hazard shape applies unchanged now that `DebugPass` runs after `FXAAPass` - the upstream compute dispatch whose writes its barrier must cover is FXAA's rather than `ComposePass`'s, but it is still `ShaderWrite -> ColorAttachment` out of a compute pass, so nothing about the pattern moves. `GizmoPass` then transitions the same image `ColorAttachment -> ColorAttachment`, which is *not* a no-op: `Barrier::Transition` does not early-out when before == after, so the write-after-write dependency between the two overlay passes is still emitted. Note that validation does not catch this - the layouts are all consistent - so synchronization validation plus a coloured `LOAD_OP_CLEAR` probe is the way to find it.

**`Undefined`/`Invalid` use `eAllCommands`, never `eTopOfPipe`, as a source stage.** They only ever appear as the "before" state of a discard transition, so the access mask stays empty - but `eTopOfPipe` in a `srcStageMask` creates no execution dependency at all, which leaves a discard transition unordered against whoever last read the image. The concrete case is the swapchain image: it is transitioned `Undefined -> TransferDst` for the final blit while the presentation engine may still be reading it. For the same reason the submit waits the acquire semaphore at `eColorAttachmentOutput | eTransfer` - the first use of a swapchain image on this path is a transfer, not a color-attachment write. Both were caught by synchronization validation (`SYNC-HAZARD-WRITE-AFTER-READ` at `vkQueueSubmit`); re-run it with `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT` after touching barriers or submit scopes.

### ImageState::Invalid convention

- `ImageState::Invalid` signals an image whose GPU creation failed (e.g. missing source data, unsupported format). The image has no valid GPU resources.
- `Barrier::Transition` maps `Invalid` to `Undefined` layout (a safe no-op barrier).
- `Image::FromImageData()` returns `std::shared_ptr<Image>` - on failure, returns a shared_ptr containing an empty Image with `ImageState::Invalid`.
- Callers should check `image.State() != ImageState::Invalid` before using the image.
- A default-constructed `Image` is empty (all handles null, `Undefined` state); factory functions set `Invalid` explicitly on failure.

## Retired types

`GeometryRenderItem` (removed) previously mixed CPU (`MeshData`) and GPU (`VertexBuffer`, `IndexBuffer`) concerns in one struct. It was removed as part of CPU/GPU isolation refactoring. GPU resources now live in `MeshGPU`; CPU data stays in `MeshData`.
