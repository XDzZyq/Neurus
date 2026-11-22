# Render Graph

The deferred pipeline is a document the Editor owns and the renderer uploads onto a runtime DAG. A node is a pass instance, its sockets are the ports its shader declares, and an output socket carries the resource tag the renderer plans its image from. The uploader maps the document onto the GPU graph through a `GraphMorphism`, so a node or edge the renderer cannot reproduce is reported instead of silently dropped.

## The document

`RenderGraphData` is a pooled `UID` resource. Its movable `RenderGraphTopology` base holds the pass graph used for editing and publication. `src/asset/data/render_graph/RenderGraphTopology.hpp` defines this value using `Graph<CpuPort, CpuNode>`.

- a node payload, `CpuNode`, holds the pass kind, a `shared_ptr<const ShaderData>` shader reference and the graph-editor position; `Graph::Node::name` is the document id;
- a socket payload, `CpuPort`, mirrors one shader port (name, direction, GLSL type, binding, location), and an output additionally carries a `GraphOutputTag`;
- an edge is an ordinary `Graph` connection, so there is no string reference between nodes. The saved form keys each edge by node name and port.

`GraphOutputTag` (`src/asset/data/render_graph/GraphOutputTag.hpp`) is the closed set described in [Resources](#resources).

`RenderManager` holds a nullable `shared_ptr<RenderGraphData>` selecting the active graph. Without an authored graph, the effective topology comes from `BuiltInRenderData`. Reading the graph leaves the pointer null. Calling `neurus.render.graph.create()` copies the built-in topology into the pool and selects it. Its nodes initially use built-in shaders.

The pool stores each authored graph's nodes, output tags, edges and selected scene outputs in its `topology` record. `GraphComponent` saves the active UID in `proj_render_graph`, omitting that key when the pointer is null. `FinishLoad()` resolves selected shader UIDs from the restored pool and derives input sockets from the pass contracts. Loading a valid legacy inline graph creates a pooled resource, including when the graph is explicitly empty. See [Project Serialization](../asset/project-serialization.md#optional-config-and-render-graph).

`RenderManager::ResetConfig()` clears the stored config pointer. `ResetGraph()`
clears the stored graph pointer while preserving config. New-scene creation calls
both explicitly. During project loading, present config fields are restored with
per-field defaults; an absent config stays null. `FinishLoad()` restores a valid
saved graph or keeps it null. Read-only queries and rendering never materialize
project defaults. Explicit edits create stored data, and undo can restore absence.

Explicit project shaders remain pooled, and live nodes own references to them. Publication clones topology and retains shared CPU shader ownership. CPU preparation deduplicates selected shaders by UID and posts owned `MakeUploadShaderRequest` payloads before graph compilation. The mapper reads only stable UIDs from the node references. The GPU cache supplies the selected shader modules and their uploaded metadata for compatibility checks. Null shader selections follow built-in recipes through CPU preparation, upload and caching. Rendering never calls back into the Editor or changes which project values are serialized.

## Ports from the shader

`BuiltInRenderData` owns the compiled default shaders by recipe and exposes their pass contracts through `IPortSource`. It uses the same Vulkan-free `ExtractPassPorts` routine as `MakeUploadShaderRequest`. The routine reads parsed `ShaderStruct` metadata to derive the shader's image ports:

- graphics: a fragment stage's `layout(location = N) out` entries become output ports;
- bound `sampler*` uniforms become input ports and bound `image*` uniforms become output ports;
- UBOs, SSBOs, plain uniforms and non-image arrays are not ports;
- vertex attributes and inter-stage varyings are never ports.

`PassKinds` (`src/asset/data/render_graph/PassKinds.hpp`) supplies the outputs without shader bindings, such as G-Buffer depth and the per-light shadow bundle, and the external inputs, currently the two IBL cubemaps. The built-in catalog merges ports across a kind's shader recipes and adds its declared outputs. `ShaderGPU` retains shader ports supplied by the upload for compatibility checks.

## Validation and upload

`ValidateCpuGraph(document, ports)` (`src/asset/data/render_graph/CpuGraphValidation.hpp`) is Vulkan-free and never throws for a bad document. It reports duplicate node ids, unknown pass kinds, an output with no tag or an inconsistent one, a socket the shader no longer declares, an input that is neither connected nor external, and a cycle. Every entry is a `CpuGraphDiagnostic` naming the node and, when it has one, the port.

The renderer uploads the document through `GraphUploader` (`src/render/render_graph/GraphUploader.hpp`). It retains the published topology and maps it directly with a one-way morphism local to each compile. `Compile(cache)` prepares the runtime graph:

1. Shader uploads populate the GPU cache before graph compilation.
2. The uploader validates the document against the injected `IPortSource` and checks that each selected GPU shader preserves the pass's port, descriptor and push-constant contracts. The Editor uses the same port catalog.
3. The morphism policy uses `PassFactory` to obtain a pass instance from the selected shader UID or the built-in recipe. It creates runtime sockets from the validated pass contract and maps document edges by socket name.
4. The morphism checks that the mapping is total and preserves edge endpoints. `RenderGraph::Compile()` caches the execution order and binding plan.

A failure leaves the previous graph drawing and is reported through `LastGraphReport()`. The renderer tracks a compile key made from the published topology hash and shader cache generation. An upload under the same shader UID refreshes the graph even without a new document. The public scene outputs select the live subgraph. Connecting Compose directly to scene color bypasses FXAA. Reconnecting the FXAA output restores it. Both edits retain all seven default nodes and their internal connections.

## The runtime graph

`RenderGraph` is `Graph<PassPort, PassBody>` (`src/render/render_graph/RenderGraph.hpp`):

- `PassFactory` resolves the node's shader UID through `RenderCache`, constructs pass-specific pipelines from GPU modules, with independent descriptor state for each node. Runtime graph/resource references retain resources needed by submitted work.
- `AddPass(pass, ports, externals, id)` materializes one socket per port and keeps the document id in `PassBody::id` for resource keys. An overlay pass, which has no shader ports, keeps its historical `PassIO` sockets and is marked as a legacy node with external inputs.
- `Connect(producer, fromPort, consumer, toPort)` is port to port. The overlay compatibility overload forwards to the same call with `AttachmentNameToString()`.
- `Compile()` rejects an unconnected input unless the node declared it external, then caches the topological order of nodes reachable upstream from the selected outputs.

```mermaid
flowchart LR
    doc["Published RenderGraphTopology<br/>shared CPU shader references"] --> validate["ValidateCpuGraph<br/>named diagnostics"]
    catalog["BuiltInRenderData<br/>IPortSource"] --> validate
    validate --> upload["GraphUploader::Compile<br/>GraphMorphism::Map"]
    catalog --> upload
    cache["RenderCache<br/>uploaded ShaderGPU + metadata"] --> factory["PassFactory<br/>GPU modules → pass pipelines"]
    factory --> upload
    upload --> graph["RenderGraph<br/>PassPort sockets"]
    graph --> plan["BuildImageBindings<br/>image binding plan"]
    plan --> execute["Execute<br/>write descriptors, Record()"]
    execute --> frame["FrameProfile"]
```

The upload also plans the cache: one logical resource per tagged output, keyed by `ResourceKey(node, port)`.

`OverlayEngine` keeps a second `RenderGraph` for the selection outline, copy, debug and gizmo passes. It runs after the scene graph and consumes the selected scene color, depth and object-ID resources. Picking uses the same object-ID selection.

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

The scene maps each selected output to a resource key for downstream consumers. The overlay graph keeps its own attachment names and descriptor layout. See [Editing the render graph](../script/overview.md#editing-the-render-graph) for the socket and shader editing API.

## Descriptors and push constants

A pass no longer hand-writes its set layout. Renderer reflection reads the metadata copied by the shader upload request and merges binding numbers, descriptor kinds, stage masks and array counts into Vulkan bindings, including `PARTIALLY_BOUND` for the light SSBOs. Push-constant ranges derive from the same uploaded metadata. Values stay pass-owned; every current push constant is per-mesh, per-light, per-frame or per-selection. Scene pass constructors consume cached GPU shader modules and own their pipelines; graph mapping never reads mutable CPU shader IR or bytecode.

The graph writes the descriptors. `BuildImageBindings` turns the node's connected input ports and storage-image output ports into `GraphImageBinding`s; `WritePassBindings` resolves each one through `PassResources`, then adds the pass's buffers (`CollectGraphBuffers`) and external images (`CollectGraphExternalImages`). A pass reads its images by port name from `PassFrame::resources`, never by `AttachmentName`.

Two exceptions are explicit:

- `ShadowIntensityPass` allocates one set per shadow-casting light and writes it inside the light loop. The topology sees one output port; `SelfManagedBindings()` keeps the graph away from bindings 0, 1 and 2.
- `GeometryPass` builds per-mesh pipelines from user shaders with their own layouts; the graph still writes its camera UBO.

Barriers remain pass-owned: a pass transitions the resources it is about to use and leaves its outputs in its own write state. Graph-owned barriers are out of scope.

## Ownership boundary

- The Editor owns the project pool and the nullable active graph and config pointers. `RenderManager` posts `CompileRenderGraphRequest{graph}` with a frozen topology or a null request for the built-in.
- Renderer: owns `PassFactory`, the uploader, the runtime `RenderGraph`, the compiled binding plan and every GPU resource. It never calls back into the Editor and never reads the Editor's pool.
- Overlay: still uses `AttachmentName` and `PassIO`; it is not part of the document yet.
- `RenderConfig` is copied into every published frame and supplies per-frame pass parameters; it no longer decides the graph topology, and the graph document never lives in `RenderConfig` or `RenderFrameSnapshot`.

Built-in recipes and CPU preparation stay outside project state. Defaults follow
the same upload/cache/pass path as authored shaders. `UploadManager` constructs
`ShaderGPU`; `RenderCache` owns/indexes it. Reuploading a UID changes its cache
revision and refreshes dependent passes and graphs. Resource references and
frame fences retain old GPU resources until work completes. Scene replacement
invalidates project-specific caches because another file may reuse shader UIDs.

## Tests

- `test/asset/test_render_graph_data.cpp`, `test/core/test_graph_clone.cpp` - the document type and the deep copy undo and publishing rely on.
- `test/core/test_graph_morphism.cpp` - totality, incidence preservation and the mapping policy.
- `test/asset/test_render_graph_document.cpp` - the default document, round trip, input derivation and every named validation failure.
- `test/asset/test_pass_ports.cpp`, `test/asset/test_shader_data.cpp` - ports from real shaders, the pass-kind table and CPU shader data.
- `test/render/test_shader_reflection.cpp` - binding and push-range reflection from every main shader.
- `test/render/test_render_cache_resources.cpp` - tag-planned images, logical resources, legacy aliases.
- `test/render/test_graph_uploader.cpp` - null resolves the cached default, repeated null requests do not invalidate it, explicit empty graphs remain distinct, and unknown passes, invalid selected shaders and stale sockets have named failures.
- `test/render/test_render_graph.cpp`, `test_pass_factory.cpp`, `test_pass_bindings.cpp` - port sockets, factory, binding plan.
- `test/render/test_render_graph_pipeline.cpp` - the first frame without a project document uses the cached built-in seven-pass pipeline; explicit documents remain renderable, edited topology is followed, and a bad upload keeps the last good graph.
- `test/render/test_pass_*` - direct pass tests now use `RecordPass`, which writes the same graph-owned bindings production writes.
- `test/asset/test_graph_component.cpp`, `test/editor/test_editor_render_graph.cpp` - persistence, shared shader ownership, topology publication and upload-before-map ordering.

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
