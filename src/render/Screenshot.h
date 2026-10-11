#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <string>

namespace neurus {

// --- Forward declarations ---
class Image;
class RenderCache;

/**
 * @brief Utility for capturing Vulkan images to PNG files.
 *
 * Provides both instance methods (for high-level screenshot orchestration)
 * and static utility methods (for low-level capture of individual images).
 *
 * All capture operations are blocking (waits for queue idle).
 */
class Screenshot
{
public:
	/**
	 * @brief Constructs a Screenshot instance for member capture methods.
	 * @param device          Logical device (must outlive this instance).
	 * @param physicalDevice  Physical device for memory allocation.
	 * @param queue           Graphics queue for command submission.
	 * @param queueFamilyIndex Queue family index for transient command pools.
	 */
	Screenshot(const vk::raii::Device& device,
	           const vk::raii::PhysicalDevice& physicalDevice,
	           vk::Queue queue,
	           uint32_t queueFamilyIndex);

	// Non-copyable, non-movable
	Screenshot(const Screenshot&) = delete;
	Screenshot& operator=(const Screenshot&) = delete;
	Screenshot(Screenshot&&) = delete;
	Screenshot& operator=(Screenshot&&) = delete;

	// -------------------------------------------------------------------
	// Instance capture methods
	// -------------------------------------------------------------------

	/**
	 * @brief Captures a swapchain image to a PNG file.
	 *
	 * Reads back the given swapchain image and writes a timestamped PNG.
	 *
	 * @param swapchainImage  The VkImage handle of the swapchain image.
	 * @param swapchainFormat Format of the swapchain image.
	 * @param swapchainExtent Extent of the swapchain image.
	 * @return true on success.
	 */
	bool TakeScreenshot(vk::Image swapchainImage,
	                    vk::Format swapchainFormat,
	                    vk::Extent2D swapchainExtent);

	/**
	 * @brief Captures all G-Buffer, shadow, and intensity attachments.
	 *
	 * Iterates G-Buffer attachments (Position, Normal, Albedo,
	 * MetallicRoughness, HDRColor, SSAO, SSR), exports shadow cubemaps
	 * as equirectangular projections, and dumps shadow intensity array
	 * layers.
	 *
	 * @param renderCache       Render cache for attachment & shadow map access.
	 * @param extent            Viewport extent for G-Buffer attachments.
	 * @param cubemapResolution Per-face resolution of shadow cubemaps.
	 * @return Number of attachments captured.
	 */
	int TakeScreenshotAllAttachments(RenderCache& renderCache,
	                                 vk::Extent2D extent,
	                                 uint32_t cubemapResolution);

	/**
	 * @brief Exports a shadow depth cubemap as an equirectangular grayscale PNG.
	 *
	 * Projects all six faces of the point-light shadow cubemap onto a 2D
	 * equirectangular image and saves it as an R8 grayscale PNG.
	 *
	 * @param renderCache       Render cache for shadow map access.
	 * @param lightUID          UID of the point light whose shadow map to export.
	 * @param filenamePrefix    Path prefix for the output PNG.
	 * @param cubemapResolution Per-face resolution of the shadow cubemap.
	 * @return Path to the saved PNG file, or empty string on failure.
	 */
	std::string ExportShadowDepthEquirect(RenderCache& renderCache,
	                                      int lightUID,
	                                      const std::string& filenamePrefix,
	                                      uint32_t cubemapResolution);

	/**
	 * @brief Exports a 2D sun light shadow depth map as a grayscale PNG.
	 *
	 * Reads back a D32_SFLOAT 2D shadow map, normalises depth values to
	 * [0, 1], and saves as an R8 grayscale PNG.  Returns empty string for
	 * cubemap shadow maps.
	 *
	 * @param renderCache     Render cache for shadow map access.
	 * @param lightUID        UID of the sun light whose shadow map to export.
	 * @param filenamePrefix  Path prefix for the output PNG.
	 * @return Path to the saved PNG file, or empty string on failure.
	 */
	std::string ExportShadowDepth(RenderCache& renderCache,
	                              int lightUID,
	                              const std::string& filenamePrefix);

	// -------------------------------------------------------------------
	// Static utility and capture methods
	// -------------------------------------------------------------------

	/**
	 * @brief Generates a timestamp string: "YYYYMMDD_HHMMSS".
	 * @return "{prefix}_YYYYMMDD_HHMMSS{suffix}"
	 */
	static std::string timestampedFilename(const std::string& prefix,
	                                       const std::string& suffix);

	/**
	 * @brief Captures a swapchain image to a PNG file.
	 *
	 * Handles layout transitions (PRESENT_SRC to TRANSFER_SRC) and reads back
	 * through a staging buffer.  BGR to RGB swizzle is applied automatically
	 * for BGRA swapchain formats.
	 *
	 * @return true on success.
	 */
	static bool CaptureSwapchain(const vk::raii::Device& device,
	                             const vk::raii::PhysicalDevice& physicalDevice,
	                             vk::Queue queue,
	                             uint32_t queueFamilyIndex,
	                             vk::Image image,
	                             vk::Format format,
	                             vk::Extent2D extent,
	                             const std::string& path);

	/**
	 * @brief Captures an Image attachment to a PNG file.
	 *
	 * Transitions the image to TRANSFER_SRC, reads it back and converts it
	 * to PNG, restoring the original layout afterwards.
	 *
	 * @return true on success.
	 */
	static bool CaptureAttachment(const vk::raii::Device& device,
	                              const vk::raii::PhysicalDevice& physicalDevice,
	                              vk::Queue queue,
	                              uint32_t queueFamilyIndex,
	                              Image& vulkanImage,
	                              const std::string& path);

	/**
	 * @brief Captures a single layer of an Image array to a PNG file.
	 *
	 * @param layerIndex 0-based layer index within the array image.
	 * @return true on success.
	 */
	static bool CaptureImageLayer(const vk::raii::Device& device,
	                              const vk::raii::PhysicalDevice& physicalDevice,
	                              vk::Queue queue,
	                              uint32_t queueFamilyIndex,
	                              Image& vulkanImage,
	                              uint32_t layerIndex,
	                              const std::string& path);

	/**
	 * @brief Captures all G-Buffer and post-FX attachments to timestamped PNGs.
	 *
	 * @return Number of attachments captured.
	 */
	static int CaptureAllAttachments(const vk::raii::Device& device,
	                                 const vk::raii::PhysicalDevice& physicalDevice,
	                                 vk::Queue queue,
	                                 uint32_t queueFamilyIndex,
	                                 RenderCache& renderCache,
	                                 vk::Extent2D extent,
	                                 const std::string& prefix);

private:
	const vk::raii::Device& m_device;
	const vk::raii::PhysicalDevice& m_physicalDevice;
	vk::Queue m_queue;
	uint32_t m_queueFamilyIndex;
};

} // namespace neurus
