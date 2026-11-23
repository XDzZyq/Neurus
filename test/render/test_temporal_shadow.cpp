/**
 * @file test_temporal_shadow.cpp
 * @brief Unit tests for temporal shadow accumulation infrastructure.
 *
 * Tests Halton sequence generation, jitter range, alpha computation,
 * and frame counter management. No GPU rendering required -- these
 * are pure-logic tests that run in CI.
 */

#include <gtest/gtest.h>
#include "render/HaltonSequence.hpp"
#include "render/TemporalAccumulator.hpp"

namespace neurus {
namespace {

// ---------------------------------------------------------------------------
// Halton sequence tests
// ---------------------------------------------------------------------------

TEST(HaltonSequenceTest, ValuesInRange)
{
    // Halton(2) and Halton(3) should produce values in [0, 1) for all indices
    for (uint32_t i = 0; i < 100; ++i)
    {
        float h2 = HaltonSequence::Halton2(i);
        float h3 = HaltonSequence::Halton3(i);
        float h5 = HaltonSequence::Halton5(i);
        EXPECT_GE(h5, 0.0f) << i;
        EXPECT_LT(h5, 1.0f) << i;

        EXPECT_GE(h2, 0.0f) << "Halton2(" << i << ") = " << h2 << " < 0";
        EXPECT_LT(h2, 1.0f) << "Halton2(" << i << ") = " << h2 << " >= 1";
        EXPECT_GE(h3, 0.0f) << "Halton3(" << i << ") = " << h3 << " < 0";
        EXPECT_LT(h3, 1.0f) << "Halton3(" << i << ") = " << h3 << " >= 1";
    }
}

TEST(HaltonSequenceTest, Deterministic)
{
    EXPECT_FLOAT_EQ(HaltonSequence::Halton5(0), 0.0f);
    EXPECT_FLOAT_EQ(HaltonSequence::Halton5(1), 1.0f / 5.0f);
    // Same index should always produce the same value
    EXPECT_FLOAT_EQ(HaltonSequence::Halton2(0), 0.0f);
    EXPECT_FLOAT_EQ(HaltonSequence::Halton2(1), 0.5f);
    EXPECT_FLOAT_EQ(HaltonSequence::Halton2(3), 0.75f);

    EXPECT_FLOAT_EQ(HaltonSequence::Halton3(0), 0.0f);
    EXPECT_FLOAT_EQ(HaltonSequence::Halton3(1), 1.0f / 3.0f);
    EXPECT_FLOAT_EQ(HaltonSequence::Halton3(2), 2.0f / 3.0f);
}

// ---------------------------------------------------------------------------
// Alpha computation tests
// ---------------------------------------------------------------------------

TEST(ShadowAlphaTest, FixedEMA)
{
    // Frame 0 always returns alpha = 1 (overwrite)
    EXPECT_FLOAT_EQ(ComputeShadowAlpha(ShadowAlphaMode::FixedEMA, 0), 1.0f);

    // Frame 1+ returns fixed alpha = 1/8
    EXPECT_FLOAT_EQ(ComputeShadowAlpha(ShadowAlphaMode::FixedEMA, 1), 1.0f / 8.0f);
    EXPECT_FLOAT_EQ(ComputeShadowAlpha(ShadowAlphaMode::FixedEMA, 2), 1.0f / 8.0f);
    EXPECT_FLOAT_EQ(ComputeShadowAlpha(ShadowAlphaMode::FixedEMA, 7), 1.0f / 8.0f);
    EXPECT_FLOAT_EQ(ComputeShadowAlpha(ShadowAlphaMode::FixedEMA, 100), 1.0f / 8.0f);
}

TEST(ShadowAlphaTest, MovingAvg)
{
    // Frame 0 always returns alpha = 1 (overwrite)
    EXPECT_FLOAT_EQ(ComputeShadowAlpha(ShadowAlphaMode::MovingAvg, 0), 1.0f);

    // Subsequent frames: alpha = 1/(frameCount+1)
    EXPECT_FLOAT_EQ(ComputeShadowAlpha(ShadowAlphaMode::MovingAvg, 1), 0.5f);
    EXPECT_FLOAT_EQ(ComputeShadowAlpha(ShadowAlphaMode::MovingAvg, 2), 1.0f / 3.0f);
    EXPECT_FLOAT_EQ(ComputeShadowAlpha(ShadowAlphaMode::MovingAvg, 3), 0.25f);
    EXPECT_FLOAT_EQ(ComputeShadowAlpha(ShadowAlphaMode::MovingAvg, 7), 0.125f);
    EXPECT_FLOAT_EQ(ComputeShadowAlpha(ShadowAlphaMode::MovingAvg, 15), 0.0625f);
}

TEST(ShadowAlphaTest, AlphaInRange)
{
    // Alpha should always be in (0, 1] for all frame counts and modes
    for (uint32_t fc = 0; fc < 100; ++fc)
    {
        float fixed = ComputeShadowAlpha(ShadowAlphaMode::FixedEMA, fc);
        float moving = ComputeShadowAlpha(ShadowAlphaMode::MovingAvg, fc);

        EXPECT_GT(fixed, 0.0f);
        EXPECT_LE(fixed, 1.0f);
        EXPECT_GT(moving, 0.0f);
        EXPECT_LE(moving, 1.0f);

        // Both modes give alpha=1 on frame 0
        if (fc == 0)
        {
            EXPECT_FLOAT_EQ(fixed, 1.0f);
            EXPECT_FLOAT_EQ(moving, 1.0f);
        }
    }
}

} // namespace
} // namespace neurus
