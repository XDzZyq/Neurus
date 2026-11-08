# Scripting

Neurus embeds a CPython runtime so editor actions are scriptable at runtime,
modelled on Blender's `bpy`. This document covers the script layer: where it
sits, what it exposes, and how to run a script.

## Layer placement

`src/script/` sits on the **Editor** layer and never includes Renderer or UI
headers. It exposes scene and editor state to Python for reading and writing,
and routes every mutation through the editor's typed events rather than touching
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

The module is split by subject, not by layer:

- `neurus.data` - the resource pool, independent of scene membership:
  - `neurus.data.objects` - every pooled `ObjectID`, iterable and keyed by UID
    only (a name spans several object types, so a string key is ambiguous). Data
    resources (`MeshData`, `ImageData`, `Shader`) are not objects and are not
    listed.
  - `neurus.data.cameras`, `.lights`, `.meshes`, `.environments` - the whole
    resource pool filtered to one type, keyed by UID or registered name; a name
    lookup returns the typed proxy.
  - `neurus.data.scene` - the **same module object** as `neurus.scene`, not a
    second proxy, so `neurus.data.scene.meshes` reads naturally.
- `neurus.scene` - the scene currently being edited:
  - `.meshes`, `.cameras`, `.lights`, `.environments` - the same typed proxies
    restricted to objects the scene uses; a pooled object detached from the
    scene (undo history) is not listed.
  - `.objects` - every object the scene uses, keyed by UID only.
  - `.active_camera()` - the active camera proxy, or `None` when none is active.
  - `.selection()` - the selected objects as a list of `Object` proxies.
- `neurus.render.config` - the render settings (`gamma`, `pipeline`, ...).

```python
import neurus
for o in neurus.data.objects:
    print(o.name, o.transform.position)

ground = neurus.data.meshes["Ground"]
print(ground.name, ground.position, ground.rotation, ground.scale)
print("in scene:", len(neurus.scene.meshes))
print("active:", neurus.scene.active_camera())
print("selected:", [o.name for o in neurus.scene.selection()])
print("gamma", neurus.render.config.gamma)
```

`active_camera` and `selection` are **functions**, not properties: each call
reads the live scene, so a value can never be captured and go stale.

`Mesh`, `Light`, `Camera` and `Environment` proxies derive from both
`neurus.data.Object` (`uid`, `name`, `type`, `visible`, `transform`) and
`neurus.data.Transform3D` (`position`, `rotation`, `scale`), so a typed lookup
exposes both facets. A camera overrides `position`/`rotation` because its
transform is a look-at pose, and the environment's `rotation` is its
environment-map angle.

Every property is resolved live on access; nothing is cached. The engine
publishes a document only for the duration of a run. The pool-wide collections
need only the resource pool (no scene): they are empty, and a lookup returns
`None`, when the run has no resource lookup. The scene collections need only the
scene: they are non-empty when the scene has objects even if the run has no
resource lookup, and their proxies raise rather than resolving. A pool name
lookup returns `None` when the name is absent or belongs to another object type;
a scene name lookup additionally returns `None` when the name is unknown to the
pool or the object is pooled but not part of the scene.

Names come from the resource registry, not the scene: uniqueness is per concrete
object type, and a collision is suffixed (`.001`, `.002`, ...), so the name keys
are stable. See [Resource names](../core/uid-and-resources.md#resource-names).
The editor's pooled viewport camera is registered as `ViewportCamera`, so it
appears in `neurus.data.cameras` but not in `neurus.scene.cameras`.

`__getitem__` and `__contains__` agree on every collection: a UID or name that a
lookup resolves is also what `in` reports, and `objects` is UID-only in both.

## Writes

Setters write the live object. A setter resolves its target through the run's
resource pool, builds the exact typed editor event for the property, and emits it
through `ScriptDispatch::Emit`, the single funnel into the editor's synchronous
dispatch:

```
mesh.position = (2, 2, 2)
  |-- resolve the UID through IResourceLookup (raises when it is gone)
  |-- PositionChanged { objectUid, posX, posY, posZ }
  |-- IEventQueue::Emit -> ObjectController::OnPositionChanged
        |-- mutates the object's Transform3D
        |-- submits one SetPositionOp to the OperationManager
        |-- emits SceneModified + RenderResetEvent, plus LightingRebuild for a light
```

Dispatch is synchronous and depth-first, so every handler has run before the
setter returns: a write is visible on the very next Python statement, and a loop
can read back each value it assigns.

```python
mesh = neurus.data.meshes["Ground"]
mesh.position = (1, 2, 3)
print(mesh.position)            # (1.0, 2.0, 3.0) - already applied
```

Writability is a property of the resource pool, not of scene membership: a pooled
object that is not in the scene - an object held by undo history, or the pooled
viewport camera - is writable too, as long as a setter names it. A target that
has left the pool keeps its Python proxy, but the next write raises
`RuntimeError`, because the setter resolves the UID before it emits:

```python
mesh = neurus.data.meshes["Ground"]
# ... the object is removed from the pool
mesh.position = (0, 0, 0)       # RuntimeError: ... refers to a missing object
```

`Camera.is_active` is the exception: it names no object, it chooses which scene
camera the viewport looks through, so there is no target to resolve - and the
`SceneController` keeps refusing a camera the scene does not use.

`neurus.ReadOnlyError` no longer exists: there is no read-only mode left to
report, and an engine with no editor attached raises `RuntimeError` instead of
dropping the write silently.

### One undo step per run

A scripted write is recorded like a panel edit, so scripted changes undo and redo
like any other action. `ScriptEngineImpl::RunSource` brackets each run with
`ScriptRunStarted` and `ScriptRunFinished`; `ScriptController` forwards both to
the operation sink's `BeginScriptScope()` / `EndScriptScope()`, and
`OperationManager` implements them by buffering every operation the run records
and committing the buffer as one `ScriptRunOp`. The run supplies that entry's
History label: a console run shows "Run Script", a file run shows "Run <file>.py".
One `Ctrl+Z` therefore removes the whole run, not its last statement. A run that
raises still closes its scope, so the writes before the error stay as one entry; a
run that changed nothing records no entry at all. See
[Operations](../editor/operations.md#one-undo-step-per-script-run).

Scripts run on the GUI thread inside the frame loop, before `Editor::Edit()`, and
`RunSource` holds the GIL for the whole run. Controller work and the GPU uploads
a write triggers therefore execute under the GIL as well: the model is
single-threaded, and a script's cost is frame time like any other editor work.

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
