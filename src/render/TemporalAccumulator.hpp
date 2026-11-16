#pragma once
#include <cstdint>

namespace neurus {

/**
 * @brief Shadow accumulation alpha mode.
 */
enum class ShadowAlphaMode : uint32_t
{
    FixedEMA    = 0,  ///< Fixed alpha = 1/8 per frame
    MovingAvg   = 1,  ///< Alpha = 1/(frameCount + 1) — true moving average
};

/**
 * @brief Computes the temporal blend alpha for shadow accumulation.
 *
 * frameCount = 0 always returns alpha = 1.0 (overwrite / no history).
 *
 * @param mode       Blend mode (FixedEMA or MovingAvg).
 * @param frameCount Number of frames accumulated so far (0 = first frame).
 * @return Alpha value in (0, 1] for EMA blend: accum = lerp(prev, sample, alpha).
 */
inline float ComputeShadowAlpha(ShadowAlphaMode mode, uint32_t frameCount)
{
    if (frameCount == 0) return 1.0f;  // overwrite on first frame / reset

    switch (mode)
    {
    case ShadowAlphaMode::FixedEMA:     return 1.0f / 8.0f;
    case ShadowAlphaMode::MovingAvg:    return 1.0f / static_cast<float>(frameCount + 1);
    }

    return 1.0f;
}

} // namespace neurus
