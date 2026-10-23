/**
 * @file RenderContext.cpp
 * @brief The one place an unpublished context is turned into an empty frame.
 *
 * Exists so RenderContext.h can forward-declare RenderFrameSnapshot while
 * Frame() still needs the complete type: a context that published no snapshot
 * reads as a single shared empty frame, written once here, rather than as a
 * pointer every pass tests.
 *
 * Vulkan-free, and must stay so: RenderContext describes the frame, not the GPU.
 */

#include "RenderContext.h"

#include "render/RenderFrameSnapshot.h"

namespace neurus
{

const RenderFrameSnapshot& RenderContext::Frame() const
{
	// One empty frame for every context that published none. Immutable after
	// its first use, so handing out a reference to it stays safe once the
	// renderer reads contexts from its own thread.
	static const RenderFrameSnapshot kEmptyFrame{};
	return snapshot ? *snapshot : kEmptyFrame;
}

} // namespace neurus
