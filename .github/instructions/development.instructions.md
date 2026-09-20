# Feature Development Workflow

## Overview

Every rendering feature (new light type, new pass, new effect) follows this
proven pattern. This document encodes the learnings from implementing deferred
PBR, SSAO, multi-light shadows, and sun light.

---

## Phase 1: Plan

1. **Trace the closest existing feature** to understand all touchpoints.
   Example: Sun Light → study Point Light's entire pipeline.
2. **Map every layer**: scene → cache → passes → shaders → renderer → UI → tests → docs.
   No layer should be surprised by the new feature.
3. **Confirm architecture with user**: new class vs extend existing, storage strategy,
   binding layout, shader approach, test strategy.

### Example: Sun Light Planning

When implementing Sun Light (directional), the closest existing feature was
Point Light (point source). Mapping the pipeline revealed these touchpoints:

| Layer | Point Light | Sun Light |
|-------|------------|-----------|
| Scene | `PointLight` struct | `SunLight` struct |
| RenderContext | `pointLights` vector + SSBO backing | `sunLights` vector + SSBO backing |
| RenderCache | Cubemap shadow map (6-layer 2D_ARRAY, 1024x1024) | 2D orthographic shadow map (2048x2048) |
| ShadowDepthPass | 6-face cubemap geometry pass | Single-view 2D orthographic pass |
| ShadowIntensityPass | `samplerCubeShadow` + direction-based PCF | `sampler2DShadow` + ortho depth PCF |
| LightingPass | Binding 5: `PointLightGpu` SSBO | Binding 6: `SunLightGpu` SSBO |
| DeferredRenderer | Per-light shadow depth → intensity → lighting | Same, with light-type branching |
| Shaders | `shade_point_light.glsl` | `shade_sun_light.glsl` |

## Phase 2: Implement

Break into waves, fan out aggressively:

| Wave | What | Parallelism |
|------|------|-------------|
| Foundation | Data structs, shaders, cache support | FULL parallel (independent) |
| Core | Extend passes (dual-pipeline), modify GLSL | Parallel unless same file |
| Integration | Wire into DeferredRenderer, UI, scene | Sequential (depends on Core) |
| Tests + Docs | GPU tests, reference images, .github/instructions | FULL parallel |

### Wave 0: Foundation (Data Structs + Shaders)

Create the foundational data types and shader source. These are independent
and can be done in parallel:

- **Scene struct** (`src/scene/`): `SunLight` with direction, color, intensity,
  shadow parameters (shadow map resolution, frustum planes, shadow mode).
- **GPU SSBO struct** (`src/render/passes/`): `SunLightGpu` matching the GLSL
  `SunLight` struct byte-for-byte. Use `static_assert` on size + `offsetof` on
  each field.
- **Push constant struct**: `SunShadowPushConstants` with `mat4 lightViewProj`.
- **GLSL shaders**: `sun_shadow_depth.vert/.frag`, `sun_shadow_intensity.comp`,
  `shade_sun_light.glsl`.
- **RenderContext**: Add `std::vector<SunLightGpu> sunLights` and backing SSBO.
- **RenderCache**: Add `GetShadowMap(lightUID, LightType::SUNLIGHT)` support for
  2D orthographic depth maps at 2048×2048 resolution. Extend
  `GetShadowIntensityLayer(lightUID, extent)` to assign layers to sun lights.

### Wave 1: Core (Passes)

Extend the render passes to handle the new light type alongside existing ones:

- **ShadowDepthPass**: Add sun light recording method with orthographic
  projection, `glm::ortho()`, and depth range `[0,1]` (requires
  `GLM_FORCE_DEPTH_ZERO_TO_ONE`). View matrix via `glm::lookAt()` from light
  position/direction.
- **ShadowIntensityPass**: Add separate descriptor set layout for `sampler2DShadow`
  (different from cubemap). Add `SunShadowIntensityEval` compute dispatch with
  PCF kernel. Supports `HARD`, `SOFT_PCF_16`, and `SOFT_PCF_64` modes.
- **LightingPass**: Add `SunLightGpu` SSBO at binding 6. Add
  `shade_sun_light.glsl` include. Read shadow intensity from per-light layer in
  the `ShadowIntensity` 2D array.

**Key design decision**: Each pass maintains its own descriptor set layouts for
each light type. This avoids layout conflicts between cubemap and 2D shadow
samplers. The pass records pipeline barriers between light-type dispatches.

### Wave 2: Integration

Wire everything into the render loop and editor:

- **DeferredRenderer**: Add `RenderSunShadowMaps()` and `EvalSunShadowIntensity()`
  methods. Call them in the shadow pass loop alongside point lights.
- **Upload methods**: `UploadSunLights()` mirrors `UploadPointLights()` --
  creates/modifies SSBO backing for the sun light vector.
- **Scene population**: Add test sun lights to the default scene for visual
  verification.
- **Cleanup**: Extend `ReleaseCachedResources()` to destroy sun light resources.

### Wave 3: Tests + Documentation

- **GPU math test** (`test_sun_shadow_depth.cpp`): Depth value validation using
  orthographic projection formula. Read back shadow map and verify expected
  depth at occluder pixels.
- **GPU intensity test** (`test_sun_shadow_intensity.cpp`): Render occluder,
  evaluate shadow intensity. Verify shadowed pixels get intensity > 0 and lit
  pixels get 0.
- **Extended deferred test** (`test_deferred_shading.cpp`): Add sun light
  alongside point lights. Verify sun light contributes to HDRColor output.
- **Reference images**: Regenerate `HDRColor.png` after adding sun light.
  Verify with Python PIL before committing.
- **Update docs**: Add sun shadow conventions to `renderer.instructions.md`.
  Add test patterns to `test.instructions.md`.

## Phase 3: Verify (PER WAVE, non-negotiable)

```powershell
cmake --build build --config Debug                # 0 errors
python scripts/extract_i18n.py --check            # 100% translated, not stale
make check                                       # ALL pass, including regression
$output = & "build/Debug/Neurus.exe" 2>&1        # capture output
$output | Select-String "VUID-"                  # ZERO matches
```

**Every new user-visible string is translated in the same commit.**
`scripts/extract_i18n.py` only *extracts*: a key lands in `res/i18n/*.po` with an
empty `msgstr`, so adding one `translate("...")` call is enough to make the
catalog stale, and CI's pre-build `--check` gate stays red until the key is both
rendered into the catalog and filled in. Run the extractor, write the
translation for every shipped catalog, then re-run `--check` until it prints
`OK`. The failure looks like `160/184 translated (87.0%) MISSING 24` — that is a
missing translation, not a stale file, so regenerating the catalog does not fix
it. See ui-system.instructions.md for the pipeline.

**For reference images, always verify with Python before committing:**

```python
from PIL import Image
from collections import Counter
img = Image.open("path/to/ref.png")
pixels = list(img.getdata())
unique = Counter(pixels)
non_black = sum(1 for p in pixels if p != (0,0,0,255))
print(f"{len(unique)} unique, {non_black} non-black")
# Fail: len(unique) <= 1 (uniform = broken)
# Fail: non_black == 0 (all black = geometry not rendered)
# Fail: all white = clear not working
```

## Phase 4: Fix Common Bugs

Bugs encountered during Sun Light implementation and their fixes:

| Bug | Symptom | Root Cause | Fix |
|-----|---------|------------|-----|
| Push constant mismatch | Light direction frozen or wrong | C++ padding in `SunShadowPushConstants` ≠ GLSL `layout(push_constant)` offsets | `static_assert(sizeof(...))` + verify `offsetof` for each field |
| Zero sun contribution | HDRColor = point lights only | `UploadSunLights()` never called; SSBO uninitialized | Trace call chain: `DeferredRenderer::Record()` → verify upload methods are called |
| Uniform HDRColor | All pixels same value | Clear value or frustum range mismatch between test and production | Match ortho frustum dimensions across tests and scene setup |
| Descriptor VUID violation | `VUID-vkCmdDispatch-viewType-*` | Wrong descriptor set bound for the active pipeline | Each light type needs its own descriptor set layout/update; trace set/pipeline pairing in `Record()` |
| Build race condition | EXE locked by another process | Two agents building simultaneously; one holds file lock while other tries to write | Only one agent runs build + tests; all others compile-check only via `lsp_diagnostics` |
| Depth range wrong | Shadow map depth values off by 2x | `glm::ortho()` defaults to OpenGL depth range [-1,1] | Define `GLM_FORCE_DEPTH_ZERO_TO_ONE` before any GLM include |
| Shadow intensity all zero | No shadow on any pixel | `sampler2DShadow` comparison mode not enabled on sampler | Set `VK_COMPARE_OP_LESS_OR_EQUAL` in sampler create info |
| Stale reference from wrong cwd | Reference images created at `D:\Projects\test\render\reference\` instead of `test/render/reference/` | Running test binary from outside the CTest working directory causes resource path resolution to differ (resources are copied to `CMAKE_BINARY_DIR/res/`) | Always use `ctest -C Debug` from `build/`, or cd to `build/` before running the test binary directly |

## Phase 5: Commit

- **Do NOT commit until user explicitly approves.**
- Standard message: `feat(scope): what was built`
- Must state: "No validation error, no unreasonable reference image, all tests passed."

---

## Checklist

- [ ] All 4 waves complete
- [ ] Build: 0 errors, 0 warnings
- [ ] All tests pass (existing + new) -- `make check`
- [ ] Zero VUID validation errors -- `Neurus.exe 2>&1 | grep VUID-`
- [ ] No untranslated or stale catalogs -- `python scripts/extract_i18n.py --check`
- [ ] Every reference image verified with Python PIL
- [ ] User reviewed and approved
- [ ] Commit with standard message
