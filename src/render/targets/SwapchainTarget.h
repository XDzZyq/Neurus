#pragma once

#include "OutputTarget.h"

#include <memory>

namespace neurus {

class SharedQueue;
class Swapchain;

/**
 * @brief OutputTarget backed by a window swapchain.
 *
 * A thin wrapper: every call forwards to neurus::Swapchain, which keeps the
 * acquire/present/out-of-date behaviour exactly as the windowed renderer has
 * always used it.
 */
class SwapchainTarget final : public OutputTarget
{
public:
	/**
	 * @brief Creates the swapchain for @p surface.
	 *
	 * @throws std::runtime_error when the surface format cannot be used as a
	 *         transfer destination, which the HDR composite blit requires.
	 */
	SwapchainTarget(const vk::raii::PhysicalDevice& physicalDevice,
	                const vk::raii::Device& device,
	                SharedQueue& presentQueue,
	                const vk::raii::SurfaceKHR& surface,
	                uint32_t width,
	                uint32_t height);

	~SwapchainTarget() override;

	SwapchainTarget(const SwapchainTarget&) = delete;
	SwapchainTarget& operator=(const SwapchainTarget&) = delete;

	vk::Extent2D Extent() const override;
	vk::Format Format() const override;
	uint32_t ImageCount() const override;
	vk::Image Image(uint32_t index) const override;
	bool Presents() const override { return true; }
	uint32_t Generation() const override;

	std::optional<uint32_t> Acquire(const vk::raii::Semaphore* imageAvailable) override;
	void Present(const vk::raii::Semaphore& renderFinished, uint32_t index) override;
	void Recreate(uint32_t width, uint32_t height) override;

	/** @brief The wrapped swapchain, for the surface-change reconstruction path. */
	Swapchain& Get() { return *m_swapchain; }

private:
	std::unique_ptr<Swapchain> m_swapchain;
};

} // namespace neurus
