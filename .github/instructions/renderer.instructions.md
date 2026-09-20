# Renderer Layer

## Overview

The Renderer is a **pure rendering service** that owns GPU resources and
renders frames. It must remain stateless with respect to application logic.

## Location

- `src/render/VulkanContext.h` - Instance, physical device, logical device, queues
- `src/render/Swapchain.h` - Swapchain creation, image acquisition, presentation, recreation
- `src/render/Image.h/cpp` - GPU image with ImageState tracking (including Invalid) and mipmap generation
- `src/render/Barrier.h/cpp` - Centralized image barrier management (ImageState → Vulkan layout/stage/access)
- `src/render/RenderConfig.h` - User-settable render config: algorithms, quality params, shadow bias
- `src/render/RenderContext.h` - Per-frame immutable scene snapshot with opaque config pointer.
- `src/render/ProfilingData.h` - Vulkan-free profiling POD (PassProfile, FrameProfile)
- `src/render/GPUProfiler.h/cpp` - GPU timestamp query pool (per-frame, per-pass)
- `src/render/shaders/Shader.h/cpp` - Per-mesh shader container (multi-stage, owns ShaderUnits)
- `src/render/shaders/ShaderUnit.h` - Per-stage shader state: code text, parsed IR (`ShaderStruct`), SPIR-V, version
- `src/render/shaders/ShaderLibrary.h/cpp` - Load/parse/generate/compile service for shaders
- `src/render/shaders/ShaderParser.h/cpp` - GLSL → `ShaderStruct` IR (ShaderEditor struct mode)
- `src/render/shaders/ShaderGenerator.h/cpp` - `ShaderStruct` IR → GLSL
- `src/render/shaders/RenderShader.h/cpp, ComputeShader.h/cpp` - Render/compute pipeline wrappers (parsed + generated shaders)
- `src/render/shaders/ShaderCompiler.h/cpp, ShaderGPU.h` - SPIR-V compilation and GPU shader module
- `src/render/Renderer.h` - Public renderer API, frame drawing
- `src/render/RenderCache.h/cpp` - Cross-frame resource pool; owns MeshGPU, EnvironmentGPU, LightingCache, CameraGPU, DebugCache, attachments, shadow maps
- `src/render/UploadManager.h/cpp` - CPU-to-GPU upload service (meshes, lights, environments, IBL)
  - `UploadMesh(const Mesh&)` null-checks `o_mesh` then delegates to
    `UploadMeshData(const MeshData&)`, which owns the 14→8 float vertex stripping.
    Scene objects that carry a `MeshData` without being a `Mesh` — `DebugMesh` —
    call the latter directly rather than duplicating the stripping.
- `src/render/Texture.h/cpp` - Texture resource (Image + sampler + descriptor)
- `src/render/resources/LightingCache.h/cpp` - GPU-side light SSBO storage (point + sun, push constants)
- `src/render/resources/MeshGPU.h` - GPU-side mesh resources (VertexBuffer + IndexBuffer) + MeshPushConstants
- `src/render/resources/EnvironmentGPU.h` - GPU-side IBL resources (diffuse + specular cubemap Textures)
- `src/render/resources/LightGPU.h` - Per-light shadow GPU resources (shadow depth cubemap/map)

## Core Responsibilities

1. **Device Management** (`VulkanContext`)
   - Create VkDevice from Qt's QVulkanInstance
   - Select physical device (prefer discrete GPU, fallback to first available)
   - Choose graphics queue family
   - Enable validation layers in Debug mode
   - Report device capabilities and extensions

2. **Swapchain Management** (`Swapchain`)
   - Create swapchain from VkSurfaceKHR (borrowed from UI layer)
   - Select surface format (prefer `VK_FORMAT_B8G8R8A8_SRGB`)
   - Select present mode (prefer `VK_PRESENT_MODE_FIFO_KHR` for VSync)
   - Clamp extent to surface capabilities
   - Create image views for each swapchain image
   - Recreate on window resize (old swapchain destroyed, new created)

3. **Shader and Pipeline Management** (`Shader`)
   - Load SPIR-V from embedded C header arrays (generated at build time)
   - Create ShaderGPU instances
   - Create vk::raii::Pipeline via VK_KHR_dynamic_rendering
   - Pipeline layout (empty for triangle; uniforms added later)

4. **Frame Rendering** (`Renderer`)
   - Command pool and command buffer creation
   - Semaphore pair: imageAvailable + renderFinished
   - Fence: inFlightFence for CPU-GPU sync
   - DrawFrame(): acquire → begin dynamic rendering → bind pipeline → draw → end → present
   - WaitIdle(): DeviceWaitIdle for clean shutdown

## Data Flow

```
UIEvents::newFrame() → Renderer::DrawFrame()
                                ├── Swapchain::AcquireNextImage()
                                ├── Begin dynamic rendering
                                ├── Bind pipeline
                                ├── Draw (3 vertices for triangle)
                                ├── End dynamic rendering
                                └── Swapchain::Present()
```

## Vulkan-HPP RAII Conventions

All Vulkan objects use the `vk::raii` namespace:

```cpp
// DO: RAII - automatically destroyed on scope exit
vk::raii::Device device(physicalDevice, deviceCreateInfo);

// DON'T: raw handles requiring manual vkDestroy
// VkDevice device;
```

**Ownership rules:**
- `vk::raii` objects are non-copyable (move-only)
- Store as class members for object lifetime
- Pass by reference when sharing (non-owning)
- Use `= delete` on copy constructor and assignment operator for classes owning GPU resources

## Validation Layers

**Debug mode:**
- Enable `VK_LAYER_KHRONOS_validation`
- Set up debug utils messenger for callback-based reporting
- Report all validation messages via UIEvents signal

**Release mode:**
- No validation layers (performance)
- Minimal error checking (asserts only for unrecoverable states)

## Error Handling

### Device Loss (`VK_ERROR_DEVICE_LOST`)
- Notify user via UIEvents
- Attempt clean shutdown (destroy resources)
- Do not crash or enter infinite loop

### Swapchain Out-of-Date (`VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR`)
- Normal lifecycle event (window resize, minimize)
- Recreate swapchain with new surface dimensions
- Continue rendering with new swapchain

### Surface Lost (`VK_ERROR_SURFACE_LOST_KHR`)
- Window destroyed or display disconnected
- Clean up, stop rendering, signal application to exit

## Architectural Boundaries

### ✅ Renderer MAY:
- Read scene data via const reference
- Subscribe to UIEvents signals for configuration changes
- Own GPU resources (device, swapchain, pipeline, command buffers, RenderCache, attachments)
- Emit performance metrics or warnings via UIEvents

### ❌ Renderer MUST NOT:
- Mutate application state
- Depend on Editor or UI layers (except borrowed VkSurfaceKHR reference)
- Directly call UI or Editor functions
- Include headers from `src/editor/` or `src/ui/`

## Current Implementation (Triangle MVP)

The triangle MVP implements a minimal but correct rendering path:

1. **No vertex buffers** - Triangle vertices are hard-coded in the vertex shader
   using `gl_VertexIndex` to select positions/colors
2. **No descriptor sets** - No uniforms needed for static triangle
3. **No depth buffer** - Single triangle, no depth testing needed
4. **VK_KHR_dynamic_rendering** - No explicit VkRenderPass/VkFramebuffer objects
5. **Single command buffer** - Recorded once, replayed each frame
6. **Single in-flight frame** - No frame overlap (fence-synchronized)

## Shader Editing & Pipeline

Per-mesh shaders are editable at runtime through the Shader Editor panel. The
pipeline lives entirely in the Renderer layer (`src/render/shaders/`):

```
ShaderLibrary::LoadRenderShader(name, vertPath, fragPath)
    └── Shader (owns ShaderUnit per stage: VERTEX/FRAGMENT/COMPUTE/GEOMETRY)
          ├── ShaderUnit::code     — GLSL source text (edited in Code mode)
          ├── ShaderUnit::parsed   — ShaderStruct IR (edited in Structure mode)
          ├── ShaderUnit::spv      — compiled SPIR-V
          └── ShaderUnit::m_version— bumped on successful compile
                │
                ▼
GeometryPass pipeline (mesh shader) rebuilt when Shader::m_version changes
```

**Edit flow (event-driven):** `ShaderEditorPanel` emits `ShaderEvents`
(`ShaderCodeEdited`, `ShaderStructEdited`, `ShaderFieldAdded`) → `ShaderController`
mutates the `Mesh`'s shader data. `ShaderCreateRequested` /
`ShaderCompileRequested` recompile via `ShaderLibrary::Compile` and bump
`m_version`; the pipeline cache sees the new version and rebuilds the pipeline
on the next frame. After create/compile, `ShaderController` enqueues
`RenderResetEvent` so temporal shadow accumulation resets.

**Structure mode round-trip:** `ShaderParser` parses GLSL → `ShaderStruct` IR
(8 containers: attributes, pass outputs, inputs, outputs, uniforms, struct defs,
functions, push constants); `ShaderGenerator` regenerates GLSL from the IR.

## Current Render Pipeline

The whole pipeline is orchestrated by a **RenderGraph** (`src/render/render_graph/`).
`DeferredRenderer::m_mainGraph` is a compile-once DAG of passes; each pass declares
its image reads/writes via `Pass::GetIO()` (see `passes/Pass.h`), the graph wires
producer→consumer edges by `AttachmentName`, topologically sorts once, and
`recordFrame()` dispatches the whole pipeline with `m_mainGraph.Execute()`.

The graph is a projection of `RenderConfig`: `RebuildMainGraph()` is re-run only
when the config-derived `PipelineSignature` changes (e.g. FXAA toggled), so the
DAG always contains exactly the passes that will run. Passes still own their
barriers and descriptor writes internally; any valid topological order is correct.

Shadow resources are logical `AttachmentName` members resolved by their owning
pass (not `GetAttachment`): `ShadowDepth` (per-light bundle in `LightGPU`) and
`ShadowIntensity` (2D array via `GetShadowIntensityArray`).

```
ShadowDepthPass (per-light depth → RenderCache via GetShadowMap(uid, lightType))
    ├── Point light: cubemap geometry pass (6 faces, multiview)
    └── Sun light:   2D orthographic geometry pass (2048×2048, single view)
    │
    ▼
GeometryPass (G-Buffer MRT: Position, Normal, Albedo, MetallicRoughness, Depth)
    │
    ▼
SSAOPass (compute: reads G-Buffer, writes AO to R8 attachment)
    │
    ▼
ShadowIntensityPass (compute: per-light shadow eval → layered R8_UNORM 2D_ARRAY)
    ├── Point light: samplerCube depth comparison, PCF via cubemap sampling
    └── Sun light:   sampler2D depth comparison, ortho PCF, NDC Z in [0,1]
    │
    ▼
LightingPass (compute: reads G-Buffer + AO + shadow intensity array,
              reads LightingCache SSBOs from RenderCache, writes HDRColor)
    ├── Binding 5: PointLight SSBO (from RenderCache::GetLightingCache())
    └── Binding 6: SunLight SSBO   (from RenderCache::GetLightingCache())
    │
    ▼
IBLPass (compute: reads G-Buffer + HDRColor, applies diffuse+specular IBL, writes HDRColor)
    │
    ▼
GizmoPass (compute: reads IDBuffer, 3×3 edge detection for activeObjectId, writes R8 highlight)
    │
    ▼
ComposePass (compute: blends GizmoHighlight onto HDRColor, applies gamma correction, writes ComposedOutput)
    │
    ▼
DebugPass (raster: draws DebugDrawList over ComposedOutput with LOAD_OP_LOAD,
           depth-tested against the G-Buffer Depth it only reads)
    ├── lines      → screen-space quads (6 verts/segment, SSBO-indexed)
    ├── points     → ePointList sprites (square / rhombus / circle mask)
    └── wireframes → PolygonMode::eLine over the DebugMesh's existing MeshGPU
    │
    ▼
FXAAPass (compute: reads ComposedOutput, luma-based edge detection + full-iteration edge search, writes FXAAOutput)
    │  (conditional: only when AA == AAAlg::FXAA)
    ▼
Blit (ComposedOutput or FXAAOutput) → Swapchain (vkCmdBlitImage)
```

### ImageState & Barrier Convention

All image layout transitions **MUST** go through `Barrier::Transition()`, never raw
`vk::ImageMemoryBarrier` / `vk::ImageMemoryBarrier2`:

```cpp
// DO: use Barrier
Barrier::Transition(cmdBuf, myImage, ImageState::ColorShaderRead);

// DON'T: raw Vulkan barriers on Image objects
// vk::ImageMemoryBarrier2 barrier(...); cmd.pipelineBarrier2(...);
```

- `Image` tracks its logical state via `ImageState m_state`.
- `Barrier::Transition(cmd, image, after)` reads `image.State()` as the "before"
  layout, emits a `vkCmdPipelineBarrier2`, and updates `m_state` to `after`.
- `Barrier::Transition(cmd, image, after, subresourceRange)` does the same but
  with an explicit subresource range — does **not** update `m_state` (caller
  must manage state for partial transitions).
- Raw `vk::ImageMemoryBarrier2` is acceptable **only** for:
  - Raw `VkImage` handles not wrapped in `Image` (e.g. swapchain images)
  - Same-layout memory barriers (`eGeneral → eGeneral`) within compute passes

**Access masks carry the read bits, not just the write bits.** `ColorAttachment`
and `DepthAttachment` map to `…AttachmentWrite | …AttachmentRead`, because an
attachment reached with `VK_ATTACHMENT_LOAD_OP_LOAD` (DebugPass draws over
ComposedOutput) or consumed by the depth test without depth writes (DebugPass
again) *reads* the image through the attachment stage — a write-only
`dstAccessMask` leaves the previous writer's data unavailable to the load, which
on a tiler shows up as stale tiles blended into the frame. Passes that only
clear-and-write are unaffected by the wider mask.

**A pass transitions the images it is about to *use*, and leaves its own outputs
in its own write state.** A producer must never pre-transition an output into the
state it guesses the next consumer wants. `Barrier::Transition` derives the
`srcAccessMask` from the state the image is *currently* in, so a producer that
hands over e.g. `TransferSrc` gives the consumer's own barrier a src scope of
`eTransferRead` — which names no writes, makes none available, and so creates a
memory dependency covering nothing. ComposePass used to end with
`Transition(ComposedOutput, TransferSrc)` "ready for the blit"; DebugPass's
`TransferSrc → ColorAttachment` barrier then failed to order its overlay against
ComposePass's compute writes, and on MoltenVK the compute and render encoders
overlapped: every frame kept a different random subset of the overlay's tiles
with ComposePass's output in the rest — a per-frame-random tear that no amount of
extra serialization *after* DebugPass could fix. The rule now:
`ComposePass`/`FXAAPass` leave their output in `ShaderWrite`, `DebugPass` leaves
`ComposedOutput` in `ColorAttachment`, and the swapchain blit in
`DeferredRenderer::recordFrame` transitions its own source to `TransferSrc`.
Note that validation does not catch this — the layouts are all consistent — so
synchronization validation plus a coloured `LOAD_OP_CLEAR` probe is the way to
find it.

**`Undefined`/`Invalid` use `eAllCommands`, never `eTopOfPipe`, as a source
stage.** They only ever appear as the "before" state of a discard transition, so
the access mask stays empty — but `eTopOfPipe` in a `srcStageMask` creates no
execution dependency at all, which leaves a discard transition unordered against
whoever last read the image. The concrete case is the swapchain image: it is
transitioned `Undefined → TransferDst` for the final blit while the presentation
engine may still be reading it. For the same reason the submit waits the acquire
semaphore at `eColorAttachmentOutput | eTransfer` — the first use of a swapchain
image on this path is a transfer, not a color-attachment write. Both were caught
by synchronization validation (`SYNC-HAZARD-WRITE-AFTER-READ` at
`vkQueueSubmit`); re-run it with
`VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`
after touching barriers or submit scopes.

### ImageState::Invalid Convention

- `ImageState::Invalid` signals an image whose GPU creation failed (e.g.
  missing source data, unsupported format). The image has no valid GPU resources.
- `Barrier::Transition` maps `Invalid` → `Undefined` layout (safe no-op barrier).
- `Image::FromImageData()` returns `std::shared_ptr<Image>` — on failure, returns
  a shared_ptr containing an empty Image with `ImageState::Invalid`.
- Callers should check `image.State() != ImageState::Invalid` before using the image.
- Default-constructed `Image` is empty (all handles null, `Undefined` state);
  factory functions set `Invalid` explicitly on failure.

### SSAO Convention
- **AO value**: 1.0 = fully occluded (black), 0.0 = no occlusion (lit)
- **Sampling**: Hemisphere samples in view-space, random rotation via 16×16 noise texture
- **Output**: `VK_FORMAT_R8_UNORM` attachment (`AttachmentName::SSAO`)
- **Lighting**: Ambient term multiplied by `(1.0 - ao)` so occluded areas receive less ambient light
- **Radius**: Default 0.15 (appropriate for [-1, 1] scene scale)

### GizmoPass Convention
- **IDBuffer**: Reads `VK_FORMAT_R32_UINT` attachment at binding 0, written by GeometryPass
  (stores `objectId` as a 32-bit unsigned integer per pixel).
- **Edge detection**: 3×3 Laplacian-style kernel over neighboring IDBuffer pixels.
  A pixel is highlighted when at least one neighbor has a different objectId AND
  the center pixel's objectId matches `activeObjectId` (push constant).
- **Output**: `VK_FORMAT_R8_UNORM` attachment (`AttachmentName::GizmoHighlight`) at binding 1.
  Highlighted pixels = 255 (edge of selected object); all others = 0.
- **Push constant**: `uint32_t activeObjectId` — set to the currently selected object ID
  from `RenderContext::activeObjectId`. When `activeObjectId == 0`, the pass early-outs
  (no highlight written).

### ComposePass Convention
- **Inputs**: Reads `HDRColor` (binding 0, `R16G16B16A16_SFLOAT`) and
  `GizmoHighlight` (binding 1, `R8_UNORM`).
- **Highlight blending**: When a pixel's highlight value > 0, an orange highlight
  (`rgb(1.0, 0.5, 0.0)`) is blended at 50% alpha onto the HDR color:
  `composed = mix(hdr, highlightColor, highlight * 0.5)`.
- **Gamma correction**: After highlight blending, applies `pow(color, 1.0 / gamma)` where
  `gamma` is read from `RenderConfig::r_gamma` (default 1.0) via the push constant
  `float gamma`.
- **Output**: `ComposedOutput` (`R16G16B16A16_SFLOAT`) at binding 2, the final
  framebuffer before swapchain blit.

### DebugPass Convention

> Not to be confused with **GizmoPass** above: that is a compute pass that outlines
> the *selected* object from the IDBuffer. `DebugPass` is a raster pass that draws
> the scene's `DebugLine` / `DebugPoints` / `DebugMesh` objects (issue #22).

- **Gate**: `RenderConfig::r_debug_draw`. The pass stays in the RenderGraph
  unconditionally; the Editor publishes a null `EditorContext::debugDraw` when the
  toggle is off and `Record()` returns before touching any image. Toggling the
  overlay therefore costs **no graph rebuild** (unlike FXAA, which changes the
  `PipelineSignature`).
- **Target**: draws into `ComposedOutput` with `LOAD_OP_LOAD` so the tonemapped
  image survives underneath. It transitions the image from the `ShaderWrite` state
  ComposePass leaves it in — that barrier is the only thing ordering the overlay
  against ComposePass's compute dispatch — and leaves it in `ColorAttachment` for
  the blit to transition. See the barrier-ownership rule above; getting this wrong
  tears the overlay per-tile on MoltenVK with zero validation errors.
- **Depth**: reads the G-Buffer `Depth` with `LOAD_OP_LOAD`, `depthTestEnable` on
  and **`depthWriteEnable` off**. Debug geometry is occluded by solid objects but
  never occludes anything, including other debug geometry.
- **Three pipelines, one layout** (`p_pipelines[0..2]`): lines, points, wireframe.
  - Lines are `eTriangleList`: 6 vertices per segment expanded into a screen-space
    quad by `debug_line.vert`. Wide-line rasterization is deliberately *not* used —
    Metal caps `lineWidth` at 1.0, so `VK_EXT_line_rasterization` would silently
    degrade on macOS.
  - Points are `ePointList` with `gl_PointSize`; the shape is masked from
    `gl_PointCoord`. `gl_PointSize` is clamped to the device's `pointSizeRange[1]`,
    queried once and pushed as `DebugPushConstants::maxPointSizePx` (exceeding it is
    undefined behaviour).
  - Wireframe is `PolygonMode::eLine` over the DebugMesh's **existing** MeshGPU
    vertex/index buffers, so wireframe geometry is never duplicated or re-uploaded.
    That MeshGPU has to exist first: the pass skips any `DebugWireMesh` whose
    `meshObjectId` resolves to no cached buffers, so the Editor must upload it (see
    "MeshGPU upload for `DebugMesh`" in editor.instructions.md). A wireframe that
    simply does not appear is almost always a missing upload, not a pipeline bug.
- **X-ray is dynamic state, not a fourth pipeline**: `vk::DynamicState::eDepthTestEnable`
  is flipped between the depth-tested and x-ray halves of the partitioned
  `DebugDrawList`. The whole frame is at most **6 draws** (3 kinds × 2 depth modes),
  independent of primitive count — that is the structural half of issue #22's budget
  and `test_debug_pass.cpp` asserts it exactly (10,000 segments ⇒ 2 draws).
- **Blending**: alpha-over, no culling. Debug geometry is two-sided by nature.
- **Upload is revision-gated, and the pass does not do it**: debug producers are
  stateful, so the list is usually byte-identical to last frame's. The geometry lives
  in `RenderCache`'s `DebugCache`, not in the pass: `DeferredRenderer::recordFrame()`
  calls `RenderCache::UpdateDebugDraw(frameIndex, list)` once per frame, before any
  pass records, and a slot whose `uploadedRevision` already matches
  `DebugDrawList::revision` **skips the memcpy entirely**. `uploadedRevision` starts
  at `UINT64_MAX`, not 0, because a list legitimately at revision 0 would otherwise
  never upload.
- **Capacity grows with the geometry**: `DebugCache` sizes each slot's `CPUBuffer`
  pair in powers of two from 4 KiB, so nothing is clamped or dropped. Growth swaps
  the `VkBuffer` handle, which is why `DebugPass::Record()` re-writes bindings 0-2
  from the cache **every frame** — the same thing `LightingPass` does for the light
  SSBOs, and legal for the same reason: the frame being recorded has already waited
  on its fence. A slot is only ever written for a frame that is not in flight.
- **Camera**: the pass binds `RenderCache::GetCameraGPU()`, the renderer's single
  camera UBO (`{viewProj, view}`, the block `gbuffer.vert` declares), written once
  per frame by `recordFrame()`. `GeometryPass` binds the same buffer; neither owns
  one. `CameraGPU` also keeps the **projection matrix on the CPU**
  (`GetProjection()`) without uploading it: DebugPass converts a world-space size to
  pixels with `proj[1][1]`, and that factor cannot be recovered from `viewProj` once
  the view rotation is folded in (column-major: `(proj*view)[1][1]` sums over `k`).

### CPUBuffer Convention

`CPUBuffer` (`src/render/buffers/CPUBuffer.h`) is the **permanent** host-visible,
device-accessible buffer: mapped once at creation and written directly by the CPU.
It is named for the mirror image of `GPUBuffer`, which is device-local.

- Use it for data the CPU rewrites and the GPU reads in place — `DebugCache`'s
  segment/point SSBOs are the reference case.
- **`StagingBuffer` is for uploads and downloads only.** Do not retain one as a
  permanent buffer object; that is what `CPUBuffer` exists for.
- Use `GPUBuffer` (device-local + staging) for data that is written rarely and read
  hot, e.g. mesh vertex/index buffers. Do **not** reach for it just because a write
  looks infrequent: `GPUBuffer::Unmap()` and `ArrayBuffer::Resize()` each submit a
  copy and then wait for the queue to go idle. Debug geometry is re-flattened on
  every `RenderResetEvent`, which `CameraController` enqueues on every camera change,
  so that choice would put a full GPU stall in every frame of a viewport drag. A
  host-visible memcpy of a few hundred KB costs nothing by comparison.


- **Projection**: Orthographic (`glm::ortho()`) with configurable left/right/bottom/top planes and near/far planes
- **Depth range**: NDC Z in `[0, 1]` (Vulkan convention, requires `GLM_FORCE_DEPTH_ZERO_TO_ONE`)
- **Frustum parameters** (`Light.h` static constexpr):
  - `sun_shadow_field = 2.5` — XY half-extent (5×5 unit area centered on camera target)
  - `sun_shadow_near = -10` — near plane (view-space, behind eye)
  - `sun_shadow_far = 30` — far plane (view-space, extended past geometry so depth is not crammed at NDC Z=1.0)
  - `sun_depth_range = 10` — eye distance from ortho center (decoupled from far plane so geometry sits at mid-depth)
- **Eye placement**: `eye = center - sunDir * sun_depth_range` (NOT `* farPlane`, which would place geometry at the far boundary)
- **Push constant**: `SunShadowPushConstants` carries `mat4 lightViewProj` (view × projection)
- **Shadow map resolution**: 2048×2048, `VK_FORMAT_D32_SFLOAT`
- **PCF**: Percentage-closer filtering via `sampler2DShadow` with UV offset kernel, ortho depth comparison
- **Shadow intensity**: Written to per-light layer in `ShadowIntensity` 2D array via `SunShadowIntensityEval` compute shader
- **Shadow bias**: Depth bias read from `RenderConfig::r_shadow_bias` (default 0.02) via `ctx.editor.config`.
- **Shadow mode**: Supports `HARD`, `SOFT_PCF_16`, `SOFT_PCF_64` modes matching point light shadow pipeline

### Temporal Shadow Accumulation Convention

- **Jitter**: Per-frame 3D random direction (unit-ball vector from Halton(2,3,5)) applied
  as `pos_jittered = pos + light.radius * jitter` for point lights, and as UV-space
  offset `shadowUV + vec2(jitter.x, jitter.y) * uvRadius` for sun lights.
- **Accumulation**: In-place read-modify-write on the ShadowIntensity layered array.
  The compute shader reads the previous accumulated value via `imageLoad`, evaluates
  a single jittered shadow sample, and blends via EMA (exponential moving average)
  using `mix(prevAccum, sample, alpha)`, writing back via `imageStore`.
- **Alpha modes**: Two blend modes controlled by `RenderConfig::r_sampling_mode`:
  - Fixed EMA (0): `alpha = 1/8` — fast convergence
  - Moving Average (1): `alpha = 1/(iteration + 1)` — true averaging, resets on scene change
- **Iteration**: Global frame counter `m_iteration` in `DeferredRenderer`, exposed
  via `RenderContext::iteration`. Editor resets it through the event system when
  the scene changes — see `RenderResetEvent` in `events.instructions.md`.
- **Reset**: Any scene-changing event (camera move, light change, object transform,
  visibility toggle, config change, project load, asset import) enqueues
  `RenderResetEvent`. The Editor subscribes and calls
  `DeferredRenderer::ResetShadowAccumulation()` → sets `m_iteration = 0`.

### FXAA Convention
- **Algorithm**: NVIDIA FXAA 3.11, ported from cuda-vision full-iteration approach
- **Pipeline**: Two compute dispatches per frame: edge detection (Sobel gradient, subpixel offset) → edge-line iteration (march along edge, check 3 rows per step: center/upper/lower) → endpoint interpolation → bilinear resample
- **Direction**: Sobel gradient magnitude determines horizontal (`abs(gy) >= abs(gx)`) vs vertical edge
- **Offset**: Subpixel offset in `[-0.5, 0.5]` computed as `(neighborLo-center)/(neighborLo-neighborHi) - 0.5`
- **Edge march**: Iterates along the edge line (left/right for horizontal, up/down for vertical), up to 32 steps per direction. FLIP: center pixel luma differs from starting luma (crossed edge). END: off-axis neighbor has same luma as starting luma (left edge region). BOUND: out of image bounds.
- **Endpoint interpolation**: Endpoint offsets assigned by stop code (END→0, FLIP→±0.5, otherwise→original offset). Final offset = `mix(leftOff, rightOff, leftSteps/(leftSteps+rightSteps))`
- **Resample**: Bilinear `texture()` at subpixel coordinate offset by `finalOffset * rcpFrame` along the edge normal
- **Sampler**: Bilinear (`VK_FILTER_LINEAR`) for sub-pixel accuracy; falls back to nearest if `R16G16B16A16_SFLOAT` format doesn't support `VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT`
- **Config**: `RenderConfig::r_fxaa_subpix` (strength, default 0.75), `r_fxaa_edge_threshold` (default 0.166), `r_fxaa_edge_threshold_min` (default 0.0833)
- **Gating**: `RenderConfig::RequiresFXAA()` — only records when AA algorithm is set to FXAA in RenderConfigPanel

### RenderConfig Convention

`RenderConfig` (`src/render/RenderConfig.h`) is user-facing config owned by Editor,
passed to passes through `RenderContext::editor.config` (opaque `void*`):

- **Algorithm selection**: `r_pipeline` (Forward/Deferred), `r_aa`, `r_ao`, `r_shadow`, `r_ssr`
- **Quality parameters**: `r_gamma`, `r_ao_ksize`, `r_ao_radius`, `r_shadow_bias` (0.02), `r_sample_pf`
- **Overlay toggle**: `r_debug_draw` / `RequiresDebugDraw()` — gates *publication*, not
  graph topology (see DebugPass Convention). It is written **last** in `serialize()` and
  read back with `NEURUS_OPTIONAL_NVP(ar, r_debug_draw, true)`, because a project file
  saved before the field existed would otherwise make `ConfigComponent::Load` discard the
  whole config and reset the user's gamma, AO and shadow settings. **Any field added to
  `RenderConfig` from now on must follow that optional-trailing-field idiom** — see
  `src/core/Serialize.h`.
- **Serialized** via cereal for project save/load
- **Live-update**: passes cast `static_cast<const RenderConfig*>(ctx.editor.config)` each frame; scalar param changes take effect on next `DrawFrame()`
- **Shadow bias flow**: `RenderConfigPanel` slider → `configValueChanged` → `Editor::SetRenderConfig` → `RenderContext::editor.config` → `ShadowIntensityPass` casts to `RenderConfig*`, reads `r_shadow_bias`

### Attachment Formats

All screen-space attachments (Position, Normal, Albedo, MetallicRoughness, Depth, HDRColor,
SSAO, GizmoHighlight, ComposedOutput, FXAAOutput) are created lazily via
`RenderCache::GetAttachment(name, extent)` on first use.
Per-light shadow maps are managed via `RenderCache::GetShadowMap(lightUID, lightType)`:
- `LightType::POINTLIGHT` → cubemap (6-layer 2D_ARRAY, D32_SFLOAT, 1024×1024 per face)
- `LightType::SUNLIGHT` → 2D orthographic (D32_SFLOAT, 2048×2048)
The shadow intensity array (R8_UNORM, layered 2D_ARRAY) is created via
`RenderCache::GetShadowIntensityArray(extent)` with per-light layer indices
assigned via `RenderCache::GetShadowIntensityLayer(lightUID, extent)`.

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
| GizmoHighlight | R8_UNORM | 0 | Selected-object edge highlight (GizmoPass output) |
| ComposedOutput | R16G16B16A16_SFLOAT | (0,0,0,0) | Final composed output before FXAA/blit (ComposePass output) |
| FXAAOutput | R16G16B16A16_SFLOAT | (0,0,0,0) | FXAA anti-aliased output (FXAAPass output, blitted when FXAA active) |
| FXAAOffsets | R16G16_SFLOAT | (0,0) | FXAA edge subpixel offsets (RG16F, 2-channel, sampled+storage) |
| ShadowMap | D32_SFLOAT | 1.0 | Per-light shadow depth (RenderCache-owned). Cubemap (6-layer 2D_ARRAY, 1024×1024) for point lights; 2D (2048×2048) for sun lights |
| ShadowIntensity | R8_UNORM | 0 (no shadow) | Layered 2D_ARRAY, one layer per shadow-casting light (RenderCache-owned) |

### RenderCache GPU Resources (MeshGPU, EnvironmentGPU, LightingCache, CameraGPU, DebugCache)

`RenderCache` owns cross-frame mutable GPU resources beyond framebuffer attachments.
These resources separate GPU ownership from the Vulkan-free scene and asset layers:

**MeshGPU** (`src/render/MeshGPU.h`)
- Holds device-local `VertexBuffer` + `IndexBuffer` for a mesh, plus vertex/index counts
- Created lazily via `RenderCache::GetMeshGPU(objectId, meshData, device, pd, queue, qfi)`
  which uploads geometry from `MeshData` to GPU
- Subsequent calls return the cached `MeshGPU` immediately
- Destroyed via `RenderCache::RemoveMeshGPU(objectId)` or `RenderCache::Clean()`
- Scene `Mesh` objects call `RenderCache::GetMeshGPU()` through `Mesh::UploadToGPU()`;
  the scene layer never owns GPU buffers directly

**EnvironmentGPU** (`src/render/resources/EnvironmentGPU.h`)
- Holds diffuse irradiance and specular prefiltered cubemap `Texture` objects
  (each wraps `Image` + sampler + descriptor)
- Created lazily via `RenderCache::CreateEnvironmentGPU(envId, device, pd, queue, qfi, env)`
  from an `Environment` scene object
- Read per-frame by `LightingPass` via `RenderCache::GetEnvironmentGPU(envId)`
- Destroyed via `RenderCache::RemoveEnvironmentGPU(envId)`

**LightingCache** (`src/render/resources/LightingCache.h`)
- Manages point light and sun light SSBOs (device-local GPUBuffers)
- Built by `UploadManager` (it needs a queue) and handed over with
  `RenderCache::SetLightingCache(std::unique_ptr<LightingCache>)`, so `RenderCache`
  never has to store a queue/qfi of its own
- Updated via `RenderCache::UpdateLighting(variantDict)` — accepts a map of
  `variant<PointLightStruct, SunLightStruct>` keyed by light UID
- `RenderCache::GetLightingCache()` returns the LightingCache for per-frame SSBO binding
  by `LightingPass`
- Also defines `PointLightStruct`, `SunLightStruct` (std140-compatible, 48 bytes),
  and `LightingPushConstants` (176 bytes) — byte-for-byte matches with GLSL shaders

**CameraGPU** (`src/render/resources/CameraGPU.h`)
- The renderer's **single** camera UBO: `CameraUBOData{viewProj, view}` (128 bytes,
  matching the `CameraUBO` block in `gbuffer.vert` and `debug_*.vert`)
- Constructed **eagerly** in `RenderCache`'s constructor — it needs no queue — so
  `GetCameraGPU()` returns a reference and is never null
- Written once per frame by `DeferredRenderer::recordFrame()` via
  `RenderCache::UpdateCamera(proj, view)`, *before* any pass records. Passes only
  bind it; a pass that wrote it would make the result depend on pass order
- Keeps `proj` on the CPU behind `GetProjection()` without uploading it (see the
  DebugPass camera bullet for why `viewProj[1][1]` is not a substitute)
- `IsValid()` is false until the first `UpdateCamera()`. `GeometryPass` and
  `DebugPass` both bail out of `Record()` on a false, which is also why a **test**
  that drives either pass directly must publish a camera first — use
  `VulkanTestShared::PublishSceneCamera(cache, scene)`

**DebugCache** (`src/render/resources/DebugCache.h`)
- Per-frame-slot `CPUBuffer` pair (segments + point sprites) holding the flattened
  `DebugDrawList`, plus the partition boundaries and counts `DebugPass` draws from
- Also eager in `RenderCache`'s constructor, but allocates nothing until the first
  `UpdateDebugDraw()`; the slot vector self-sizes on `frameIndex`, so no
  `framesInFlight` count is threaded through the renderer
- Written once per frame by `recordFrame()` via
  `RenderCache::UpdateDebugDraw(frameIndex, list)`; revision-gated, so an unchanged
  list copies nothing
- `GetFrame(frameIndex)` returns a `FrameView` of borrowed pointers + counts. Null
  buffers mean no `UpdateDebugDraw()` has run for that slot, and `DebugPass` returns
  without touching an image

**MeshPushConstants** (`src/render/resources/MeshGPU.h`)
- Per-mesh push-constant block sent to the vertex shader (128 bytes total)
- Two mat4s: `model` (local-to-world transform) and `normalMatrix`

**GeometryRenderItem** (removed)
- Previously mixed CPU (MeshData) and GPU (VertexBuffer, IndexBuffer) concerns in one struct
- Removed as part of CPU/GPU isolation refactoring. GPU resources now live in `MeshGPU`;
  CPU data stays in `MeshData`

## RenderGraph

The deferred pipeline is orchestrated by a **RenderGraph** (`src/render/render_graph/`),
built on the generic `neurus::Graph<SData, NData>` DAG template (`src/core/Graph.h`).

**Model**
- `RenderGraph` = `Graph<AttachmentName, PassEntry>`. One node per pass; a node's
  `PassEntry` holds the `Pass*` and its cached `PassIO`.
- Each pass declares its image I/O via `Pass::GetIO()` returning a `PassIO`
  (name + read/write `AttachmentBinding`s: resource, binding slot, descriptor
  type, image layout). This is the single declaration the graph consumes.
- Edges are keyed by `AttachmentName`: `Connect(producer, resource, consumer)`
  links the producer's output socket for that resource to the consumer's input
  socket. Fan-out (one write → many reads) is supported.

**Lifecycle**
- `AddPass(pass)` — materializes one socket per declared read/write from `GetIO()`.
- `Connect(...)` — wires producer→consumer; self-loops and duplicate edges are
  rejected (returns `false`).
- `Compile()` — Kahn topological sort. Inputs with no in-graph producer are
  **external** (produced by a RenderCache attachment) and are allowed. A cycle
  throws `std::runtime_error` naming the involved passes.
- `Execute(cmd, cache, ctx)` - invokes each pass's `Record()` in compiled order.
  Pure dispatch: the graph owns no profiling state. Per-pass CPU/GPU timing and
  draw/dispatch counters are collected by `DeferredRenderer::recordFrame`, which
  iterates `CompiledOrder()` itself while profiling is enabled - so
  `RenderContext` stays an immutable snapshot.
- `Clear()` — drops all nodes so the graph can be rebuilt.

**Config-driven topology (single source of truth = RenderConfig)**
- `DeferredRenderer::m_mainGraph` holds the whole pipeline. It is rebuilt
  (`RebuildMainGraph`) only when the `PipelineSignature` derived from
  `RenderConfig` changes (currently FXAA on/off), so the DAG always contains
  exactly the passes that will run — no per-node "enabled" state to drift.

**Resources & descriptors**
- `DescriptorBinder` (`render_graph/DescriptorBinder.h`) translates a pass's
  `PassIO` image bindings into descriptor-set writes.
- Shadow resources are logical `AttachmentName` members resolved by their owning
  pass, **not** `GetAttachment`: `ShadowDepth` (per-light bundle in `LightGPU`)
  and `ShadowIntensity` (2D array via `GetShadowIntensityArray`). `ConfigFor`
  throws for them to catch accidental single-attachment fetches.

**Ownership boundary (current)**
- Passes still own their own barriers and descriptor writes inside `Record()`.
  The graph only orders and dispatches. Moving barrier/descriptor/resource
  ownership into the graph (and enabling transient aliasing + custom passes) is
  tracked in issue #32; parallel execution built on it in #33–#37.

**Tests**: `test/render/test_render_graph.cpp` - socket materialization,
connect/duplicate rejection, external inputs, linear/fan-out/diamond ordering,
multi-resource edges, cycle detection (with named nodes), rebuild determinism,
clear/reuse, and execute preconditions. Pass draw/dispatch reporting via the
`PassStats` return value is covered by `PassCountersTest.RecordReturnsStats`
(GPU fixture).

## GPU Profiling (issue #33)

Per-frame profiling answers whether the renderer is **CPU-bound** (command
recording) or **GPU-bound** before any parallel-rendering work.

**Always on**: profiling is unconditional. `DeferredRenderer` owns a
`GPUProfiler` and a `FrameProfile` and populates them every frame — there is no
enable/disable toggle. `Application::InitRenderer()` needs no profiling call;
`DrawFrame()` always returns a live `FrameProfile`.

**What is measured** (`FrameProfile` in `ProfilingData.h`)
- `cpuRecordMs` - total `RenderGraph::Execute()` command-recording wall time.
- Per pass: `cpuMs` (around each `Record()` in `RenderGraph::Execute`), `gpuMs`
  (timestamp queries), `drawCalls`, `dispatches`.
- Frame totals: `drawCalls`, `dispatches`, `passCount`, `gpuTotalMs`.

**GPU timestamps** (`GPUProfiler`, section-based)
- One `vk::QueryPool` of `kMaxFramesInFlight * (2 + 2 * kMaxPasses)` timestamps;
  per frame slot: frame begin@0, frame end@1, pass i begin@2+2*i, end@3+2*i.
- The RenderGraph drives the profiler around each pass:
  `BeginFrame` → (`BeginPass`/`EndPass`)* → `EndFrame`. Passes stay unaware of
  GPU profiling. `Resolve()` reads back the slot about to be reused (after its
  fence signals) non-blockingly, so results lag CPU data by 1-2 frames by design.
- The pool is created only when `timestampComputeAndGraphics` and the graphics
  queue family's `timestampValidBits` allow it; otherwise `Available()==false`
  and the Profiling panel/log show CPU data only.

**Counters**: each pass reports its own draw/dispatch counts by returning a
`PassStats { drawCalls, dispatches }` from `Record()` — passes are stateless
with respect to profiling (no members, no reset). `RenderContext` carries no
profiling data - it stays an immutable per-frame snapshot. `RenderGraph::Execute`
aggregates the returned `PassStats` into each `PassProfile` and sums them into
the frame totals.

**Surface**: `DeferredRenderer::DrawFrame()` returns the `FrameProfile`; the
Application layer assembles the `UIContext` each frame (the Editor never produces
it) and sets `UIContext::profile` to the returned profile (opaque `const void*`).
The ProfilingPanel casts it back and renders the per-pass timings as a tree
(Frame totals + per-pass rows). See ui-system.instructions.md.

## Swapchain & Frame Pacing Convention

- **Usage flags**: `Swapchain` requests
  `eColorAttachment | eTransferDst | eTransferSrc`, masked by
  `capabilities.supportedUsageFlags`. `eTransferDst` is the final blit's target;
  `eTransferSrc` exists solely so `Screenshot` can copy the presented image back —
  without it the capture path raises copy-source validation errors. The constructor
  and `Recreate()` must request the same set.
- **`kMaxFramesInFlight` is 1.** `RenderCache::GetAttachment()` returns a *single*
  shared `Image` per `AttachmentName` (the extent argument is honoured only on the
  first, lazy creation), so a second in-flight frame would write
  `ComposedOutput`/`Depth` while the previous frame's blit still reads them, across
  two submits with no dependency between them. Raising it requires per-frame-slot
  attachments first.
- **`kFenceTimeoutNs` is a deadlock guard, not a frame budget** (5 s). On timeout
  `DrawFrame` abandons a frame it has already acquired an image for, so a value
  near the real frame time silently drops frames — a Debug build with validation
  layers on measures ~600 ms per frame, which the old 100 ms tripped every frame.

## Future Evolution

- IBL enhancements (BRDF LUT, multi-scattering)
- Shadow mapping improvements (multi-light, shadow evaluation pass)
- SSR (Screen-Space Reflections): ray marching variants
- Tonemapping: filmic (ACES) + gamma correction
- FXAA: luma-based edge anti-aliasing
- VMA integration for memory management
- Multiple in-flight frames (double/triple buffering) — done (`kMaxFramesInFlight`)
- Threaded command buffer recording — see issues #35 / #37
- Pipeline cache for faster startup
- Render graph abstraction — done (see the RenderGraph section above); graph-owned
  resources/barriers/descriptors tracked in #32
