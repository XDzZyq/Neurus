# Project Serialization

Project persistence is a pure registration-based serializer plus a fixed set of components, one per concern, that the Application registers in a load-bearing order.

## Project

`src/asset/Project.h` defines `project::Project`:

- It owns no data - no project path, no dirty flag, no concrete type knowledge.
- Callers register `Serializable` components via `Register<T>(args...)`, which constructs each component and stores it as `unique_ptr<Serializable>`, returning a reference.
- `Save(path)` / `Load(path)` iterate every registered component and call its virtual `Save` / `Load`; the document is written as `.neurus.json` under the `"project"` node.
- `Clear()` destroys all components.
- It depends only on cereal and `Serializable.h`.

The Application owns the project path and builds a transient `Project` per save/open, registering the components below. The generic helper machinery lives in [Serialization](../core/serialization.md).

## Serializable

`src/asset/Serializable.h` defines the component interface:

- `Key()` returns the archive node name.
- `Save(cereal::JSONOutputArchive&)` / `Load(cereal::JSONInputArchive&)` are the persistence hooks.

`Project` casts to the JSON archive because the virtual interface is typed to it.

## Components and registration order

Registration order **is** archive read order, so a new component belongs at the end.

| Order | Component | Key | Wraps |
|---|---|---|---|
| 1 | `ResourceComponent` | `m_resources` | The `ResourceManager` UID pool |
| 2 | `SceneComponent` | `m_scene` | The `Scene` |
| 3 | `ConfigComponent` | `proj_config` | A `RenderConfig` |
| 4 | `UIComponent` | `ui` | An opaque UI-state string |
| 5 | `HistoryComponent` | `m_history` | The undo/redo stacks |
| 6 | `EditorComponent` | `m_editor` | The viewport camera's pool UID |

`ResourceComponent` is registered **first** so the pool is restored - and its data references wired - before `SceneComponent` resolves the Scene's ID lists against it. `HistoryComponent` and `EditorComponent` are registered last, in that order, so legacy files without their nodes load cleanly: `HistoryComponent` clears the stacks instead of throwing, and `EditorComponent` leaves its uid at `0`, which makes the finish-load step mint a fresh viewport camera.

Each component wraps a non-owning pointer. The headers stay lightweight (forward-declaring `ResourceManager`, `Scene`, `RenderConfig`, `OperationManager`, `EditorViewport`), and only the `.cpp` files include the force-init registration headers for the pooled types.

## Scene persistence

`Scene::serialize` persists selection state alongside the typed object pools:

- Selection is held at runtime as `const ObjectID*` pointers but written as UIDs (`selectedUids` plus `activeUid`), mirroring `SelectionState`. On load the UIDs are resolved back to pointers via `Scene::GetObjectID()` after `RebuildObjList()`.
- The selection block is optional: legacy files without it load with an empty selection.
- `activeCamUid` is three-state, and that is load-bearing. Which scene camera the viewport looks through is written last in both `serialize` branches and resolved right after `ResolvePool<Camera>()`: `> 0` is an explicit UID, `0` is *explicitly none* (the normal state - the viewport's own camera takes the view), and `-1` is the field being *absent*, which migrates once by activating `cam_list.begin()->second` so a project saved before the feature still opens through the camera it was saved with. The next save writes the field, so the migration never recurs. This is why the field uses the raw `archive::OptionalBlock` bool form rather than a helper that just leaves the member alone.

See [Scene Objects](../scene/objects.md) and [Serialization](../core/serialization.md).

## ResourceComponent: pool and data wiring

`ResourceComponent` saves the whole `ResourceManager` pool polymorphically and restores it on load; each pooled object's own `serialize(load)` restores its content. Load then wires the per-object data references - `Mesh` to `MeshData` / `Shader`, `Environment` to `ImageData`, `DebugMesh` to `MeshData` - against the pool, so the pool is self-contained and uploadable after a reload. Its `.cpp` includes the cereal force-init headers for scene, data and shader types so the static-library registration translation units are linked in. See [UID and Resources](../core/uid-and-resources.md) and [Data Resources](data-resources.md).

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

There are two deliberate reverse dependencies from asset to editor, each confined to one `.cpp` file while the header stays editor-free:

- `components/HistoryComponent.cpp` includes editor/operations headers to persist the undo/redo stacks.
- `components/EditorComponent.cpp` includes the viewport header to record the camera UID.

Every final binary links editor alongside asset. See [Layers](../architecture/layers.md).
