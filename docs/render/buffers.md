# Buffers

The renderer has a small buffer class hierarchy that maps a memory-locality choice (host-visible vs device-local) onto a typed wrapper.

## Buffer classes

- `Buffer.h/cpp` - virtual base class (`Buffer`).
- `StagingBuffer.h/cpp` - host-visible staging (upload/download ONLY).
- `CPUBuffer.h/cpp` - permanent host-visible mapped buffer.
- `GPUBuffer.h/cpp` - device-local with staging.
- `ArrayBuffer.h` - growable typed device buffer (`ArrayBuffer<T>`).
- `UniformBuffer.h` - template uniform (`UniformBuffer<T>`).
- `VertexBuffer.h/cpp` - vertex buffer (inherits `GPUBuffer`).
- `IndexBuffer.h/cpp` - index buffer (inherits `GPUBuffer`).
- `BufferLayout.h/cpp` - vertex input layout description.

## Choosing a buffer type

`CPUBuffer` (`src/render/buffers/CPUBuffer.h`) is the **permanent** host-visible, device-accessible buffer: mapped once at creation and written directly by the CPU. It is named for the mirror image of `GPUBuffer`, which is device-local.

- Use it for data the CPU rewrites and the GPU reads in place - `DebugCache`'s segment/point SSBOs are the reference case.
- **`StagingBuffer` is for uploads and downloads only.** Do not retain one as a permanent buffer object; that is what `CPUBuffer` exists for.
- Use `GPUBuffer` (device-local + staging) for data that is written rarely and read hot, e.g. mesh vertex/index buffers. Do **not** reach for it just because a write looks infrequent: `GPUBuffer::Unmap()` and `ArrayBuffer::Resize()` each submit a copy and then wait for that submit to complete. Debug geometry is re-flattened on every `RenderResetEvent`, which the camera controller enqueues on every camera change, so that choice would put a full GPU stall in every frame of a viewport drag. A host-visible memcpy of a few hundred KB costs nothing by comparison.
- `StagingBuffer` is transfer-only by contract. Using one as a permanent buffer would put a copy plus a submit plus a wait on that submit in every write, which is too costly for data the CPU rewrites each frame - that is exactly why `CPUBuffer` exists.
- **Growth replaces the buffer.** Growing a `CPUBuffer` constructs a new buffer, which invalidates every descriptor that names the old one, so a replaced buffer may only be swapped out once its frame is known complete.

For the resources these buffers back, see [Resources](resources.md).
