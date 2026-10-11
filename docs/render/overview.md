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

Cross-layer communication goes through UIEvents (Qt signals/slots) for UI to Editor, the EventQueue for Editor to renderer events, and Context objects for data queries. The renderer receives read-only scene data through Context objects and never calls back into the Editor.

### Vulkan ownership

- The UI layer owns `VkSurfaceKHR` (created from a `QWindow` and `QVulkanInstance`).
- The renderer owns everything else: the instance (via `QVulkanInstance` sharing), device, swapchain, pipeline, command pool and all GPU resources.
- No GPU resource is shared across layers.
- Non-copyable GPU resource classes delete copy and assignment.

### Threading

Main thread only for the current version. In future the renderer may run on a dedicated thread with proper synchronization.

## Where the code lives

- `src/render/` - the renderer layer sources.
- `src/render/VulkanContext` - instance, physical device, logical device, queues.
- `src/render/Swapchain.h` - swapchain creation, image acquisition, presentation, recreation.
- `src/render/Image.h/cpp` - GPU image with `ImageState` tracking (including `Invalid`) and mipmap generation.
- `src/render/Barrier.h/cpp` - centralized image barrier management (`ImageState` to Vulkan layout/stage/access).
- `src/core/RenderConfig.h` - user-settable render config: algorithms, quality params, shadow bias.
- `src/render/RenderContext.h` - per-frame immutable scene snapshot; embeds the `SceneContext snapshot` (all typed pointers).
- `src/render/ProfilingData.h` - Vulkan-free profiling POD (`PassProfile`, `FrameProfile`).
- `src/render/GPUProfiler.h/cpp` - GPU timestamp query pool (per-frame, per-pass).
- `src/render/shaders/` - see [Shaders](shaders.md).
- `src/render/DeferredRenderer.h/cpp` - the public renderer entry point and frame drawing.
- `src/render/RenderCache.h/cpp` - cross-frame resource pool; owns `MeshGPU`, `EnvironmentGPU`, `LightingCache`, `CameraGPU`, `DebugCache`, `GizmoCache`, attachments and shadow maps.
- `src/render/UploadManager.h/cpp` - CPU-to-GPU upload service (meshes, lights, environments, IBL).
- `src/render/Texture.h/cpp` - texture resource (`Image` + sampler + descriptor).
- `src/render/resources/` - GPU resource structs owned by `RenderCache`; see [Resources](resources.md).
- `src/render/buffers/` - buffer class hierarchy; see [Buffers](buffers.md).
- `src/render/render_graph/` - pass DAG orchestration; see [Render Graph](render-graph.md).
- `src/render/passes/` - render passes; see [Passes](passes.md).

> Note: the renderer's `VulkanContext` lives under `src/app/`, not `src/render/`.

`UploadMesh(const Mesh&)` null-checks `o_mesh` and then delegates to `UploadMeshData(const MeshData&)`, which owns the 14-to-8 float vertex stripping. Scene objects that carry a `MeshData` without being a `Mesh` - `DebugMesh` - call `UploadMeshData` directly rather than duplicating the stripping.

## Device and swapchain

### Device management (`VulkanContext`)

- Create `VkDevice` from Qt's `QVulkanInstance`
- Select the physical device (prefer a discrete GPU, fall back to the first available)
- Choose the graphics queue family
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

`DeferredRenderer` owns the command pool and command buffers, an `imageAvailable` + `renderFinished` semaphore pair, and an in-flight fence for CPU-GPU synchronization. The frame is drawn as: acquire, begin dynamic rendering, bind pipeline, draw, end dynamic rendering, present. `WaitIdle()` calls `DeviceWaitIdle` for clean shutdown.

For the current pass-by-pass structure, see [Passes](passes.md). For shader and pipeline management, see [Shaders](shaders.md).

## Renderer preconditions

- `DeferredRenderer` assumes `ctx.snapshot.camera` is non-null. The Editor's viewport camera (`EnsureCamera` / `SyncCamera`) keeps it populated; the renderer only enforces the precondition, it does not synthesise a camera.

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
- Continue rendering with the new swapchain

### Surface lost (`VK_ERROR_SURFACE_LOST_KHR`)

- Window destroyed or display disconnected
- Clean up, stop rendering, signal the application to exit

## Swapchain and frame pacing

- **Usage flags**: `Swapchain` requests `eColorAttachment | eTransferDst | eTransferSrc`, masked by `capabilities.supportedUsageFlags`. `eTransferDst` is the final blit's target; `eTransferSrc` exists solely so `Screenshot` can copy the presented image back - without it the capture path raises copy-source validation errors. The constructor and `Recreate()` must request the same set.
- **`kMaxFramesInFlight` is 1.** `RenderCache::GetAttachment()` returns a *single* shared `Image` per `AttachmentName` (the extent argument is honoured only on the first, lazy creation), so a second in-flight frame would write `ComposedOutput`/`Depth` while the previous frame's blit still reads them, across two submits with no dependency between them. Raising it requires per-frame-slot attachments first.
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
