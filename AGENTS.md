# AGENTS.md

Guidance for agentic coding in Neurus: a C++20 Vulkan-HPP 1.4 real-time renderer
built around strict layer isolation.

System design, architecture and style are **product documentation** under
[`docs/`](docs/README.md). This file is the entry point - hard rules, the
development workflow, and a routing table. It does not restate design prose;
route to `docs/` or `.github/instructions/` instead.

---

## Hard Rules

### Layer isolation and ownership

Application wires the concrete components. UI, Script and Renderer depend on
the contracts below. Only Editor and its controllers mutate the scene.

- **Application** (`src/app/`): composition root; owns the Qt app, Preferences,
  `VulkanContext` (instance, device, queues) and the `VkSurfaceKHR`; wires UI,
  Editor, Renderer, Script and the command `SessionServer`.
- **Renderer** (`src/render/`): pure rendering service; borrows the surface,
  device and queues and owns the swapchain and every GPU resource below it;
  consumes read-only scene data; must never mutate application state; must never
  include Editor or UI headers.
- **Editor** (`src/editor/`): application logic and scene mutation; owns the
  Scene, `RenderConfig`, Controllers and the `ResourceManager`; communicates via
  Context, UIEvents, its internal typed EventQueue and posted `RenderRequest`s.
- **UI** (`src/ui/`): Qt6 Widgets presentation only; owns the native window and
  exposes its handle (no Vulkan objects); emits signals; must never include
  Renderer headers.
- **Script** (`src/script/`): embedded CPython `neurus` module; reads the scene
  through a read-only `ScriptContext` and mutates only by emitting typed editor
  events through `ScriptDispatch`; never calls Editor methods, never reaches
  project save or snapshot capture, never includes Renderer or UI headers.
- **Asset** (`src/asset/`): Vulkan-free CPU-side asset loading (`MeshData`,
  `ImageData`, `PixelFormat`).
- **Scene** (`src/scene/`): Vulkan-free logical objects (Camera, Light, Mesh,
  Transform, overlays).

### Communication

| Intent | Required interface | Who handles it |
| --- | --- | --- |
| UI edits the document | Panel or `UIEvents` signal | Application connects it to `Editor::OnUIEvent`. Editor drains its queue during `Edit()`. |
| Script reads or edits the document | Read through `ScriptContext`. Emit writes via `ScriptDispatch` and `IEventQueue`. | The Editor handler applies the event synchronously. |
| UI displays document state | Application assembles a read-only `UIContext`. | Panels refresh from that view. |
| Renderer reads a frame | `Editor::BuildSnapshot` produces an owned `RenderFrameSnapshot`. Application publishes it to `RenderThread`. | Render passes read that snapshot through `RenderContext::Frame()`. |
| Editor requests GPU work | Post an owned, typed `RenderRequest` through `RenderRequestSink`. | The render thread drains requests in posting order with the published frame. |
| Window resize, surface replacement, picking or screenshot | Application posts the corresponding `RenderThread` control. | The render thread performs it. Application translates a completed pick into an editor event. |
| CLI requests a scene edit | IPC queues the command in `CommandDispatcher`. | Application drains on the main thread. The editor adapter emits typed edits. |

The concrete `EventQueue` stays inside Editor. Controllers react to events
and dispatch follow-up events synchronously. Renderer must never call back
into Editor or retain a live Editor scene for later reads.

UI and Script may include Editor's public input and typed event contracts.
UI also consumes the read-only `HistoryView`; Script uses `IEventQueue` for
dispatch. Concrete headers such as `Editor.hpp`, `EventBus.hpp` and controller
implementations stay behind that boundary. Application may include the concrete
components it wires. Component libraries must never include Application headers.
When exposing a new Editor contract, add its header for the intended consumer
in `scripts/check_layer_boundaries.py` and cover it in the checker tests.
See [layer responsibilities](docs/architecture/layers.md) and
[runtime data flow](docs/architecture/data-flow.md).

### Vulkan ownership

- Application owns the instance, device and queues (`src/app/VulkanContext`, no
  `QVulkanInstance`) and the `VkSurfaceKHR`, created with
  `platform::CreateSurface` from the viewport's native window handle.
- Renderer borrows the surface, device and queues and owns everything below
  them: swapchain, pipelines, command pools and all GPU resources.
- UI owns no Vulkan objects; it only exposes the native window handle.
- No shared ownership of GPU resources across layers. GPU-resource classes
  `= delete` copy/assign and use `vk::raii` - never a raw `vkDestroy*`.

### Platform-specific code is isolated

OS-specific code lives under `src/platform/win/`, `src/platform/mac/` or
`src/platform/linux/`, selected by `src/platform/CMakeLists.txt`. App, editor, UI,
CLI, script and test code call the platform free functions; do not add
`#ifdef _WIN32`, `#ifdef __APPLE__` or `#ifdef __linux__` outside those
implementations. A test may keep an ifdef only when the defended value is itself
platform-specific, such as an executable name or environment variable name.

Linux is a headless platform: `src/platform/linux/LinuxSurface.cpp` is a stub
whose `CreateSurface` throws, because there is no window system and no
swapchain. The Linux product binary is `neurus_headless` (no Qt, driven by the
`neurus` CLI); the Qt application is Windows and macOS only. See
[docs/build/build.md](docs/build/build.md#linux-headless).

### Coordinate system (Z-up, +Y forward, right-hand)

- Up `+Z`, forward `+Y`, right `+X` (Blender convention).
- Euler storage `m_rotation = (pitch=X, roll=Y, yaw=Z)`; order
  `T * Rz(yaw) * Rx(pitch) * Ry(roll) * S`.
- View space is left-handed (`GLM_FORCE_DEPTH_ZERO_TO_ONE`): `+Z` forward, NDC Z
  in `[0, 1]`. Camera projection flips Y for Vulkan NDC (`proj[1][1] *= -1`).
- Do not edit `.obj` files - Blender exports already use this convention.

---

## Development Workflow

1. **Think and plan.** Trace the closest existing feature; map every layer the
   change crosses; surface concerns and confirm the design with the user.
2. **Implement.** Break the work into waves (foundation -> core -> integration ->
   tests/docs) and fan out where files are disjoint. Wire every feature into the
   renderer and application immediately; expose errors directly.
3. **Test and verify** - gated by what you are about to do, non-negotiable:
   - **Before every commit:** build the live tree, 0 errors. A commit that does
     not build is never acceptable.
   - **Before every push:** `make check` all green, including reference-image
     regression, then start a session and drive it through the CLI over what the
     change touches (`neurus project new`, `scene list`, `object set`,
     `render set`, `history undo`, `snapshot capture`) - zero `VUID-` validation
     errors in the captured app output, no crashes or unexpected logs.
   - **Manual viewport checks:** interaction and resize testing are handled by
     the user.
   - **Mid-stream:** the suite is not required per commit, but run it after a
     large modification - a cross-system refactor, a new feature, anything
     crossing layers - rather than meeting the breakage at the push gate.
   - **When the change touches them:** verify every reference image with Python
     PIL (not empty, not uniform, not all-black/white) - never cheat yourself -
     and run `python scripts/extract_i18n.py --check` for any new user-visible
     string.
4. **Update docs.** Keep `docs/` and `.github/instructions/` current before
   committing.
5. **Commit** only after the user approves (unless told in advance). Message
   `feat(scope): what was built`, and add "No validation error, no unreasonable
   reference image, all tests passed." only when those checks actually ran - the
   trailer is evidence, not decoration. A commit resting on the build gate alone
   says what was verified instead.

Stop and wait for user verification after each phase.

---

## Routing Table

| I need... | Read |
|-----------|------|
| Coding rules (format, naming, RAII, errors, comments, shaders) | [conventions.instructions.md](.github/instructions/conventions.instructions.md), [docs/style/style.md](docs/style/style.md), [docs/style/conventions.md](docs/style/conventions.md) |
| Feature-development workflow | [development.instructions.md](.github/instructions/development.instructions.md) |
| Build/test commands, build trees, worktrees, graphify | [tips.instructions.md](.github/instructions/tips.instructions.md), [docs/build/build.md](docs/build/build.md), [docs/build/test.md](docs/build/test.md) |
| Web documentation (`make doc`, MkDocs, API reference, structure graph) | [docs/build/build.md](docs/build/build.md#web-documentation), [tips.instructions.md](.github/instructions/tips.instructions.md) |
| Encountered issues / pitfalls (symptom -> rule) | [lessons.instructions.md](.github/instructions/lessons.instructions.md) |
| Git, branch, worktree and commit conventions | [git-workflow.instructions.md](.github/instructions/git-workflow.instructions.md) |
| Agent roles, parallel lanes, racing prevention | [roles.instructions.md](.github/instructions/roles.instructions.md) |
| Architecture, layers, data flow, ownership, coordinates | [docs/architecture/overview.md](docs/architecture/overview.md), [layers.md](docs/architecture/layers.md), [data-flow.md](docs/architecture/data-flow.md), [ownership.md](docs/architecture/ownership.md), [coordinate-system.md](docs/core/coordinate-system.md) |
| Renderer: passes, graph, buffers, resources, shaders, temporal | [docs/render/overview.md](docs/render/overview.md) and its siblings |
| Editor: viewport, controllers, events, operations, gizmo, overlays | [docs/editor/overview.md](docs/editor/overview.md) and its siblings |
| UI: panels, items, models, i18n | [docs/ui/overview.md](docs/ui/overview.md) and its siblings |
| Core (UID, serialization, render config, logging), scene and assets | [docs/core/uid-and-resources.md](docs/core/uid-and-resources.md), [docs/scene/objects.md](docs/scene/objects.md), [docs/asset/data-resources.md](docs/asset/data-resources.md) |
| Scripting: embedded CPython, the `neurus` module, Script Console | [docs/script/overview.md](docs/script/overview.md) |
| Full indexes | [.github/instructions/README.md](.github/instructions/README.md), [docs/README.md](docs/README.md) |

Link direction is one-way: instructions and `AGENTS.md` may link to `docs/`;
`docs/` never links back.

---

## Working Agreements

- Only the **master** agent builds, runs tests and commits. Subagents edit only -
  they never invoke the build system, run `ctest` or start a Neurus session.
- Only **one** agent runs `ctest` or a Neurus session (the app plus its CLI) at a
  time - the binary and the session registry are shared.
- Never `git stash` while others are editing.
- **Push Neurus-Lib before Neurus.** If the `lib` submodule is modified (or its
  pointer moved), push Neurus-Lib and wait for its build/release workflow to
  succeed before pushing Neurus - Neurus CI downloads the prebuilt libs from that
  release, so pushing Neurus first breaks CI. See
  [git-workflow.instructions.md](.github/instructions/git-workflow.instructions.md).
- Keep changes minimal and localized; respect layer isolation. See
  [roles.instructions.md](.github/instructions/roles.instructions.md) for the
  full set.
- **Command/IPC/CLI targets stay Qt-free and Vulkan-free.** `neurus_command`,
  `neurus_ipc` and `neurus_cli` must not include Qt, Vulkan, Editor or UI
  headers. The IPC I/O thread never touches editor state; only the
  main-thread drain in `Application::NewFrameSignals` may execute a command.
  The CLI never constructs an `Editor`, and the session registry under
  `~/.neurus/sessions` is per-user and owner-only. See
  [docs/command/overview.md](docs/command/overview.md).
