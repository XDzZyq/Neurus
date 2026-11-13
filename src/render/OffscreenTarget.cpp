#include "OffscreenTarget.h"

namespace neurus {

OffscreenTarget::OffscreenTarget(vk::Extent2D extent)
	: m_extent(extent)
{
}

vk::Format OffscreenTarget::Format() const
{
	// The attachment a headless capture reads (OverlayOutput) is RGBA16F. Nothing
	// is presented, so this value only describes the frame's colour space.
	return vk::Format::eR16G16B16A16Sfloat;
}

vk::Image OffscreenTarget::Image(uint32_t) const
{
	// A headless frame's final image is an attachment, not target-owned memory.
	return vk::Image{};
}

std::optional<uint32_t> OffscreenTarget::Acquire(const vk::raii::Semaphore*)
{
	return 0;
}

void OffscreenTarget::Present(const vk::raii::Semaphore&, uint32_t)
{
	// Nothing to present.
}

void OffscreenTarget::Recreate(uint32_t width, uint32_t height)
{
	const vk::Extent2D requested{width, height};
	if (requested.width == m_extent.width && requested.height == m_extent.height)
	{
		// Same-size resize must not invalidate the attachments: the renderer
		// drops every screen-space image on a generation change.
		return;
	}
	m_extent = requested;
	++m_generation;
}

} // namespace neurus
