# Render Graph

The deferred pipeline is described by a serialized document and compiled into a runtime DAG. A scene pass declares its ports from its shader; the compiler validates port-to-port edges, plans the logical cache from output tags, and the renderer writes the descriptor bindings the graph owns.

## The document

`RenderGraphSpec` (`src/asset/data/render_graph/RenderGraphSpec.hpp`) is Vulkan-free:

- `GraphNodeSpec` - a node id, a pass kind and a map of output-port tags.
- `GraphEdgeSpec` - `from`/`fromPort` to `to`/`toPort`, name to name.
- `GraphOutputTag` - `format`, `usage`, `extent` and `layers`, the closed set described in [Resources](#resources).

The Editor owns the live document. `project::GraphComponent` stores it under `proj_render_graph`, and a project written before the document existed loads `MakeDefaultRenderGraphSpec()`. The default is today's scene graph: geometry, shadow depth, shadow intensity, SSAO, lighting, compose and FXAA, with the same edges the old hard-coded graph wired.

## Ports from the shader

`ExtractPassPorts` (`src/asset/graph/PassPorts.hpp`) reads the parsed `ShaderStruct` every pass already owns:

- graphics: a fragment stage's `layout(location = N) out` entries become output ports;
- compute: `layout(binding = N) uniform sampler*` becomes an input port, and `uniform image*` becomes an output port;
- UBOs, SSBOs, plain uniforms and non-image arrays are not ports;
- vertex attributes and inter-stage varyings are never ports.

`Pass::DeclaredOutputs()` adds outputs a pass writes without a shader binding: the G-Buffer depth and the per-light shadow bundle. `Pass::ExternalInputPorts()` names inputs the pass resolves outside the graph, which today is only the two IBL cubemaps.

## Compile

`CompileRenderGraphSpec(spec, ports, fxaa)` (`src/asset/graph/RenderGraphCompile.hpp`) is Vulkan-free and never throws for a bad document. It:

1. prunes `FXAAPass` when the FXAA condition is off;
2. checks unique node ids and known pass kinds;
3. checks every edge endpoint, direction, duplicate and second producer;
4. checks that every used output has a tag, every tag names a declared output, and every tag value is in the closed set;
5. checks that every live input is connected or declared external;
6. runs Kahn's algorithm and reports the nodes left on a cycle.

Every failure is a `GraphDiagnostic` naming the node, the port and the reason. The renderer logs all of them, keeps its previous graph and still draws. `PipelineSignature` is the document hash plus the FXAA flag, so a compile happens only when the document or the condition changes.

## The runtime graph

`RenderGraph` is `Graph<ShaderPort, PassEntry>` (`src/render/render_graph/RenderGraph.hpp`):

- `PassFactory` owns one instance of every built-in pass and answers `pass kind -> PassPorts`; `DeferredRenderer::CompileMainGraph` maps the compiled order onto those instances.
- `AddPass(pass, ports, externals, id)` materializes one socket per port. An overlay pass, which has no shader ports, keeps its historical `PassIO` sockets and is marked as a legacy node with external inputs.
- `Connect(producer, fromPort, consumer, toPort)` is port to port. The overlay compatibility overload forwards to the same call with `AttachmentNameToString()`.
- `Compile()` rejects an unconnected input unless the node declared it external, then caches the topological order.

```mermaid
flowchart LR
    doc["RenderGraphSpec<br/>nodes, ports, tags"] --> compile["CompileRenderGraphSpec<br/>validate, prune, sort"]
    compile --> registry["PassFactory<br/>pass kind -> instance"]
    registry --> graph["RenderGraph<br/>ShaderPort sockets"]
    graph --> plan["BuildImageBindings<br/>image binding plan"]
    plan --> execute["Execute<br/>write descriptors, Record()"]
    execute --> frame["FrameProfile"]
```

`OverlayEngine` keeps a second `RenderGraph` for the selection outline, copy, debug and gizmo passes. It runs after the scene graph over the scene tail and is not part of the document in Stage 1.

## Resources

The graph plans one logical resource per used output tag, keyed by `ResourceKey(node, port)`:

| Field | Values |
|---|---|
| `format` | `rgba16f`, `rgba8_srgb`, `rgba8_unorm`, `r8_unorm`, `r32_uint`, `d32_sfloat`, or empty for `logical` |
| `usage` | `color+sampled`, `storage+sampled`, `depth+sampled`, `logical` |
| `extent` | `render` or `shadow` |
| `layers` | `single` or `lights` |

`RenderCache::PlanResource` records the tag; `GetResource(key, extent)` creates the image lazily at the live extent, so a resize does not recompile. `layers: lights` creates a 4-layer array, and `usage: logical` creates nothing: the shadow depth bundle is an ordering token for per-light maps owned by `LightGPU`, not a cache image.

`AttachmentName` remains for the overlay graph and for the existing accessors. The default document's resources are aliased to the legacy names (`geometry/gPosition` to `Position`, `lighting/outputImage` to `HDRColor`, and so on) by `RenderCache::AliasResourceToAttachment`, so the overlay and the tests read the same physical image under both identities. A node id that is not part of the default document gets its own image. This bridge is removed in Stage 3.

## Descriptors and push constants

A pass no longer hand-writes its set layout. `ReflectDescriptorLayout` (`src/render/render_graph/ShaderReflection.hpp`) builds the bindings from the shader IR: binding number, descriptor type, stage flags, array count, and `PARTIALLY_BOUND` for the light SSBOs. `MakeDescriptorLayout` wraps that for the pass constructors, and `PushConstantRangeFromShaders` derives the range size from the declared members. Values stay pass-owned; every current push constant is per-mesh, per-light, per-frame or per-selection.

The graph writes the descriptors. `BuildImageBindings` turns the node's connected input ports and storage-image output ports into `GraphImageBinding`s; `WritePassBindings` resolves each one through `PassResources`, then adds the pass's buffers (`CollectGraphBuffers`) and external images (`CollectGraphExternalImages`). A pass reads its images by port name from `PassFrame::resources`, never by `AttachmentName`.

Two Stage 1 exceptions are explicit:

- `ShadowIntensityPass` allocates one set per shadow-casting light and writes it inside the light loop. The topology sees one output port; `SelfManagedBindings()` keeps the graph away from bindings 0, 1 and 2.
- `GeometryPass` builds per-mesh pipelines from user shaders with their own layouts; the graph still writes its camera UBO.

Barriers remain pass-owned: a pass transitions the resources it is about to use and leaves its outputs in its own write state. Graph-owned barriers are out of scope for Stage 1.

## Ownership boundary

- Editor: owns the document, validates nothing about the GPU, and posts `CompileRenderGraphRequest` when the document or project changes.
- Renderer: owns `PassFactory`, the runtime `RenderGraph`, the compiled binding plan and every GPU resource. It never calls back into the Editor.
- Overlay: still uses `AttachmentName` and `PassIO`; it is not part of the document yet.
- `RenderConfig` is copied into every published frame and still decides the FXAA condition; the graph document never lives in `RenderConfig` or `RenderFrameSnapshot`.

## Tests

- `test/core/test_render_graph_spec.cpp` - default document, round trip, FXAA pruning, hash.
- `test/asset/test_pass_ports.cpp` - ports from the real shaders, declared outputs, conflicting duplicates.
- `test/asset/test_render_graph_compile.cpp` - every named compile failure, external inputs, cycles, deterministic order.
- `test/render/test_shader_reflection.cpp` - binding and push-range reflection from every main shader.
- `test/render/test_render_cache_resources.cpp` - tag-planned images, logical resources, legacy aliases.
- `test/render/test_render_graph.cpp`, `test_pass_factory.cpp`, `test_pass_bindings.cpp` - port sockets, factory, binding plan.
- `test/render/test_render_graph_pipeline.cpp` - the production renderer compiles, draws, changes topology and keeps the last good graph on a bad document.
- `test/render/test_pass_*` - direct pass tests now use `RecordPass`, which writes the same graph-owned bindings production writes.
- `test/asset/test_graph_component.cpp`, `test/editor/test_editor_render_graph.cpp` - persistence and the Editor's publish path.

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
