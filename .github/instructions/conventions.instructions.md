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

## Naming

- Types/classes: PascalCase (`VulkanContext`, `Shader`).
- Public API functions/methods: PascalCase (`DrawFrame()`); internal helpers in
  `.cpp` may be camelCase - match the surrounding convention.
- Members: `m_` prefix for member variables.
- Constants: UPPER_SNAKE_CASE. Enums: PascalCase type, PascalCase or UPPER_CASE
  values.
- Files: PascalCase (`VulkanContext.h`).

## RAII (Non-Negotiable)

- Resource-owning classes initialize fully in the constructor and release in the
  destructor. No separate `Init()`/`Terminate()` two-phase setup.
- Classes owning GPU or OS resources `= delete` copy constructor and assignment.
- `std::unique_ptr`/`std::shared_ptr` for explicit ownership; raw pointers for
  non-owning references only.

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

Use the macros from `core/Log.h`; they inject a `[func:line]` prefix.

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

## Shader Conventions

- GLSL source lives in `res/shaders/`.
- SPIR-V is compiled at CMake configure time via glslangValidator and embedded as
  C headers in `${CMAKE_BINARY_DIR}/generated/shaders/`.
- Keep GLSL structs byte-for-byte compatible with their C++ counterparts; back
  them with `static_assert(sizeof(...))` and `offsetof` checks.
