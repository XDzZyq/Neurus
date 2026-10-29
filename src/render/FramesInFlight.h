/**
 * @file FramesInFlight.h
 * @brief How many frames the renderer may have in flight at once.
 *
 * Shared by every consumer of the number - the renderer, the frame data pool
 * and the passes that size per-frame descriptor sets - so they can never
 * disagree about how many slots exist. Header-only: a compile-time constant,
 * not a resource.
 */

#pragma once

#include <cstdint>

namespace neurus
{

/**
 * @brief Number of frames the renderer may have in flight at once.
 *
 * One frame in flight: the render cache returns one shared attachment per
 * name, so a second in-flight frame would write an attachment the previous
 * frame's blit still reads. Raising this above 1 requires per-frame-slot
 * attachments first.
 */
inline constexpr uint32_t kMaxFramesInFlight = 1;

} // namespace neurus
