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

```
+--------------------------------------------------------------------+
| UI Layer                                                src/ui/    |
|  owns: main window (ADS), docks, panels, native window handle      |
|  emits: Qt signals through UIEvents                                |
|  no Vulkan object, no scene state                                  |
+---------------------------------+----------------------------------+
                                  |
                                  | UIEvents (Qt signals / slots)
                                  v
+--------------------------------------------------------------------+
| Editor Layer                                         src/editor/   |
|  owns: Scene, RenderConfig, ResourceManager, Controllers,          |
|        EventQueue (internal), OperationManager (undo/redo)         |
|  builds: RenderFrameSnapshot (renderer view), UIContext (UI view)  |
|  posts: RenderRequest values through RenderRequestSink             |
+---------------------------------+----------------------------------+
                                  |
                                  | RenderFrameSnapshot + RenderRequest
                                  v
+--------------------------------------------------------------------+
| Renderer Layer                                       src/render/   |
|  owns: VkSwapchainKHR and image views, RenderCache and every       |
|        attachment, pipelines, command pools, OverlayEngine         |
|  borrows: VkSurfaceKHR, VkDevice, VkQueue (all owned by the app)   |
|  drives: RenderThread (frame mailbox, control queue); acquires and |
|          presents via Swapchain; blits OverlayOutput to it         |
+--------------------------------------------------------------------+

+--------------------------------------------------------------------+
| Script Layer                                         src/script/   |
|  owns: embedded CPython, the `neurus` module, bindings, proxies,   |
|        Script Console buffer                                       |
|  reads: ScriptContext (Scene, RenderConfig, resource lookup)       |
|  mutates: typed editor events only, ScriptDispatch -> EventQueue   |
|  no project save, no snapshot, no Renderer or UI headers           |
+--------------------------------------------------------------------+

+--------------------------------------------------------------------+
| Application Layer                                       src/app/   |
|  owns: QApplication, Preferences, VulkanContext (VkInstance,       |
|        VkDevice, VkQueue), VkSurfaceKHR, UploadManager,            |
|        RenderThread, DeferredRenderer, ScriptEngine, SessionServer |
|  builds every layer and wires the channels between them            |
+--------------------------------------------------------------------+

+--------------------------------+  +--------------------------------+
| Asset Layer        src/asset/  |  | Scene Layer        src/scene/  |
| Vulkan-free CPU data:          |  | Vulkan-free objects:           |
| MeshData, ImageData,           |  | Camera, Light, Mesh,           |
| PixelFormat                    |  | Transform, UID                 |
+--------------------------------+  +--------------------------------+

+--------------------------+        +--------------------------------+
| CLI process (separate)   |        | host application               |
| `neurus` CLI, src/cli/   |  --->  | SessionServer                  |
| parses arguments,        |  JSON  | src/app/session/: token,       |
| attaches to a named      |  --->  | registry, loopback listener    |
| session, formats         |  <---  | src/ipc/                       |
| results                  | result | Asio I/O thread accepts,       |
+--------------------------+        | reads and writes only          |
                                    | main-thread drain ->           |
                                    | EditorCommandAdapter           |
                                    | (src/editor/commands/) emits   |
                                    | typed editor events            |
                                    | src/command/: Qt-free and      |
                                    | Vulkan-free request/result     |
                                    | values, codec, descriptors     |
                                    +--------------------------------+
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

Two composition roots exist and share one service. `ApplicationCore`
(`src/app/ApplicationCore.h`) is Qt-free and owns the preferences file, the
script engine and the open project; it exposes document I/O, the `script.*`
commands and the sandbox project root. The Qt application owns one and adds the
window, UI, surface and Qt signal wiring; the headless application owns one and
adds the offscreen target and its own loop. The dependency is one-way - the core
knows nothing of Qt, the UI or Vulkan - which is what lets `neurus_headless`
link no Qt at all while the CLI sees identical behaviour on every platform.

### Generated Structure Graph

<!-- The link below is raw HTML on purpose. graphify.html is generated build
     output and gitignored, so a markdown link would be reported by MkDocs link
     validation and fail `mkdocs build --strict` on any build where the graph
     has not been generated (a plain `mkdocs serve`, or a CI run where the
     non-fatal graphify step failed). Raw hrefs are passed through unvalidated
     AND unrewritten, so this path is URL-relative to /architecture/overview/,
     not source-relative like a markdown link would be. -->

The diagram above is hand-maintained. For a generated counterpart, the
<a href="../../graphify.html" target="_blank">structure graph</a> maps the
classes and functions in `src/` and the calls, inheritance and containment
between them, grouped into communities that track the layer boundaries above. It
is built from tree-sitter ASTs by
[graphify](https://github.com/Graphify-Labs/graphify) during the documentation
build; regenerate it with `make doc`. Shaders do not appear, and the page needs
network access to load its layout engine.
