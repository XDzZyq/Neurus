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
- `src/editor/commands/` is reserved for the later scene and object command
  adapters.

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

The directory is per-user and owner-only on POSIX. The token never enters the
project directory. A session name must match `[A-Za-z0-9][A-Za-z0-9_-]{0,63}`,
which also prevents path traversal. Stale entries (dead pid) are pruned during
discovery. `NEURUS_SESSIONS_DIR` overrides the directory for tests.

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

Timeouts: connect 3 s, handshake 3 s, command 10 s, `close --save` 60 s, and a
5 s idle read on the server.

## Security

- The listener binds `127.0.0.1` only.
- Each session uses a 256-bit token from the operating-system CSPRNG, compared
  in constant time.
- Logging in is a handshake, not an argument on every request.
- `session.close` is marked sensitive. Later `script.run` will be too.
- No raw pointers or memory addresses cross the protocol.

## CLI reference

```
neurus open [PROJECT] [--name NAME] [--app PATH] [--timeout SECS] [--no-wait]
neurus close [NAME] [--all] [--save] [--force] [--timeout SECS]
neurus status [NAME]
neurus list
neurus --format human|json
```

`--format` is accepted before or after the subcommand.

- `open` allocates a name when none is given, claims it, spawns the application
  detached with `--session` and optional `--project`, then polls
  `session.status` until ready. `--no-wait` prints the pid immediately.
- `close` targets a name, the only live session when no name is given, or every
  session with `--all`. Several live sessions without a name is
  `AMBIGUOUS_SESSION` and changes nothing. A dirty project is refused unless
  `--save` or `--force` is given.
- `status` performs the handshake and reports one session.
- `list` reads only the local registry and prunes stale entries.

Exit codes:

| Code | Meaning |
|------|---------|
| 0 | Success |
| 2 | Usage or validation error |
| 3 | No session, or an ambiguous target |
| 4 | Transport failure or timeout |
| 5 | Command rejected (dirty project, save failed) |
| 6 | Application missing or exited during startup |

## Error codes

| Code | Meaning |
|------|---------|
| `INVALID_ARGUMENT` | Missing, malformed or conflicting argument |
| `SESSION_NOT_FOUND` | Named registry entry absent or pruned |
| `SESSION_EXISTS` | `open` requested a live name |
| `AMBIGUOUS_SESSION` | `close` had no name and several live sessions |
| `NO_SESSIONS` | No live session was found |
| `DIRTY_PROJECT` | `close` refused because of unsaved changes |
| `SAVE_FAILED` | `close --save` failed to persist |
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

Version 1 implements session lifecycle only:

- `session.status` - session name, pid, project, dirty flag, versions, uptime.
- `session.close` - dirty policy, optional save, graceful shutdown.

The command naming convention reserves later families: `scene.list`,
`object.set`, `project.save`, `render.set`, `script.run`, `snapshot.capture`.
They will map onto the existing typed events and operations, with name-to-UID
resolution performed on the application side.

## Testing

The command core, protocol, registry, dispatcher and close policy are covered by
CPU-only GoogleTest suites under `test/command`, `test/app` and `test/session`.
The end-to-end `open`, `status`, `close` sequence needs a windowed application
and is documented in [Test](../build/test.md).
