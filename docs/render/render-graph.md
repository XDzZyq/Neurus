# Render Graph

The deferred pipeline is orchestrated by a compile-once DAG that orders passes by their declared image reads and writes and dispatches them topologically.

## Model

The graph is built on the generic `neurus::Graph<SData, NData>` DAG template (`src/core/Graph.h`).

- `RenderGraph` = `Graph<AttachmentName, PassEntry>`. One node per pass; a node's `PassEntry` holds the `Pass*` and its cached `PassIO`.
- Each pass declares its image I/O via `Pass::GetIO()` returning a `PassIO` (name + read/write `AttachmentBinding`s: resource, binding slot, descriptor type, image layout). This is the single declaration the graph consumes.
- Edges are keyed by `AttachmentName`: `Connect(producer, resource, consumer)` links the producer's output socket for that resource to the consumer's input socket. Fan-out (one write to many reads) is supported.
- A node may declare the same `AttachmentName` as both read and write (e.g. the `DebugPass`/`GizmoPass` target), and a node's own name can never produce a self-edge. `Connect()` matches sockets by `AttachmentName`, so an overlay names the concrete attachment (`p_target`) rather than an alias that shares both images.

## Lifecycle

- `AddPass(pass)` - materializes one socket per declared read/write from `GetIO()`.
- `Connect(...)` - wires producer to consumer; self-loops and duplicate edges are rejected (returns `false`).
- `Compile()` - Kahn topological sort. Inputs with no in-graph producer are **external** (produced by a RenderCache attachment) and are allowed. A cycle throws `std::runtime_error` naming the involved passes.
- `Execute(cmd, cache, ctx)` - invokes each pass's `Record()` in compiled order. Pure dispatch: the graph owns no profiling state. Per-pass CPU/GPU timing and draw/dispatch counters are collected by `DeferredRenderer::recordFrame`, which iterates `CompiledOrder()` itself while profiling is enabled - so `RenderContext` stays an immutable snapshot.
- `Clear()` - drops all nodes so the graph can be rebuilt.

## Config-driven topology

The single source of truth for topology is `RenderConfig`.

- `DeferredRenderer::m_mainGraph` holds the whole pipeline. It is rebuilt (`RebuildMainGraph`) only when the `PipelineSignature` derived from `RenderConfig` changes (currently FXAA on/off), so the DAG always contains exactly the passes that will run - no per-node "enabled" state to drift.
- **Maintenance rule:** a new conditional pass must add a field to `PipelineSignature`. It is the single `RenderConfig` to topology mapping point, and the rebuild check is derived from it.

For the pass-by-pass chain this graph orders, see [Passes](passes.md).

## Resources and descriptors

- `DescriptorBinder` (`render_graph/DescriptorBinder.h`) translates a pass's `PassIO` image bindings into descriptor-set writes.
- Shadow resources are logical `AttachmentName` members resolved by their owning pass, **not** `GetAttachment`: `ShadowDepth` (per-light bundle in `LightGPU`) and `ShadowIntensity` (2D array via `GetShadowIntensityArray`). `ConfigFor` throws for them to catch accidental single-attachment fetches.

## Ownership boundary

- Passes still own their own barriers and descriptor writes inside `Record()`. The graph only orders and dispatches. Moving barrier, descriptor and resource ownership into the graph (and enabling transient aliasing plus custom passes) is tracked in issue #32; parallel execution built on it in issues #33 to #37.

## Tests

`test/render/test_render_graph.cpp` covers socket materialization, connect/duplicate rejection, external inputs, linear/fan-out/diamond ordering, multi-resource edges, cycle detection (with named nodes), rebuild determinism, clear/reuse, and execute preconditions. Pass draw/dispatch reporting via the `PassStats` return value is covered by `PassCountersTest.RecordReturnsStats` (GPU fixture).

## Profiling

Per-frame profiling answers whether the renderer is **CPU-bound** (command recording) or **GPU-bound** before any parallel-rendering work (issue #33).

**Always on**: profiling is unconditional. `DeferredRenderer` owns a `GPUProfiler` and a `FrameProfile` and populates them every frame - there is no enable/disable toggle. `Application::InitRenderer()` needs no profiling call; `DrawFrame()` always returns a live `FrameProfile`.

**What is measured** (`FrameProfile` in `ProfilingData.h`):

- `cpuRecordMs` - total `RenderGraph::Execute()` command-recording wall time.
- Per pass: `cpuMs` (around each `Record()` in `RenderGraph::Execute`), `gpuMs` (timestamp queries), `drawCalls`, `dispatches`.
- Frame totals: `drawCalls`, `dispatches`, `passCount`, `gpuTotalMs`.

**GPU timestamps** (`GPUProfiler`, section-based):

- One `vk::QueryPool` of `kMaxFramesInFlight * (2 + 2 * kMaxPasses)` timestamps; per frame slot: frame begin@0, frame end@1, pass i begin@2+2*i, end@3+2*i.
- The RenderGraph drives the profiler around each pass: `BeginFrame` then (`BeginPass`/`EndPass`)* then `EndFrame`. Passes stay unaware of GPU profiling. `Resolve()` reads back the slot about to be reused (after its fence signals) non-blockingly, so results lag CPU data by 1-2 frames by design.
- The pool is created only when `timestampComputeAndGraphics` and the graphics queue family's `timestampValidBits` allow it; otherwise `Available()==false` and the Profiling panel/log show CPU data only.

**Counters**: each pass reports its own draw/dispatch counts by returning a `PassStats { drawCalls, dispatches }` from `Record()` - passes are stateless with respect to profiling (no members, no reset). `RenderContext` carries no profiling data - it stays an immutable per-frame snapshot. `RenderGraph::Execute` aggregates the returned `PassStats` into each `PassProfile` and sums them into the frame totals.

**Surface**: `DeferredRenderer::DrawFrame()` returns the `FrameProfile`; the Application layer assembles the `UIContext` each frame (the Editor never produces it) and sets `UIContext::profile` to the returned profile (a typed `const FrameProfile*`, forward-declared in `UIContext.h`). The ProfilingPanel reads it directly and renders the per-pass timings as a tree (frame totals + per-pass rows).
