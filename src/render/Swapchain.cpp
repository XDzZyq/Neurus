#include "Swapchain.h"

#include "core/Log.h"

#include <algorithm>
#include <stdexcept>

namespace neurus {

Swapchain::Swapchain(const vk::raii::PhysicalDevice& physicalDevice,
                     const vk::raii::Device& device,
                     const vk::raii::SurfaceKHR& surface,
                     uint32_t width, uint32_t height)
	: r_physicalDevice(physicalDevice)
	, r_device(device)
	, r_surface(surface)
{
	r_recreateWidth = width;
	r_recreateHeight = height;

	auto surfaceFormat = chooseSurfaceFormat(physicalDevice, surface);
	r_format = surfaceFormat.format;
	auto presentMode = choosePresentMode(physicalDevice, surface);
	r_extent = chooseExtent(physicalDevice, surface, width, height);

	// --- Get surface capabilities for image count and supported usage ---
	auto capabilities = physicalDevice.getSurfaceCapabilitiesKHR(*surface);
	uint32_t minImageCount = capabilities.minImageCount + 1;

	// Clamp to maxImageCount if it's not unlimited (0 = unlimited)
	if (capabilities.maxImageCount > 0 && minImageCount > capabilities.maxImageCount)
	{
		minImageCount = capabilities.maxImageCount;
	}

	// Determine actual image usage: intersect requested usage with surface-supported usage
	// eTransferSrc is requested for the screenshot path (Screenshot::TakeScreenshot
	// copies the presented image to a buffer); without it every capture raised
	// VUID-vkCmdCopyImageToBuffer-srcImage-00186 and read undefined contents.
	constexpr vk::ImageUsageFlags kRequestedUsage =
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst
		| vk::ImageUsageFlagBits::eTransferSrc;
	r_actualUsage = kRequestedUsage & capabilities.supportedUsageFlags;

	vk::SwapchainCreateInfoKHR swapchainCreateInfo(
		{},
		*surface,
		minImageCount,
		r_format,
		vk::ColorSpaceKHR::eSrgbNonlinear,
		r_extent,
		1,                                  // Single array layer (no stereo rendering)
		r_actualUsage,
		vk::SharingMode::eExclusive,        // Single queue family for MVP
		{},
		capabilities.currentTransform,      // Use surface's actual transform
		vk::CompositeAlphaFlagBitsKHR::eOpaque,
		presentMode,
		VK_TRUE,                            // clipped
		nullptr                             // old swapchain (null for initial creation)
	);

	r_swapchain = std::make_unique<vk::raii::SwapchainKHR>(device, swapchainCreateInfo);

	// --- Store actual values ---
	r_extent = swapchainCreateInfo.imageExtent;
	r_imageCount = minImageCount;

	NEURUS_LOG("[Swapchain] " << r_extent.width << "x" << r_extent.height
	          << " format=" << vk::to_string(r_format)
	          << " present=" << vk::to_string(presentMode)
	          << " images=" << r_imageCount
	          << " usage=" << vk::to_string(r_actualUsage));

	// --- Create image views ---
	r_images = r_swapchain->getImages();
	r_imageViews = createImageViews(device, *r_swapchain, r_format);
}

Swapchain::~Swapchain()
{
	// vk::raii handles cleanup automatically.
	// Image views destroyed first, then swapchain.
	// Order is ensured by C++ destruction order (member reverse order).
}

void Swapchain::Recreate(uint32_t width, uint32_t height)
{
	r_recreateWidth = width;
	r_recreateHeight = height;

	// Get new surface capabilities (window may have changed).
	// Query surface validity BEFORE destroying the old swapchain so that
	// a lost surface doesn't leave us with a null r_swapchain.
	auto capabilities = vk::SurfaceCapabilitiesKHR{};
	try
	{
		capabilities = r_physicalDevice.getSurfaceCapabilitiesKHR(*r_surface);
	}
	catch (const vk::SurfaceLostKHRError&)
	{
		NEURUS_LOG("[Swapchain] Surface lost during recreation - deferring.");
		return;  // Keep the old swapchain - retry next frame
	}

	// Handle minimized window (zero-area surface)
	if (capabilities.currentExtent.width == 0 || capabilities.currentExtent.height == 0)
	{
		// Wait until window is restored before recreating
		// The next DrawFrame() call will retry recreation
		return;
	}

	// Destroy old resources **after** confirming the surface is still valid
	r_imageViews.clear();
	r_swapchain.reset();

	r_extent = chooseExtent(r_physicalDevice, r_surface, width, height);
	auto presentMode = choosePresentMode(r_physicalDevice, r_surface);

	uint32_t minImageCount = capabilities.minImageCount + 1;
	if (capabilities.maxImageCount > 0 && minImageCount > capabilities.maxImageCount)
	{
		minImageCount = capabilities.maxImageCount;
	}

	// Determine actual image usage: intersect requested usage with surface-supported usage
	// eTransferSrc: see the note on the same constant in the constructor above.
	constexpr vk::ImageUsageFlags kRequestedUsage =
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst
		| vk::ImageUsageFlagBits::eTransferSrc;
	r_actualUsage = kRequestedUsage & capabilities.supportedUsageFlags;

	vk::SwapchainCreateInfoKHR swapchainCreateInfo(
		{},
		*r_surface,
		minImageCount,
		r_format,
		vk::ColorSpaceKHR::eSrgbNonlinear,
		r_extent,
		1,
		r_actualUsage,
		vk::SharingMode::eExclusive,
		{},
		vk::SurfaceTransformFlagBitsKHR::eIdentity,
		vk::CompositeAlphaFlagBitsKHR::eOpaque,
		presentMode,
		VK_TRUE,
		nullptr
	);

	r_swapchain = std::make_unique<vk::raii::SwapchainKHR>(r_device, swapchainCreateInfo);
	r_extent = swapchainCreateInfo.imageExtent;
	r_imageCount = minImageCount;

	r_images = r_swapchain->getImages();
	r_imageViews = createImageViews(r_device, *r_swapchain, r_format);

	++r_generation;
}

uint32_t Swapchain::AcquireNextImage(const vk::raii::Semaphore& signalSemaphore)
{
	try
	{
		// UINT64_MAX disables the timeout - the call blocks until an image is
		// available. This is safe because waitForFences (in DrawFrame) uses a
		// finite timeout and processEvents() keeps the Qt event loop responsive.
		auto [result, imageIndex] = r_swapchain->acquireNextImage(
			UINT64_MAX, *signalSemaphore, nullptr);

		if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR)
		{
			// Swapchain needs recreation (window resized)
			Recreate(r_recreateWidth, r_recreateHeight);
			return 0;
		}

		if (result != vk::Result::eSuccess)
		{
			throw std::runtime_error("Failed to acquire swapchain image.");
		}

		return imageIndex;
	}
	catch (const vk::OutOfDateKHRError&)
	{
		// vk::raii wrapper may throw instead of returning the error result
		Recreate(r_recreateWidth, r_recreateHeight);
		return 0;
	}
	catch (const vk::SurfaceLostKHRError&)
	{
		// Surface lost - can't acquire. Keep the old swapchain, signal caller.
		return 0;
	}
}

void Swapchain::Present(const vk::raii::Semaphore& waitSemaphore, uint32_t imageIndex, vk::Queue presentQueue)
{
	try
	{
		vk::PresentInfoKHR presentInfo(
			*waitSemaphore,
			**r_swapchain,
			imageIndex
		);

		auto result = presentQueue.presentKHR(presentInfo);

		if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR)
		{
			Recreate(r_recreateWidth, r_recreateHeight);
		}
	}
	catch (const vk::OutOfDateKHRError&)
	{
		Recreate(r_recreateWidth, r_recreateHeight);
	}
	catch (const vk::SurfaceLostKHRError&)
	{
		// Surface lost - can't present. Don't recreate (would fail).
	}
}

vk::SurfaceFormatKHR Swapchain::chooseSurfaceFormat(const vk::raii::PhysicalDevice& physicalDevice,
                                                      const vk::raii::SurfaceKHR& surface)
{
	auto formats = physicalDevice.getSurfaceFormatsKHR(*surface);

	// Prefer sRGB BGRA8
	for (const auto& format : formats)
	{
		if (format.format == vk::Format::eB8G8R8A8Srgb &&
		    format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
		{
			return format;
		}
	}

	// Fallback: first available format
	return formats[0];
}

vk::PresentModeKHR Swapchain::choosePresentMode(const vk::raii::PhysicalDevice& physicalDevice,
                                                  const vk::raii::SurfaceKHR& surface)
{
	auto presentModes = physicalDevice.getSurfacePresentModesKHR(*surface);

	// Prefer MAILBOX for low latency (tearing-free, doesn't block on vsync)
	for (const auto& mode : presentModes)
	{
		if (mode == vk::PresentModeKHR::eMailbox)
		{
			return mode;
		}
	}

	// Fallback to FIFO (guaranteed to be available, VSync)
	return vk::PresentModeKHR::eFifo;
}

vk::Extent2D Swapchain::chooseExtent(const vk::raii::PhysicalDevice& physicalDevice,
                                       const vk::raii::SurfaceKHR& surface,
                                       uint32_t width, uint32_t height)
{
	auto capabilities = physicalDevice.getSurfaceCapabilitiesKHR(*surface);

	// If the current extent is valid, use it
	if (capabilities.currentExtent.width != UINT32_MAX)
	{
		return capabilities.currentExtent;
	}

	// Otherwise clamp to min/max
	vk::Extent2D extent = {width, height};
	extent.width = std::clamp(extent.width,
	                          capabilities.minImageExtent.width,
	                          capabilities.maxImageExtent.width);
	extent.height = std::clamp(extent.height,
	                           capabilities.minImageExtent.height,
	                           capabilities.maxImageExtent.height);
	return extent;
}

std::vector<vk::raii::ImageView> Swapchain::createImageViews(const vk::raii::Device& device,
                                                               const vk::raii::SwapchainKHR& swapchain,
                                                               vk::Format format)
{
	auto images = swapchain.getImages();
	std::vector<vk::raii::ImageView> views;
	views.reserve(images.size());

	for (const auto& image : images)
	{
		vk::ImageViewCreateInfo viewCreateInfo(
			{},
			image,
			vk::ImageViewType::e2D,
			format,
			vk::ComponentMapping(),  // Identity mapping (R→R, G→G, B→B, A→A)
			vk::ImageSubresourceRange(
				vk::ImageAspectFlagBits::eColor,
				0, 1,  // base mip level, level count
				0, 1   // base array layer, layer count
			)
		);

		views.emplace_back(device, viewCreateInfo);
	}

	return views;
}

} // namespace neurus
