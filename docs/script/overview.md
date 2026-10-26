# Scripting

Neurus embeds a CPython runtime so editor actions are scriptable at runtime,
modelled on Blender's `bpy`. This document covers the script layer: where it
sits, what it exposes, and how to run a script.

## Layer placement

`src/script/` sits on the **Editor** layer and never includes Renderer or UI
headers. It exposes scene and editor state to Python for **reading**, and routes
every requested mutation through the editor's typed events rather than touching
state directly. The isolation boundary is enforced by a test
(`test/script/test_script_isolation.cpp`).

- **`ScriptEngine`** (`src/script/ScriptEngine.h`) owns the interpreter. Its
  public header is Python-free (PIMPL), so the rest of the codebase never sees a
  pybind11 or CPython type.
- **`ScriptContext`** (`src/script/ScriptContext.h`) is the read-only document
  view: borrowed `const Scene*`, `const RenderConfig*` and
  `const IResourceLookup*` pointers. The active camera and the selection are read
  live from the scene, so neither is snapshotted here; names resolve through the
  resource pool. `Editor::UpdateScriptContext()` fills it per run; the engine
  publishes it for that run and clears it afterwards.
- **`ScriptView`** (`src/script/ScriptView.h`) is the engine-owned view
  injected into every collection and proxy when it is constructed. It is a plain
  pair of pointers with no accessors; each proxy resolves its own object through
  `view->ctx` on every access. The view's address is stable for the interpreter's
  lifetime, so a proxy can outlive its run without dangling.
- **Bindings** (`src/script/bindings/`) register the embedded `neurus` module
  once.

`Application` owns the engine and is the only place Python is started, wired
through the Script Console panel and the Tools menu.

## The `neurus` module

Read-only in this phase:

- `neurus.data.objects` - every pooled object, iterable and keyed by UID only
  (a name spans several object types, so a string key is ambiguous).
- `neurus.data.cameras`, `.lights`, `.meshes`, `.environments` - the whole
  resource pool filtered to one type, keyed by UID or display name; a name
  lookup returns the typed proxy.
- `neurus.data.scene.meshes`, `.cameras`, `.lights`, `.environments` - the same
  typed proxies restricted to objects the current scene uses; a pooled object
  detached from the scene (undo history) is not listed.
- `neurus.data.scene.objects` - every object the scene uses, keyed by UID only.
- `neurus.data.config` - the render settings (`gamma`, `pipeline`, ...).
- `neurus.context.active_camera`, `neurus.context.selection`.

```python
import neurus
for o in neurus.data.objects:
    print(o.name, o.transform.position)

ground = neurus.data.meshes["Ground"]
print(ground.name, ground.position, ground.rotation, ground.scale)
print("in scene:", len(neurus.data.scene.meshes))
print("gamma", neurus.data.config.gamma)
```

`Mesh`, `Light`, `Camera` and `Environment` proxies derive from both
`neurus.data.Object` (`uid`, `name`, `type`, `visible`, `transform`) and
`neurus.data.Transform3D` (`position`, `rotation`, `scale`), so a typed lookup
exposes both facets. A camera overrides `position`/`rotation` because its
transform is a look-at pose, and the environment's `rotation` is its
environment-map angle.

Every property is resolved live on access; nothing is cached. A collection is
empty, and a lookup returns `None`, when the run has no scene or no resource
lookup - the engine publishes a document only for the duration of a run. A pool
name lookup also returns `None` when the name is absent or belongs to another
object type; a scene name lookup additionally returns `None` when the object is
pooled but not part of the scene.

Names come from the resource registry, not the scene: uniqueness is per concrete
object type, and a collision is suffixed (`.001`, `.002`, ...), so the name keys
are stable. See [Resource names](../core/uid-and-resources.md#resource-names).

## Writes are gated

Setters exist but do **not** write the C++ property. Each one constructs the
exact typed editor event it will emit and passes it to a single dispatch funnel
(`ScriptDispatch::Emit`). In this read-only phase the dispatch carries no event
queue, so a setter raises `neurus.ReadOnlyError`:

```python
o = list(neurus.data.objects)[0]
try:
    o.transform.position = (0, 0, 0)
except neurus.ReadOnlyError as e:
    print("refused:", e)
```

Enabling writes later means supplying an `IEventQueue` to the dispatch and
nothing else changes: the event flows to the existing controller and the
`OperationManager`, so scripted edits undo and redo like any other action.

## Environment

The interpreter is the **base** CPython that the managed environment is built on,
embedded with an isolated `PyConfig` pinned to that base prefix. The virtual
environment under `lib/.venv` (a `uv` environment owned by the `Neurus-Lib`
submodule) is attached at runtime so its site-packages are importable; that is
how a heavier package such as PyTorch would be added later.

- Provision the environment with `make update`.
- Override the environment path at configure time with
  `-DNEURUS_PYTHON_ENV=<path>` (the value is baked into the generated
  `neurus_python_config.h`).
- If the environment is missing, the interpreter still starts isolated against
  its base install.

## Running a script

Two surfaces drive the engine:

- **Script Console** (bottom dock) - paste a snippet and press `Ctrl+Enter`, or
  use **Run File...**.
- **Tools -> Run Script** - pick a `.py` file; the console dock is raised.

Output (`print`, tracebacks) appears in the console. Errors also reach the Log
panel as `NEURUS_ERR`, and never crash the host.
