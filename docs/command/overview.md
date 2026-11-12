# Command Sessions and the `neurus` CLI

Neurus exposes a typed command interface to a running application over
loopback IPC. The standalone `neurus` executable drives it so scripts, external
tools and agents can inspect and control an editor session without going through
the Qt UI.

The command layer is deliberately thin: it validates requests, resolves targets
on the application thread, and then uses the existing typed events, controllers
and operations. It is not a second mutation or undo system.

## Session model

- A session exists only when Neurus is launched through `neurus open`. A normal
  launch, including a double-click on the executable, starts no listener and
  writes no registry entry.
- Sessions are named and independent. Several may run at once, including
  against the same project.
- There is no lifetime coupling to the terminal: closing the terminal leaves
  the session running. Use `neurus close` or close the window.
- `neurus open` waits for the readiness handshake before it exits, so a
  following command can target the session immediately.
- `neurus open` binds the new session to the current terminal, and
  `neurus use NAME` changes that binding. A terminal binding lets parallel
  agents work against different sessions without passing `--session` on every
  command. Explicit `--session` and `NEURUS_SESSION` override the binding.

Session targeting resolves in this order:

1. `--session NAME` on the command.
2. `NEURUS_SESSION` from the environment.
3. The terminal binding written by `neurus open` or `neurus use`.
4. The only live session, if exactly one exists.

With several live sessions and no explicit target, the command fails as
`AMBIGUOUS_SESSION` instead of guessing.

## Architecture

```
neurus (CLI) -> neurus_ipc -> neurus_command -> neurus_core
                   |
                   +-> Asio loopback TCP
```

- `src/command/` (`neurus_command`) holds request/result values, the stable
  error codes, descriptors, the JSON codec and the dispatcher. It is Qt-free and
  Vulkan-free.
- `src/ipc/` (`neurus_ipc`) holds length framing, the session registry, the
  token generator, and the Asio client and server. It is Qt-free and
  Vulkan-free.
- `src/cli/` (`neurus_cli`, output name `neurus`) holds CLI11 parsing, process
  launch and output formatting.
- `src/app/session/` hosts the server in the running application. It is the
  only new code that uses Qt.
- `src/editor/commands/` holds the scene/object adapter. It reads the scene and
  pool directly and emits the same typed events the UI emits for writes.

The renderer is not involved. Commands reach it only when an editor handler
posts a `RenderRequest`, exactly like a UI edit.

## Threading

- One Asio thread accepts connections, reads frames and writes responses.
- The Asio thread never calls into the editor, the scene or the renderer.
- Each request enters a bounded queue (`CommandDispatcher`). The application
  calls `DrainOnMainThread()` at the start of the frame tick, before scripts and
  `Editor::Edit()`, and executes at most 16 commands per tick.
- Execution is synchronous and complete. A successful response means every
  controller triggered by the command has finished; there is no
  accepted-but-pending state in protocol version 1.
- `session.close` writes its response first, and only then asks the application
  to quit. The window close path removes the registry entry.

## Registry

Entries live in `~/.neurus/sessions`, next to `preferences.json`:

- `<name>.json` - pid, port, token, project, versions and start time.
- `<name>.log` - stdout and stderr of the spawned application.
- `<name>.claim` - a short-lived claim used while `neurus open` waits.
- `terminals/<hash>.json` - the session bound to one terminal/tab.

The directory is per-user and owner-only on POSIX. Entries and bindings are
written owner-only from creation (`platform::WriteOwnerOnlyFile`) and renamed
into place, and a parent directory the CLI or app creates (such as `~/.neurus`)
is owner-only too; an existing parent keeps its mode. The token never enters the
project directory. A session name must match `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`,
which also prevents path traversal. Stale entries (dead pid) are pruned during
discovery. `NEURUS_SESSIONS_DIR` overrides the directory for tests.
`NEURUS_TERMINAL_ID` overrides the terminal identity, which is useful for
harnesses that do not expose a stable terminal/tab id. Without it, a terminal
hint (`WT_SESSION`, `TERM_SESSION_ID`, `TMUX_PANE`, ...) is used when present.
Otherwise Windows keys on the parent process and its creation time, and macOS
keys on the POSIX session (`getsid`) and its leader's start time (the session
id alone once the leader has exited), so a binding
survives `sh -c` wrappers and stdin redirection but not a new terminal.

## Protocol

Transport is TCP on `127.0.0.1`, using an ephemeral port recorded in the
registry. One connection carries one command: hello, command, response, close.

Framing is a 4-byte big-endian length followed by one UTF-8 JSON body. Request
bodies are capped at 1 MiB and response bodies at 4 MiB. At most 8 connections
are accepted at once.

```json
{"v":1,"id":1,"command":"session.hello","args":{"protocol_version":1,"token":"<hex>"}}
{"v":1,"id":2,"command":"session.close","args":{"save":false,"force":false}}
{"v":1,"id":2,"ok":true,"result":{"session":"build-1","pid":4212}}
{"v":1,"id":2,"ok":false,"error":{"code":"DIRTY_PROJECT","message":"...","details":{}}}
```

`v` is the protocol major version. Additive fields are allowed within a major
version and unknown fields are ignored. `id` is echoed by the server. The first
command must be `session.hello`; a wrong token, a non-loopback peer or an
unsupported version is rejected before any command reaches the executor.

Timeouts: connect 3 s, handshake 3 s, command 10 s, `script` commands 300 s,
`close --save` 60 s, and a 5 s idle read on the server. The longer `script`
default absorbs a first cold import of a heavy package; preload is the primary
fix and `--timeout` stays authoritative.

## Security

- The listener binds `127.0.0.1` only.
- Each session uses a 256-bit token from the operating-system CSPRNG, compared
  in constant time.
- Logging in is a handshake, not an argument on every request.
- `session.close` and `script.run` are marked sensitive. Later commands can opt
  in the same way.
- No raw pointers or memory addresses cross the protocol.

## CLI reference

```
neurus open [PROJECT] [--name NAME] [--app PATH] [--preload NAME]... [--timeout SECS] [--no-wait] [--no-bind]
neurus close [NAME] [--all] [--save|--force] [--timeout SECS]
neurus status [NAME]
neurus list
neurus use [NAME] [--clear]
neurus project info|new [--force]|open --path FILE [--force]|save [--path FILE]
neurus scene list [--type all|mesh|light|camera|environment|sprite|debug]
neurus scene info
neurus object [--name NAME|--uid N] [--position|--rotation|--scale|--visible]
neurus object get --name NAME|--uid N [--type TYPE]
                   [--position|--rotation|--scale|--visible]
neurus object set --name NAME|--uid N [--rename NAME] [--position X,Y,Z]
                   [--rotation X,Y,Z] [--scale X,Y,Z] [--visible true|false]
                   [--viewport-visible true|false] [--render-visible true|false]
                   [--fov DEG] [--target X,Y,Z] [--active true|false]
                   [--power P] [--color R,G,B] [--radius R] [--cutoff C]
                   [--outer-cutoff C] [--shadow true|false]
                   [--material true|false] [--intensity I]
                   [--environment-rotation DEG]
neurus object load -f PATH [--name NAME]
neurus object add --type camera [--name NAME] [--position X,Y,Z]
                   [--target X,Y,Z] [--fov DEG] [--active true|false]
neurus object duplicate --name NAME|--uid N [--rename NAME] [--type TYPE]
neurus object delete --name NAME|--uid N|--uids N,N,... [--type TYPE]
neurus render get
neurus render set [--pipeline forward|deferred|custom0] [--aa none|msaa|fxaa]
                  [--ao none|ssao]
                  [--shadow none|shadow_mapping|sdf_soft|vssm]
                  [--ssr none|ray_marching|sdf_ray_marching|sdf_resolved]
                  [--gamma G] [--shadow-bias B] [--ao-radius R] [--ao-ksize N]
                  [--sample-pf N] [--sampling-count N] [--sampling-mode 0|1]
                  [--transparent true|false] [--debug-draw true|false]
                  [--fxaa-subpix V] [--fxaa-edge-threshold V]
                  [--fxaa-edge-threshold-min V]
neurus history show|undo|redo
neurus script run --code "PYTHON" | --file FILE
                   [--sandbox|--trusted] [--allow-read PATH] [--allow-write PATH]
                   [--block MODULE] [--allow-network] [--allow-subprocess]
                   [--allow-ctypes] [--time-limit SECONDS] [--memory-limit MB]
neurus script eval "EXPRESSION" [--sandbox|--trusted] [--allow-read PATH] ...
neurus script info
neurus script mode [trusted|sandboxed]
neurus script console [--tail N]
neurus script clear
neurus snapshot capture --path PATH [--all]
neurus --format human|json
```

`--format` and `--session` are accepted before or after the subcommand. Every
document command accepts `--session`, so several agents can target different
sessions from different terminals. Every `script` subcommand accepts
`--timeout SECS` (default 300, see the timeout note above).

- `open` allocates a name when none is given, claims it, spawns the application
  detached with `--session` and optional `--project`, then polls
  `session.status` until ready. `--preload NAME` is forwarded to the application
  (repeatable, comma-separated) so packages are imported before its first frame;
  a heavy preload can delay readiness past the default `--timeout`, which is
  then raised explicitly. A name whose entry belongs to a live process is
  `SESSION_EXISTS`; a stale entry is replaced. Only an entry carrying the pid
  this launch spawned counts as ready. The app is `--app`, then `NEURUS_APP`,
  then the build tree next to the CLI; only when neither override is given does
  it fall back to the installed bundle (`/Applications`, then
  `~/Applications` on macOS). A launch that cannot start (for example a
  non-executable `--app`) fails at once with `APP_EXITED`, and an app that exits
  during startup is reported as soon as it is gone. It binds the session to the
  current terminal unless `--no-bind` is given. `--no-wait` prints the pid
  immediately.
- `close` targets a name, the terminal binding, the only live session when no
  name is given, or every session with `--all`. Several live sessions without a
  target is `AMBIGUOUS_SESSION` and changes nothing. A dirty project is refused
  unless `--save` or `--force` is given; the two are mutually exclusive, so
  `--save --force` is a usage error. A successful close clears a binding
  that points at the closed session.
- `status` performs the handshake and reports one session, with the project the
  application has open now (the registry keeps the launch-time project, which
  `list` shows).
- `list` reads only the local registry and prunes stale entries. The bound
  session is marked with `*` in human output and `bound: true` in JSON.
- `use` binds a live session to this terminal, or reports/clears the binding.
- `project` creates, opens, saves and reports the document. `new` and `open`
  refuse to discard a dirty document without `--force`.
- `scene list` reports scene membership, optionally filtered by object type.
- `scene info` reports counts by type, selection and the active camera.
- `object` reads and writes scene objects. The root form with a property flag
  is the read shorthand; `get` accepts the same target and property flags and
  prints the same subset, or the whole object when none is given. `set`, `add`,
  `duplicate`, `load` and `delete` are explicit. `add` currently creates cameras
  only; adding and duplicating lights stay disabled until issue #60 is fixed.
  Transform and property writes go through the same typed events as the panels,
  so they are undoable and visible to the next frame. A multi-property `set`, an
  `add` with properties, and a `duplicate` are each one undo entry.
- `render get` reports the render configuration; `render set` changes any
  subset through the render-config controller and its undo entry.
- `history show` reports the undo/redo stacks; `history undo` and
  `history redo` step them.
- `script run` executes Python in the running application and returns the
  captured output. A run is one undo entry, like the Script Console. The script
  family defaults `--timeout` to 300 s so a first cold import does not look like
  a transport failure; preload is the primary fix. It runs sandboxed by default;
  `--trusted` removes the restriction and the policy flags override the sandbox
  defaults. A refusal is `SCRIPT_SANDBOX_VIOLATION`.
- `script eval` evaluates one expression and returns its `repr`, with the same
  sandbox flags and error mapping.
- `script info` reports readiness, the Python version, the managed environment,
  the resident modules (`modules`, `module_count`, `modules_truncated`), the
  session mode and the policy summary a run under that mode would use.
- `script mode` reports the session pin, or sets it to `trusted` or
  `sandboxed`. The pin drives the Tools-menu run and the `script info`
  summary; it does not override an explicit CLI choice.
- `script console` returns recent captured lines; `script clear` empties the
  buffer.
- `snapshot capture` writes a PNG of the presented viewport, or every render
  attachment with `--all`. The session name is injected into the destination so
  parallel sessions cannot overwrite each other: `out.png` becomes
  `out-<session>.png`, and `--all --path DIR` writes under `DIR/<session>/`.
  The response carries the final path and every written file.

Exit codes:

| Code | Meaning |
|------|---------|
| 0 | Success |
| 2 | Usage or validation error, including any command-line parse error |
| 3 | No session, or an ambiguous target |
| 4 | Transport failure or timeout |
| 5 | Command rejected (dirty project, save/script failure, object lookup) |
| 6 | Application missing or exited during startup |
| 7 | A sandbox policy refused an operation during a script run |

## Error codes

| Code | Meaning |
|------|---------|
| `INVALID_ARGUMENT` | Missing, malformed or conflicting argument |
| `SESSION_NOT_FOUND` | Named registry entry absent or pruned |
| `SESSION_EXISTS` | `open` requested a live name |
| `AMBIGUOUS_SESSION` | A command had no explicit target and several live sessions exist |
| `NO_SESSIONS` | No live session was found |
| `DIRTY_PROJECT` | `close`, `project.new` or `project.open` refused because of unsaved changes |
| `SAVE_FAILED` | `project.save` or `close --save` failed to persist |
| `IO_ERROR` | Project or mesh file could not be read/written |
| `SCRIPT_ERROR` | Python code raised an exception, or a run exceeded its wall-clock limit |
| `SCRIPT_SANDBOX_VIOLATION` | The script policy refused a filesystem, import, network, subprocess or ctypes operation |
| `OBJECT_NOT_FOUND` | No scene object matched the name or UID |
| `AMBIGUOUS_OBJECT` | Several scene objects matched the name; pass `type` or `uid` |
| `UNAUTHORIZED` | Missing or wrong token, or non-loopback peer |
| `PROTOCOL_VERSION_MISMATCH` | Unsupported protocol major |
| `FRAME_INVALID` | Malformed frame or JSON |
| `FRAME_TOO_LARGE` | Frame exceeded its cap |
| `SERVER_BUSY` | Dispatcher queue full |
| `TIMEOUT` | A protocol step exceeded its budget |
| `APP_UNREACHABLE` | Registry exists but the endpoint refuses connections |
| `APP_EXITED` | The spawned app exited before becoming ready |
| `INTERNAL` | Unexpected server-side failure |

## Current command surface

Version 1 implements session lifecycle, project persistence, scene/object reads
and writes, camera creation, render settings, undo history and Python script
runs:

- `session.status` - session name, pid, project, dirty flag, object count,
  versions, uptime.
- `session.close` - dirty policy, optional save, graceful shutdown.
- `project.info` - path, dirty flag, object count, versions, uptime.
- `project.new` - replace the document with the default scene.
- `project.open` - load a project file.
- `project.save` - save to the current path or a new path.
- `scene.list` - scene objects, optionally filtered by type.
- `scene.info` - counts by type, selection and active camera.
- `object.get` - full object record by name or UID.
- `object.set` - rename, transform, visibility and type-specific properties.
- `object.load` - load an OBJ mesh and add it to the scene.
- `object.add` - create a camera (lights are disabled until issue #60 is fixed).
- `object.duplicate` - copy a mesh or camera.
- `object.delete` - remove one or more objects from the scene.
- `render.get` - pipeline, anti-aliasing, AO, shadow, SSR and quality settings.
- `render.set` - change a subset of render settings.
- `history.get` - undo/redo labels and availability.
- `history.undo` - undo the most recent edit.
- `history.redo` - redo the most recently undone edit.
- `script.run` - run Python and return its captured output, optionally under a
  sandbox policy.
- `script.eval` - evaluate one Python expression and return its `repr`.
- `script.info` - readiness, Python version, managed environment, console size,
  resident modules, session mode and the policy summary.
- `script.mode` - report or pin the session scripting mode.
- `script.console` - recent captured console lines.
- `script.clear` - clear the console buffer.
- `snapshot.capture` - write the viewport or all attachments, with the session
  name injected into the path.

The command surface is complete for this milestone; later work is additive.

## Testing

The command core, protocol, registry, dispatcher, close policy and terminal
binding are covered by CPU-only GoogleTest suites under `test/command`,
`test/app`, `test/session` and `test/editor`. `test_editor_command_adapter`
drives the real editor controllers with no GPU, so the JSON shape and the
scene/undo behavior are verified without a window. The end-to-end `open`,
`project`, `object`, `save`, `close` sequence needs a windowed application and
is documented in [Test](../build/test.md).
