# Project Serialization

Project persistence is a pure registration-based serializer plus a fixed set of components, one per concern, that the Application registers in a load-bearing order.

## Project

`src/asset/Project.hpp` defines `project::Project`:

- It owns no data - no project path, no dirty flag, no concrete type knowledge.
- Callers register `Serializable` components via `Register<T>(args...)`, which constructs each component and stores it as `unique_ptr<Serializable>`, returning a reference.
- `Save(path)` writes registered components through cereal under the `"project"` node. Optional components omit their keys when their stored pointers are null.
- `Load(path)` parses the project object with `nlohmann::ordered_json`, then gives each registered component an isolated one-key JSON object through `LoadJson`. Missing keys receive an empty object. A failed component cannot move another component's archive cursor.
- `Clear()` destroys all components.
- It depends on cereal, nlohmann JSON and `Serializable.hpp`; it does not depend on concrete component types.

The Application owns the project path and builds a transient `Project` per save/open, registering the components below. The generic helper machinery lives in [Serialization](../core/serialization.md).

## Serializable

`src/asset/Serializable.hpp` defines the component interface:

- `Key()` returns the archive node name.
- `Save(cereal::JSONOutputArchive&)` writes the component's live stored data.
- `Load(cereal::JSONInputArchive&)` reads an isolated cereal object.
- `LoadJson(const std::string&)` is the project-load entry point. Its default implementation opens an isolated cereal archive and calls `Load`; components may override it to validate JSON types or recover individual entries before cereal reads them.

Components retain their own fallback policy. Invalid whole-project JSON or an invalid `"project"` root fails project loading; component recovery applies after that root has been parsed.

## Components and registration order

Registration order determines component processing order. The resource pool must load before scene references; graph records are applied by the Editor after the restored pool is available.

```mermaid
flowchart LR
    resources["1 - ResourceComponent<br/>the UID pool"]
    scene["2 - SceneComponent<br/>the Scene"]
    config["3 - ConfigComponent<br/>optional config"]
    graph["4 - GraphComponent<br/>optional active graph UID"]
    ui["5 - UIComponent<br/>layout blob"]
    history["6 - HistoryComponent<br/>undo/redo stacks"]
    editor["7 - EditorComponent<br/>viewport camera UID"]

    resources -->|"the pool must be restored first"| scene --> config --> graph --> ui --> history --> editor
```

| Order | Component | Key | Wraps |
| :-: |---|---|---|
| 1 | `ResourceComponent` | `m_resources` | The `ResourceManager` UID pool |
| 2 | `SceneComponent` | `m_scene` | The `Scene` |
| 3 | `ConfigComponent` | `proj_config` | Nullable `shared_ptr<RenderConfig>` |
| 4 | `GraphComponent` | `proj_render_graph` | Active graph UID in the resource pool |
| 5 | `UIComponent` | `ui` | An opaque UI-state string |
| 6 | `HistoryComponent` | `m_history` | The undo/redo stacks |
| 7 | `EditorComponent` | `m_editor` | The viewport camera's pool UID |

`ResourceComponent` is registered **first** so every pooled UID is indexed before the scene or active graph requests dependencies. `HistoryComponent` and `EditorComponent` are registered last, in that order, so legacy files without their nodes load cleanly: `HistoryComponent` clears the stacks instead of throwing, and `EditorComponent` leaves its uid at `0`, which makes the finish-load step mint a fresh viewport camera.

Components borrow their owners' data or shared-pointer slots; the serializer does not own application state. Application composition force-links concrete shader registrations. The Editor-owned history and viewport adapters live under `src/editor/project/`, keeping Asset independent of Editor implementation.

## Scene persistence

`SceneComponent` reads membership, selection and camera IDs into a local record. It restores the scene in this order.

- Selection is held at runtime as `const ObjectID*` pointers but written as UIDs (`selectedUids` plus `activeUid`), mirroring `SelectionState`. On load the UIDs are resolved back to pointers via `Scene::GetObjectID()` after restoring scene membership.
- The selection block is optional: legacy files without it load with an empty selection.
- A positive `activeCamUid` activates that scene camera. Zero or an absent field selects the viewport camera.

See [Scene Objects](../scene/objects.md) and [Serialization](../core/serialization.md).

## Resource pool persistence

`ResourceComponent` saves the explicit `ResourceManager` pool polymorphically. On open it expands cereal's archive-local type names, validates each record's identity, and registers an owned payload and loader under that UID. All indexed UIDs are reserved before any loader runs. Each loader reads one pointer through its own archive. The scene, viewport camera and active graph request their dependencies through the pool. Unrelated resources stay deferred until a caller requests or enumerates them. A malformed record leaves independent entries readable. Null entries and shaders whose content cannot restore or compile are excluded from the live project pool.

The pool reader uses registered type metadata for names and validation. Each object declares its own references. Render graphs also register a completion step during deserialization to rebuild their connections once pooled dependencies are available. The application supplies the pass catalog at load time. It provides built-in ports and reference shaders for CPU graph validation.

Mesh and debug-mesh geometry, environment images, and mesh shaders use typed `ResourceRef` fields. Geometry and image references retain the saved UID when the target is missing, so another save preserves the link. Shader references fall back to the default when their target is missing or incompatible. Surviving names are restored deterministically by UID. See [UID and Resources](../core/uid-and-resources.md) and [Data Resources](data-resources.md).

For example, `archive::OptionalRef(ar, "o_shaderId", o_shader)` writes the shader UID and restores its shared object. An empty shader omits the key. Both readers collect bindings as cereal reads the object. The indexed reader resolves them before publishing that object. The direct cereal pool reader resolves them after all objects are registered, so a mesh can precede its shader in the file.

References in scene membership lists, graph records and viewport settings can be read before their targets are available. These records carry the UID until the corresponding scene, graph or viewport is restored. Graph undo records also keep the UID so replay resolves the shader against the current project pool.

### Saving a mixed pool

Project save walks the registry's UIDs. For a live entry it writes the object's current fields. For a deferred entry it writes the owned record and applies its current registered name. Both paths use the same output archive to assign cereal's pointer and type IDs. Removed UIDs have no entry to write. Save As can write the document after the original project file has been moved or deleted. External assets continue to use the normal asset path resolver.

## Optional config and render graph

A missing, null or unusable whole config stays null and is omitted on save. A present config is constructed with declared defaults, then valid fields are restored independently; malformed fields keep their defaults without discarding valid neighbors. Read-only queries use effective defaults without creating project data. Explicit edits materialize stored config, and undo can restore absence.

`GraphComponent` writes the active graph selection as `"proj_render_graph": {"uid": 42}`. The corresponding `RenderGraphData` entry under `m_resources` owns the `topology` record containing the nodes, output tags, edges and selected scene outputs. The `outputs` array records the name of each scene destination and its producer node and port. A graph lookup resolves its shaders and socket connections from its own topology record. An inactive graph stays deferred until its UID is requested. `GraphComponent` then looks up the active UID in the restored pool. A missing or unusable shader selects the built-in shader for that node. A null node shader omits its `shader` field on save.

An absent graph key, unusable graph or unresolved active UID leaves the graph pointer null. A read or render uses the cached built-in topology and preserves the missing key on save. See [Render Graph](../render/render-graph.md).

## UIComponent: opaque layout blob

`UIComponent` wraps an opaque UI-state string produced and consumed entirely by the UI layer (`UIManager::ExportLayout()` / `ApplyLayout()`). It bundles the main-window geometry plus the ADS dock-manager state. The asset and Application layers treat it as an opaque string, which keeps the component Qt-free.

## HistoryComponent: undo/redo stacks

`HistoryComponent` wraps a non-owning `OperationManager` pointer. Save writes both stacks as arrays of polymorphic `Operation` pointers (cereal auto-emits each op's concrete type); Load reconstructs them and hands them back with `RestoreHistory()`. Every undo/redo `Operation`'s own fields stay strict on load, because a half-read op replays a `m_before` nobody ever had. See [Serialization](../core/serialization.md).

## EditorComponent: viewport camera identity

`EditorComponent` carries exactly one field today: the pool UID of the **viewport camera** - the camera the viewport looks through unless a scene camera is activated.

- The camera *object* needs no adapter, because it is pooled and `ResourceComponent` serializes the pool wholesale. Only the viewport's claim on which pooled camera is its own has to be recorded. Without it the identity is lost and the viewpoint snaps back to the default framing on every open.
- It binds to `EditorViewport`, not to the Editor, which has no camera API.
- The viewport camera is stored under `viewportCamUid`.

See [Viewport](../editor/viewport.md).

## Layering

The asset layer is Vulkan-free: no Vulkan includes appear in public headers, and format queries use `PixelFormat` rather than raw `vk::Format` values. Asset code may load and parse asset files, provide CPU-side data structs, and query pixel-format metadata; it must not issue draw calls, create pipelines or shader modules, manage the swapchain or presentation, or store application-level state.

Asset persistence has no reverse dependency on Editor or CPU shader tooling. History and viewport adapters belong to the Editor; concrete shader registration belongs to Shader and is composed by Application. Built-in CPU data and GPU caches remain separate from nullable project data. See [Layers](../architecture/layers.md).
