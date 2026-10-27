# UID and Resources

`UID` is the generic unique-identifier primitive at the base of every identity-tracked object, and `ResourceManager` is the single factory and registry that owns every persistent UID object.

## UID (`core/UID.h`)

`UID` provides globally unique integer IDs:

- IDs are sequential and allocated from a static counter (`s_count`).
- Not thread-safe: IDs should be allocated on the main thread.
- IDs are never reused, even after object destruction.
- Value semantics are deleted - `UID` is non-copyable and non-movable, because identity must not be duplicated or transferred. Objects are held via `shared_ptr`.
- `GetObjectID()` returns the unique integer; `GetTotalAllocated()` returns the total allocation count, including destroyed objects.
- `serialize(ar)` writes `o_id`, and on load bumps `s_count` ahead of the loaded id so newly created objects do not collide with restored ones.

`ObjectID` (scene layer) extends `UID` with scene metadata, and data resources such as `MeshData`, `ImageData` and `Shader` inherit `UID` directly and serialize themselves, bound by UID plus cereal polymorphism in the `ResourceManager` pool. See [Scene Objects](../scene/objects.md).

## ObjectID as identity (`scene/ObjectID.h`)

`ObjectID : public UID` adds scene-specific metadata:

- `o_name` - display name in the editor UI.
- `o_type` - a `GOType` tag used for runtime type identification without RTTI; see [Scene Objects](../scene/objects.md).
- `is_viewport` / `is_rendered` - visibility flags for editor display and the render pipeline, set together by `SetVisible(v, r)`.
- `IsLight()` - true for `GO_LIGHT` and `GO_POLYLIGHT`, the two types that project into the light SSBO. It is a member rather than a predicate repeated in each caller, so the second answer is never forgotten.
- Polymorphic accessors `GetShader()`, `GetShaderUnit(int)`, `GetTransform()` and `GetMaterial()`, all returning `nullptr` by default and overridden by the classes that own those components.

`ObjectID::serialize` keeps the `base_class<UID>` hook strict but reads its own metadata fields through `archive::Optional` - the concrete default constructor already sets `o_type` and `o_name`, so a field a file lacks keeps the right value.

## ResourceManager (`core/ResourceManager.h`)

`ResourceManager` is the single factory and registry for UID-derived objects (scene objects via `ObjectID`, data resources such as `MeshData` / `ImageData` / `Shader`). It is:

- App-scoped: owned by the Editor, and cleared on project new/open so no stale object leaks into a save.
- Type-erased: it holds `shared_ptr<UID>` and knows no concrete type.
- The owner of the whole pool's polymorphic serialization.

Design:

- `UID` is the only resource base. Every pooled object derives from it and serializes itself (identity plus its own fields); there is no reload-hook interface, because each object's `serialize(load)` restores its own state.
- `Load<T>(args...)` constructs and registers in one step, returning a typed `shared_ptr<T>`. It `static_assert`s that `T` derives from `UID`, and - when `T` has a public `o_name` - registers that name and mirrors the unique result back into `o_name` (see [Resource names](#resource-names)).
- `Register(shared_ptr<UID>)` registers an already-constructed object (tests, legacy paths).
- `Get(int)` returns the untyped base, `Get<T>(int)` casts it, and `Contains(int)` tests membership.
- `ForEach<T>(fn)` visits every pooled object whose **exact dynamic type** is `T`; this is how UID-keyed GPU caches follow the pool rather than the scene (pooled objects stay referenced by undo history after deletion). It is not a base-class walk: once a base instance (`ObjectID`, `UID`) is itself pooled, the per-type index holds only that instance and derived types are skipped. Base-class enumeration uses `ForEachUID(fn)` plus a cast. Neither callback may `Load` / `Register` / `Remove` / `Clear`, because mutating the pool during iteration invalidates the walk.
- `Remove(int)` and `Clear()` mutate the pool; `Size()` reports its size.
- `SetName(id, name)` / `GetByName(...)` own the display-name registry; names are unique per concrete dynamic type and are derived state, never serialized (see [Resource names](#resource-names)).
- `serialize(ar)` writes every pooled object polymorphically (which is what makes the type registrations load-bearing); on load, each object's own `serialize(load)` restores its content. A load replaces the pool, so it also drops the previous name registry and rebuilds the type index from the restored objects; names are then re-seeded by the caller.
- Not thread-safe: main thread only.

Two rules keep the pool decoupled:

- The Scene neither owns nor references the pool. It holds plain typed pools and resolves references against the pool.
- No UID object knows the `ResourceManager`, and no UID object constructs another UID object internally - construction happens through `Load<T>`.

Because the pool serializes the real objects (polymorphically) and the Scene serializes only ID references, the pool must be loaded before the Scene. See [Serialization](serialization.md) and [Project Serialization](../asset/project-serialization.md).

## Resource names

Display names are owned by the registry, not by `UID` or the scene. Every lookup
key lives in `ResourceManager`; `ObjectID::o_name` is the serialized display
string that the editor mirrors from it.

- `SetName(int id, const std::string& name)` registers or renames a resource and returns the unique name. `GetByName<T>(name)` and `GetByName(type_index, name)` resolve a name to a pooled object.
- Names are unique per concrete dynamic type, not across the pool: a `Mesh` and a `Camera` may both be called `Ground`. A collision gets the lowest free zero-padded suffix - `Ground`, `Ground.001`, `Ground.002`, ... - and an empty request becomes `Object`.
- Renaming a resource to the name it already owns is a no-op. `SetName` returns an empty string when the id is not pooled, and `GetByName<T>` requires the name to belong to a `T`.
- Removing a resource frees its name, so a later resource can reuse it.
- Names are derived state and are never serialized. After the pool loads, `ResourceComponent::Load` re-seeds the registry from every non-empty restored `ObjectID::o_name` in ascending UID order and writes the returned unique name back, so duplicates in an older project file are disambiguated deterministically and a re-save preserves them. Creation paths need no extra step: `ResourceManager::Load<T>` registers a scene object's constructor name and mirrors the unique result, so every creation path seeds automatically.
- The registry name is authoritative for lookup; `o_name` is what the UI shows and what a project file stores. Any editor rename path must mirror the returned unique name back into `o_name`; `Load<T>` and `ResourceComponent::Load` write the returned name back themselves. Writing `o_name` directly changes the display string but not the lookup key.
- `"ViewportCamera"` is reserved by the editor for its pooled viewport camera: `EditorViewport::EnsureCamera` loads the camera (which registers `"Camera"`) and immediately renames it, so the viewport camera can never claim the document camera's `"Camera"` name on a reopen. The viewport camera is not scene content, so it never appears in `Scene::cam_list`.

`Scene` keeps no name index: registration, removal and `RebuildObjList` only maintain the typed pools and `obj_list`. See [Scene Objects](../scene/objects.md).

## Resource<T> and ResPool (`scene/Scene.h`)

The Scene's typed containers are defined in the scene layer:

- `Scene::Resource<Object>` is an alias for `std::shared_ptr<Object>`.
- `Scene::ResPool<Object>` is `std::unordered_map<int, Resource<Object>>`, mapping a UID to its shared pointer for O(1) lookup and removal.

Each object category has a dedicated `ResPool` (`cam_list`, `mesh_list`, and so on), and a master `obj_list` holds all `ObjectID` instances for global lookup. Objects are owned via `shared_ptr`, so removing from a map destroys an object only when no other reference exists. See [Scene Objects](../scene/objects.md).

## IResourceLookup (`core/IResourceLookup.h`)

`IResourceLookup` is the fetch/lookup plus name-registry surface over a UID-keyed pool:

- `Get(int)` returns the untyped base (or `nullptr`), `Contains(int)` tests membership, and `Get<T>(int)` is the typed lookup.
- `GetByName(...)` resolves a registered name within one concrete type, and `SetName(...)` registers or renames a resource; these are the registry operations described in [Resource names](#resource-names).
- `ForEachUID(fn)` enumerates the whole pool; `ForEach<T>(fn)` enumerates one exact dynamic type. Base-class enumeration therefore goes through `ForEachUID` plus a cast.
- Controllers depend on this narrow interface instead of the concrete `ResourceManager`: they can fetch pooled objects by integer id, enumerate them and register names, but cannot mutate the pool (no `Load` / `Register` / `Remove` / `Clear`) or drive its serialization. This mirrors `IOperationSink`, keeping the dependency inverted.
- Objects are fetched as `shared_ptr` because the pool owns them; a handler typically derives a raw pointer from the temporary `shared_ptr` and uses it within the handler scope. The pool keeps pooled objects alive, so id-based lookup never dangles.

`ResourceManager` implements `IResourceLookup`.

## Identity across save and load

Selection is held at runtime as `const ObjectID*` pointers but written as UIDs (`selectedUids` plus `activeUid`), mirroring `SelectionState`. The pointer-to-UID conversion lives in the scene-layer free functions `neurus::SnapshotSelectionUids` / `RestoreSelectionUids` (in `Scene.h`), shared by `Scene::serialize` and the editor's selection snapshot/restore. On load the UIDs are resolved back to pointers via `Scene::GetObjectID()` after `RebuildObjList()`. Keeping this in the scene layer (which owns the pointer-to-UID mapping) means the editor depends downward only.

## Cereal registration

The pool stores `shared_ptr<UID>`, so every reachable type must be registered with cereal's polymorphic system:

- Scene types: `scene/registrations/TypeRegistration.cpp` registers the `UID` and `ObjectID` bases and every derived scene type, plus the `UID`-level and `ObjectID`-level polymorphic relations that make pool serialization load-bearing.
- Data resources: `asset/registrations/DataRegistration.cpp` registers `MeshData` and `ImageData` as polymorphic leaves of `UID`.

The registrations live in `.cpp` translation units, so static-library builds would dead-strip them unless a symbol is odr-used. The force-init headers `scene/registrations/TypeRegistration.h` and `asset/registrations/DataRegistration.h` (each a `CEREAL_FORCE_DYNAMIC_INIT`) must be included at every serialization site that touches the pool. See [Serialization](serialization.md).
