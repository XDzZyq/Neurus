# Scene Objects

Scene objects are Vulkan-free logical objects that carry identity through `ObjectID` and are discriminated at runtime by a `GOType` tag rather than RTTI.

## ObjectID base

`ObjectID` (`scene/ObjectID.h`) extends the core `UID` with the metadata every scene object shares:

- `o_name` - display name in the editor UI.
- `o_type` - a `GOType` tag for runtime type identification; the concrete class fixes it in its constructor.
- `is_viewport` / `is_rendered` - editor display and render-pipeline visibility, set together by `SetVisible(v, r)`.
- `IsLight()` - true for `GO_LIGHT` and `GO_POLYLIGHT`, the two types that project into the light SSBO.
- Polymorphic accessors `GetShader()`, `GetShaderUnit(int)`, `GetTransform()` and `GetMaterial()`, returning `nullptr` by default so consumers can query a component without a concrete type.

The identity side of `ObjectID` is described in [UID and Resources](../core/uid-and-resources.md).

## GOType

`ObjectID::GOType` assigns each object category an integer tag:

| Value | Object |
|---|---|
| `NONE_GO` | Invalid or uninitialized object |
| `GO_CAM` | Camera object |
| `GO_MESH` | Mesh geometry |
| `GO_LIGHT` | Light source (point, sun, spot, area) |
| `GO_POLYLIGHT` | Polygonal area light |
| `GO_ENVIR` | Environment map for IBL |
| `GO_SPRITE` | 2D sprite |
| `GO_DL` | Debug line primitive |
| `GO_DP` | Debug point primitive |
| `GO_DM` | Debug mesh |
| `GO_SDFFIELD` | SDF volume for soft shadows |

`GO_POLYLIGHT` and `GO_SDFFIELD` are declared type tags; the current scene classes below do not instantiate them.

## Object classes

Every scene object inherits `ObjectID`, and every spatially placed one also inherits `Transform3D` (see [Transform](transform.md)). Spatial classes expose a `static constexpr ObjectID::GOType Type` tag.

| Class | GOType | Source | Notes |
|---|---|---|---|
| `Camera` | `GO_CAM` | `scene/Camera.h` | View and projection matrices; perspective only for now; `cam_tar` look-at target |
| `Mesh` | `GO_MESH` | `scene/Mesh.h` | Renderable geometry referencing pooled `MeshData` and a `Shader`; owns a `Material` |
| `Light` | `GO_LIGHT` | `scene/Light.h` | Point/sun/spot/area via `LightType` (`NONELIGHT` default) |
| `Environment` | `GO_ENVIR` | `scene/Environment.h` | IBL environment wrapping pooled `ImageData`; intensity and Y-axis rotation |
| `Sprite` | `GO_SPRITE` | `scene/Sprite.h` | Billboard icon marker; type-to-path resolution; holds a `Texture` reference |
| `DebugLine` | `GO_DL` | `scene/DebugLine.h` | Retained segment list; see [Debug Objects](debug-objects.md) |
| `DebugPoints` | `GO_DP` | `scene/DebugPoints.h` | Retained point list; see [Debug Objects](debug-objects.md) |
| `DebugMesh` | `GO_DM` | `scene/DebugMesh.h` | Retained wireframe over pooled geometry; see [Debug Objects](debug-objects.md) |

`Light` uses the `LightType` enum (`NONELIGHT`, `POINTLIGHT`, `SUNLIGHT`, `SPOTLIGHT`, `AREALIGHT`) and also carries static `constexpr` shadow parameters (near/far planes, blur ranges, sun field size) that are compile-time constants and are not serialized.

`Material` (`scene/Material.h`) is not an `ObjectID`; it is a surface descriptor referenced by `Mesh`, holding PBR parameters (`MAT_ALBEDO`, `MAT_METAL`, `MAT_ROUGH`, and so on) each of which may be a float, a color or a `Texture`.

`Sprite` is the only scene object whose `GetTransform()` returns null; it is the case that exercises `PropertyPanel`'s null-transform path.

Conventionally only one `Environment` is active per scene.

## Scene container

`Scene` (`scene/Scene.h`) inherits `UID` and aggregates every object:

- Each category has a typed `ResPool` (`cam_list`, `mesh_list`, ...) and a master `obj_list` holds all `ObjectID` instances for global lookup. `Resource<T>` and `ResPool<T>` are defined in [UID and Resources](../core/uid-and-resources.md).
- Active objects (camera, environment) are cached for fast access during rendering.
- `SceneModifStatus` bitfield flags track which aspects changed so the renderer can avoid redundant work; flags reset after processing.
- Objects are owned via `shared_ptr`; removing an entry destroys an object only when no other reference exists.
- The Scene neither owns nor references the `ResourceManager` pool: it holds plain typed pools and resolves references against the pool.

A scene containing zero cameras is a legal document, and deleting the last camera is a legal gesture. See [Viewport](../editor/viewport.md).

## Display names

Names are unique within a scene. `Scene` keeps a name-to-UID index beside the typed pools:

- `RegisterObject` (reached through every `Use*` method) keeps the requested `o_name` when it is free and otherwise appends the lowest free `.001`, `.002`, ... suffix, writing the unique name back to the object.
- `RemoveObject` frees the name, so a later object can reuse it.
- `ResolveReferences` / `RebuildObjList` rebuild the index in ascending UID order, so duplicate names restored from an older project file are suffixed deterministically (lower UID keeps the bare name).
- `FindObjectByName(name)` returns the object or `nullptr`; `SetObjectName(uid, name)` is the supported rename path - it uniquifies and updates the index. Writing `o_name` directly bypasses the index.
- Names are scene-wide, not per type: a camera and a mesh cannot both own `Ground`.

## Pooled data references

Scene objects never hold file paths. They reference pooled data resources through integer IDs, which the loader re-wires after deserialization:

- `Mesh` - `o_meshDataId` (pooled `MeshData`) and `o_shaderId` (pooled `Shader` / `RenderShader`); `o_mesh` and `o_shader` are the live pointers.
- `Environment` - `o_imageDataId` (pooled `ImageData`); `o_equirectData` is the live pointer.
- `DebugMesh` - `o_meshDataId` (pooled `MeshData`); `o_mesh` is the live pointer.

A zero id is an already-handled, reported state, so an absent id costs that object its data and nothing else in the pool. `Scene::ResolveReferences` / `ResolveDataReferences` restore the pointers on load; see [Project Serialization](../asset/project-serialization.md) and [Data Resources](../asset/data-resources.md).

## Serialization and registration

Each scene object serializes its `ObjectID` base and its `Transform3D` base (under the `"transform"` node) directly, then reads its own value fields with `archive::Optional`. Scene types are registered with cereal in `scene/registrations/TypeRegistration.cpp`, both as `CEREAL_REGISTER_TYPE`s and as polymorphic relations from `UID` and `ObjectID`, which is what lets the pool serialize them through a base pointer. See [Serialization](../core/serialization.md).

Legacy project files that use the pre-`m_scene` full-pool format fail to read the ID lists and degrade to an empty scene, with a default-camera fallback.
