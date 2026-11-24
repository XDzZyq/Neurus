#include "Swapchain.hpp"

#include "SharedQueue.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <stdexcept>

namespace neurus {

Swapchain::Swapchain(const vk::raii::PhysicalDevice& physicalDevice,
                     const vk::raii::Device& device,
                     SharedQueue& presentQueue,
                     const vk::raii::SurfaceKHR& surface,
                     uint32_t width, uint32_t height, bool maintenanceEnabled)
	: r_physicalDevice(physicalDevice)
	, r_device(device)
	, r_surface(surface)
	, r_presentQueue(&presentQueue)
	, r_maintenanceEnabled(maintenanceEnabled)
{
	r_recreateWidth = width;
	r_recreateHeight = height;

	auto surfaceFormat = chooseSurfaceFormat(physicalDevice, surface);
	r_format = surfaceFormat.format;
	auto presentMode = choosePresentMode(physicalDevice, surface);

	// --- Get surface capabilities for image count and supported usage ---
	auto capabilities = physicalDevice.getSurfaceCapabilitiesKHR(*surface);
	r_extent = chooseExtent(capabilities, width, height);
	uint32_t minImageCount = capabilities.minImageCount + 1;

	// Clamp to maxImageCount if it's not unlimited (0 = unlimited)
	if (capabilities.maxImageCount > 0 && minImageCount > capabilities.maxImageCount)
	{
		minImageCount = capabilities.maxImageCount;
	}

	// Determine actual image usage: intersect requested usage with surface-supported usage.
	constexpr vk::ImageUsageFlags kRequestedUsage =
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
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

	r_scalesToSurface = supportsStretch(presentMode, r_extent);
	vk::SwapchainPresentScalingCreateInfoEXT scaling(vk::PresentScalingFlagBitsEXT::eStretch);
	if (r_scalesToSurface)
		swapchainCreateInfo.pNext = &scaling;
	r_swapchain = std::make_unique<vk::raii::SwapchainKHR>(device, swapchainCreateInfo);

	// --- Store actual values ---
	r_extent = swapchainCreateInfo.imageExtent;
	r_images = r_swapchain->getImages();
	r_imageCount = static_cast<uint32_t>(r_images.size());

	NEURUS_LOG("[Swapchain] " << r_extent.width << "x" << r_extent.height
	          << " format=" << vk::to_string(r_format)
	          << " present=" << vk::to_string(presentMode)
	          << " images=" << r_imageCount
	          << " usage=" << vk::to_string(r_actualUsage));

	// --- Create image views ---
	r_imageViews = createImageViews(device, *r_swapchain, r_format);
}

Swapchain::~Swapchain()
{
	// vk::raii handles cleanup automatically; image views are destroyed before
	// the swapchain by member reverse order.
}

void Swapchain::Recreate(uint32_t width, uint32_t height)
{
	recreate(width, height, false);
}

bool Swapchain::supportsStretch(vk::PresentModeKHR mode, vk::Extent2D extent) const
{
	if (!r_maintenanceEnabled)
		return false;
	const vk::SurfacePresentModeEXT surfaceMode(mode);
	const vk::PhysicalDeviceSurfaceInfo2KHR info(*r_surface, &surfaceMode);
	const auto caps = r_physicalDevice.getSurfaceCapabilities2KHR<
		vk::SurfaceCapabilities2KHR, vk::SurfacePresentScalingCapabilitiesEXT>(info);
	const auto& scaling = caps.get<vk::SurfacePresentScalingCapabilitiesEXT>();
	return (scaling.supportedPresentScaling & vk::PresentScalingFlagBitsEXT::eStretch) &&
	       extent.width >= scaling.minScaledImageExtent.width && extent.width <= scaling.maxScaledImageExtent.width &&
	       extent.height >= scaling.minScaledImageExtent.height && extent.height <= scaling.maxScaledImageExtent.height;
}

void Swapchain::recreate(uint32_t width, uint32_t height, bool force)
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
		// Wait until the window is restored; recreation is retried next frame.
		return;
	}

	const auto newExtent = chooseExtent(capabilities, width, height);
	// An acquire or present may have already recovered at this exact size.
	// Vulkan-requested rebuilds still run even when the dimensions match.
	if (!force && newExtent == r_extent)
		return;

	vk::PresentModeKHR presentMode;
	bool stretch;
	try
	{
		presentMode = choosePresentMode(r_physicalDevice, r_surface);
		stretch = supportsStretch(presentMode, newExtent);
	}
	catch (const vk::SurfaceLostKHRError&)
	{
		return;
	}

	// Destroy old resources **after** confirming the surface is still valid.
	// Recreate() also runs straight out of Present() and AcquireNextImage(),
	// while the frame just presented is still in flight: the old swapchain may
	// only go once every queue operation on its images has completed, and
	// MoltenVK's present-completion handler still dereferences it until then.
	r_presentQueue->WaitIdle();
	r_imageViews.clear();
	r_swapchain.reset();

	r_extent = newExtent;

	uint32_t minImageCount = capabilities.minImageCount + 1;
	if (capabilities.maxImageCount > 0 && minImageCount > capabilities.maxImageCount)
	{
		minImageCount = capabilities.maxImageCount;
	}

	// Determine actual image usage: intersect requested usage with surface-supported usage
	constexpr vk::ImageUsageFlags kRequestedUsage =
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
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
		capabilities.currentTransform,
		vk::CompositeAlphaFlagBitsKHR::eOpaque,
		presentMode,
		VK_TRUE,
		nullptr
	);

	vk::SwapchainPresentScalingCreateInfoEXT scaling(vk::PresentScalingFlagBitsEXT::eStretch);
	if (stretch)
		swapchainCreateInfo.pNext = &scaling;
	r_swapchain = std::make_unique<vk::raii::SwapchainKHR>(r_device, swapchainCreateInfo);
	r_scalesToSurface = stretch;
	r_extent = swapchainCreateInfo.imageExtent;

	r_images = r_swapchain->getImages();
	r_imageCount = static_cast<uint32_t>(r_images.size());
	r_imageViews = createImageViews(r_device, *r_swapchain, r_format);

	++r_generation;
}

std::optional<uint32_t> Swapchain::AcquireNextImage(const vk::raii::Semaphore& signalSemaphore)
{
	try
	{
		// UINT64_MAX disables the timeout - the call blocks until an image is
		// available.
		auto [result, imageIndex] = r_swapchain->acquireNextImage(
			UINT64_MAX, *signalSemaphore, nullptr);

		// Suboptimal still acquired an image and signalled the semaphore, so it
		// must be rendered and presented like any other. Recreating here would
		// drop the acquired image and leave its semaphore signalled for the next
		// acquire to signal again. The present path decides whether to rebuild.
		if (result == vk::Result::eSuboptimalKHR)
		{
			return imageIndex;
		}

		if (result == vk::Result::eErrorOutOfDateKHR)
		{
			recreate(r_recreateWidth, r_recreateHeight, true);
			return std::nullopt;
		}

		if (result == vk::Result::eErrorSurfaceLostKHR)
		{
			// The non-throwing form of the same failure the catch below handles.
			NEURUS_LOG("[Swapchain] Surface lost during acquire - deferring.");
			return std::nullopt;
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
		recreate(r_recreateWidth, r_recreateHeight, true);
		return std::nullopt;
	}
	catch (const vk::SurfaceLostKHRError&)
	{
		// Nothing was acquired and the semaphore stayed unsignalled. Reporting
		// index 0 as if it were an acquisition would make the caller submit a
		// frame that waits on a never-signalled semaphore and presents an image
		// it never acquired (VUID-vkQueueSubmit-pWaitSemaphores-03238 and
		// VUID-VkPresentInfoKHR-pImageIndices-01430). Keep the swapchain and let
		// the surface-change path rebuild it.
		NEURUS_LOG("[Swapchain] Surface lost during acquire - deferring.");
		return std::nullopt;
	}
}

void Swapchain::Present(const vk::raii::Semaphore& waitSemaphore, uint32_t imageIndex)
{
	try
	{
		vk::PresentInfoKHR presentInfo(
			*waitSemaphore,
			**r_swapchain,
			imageIndex
		);

		auto result = r_presentQueue->Present(presentInfo);

		if (result == vk::Result::eErrorOutOfDateKHR ||
		    (result == vk::Result::eSuboptimalKHR && !r_scalesToSurface))
		{
			recreate(r_recreateWidth, r_recreateHeight, true);
		}
	}
	catch (const vk::OutOfDateKHRError&)
	{
		recreate(r_recreateWidth, r_recreateHeight, true);
	}
	catch (const vk::SurfaceLostKHRError&)
	{
		// No queue operation can present to a surface that no longer exists; the
		// surface-change path owns recovery. Log it - a silent catch hid this.
		NEURUS_LOG("[Swapchain] Surface lost during present - dropping the frame.");
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

vk::Extent2D Swapchain::chooseExtent(const vk::SurfaceCapabilitiesKHR& capabilities,
                                       uint32_t width, uint32_t height)
{
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
			vk::ComponentMapping(),  // Identity mapping
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
