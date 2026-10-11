# Temporal Accumulation

Neurus accumulates jittered shadow samples across frames, resetting the accumulation whenever the scene changes.

## Shadow accumulation

The shadow intensity pass accumulates per-light shadow samples over time, and the renderer tracks a per-frame jitter direction and an iteration counter that drive it.

- **Jitter**: a per-frame 3D random direction (unit-ball vector from `Halton(2,3,5)`) applied as `pos_jittered = pos + light.radius * jitter` for point lights, and as a UV-space offset `shadowUV + vec2(jitter.x, jitter.y) * uvRadius` for sun lights. The file `src/render/HaltonSequence.h` provides the low-discrepancy sequence.
- **Accumulation**: in-place read-modify-write on the `ShadowIntensity` layered array. The compute shader reads the previous accumulated value via `imageLoad`, evaluates a single jittered shadow sample, and blends via EMA (exponential moving average) using `mix(prevAccum, sample, alpha)`, writing back via `imageStore`.
- **Alpha modes**: two blend modes controlled by `RenderConfig::r_sampling_mode`, read via `ctx.Settings()` so the declared default (0 = Fixed EMA) is the one a frame with no published config gets:
  - Fixed EMA (0): `alpha = 1/8` - fast convergence
  - Moving Average (1): `alpha = 1/(iteration + 1)` - true averaging, resets on scene change
- **Iteration**: the global frame counter `m_iteration` in `DeferredRenderer`, exposed via `RenderContext::iteration`. The Editor resets it through the event system when the scene changes - see `RenderResetEvent`.
- **Reset**: any scene-changing event (camera move, light change, object transform, visibility toggle, config change, project load, asset import) enqueues a `RenderResetEvent`. The Editor subscribes and calls `DeferredRenderer::ResetShadowAccumulation()`, which sets `m_iteration = 0`.

## Accumulation convention

The renderer's temporal accumulation follows one convention across the pipeline:

- **Iteration** (`DrawFrame()` counter): `DeferredRenderer::m_iteration`, reset by the Editor on scene changes via `RenderResetEvent`.
- **Jitter**: 3D jitter (`RenderContext::jitter`). Point: `pos + radius*jitter`. Sun: UV-space `shadowUV + (jitter.x, jitter.y) * uvRadius`.
- **Accumulation**: in-place read-modify-write on `ShadowIntensity` with EMA blend: `mix(prev, sample, alpha)`.
- **Alpha**: 0 = FixedAlpha (1/8), 1 = MovingAvg (1/(n+1)). Both the mode (`r_sampling_mode`) and the shadow bias are read through `RenderContext::Settings()`, so the only place either default is written is `RenderConfig`'s member initializers - no pass restates one.

The accumulation state lives in the `ShadowIntensity` attachments described in [Resources](resources.md), and the pass that performs it is described in [Passes](passes.md). The reset event is delivered through the event system; `RenderContext` stays an immutable per-frame snapshot throughout.
