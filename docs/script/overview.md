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
- **`ScriptContext`** (`src/script/ScriptContext.h`) is the read-only view the
  bindings read: a borrowed `const Scene*`, a borrowed `const RenderConfig*`,
  the active camera id, and the selected UIDs. `BuildScriptContext(...)` fills it
  from a `Scene` and a `RenderConfig`.
- **Bindings** (`src/script/bindings/`) register the embedded `neurus` module.

`Application` owns the engine and is the only place Python is started, wired
through the Script Console panel and the Tools menu.

## The `neurus` module

Read-only in this phase:

- `neurus.data.objects` - every scene object, iterable and keyed by UID.
- `neurus.data.cameras`, `.lights`, `.meshes`, `.environments` - typed pools.
- `neurus.data.config` - the render settings (`gamma`, `pipeline`, ...).
- `neurus.context.active_camera`, `neurus.context.selection`.

```python
import neurus
for o in neurus.data.objects:
    print(o.name, o.transform.position)
print("gamma", neurus.data.config.gamma)
```

Every property is resolved live on access; nothing is cached. A collection is
empty, and a lookup returns `None`, outside a run or when the run has no scene.

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
