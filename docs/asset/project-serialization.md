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

`ResourceComponent` is registered **first** so the pool is restored - and its data references wired - before `SceneComponent` resolves the Scene's ID lists against it. `HistoryComponent` and `EditorComponent` are registered last, in that order, so legacy files without their nodes load cleanly: `HistoryComponent` clears the stacks instead of throwing, and `EditorComponent` leaves its uid at `0`, which makes the finish-load step mint a fresh viewport camera.

Components borrow their owners' data or shared-pointer slots; the serializer does not own application state. Application composition force-links concrete shader registrations. The Editor-owned history and viewport adapters live under `src/editor/project/`, keeping Asset independent of Editor implementation.

## Scene persistence

`Scene::serialize` persists selection state alongside the typed object pools:

- Selection is held at runtime as `const ObjectID*` pointers but written as UIDs (`selectedUids` plus `activeUid`), mirroring `SelectionState`. On load the UIDs are resolved back to pointers via `Scene::GetObjectID()` after `RebuildObjList()`.
- The selection block is optional: legacy files without it load with an empty selection.
- `activeCamUid` is three-state, and that is load-bearing. Which scene camera the viewport looks through is written last in both `serialize` branches and resolved right after `ResolvePool<Camera>()`: `> 0` is an explicit UID, `0` is *explicitly none* (the normal state - the viewport's own camera takes the view), and `-1` is the field being *absent*, which migrates once by activating `cam_list.begin()->second` so a project saved before the feature still opens through the camera it was saved with. The next save writes the field, so the migration never recurs. This is why the field uses the raw `archive::OptionalBlock` bool form rather than a helper that just leaves the member alone.

See [Scene Objects](../scene/objects.md) and [Serialization](../core/serialization.md).

## ResourceComponent: pool and data wiring

`ResourceComponent` saves the explicit `ResourceManager` pool polymorphically. Its `LoadJson` validates and deserializes each entry in a separate archive, carrying cereal's polymorphic type dictionary forward. A malformed shader entry is skipped without losing later valid resources. Null entries and shaders whose content cannot restore or compile are excluded from the live project pool.

Load then wires `Mesh` to `MeshData` / `ShaderData`, `Environment` to `ImageData`, and `DebugMesh` to `MeshData`. Missing or unusable mesh shaders become null references with UID `0`, selecting cached renderer defaults without inserting replacement resources. Surviving names are restored deterministically by UID. See [UID and Resources](../core/uid-and-resources.md) and [Data Resources](data-resources.md).

## Optional config and render graph

A missing, null or unusable whole config stays null and is omitted on save. A present config is constructed with declared defaults, then valid fields are restored independently; malformed fields keep their defaults without discarding valid neighbors. Read-only queries use effective defaults without creating project data. Explicit edits materialize stored config, and undo can restore absence.

`GraphComponent` writes the active graph selection as `"proj_render_graph": {"uid": 42}`. The corresponding `RenderGraphData` entry under `m_resources` owns the `topology` record containing the nodes, output tags and edges. On load the Editor resolves the active UID and wires the graph's authored shader UIDs after the pool has been restored. A missing or unusable shader selects the built-in shader for that node. A null node shader omits its `shader` field on save.

A valid legacy inline `proj_render_graph` with `nodes` and `edges` is adopted into the pool, including a graph with both lists empty. The next save writes the active UID. An absent graph key, unusable graph or unresolved active UID leaves the graph pointer null. A read or render uses the cached built-in topology and preserves the missing key on save. See [Render Graph](../render/render-graph.md).

## UIComponent: opaque layout blob

`UIComponent` wraps an opaque UI-state string produced and consumed entirely by the UI layer (`UIManager::ExportLayout()` / `ApplyLayout()`). It bundles the main-window geometry plus the ADS dock-manager state. The asset and Application layers treat it as an opaque string, which keeps the component Qt-free.

## HistoryComponent: undo/redo stacks

`HistoryComponent` wraps a non-owning `OperationManager` pointer. Save writes both stacks as arrays of polymorphic `Operation` pointers (cereal auto-emits each op's concrete type); Load reconstructs them and hands them back with `RestoreHistory()`. Every undo/redo `Operation`'s own fields stay strict on load, because a half-read op replays a `m_before` nobody ever had. See [Serialization](../core/serialization.md).

## EditorComponent: viewport camera identity

`EditorComponent` carries exactly one field today: the pool UID of the **viewport camera** - the camera the viewport looks through unless a scene camera is activated.

- The camera *object* needs no adapter, because it is pooled and `ResourceComponent` serializes the pool wholesale. Only the viewport's claim on which pooled camera is its own has to be recorded. Without it the identity is lost and the viewpoint snaps back to the default framing on every open.
- It binds to `EditorViewport`, not to the Editor, which has no camera API.
- A file written before the rename carries the field as `editorCamUid`; `Load()` reads either name as an explicit migration rather than a default.

See [Viewport](../editor/viewport.md).

## Layering

The asset layer is Vulkan-free: no Vulkan includes appear in public headers, and format queries use `PixelFormat` rather than raw `vk::Format` values. Asset code may load and parse asset files, provide CPU-side data structs, and query pixel-format metadata; it must not issue draw calls, create pipelines or shader modules, manage the swapchain or presentation, or store application-level state.

Asset persistence has no reverse dependency on Editor or CPU shader tooling. History and viewport adapters belong to the Editor; concrete shader registration belongs to Shader and is composed by Application. Built-in CPU data and GPU caches remain separate from nullable project data. See [Layers](../architecture/layers.md).
