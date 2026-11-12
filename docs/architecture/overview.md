# Architecture Overview

Neurus is a C++20 Vulkan-HPP 1.4 real-time renderer designed for experimentation
with modern rendering algorithms, organized into strictly isolated layers.

## System Design Philosophy

The architecture prioritizes:

- **Strict layer isolation** - UI, Editor, Renderer, Script and Command
  boundaries must not be violated
- **Minimal global state** - State is explicit and localized
- **Explicit data flow** - Communication via UIEvents (Qt Signals/Slots),
  typed `RenderRequest` values (Editor to Renderer), Context objects, and typed
  editor events (Script to Editor); the `EventQueue` typed dispatcher stays
  inside the Editor
- **Stateless rendering** - Renderer does not own application-level state
- **Deterministic GPU resource management** - Vulkan resources have explicit
  RAII ownership via the `vk::raii` namespace

## Layer Diagram

```mermaid
flowchart TD
    ui["UI Layer - src/ui/<br/>owns: main window (ADS), docks, panels, native window handle<br/>emits: Qt signals through UIEvents<br/>no Vulkan object, no scene state"]
    editor["Editor Layer - src/editor/<br/>owns: Scene, RenderConfig, ResourceManager, Controllers,<br/>EventQueue (internal), OperationManager (undo/redo)<br/>builds: RenderFrameSnapshot (renderer view), UIContext (UI view)<br/>posts: RenderRequest values through RenderRequestSink"]
    renderer["Renderer Layer - src/render/<br/>owns: VkSwapchainKHR and image views, RenderCache and every<br/>attachment, pipelines, command pools, OverlayEngine<br/>borrows: VkSurfaceKHR, VkDevice, VkQueue (all owned by the app)<br/>drives: RenderThread (frame mailbox, control queue); acquires and<br/>presents via Swapchain; blits OverlayOutput to it"]

    ui -->|"UIEvents (Qt signals / slots)"| editor
    editor -->|"RenderFrameSnapshot + RenderRequest"| renderer
```

The remaining layers sit outside that chain:

```mermaid
flowchart TB
    subgraph standalone["process-level layers"]
        direction LR
        script["Script Layer - src/script/<br/>owns: embedded CPython, the neurus module, bindings, proxies,<br/>Script Console buffer<br/>reads: ScriptContext (Scene, RenderConfig, resource lookup)<br/>mutates: typed editor events only, ScriptDispatch -> EventQueue<br/>no project save, no snapshot, no Renderer or UI headers"]
        application["Application Layer - src/app/<br/>owns: QApplication, Preferences, VulkanContext (VkInstance,<br/>VkDevice, VkQueue), VkSurfaceKHR, UploadManager,<br/>RenderThread, DeferredRenderer, ScriptEngine, SessionServer<br/>builds every layer and wires the channels between them"]
    end
    subgraph shared["shared, Vulkan-free libraries"]
        direction LR
        asset["Asset Layer - src/asset/<br/>CPU data: MeshData, ImageData, PixelFormat"]
        scene["Scene Layer - src/scene/<br/>objects: Camera, Light, Mesh, Transform, UID"]
        core["Core Layer - src/core/<br/>UID, ResourceManager, RenderConfig, Log, Graph"]
    end
```

### The event bus is the pivot

Every editor mutation crosses one node, whichever layer starts it:

```mermaid
flowchart LR
    ui["UI panels"] --> uiEvents["UIEvents<br/>Qt signals"]
    uiEvents --> onUIEvent["Editor::OnUIEvent"]
    onUIEvent -->|"Enqueue"| bus["EventBus (EventQueue)<br/>owned by the Editor"]
    bus -->|"Process once per Edit()"| controllers["Controllers<br/>owned by the Editor"]
    controllers -->|"Emit (synchronous)"| bus
    script["Script Layer<br/>ScriptDispatch"] -->|"Emit (synchronous)"| bus
    controllers -->|"record"| ops["OperationManager<br/>undo / redo"]
    ops -->|"replay the inverse event"| bus
    controllers -->|"mutate"| scene["Scene and ResourceManager<br/>owned by the Editor"]
    onUIEvent -->|"RenderRequest + frame snapshot"| renderer["Renderer"]
```

The bus carries four paths, and they differ only in who starts the walk:

- **UI intent**: `UIEvents` -> `Editor::OnUIEvent` -> `Enqueue` -> `Process` -> controllers.
- **Script write**: `ScriptDispatch::Emit` -> controllers, synchronously.
- **Undo / redo**: a controller records an `Operation`; `Apply()` re-emits the
  original event through the bus, so replay runs the same handler a live edit
  runs.
- **GPU work**: controllers mutate the Scene, and the Editor posts
  `RenderRequest`s and publishes the frame snapshot to the Renderer - the
  Renderer never sees an event.

A command arrives from a separate process:

```mermaid
flowchart LR
    cli["CLI process (separate)<br/>neurus CLI, src/cli/<br/>parses arguments, attaches to a named session, formats results"]
    host["Host application<br/>SessionServer - src/app/session/: token, registry, loopback listener<br/>src/ipc/: Asio I/O thread accepts, reads and writes only<br/>main-thread drain -> EditorCommandAdapter (src/editor/commands/)<br/>emits typed editor events<br/>src/command/: Qt-free and Vulkan-free request/result values,<br/>codec, descriptors"]

    cli -->|"JSON request"| host
    host -->|"result"| cli
```

Scene objects (`src/scene/`) and Core utilities (`src/core/`) are shared across
layers.

The Application layer is the composition root. It creates the window, the
Vulkan context and surface, the editor, the renderer and its thread, the script
engine and the session server. One tick drains session commands first, then runs
queued scripts, then applies a finished pick, and only then calls `Editor::Edit()`,
builds the frame snapshot, publishes it and refreshes the UI from `UIContext`.
Commands and scripts therefore see their mutations applied in the tick that
issued them.

### Generated Structure Graph

<!-- The link below is raw HTML on purpose. graphify.html is generated build
     output and gitignored, so a markdown link would be reported by MkDocs link
     validation and fail `mkdocs build --strict` on any build where the graph
     has not been generated (a plain `mkdocs serve`, or a CI run where the
     non-fatal graphify step failed). Raw hrefs are passed through unvalidated
     AND unrewritten, so this path is URL-relative to /architecture/overview/,
     not source-relative like a markdown link would be. -->

The diagrams above are hand-maintained. For a generated counterpart, the
<a href="../../graphify.html" target="_blank">structure graph</a> maps the
classes and functions in `src/` and the calls, inheritance and containment
between them, grouped into communities that track the layer boundaries above. It
is built from tree-sitter ASTs by
[graphify](https://github.com/Graphify-Labs/graphify) during the documentation
build; regenerate it with `make doc`. Shaders do not appear, and the page needs
network access to load its layout engine.
