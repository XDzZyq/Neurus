# Ownership

Vulkan resources are owned by exactly one layer, with the UI layer contributing
only the surface that the renderer consumes.

## Vulkan Ownership Graph

```
UI Layer owns:
  QWindow -> VkSurfaceKHR

Renderer Layer owns:
  VkInstance (shared via QVulkanInstance)
  VkDevice + VkQueue
  VkSwapchainKHR (consumes UI's VkSurfaceKHR)
  VkPipeline + VkPipelineLayout
  VkCommandPool + VkCommandBuffers
  RenderCache (cross-frame mutable resource pool, lazy GetAttachment /
    GetShadowMap / GetShadowIntensityArray)
  All framebuffer attachments (via RenderCache, dynamic rendering)

  Also through src/render/ abstractions:
  VkBuffer + VkDeviceMemory pairs (Buffer hierarchy: GPUBuffer, StagingBuffer,
    UniformBuffer<T>)
  VkImage + VkDeviceMemory + VkImageView triples (Image, Texture)
  VkDescriptorPool + VkDescriptorSet (DescriptorManager)
  Barrier::Transition (centralized layout transitions, ImageState -> Vulkan
    mapping)
  MeshGPU (GPU-side mesh resources: VertexBuffer + IndexBuffer, owned by
    RenderCache)
  EnvironmentGPU (GPU-side IBL resources: diffuse + specular cubemap Textures,
    owned by RenderCache)
  LightingCache (GPU-side light SSBOs: point + sun light, owned by RenderCache)
```

Scene and Asset layers own NO GPU resources. `Mesh::UploadToGPU()` delegates to
`RenderCache::GetMeshGPU()` which creates and owns the `MeshGPU`. `ImageData`
provides CPU-side pixel buffers; GPU upload goes through `Image::FromImageData`.
The `GeometryRenderItem` struct that previously mixed CPU and GPU concerns has
been removed.

## Ownership Rules

- UI Layer owns `VkSurfaceKHR` (created from QWindow + QVulkanInstance)
- Renderer Layer owns everything else (instance via QVulkanInstance sharing,
  device, swapchain, pipeline, command pool, all GPU resources)
- No shared ownership of GPU resources across layers
- Non-copyable GPU resource classes (`= delete` copy/assign)
- Each Vulkan handle has one owning layer

## Architectural Invariants

1. **No cross-layer direct coupling** - Use UIEvents/EventQueue/Context only
2. **Renderer is stateless** - Application state lives in Editor
3. **Full RAII** - No two-phase initialization; no `Init()`/`Terminate()` methods
4. **Explicit GPU ownership** - Each Vulkan handle has one owning layer
5. **Non-copyable GPU resources** - `= delete` copy/assign
6. **Vulkan validation** - Debug builds enable `VK_LAYER_KHRONOS_validation`
