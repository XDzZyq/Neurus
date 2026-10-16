/**
 * @file RenderContext.cpp
 * @brief The one place a missing half of the frame is turned into defaults.
 *
 * Exists so RenderContext.h can forward-declare RenderConfig while Settings()
 * still needs the complete type: the fallback is written once, in
 * DefaultRenderConfig(), instead of as a `cfg ? cfg->x : <literal>` ternary at
 * every reader. Frame() does the same for an unpublished snapshot.
 *
 * Vulkan-free, and must stay so: RenderContext describes the frame, not the GPU.
 */

#include "RenderContext.h"

#include "core/RenderConfig.h"
#include "render/RenderFrameSnapshot.h"

namespace neurus
{

const RenderConfig& RenderContext::Settings() const
{
	return config ? *config : DefaultRenderConfig();
}

const RenderFrameSnapshot& RenderContext::Frame() const
{
	// One empty frame for every context that published none. Immutable after
	// its first use, so handing out a reference to it stays safe once the
	// renderer reads contexts from its own thread.
	static const RenderFrameSnapshot kEmptyFrame{};
	return snapshot ? *snapshot : kEmptyFrame;
}

} // namespace neurus
