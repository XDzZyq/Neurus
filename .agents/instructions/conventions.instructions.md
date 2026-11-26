# Conventions

Daily coding rules for Neurus. The full style guide lives in
[docs/style/style.md](../../docs/style/style.md) and
[docs/style/conventions.md](../../docs/style/conventions.md); this file is the
short working reference.

## Language and Standard

- C++20 (see `CMakeLists.txt`).
- Vulkan-HPP `vk::raii` for automatic resource management - never a raw
  `vkDestroy*` call.

## Formatting (Blender C/C++ Guidelines)

- Indentation: tabs.
- Braces: Allman style (opening brace on its own line).
- Keep lines <= 120 characters where practical.
- Do not run clang-format on project code unless explicitly requested.

## Includes

- `#pragma once` in headers.
- Local project headers before system headers.
- Group: local, third-party (Vulkan, Qt, GLM), STL.

## File organization

Place new code beside the closest implementation in its owning layer. Where
callers use several implementations through one interface, keep the base small
and put specialized behavior in the concrete types. Keep a helper used by one
implementation in its `.cpp`. Extract a named helper when it has a distinct
responsibility shared by callers, and keep it with the family that uses it.

| Family | Where the next addition belongs |
| --- | --- |
| Render passes | [Pass.hpp](../../src/render/passes/Pass.hpp) defines the common interface. Add a concrete pass beside [GeometryPass.hpp](../../src/render/passes/GeometryPass.hpp) in `src/render/passes/`. |
| Editor events | Group related typed payloads in a domain header such as [SceneEvents.hpp](../../src/editor/events/SceneEvents.hpp). Plain event structs need no base class. |
| Undo operations | [Operation.hpp](../../src/editor/operations/Operation.hpp) defines the replay contract. Group related concrete edits in a domain header such as [SceneOperations.hpp](../../src/editor/operations/SceneOperations.hpp). |
| CLI commands | [SubCommand.hpp](../../src/cli/SubCommand.hpp) defines the CLI interface. Each command family has a sibling implementation under `src/cli/subcommands/`, such as [SceneCommand.hpp](../../src/cli/subcommands/SceneCommand.hpp). |
| Project components | Keep persistence with the layer that owns the data. Asset components include [SceneComponent.hpp](../../src/asset/components/SceneComponent.hpp). Editor-owned history lives in [HistoryComponent.hpp](../../src/editor/components/HistoryComponent.hpp). |
| Type registrations | Add serialization registrations to the owning family's `registrations/` directory. For an undo operation, extend [OperationRegistration.cpp](../../src/editor/operations/registrations/OperationRegistration.cpp). |

## Naming

- Use PascalCase for types, class filenames and member functions, including
  private methods. Use lowerCamelCase for file-local free functions. Keep
  acronyms upper case in type, function and file names (`ShaderGPU`,
  `CPUPorts.hpp`, `ValidateCPUGraph`).
- Prefix direct private instance fields by declaring class. Use `r_` in
  DeferredRenderer and Swapchain, `ed_` in Editor, `ctx_` in VulkanContext,
  `app_` in Application and HeadlessApplication, and `win_` in UIManager.
  Use `op_` in OperationManager, `cmd_` in CommandDispatcher and CommandCatalog,
  `sh_` in ShaderData (including its protected fields), RenderShader and
  ComputeShader, and `sc_` in ScriptEngine and its private implementation.
  Use lowerCamelCase after the prefix. The default for other classes is `m_`.
- Use UPPER_SNAKE_CASE for constants and PascalCase for new enums and their
  values. Extend an existing enum using its current spelling.
- Preserve exposed API and serialized names during private member renames.
  See the [style guide's naming table](../../docs/style/style.md#naming) for
  examples, scope and compatibility rules.

## RAII (Non-Negotiable)

- Resource-owning classes initialize fully in the constructor and release in the
  destructor. No separate `Init()`/`Terminate()` two-phase setup.
- Classes owning GPU or OS resources `= delete` copy constructor and assignment.
- Identity types - every `UID`-derived class (`ObjectID`, `Camera`, `Mesh`,
  `Light`, `Environment`, `Sprite`, `DebugLine`/`DebugMesh`/`DebugPoints`,
  `Scene`, `MeshData`, `ImageData`, `ShaderData`) - `= delete` **all four**
  special members: copy construction, copy assignment, move construction and
  move assignment. Never write `= default` on a move operation of one of these:
  the base is non-movable, so the defaulted function is implicitly deleted and
  clang reports it under `-Wdefaulted-function-deleted` once per translation
  unit. Duplicate an object through the pool - `ResourceManager::Load<T>` plus
  an explicit field copy (`Editor::DuplicateObject`) - so the copy gets its own
  id. `test/scene/test_uid.cpp` asserts the four traits for every identity type.
- `std::unique_ptr`/`std::shared_ptr` for explicit ownership; raw pointers for
  non-owning references only.

## Core Layer

- `src/core/` holds generic, layer-agnostic primitives only: identity (`UID`),
  resource lookup (`IResourceLookup`), traversal (`Graph`, `GraphMorphism`), logging,
  serialization helpers, `RenderConfig`.
- A concrete document or data type that names this product's features belongs
  to the layer that owns it. A render graph document lives in `asset/data/`,
  not in `core/`: if a type mentions a pass kind, an output tag or a scene
  concept, it is not core.
- `core/` never includes asset, scene, editor, render or UI headers.

## CPU and Render Boundary

- Shared renderer frame/context/profiling values, overlay payloads and typed render
  requests belong under `render_api/` in `neurus_render_api`. It contains no Qt,
  Vulkan, Editor or Renderer implementation headers. UIContext stays in UI;
  Application assembles it from Editor getters.
- `ShaderData` belongs under `asset/data/`; stage values and IR belong under
  `asset/data/shader/`. It owns source IO and path resolution. CPU parsing,
  generation, compilation and high-level RenderShader/ComputeShader belong under
  `shader/` in `neurus_shader`.
- Keep authored `RenderGraphData` in the resource pool. `proj_render_graph` saves
  its active UID. Publish an unpooled clone of the document through
  `MakeCompileRenderGraphRequest`, which records each node's selection as a plain
  `PublishedShader {uid, version}` and clears the clone's `PassShaderRef`, so the
  render thread never reads pooled `ShaderData`. Null selection uses a built-in.
  CPU `MakeUploadShaderRequest` copies SPIR-V and reflection metadata into owned
  typed requests before FIFO graph mapping; it carries no ports. UploadManager
  constructs ShaderGPU; RenderCache owns/indexes it; passes consume modules and own
  pipelines. The uploader's GPU-only guard requires the published version, type
  and stage set to match the resident upload; the CPU port, descriptor and
  push-constant contracts are checked before publication. A changed version needs
  a matching republished request. Resource references and fences preserve lifetime.
  Persist authored UID references only; defaults follow recipes through CPU
  preparation/upload/cache outside the pool.
- Asset data and persistence must not depend on Shader tooling. Shader cereal
  registrations belong under `shader/registrations/` and are force-linked by
  ApplicationCore; generic persistence uses its resource callback. Preserve
  existing shader serialization reload/recompile behavior.
- Editor light requests carry scene-light snapshots; GPU layout conversion
  belongs to the Renderer. Never include UploadManager just to prepare a request.
- Asset and scene targets must not link Renderer. Editor-specific persistence
  adapters live under `editor/components/`; Qt UIEvents and input translations live
  under `ui/`.
- Run `python scripts/check_layer_boundaries.py` when changing layer includes;
  `neurus_cpu_test` verifies CPU document/shader/Editor behavior without GPU or
  Qt linkage. See [Testing](../../docs/build/test.md#cpu-boundary-tests).
- Application wires concrete components. For UI and Script imports from Editor,
  use the consumer's public input, event or view contracts listed in
  `scripts/check_layer_boundaries.py`. Use the [data-flow guide](../../docs/architecture/data-flow.md)
  to choose between posting an editor event, publishing a frame, posting a GPU
  request or sending a render-thread control.

## Const Correctness

- Mark non-mutating methods and references `const`.
- Renderer receives `const` scene/context data.

## Error Handling

- Never hide or swallow errors. Every `catch` block either re-throws with an
  enhanced message or prints via `NEURUS_ERR`. Silent `catch (...) {}` is
  forbidden.
- `assert()` for precondition violations (debug-only); do not use exceptions for
  control flow.
- Treat `VK_ERROR_DEVICE_LOST` and `VK_ERROR_OUT_OF_DATE_KHR` as normal
  lifecycle events: print, then recover.
- Early return on invalid state after printing the reason.

## Debug Printing

Use the macros from `core/Log.hpp`; they inject a `[func:line]` prefix.

```cpp
NEURUS_LOG("[Swapchain] " << extent.width << "x" << extent.height);
NEURUS_ERR("Texture::createFromPixelData failed: " << e.what());
```

- `NEURUS_LOG` - debug-only info (`std::cout`), compiles to nothing in Release.
  Print key parameters in the constructors of large classes.
- `NEURUS_ERR` - always-on error (`std::cerr`), active in all builds.

## Comments and Documentation

- Doxygen `@brief`/`@param`/`@return`/`@note` on public APIs. Keep simple
  functions to a line or two and complex ones to a few; describe the entity
  itself, not other layers.
- Inline comments follow Blender style (`// comment`, space after `//`) and
  explain non-obvious intent ("why"), never restate the code.
- Non-trivial sections use `// --- Section Name ---` separators.
- Write a normal hyphen (`-`), never an em/en dash or an arrow.

## Non-ASCII Hygiene

- Never use PowerShell `Set-Content -Force` on source files - on Windows
  PowerShell 5.1 it defaults to UTF-16 and corrupts non-ASCII into replacement
  characters.
- Use the native `write`/`edit` tools for all file changes. If a PowerShell write
  is unavoidable, use
  `[System.IO.File]::WriteAllText($path, $content, [System.Text.UTF8Encoding]::new($false))`.
- Before committing, scan staged changes for the replacement-character byte
  sequence (`EF BF BD`) and fail if any file contains it.

## Platform-specific code

- Keep OS APIs and `#ifdef _WIN32` / `#ifdef __APPLE__` / `#ifdef __linux__`
  inside `src/platform/win/`, `src/platform/mac/` and `src/platform/linux/`,
  selected by `src/platform/CMakeLists.txt`.
- A caller in app, editor, UI, command, IPC, CLI, script or tests uses a free
  function from a `Platform*.hpp` header instead of branching on the OS.
- An ifdef in a test is acceptable only when the expected value itself differs by
  platform (for example the executable name or the home environment variable).
- When a new platform primitive is needed, add it to the shared header and every
  implementation in the same change; do not leave one platform with a stub that
  silently succeeds.
- Linux is headless: `linux/LinuxSurface.cpp` exists only so the platform target
  links, and `CreateSurface` throws. Never call it from a Linux code path; the
  headless application builds its own instance and never creates a surface.

## Shader Conventions

- GLSL source lives in `res/shaders/`.
- SPIR-V is compiled at CMake configure time via glslangValidator and embedded as
  C headers in `${CMAKE_BINARY_DIR}/generated/shaders/`.
- Keep GLSL structs byte-for-byte compatible with their C++ counterparts; back
  them with `static_assert(sizeof(...))` and `offsetof` checks.
