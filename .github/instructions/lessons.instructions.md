# Lessons Learned

Encountered issues and the rules they produced. Each entry is a **symptom** and
the **rule** that prevents it. Design detail behind these lives in
[`docs/`](../../docs/README.md).

## Build and tooling

- **A `ctest` run reports tests passed but nothing executed.** A multi-config
  build tree has no tests without `-C`. Rule: always name the config -
  `cd build && ctest -C Debug --output-on-failure` - and read an unexpected suite
  total as "wrong tree", not "tests disappeared". See
  [tips.instructions.md](tips.instructions.md).
- **Reference images appear outside the tree** (for example
  `D:\Projects\test\render\reference\`). Rule: run tests through CTest so the
  working directory is `CMAKE_BINARY_DIR`, where `res/` is copied. Never launch
  the test binary by hand from another directory.
- **`error C1128: number of sections exceeded object file format limit`.** Rule:
  `/bigobj` is already applied to every MSVC translation unit; when a TU trips
  C1128 again, split that TU - do not add the flag.
- **SIGSEGV at `0x1` on the Debug leg only after a dependency change.** An
  `qtadvanceddocking` archive linked against a different Qt's inline templates.
  Rule: the prebuilt ADS archive is used only when its `build_info.txt`
  `qt_version` and `cxx_standard` match this build exactly; rebuild (re-stamp) the
  archive whenever Qt changes.

## Validation and synchronization

- **No validation output despite a bug.** Validation goes through the
  debug-utils callback, and the IDE's Debug Output window is a separate channel.
  Rule: capture with `2>&1` and grep for `VUID-`; never trust a clean IDE.
- **A sync hazard with zero core validation errors.** Layouts can be consistent
  while ordering is wrong. Rule: after touching barriers or submit scopes, re-run
  with
  `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`.
- **Per-frame-random tearing of an overlay on MoltenVK, no validation error.**
  A producer pre-transitioned its output to the state it guessed the next
  consumer wanted, so the consumer's barrier covered no writes. Rule: a pass
  transitions the images it is about to *use* and leaves its own outputs in its
  own write state (`ShaderWrite`); the consumer orders against it.
- **A discard transition hazards against the previous reader.**
  `Undefined`/`Invalid` must use `eAllCommands`, never `eTopOfPipe`, as a source
  stage.

## Correctness and "do not cheat yourself"

- **A uniform, all-black or all-white reference image "passes".** Rule: verify
  every reference with Python PIL before committing - fail on <= 1 unique colour
  or on zero non-black pixels for a geometry pass.
- **A green suite that proves nothing.** Rule: back the implementation up
  outside the repo, inject realistic defects, confirm that exactly the intended
  tests fail, then restore and `diff` the source to byte-identical before
  rebuilding.

## GPU test scenes

- **HDRColor is black.** Check, in order: `Position.w > 0` (the compute early-out),
  light distance (inverse-square falloff), light power, then descriptor bindings.
  Do not assume the compute shader is broken.
- **G-Buffer attachments are uniform everywhere.** The object is too large or the
  camera too far - scale vertices (`pos * 0.25f`) or move the camera back.
- **A pass draws nothing and reports no error.** A test that drives a pass
  directly must publish the camera first
  (`VulkanTestShared::PublishSceneCamera`), and a scene camera must be activated
  (`scene.ActivateCamera(...)`) - no camera is active by default.
- **Shadow depth is off by 2x / clip-space Z is wrong.** Define
  `GLM_FORCE_DEPTH_ZERO_TO_ONE` before any GLM include.
- **Shadow intensity is zero everywhere.** Enable comparison mode on the shadow
  sampler (`VK_COMPARE_OP_LESS_OR_EQUAL`).
- **Push constants or SSBO data read as garbage.** Keep the C++ struct
  byte-for-byte with the GLSL declaration; `static_assert` the size and check
  `offsetof` for every field.

## Temporal accumulation

- **Accumulation attributed to the wrong pass.** The EMA
  `mix(prev, sample, alpha)` lives in the shadow-intensity compute shaders
  (`shadow_eval.comp`, `sun_shadow_eval.comp`), i.e. `ShadowIntensityPass` - not
  `ComposePass`, which only blends the selection highlight and applies gamma.

## Editor and viewport

- **The gizmo handle keeps the previous camera's scale / a camera change does not
  move it.** The guide is a fixed pixel budget, so the camera is an input. Rule: a
  camera change must `MarkGizmoDirty()`.
- **The view snaps or dangles after activating, deactivating or deleting a
  camera.** Those land inside `EventQueue::Process()`. Rule: sync the view camera
  before and after `Process()`, and again after the delete subscription has run.
- **A cancelled gesture leaves the document needing a save.** Rule: a drag frame
  (and the cancel path) is `ObjectPreviewed` with no `SceneModified`; only a
  confirm is `ObjectMutated`. `SceneModified` is one-way.

## Process and environment

- **"Access denied" / file-in-use when launching `Neurus.exe` or a test.** Two
  agents built or ran at once. Rule: only the master builds, and only one agent
  runs `ctest`/`Neurus.exe` at a time; subagents compile-check only and never
  invoke the build system.
- **Another agent's in-progress edits vanish.** Rule: never use `git stash` while
  others are editing.
- **Replacement characters appear where non-ASCII used to be in source.** Rule:
  never use PowerShell `Set-Content -Force`; use the native `write`/`edit` tools,
  which preserve UTF-8.
