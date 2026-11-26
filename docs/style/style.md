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

| New declaration | Spelling | Neurus example |
| --- | --- | --- |
| Type and its class files | PascalCase | `VulkanContext`, `VulkanContext.hpp`, `VulkanContext.cpp` |
| Acronym in a type, function or file name | Upper case | `ShaderGPU`, `CPUPorts.hpp`, `ValidateCPUGraph`, `UID` |
| Public or private member function | PascalCase at either access level | Public `DeferredRenderer::DrawFrame`, private `DeferredRenderer::CompileDocument` |
| File-local free function | lowerCamelCase | `vkFormatToPixelFormat` in `Screenshot.cpp` |
| Private instance field | Owner prefix below, followed by lowerCamelCase. The default is `m_`. | `Editor::ed_eventBus`, `IpcServer::m_impl` |
| Named constant | UPPER_SNAKE_CASE | `RenderCache::MAX_SHADOW_LAYERS` |
| New enum type and enumerators | PascalCase | `FrameWork::Overlay` |

The following prefixes identify the class that declares a field. For example,
`Application::app_renderer` points to the renderer, and `Editor::ed_ctx`
holds a controller context. Use the same owner prefix for handles, borrowed
references, flags and counters.

| Declaring class | Prefix | Example |
| --- | --- | --- |
| `Application`, `HeadlessApplication` | `app_` | `app_editor` |
| `DeferredRenderer`, `Swapchain` | `r_` | `r_device` |
| `Editor` | `ed_` | `ed_operations` |
| `VulkanContext` | `ctx_` | `ctx_instance` |
| `UIManager` | `win_` | `win_dockManager` |
| `OperationManager` | `op_` | `op_undo` |
| `CommandDispatcher`, `CommandCatalog` | `cmd_` | `cmd_pending`, `cmd_descriptors` |
| `ShaderData`, `RenderShader`, `ComputeShader` | `sh_` | `sh_stages`, `sh_vertPath` |
| `ScriptEngine`, `ScriptEngine::Impl` | `sc_` | `sc_impl`, `sc_pending` |

Apply the table to direct private instance fields and to the protected
storage `ShaderData` shares with its derived shader classes. Helpers in the same
directory and nested value types have their own declarations. Public data
contracts such as `RenderContext::frameIndex` keep their established spelling.
Check archive names before renaming storage. For example,
`RenderConfig::r_sampling_count` is also a saved project key through
`CEREAL_NVP`. When the saved spelling differs from the member, bind it
explicitly, as in `cereal::make_nvp("m_vertPath", sh_vertPath)`.

When extending an existing renderer feature, give a new private method a
PascalCase name, following `CompileDocument`. For a new value in an existing
enum, follow its siblings: `FrameWork` uses `Overlay`, while `Pass::PassType`
uses `POST_FX`. Preserve existing method names used by callers, virtual
overrides, Qt properties and bindings when adding a member.

## RAII (Non-Negotiable)

- All resource-owning classes initialize fully in the constructor and release in
  the destructor. No separate `Init()`/`Terminate()` methods.
- Classes owning GPU/OS resources: `= delete` copy constructor and assignment.
- Use `std::unique_ptr`/`std::shared_ptr` for explicit ownership.
- Raw pointers for non-owning references only.

## Const Correctness

- Use `const` for non-mutating methods and references.
- Renderer receives `const` scene/context data.
