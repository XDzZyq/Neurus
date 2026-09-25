# Asset Layer & GPU Resource Management

## Overview

The Asset layer (`src/asset/`) manages **loading, storing, and providing access**
to external asset data (meshes, images). It is **Vulkan-free**: no `<vulkan/>`
includes appear in public headers. Format queries use the `PixelFormat` enum
rather than raw `vk::Format` values. GPU resource abstractions (buffers,
images, descriptors) are owned by the **Renderer layer** (`src/render/`) -- see
`renderer.instructions.md` for the Buffer class hierarchy (Buffer, StagingBuffer, GPUBuffer, UniformBuffer), Image, Texture, and
DescriptorManager.

The original "Data & Resource" layer concept has been absorbed: asset loading
lives in `src/asset/`, GPU resource management lives in `src/render/`.

## Location

- `src/asset/data/MeshData.h/cpp` - Mesh geometry data (vertices, indices)
- `src/asset/data/ImageData.h/cpp` - Image pixel data (CPU-side, owning vector, PNG/HDR save)
- `src/asset/PixelFormat.h` - Vulkan-free pixel format enum for CPU-side format queries (PixelByteSize, ChannelCount, IsSRGB, IsHDR helpers)
- `src/asset/Project.h/cpp` - Pure registration-based serializer (no data ownership)
- `src/asset/Serializable.h` - Abstract base class: Key/Save/Load virtual interface
- `src/asset/components/SceneComponent.h/cpp` - Scene serialization adapter (Serializable implementation)
- `src/asset/components/ResourceComponent.h/cpp` - ResourceManager pool serialization adapter (Serializable implementation); Load also wires pooled data references (Mesh -> MeshData/Shader, Environment -> ImageData) so the pool is self-contained and uploadable after a reload
- `src/asset/components/ConfigComponent.h/cpp` - RenderConfig serialization adapter (Serializable implementation)
- `src/asset/components/UIComponent.h/cpp` - UI-state serialization adapter (opaque layout blob; Serializable implementation)
- `src/asset/components/HistoryComponent.h/cpp` - Undo/redo-stack serialization adapter (Serializable implementation; header forward-declares OperationManager, .cpp includes editor/operations headers)
- `src/scene/registrations/TypeRegistration.cpp` - Cereal polymorphic registration for scene object types
- `src/render/buffers/Buffer.h` - Virtual base class (Buffer) with m_buffer, m_memory
- `src/render/buffers/StagingBuffer.h/cpp` - Host-visible staging buffer (StagingBuffer) for CPU↔GPU transfers
- `src/render/buffers/GPUBuffer.h/cpp` - Device-local GPU buffer (GPUBuffer) with staging Map/Unmap
- `src/render/buffers/UniformBuffer.h` - Template uniform buffer (UniformBuffer<T>) for host-visible struct upload
- `src/render/buffers/VertexBuffer.h/cpp` - Vertex buffer (VertexBuffer, inherits GPUBuffer)
- `src/render/buffers/IndexBuffer.h/cpp` - Index buffer (IndexBuffer, inherits GPUBuffer)
- `src/render/Image.h/cpp` - GPU image with ImageState tracking (including Invalid), FromImageData factory (returns shared_ptr), mipmap gen
- `src/render/Barrier.h/cpp` - Centralized image barrier: ImageState → Vulkan layout/stage/access (maps Invalid → Undefined)
- `src/render/Texture.h/cpp` - Texture resource (combines Image + sampler + descriptor)
- `src/render/DescriptorManager.h/cpp` - Descriptor pool/set lifecycle management
- `src/render/UploadManager.h/cpp` - CPU-to-GPU upload service (meshes, lights, environments, IBL)
- `src/render/resources/LightingCache.h/cpp` - GPU-side light SSBO storage (point + sun, push constants)

## Core Responsibilities

1. **Mesh Data Loading** (`src/asset/data/MeshData.h/cpp`)
   - Parse OBJ files into vertex/index buffers
   - Compute vertex attributes (positions, normals, UVs, tangents)
   - Provide MeshData struct for GPU upload

2. **Image Data Loading** (`src/asset/data/ImageData.h/cpp`)
   - Decode PNG/BMP/HDR files into owned pixel buffers (std::vector&lt;uint8_t&gt;)
   - Owns width, height, and format metadata
   - Member functions: `SavePNG()` / `SaveHDR()` for disk output
   - Constructor from path auto-loads; CPU-side representation; GPU upload via `Image::FromImageData`

3. **Buffer Class Hierarchy** (`src/render/buffers/Buffer.h`)
   - Virtual base class `Buffer` with `m_buffer`, `m_memory`
   - `StagingBuffer` — host-visible transfers (owns m_queue)
   - `GPUBuffer` — device-local with staging-backed Map/Unmap
   - `UniformBuffer<T>` — host-visible template for uniform structs
   - Vertex/index buffers inherit from GPUBuffer

4. **GPU Image Abstraction** (`src/render/Image.h/cpp`)
   - Create `vk::raii::Image` with appropriate tiling, usage, memory
   - Create `vk::raii::ImageView` and `vk::raii::Sampler`
   - Track logical state via `ImageState` enum; all transitions through `Barrier::Transition`
   - Mipmap generation via `GenerateMipmaps()`

5. **Texture Management** (`src/render/Texture.h/cpp`)
   - Combines GPU Image + ImageView + Sampler into a single resource
   - Descriptor binding for shaders

6. **Descriptor Management** (`src/render/DescriptorManager.h/cpp`)
   - Pool-based descriptor allocation
   - Per-frame descriptor pool rotation
   - Descriptor set layout caching

7. **Project Serialization** (`src/asset/Project.h/cpp`)
   - Pure registration-based serializer; owns no data (no project path, no dirty flag)
   - Components register via `Register<T>(args...)` and implement `Serializable` base class
   - Save/Load iterates all registered components for persistence
   - No coupling to Scene, RenderConfig, or any concrete type
   - Coordinated at the Application level: `Application` owns the project path and
     builds a transient `Project` per save/open, registering `ResourceComponent`
     (the pool, FIRST — so it deserializes and wires pooled data references before
     the Scene resolves its ID lists) + `SceneComponent` +
     `ConfigComponent` (Editor-owned) and `UIComponent` (Application-owned UI blob
     sourced from `UIManager::ExportLayout()` / applied via `UIManager::ApplyLayout()`),
     plus `HistoryComponent` (undo/redo-stack adapter in asset/components that
     wraps the editor's `OperationManager`). `HistoryComponent`
     is registered last so legacy files without an `m_history` node load cleanly
     (its `Load` clears the stacks instead of throwing).
   - `Scene::serialize` persists selection state alongside the typed object
     pools. Selection is held at runtime as `const ObjectID*` pointers but
     written as UIDs (`selectedUids` + `activeUid`), mirroring `SelectionState`
     in `SceneOperations.h`. On load the UIDs are resolved back to pointers via
     `Scene::GetObjectID()` *after* `RebuildObjList()`. The selection block is
     optional: legacy files without it load with an empty selection.
   - **Optional trailing fields** (`src/core/Serialize.h`): a field appended after a
     format shipped is absent from older files, and cereal reports that by throwing
     `cereal::Exception` from the read — unhandled, the throw aborts the whole
     component's `Load`, so one new setting discards every setting around it. Write
     such fields **last** and read them through:
     - `NEURUS_OPTIONAL_NVP(ar, field, fallback)` for a single field
       (`RenderConfig::serialize` and `r_debug_draw`), or
     - `neurus::archive::OptionalBlock(ar, CEREAL_NVP(a), CEREAL_NVP(b), ...)`, which
       returns false on a load that found the block missing, when several fields must
       be defaulted together (`Scene::serialize`'s selection and debug-pool blocks).

     Order is permanent: the binary and portable-binary archives are positional, so
     once a file exists with the field, its position is fixed. The namespace is
     `archive`, not `serialize` — several headers declare free
     `neurus::serialize(Archive&, T&)` overloads for cereal to find by ADL, and a
     namespace of that name inside `neurus` redefines them.
   - The pointer↔UID conversion lives in scene-layer free functions
     `neurus::SnapshotSelectionUids` / `RestoreSelectionUids` (in `Scene.h`),
     shared by `Scene::serialize` and the editor's `SceneController` selection
     snapshot/restore. Kept in the scene layer (which owns the pointer↔UID
     mapping) so the editor depends downward only.

## Data Flow

```
File System (OBJ, PNG)
    │
    ▼
src/asset/data/: MeshData / ImageData (CPU-side loading)
    │                              ▲
    │  Image::FromImageData()      │  Image::ReadImageData()
    ▼                              │
src/render/: Image (GPU) ──────────┘
    │
    ▼
src/render/buffers/: Buffer / StagingBuffer / GPUBuffer / UniformBuffer (GPU buffers)
    │
    ▼
Renderer passes (GeometryPass, LightingPass): consume GPU resources
```

All image layout transitions go through `Barrier::Transition()`.

## Architectural Boundaries

### ✅ Asset & Resource Code MAY:
- Load and parse asset files (OBJ, PNG, HDR)
- Provide CPU-side data structs (MeshData, ImageData)
- Query pixel format metadata via `PixelFormat` enum (Vulkan-free)
- Own GPU memory allocations (VkDeviceMemory via Buffer, Image) -- Renderer layer only
- Provide allocation utilities to Renderer passes

### ❌ Asset & Resource Code MUST NOT:
- Include Vulkan headers in public interfaces (`src/asset/` is Vulkan-free)
- Issue draw calls (Renderer's responsibility)
- Create pipelines or shader modules (Renderer's responsibility)
- Manage swapchain or presentation (Renderer's responsibility)
- Depend on Editor or UI layers (exception: `components/HistoryComponent.cpp`
  includes editor/operations headers to persist the undo/redo stacks; its
  header stays editor-free so the dependency is confined to one .cpp)
- Store application-level state

## Current Scope

- OBJ mesh loading via MeshData (icosphere, cube, etc.)
- PNG/HDR image decoding via ImageData (owns pixel data, member SavePNG/SaveHDR)
- `PixelFormat` enum for CPU-side format queries (PixelByteSize, ChannelCount, IsSRGB, IsHDR) -- zero Vulkan includes
- Buffer hierarchy (Buffer, StagingBuffer, GPUBuffer, UniformBuffer<T>) for vertex, index, uniform, and storage buffers
- Image for GPU image allocation with ImageState tracking (including Invalid) and Barrier::Transition for layout changes; FromImageData returns shared_ptr<Image>
- Texture class combining image + sampler + descriptor
- Barrier for centralized image barrier management (ImageState → Vulkan layout/stage/access, maps Invalid → Undefined)
- DescriptorManager with per-frame descriptor pool rotation
- UploadManager for CPU-to-GPU uploads (meshes, lights, environments, IBL cubemaps)
- LightingCache for point/sun light SSBO management (owned by RenderCache)
- Project: pure registration-based serializer (no data ownership) with Serializable base class,
  SceneComponent, ConfigComponent, UIComponent, and HistoryComponent adapter components
- RenderCache (renderer-owned): cross-frame mutable resource pool with lazy attachment creation (`GetAttachment(name, extent)`), per-light shadow map management (`GetShadowMap(lightUID, lightType)` supporting `LightType::POINTLIGHT` cubemap and `LightType::SUNLIGHT` 2D orthographic), a shared layered shadow intensity array (`GetShadowIntensityArray(extent)` with per-light layer indices via `GetShadowIntensityLayer(lightUID, extent)`), cross-frame GPU resources for meshes (`MeshGPU` via `GetMeshGPU()`) and environments (`EnvironmentGPU` via `CreateEnvironmentGPU()`), and LightingCache for light SSBOs (`InitLightingCache()`, `GetLightingCache()`, `UpdateLighting(variantDict)`). The `m_shadowMaps` map stores both `vk::ImageType::eCube` (point) and `vk::ImageType::e2D` (sun) `Image` instances by light UID.

## Future Enhancements

- glTF 2.0 loader with PBR material support
- Texture compression (BCn, ASTC)
- Async asset loading thread pool
- Vulkan Memory Allocator (VMA) integration
- Pipeline cache serialization
