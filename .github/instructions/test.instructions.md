# Testing Standards & Roles

## Overview

Neurus uses **Google Test** for both GPU-dependent and non-GPU unit tests.
GPU tests require a Vulkan 1.4-capable device and are excluded from CI;
non-GPU tests (editor layer, events) run on every CI push.

## Test Fixture Hierarchy

### VulkanTestShared (base class for ALL GPU tests)

Location: `test/shared/TestVulkanShared.h`

Responsibilities:
- Create `vk::raii::Instance` (with surface extensions + debug-utils in Debug)
- Enumerate physical devices, prefer discrete GPU
- Find graphics queue family
- Create `vk::raii::Device` + `vk::Queue`
- Create `vk::raii::CommandPool` + one-shot command buffers
- Own shared `UploadManager` (`m_uploadManager`) for mesh/light/environment uploads
- Provide helpers: `BeginCmd()`, `EndSubmitWait(cmd)`, `ResolveAssetPath()`

```cpp
class VulkanTestShared : public ::testing::Test
{
protected:
    bool HasVulkan() const;
    vk::raii::PhysicalDevice& PhysicalDevice();
    vk::raii::CommandBuffer& BeginCmd();       // Returns a begun one-shot cmd buffer
    void EndSubmitWait(vk::raii::CommandBuffer& cmd);  // Submits + waitIdle
    static std::string ResolveAssetPath(const char* assetRelative);

    bool m_hasVulkan = false;
    std::unique_ptr<vk::raii::Device> m_device;
    vk::Queue m_queue = nullptr;
    uint32_t m_graphicsQueueFamily = 0;
    std::unique_ptr<UploadManager> m_uploadManager;  // Shared upload service
    // ... (full state in header)
};
```

Inheriting tests MUST:
- Call `VulkanTestShared::SetUp()` first in their own `SetUp()` override
- Check `if (!m_hasVulkan) return;` after the base `SetUp()` call
- Call `VulkanTestShared::TearDown()` in their `TearDown()` override
- Use `PhysicalDevice()` for format/limit queries
- Use `BeginCmd()` / `EndSubmitWait(cmd)` for all GPU command sequences

### Override SetUp for Special Requirements

Some tests need extensions the base class doesn't enable. Override `SetUp`
completely and re-bootstrap with your extensions:

```cpp
void SetUp() override
{
    if (!HasVulkan()) return;  // May be true from parent
    // ... custom Instance/Device creation with needed extensions ...
}
```

See `test_scene_wiring.cpp` for an example that enables `VK_KHR_swapchain`.

### Shared Static Helpers

In addition to the protected member methods above, `VulkanTestShared` provides
static helper functions/structs for common test operations. These eliminate
duplicated code across test files:

| Helper | Signature | What it does |
|--------|-----------|-------------|
| `TestVertex` struct | `posX/Y/Z, nrmX/Y/Z, uvX/uvY` | 32-byte vertex matching `GeometryPass::BufferLayout` (3 pos + 3 normal + 2 UV floats) |
| `HalfToFloat(uint16_t)` | `static float` | IEEE 754 half-precision (16-bit) to single-precision (32-bit) float conversion |
| `FindMemoryType(pd, typeBits, required)` | `static uint32_t` | Memory type index lookup matching type bits and property flags for staging/buffer allocation |
| `TransitionGbufferToColorAttachment(am, fixture)` | `static void` | Transitions all 4 color attachments (Position/Normal/Albedo/MetallicRoughness) to `eColorAttachmentOptimal` and Depth to `eDepthStencilAttachmentOptimal` |
| `ComputeCameraUBO(Camera&)` | `static CameraUBOData` | Computes view and view-projection matrices from a `Camera` object |
| `MakeTestCamera(w, h)` | `static CameraUBOData` | Creates a default 60° FOV camera at `(0,0,2)` looking at origin with given aspect ratio |
| `TestTriangle()` | `static pair<vector<TestVertex>, vector<uint32_t>>` | Returns a 3-vertex triangle (XY plane, facing +Z) + 3 indices, suitable for quick geometry tests |
| `ReadbackHdrOutput(device, pd, queue, qfi, am, w, h)` | `static vector<float>` | Reads back HDRColor RGBA16F attachment to CPU; handles layout transition, staging copy, and half→float conversion |
| `PublishSceneCamera(cache, scene)` / `(cache, editorContext)` | `static void` | Writes the scene's active camera into `RenderCache`'s shared `CameraGPU`, standing in for `DeferredRenderer::recordFrame()`. No-op when the scene has no active camera |

### Publishing the camera before recording a pass

`CameraGPU` and `DebugCache` belong to `RenderCache`, and the **frame driver**
(`DeferredRenderer::recordFrame()`) writes them once per frame *before* any pass
records. `GeometryPass` and `DebugPass` only bind them, and both return early from
`Record()` while `RenderCache::GetCameraGPU().IsValid()` is false.

A test that drives a pass directly *is* the frame driver, so it must publish first:

```cpp
VulkanTestShared::PublishSceneCamera(*m_renderCache, ctx.editor);
m_geometryPass->Record(*cmd, *m_renderCache, ctx);
```

Forget it and the pass records nothing — no validation error, just an empty
attachment and a reference-image diff that points nowhere near the cause.
`LightingPass` and `SSAOPass` read the `Camera` out of the context directly and need
no publish.

Usage example — replace 15+ lines of manual G-Buffer transitions with one call:

```cpp
// Instead of manually transitioning each attachment:
VulkanTestShared::TransitionGbufferToColorAttachment(*m_renderCache, *this);
```

All helpers are defined in `test/shared/TestVulkanShared.h`. Tests that need a
helper not yet available should add it to the shared header rather than
duplicating it in the local test file.

## Writing a GPU Test

### Pattern (established in this session)

1. **Include** the shared fixture header and renderer headers
2. **Inherit** from `VulkanTestShared`
3. **Construct** renderer components (RenderCache, passes, etc.) in `SetUp()`
4. **Test body**:
   a. Load assets (OBJ meshes via `ResolveAssetPath()`)
   b. Create scene objects (Camera, Light, Mesh)
   c. Upload vertex/index buffers with staging → device-local
   d. Record and submit geometry pass
   e. Upload per-frame data (light SSBO, camera UBO)
   f. Record and submit lighting/compute pass
   g. Capture or verify output

```cpp
#include <gtest/gtest.h>
#include "shared/TestVulkanShared.h"
#include "render/passes/RenderCache.h"
#include "render/GeometryPass.h"
// ... other includes ...

class MyFeatureTest : public VulkanTestShared { /* ... */ };

TEST_F(MyFeatureTest, DoesWhatItShould)
{
    if (!m_hasVulkan) GTEST_SKIP() << "No Vulkan GPU.";
    // ... test logic ...
}
```

## Coordinate Space Conventions for Test Scenes

| Quantity | Space | Format | Notes |
|----------|-------|--------|-------|
| Position attachment | World-space | `R16G16B16A16_SFLOAT` | w=1.0 for rendered pixels, w=0.0 for clear |
| Normal attachment | View-space | `R16G16B16A16_SFLOAT` | w=0.0. Remap `(val+1)*0.5` when saving to PNG |
| Camera position | World-space | `vec3` | Push constant to compute shader |
| Light positions | World-space | `vec3` | In PointLightGpu SSBO |
| View matrix | Camera→World | `mat4` | `glm::lookAt()`, stored in UBO + push constant |
| Projection | Clip-space | `mat4` | `glm::perspective()` (may need depth-range fix) |

### Critical: Object Scale and Light Placement

This was the single most impactful debugging lesson from the deferred shading test:

**Object scale matters.** An OBJ mesh at unit scale (radius ≈ 1) seen from
distance 5 at 60° FOV fills roughly 36% of the viewport. If smaller coverage
is desired, scale vertex positions:
```cpp
v.posX = s[0] * 0.25f;  // Scale to 25%
```

**Light placement matters.** Inverse-square attenuation (`1/d²`) is aggressive:
- Light at distance 2 → attenuation = 1/4 = 25%
- Light at distance 3 → attenuation = 1/9 = 11%
- Light at distance 5 → attenuation = 1/25 = 4%

For visible lighting with typical power values (10-100), keep lights within
2-4 units of the geometry surface. A light at `(3,3,3)` with the sphere at
origin gives distance ≈ 5.2; moving it to `(2,2,2)` gives distance ≈ 3.5.

**Camera position** relative to geometry also changes what portion of the
scene is visible. A camera at `(0,2,5)` looking at origin sees the sphere
from above-front; the visible hemisphere has negative world-space z.

## Sun Shadow Test Patterns

### Depth Math Validation (test_sun_shadow_depth.cpp)

Follows the `test_shadow_cubemap.cpp` depth validation pattern for orthographic
projection. Key differences from point light cubemap tests:

- **Projection**: Orthographic (`glm::ortho()`) with configurable frustum planes.
  No perspective division needed; depth is linear in world-space z.
- **Depth range**: NDC Z in `[0, 1]` (Vulkan convention). Requires
  `GLM_FORCE_DEPTH_ZERO_TO_ONE` to be defined before GLM includes.
- **No distance/farPlane division**: Unlike cubemap depth which computes
  `gl_FragCoord.z = distance / farPlane`, orthographic depth from
  `glm::ortho()` maps world-space z directly to NDC [0,1] via the
  projection matrix. No manual depth normalization is required in the
  vertex shader.
- **Push constant**: `SunShadowPushConstants` struct carries `mat4 lightViewProj`
  (view × projection matrix). View matrix is `glm::lookAt()` from light
  position/direction; projection is `glm::ortho()`.
- **Shadow map**: 2D `VK_FORMAT_D32_SFLOAT`, 2048×2048 resolution, accessed
  via `RenderCache::GetShadowMap(lightUID, LightType::SUNLIGHT)`.

**Test pattern**: Place occluder geometry (e.g. a quad at y=-5, 5 units forward
from the eye at y=-10) filling the ortho field. Compute expected depth value
at the occluder's pixel using the orthographic projection formula:
`Vz = world_y - eye_y = -5 - (-10) = 5` (view-space, left-handed),
`ndc = (Vz - near) / (far - near) = (5 - (-10)) / (30 - (-10)) = 15/40 = 0.375`.
The eye distance (`sun_depth_range = 10`) is decoupled from the far plane
(`sun_shadow_far = 30`) so geometry sits at mid-depth instead of the far
boundary. The fragment shader writes `gl_FragCoord.z` (NDC depth). Read back
the shadow map depth buffer, verify that occluder pixels have the expected
depth and background pixels have the clear value (1.0).

### Sun Shadow Intensity Eval (test_sun_shadow_intensity.cpp)

Validates that `SunShadowIntensityEval` compute shader correctly samples
a `sampler2DShadow` and writes per-pixel shadow intensity to the
`ShadowIntensity` layered array.

- **Descriptor binding**: `sampler2DShadow` at binding 0, storage image
  (`image2D`, `r8`) at binding 1. Separate descriptor set layout from
  the point light cubemap pipeline within `ShadowIntensityPass`.
- **PCF sampling**: Percentage-closer filtering with UV offset kernel.
  Supports `HARD`, `SOFT_PCF_16`, and `SOFT_PCF_64` shadow modes.
- **Shadow comparison**: Uses `texture(sampler2DShadow, ...)` with
  orthographic depth, no cubemap-specific direction logic.
- **Output**: Single `R8_UNORM` value per pixel written to the light's
  assigned layer in the `ShadowIntensity` 2D array.

**Test pattern**: Render occluder geometry into a 2D orthographic shadow map
via `SunShadowDepthPass`. Then run `SunShadowIntensityEval` compute shader
over a full-screen quad. Read back the shadow intensity layer and verify:
- Pixels behind the occluder have intensity > 0 (shadowed).
- Pixels not behind the occluder have intensity = 0 (lit).
- Intensity values are within [0, 1] range (R8_UNORM).

### Extended Deferred Test (test_deferred_shading.cpp)

The deferred shading integration test includes a sun light alongside point
lights. Additional attachments/checks:

- **Sun light SSBO**: `SunLightGpu` struct at LightingPass binding 6, with
  `direction`, `color`, `intensity`, and shadow parameters.
- **Shadow intensity layer**: Verify the sun light's assigned layer in the
  `ShadowIntensity` array is populated correctly.
- **HDRColor**: Should show visible sun light contribution (directional
  lighting with shadow attenuation on the lit side).

### Mandatory Verification Steps

**Python PIL verification (REQUIRED before committing any reference images)**:

Every reference image must be verified with Python before commit. This is not
optional:

```python
from PIL import Image
from collections import Counter

for name in ["Position", "Normal", "Albedo", "MetallicRoughness", "HDRColor"]:
    img = Image.open(f"test/render/reference/deferred/{name}.png")
    pixels = list(img.getdata())
    unique = Counter(pixels)
    non_black = sum(1 for p in pixels if p != (0, 0, 0, 0) and p != (0, 0, 0, 255))

    print(f"{name}: {len(unique)} unique colors, {non_black} non-black pixels")

    # Fail if only 1 unique value (uniform output = broken rendering)
    if len(unique) <= 1:
        raise ValueError(f"{name}: UNIFORM OUTPUT - lighting or geometry likely broken")

    # Fail if zero non-black pixels for geometry passes
    if name in ("Position", "Normal", "Albedo") and non_black == 0:
        raise ValueError(f"{name}: ALL BLACK - geometry not rendered")
```

**VUID check (REQUIRED before committing any GPU test or renderer change)**:

Run the test executable and verify zero `VUID-` violations in output:

```bash
# Run the affected test suite
./build/Debug/neurus_test.exe --gtest_filter="SunShadow*:DeferredShading*" 2>&1 > test_output.txt

# Verify zero VUID violations
grep -c "VUID-" test_output.txt
# Expected: 0

# Also check the main application
$output = & "build/Debug/Neurus.exe" 2>&1; Start-Sleep -Seconds 3
if ($output -match "VUID-") { Write-Host "VALIDATION ERROR DETECTED" }
```

Never skip this check. Validation errors that go unnoticed in the IDE's debug
output window will still crash the application on other machines or driver
versions.

## Reference-Image Regression Testing

### Pattern (first-run → second-run comparison)

The deferred shading test (`test_deferred_shading.cpp`) establishes this
pattern for GPU output validation:

1. **First run**: Test enumerates 5 attachments (Position, Normal, Albedo,
   MetallicRoughness, HDRColor). For each, check if `reference/deferred/Name.png`
   exists. If ANY is missing, capture all 5 as PNGs into the reference directory
   and `GTEST_SKIP()`.
2. **Second run**: All reference images exist. Capture attachments as `.tmp`
   PNGs, load both captured and reference, compare pixel-by-pixel with a
   per-channel tolerance of ±2. FAIL on any mismatch.

### Attachment-to-PNG Conversion Rules

Each attachment format maps to RGBA8 PNG differently:

| Attachment | Format | remapSigned | Conversion |
|------------|--------|-------------|------------|
| Position   | `R16G16B16A16_SFLOAT` | `false` | half→float→clamp[0,1]→U8 |
| Normal     | `R16G16B16A16_SFLOAT` | `true`  | half→float→`(val+1)*0.5`→clamp→U8 |
| Albedo     | `R8G8B8A8_SRGB` | `false` | raw U8 (sRGB-encoded) |
| MetallicRoughness | `R8G8B8A8_Unorm` | `false` | raw U8 |
| HDRColor   | `R16G16B16A16_SFLOAT` | `false` | half→float→clamp[0,1]→U8 |
| GizmoHighlight | `R8_UNORM` | `false` | raw U8 (0=no highlight, 255=edge highlight) |
| ComposedOutput | `R16G16B16A16_SFLOAT` | `false` | half→float→clamp[0,1]→U8 (gamma-corrected composed output) |

**Background pixels** (no geometry rendered) have:
- Position: `(0,0,0,0)` → PNG black
- Normal: `(0,0,0,0)` → PNG (0,0,0) after signed remap → (127,127,127) after
  `(val+1)*0.5` for zero → actually background check in ConvertHalfToU8 writes
  alpha=0 for background normal pixels
- Albedo: `(0,0,0,0)` → PNG black
- MetallicRoughness: `(0,0,0,0)` → PNG black
- GizmoHighlight: `0` → PNG black (no highlight on clear pixels)
- ComposedOutput: `(0,0,0,0)` → PNG black

### Generating and Verifying Reference Images

```bash
# Delete old references
rm test/render/reference/deferred/*.png

# Run test — generates + skips
make check FILTER="-R DeferredShading"

# Run test again — compares against newly generated references
make check FILTER="-R DeferredShading"
```

**(IMPORTANT) Always verify references with Python** before committing:

```python
from PIL import Image
from collections import Counter

img = Image.open("test/render/reference/deferred/HDRColor.png")
pixels = list(img.getdata())
unique = Counter(pixels)
print(f"Unique values: {len(unique)} out of {len(pixels)}")

# Check: if only 1 unique value, lighting may be broken (should vary across sphere)
```

Key things to check:
- **Position**: should have a visible blob with varying R/G values (B may be 0
  depending on camera orientation). Non-black pixel count = rendered sphere area.
- **Normal**: should have smoothly varying RGB values on sphere pixels. Uniform
  normals suggest a transform bug.
- **Albedo**: white on sphere pixels (255), black (0) on background. If ALL
  pixels are white, the clear isn't working or geometry covers everything.
- **MetallicRoughness**: R=0 (metallic), G=127-128 (roughness 0.5) on sphere,
  black on background. If background shows non-zero, clear isn't working.
- **HDRColor**: should show visible lighting (brighter on the lit side). If
  uniformly dark (~8) everywhere, lighting contribution is zero.
- **`reference/debug/DebugLine_Visible.png`** (64×64, DebugPass): a two-color image
  is the *correct* result here, not a broken one — a white overlay line on a black
  target. Verify by counting: exactly two colors, ~250 white pixels forming a 5-row
  band across the middle. A single unique value means the overlay did not rasterize
  or the whole target was covered.

## Running Tests

```bash
# Build tests
cmake --build build --config Debug

# Run all tests
make check

# Run a specific test by name pattern
make check FILTER="-R DeferredShading"

# Run a specific test case
make check FILTER="-R DeferredShadingTest.GbufferAttachments"

# Run only non-GPU tests (CI-compatible)
make check FILTER="-E DeferredShading|Lighting|Screenshot|ModelRender|Texture"

# Run the test executable directly with GTest filters
build/Debug/neurus_test --gtest_filter="DeferredShadingTest.*"
```

### Filtering Test Output

Coupled test output can be hard to parse. Use these patterns to pinpoint failures:

```bash
# PowerShell: Get only passed/failed summary
cd build; ctest -C Debug --output-on-failure 2>&1 | Select-String -Pattern "tests passed|tests failed"

# PowerShell: Get ALL failing test names (***Failed marker)
cd build; ctest -C Debug --output-on-failure 2>&1 | Select-String -Pattern "\*\*\*Failed"

# PowerShell: Filter to a specific subsystem
cd build; ctest -C Debug -R "SceneWiring|SSAO|Lighting" --output-on-failure 2>&1 | Select-String -Pattern "FAIL|pass|fail"

# PowerShell: Run test binary directly for cleaner output
./build/Debug/neurus_test.exe --gtest_filter="SceneWiring*" 2>&1 | Select-String -Pattern "FAILED|Running|OK|PASSED"

# PowerShell: List failing tests only (gtest summary line)
./build/Debug/neurus_test.exe --gtest_filter="SceneWiring*" 2>&1 | Select-String -Pattern "FAILED.*test"

# Bash/Git Bash: Same patterns but use grep instead of Select-String
cd build && ctest -C Debug --output-on-failure 2>&1 | grep -E "tests (passed|failed)|FAILED"
```

### Test Working Directory

CTest runs the test exe with `WorkingDirectory = CMAKE_BINARY_DIR` (e.g. `build/`).
The `res/` folder is copied to `CMAKE_BINARY_DIR/res/` by a POST_BUILD step on
`neurus_test`. `ResolveAssetPath()` reads assets directly from CWD
(e.g. `res/obj/sphere.obj`) without relative-path probing.

## Debugging GPU Test Failures

### HDRColor is Black / Near-Black

Checklist (in order):

1. **Position attachment early-out**: `posSample.w < 0.5` in the compute shader.
   If Position.w is 0 (clear value), all pixels early-out → pure black output.
   Check the Position PNG: if it shows a sphere, w should be 1.0 on those pixels.

2. **Light distance**: With inverse-square falloff, a light 5+ units away from
   the surface may contribute only 4% or less of the light power. Move the light
   closer or increase power.

3. **Object scale**: A coarse icosphere (20 triangles, radius ≈ 1) can fill most
   of a 256×256 viewport at 60° FOV from distance 5. Scale vertices down with
   `pos * 0.25f`.

4. **Descriptor bindings**: Verify that the compute shader's bindings (0-3 for
   G-Buffer samplers, 4 for storage image, 5 for light SSBO) match the
   `DescriptorSetLayout` bindings in `LightingPass::CreateDescriptorSetLayout()`.

5. **SSBO data**: Check that the `PointLightGpu` struct in C++ matches the
   `PointLight` struct in GLSL exactly (same offsets, same padding, same total
   size of 48 bytes). Use `static_assert` in C++.

6. **Push constants**: Verify the `LightingPushConstants` struct alignment
   matches the GLSL push constant layout (96 bytes total: int + pad12 +
   vec4 + mat4).

7. **Validation layers**: Run in Debug mode. If no validation errors print,
   the API usage is correct and the issue is algorithmic.

### G-Buffer Attachments All Uniform

If Albedo, MetallicRoughness, and HDRColor show the same values on ALL pixels
(both sphere and background), suspect:

1. **Dynamic rendering clear not working**: Verify `vk::AttachmentLoadOp::eClear`
   is being specified and the clear values are correct.
2. **Geometry covers all pixels**: Very coarse meshes (20-triangle icosphere)
   can have large projected triangles. Scale down or move camera back.
3. **Pipeline color blend writes everywhere**: Check no attachment has
   `blendEnable = VK_TRUE` with source factor that bleeds across pixels.

### Validation Error Debug

If validation layers print an error:

1. Read the error message to identify the violated VUID.
2. Check the VUID in the Vulkan spec (or search web for the exact error).
3. Fix the root cause — never suppress validation errors with `#ifndef _DEBUG`.

### Runtime Validation Error Capture

Vulkan validation errors are routed through a `vk::DebugUtilsMessengerEXT`
callback function, NOT directly to stdout or stderr. This means standard
output redirection (`2>&1`) may or may not capture validation messages
depending on how the callback is implemented.

**Common validation error patterns**:

1. **Wrong image view type**: Creating `VkImageView` with
   `VK_IMAGE_VIEW_TYPE_CUBE` for per-mip sub-resources of a cubemap is
   invalid — each per-mip view covers one array layer (not 6). Use
   `VK_IMAGE_VIEW_TYPE_2D_ARRAY` with `layerCount = 6` instead.

2. **Missing command pool reset flag**: `vk::CommandPoolCreateInfo` without
   `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` will trigger
   `VK_ERROR_VALIDATION_FAILED_EXT` if `vkResetCommandBuffer()` is called
   explicitly.

**Why validation errors can be invisible**:

| Output channel | Visible in terminal? | Captured by `2>&1`? |
|---|---|---|
| `std::cout` from callback | Yes | Yes (stdout) |
| `std::cerr` from callback | Yes | Yes (stderr) |
| File-only logger | No | No |
| Silent/no-op callback | No | No |
| `OutputDebugString` (VS Debug Output window) | No | No |

**How to capture validation errors reliably**:

1. **Use `2>&1` always** — this merges stdout and stderr, capturing both
   `std::cout` and `std::cerr` from any debug callback:
   ```
   $output = & "build/Debug/Neurus.exe" 2>&1
   ```
   Then grep for `VUID-` to detect any validation violations.

2. **Verify the debug callback implementation** in `VulkanContext.cpp` or
   `TestVulkanShared.cpp`. The callback should write to `std::cerr`, not
   `std::cout`, so validation messages survive stderr-only captures:
   ```cpp
   static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
       VkDebugUtilsMessageSeverityFlagBitsEXT severity,
       VkDebugUtilsMessageTypeFlagsEXT /*type*/,
       const VkDebugUtilsMessengerCallbackDataEXT* data,
       void* /*userData*/) {
       std::cerr << "[VALIDATION] " << data->pMessage << std::endl;
       return VK_FALSE;
   }
   ```

3. **Explicitly verify after fixing** suspected validation issues by running
   the application and checking for remaining VUIDs:
   ```
   $output = & "build/Debug/Neurus.exe" 2>&1
   if ($output -match "VUID-") { Write-Host "VALIDATION ERROR STILL PRESENT" }
   ```

4. **Be aware of the IDE pitfall**: Visual Studio's Debug Output window
   (`OutputDebugString`) can receive validation messages through a separate
   channel. The application may appear clean in the IDE while silently
   violating validation rules when run outside it. Always verify from a
   clean terminal, not from within the IDE.

## CI Expectations

- **Non-GPU tests** (editor layer, events): Run on every CI push. Must pass.
- **GPU tests**: Excluded from CI (no Vulkan 1.4 device in GitHub Actions).
  Must pass locally before merging.
- **Reference images**: Committed to the repository. If a PR changes rendered
  output, the reference images must be regenerated and re-committed.

### Regenerating Reference Images for a PR

1. Delete the affected `test/render/reference/*/*.png` files.
2. Run the test twice (first run generates, second run compares).
3. Verify the new references with Python (`PIL.Image` analysis).
4. Commit the new PNGs alongside the code changes.
5. The PR reviewer should verify the visual change is expected.

## GPU Test Patterns (Quick Reference)

These patterns were established during deferred PBR development and apply to all GPU tests:

- **Inherit `VulkanTestShared`** (`test/shared/TestVulkanShared.h`). The base class bootstraps Instance → PhysicalDevice → Device → Queue → CommandPool.
- **Use `BeginCmd()` and `EndSubmitWait(cmd)`** for every GPU command sequence. `BeginCmd()` returns a one-shot command buffer; `EndSubmitWait(cmd)` submits and waits-idle.
- **Reference-image tests**: "first-run-generates, second-run-compares" pattern. First run captures attachments as PNGs to `reference/deferred/` and SKIPs; second run compares pixel-by-pixel against those references with ±2 tolerance.
- **Attachment format conversions**: HDRColor (`R16G16B16A16_SFLOAT`) converts via half→float→clamp→U8. Normal attachments use signed remap `(val+1)*0.5` before clamp.
- **Test scene scale**: A sphere OBJ (radius ≈ 1) fills ~36% of a 256×256 viewport at 60° FOV from distance 5. Scale vertices with `pos * 0.25f` or reposition camera/light as needed.
- **Light placement**: Inverse-square attenuation `1/d²` is aggressive. Keep test lights within 2-4 units of geometry for visible lighting. A light at distance 5 produces only ~4% radiance vs distance 2.
- **Debugging black HDRColor**: First check that Position.w > 0 (the early-out in the compute shader), then check attenuation distance, light power, and descriptor bindings. Don't assume "zero lighting" means the compute shader is broken.
- **Verify references with Python** before committing: use `PIL.Image` to check uniform values, unique pixel counts, and histogram analysis.
- **Use `Mesh::UploadToGPU()` instead of raw VBO/IBO** in rendering tests. Instead of creating `VertexBuffer`/`IndexBuffer` directly (which duplicates GPU upload logic and bypasses the Mesh abstraction), load geometry through `MeshData` and `Mesh`:
  ```cpp
  // BEFORE (direct buffer creation — only for buffer-specific tests):
  VertexBuffer vbo(device, pd, queue, qfi, vertexData, size, stride, count);
  IndexBuffer  ibo(device, pd, queue, qfi, indexData, isize, icount);

  // AFTER (required for rendering/feature tests):
  auto meshData = std::make_shared<MeshData>();
  meshData->LoadObjFromString(kObjString);  // or LoadObj(path)
  auto mesh = std::make_shared<Mesh>();
  mesh->o_mesh = meshData;
  mesh->UploadToGPU(device, pd, queue, qfi);
  // Access GPU resources via:
  //   mesh->GetVertexBuffer(), mesh->GetIndexBuffer(), mesh->GetGPUIndexCount()
  ```
  **Exemption**: `test_buffers.cpp` and `test_vulkan_buffer.cpp` are the unit
  tests for `VertexBuffer`/`IndexBuffer`/`Buffer` hierarchy — they use raw buffers
  by design and are exempt from this rule.
- **"Extract, Don't Duplicate"**: Before writing a helper function in a test
  file, check if `VulkanTestShared` already provides it (see Shared Static
  Helpers above). If a helper is needed across 2+ test files, extract it to
  `TestVulkanShared.h` as a static method. Do **not** duplicate vertex structs,
  half-float converters, G-Buffer transition logic, or camera UBO computation
  — they are already centralized.
- **Temporal shadow tests** (`test_temporal_shadow.cpp`): Pure-logic tests that do NOT
  require a GPU. Test Halton sequence generation (Halton2/3/5 produce values in [0,1),
  jitter maps to [-0.5,0.5], and results are deterministic). Test `ComputeShadowAlpha`
  for both FixedEMA (alpha=1/8) and MovingAvg (alpha=1/(frameCount+1)) modes, verifying
  frame 0 always returns alpha=1 (overwrite). Run in CI.
- **SceneController tests** (`test/editor/test_scene_controller.cpp`): Non-GPU tests
  that drive `SceneController` through the `EventQueue` (events carry `int
  objectUid` payloads; the controller resolves them against the fixture's Scene
  via a `ControllerContext`). Cover selection (select /
  background-clear / shift-add / deselect), visibility flags (light visibility
  enqueues `LightingRebuild`), transform (position/scale; light transform enqueues
  `LightingRebuild`), camera target/FOV, mesh shadow/material, light
  power/radius/shadow/cutoff (power/radius/cutoff enqueue `LightGpuChanged`, shadow
  enqueues `LightingRebuild`), environment intensity/rotation, scene-add GPU sync
  (mesh/light/env add enqueues `SceneObjectGpuUploadRequested`, camera does not;
  shadow-enable enqueues it, shadow-disable does not), and dirty semantics
  (`SceneModified` on property changes, never on selection). Payload-capture
  variables for `LightGpuChanged` / `SceneObjectGpuUploadRequested` are plain
  `int` (the events carry `objectUid`). Run in CI.
- **ShaderLinkOp tests** (`test/editor/test_shader_controller.cpp`
  `ShaderCreateUndoTest`, `test/editor/test_operation_serialization.cpp`
  `OperationSerializationTest.ShaderLinkOp_RoundTrip`): Non-GPU tests that
  verify Create Shader is undoable as a pool-preserving membership toggle.
  A fixture mirrors `Editor::Initialize`'s `ShaderLinkRestored` /
  `ShaderUnlinkRestored` subscriptions (pooled mesh + pooled `RenderShader`;
  the restore events carry `int objectUid`); asserts: submit `ShaderLinkOp` →
  mesh linked; undo → `o_shader`/`o_shaderId`
  cleared while the pool keeps the shader; redo → the SAME pooled shader
  relinked (no new object minted); stale mesh UID no-ops; and the op
  round-trips through a cereal JSON archive. Run in CI.
- **Log Buffer Tests** (`test/core/test_log_buffer.cpp`, `test/ui/test_log_model.cpp`): Non-GPU tests that run in CI.
  `LogBufferTest` (core) covers ring capacity + wrap (drops oldest), per-level
  counts (Info/Error) including wrap adjustment, Clear() resets size/counts/seq,
  monotonic seq assignment, thread-safe Append from 2 threads. `LogModelTest`
  (ui) covers rowCount matching the buffer, custom role data
  (Level/Timestamp/Source/Message/Seq), Refresh inserting new rows, Clear reset.
  `LogFilterProxyTest` (ui) covers the level filter (All/InfoOnly/ErrorsOnly),
  case-insensitive search, and level + search combined.
- **Headless raster-pass tests** (`test/render/test_debug_pass.cpp`, issue #22): a
  raster pass that draws into `RenderCache` attachments with `LOAD_OP_LOAD` needs
  **no swapchain, renderer or RenderGraph** — the fixture primes the attachments by
  hand and records the pass over them, so it runs everywhere including macOS, where
  the presentation-based `SceneWiringTest` cases are skipped:
  ```cpp
  m_cache = std::make_unique<RenderCache>(*m_device, PhysicalDevice());
  m_pass  = std::make_unique<DebugPass>(*m_device, PhysicalDevice(), 2);
  // Prime: clearColorImage(black) + clearDepthStencilImage(depth) via TransferDst
  // Run:   RenderContext ctx; ctx.editor.{config,scene,debugDraw} = ...;
  //        m_pass->Record(*BeginCmd(), *m_cache, ctx); EndSubmitWait(c);
  ```
  **Isolate one variable per test rather than reading one composite image.** The four
  line tests submit the *same* segment against the *same* camera and differ only in
  depth/x-ray: far depth + depth-tested ⇒ drawn (rasterization works); near depth +
  depth-tested ⇒ **not** drawn (depth occlusion works); near depth + x-ray ⇒ drawn
  (x-ray bypasses depth); empty list ⇒ 0 draws and an untouched image. Because #1 and
  #3 produce identical pixels, "x-ray draws" cannot be faked by a pass that simply
  ignores depth — #2 would fail.
  **Measure position, not just area.** Geometry is chosen to be analytically
  predictable (camera at `(0,-5,0)` looking down +Y, up = +Z, so a segment along X
  through the origin is a horizontal band across the middle of a square target), and
  the assertions check the lit row/column extent, not a pixel count alone.
  **Time the marginal cost, not the absolute.** A record-submit-wait round trip is
  dominated by fixed submission and fence latency, so timing the 10,000-segment case
  alone measures the driver. Time 1 segment and 10,000 and assert the *difference*
  against issue #22's 0.5 ms budget — and assert the structural half exactly
  (10,000 segments must still be 2 draw calls). A slightly negative marginal is a
  pass, not an anomaly.
  **Distinguish a wireframe from a fill by area ratio, not by eye.** The wire-mesh
  test uploads a 4×4 quad OBJ through `UploadManager::UploadMeshData`, registers the
  MeshGPU under an arbitrary cache id, and points a `DebugWireMesh` at it. That the
  result is *edges* is asserted by counting lit pixels against the area of their own
  bounding box: 220 of 2116 (10%) for five thin edges, where a filled quad would light
  nearly all of it. A regression from `PolygonMode::eLine` to `eFill` therefore has to
  fail, which a "something was drawn" assertion would not catch.
- **DebugDrawBuilder tests** (`test/editor/test_debug_draw_builder.cpp`, issue #22):
  non-GPU tests that pin the three CPU-side contracts `DebugPass` trusts but cannot
  check — the x-ray **partition** (`xraySegmentStart` / `xrayPointStart` become draw
  ranges, so an off-by-one draws x-ray geometry depth-tested while every draw and
  pixel count still looks plausible), the **revision** (exactly one `Touch()` per
  rebuild; `DebugPass` skips its upload on a matching revision, so a missing Touch
  freezes the overlay and a double Touch re-uploads every frame), and **world-space
  baking**. Also covers dirty-flag lifecycle, `PackDebugColor`'s `0xAABBGGRR` layout
  and clamping, opacity folded into the packed alpha, and the
  `PointType::CUBE` → 12-edge decomposition — the last checked *geometrically*
  (every edge axis-aligned, one side long, 4 per axis, corners on the cube surface)
  rather than by index. Run in CI.
  > `Scene::ResPool` is an `unordered_map`, so the order of primitives from
  > *different* objects is unspecified. Assert over counts, boundary values, or
  > predicates keyed off the boundary itself
  > (`EXPECT_EQ(Has(flags, XRay), i >= xraySegmentStart)`) — never an absolute index,
  > which would make the suite flaky across libstdc++/libc++ or a hash-seed change.
- **Prove a passing suite is not vacuous by mutating the source.** Green tests on
  their own prove nothing (AGENTS.md: *"Do not cheat yourself"*). Back the
  implementation up outside the repo, inject realistic defects, and confirm that
  *exactly the intended* tests fail — for `DebugDrawBuilder` that was: drop
  `m_xraySegments.clear()`, move the boundary assignments *after* the `insert` calls,
  and drop `m_list.Touch()` ⇒ 8 targeted failures. Then restore and **verify with
  `diff` that the source is byte-identical** before rebuilding.

## Common Pitfalls Summary

| Pitfall | Symptom | Fix |
|---------|---------|-----|
| Object too large | Geometry fills entire viewport, G-Buffer uniform | `pos * 0.25f` |
| Light too far | HDRColor = ambient only (8,8,8) | Move light to dist 2-4 |
| Light radius too small | Attenuation too strong | Set `light_radius = 10.0f` |
| Push constants misaligned | Shader reads garbage | Check struct alignment |
| SSBO struct mismatch | Light data corrupted | `static_assert(sizeof == 48)` |
| Missing depth-range define | Wrong clip-space Z range | Add `GLM_FORCE_DEPTH_ZERO_TO_ONE` |
| Wrong normal space | Lighting doesn't match geometry | VS stores view-space, compute transforms back |
| First-run reference stale | Test passed against old reference | Delete PNGs, regenerate, verify |
| No `VK_EXT_DEBUG_UTILS` | Deferred validation errors | Include in Debug builds |
| Manual Vulkan bootstrap | Test file has its own Instance/Device/Queue/CommandPool setup (~70 lines) | Inherit `VulkanTestShared` base class |
| Raw VBO/IBO in rendering test | Test creates `VertexBuffer`/`IndexBuffer` directly | Use `Mesh::UploadToGPU()` |
| Duplicated `TestVertex` struct | `struct TestVertex { ... }` defined locally in test file | Use `TestVertex` from `TestVulkanShared.h` |
| Duplicated helper functions | `HalfToFloat`, `TransitionGbufferToColorAttachment`, `FindMemoryType` repeated across files | Use static helpers from `VulkanTestShared` |
| Placeholder/stub test file | File with single `GTEST_SKIP` test | Delete from CMakeLists.txt; do NOT write new test logic unless planned |
| **Stale reference from wrong cwd** | Reference images appear at `D:\Projects\test\render\reference\` instead of `test/render/reference/` | Running test binary directly from outside the CTest working directory causes resource path resolution to differ (resources are copied to `CMAKE_BINARY_DIR/res/`) | Always use `ctest -C Debug` from `build/`; if running the test binary directly, `cd build` first. Clean stale directories with `Remove-Item -Recurse -Force test/` at the project root. |

## Complete Development Cycle Checklist

Every subagent, feature implementation, or bugfix MUST complete ALL of
the following steps before declaring the task done.  Partial completion
is NOT acceptable -- skip none of these steps.

1. **Build → 0 errors**
   ```bash
   cmake --build build --config Debug
   ```
   No compilation errors, no linker errors.  Warnings should be addressed
   or justified.

2. **Run `Neurus.exe` → check terminal output**
   ```powershell
   $output = & "build/Debug/Neurus.exe" 2>&1; Start-Sleep -Seconds 3; Write-Host $output
   ```
   Scan for:
   - `VUID-` validation violations
   - `[VALIDATION]` prefixed errors from the debug messenger
   - Crashes (access violations, segfaults)
   - Unexpected `NEURUS_ERR` log lines

4. **Run tests related to the task**
   ```bash
   make check FILTER="-R <related_test_name>"
   ```
   Verify the test that is associated with the task FIRST. Otherwise, it will waste too much time on running the whole test suite.

4. **Run tests → 100% all pass**
   ```bash
   make check
   ```
   ALL tests must pass.  Do not ignore failures; fix them or update
   reference images if the change is intentional.

5. **Verify feature works visually**
   - Screenshots: check that `TakeScreenshotAllAttachments()` exports
     all relevant attachments (screenshots/ directory).
   - Visual check: the rendered viewport shows the expected result
     (lighting, shadows, geometry, etc.).
   - Runtime behaviour: resize the window, interact with the viewport,
     verify no deadlocks or freezes.

6. **No stubs, TODOs, or placeholders remain**
   - No `// TODO`, `// FIXME`, `// STUB`, or placeholder comments
     left behind in the changed files.
   - No hardcoded magic numbers without documentation.
   - No commented-out code blocks unless explicitly justified.

7. **Feature workflow** — For new rendering features, follow the full
    pattern in `.github/instructions/development.instructions.md` (4-wave
    implementation, per-wave verification, common bug fixes)
