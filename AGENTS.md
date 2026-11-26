# AGENTS.md

Guidance for agentic coding in Neurus: a C++20 Vulkan-HPP 1.4 real-time renderer
built around strict layer isolation.

Use the shared [neurus-develop skill](.agents/skills/neurus-develop/SKILL.md)
when implementing or reviewing changes to layer boundaries, editor operations
or GPU uploads. Detailed contributor rules live in the
[instructions index](.agents/instructions/README.md). System design and style
live in the [product documentation](docs/README.md).

---

## Hard Rules

### Layer isolation and ownership

- Application wires the layers. Editor owns scene mutation. UI and Script use
  public Editor contracts. Renderer consumes owned snapshots and typed requests.
- Keep controllers lean and event-driven. Submit fine-grained operations through
  `IOperationSink`. In production, only the operation system may use
  `OperationContext` and call `Operation::Apply`.
- Trace Mesh or RenderGraph before adding a GPU upload. Application owns the
  Vulkan device and surface. Renderer owns the swapchain and GPU resources.
- Singletons are forbidden for every stateful system, including resource
  management and script contexts. Give state an explicit owner and pass
  dependencies directly. The existing logging system is the sole exception.
- Keep command, IPC and CLI targets Qt-free and Vulkan-free. Execute session
  commands on the application main thread.

Follow the [architecture instructions](.agents/instructions/architecture.instructions.md)
for permitted interfaces, controller dependencies, replay authority and GPU ownership.

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
4. **Update docs.** Keep `docs/` and `.agents/instructions/` current before
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
| Coding rules (format, naming, RAII, errors, comments, shaders) | [conventions.instructions.md](.agents/instructions/conventions.instructions.md), [docs/style/style.md](docs/style/style.md), [docs/style/conventions.md](docs/style/conventions.md) |
| Feature-development workflow | [development.instructions.md](.agents/instructions/development.instructions.md) |
| Build/test commands, build trees, worktrees, graphify | [tips.instructions.md](.agents/instructions/tips.instructions.md), [docs/build/build.md](docs/build/build.md), [docs/build/test.md](docs/build/test.md) |
| Web documentation (`make doc`, MkDocs, API reference, structure graph) | [docs/build/build.md](docs/build/build.md#web-documentation), [tips.instructions.md](.agents/instructions/tips.instructions.md) |
| Encountered issues / pitfalls (symptom -> rule) | [lessons.instructions.md](.agents/instructions/lessons.instructions.md) |
| Git, branch, worktree and commit conventions | [git-workflow.instructions.md](.agents/instructions/git-workflow.instructions.md) |
| Agent roles, parallel lanes, racing prevention | [roles.instructions.md](.agents/instructions/roles.instructions.md) |
| Architecture, layers, data flow, ownership, coordinates | [docs/architecture/overview.md](docs/architecture/overview.md), [layers.md](docs/architecture/layers.md), [data-flow.md](docs/architecture/data-flow.md), [ownership.md](docs/architecture/ownership.md), [coordinate-system.md](docs/core/coordinate-system.md) |
| Renderer: passes, graph, buffers, resources, shaders, temporal | [docs/render/overview.md](docs/render/overview.md) and its siblings |
| Editor: viewport, controllers, events, operations, gizmo, overlays | [docs/editor/overview.md](docs/editor/overview.md) and its siblings |
| UI: panels, items, models, i18n | [docs/ui/overview.md](docs/ui/overview.md) and its siblings |
| Core (UID, serialization, render config, logging), scene and assets | [docs/core/uid-and-resources.md](docs/core/uid-and-resources.md), [docs/scene/objects.md](docs/scene/objects.md), [docs/asset/data-resources.md](docs/asset/data-resources.md) |
| Scripting: embedded CPython, the `neurus` module, Script Console | [docs/script/overview.md](docs/script/overview.md) |
| Full indexes | [.agents/instructions/README.md](.agents/instructions/README.md), [docs/README.md](docs/README.md) |

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
  [git-workflow.instructions.md](.agents/instructions/git-workflow.instructions.md).
- Keep changes minimal and localized; respect layer isolation. See
  [roles.instructions.md](.agents/instructions/roles.instructions.md) for the
  full set.
