/**
 * @file RenderContext.cpp
 * @brief The one place a missing RenderConfig is turned into the defaults.
 *
 * Exists so RenderContext.h can forward-declare RenderConfig while Settings()
 * still needs the complete type: the fallback is written once, in
 * DefaultRenderConfig(), instead of as a `cfg ? cfg->x : <literal>` ternary at
 * every reader.
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
