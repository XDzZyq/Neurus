#pragma once

#include "OutputTarget.h"

namespace neurus {

/**
 * @brief OutputTarget for a windowless renderer.
 *
 * Owns no image: the last stage of a frame is the OverlayOutput attachment, so
 * a headless capture reads that attachment directly. Acquire always succeeds
 * with index 0, Present does nothing, and no acquire/render-finished semaphore
 * is ever needed - the frame is fenced by the renderer's timeline and slot
 * fence alone.
 */
class OffscreenTarget final : public OutputTarget
{
public:
	explicit OffscreenTarget(vk::Extent2D extent);

	vk::Extent2D Extent() const override { return m_extent; }
	vk::Format Format() const override;
	uint32_t ImageCount() const override { return 1; }
	vk::Image Image(uint32_t index) const override;
	bool Presents() const override { return false; }
	uint32_t Generation() const override { return m_generation; }

	std::optional<uint32_t> Acquire(const vk::raii::Semaphore* imageAvailable) override;
	void Present(const vk::raii::Semaphore& renderFinished, uint32_t index) override;
	void Recreate(uint32_t width, uint32_t height) override;

private:
	vk::Extent2D m_extent;
	uint32_t m_generation = 0;
};

} // namespace neurus
