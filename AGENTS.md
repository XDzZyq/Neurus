# AGENTS.md

Guidance for agentic coding in Neurus: a C++20 Vulkan-HPP 1.4 real-time renderer
built around strict layer isolation.

System design, architecture and style are **product documentation** under
[`docs/`](docs/README.md). This file is the entry point - hard rules, the
development workflow, and a routing table. It does not restate design prose;
route to `docs/` or `.github/instructions/` instead.

---

## Hard Rules

### Layer isolation is unidirectional

```
UI (Qt6) -> UIEvents (Qt signals) -> Editor -> EventQueue (typed events)
         -> Controllers -> Context -> Renderer
```

Each arrow is a **decoupled interface** (signals, typed events, context objects).
No layer knows the internals of the next.

- **Renderer** (`src/render/`): pure rendering service; owns all GPU resources;
  consumes read-only scene data; must never mutate application state; must never
  include Editor or UI headers.
- **Editor** (`src/editor/`): application logic and scene mutation; owns the
  Scene, `RenderConfig`, Controllers and the `ResourceManager`; communicates via
  Context, UIEvents and the typed EventQueue.
- **UI** (`src/ui/`): Qt6 Widgets presentation only; owns the native window and
  the surface; emits signals; must never include Renderer headers.
- **Asset** (`src/asset/`): Vulkan-free CPU-side asset loading (`MeshData`,
  `ImageData`, `PixelFormat`).
- **Scene** (`src/scene/`): Vulkan-free logical objects (Camera, Light, Mesh,
  Transform, overlays).

### Communication

Cross-layer traffic MUST go through exactly one of:

- **UIEvents** (Qt Signals/Slots) for UI <-> Editor.
- **EventQueue** (typed events) for Editor <-> Renderer.
- **Context objects** (`SceneContext`, `RenderContext`, `UIContext`) for
  read-only data queries.

Direct coupling across layers is forbidden. Controllers react to discrete typed
events - never poll per frame. Renderer never calls back into Editor.

### Vulkan ownership

- UI owns `VkSurfaceKHR` (created from the native window via
  `src/platform/PlatformSurface`).
- Renderer owns everything else: instance (shared via `QVulkanInstance`),
  device, queues, swapchain, pipelines, command pools and all GPU resources.
- No shared ownership of GPU resources across layers. GPU-resource classes
  `= delete` copy/assign and use `vk::raii` - never a raw `vkDestroy*`.

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
3. **Test and verify** - every wave, non-negotiable:
   - Build the live tree: 0 errors.
   - `make check`: all tests pass, including reference-image regression.
   - Launch `Neurus.exe`: zero `VUID-` validation errors, no crashes/unexpected
     logs, viewport looks correct, resize/interaction stays responsive.
   - Verify every reference image with Python PIL (not empty, not uniform, not
     all-black/white) - never cheat yourself.
   - `python scripts/extract_i18n.py --check`: catalogs complete and fresh.
4. **Update docs.** Keep `docs/` and `.github/instructions/` current before
   committing.
5. **Commit** only after the user approves (unless told in advance). Message
   `feat(scope): what was built`, and add "No validation error, no unreasonable
   reference image, all tests passed."

Stop and wait for user verification after each phase.

---

## Routing Table

| I need... | Read |
|-----------|------|
| Coding rules (format, naming, RAII, errors, comments, shaders) | [conventions.instructions.md](.github/instructions/conventions.instructions.md), [docs/style/style.md](docs/style/style.md), [docs/style/conventions.md](docs/style/conventions.md) |
| Feature-development workflow | [development.instructions.md](.github/instructions/development.instructions.md) |
| Build/test commands, build trees, worktrees, graphify | [tips.instructions.md](.github/instructions/tips.instructions.md), [docs/build/build.md](docs/build/build.md), [docs/build/test.md](docs/build/test.md) |
| Encountered issues / pitfalls (symptom -> rule) | [lessons.instructions.md](.github/instructions/lessons.instructions.md) |
| Git, branch, worktree and commit conventions | [git-workflow.instructions.md](.github/instructions/git-workflow.instructions.md) |
| Agent roles, parallel lanes, racing prevention | [roles.instructions.md](.github/instructions/roles.instructions.md) |
| Architecture, layers, data flow, ownership, coordinates | [docs/architecture/overview.md](docs/architecture/overview.md), [layers.md](docs/architecture/layers.md), [data-flow.md](docs/architecture/data-flow.md), [ownership.md](docs/architecture/ownership.md), [coordinate-system.md](docs/architecture/coordinate-system.md) |
| Renderer: passes, graph, buffers, resources, shaders, temporal | [docs/render/overview.md](docs/render/overview.md) and its siblings |
| Editor: viewport, controllers, events, operations, gizmo, overlays | [docs/editor/overview.md](docs/editor/overview.md) and its siblings |
| UI: panels, items, models, i18n | [docs/ui/overview.md](docs/ui/overview.md) and its siblings |
| Core (UID, serialization, render config, logging), scene and assets | [docs/core/uid-and-resources.md](docs/core/uid-and-resources.md), [docs/scene/objects.md](docs/scene/objects.md), [docs/asset/data-resources.md](docs/asset/data-resources.md) |
| Full indexes | [.github/instructions/README.md](.github/instructions/README.md), [docs/README.md](docs/README.md) |

Link direction is one-way: instructions and `AGENTS.md` may link to `docs/`;
`docs/` never links back.

---

## Working Agreements

- Only the **master** agent builds, runs tests and commits. Subagents edit only -
  they never invoke the build system, run `ctest` or launch `Neurus.exe`.
- Only **one** agent runs `ctest`/`Neurus.exe` at a time - the binary is locked.
- Never `git stash` while others are editing.
- Keep changes minimal and localized; respect layer isolation. See
  [roles.instructions.md](.github/instructions/roles.instructions.md) for the
  full set.
