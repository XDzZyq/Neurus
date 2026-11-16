#pragma once
#include <cstdint>

namespace neurus {

/**
 * @brief Quasi-random Halton(2,3) sequence generator for temporal jitter.
 *
 * Produces values in [0, 1) that are evenly distributed across the unit
 * square, for sub-pixel jitter.
 */
struct HaltonSequence
{
    /**
     * @brief Halton sequence base-2 value at the given index.
     * @param index Zero-based index into the sequence.
     * @return Value in [0, 1).
     */
    static float Halton2(uint32_t index)
    {
        float result = 0.0f;
        constexpr float invBase = 0.5f;
        float f = invBase;
        uint32_t i = index;
        while (i > 0)
        {
            result += (i % 2) * f;
            i /= 2;
            f *= invBase;
        }
        return result;
    }

    /**
     * @brief Halton sequence base-3 value at the given index.
     * @param index Zero-based index into the sequence.
     * @return Value in [0, 1).
     */
    static float Halton3(uint32_t index)
    {
        float result = 0.0f;
        constexpr float invBase = 1.0f / 3.0f;
        float f = invBase;
        uint32_t i = index;
        while (i > 0)
        {
            result += (i % 3) * f;
            i /= 3;
            f *= invBase;
        }
        return result;
    }

    /**
     * @brief Halton sequence base-5 value at the given index.
     * @param index Zero-based index into the sequence.
     * @return Value in [0, 1).
     */
    static float Halton5(uint32_t index)
    {
        float result = 0.0f;
        constexpr float invBase = 1.0f / 5.0f;
        float f = invBase;
        uint32_t i = index;
        while (i > 0)
        {
            result += (i % 5) * f;
            i /= 5;
            f *= invBase;
        }
        return result;
    }
};

} // namespace neurus
