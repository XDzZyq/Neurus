# Architecture constraints

## Layer isolation and ownership

Application wires the concrete components. UI, Script and Renderer depend on
the contracts below. Only Editor and its controllers mutate the scene.

- **Application** (`src/app/`): composition root; owns the Qt app, Preferences,
  `VulkanContext` (instance, device, queues) and the `VkSurfaceKHR`; wires UI,
  Editor, Renderer, Script and the command `SessionServer`.
- **Renderer** (`src/render/`): pure rendering service; borrows the surface,
  device and queues and owns the swapchain and every GPU resource below it;
  consumes read-only scene data; must never mutate application state; must never
  include Editor or UI headers.
- **Editor** (`src/editor/`): application logic and scene mutation; owns the
  Scene, `RenderConfig`, Controllers and the `ResourceManager`; communicates via
  Context, UIEvents, its internal typed EventQueue and posted `RenderRequest`s.
- **UI** (`src/ui/`): Qt6 Widgets presentation only; owns the native window and
  exposes its handle (no Vulkan objects); emits signals; must never include
  Renderer headers.
- **Script** (`src/script/`): embedded CPython `neurus` module; reads the scene
  through a read-only `ScriptContext` and mutates only by emitting typed editor
  events through `ScriptDispatch`; never calls Editor methods, never reaches
  project save or snapshot capture, never includes Renderer or UI headers.
- **Asset** (`src/asset/`): Vulkan-free CPU-side asset loading (`MeshData`,
  `ImageData`, `PixelFormat`).
- **Scene** (`src/scene/`): Vulkan-free logical objects (Camera, Light, Mesh,
  Transform, overlays).

## Stateful systems

- Do not use singletons for stateful systems. The existing logging system
  (`LogBuffer::instance()`) is the sole exception.
- Give state an explicit owner and pass the relevant instance, context or
  interface to its consumers. This applies to script contexts, resource pools,
  caches and loading contexts. Do not use mutable globals or thread-local
  state to supply an implicit current instance.
- For resource loading, the
  [resource input archive](../../src/asset/ResourceInputArchive.hpp) receives an
  `IResourceLookup` reference from the pooled loader.
  Each nested loader creates its own archive for the same pool. Serializable
  types declare their `ResourceRef` fields and the archive supplies the lookup.

## Communication

| Intent | Required interface | Who handles it |
| --- | --- | --- |
| UI edits the document | Panel or `UIEvents` signal | Application connects it to `Editor::OnUIEvent`. Editor drains its queue during `Edit()`. |
| Script reads or edits the document | Read through `ScriptContext`. Emit writes via `ScriptDispatch` and `IEventQueue`. | The Editor handler applies the event synchronously. |
| UI displays document state | Application assembles a read-only `UIContext`. | Panels refresh from that view. |
| Renderer reads a frame | `Editor::BuildSnapshot` produces an owned `RenderFrameSnapshot`. Application publishes it to `RenderThread`. | Render passes read that snapshot through `RenderContext::Frame()`. |
| Editor requests GPU work | Post an owned, typed `RenderRequest` through `RenderRequestSink`. | The render thread drains requests in posting order with the published frame. |
| Window resize, surface replacement, picking or screenshot | Application posts the corresponding `RenderThread` control. | The render thread performs it. Application translates a completed pick into an editor event. |
| CLI requests a scene edit | IPC queues the command in `CommandDispatcher`. | Application drains on the main thread. The editor adapter emits typed edits. |

The concrete `EventQueue` stays inside Editor. Controllers react to events
and dispatch follow-up events synchronously. Renderer must never call back
into Editor or retain a live Editor scene for later reads.

UI and Script may include Editor's public input and typed event contracts.
UI also consumes the read-only `HistoryView`; Script uses `IEventQueue` for
dispatch. Concrete headers such as `Editor.hpp`, `EventBus.hpp` and controller
implementations stay behind that boundary. Application may include the concrete
components it wires. Component libraries must never include Application headers.
When exposing a new Editor contract, add its header for the intended consumer
in `scripts/check_layer_boundaries.py` and cover it in the checker tests.
See [layer responsibilities](../../docs/architecture/layers.md) and
[runtime data flow](../../docs/architecture/data-flow.md).

## Controllers and undo

- A controller handles a typed event, makes the live CPU edit, and submits a
  reversible description through `ControllerContext::ops` (`IOperationSink`).
  Only the operation system may call `Operation::Apply`. Only
  `OperationManager` constructs the production `OperationContext` for undo,
  redo and rollback. Controllers must never acquire a replay context or drive
  replay themselves.
- Prefer one event per edit intent and one operation per reversible change.
  Record the target UID and the smallest set of values needed to restore it.
  For example, moving an object records its old and new position. Group
  related edits into a composite for one undo step. Avoid capturing the
  entire scene, config or graph to undo a change to one field.
- Keep controllers stateless wherever possible. Resolve current objects through
  `ControllerContext` in the handler. Keep temporary state only for the span of
  an interaction that needs it. Keep headers limited to declarations and their
  required types. Forward-declare pointer/reference types and put handler
  bodies, concrete operation includes and resource dependencies in the `.cpp`.
  See [controllers](../../docs/editor/controllers.md#controller-design-constraints)
  and [operations](../../docs/editor/operations.md#overview).

## GPU uploads

Before implementing an upload, trace the current Mesh or RenderGraph path from
CPU preparation through the typed request to its renderer consumer. Use Mesh
for individual resources and RenderGraph when publication depends on other
uploads. Follow the reference path's payload lifetime, FIFO ordering and GPU
cache ownership. See the [two upload paths](../../docs/render/resources.md#upload-reference-paths).

## Vulkan ownership

- Application owns the instance, device and queues (`src/app/VulkanContext`, no
  `QVulkanInstance`) and the `VkSurfaceKHR`, created with
  `platform::CreateSurface` from the viewport's native window handle.
- Renderer borrows the surface, device and queues and owns everything below
  them: swapchain, pipelines, command pools and all GPU resources.
- UI owns no Vulkan objects; it only exposes the native window handle.
- No shared ownership of GPU resources across layers. GPU-resource classes
  `= delete` copy/assign and use `vk::raii` - never a raw `vkDestroy*`.

## Command sessions

- **Command/IPC/CLI targets stay Qt-free and Vulkan-free.** `neurus_command`,
  `neurus_ipc` and `neurus_cli` must not include Qt, Vulkan, Editor or UI
  headers. The IPC I/O thread never touches editor state; only the
  main-thread drain in `Application::NewFrameSignals` may execute a command.
  The CLI never constructs an `Editor`, and the session registry under
  `~/.neurus/sessions` is per-user and owner-only. See
  [docs/command/overview.md](../../docs/command/overview.md).
