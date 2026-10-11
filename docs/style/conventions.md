# Conventions

Beyond formatting, Neurus enforces rules for error handling, logging, comments,
source-file encoding and shader assets.

## Error Handling (Karpathy Guidelines)

- **Never use try-catch to hide or swallow errors.** Every catch block MUST
  either re-throw (with an enhanced message) or print the error. Silent
  `catch (...) {}` is forbidden.
- Use `catch (const std::exception& e)` and `NEURUS_ERR(e.what())` when
  recovery is legitimate (for example swapchain out-of-date, acquire failure).
- `assert()` for precondition violations (debug-only). Do not use try-catch for
  logic flow.
- Handle `VK_ERROR_DEVICE_LOST` and `VK_ERROR_OUT_OF_DATE_KHR` as normal
  lifecycle events - print the error, then recover.
- Early return on invalid state after printing the reason.
- Debug builds enable Vulkan validation layers.

## Debug Printing (Karpathy Guidelines)

Use the `NEURUS_LOG` / `NEURUS_ERR` macros from `core/Log.h` - they
automatically inject a `[func:line]` prefix for traceability:

```cpp
NEURUS_LOG("[Swapchain] " << extent.width << "x" << extent.height);
NEURUS_ERR("Texture::createFromPixelData failed: " << e.what());
```

- `NEURUS_LOG` - debug-only info output (`std::cout`). Compiles to nothing in
  Release. Use for constructor diagnostics, key parameters, lifecycle events.
- `NEURUS_ERR` - always-on error output (`std::cerr`). Active in all builds.
  Use in catch blocks and unrecoverable error paths.
- Always print debug information in constructors of large classes (Swapchain,
  DeferredRenderer, etc.) - dimension, image count, format, key parameters.

## Comments and Documentation

- Doxygen-style on all public APIs:

  ```cpp
  /**
   * @brief Short one-line description.
   * @param ctx Read-only rendering context.
   * @return true if frame was drawn successfully.
   * @note Caller must ensure the surface is valid.
   */
  ```

- Inline comments follow Blender C/C++ style: `// comment` (space after //)
- Non-trivial sections get `// --- Section Name ---` separators
- Use a normal hyphen (`-`) instead of an em dash or en dash.

## Non-ASCII Hygiene

- Never use PowerShell `Set-Content -Force` on source files. On Windows
  PowerShell 5.1 it defaults to UTF-16, corrupting em dashes, arrows and other
  non-ASCII characters into replacement characters.
- Use the `write` or `edit` tools (OpenCode native tools) for all file
  modifications - they preserve UTF-8 encoding correctly.
- If a PowerShell file write is unavoidable, use:

  ```powershell
  [System.IO.File]::WriteAllText($path, $content, [System.Text.UTF8Encoding]::new($false))
  ```

- Before committing, scan staged changes for corruption:

  ```powershell
  git diff --cached --name-only | ForEach-Object {
    $bytes = [System.IO.File]::ReadAllBytes($_)
    for ($i = 0; $i -lt $bytes.Length - 2; $i++) {
      if ($bytes[$i] -eq 0xEF -and $bytes[$i+1] -eq 0xBF -and $bytes[$i+2] -eq 0xBD) {
        throw "REPLACEMENT CHARACTER in $_ at byte $i"
      }
    }
  }
  ```

## Shader Conventions

- GLSL source: `res/shaders/`
- SPIR-V compiled at CMake configure time via glslangValidator
- Embedded as C headers in `${CMAKE_BINARY_DIR}/generated/shaders/`
