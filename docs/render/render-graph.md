# Render Graph

The deferred pipeline is a document the Editor owns and the renderer uploads onto a runtime DAG. A node is a pass instance, its sockets are the ports its shader declares, and an output socket carries the resource tag the renderer plans its image from. The uploader maps the document onto the GPU graph through a `GraphMorphism`, so a node or edge the renderer cannot reproduce is reported instead of silently dropped.

## The document

`RenderGraphData` (`src/asset/data/RenderGraphData.hpp`) derives from `core::Graph`, which keeps the document the same shape as the runtime graph:

- a node payload, `CpuNode`, holds the pass kind, the pooled shader uid and the graph-editor position; `Graph::Node::name` is the document id;
- a socket payload, `CpuPort`, mirrors one shader port (name, direction, GLSL type, binding, location), and an output additionally carries a `GraphOutputTag`;
- an edge is an ordinary `Graph` connection, so there is no string reference between nodes. The saved form keys each edge by node name and port.

`GraphOutputTag` (`src/asset/data/render_graph/GraphOutputTag.hpp`) is the closed set described in [Resources](#resources).

The Editor owns the live document. `GraphComponent` (`src/asset/components/GraphComponent.hpp`) owns the `proj_render_graph` schema: the flat `RenderGraphRecord`, its node/edge/port records, and `ToRecord`/`FromRecord`, the only crossing between the live graph and the archive. Node fields and output sockets with their tags are saved; input sockets are re-derived from each node's shader on load. A project without the record, or a corrupt node, leaves no pending record and the Editor builds `RenderGraphData::Default()`. The default is today's scene graph - geometry, shadow depth, shadow intensity, SSAO, lighting, compose and FXAA - and, like `CreateDefaultScene`, it is only the built-in fallback.

The Editor pools one shader per built-in source through `PassShaders` (`src/asset/data/render_graph/PassShaders.hpp`). A restored project adopts its own shaders by name instead of duplicating them, and every publish carries a `ShaderSnapshot` per shader the document names, so the renderer never reads the Editor's pool.

## Ports from the shader

`ExtractPassPorts` (`src/asset/data/render_graph/PassPorts.hpp`) reads the parsed `ShaderStruct` every pass already owns:

- graphics: a fragment stage's `layout(location = N) out` entries become output ports;
- compute: `layout(binding = N) uniform sampler*` becomes an input port, and `uniform image*` becomes an output port;
- UBOs, SSBOs, plain uniforms and non-image arrays are not ports;
- vertex attributes and inter-stage varyings are never ports.

`Pass::DeclaredOutputs()` adds outputs a pass writes without a shader binding: the G-Buffer depth and the per-light shadow bundle. `Pass::ExternalInputPorts()` names inputs the pass resolves outside the graph, which today is only the two IBL cubemaps. Both now read the built-in table (`src/asset/data/render_graph/PassKinds.hpp`), so the pass does not repeat them and the Editor can resolve ports without a pass instance.

## Validation and upload

`ValidateCpuGraph(document, ports)` (`src/asset/data/render_graph/CpuGraphValidation.hpp`) is Vulkan-free and never throws for a bad document. It reports duplicate node ids, unknown pass kinds, an output with no tag or an inconsistent one, a socket the shader no longer declares, an input that is neither connected nor external, and a cycle. Every entry is a `CpuGraphDiagnostic` naming the node and, when it has one, the port.

The renderer uploads the document through `GraphUploader` (`src/render/render_graph/GraphUploader.hpp`), a service that owns the document, its snapshots and the permanent morphism; `Compile(cache)` is the whole upload:

1. the document is validated against the factory's port tables, so the renderer and the Editor agree on what a valid edge is;
2. every node whose shader uid names no snapshot is rejected by name;
3. each node becomes a pass instance from `PassFactory`, with sockets materialized from the pass's ports;
4. the morphism (`GraphMorphism::Map`) builds the runtime nodes and connections and records every image;
5. the morphism must be total and incidence-preserving, then `Compile()` caches the order and the binding plan.

A failure leaves the previous graph drawing and is reported through `LastGraphReport()`. `PipelineSignature` is the document hash plus the shader-snapshot versions, so a rebuild happens only when the document or a shader changed. FXAA is a normal node in the document and always runs, so a render-config change never rebuilds the graph.

## The runtime graph

`RenderGraph` is `Graph<PassPort, PassBody>` (`src/render/render_graph/RenderGraph.hpp`):

- `PassFactory` owns one instance of every built-in pass and answers `pass kind -> PassPorts`; `GraphUploader` maps the document's nodes onto those instances.
- `AddPass(pass, ports, externals, id)` materializes one socket per port and keeps the document id in `PassBody::id` for resource keys. An overlay pass, which has no shader ports, keeps its historical `PassIO` sockets and is marked as a legacy node with external inputs.
- `Connect(producer, fromPort, consumer, toPort)` is port to port. The overlay compatibility overload forwards to the same call with `AttachmentNameToString()`.
- `Compile()` rejects an unconnected input unless the node declared it external, then caches the topological order.

```mermaid
flowchart LR
    doc["RenderGraphData<br/>nodes, ports, tags"] --> validate["ValidateCpuGraph<br/>named diagnostics"]
    validate --> upload["GraphUploader::Compile<br/>GraphMorphism::Map"]
    factory["PassFactory<br/>pass kind -> instance"] --> upload
    snapshots["ShaderSnapshot<br/>per shader"] --> upload
    upload --> graph["RenderGraph<br/>PassPort sockets"]
    graph --> plan["BuildImageBindings<br/>image binding plan"]
    plan --> execute["Execute<br/>write descriptors, Record()"]
    execute --> frame["FrameProfile"]
```

The upload also plans the cache: one logical resource per tagged output, keyed by `ResourceKey(node, port)`.

`OverlayEngine` keeps a second `RenderGraph` for the selection outline, copy, debug and gizmo passes. It runs after the scene graph over the scene tail and is not part of the document yet.

## Resources

The graph plans one logical resource per used output tag, keyed by `ResourceKey(node, port)`:

| Field | Values |
|---|---|
| `format` | `none` (`logical`), `rgba16f`, `rgba8_srgb`, `rgba8_unorm`, `r8_unorm`, `r32_uint`, `d32_sfloat` |
| `usage` | `color_sampled`, `storage_sampled`, `depth_sampled`, `logical` |
| `extent` | `render` or `shadow` |
| `layers` | `single` or `lights` |

The four fields are closed sets, so they are enums (`GraphFormat`, `GraphUsage`, `GraphExtent`, `GraphLayers` in `src/asset/data/render_graph/GraphOutputTag.hpp`). `ResourceTag.cpp` is the only place that turns them into Vulkan values.

`RenderCache::PlanResource` records the tag; `GetResource(key, extent)` creates the image lazily at the live extent, so a resize does not recompile. `layers: lights` creates a 4-layer array, and `usage: logical` creates nothing: the shadow depth bundle is an ordering token for per-light maps owned by `LightGPU`, not a cache image.

`AttachmentName` remains for the overlay graph and for the existing accessors. The document's resources are aliased to the legacy names (`geometry/gPosition` to `Position`, `lighting/outputImage` to `HDRColor`, and so on) by `RenderCache::AliasResourceToAttachment`, so the overlay and the tests read the same physical image under both identities. A node id outside that map gets its own image. The bridge goes away when the overlay graph moves to ports.

## Descriptors and push constants

A pass no longer hand-writes its set layout. CPU reflection in `shader/ShaderSnapshotBuilder.cpp` produces binding numbers, descriptor kinds, stage masks, array counts and optional-binding flags. `ReflectDescriptorLayout` (`src/render/render_graph/ShaderReflection.hpp`) translates and merges that metadata into Vulkan bindings, including `PARTIALLY_BOUND` for the light SSBOs. `MakeDescriptorLayout` wraps that for the current pass constructors, and `PushConstantRangeFromShaders` derives the range size from the same snapshot metadata. Values stay pass-owned; every current push constant is per-mesh, per-light, per-frame or per-selection. Pass construction from the Editor-published snapshot SPIR-V is still future work; current passes load their built-in shaders through the CPU Shader module.

The graph writes the descriptors. `BuildImageBindings` turns the node's connected input ports and storage-image output ports into `GraphImageBinding`s; `WritePassBindings` resolves each one through `PassResources`, then adds the pass's buffers (`CollectGraphBuffers`) and external images (`CollectGraphExternalImages`). A pass reads its images by port name from `PassFrame::resources`, never by `AttachmentName`.

Two exceptions are explicit:

- `ShadowIntensityPass` allocates one set per shadow-casting light and writes it inside the light loop. The topology sees one output port; `SelfManagedBindings()` keeps the graph away from bindings 0, 1 and 2.
- `GeometryPass` builds per-mesh pipelines from user shaders with their own layouts; the graph still writes its camera UBO.

Barriers remain pass-owned: a pass transitions the resources it is about to use and leaves its outputs in its own write state. Graph-owned barriers are out of scope.

## Ownership boundary

- Editor: owns `RenderManager` (the document, the render config, the pooled pass shaders and their snapshots) and posts `CompileRenderGraphRequest{graph, shaders}` when the document or project changes. It never touches a GPU object.
- Renderer: owns `PassFactory`, the uploader and its morphism, the runtime `RenderGraph`, the compiled binding plan and every GPU resource. It never calls back into the Editor and never reads the Editor's pool.
- Overlay: still uses `AttachmentName` and `PassIO`; it is not part of the document yet.
- `RenderConfig` is copied into every published frame and supplies per-frame pass parameters; it no longer decides the graph topology, and the graph document never lives in `RenderConfig` or `RenderFrameSnapshot`.

## Tests

- `test/asset/test_render_graph_data.cpp`, `test/core/test_graph_clone.cpp` - the document type and the deep copy undo and publishing rely on.
- `test/core/test_graph_morphism.cpp` - totality, incidence preservation and the mapping policy.
- `test/asset/test_render_graph_document.cpp` - the default document, round trip, input derivation and every named validation failure.
- `test/asset/test_pass_ports.cpp`, `test/asset/test_shader_snapshot.cpp` - ports from the real shaders, the pass-kind table, and the snapshot boundary.
- `test/render/test_shader_reflection.cpp` - binding and push-range reflection from every main shader.
- `test/render/test_render_cache_resources.cpp` - tag-planned images, logical resources, legacy aliases.
- `test/render/test_graph_uploader.cpp` - a total upload, node and order agreement, and a named failure for an unknown pass, a missing snapshot and a stale socket.
- `test/render/test_render_graph.cpp`, `test_pass_factory.cpp`, `test_pass_bindings.cpp` - port sockets, factory, binding plan.
- `test/render/test_render_graph_pipeline.cpp` - the production renderer draws before a document arrives, compiles it, follows an edited topology, and keeps the last good graph on a bad document.
- `test/render/test_pass_*` - direct pass tests now use `RecordPass`, which writes the same graph-owned bindings production writes.
- `test/asset/test_graph_component.cpp`, `test/editor/test_editor_render_graph.cpp` - persistence and the Editor's publish path, snapshots included.

## Profiling

Per-frame profiling answers whether the renderer is **CPU-bound** (command recording) or **GPU-bound** before any parallel-rendering work (issue #33).

**Always on**: profiling is unconditional. `DeferredRenderer` owns a `GPUProfiler` and a `FrameProfile` and populates them every frame - there is no enable/disable toggle. `Application::InitRenderer()` needs no profiling call; `DrawFrame()` always returns a live `FrameProfile`.

**What is measured** (`FrameProfile` in `ProfilingData.hpp`):

- `cpuRecordMs` - total command-recording wall time across both graphs (scene, then overlay).
- Per pass: `cpuMs` (around each `Record()` in `RenderGraph::Execute`), `gpuMs` (timestamp queries), `drawCalls`, `dispatches`.
- Frame totals: `drawCalls`, `dispatches`, `passCount`, `gpuTotalMs`.

**GPU timestamps** (`GPUProfiler`, section-based):

- One `vk::QueryPool` of `kMaxFramesInFlight * (2 + 2 * kMaxPasses)` timestamps; per frame slot: frame begin@0, frame end@1, pass i begin@2+2*i, end@3+2*i.
- The RenderGraph drives the profiler around each pass: `BeginFrame` then (`BeginPass`/`EndPass`)* then `EndFrame`. Passes stay unaware of GPU profiling. One `BeginFrame`/`EndFrame` pair covers both graphs, so overlay sections follow scene sections in query order and in the profile's pass list; `Resolve()` reads back the slot about to be reused (after its fence signals) non-blockingly, so results lag CPU data by 1-2 frames by design.
- The pool is created only when `timestampComputeAndGraphics` and the graphics queue family's `timestampValidBits` allow it; otherwise `Available()==false` and the Profiling panel/log show CPU data only.

**Counters**: each pass reports its own draw/dispatch counts by returning a `PassStats { drawCalls, dispatches }` from `Record()` - passes are stateless with respect to profiling (no members, no reset). `RenderContext` carries no profiling data - it stays an immutable per-frame snapshot. `RenderGraph::Execute` aggregates the returned `PassStats` into each `PassProfile` and sums them into the frame totals.

**Surface**: `DeferredRenderer::DrawFrame()` returns the `FrameProfile`; the Application layer assembles the `UIContext` each frame (the Editor never produces it) and sets `UIContext::profile` to the returned profile (a typed `const FrameProfile*`, forward-declared in `UIContext.hpp`). The ProfilingPanel reads it directly and renders the per-pass timings as a tree (frame totals + per-pass rows).
