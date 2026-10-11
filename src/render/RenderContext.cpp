/**
 * @file RenderContext.cpp
 * @brief The one place a missing RenderConfig is turned into the defaults.
 *
 * This translation unit exists only so RenderContext.h can keep RenderConfig on a
 * forward declaration while Settings() still needs the complete type. That trade
 * is the point of the file: the fallback is written once in
 * core/RenderConfig.h's DefaultRenderConfig() instead of as a
 * `cfg ? cfg->x : <literal>` ternary at every reader.
 *
 * Vulkan-free, and must stay so: RenderContext describes the frame, not the GPU.
 */

#include "RenderContext.h"

#include "core/RenderConfig.h"

namespace neurus
{

const RenderConfig& RenderContext::Settings() const
{
	return config ? *config : DefaultRenderConfig();
}

} // namespace neurus
