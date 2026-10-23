# Render Config

`RenderConfig` aggregates every user-settable rendering option that affects visual quality and performance, and its in-class member initializers are the single source of truth for their defaults.

## Location and ownership

- Pure data class with query helpers, owned by the Editor (`Editor::m_config`).
- Lives in `src/core/` rather than `src/render/` because it is Vulkan-free and read by four layers (asset, editor, render, UI). A type's directory names its dependency layer, not its owner - as `Camera` and `Mesh` live in `src/scene/` while the Editor owns them. `src/core/` is the only genuine leaf: it has no dependency on `src/render/`, and this type's only project dependency is `core/Serialize.h`.
- Reaches the render passes as a value copied into `RenderFrameSnapshot` (`src/render/RenderFrameSnapshot.h`), read through `ctx.Frame().Settings()`, and the UI panels by embedded pointer in `UIContext` (`src/ui/UIContext.h`), read through `UIContext::Settings()`. When there is no Editor config, the UI's `Settings()` falls back to `core/RenderConfig.h`'s `DefaultRenderConfig()` - which *is* the member initializers rather than a second copy of them - and the frame's `config` is default-constructed, which *is* that same value; so neither reader ever sees null and none needs a fallback literal of its own. Because both resolve to the same single default, a pass and a panel cannot disagree about an unset value.
- Not a per-frame struct: it persists across frames and is read at the start of each frame. Changes take effect on the next `RecordCommandBuffer` / `DrawFrame()` call.

See [Layers](../architecture/layers.md) and [Data Flow](../architecture/data-flow.md).

## Algorithm selection

Enums choose the active algorithm:

- `RenderPipeLine` - `Forward` (single-pass), `Deferred` (multi-pass with G-Buffer, current), `Custom0` (reserved slot).
- `AAAlg` - `None`, `MSAA`, `FXAA`.
- `AOAlg` - `None`, `SSAO` (current implementation).
- `ShadowAlg` - `None`, `ShadowMapping` (current implementation), `SDFSoftShadow` (planned), `VSSM`.
- `SSRAlg` - `None`, `RayMarching`, `SDFRayMarching`, `SDFResolvedRayMarching`.

## Options and defaults

Defaults are written only in the member initializers:

| Setting | Default | Meaning |
|---|---|---|
| `r_pipeline` | `RenderPipeLine::Deferred` | Active rendering pipeline |
| `r_aa` | `AAAlg::None` | Anti-aliasing mode |
| `r_ao` | `AOAlg::SSAO` | Ambient occlusion algorithm |
| `r_shadow` | `ShadowAlg::ShadowMapping` | Shadow algorithm |
| `r_ssr` | `SSRAlg::None` | Screen-space reflections |
| `r_gamma` | `1.0` | Gamma correction factor |
| `r_shadow_bias` | `0.02` | Depth bias for shadow acne prevention |
| `r_sampling_mode` | `0` | Temporal sampling mode (0 = FixedEMA/8, 1 = MovingAvg) |
| `r_ao_ksize` | `16` | AO kernel size (number of samples) |
| `r_ao_radius` | `0.5` | AO sample radius in world space |
| `r_sample_pf` | `128` | Samples per frame for progressive rendering |
| `r_sampling_count` | `64` | Scene frames accumulated after a restart before the renderer stops sampling (clamped to at least 1) |
| `r_transparent` | `false` | Transparent background (checkerboard instead of skybox) |
| `r_debug_draw` | `true` | Master switch for the viewport debug/gizmo overlay |
| `r_fxaa_subpix` | `0.75` | Sub-pixel aliasing removal amount (0.0-1.0) |
| `r_fxaa_edge_threshold` | `0.166` | Edge detection threshold (0.063-0.333) |
| `r_fxaa_edge_threshold_min` | `0.0833` | Minimum edge threshold (0.0312-0.0833) |

`r_debug_draw` gates publication, not pipeline topology: when false `Editor::BuildSnapshot()` leaves the frame's `debugDraw` empty with `HasDebugDraw()` false, and `DebugPass` returns before touching any image, so the pass stays in the render graph unconditionally and toggling this costs no graph rebuild.

`r_sampling_count` is the depth of the progressive image: `DeferredRenderer::NextWork()` reports `Sample` while fewer than `max(1, r_sampling_count)` scene frames have been accumulated since the last restart, then `Overlay` while overlay content is stale and `Idle` otherwise. It is read on each sample frame, and a new value always arrives with the restart a config change posts, so the cached budget is never compared against a run it does not describe. See [Temporal Accumulation](../render/temporal.md).

## Defaults as the single source of truth

Both the temporal sampling mode (`r_sampling_mode`) and the shadow bias (`r_shadow_bias`) are read through `ctx.Frame().Settings()` (and `UIContext::Settings()` from a panel), so the only place either default is written is `RenderConfig`'s member initializers, reached through `DefaultRenderConfig()` - no pass and no panel restates one. Reading a raw `config` pointer with a `cfg ? cfg->x : <literal>` ternary is how the shadow-bias default once came to differ by 40x between a pass and the declaration; `Settings()` exists to make that impossible. This is the same "one definition of every default" rule applied to serialization in [Serialization](serialization.md).

## Query helpers

- `RequiresShadow()` - true when any shadow algorithm is active.
- `RequiresSSR()` - true when screen-space reflections are enabled.
- `RequiresSDF()` - true when SDF construction is needed for shadows or SSR.
- `RequiresFXAA()` - true when `r_aa == AAAlg::FXAA`.
- `RequiresDebugDraw()` - true when the viewport debug/gizmo overlay should be drawn.
- `operator==` is defaulted for value equality across all settings, used to skip no-op undo entries.

## Serialization

`RenderConfig::serialize` reads every setting with `archive::Optional`, one field independently of the rest. No render setting is load-bearing for identity, so none is worth failing a load over: a key this build does not find keeps the member at its in-class default and the settings around it still load. `ConfigComponent::Load` reads through `archive::LoadFresh`, so the object being filled is default-constructed and "absent" means "default" rather than "whatever the running session happened to have".

See [Serialization](serialization.md) and [Project Serialization](../asset/project-serialization.md).
