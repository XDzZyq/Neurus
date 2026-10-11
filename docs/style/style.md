# Style

Neurus code follows the Blender C/C++ guidelines, built on C++20 and Vulkan-HPP
RAII throughout.

## Language and Standard

- C++20 (see `CMakeLists.txt`)
- Vulkan-HPP `vk::raii` namespace for automatic RAII resource management

## Formatting (Blender C/C++ Guidelines)

- Indentation: tabs
- Braces: Allman style (opening brace on new line)
- Keep lines <= 120 characters where practical

## Includes

- Local project headers before system headers
- Group: local, third-party (Vulkan, Qt, GLM), STL
- Use `#pragma once` in headers

## Naming

- Types / classes: PascalCase (for example `VulkanContext`, `Shader`)
- Methods: PascalCase (for example `DrawFrame()`, `AcquireNextImage()`)
- Members: prefixes as needed (`m_` for member variables, or match local style)
- Functions in .cpp: camelCase if local; match surrounding convention
- Enums: PascalCase for types, PascalCase or UPPER_CASE for values
- Files: PascalCase for class files (for example `VulkanContext.h`)
- Constants: UPPER_SNAKE_CASE
- Functions: PascalCase for public API; camelCase for internals

## RAII (Non-Negotiable)

- All resource-owning classes initialize fully in the constructor and release in
  the destructor. No separate `Init()`/`Terminate()` methods.
- Classes owning GPU/OS resources: `= delete` copy constructor and assignment.
- Use `std::unique_ptr`/`std::shared_ptr` for explicit ownership.
- Raw pointers for non-owning references only.

## Const Correctness

- Use `const` for non-mutating methods and references.
- Renderer receives `const` scene/context data.
