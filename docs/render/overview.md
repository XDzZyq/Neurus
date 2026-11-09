# Renderer Overview

The renderer is a pure rendering service that owns every GPU resource in Neurus and turns a read-only per-frame scene snapshot into a presented image.

## Responsibility

The Renderer owns GPU resources and renders frames. It must remain stateless with respect to application logic. It consumes read-only scene data and must not mutate application-level state.

The renderer owns `MeshGPU` and `EnvironmentGPU` through `RenderCache`. It separates GPU ownership from the Vulkan-free scene and asset layers.

## Layer boundary

The renderer may:

- Read scene data via const reference
- Subscribe to UIEvents signals for configuration changes
- Own GPU resources (device, swapchain, pipeline, command buffers, RenderCache, attachments)
- Emit performance metrics or warnings via UIEvents

The renderer must not:

- Mutate application state
- Depend on the Editor or UI layers (except a borrowed `VkSurfaceKHR` reference)
- Directly call UI or Editor functions
- Include headers from `src/editor/` or `src/ui/`

Cross-layer communication goes through UIEvents (Qt signals/slots) for UI to Editor, the EventQueue for typed events inside the Editor, typed `RenderRequest` values the Editor posts through `RenderRequestSink` for Editor to renderer GPU work, and Context objects for data queries. The EventQueue is Editor-internal and is not a channel to the renderer. The renderer receives read-only scene data through Context objects and never calls back into the Editor.

### Vulkan ownership

- The UI layer owns `VkSurfaceKHR` (created from a `QWindow` and `QVulkanInstance`).
- The renderer owns everything else: the instance (via `QVulkanInstance` sharing), device, swapchain, pipeline, command pool and all GPU resources.
- No GPU resource is shared across layers.
- Non-copyable GPU resource classes delete copy and assignment.

### Threading

The renderer runs on a dedicated `RenderThread` once it is started: the main thread publishes one `PublishedFrame` per tick - that tick's snapshot plus the `RenderRequest`s posted during it - into a latest-wins mailbox without ever blocking, posts control messages, and never draws or waits the GPU. Only the render thread calls into `DeferredRenderer`. `DeferredRenderer` is still usable directly - it is what the thread drives, and what tests fake.

**One interlock for the queues.** Every queue operation goes through `SharedQueue` (`src/render/SharedQueue.h/cpp`), one instance per distinct `VkQueue` handle. Uploads go through the graphics queue family on every device: `VulkanContext::findTransferQueueFamily()` returns it, because an upload runs synchronously on the thread that submits it and the IBL convolution dispatches need compute, which a graphics family always supports - a second family buys no overlap and only costs the queue-family ownership transfers the uploaded images would need. `InitDevice()` therefore creates one queue and wraps it in one `SharedQueue` for both roles, logging "uploads use the graphics queue family N". `InitQueue()` then re-picks the graphics family that supports the surface's present, which must be the family the device created its queue from - the handle matches, so both roles fall on one lock ("graphics and transfer share one VkQueue (one lock)"). The lock covers the call, not the GPU work: `Submit` and `Present` hold it only for the duration of the call, and `SubmitAndWait` releases it before waiting on its own fence, so one submitter's wait never locks out another. Only `WaitIdle()` holds the lock across a wait, and it is reserved for the rare events that want the queue quiet - swapchain rebuild (including the drain `Swapchain::Recreate` does before destroying the old swapchain, whose present completion MoltenVK still dereferences), surface change, shutdown, and the one fallback: `DeferredRenderer::DrainInFlightFrames()` idles the graphics queue when its frame-fence wait fails or throws, because the caller is about to destroy what a recorded frame may still read.

The waits are per submitter. Uploads (`StagingBuffer`, `Texture`, the IBL bake) and the screenshot and pick readbacks call `SubmitAndWait`, so they wait their own work and nothing else's. The request handlers that free a resource a recorded frame may still read instead drain the in-flight frames: `RenderRequestExecutor` is given `DeferredRenderer::DrainInFlightFrames()`, which waits every frame fence with `kFenceTimeoutNs` and falls back to an idle graphics queue if the wait fails. That is what a pipeline replacement, light SSBO growth (a kind outgrows its allocation, so `ArrayBuffer::Upload` resizes the old buffer away; a first allocation destroys nothing) and the scene drop rely on, and it is why they do not depend on the frame count. Replacing or removing a mesh or environment entry needs no drain (a mesh upload over a resident id is a no-op, so a mesh entry only goes away with the scene): the mesh and environment tables hold `shared_ptr<const MeshGPU>` / `shared_ptr<const EnvironmentGPU>`, and a submitted frame keeps the table it drew with until its fence is seen signalled. The in-place camera, light and SSAO UBO/SSBO rewrites do not drain either: they rely on the frame-slot fence wait, which only proves the slot the next frame would reuse is done, so it is safe only with one frame in flight.

Once progressive accumulation converges the thread stops drawing on its own: it asks `IFrameRenderer::NextWork()` how much work the next frame is worth before waiting on the mailbox, so a remaining sample reuses the snapshot it already holds, an overlay-only change waits for a fresh frame, and nothing left to do parks without acquiring a swapchain image. See [Temporal Accumulation](temporal.md).

## Where the code lives

- `src/render/` - the renderer layer sources.
- `src/render/VulkanContext` - instance, physical device, logical device, queues.
- `src/render/Swapchain.h` - swapchain creation, image acquisition, presentation, recreation.
- `src/render/Image.h/cpp` - GPU image with `ImageState` tracking (including `Invalid`) and mipmap generation.
- `src/render/Barrier.h/cpp` - centralized image barrier management (`ImageState` to Vulkan layout/stage/access).
- `src/core/RenderConfig.h` - user-settable render config: algorithms, quality params, shadow bias.
- `src/render/RenderContext.h` - the renderer's per-frame view (Vulkan-free, all typed pointers), with two writers: the Editor publishes the semantic half (`scene`, `config`, `camera`, `debugDraw`, `gizmoDraw`) and `DeferredRenderer::recordFrame()` stamps the frame half (`width`, `height`, `frameIndex`, `jitter`, `iteration`). `Settings()` lives here and falls back to `DefaultRenderConfig()` (`src/core/RenderConfig.h`).
- `src/render/ProfilingData.h` - Vulkan-free profiling POD (`PassProfile`, `FrameProfile`).
- `src/render/GPUProfiler.h/cpp` - GPU timestamp query pool (per-frame, per-pass).
- `src/render/shaders/` - see [Shaders](shaders.md).
- `src/render/DeferredRenderer.h/cpp` - the public renderer entry point, `NextWork()` and frame drawing.
- `src/render/overlay/OverlayEngine.h/cpp` - the overlay graph: selection outline, overlay copy, debug and gizmo passes over the scene graph's tail image, plus the `OverlayCopyPass` that seeds `OverlayOutput`.
- `src/render/RenderCache.h/cpp` - cross-frame resource pool; holds `MeshGPU` and `EnvironmentGPU` in refcounted tables, owns `LightingCache`, `CameraGPU`, `DebugCache`, `GizmoCache`, attachments and shadow maps.
- `src/render/UploadManager.h/cpp` - CPU-to-GPU upload service (meshes, lights, environments, IBL). Owned by the Application and borrowed by the renderer's request executor; the Editor no longer holds it.
- `src/render/requests/RenderRequests.h` - typed GPU-work requests the Editor posts as owned values (`UploadMeshRequest`, `CreateLightGpuRequest`, `UploadEnvironmentRequest`, `UpdateLightingRequest`, `UpdateLightRequest`, `EnsurePipelineRequest`, `DropSceneResourcesRequest`, `ResetAccumulationRequest`, `RedrawOverlaysRequest`).
- `src/render/requests/RenderRequestProcessor.h/cpp` - owns the renderer's inbox (`BatchQueue<RenderRequest>`, `src/core/BatchQueue.h`) and the executor. `Post` appends; `Drain` takes the whole batch once per frame, applies `ResetAccumulationRequest` / `RedrawOverlaysRequest` to renderer state, and passes every GPU request to the executor. `DeferredRenderer::DrawFrame()` has exactly one `Drain()` call site. The main thread's half of the handoff is the posting queue inside `RenderThread`, which `Publish` moves into the frame it publishes.
- `src/render/requests/RenderRequestSink.h` - the Post-only interface the Editor holds instead of the renderer; implemented by `DeferredRenderer` (enqueue, drained by the next `DrawFrame`) and `RenderThread` (append to the posting queue that `Publish` moves into the frame; no wake, no invalidate).
- `src/render/IFrameRenderer.h` - the operations `RenderThread` sequences (fence wait, `Post`, `NextWork`, draw, resize, surface change, ID-buffer read, screenshot, idle); `DeferredRenderer` implements it, tests use a fake.
- `src/render/SharedQueue.h/cpp` - mutex-guarded access to one `VkQueue` handle: `Submit`, `Present`, `SubmitAndWait` (lock released before the fence wait) and `WaitIdle` (lock held across the wait). One instance per distinct handle; uploads are the graphics family, so one wrapper guards both roles.
- `src/render/FrameMailbox.h/cpp` - one-slot latest-wins channel from publish to render thread. `Acquire(out, mode)` takes `Wait`, `Reuse` or `Idle` and reports `Frame`, `Repeat`, `Interrupted` or `Stopped`, so a converged renderer can redraw the snapshot it holds or park without acquiring an image; `Invalidate()` drops a pending frame and forces the next acquire to wait for a fresh publish. Snapshots are the lossy half: the requests carried by a displaced or dropped frame are prepended to the next publish.
- `src/render/RenderControl.h` - main-thread control messages (`ResizeControl`, `SurfaceChangeControl`, `PickControl`, `ScreenshotControl`); the surface change and the pick carry a `std::promise`, the screenshot carries its destination directory as a value.
- `src/render/RenderThread.h/cpp` - the dedicated render thread: owns the `FrameMailbox` and the control queue, and is the only caller into the renderer once started. Loop: wait the frame fence, drain controls (last resize of a batch wins), ask `NextWork()` for the acquisition mode, `Acquire`, hand the frame's requests to the renderer and clear them, draw. `Post` appends to the posting queue that `Publish` moves into the frame it hands over, so a request executes with its own tick's snapshot or a newer one, never an older one, and neither wakes nor invalidates anything; resize and surface change invalidate, pick and screenshot only wake. A frame displaced or invalidated before the render thread took it leaves its requests for the next publish. On exit it rejects new controls and breaks pending promises, then `WaitIdle`s.
- `src/render/requests/RenderRequestExecutor.h/cpp` - executes one GPU request against the device, `RenderCache`, `UploadManager` and `GeometryPass`; the testable seam `DeferredRenderer` delegates to. Renderer-state requests never reach it: `Execute` is a constrained template, so handing it `ResetAccumulationRequest` or `RedrawOverlaysRequest` is a compile error rather than a silent no-op.
- `src/render/Texture.h/cpp` - texture resource (`Image` + sampler + descriptor).
- `src/render/resources/` - GPU resource structs owned by `RenderCache`; see [Resources](resources.md).
- `src/render/buffers/` - buffer class hierarchy; see [Buffers](buffers.md).
- `src/render/render_graph/` - pass DAG orchestration; see [Render Graph](render-graph.md).
- `src/render/passes/` - render passes; see [Passes](passes.md).
- `src/render/overlay/` - overlay graph and its copy pass (`OverlayEngine`), plus the Vulkan-free overlay payloads; see [Passes](passes.md) and [Overlays](../scene/overlays.md).

> Note: the renderer's `VulkanContext` lives under `src/app/`, not `src/render/`.

`UploadMesh(const Mesh&)` null-checks `o_mesh` and then delegates to `UploadMeshData(const MeshData&)`, which owns the 14-to-8 float vertex stripping. The running application reaches only `UploadMeshData`: `UploadMeshRequest` carries a `MeshData`, so the executor uploads a `Mesh` and a `DebugMesh` through the same call instead of duplicating the stripping. `UploadMesh(const Mesh&)` remains for the GPU test fixture.

## Device and swapchain

### Device management (`VulkanContext`)

- Create `VkDevice` from Qt's `QVulkanInstance`
- Select the physical device (prefer a discrete GPU, fall back to the first available)
- Choose the graphics queue family
- Choose the queue family uploads use: the graphics family, always. An upload runs synchronously on the render thread and the IBL convolution dispatches need compute, which a graphics family always supports; a second family buys no overlap, and the uploaded images would need queue-family ownership transfers the recorded barriers do not express. One queue family for both roles, and the startup log names it: "uploads use the graphics queue family N".
- Enable validation layers in Debug mode
- Report device capabilities and extensions

### Swapchain management (`Swapchain`)

- Create the swapchain from a `VkSurfaceKHR` (borrowed from the UI layer)
- Select the surface format (prefer `VK_FORMAT_B8G8R8A8_SRGB`)
- Select the present mode (prefer `VK_PRESENT_MODE_FIFO_KHR` for VSync)
- Clamp the extent to surface capabilities
- Create image views for each swapchain image
- Recreate on window resize (the old swapchain is destroyed, a new one is created)

## Drawing a frame

`DeferredRenderer` owns the command pool and command buffers, an `imageAvailable` + `renderFinished` semaphore pair, and an in-flight fence for CPU-GPU synchronization. The frame is drawn as: wait the frame-slot fence, drain the Editor-posted render-request queue, decide the work (`NextWork()`), acquire, record the graphs that work needs, present. A Sample frame records the scene graph and then the overlay graph; an Overlay frame records the overlay graph alone; an Idle frame returns before acquiring an image at all. `WaitIdle()` idles the graphics queue (`SharedQueue::WaitIdle`) for clean shutdown.

`NextWork()` is the contract between the two: `Sample` while `r_sampling_count` scene frames have not been accumulated since the last restart, `Overlay` when only overlay content is stale, `Idle` when there is nothing left to draw, and `Retry` when the last `DrawFrame()` drew nothing (no camera, failed acquire or present). `RenderThread` turns that into the mailbox acquisition mode, so the right amount of waiting happens before the draw instead of being discovered after it.

The request drain sits deliberately after the fence wait and before swapchain acquire (and before the no-camera early-out, so a camera-less session still services its queue). Three handlers do not rely on that fence wait alone: a pipeline replacement, light SSBO growth in `UpdateLighting` and the scene drop each drain every in-flight frame fence (`DeferredRenderer::DrainInFlightFrames`), which is what keeps a recorded frame from reading a resource that is about to disappear - that does not depend on the frame count. Mesh and environment entries that are replaced or removed drain nothing (a mesh upload over a resident id is a no-op, so a mesh entry only goes away with the scene): the cache installs a fresh `shared_ptr<const MeshTable>` / `EnvironmentTable`, and each submitted frame holds the table it drew with (`DeferredRenderer::HeldTables`) until its fence is seen signalled. The in-place camera, light and SSAO UBO/SSBO rewrites touch memory a recorded frame may read and rely on the frame-slot fence wait with one frame in flight, which is one more reason `kMaxFramesInFlight` stays 1 (see [Swapchain and frame pacing](#swapchain-and-frame-pacing)). See [Render request path](resources.md#render-request-path).

For the current pass-by-pass structure, see [Passes](passes.md). For shader and pipeline management, see [Shaders](shaders.md).

## Renderer preconditions

- `DeferredRenderer` requires the published frame to carry a camera - it checks `ctx.Frame().HasCamera()` and skips the frame otherwise. The Editor's active-camera publish (`EnsureCamera` + `PublishActiveCamera`) keeps the frame populated; the renderer only enforces the precondition, it does not synthesise a camera.

## Vulkan-HPP RAII conventions

All Vulkan objects use the `vk::raii` namespace:

```cpp
// DO: RAII - automatically destroyed on scope exit
vk::raii::Device device(physicalDevice, deviceCreateInfo);

// DON'T: raw handles requiring manual vkDestroy
// VkDevice device;
```

Ownership rules:

- `vk::raii` objects are non-copyable (move-only)
- Store as class members for object lifetime
- Pass by reference when sharing (non-owning)
- Use `= delete` on the copy constructor and assignment operator for classes that own GPU resources

## Validation layers

Debug mode:

- Enable `VK_LAYER_KHRONOS_validation`
- Set up a debug utils messenger for callback-based reporting
- Report all validation messages via a UIEvents signal

Release mode:

- No validation layers (performance)
- Minimal error checking (asserts only for unrecoverable states)

## Error handling

### Device loss (`VK_ERROR_DEVICE_LOST`)

- Notify the user via UIEvents
- Attempt a clean shutdown (destroy resources)
- Do not crash or enter an infinite loop

### Swapchain out-of-date (`VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR`)

- Normal lifecycle event (window resize, minimize)
- Recreate the swapchain with the new surface dimensions
- `Swapchain::Recreate()` idles the present queue (`SharedQueue::WaitIdle`) before destroying the old swapchain: it also runs directly from `Present()`/`AcquireNextImage()`, while the frame just presented is still in flight, and on MoltenVK the present-completion handler dereferences the old swapchain until that frame completes. With the render thread presenting back-to-back during a window drag, skipping the wait is a use-after-free.
- Continue rendering with the new swapchain
- **`VK_SUBOPTIMAL_KHR` from `vkAcquireNextImageKHR` is a success**, not a failure: an image was acquired and the semaphore was signalled, so it must be rendered and presented like any other frame. Recreating on acquire would drop that image and leave its semaphore signalled for the next acquire to signal again. The present path is where a suboptimal swapchain is rebuilt.
- **`AcquireNextImage` returns `std::optional<uint32_t>`**: a nullopt means no image was acquired and no semaphore was signalled, so `DrawFrame` skips the frame. This matters when `Recreate()` defers - a zero-area surface (minimize/dock transition) or a lost surface leaves the old swapchain in place and does not bump its generation, and reporting index 0 as if it had been acquired made the renderer submit a frame waiting on an unsignalled semaphore (`VUID-vkQueueSubmit-pWaitSemaphores-03238`) and present an unacquired image (`VUID-VkPresentInfoKHR-pImageIndices-01430`). Only a successful acquire may reach the submit/present path.

### Surface lost (`VK_ERROR_SURFACE_LOST_KHR`)

- Window destroyed or display disconnected
- No image is acquired: `AcquireNextImage` returns nullopt and logs once; `Present` logs and drops the frame. Neither submits against a dead surface. The surface-change path owns recovery, and shutdown joins the render thread before the members it touches are destroyed.

## Swapchain and frame pacing

- **Usage flags**: `Swapchain` requests `eColorAttachment | eTransferDst | eTransferSrc`, masked by `capabilities.supportedUsageFlags`. `eTransferDst` is the final blit's target; `eTransferSrc` exists solely so `Screenshot` can copy the presented image back - without it the capture path raises copy-source validation errors. The constructor and `Recreate()` must request the same set.
- **`kMaxFramesInFlight` is 1.** `RenderCache::GetAttachment()` returns a *single* shared `Image` per `AttachmentName` (the extent argument is honoured only on the first, lazy creation), so a second in-flight frame would write `ComposedOutput`/`OverlayOutput`/`Depth` while the previous frame's blit still reads them, across two submits with no dependency between them. Raising it requires per-frame-slot attachments first.
- **`kFenceTimeoutNs` is a deadlock guard, not a frame budget** (5 s). On timeout `DrawFrame` abandons a frame it has already acquired an image for, so a value near the real frame time silently drops frames - a Debug build with validation layers on measures about 600 ms per frame, which the old 100 ms tripped every frame.

## Legacy triangle path

The first milestone implemented a minimal but correct rendering path:

1. **No vertex buffers** - triangle vertices are hard-coded in the vertex shader using `gl_VertexIndex` to select positions/colors
2. **No descriptor sets** - no uniforms needed for a static triangle
3. **No depth buffer** - a single triangle, no depth testing needed
4. **VK_KHR_dynamic_rendering** - no explicit `VkRenderPass`/`VkFramebuffer` objects
5. **Single command buffer** - recorded once, replayed each frame
6. **Single in-flight frame** - no frame overlap (fence-synchronized)

Its data flow was:

```
UIEvents::newFrame() -> DeferredRenderer::DrawFrame()
                             |-- Swapchain::AcquireNextImage()
                             |-- Begin dynamic rendering
                             |-- Bind pipeline
                             |-- Draw (3 vertices for triangle)
                             |-- End dynamic rendering
                             `-- Swapchain::Present()
```

This path has since been superseded by the deferred pipeline in [Passes](passes.md); the triangle remains only as the historical starting point.

## Future evolution

- IBL enhancements (BRDF LUT, multi-scattering)
- Shadow mapping improvements (multi-light, shadow evaluation pass)
- SSR (screen-space reflections): ray marching variants
- Tonemapping: filmic (ACES) + gamma correction
- FXAA: luma-based edge anti-aliasing
- VMA integration for memory management
- Multiple in-flight frames (double/triple buffering) - done (`kMaxFramesInFlight`)
- Threaded command buffer recording - see issues #35 / #37
- Pipeline cache for faster startup
- Render graph abstraction - done (see [Render Graph](render-graph.md)); graph-owned resources, barriers and descriptors are tracked in issue #32
