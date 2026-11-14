#include "SwapchainTarget.h"

#include "SharedQueue.h"
#include "Swapchain.h"

#include <stdexcept>

namespace neurus {

SwapchainTarget::SwapchainTarget(const vk::raii::PhysicalDevice& physicalDevice,
                                 const vk::raii::Device& device,
                                 SharedQueue& presentQueue,
                                 const vk::raii::SurfaceKHR& surface,
                                 uint32_t width,
                                 uint32_t height)
	: m_swapchain(std::make_unique<Swapchain>(
		physicalDevice, device, presentQueue, surface, width, height))
{
	// The HDR composite blit writes into the swapchain image, so TRANSFER_DST
	// must be one of the usages the surface actually granted. A headless target
	// declares its own usage and never takes this path.
	const bool hasTransferDst =
		(m_swapchain->actualImageUsage() & vk::ImageUsageFlagBits::eTransferDst) != vk::ImageUsageFlags{};
	if (!hasTransferDst)
	{
		throw std::runtime_error(
			"Swapchain surface does not support VK_IMAGE_USAGE_TRANSFER_DST_BIT.\n"
			"DeferredRenderer requires TRANSFER_DST for the HDRColor-to-swapchain blit composite.\n"
			"Try running on a GPU/driver that supports this usage flag for the surface format.");
	}
}

SwapchainTarget::~SwapchainTarget() = default;

vk::Extent2D SwapchainTarget::Extent() const
{
	return m_swapchain->extent();
}

vk::Format SwapchainTarget::Format() const
{
	return m_swapchain->format();
}

uint32_t SwapchainTarget::ImageCount() const
{
	return m_swapchain->imageCount();
}

vk::Image SwapchainTarget::Image(uint32_t index) const
{
	const auto& images = m_swapchain->images();
	return index < images.size() ? images[index] : vk::Image{};
}

uint32_t SwapchainTarget::Generation() const
{
	return m_swapchain->generation();
}

std::optional<uint32_t> SwapchainTarget::Acquire(const vk::raii::Semaphore* imageAvailable)
{
	return m_swapchain->AcquireNextImage(*imageAvailable);
}

void SwapchainTarget::Present(const vk::raii::Semaphore& renderFinished, uint32_t index)
{
	m_swapchain->Present(renderFinished, index);
}

void SwapchainTarget::Recreate(uint32_t width, uint32_t height)
{
	m_swapchain->Recreate(width, height);
}

} // namespace neurus
