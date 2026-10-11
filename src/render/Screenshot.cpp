#include "Screenshot.h"
#include "Image.h"
#include "RenderCache.h"
#include "asset/data/ImageData.h"
#include "asset/PixelFormat.h"
#include "Texture.h"
#include "render/Barrier.h"
#include "core/Log.h"

#include "PipelineBuilder.h"
#include "DescriptorManager.h"
#include "shaders/ShaderLibrary.h"
#include "shaders/ComputeShader.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace neurus {

// ---------------------------------------------------------------------------
// Local helpers (format conversion)
// ---------------------------------------------------------------------------

/**
 * @brief Converts a Vulkan vk::Format to the CPU-side PixelFormat equivalent.
 */
static PixelFormat vkFormatToPixelFormat(vk::Format format)
{
	switch (format)
	{
	case vk::Format::eUndefined:            return PixelFormat::Undefined;
	case vk::Format::eR8G8B8A8Unorm:        return PixelFormat::RGBA8U;
	case vk::Format::eR8G8B8A8Srgb:         return PixelFormat::RGBA8S;
	case vk::Format::eR32G32B32A32Sfloat:   return PixelFormat::RGBA32F;
	case vk::Format::eR16G16B16A16Sfloat:   return PixelFormat::RGBA16F;
	case vk::Format::eR8Unorm:              return PixelFormat::R8U;
	case vk::Format::eD32Sfloat:            return PixelFormat::D32F;
	case vk::Format::eB8G8R8A8Unorm:        return PixelFormat::BGRA8U;
	case vk::Format::eB8G8R8A8Srgb:         return PixelFormat::BGRA8S;
	case vk::Format::eR16G16B16A16Unorm:    return PixelFormat::RGBA16U;
	case vk::Format::eR16G16B16A16Snorm:    return PixelFormat::RGBA16SN;
	case vk::Format::eR8Srgb:               return PixelFormat::R8S;
	case vk::Format::eR32Uint:              return PixelFormat::R32U;
	default:                                return PixelFormat::Undefined;
	}
}

/**
 * @brief Returns bytes per pixel for a Vulkan vk::Format.
 */
static uint32_t vkFormatByteSize(vk::Format format)
{
	switch (format)
	{
	case vk::Format::eR8G8B8A8Unorm:
	case vk::Format::eR8G8B8A8Srgb:
	case vk::Format::eB8G8R8A8Unorm:
	case vk::Format::eB8G8R8A8Srgb:
		return 4;
	case vk::Format::eR16G16B16A16Sfloat:
	case vk::Format::eR16G16B16A16Unorm:
	case vk::Format::eR16G16B16A16Snorm:
		return 8;
	case vk::Format::eR32G32B32A32Sfloat:
		return 16;
	case vk::Format::eR8Unorm:
	case vk::Format::eR8Srgb:
		return 1;
	case vk::Format::eD32Sfloat:
		return 4;
	case vk::Format::eR32Uint:
		return 4;
	default:
		return 0;
	}
}

// ===========================================================================
// Constructor
// ===========================================================================

Screenshot::Screenshot(const vk::raii::Device& device,
                       const vk::raii::PhysicalDevice& physicalDevice,
                       vk::Queue queue,
                       uint32_t queueFamilyIndex)
	: m_device(device)
	, m_physicalDevice(physicalDevice)
	, m_queue(queue)
	, m_queueFamilyIndex(queueFamilyIndex)
{
}

// ===========================================================================
// Instance method: TakeScreenshot
// ===========================================================================

bool Screenshot::TakeScreenshot(vk::Image swapchainImage,
                                vk::Format swapchainFormat,
                                vk::Extent2D swapchainExtent)
{
	const std::string path = Screenshot::timestampedFilename("screenshots/swapchain", ".png");

	return Screenshot::CaptureSwapchain(m_device, m_physicalDevice,
	                                     m_queue, m_queueFamilyIndex,
	                                     swapchainImage, swapchainFormat,
	                                     swapchainExtent, path);
}

// ===========================================================================
// Instance method: TakeScreenshotAllAttachments
// ===========================================================================

int Screenshot::TakeScreenshotAllAttachments(RenderCache& renderCache,
                                             vk::Extent2D extent,
                                             uint32_t cubemapResolution)
{
	int count = 0;

	count = Screenshot::CaptureAllAttachments(m_device, m_physicalDevice,
	                                           m_queue, m_queueFamilyIndex,
	                                           renderCache,
	                                           extent,
	                                           "screenshots/gbuffer");

	// --- Export shadow maps: cubemaps → equirect, 2D → direct depth readback ---
	{
		const auto shadowUIDs = renderCache.GetShadowMapUIDs();
		for (int lightUID : shadowUIDs)
		{
			// Cubemaps (point lights) only; returns empty otherwise.
			{
				const std::string result = ExportShadowDepthEquirect(
					renderCache, lightUID, "screenshots/shadow_cubemap", cubemapResolution);
				if (!result.empty())
				{
					++count;
				}
			}
			// 2D depth maps (sun lights) only; returns empty otherwise.
			{
				const std::string result = ExportShadowDepth(
					renderCache, lightUID, "screenshots/sun_shadow");
				if (!result.empty())
				{
					++count;
				}
			}
		}
	}

	// --- Export shadow intensity array layers ---
	{
		Image* intensityArray = renderCache.GetShadowIntensityArray();
		if (intensityArray)
		{
			const auto shadowUIDs = renderCache.GetShadowMapUIDs();
			for (int lightUID : shadowUIDs)
			{
				const uint32_t layer = renderCache.GetShadowIntensityLayerIndex(lightUID);
				const std::string path = Screenshot::timestampedFilename(
					"screenshots/shadow_intensity_Light" + std::to_string(lightUID), ".png");
				if (Screenshot::CaptureImageLayer(m_device, m_physicalDevice,
				                                   m_queue, m_queueFamilyIndex,
				                                   *intensityArray, layer, path))
				{
					++count;
				}
			}
		}
	}

	return count;
}

// ===========================================================================
// C2E — Shadow cubemap → Equirectangular export
// ===========================================================================

std::string Screenshot::ExportShadowDepthEquirect(RenderCache& renderCache,
                                                  const int lightUID,
                                                  const std::string& filenamePrefix,
                                                  const uint32_t cubemapResolution)
{
	LightGPU* lgpu = renderCache.GetLightGPU(lightUID);
	if (!lgpu || !lgpu->shadowDepthMap) return {};
	auto& cubemap = *lgpu->shadowDepthMap;

	// Cubemaps only; 2D shadow maps are handled by ExportShadowDepth().
	if (cubemap.Type() != Image::ImageType::eCube)
	{
		return {};
	}

	const uint32_t cubeRes = cubemapResolution;
	const uint32_t equiWidth = cubeRes * 2;
	const uint32_t equiHeight = cubeRes;

	// --- 1. Create temporary equirect output image (rgba32f) ---
	Image equirectImage(m_device, m_physicalDevice,
	                    vk::Extent2D{equiWidth, equiHeight},
	                    vk::Format::eR32G32B32A32Sfloat,
	                    vk::ImageUsageFlagBits::eStorage |
	                        vk::ImageUsageFlagBits::eTransferSrc,
	                    1u, Image::ImageType::e2D,
	                    "ShadowEquirectTemp");

	// --- 2. Create sampler for depth cubemap ---
	vk::SamplerCreateInfo samplerCI(
		{}, vk::Filter::eNearest, vk::Filter::eNearest,
		vk::SamplerMipmapMode::eNearest,
		vk::SamplerAddressMode::eClampToEdge,
		vk::SamplerAddressMode::eClampToEdge,
		vk::SamplerAddressMode::eClampToEdge,
		0.0f, VK_FALSE, 0.0f, VK_FALSE,
		vk::CompareOp::eAlways,
		0.0f, 0.0f, vk::BorderColor::eFloatTransparentBlack, VK_FALSE);
	vk::raii::Sampler cubeSampler(m_device, samplerCI);

	// --- 3. Descriptor set layout (2 bindings) ---
	DescriptorSetLayout c2eLayout = BuildLayout()
		.AddBinding(0, vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		.AddBinding(1, vk::DescriptorType::eStorageImage,
		            vk::ShaderStageFlagBits::eCompute)
		.Build(m_device);

	DescriptorPool c2ePool(m_device, 1,
		DescriptorPool::CalculatePoolSizes({&c2eLayout}, 1));
	auto c2eSet = std::move(c2ePool.Allocate(c2eLayout, 1).front());

	{
		vk::DescriptorImageInfo cubeInfo(cubeSampler, *cubemap.ImageViewHandle(),
		                                  vk::ImageLayout::eShaderReadOnlyOptimal);
		c2eSet.WriteImage(0, cubeInfo, vk::DescriptorType::eCombinedImageSampler);

		vk::DescriptorImageInfo equiInfo(nullptr, *equirectImage.ImageViewHandle(),
		                                  vk::ImageLayout::eGeneral);
		c2eSet.WriteImage(1, equiInfo, vk::DescriptorType::eStorageImage);
	}

	// --- 4. Load compute shader via ShaderLibrary ---
	auto c2eShader =
		ShaderLibrary::LoadComputeShader("c2e_export",
		                                  "res/shaders/convert/c2e.comp");
	if (!c2eShader)
	{
		throw std::runtime_error("[Screenshot] Failed to load c2e compute shader");
	}

	auto c2eSpv = ShaderLibrary::Compile(c2eShader->GetStage(ShaderType::COMPUTE),
	                                     ShaderType::COMPUTE, "c2e_export");
	vk::ShaderModuleCreateInfo c2eSmCI({}, c2eSpv);
	vk::raii::ShaderModule c2eModule(m_device, c2eSmCI);
	vk::PipelineShaderStageCreateInfo c2eStageCI({}, vk::ShaderStageFlagBits::eCompute, *c2eModule, "main");

	PipelineBuilder c2eBuilder;
	c2eBuilder.AddShaderStage(c2eStageCI);
	c2eBuilder.SetDebugName("Screenshot::CubemapToEquirect");
	c2eBuilder.AddDescriptorSetLayout(*c2eLayout.layout());

	Pipeline c2ePipeline = c2eBuilder.BuildComputePipeline(m_device);

	// --- 5. Record & dispatch ---
	{
		vk::CommandPoolCreateInfo poolCI(vk::CommandPoolCreateFlagBits::eTransient,
		                                  m_queueFamilyIndex);
		vk::raii::CommandPool cmdPool(m_device, poolCI);
		vk::CommandBufferAllocateInfo allocInfo(*cmdPool, vk::CommandBufferLevel::ePrimary, 1);
		vk::raii::CommandBuffers cmdBufs(m_device, allocInfo);

		auto& cmd = cmdBufs[0];
		cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

		// Transition cubemap → SHADER_READ_ONLY
		{
			Barrier::Transition(*cmd, cubemap, ImageState::ColorShaderRead);
		}

		// Transition equirect → GENERAL
		{
			Barrier::Transition(*cmd, equirectImage, ImageState::ShaderWrite);
		}

		// Bind and dispatch
		cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *c2ePipeline.pipeline);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
		                       *c2ePipeline.pipelineLayout, 0,
		                       {c2eSet.handle()}, {});

		uint32_t gx = (equiWidth  + 15) / 16;
		uint32_t gy = (equiHeight + 15) / 16;
		cmd.dispatch(gx, gy, 1);

		// Transition equirect → TRANSFER_SRC for readback
		{
			Barrier::Transition(*cmd, equirectImage, ImageState::TransferSrc);
		}

		cmd.end();

		vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmd));
		m_queue.submit(submitInfo);
		m_queue.waitIdle();
	}

	// --- 6. Read back equirect as grayscale PNG ---
	auto equirectData = equirectImage.ReadImageData(
		m_device, m_physicalDevice, m_queue, m_queueFamilyIndex);

	if (!equirectData || !equirectData->IsValid())
	{
		NEURUS_ERR("[ExportShadowDepthEquirect] Readback failed for lightUID=" << lightUID);
		return {};
	}

	const auto& rawPixelData = equirectData->GetPixelData();
	const size_t pixelCount = static_cast<size_t>(equiWidth) * equiHeight;
	std::vector<uint8_t> grayPixels(pixelCount);

	for (size_t i = 0; i < pixelCount; ++i)
	{
		float r;
		std::memcpy(&r, &rawPixelData[i * 16], sizeof(float));
		r = std::max(0.0f, std::min(1.0f, r));
		grayPixels[i] = static_cast<uint8_t>(r * 255.0f + 0.5f);
	}

	const std::string path = Screenshot::timestampedFilename(
		filenamePrefix + "_Light" + std::to_string(lightUID), ".png");

	ImageData grayImg(grayPixels.data(), equiWidth, equiHeight, PixelFormat::R8U);
	const bool saved = grayImg.SavePNG(path);

	if (saved)
	{
		NEURUS_LOG("[ExportShadowDepthEquirect] Saved " << path
		           << " (" << equiWidth << "x" << equiHeight << ")");
		return path;
	}
	return {};
}

// ===========================================================================
// ExportShadowDepth — 2D sun light shadow depth map → grayscale PNG
// ===========================================================================

std::string Screenshot::ExportShadowDepth(RenderCache& renderCache,
                                          const int lightUID,
                                          const std::string& filenamePrefix)
{
	LightGPU* slgpu = renderCache.GetLightGPU(lightUID);
	if (!slgpu || !slgpu->shadowDepthMap) return {};
	auto& depthMap = *slgpu->shadowDepthMap;

	if (depthMap.Type() == Image::ImageType::eCube)
	{
		return {}; // This method only handles 2D depth maps
	}

	const vk::Extent2D extent = depthMap.Extent();
	const auto originalState = depthMap.State();

	auto depthData = depthMap.ReadImageData(
		m_device, m_physicalDevice, m_queue, m_queueFamilyIndex);

	if (!depthData || !depthData->IsValid())
	{
		NEURUS_ERR("[ExportShadowDepth] Readback failed for 2D sun shadow lightUID=" << lightUID);
		return {};
	}

	// Convert D32 float depth → R8 grayscale
	const auto& rawPixels = depthData->GetPixelData();
	const size_t pixelCount = static_cast<size_t>(extent.width) * extent.height;
	std::vector<uint8_t> grayPixels(pixelCount);

	for (size_t i = 0; i < pixelCount; ++i)
	{
		float d;
		std::memcpy(&d, &rawPixels[i * sizeof(float)], sizeof(float));
		d = std::max(0.0f, std::min(1.0f, d));
		grayPixels[i] = static_cast<uint8_t>(d * 255.0f + 0.5f);
	}

	const std::string path = Screenshot::timestampedFilename(
		filenamePrefix + "_Light" + std::to_string(lightUID), ".png");

	ImageData grayImg(grayPixels.data(), extent.width, extent.height,
	                   PixelFormat::R8U);
	const bool saved = grayImg.SavePNG(path);

	// Restore the original layout - ReadImageData transitions to TransferSrc.
	if (saved && depthMap.State() != originalState)
	{
		vk::CommandPoolCreateInfo poolCI(vk::CommandPoolCreateFlagBits::eTransient,
		                                  m_queueFamilyIndex);
		vk::raii::CommandPool cmdPool(m_device, poolCI);
		vk::CommandBufferAllocateInfo allocInfo(*cmdPool, vk::CommandBufferLevel::ePrimary, 1);
		vk::raii::CommandBuffers cmdBufs(m_device, allocInfo);

		cmdBufs[0].begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
		Barrier::Transition(*cmdBufs[0], depthMap, originalState);
		cmdBufs[0].end();

		vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmdBufs[0]));
		m_queue.submit(submitInfo);
		m_queue.waitIdle();
	}

	if (saved)
	{
		NEURUS_LOG("[ExportShadowDepth] Saved " << path
		           << " (" << extent.width << "x" << extent.height << ")");
		return path;
	}
	return {};
}

// ===========================================================================
// Timestamp helper
// ===========================================================================

std::string Screenshot::timestampedFilename(const std::string& prefix,
                                            const std::string& suffix)
{
	const auto now = std::chrono::system_clock::now();
	const auto timeT = std::chrono::system_clock::to_time_t(now);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &timeT);
#else
	localtime_r(&timeT, &tm);
#endif

	std::ostringstream oss;
	oss << prefix << "_"
	    << std::put_time(&tm, "%Y%m%d_%H%M%S")
	    << suffix;
	return oss.str();
}

// ===========================================================================
// CaptureSwapchain
// ===========================================================================

bool Screenshot::CaptureSwapchain(const vk::raii::Device& device,
                                   const vk::raii::PhysicalDevice& physicalDevice,
                                   vk::Queue queue,
                                   uint32_t queueFamilyIndex,
                                   vk::Image image,
                                   vk::Format format,
                                   vk::Extent2D extent,
                                   const std::string& path)
{
	if (vkFormatByteSize(format) == 0)
	{
		return false;
	}

	// --- 1. Transition PRESENT_SRC → TRANSFER_SRC ---
	{
		vk::CommandPoolCreateInfo poolCI(vk::CommandPoolCreateFlagBits::eTransient,
		                                 queueFamilyIndex);
		vk::raii::CommandPool cmdPool(device, poolCI);
		vk::CommandBufferAllocateInfo allocInfo(*cmdPool, vk::CommandBufferLevel::ePrimary, 1);
		vk::raii::CommandBuffers cmdBufs(device, allocInfo);

		cmdBufs[0].begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

		vk::ImageMemoryBarrier barrier(
			vk::AccessFlagBits::eMemoryRead,
			vk::AccessFlagBits::eTransferRead,
			vk::ImageLayout::ePresentSrcKHR,
			vk::ImageLayout::eTransferSrcOptimal,
			VK_QUEUE_FAMILY_IGNORED,
			VK_QUEUE_FAMILY_IGNORED,
			image,
			vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
		cmdBufs[0].pipelineBarrier(vk::PipelineStageFlagBits::eBottomOfPipe,
		                           vk::PipelineStageFlagBits::eTransfer,
		                           {}, {}, {}, barrier);

		cmdBufs[0].end();

		vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmdBufs[0]));
		queue.submit(submitInfo);
		queue.waitIdle();
	}

	// --- 2. Read back via staging buffer ---
	const uint32_t bytesPerPixel = [&]() -> uint32_t {
		switch (format)
		{
		case vk::Format::eR8G8B8A8Unorm:
		case vk::Format::eR8G8B8A8Srgb:
		case vk::Format::eB8G8R8A8Unorm:
		case vk::Format::eB8G8R8A8Srgb:
			return 4;
		case vk::Format::eR16G16B16A16Sfloat:
		case vk::Format::eR16G16B16A16Unorm:
		case vk::Format::eR16G16B16A16Snorm:
			return 8;
		case vk::Format::eR32G32B32A32Sfloat:
			return 16;
		case vk::Format::eR8Unorm:
		case vk::Format::eR8Srgb:
			return 1;
		default:
			return 0;
		}
	}();

	const vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(extent.width) *
	                                 extent.height * bytesPerPixel;

	// --- Staging buffer ---
	vk::BufferCreateInfo stagingCI({}, imageSize, vk::BufferUsageFlagBits::eTransferDst);
	vk::raii::Buffer stagingBuffer(device, stagingCI);

	auto stagingMemReqs = stagingBuffer.getMemoryRequirements();
	uint32_t stagingMemType = [&]() -> uint32_t {
		const auto memProps = physicalDevice.getMemoryProperties();
		for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
		{
			if ((stagingMemReqs.memoryTypeBits & (1u << i)) &&
			    (memProps.memoryTypes[i].propertyFlags &
			     (vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent)) ==
			        (vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent))
			{
				return i;
			}
		}
		NEURUS_ERR("[Screenshot] Failed to find memory type for staging buffer");
		return 0;
	}();

	vk::MemoryAllocateInfo stagingAlloc(stagingMemReqs.size, stagingMemType);
	vk::raii::DeviceMemory stagingMemory(device, stagingAlloc);
	stagingBuffer.bindMemory(*stagingMemory, 0);

	// --- Transient command buffer ---
	{
		vk::CommandPoolCreateInfo poolCI(vk::CommandPoolCreateFlagBits::eTransient,
		                                 queueFamilyIndex);
		vk::raii::CommandPool cmdPool(device, poolCI);
		vk::CommandBufferAllocateInfo allocInfo(*cmdPool, vk::CommandBufferLevel::ePrimary, 1);
		vk::raii::CommandBuffers cmdBufs(device, allocInfo);

		cmdBufs[0].begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

		vk::BufferImageCopy copyRegion;
		copyRegion.bufferOffset = 0;
		copyRegion.bufferRowLength = 0;
		copyRegion.bufferImageHeight = 0;
		copyRegion.imageSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
		copyRegion.imageOffset = vk::Offset3D(0, 0, 0);
		copyRegion.imageExtent = vk::Extent3D(extent.width, extent.height, 1);

		cmdBufs[0].copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, *stagingBuffer, copyRegion);

		vk::MemoryBarrier barrier(vk::AccessFlagBits::eTransferWrite,
		                          vk::AccessFlagBits::eHostRead);
		cmdBufs[0].pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                           vk::PipelineStageFlagBits::eHost,
		                           {}, {barrier}, {}, {});

		cmdBufs[0].end();

		vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmdBufs[0]));
		queue.submit(submitInfo);
		queue.waitIdle();
	}

	std::vector<uint8_t> rawData(static_cast<size_t>(imageSize));
	void* mapped = stagingMemory.mapMemory(0, imageSize);
	std::memcpy(rawData.data(), mapped, static_cast<size_t>(imageSize));
	stagingMemory.unmapMemory();

	// --- 3. Transition back TRANSFER_SRC → PRESENT_SRC ---
	{
		vk::CommandPoolCreateInfo poolCI(vk::CommandPoolCreateFlagBits::eTransient,
		                                 queueFamilyIndex);
		vk::raii::CommandPool cmdPool(device, poolCI);
		vk::CommandBufferAllocateInfo allocInfo(*cmdPool, vk::CommandBufferLevel::ePrimary, 1);
		vk::raii::CommandBuffers cmdBufs(device, allocInfo);

		cmdBufs[0].begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

		vk::ImageMemoryBarrier barrier(
			vk::AccessFlagBits::eTransferRead,
			vk::AccessFlagBits::eMemoryRead,
			vk::ImageLayout::eTransferSrcOptimal,
			vk::ImageLayout::ePresentSrcKHR,
			VK_QUEUE_FAMILY_IGNORED,
			VK_QUEUE_FAMILY_IGNORED,
			image,
			vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
		cmdBufs[0].pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
		                           vk::PipelineStageFlagBits::eBottomOfPipe,
		                           {}, {}, {}, barrier);

		cmdBufs[0].end();

		vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmdBufs[0]));
		queue.submit(submitInfo);
		queue.waitIdle();
	}

	// --- 4. Delegate PNG write to ImageData ---
	ImageData imgData(rawData.data(), extent.width, extent.height, vkFormatToPixelFormat(format));
	return imgData.SavePNG(path);
}

// ===========================================================================
// CaptureAttachment
// ===========================================================================

bool Screenshot::CaptureAttachment(const vk::raii::Device& device,
                                    const vk::raii::PhysicalDevice& physicalDevice,
                                    vk::Queue queue,
                                    uint32_t queueFamilyIndex,
                                     Image& vulkanImage,
                                    const std::string& path)
{
	if (vkFormatByteSize(vulkanImage.Format()) == 0)
	{
		return false;
	}

	return Texture::SaveImage(vulkanImage, device, physicalDevice,
	                               queue, queueFamilyIndex, path);
}

// ===========================================================================
// CaptureImageLayer
// ===========================================================================

bool Screenshot::CaptureImageLayer(const vk::raii::Device& device,
                                    const vk::raii::PhysicalDevice& physicalDevice,
                                    vk::Queue queue,
                                    uint32_t queueFamilyIndex,
                                    Image& vulkanImage,
                                    uint32_t layerIndex,
                                    const std::string& path)
{
	if (vkFormatByteSize(vulkanImage.Format()) == 0)
	{
		return false;
	}

	// Save the original layout - ReadImageData transitions the image to
	// TRANSFER_SRC_OPTIMAL for readback and does not restore it.
	const auto originalState = vulkanImage.State();

	// Read back the specified layer via the Layer() subresource range.
	const auto layerRange = vulkanImage.Layer(layerIndex);
	auto imageData = vulkanImage.ReadImageData(
		device, physicalDevice, queue, queueFamilyIndex, &layerRange);

	if (!imageData || !imageData->IsValid())
	{
		NEURUS_ERR("[Screenshot] CaptureImageLayer: readback failed for layer " << layerIndex);
		return false;
	}

	const bool saved = imageData->SavePNG(path);

	// Restore original layout so rendering can continue without validation errors.
	// ReadImageData transitions to TransferSrc but the image must be returned
	// to its original state (e.g., ShaderRead for ShadowIntensityArray).
	if (saved && vulkanImage.State() != originalState)
	{
		vk::CommandPoolCreateInfo poolCI(vk::CommandPoolCreateFlagBits::eTransient,
		                                  queueFamilyIndex);
		vk::raii::CommandPool cmdPool(device, poolCI);
		vk::CommandBufferAllocateInfo allocInfo(*cmdPool, vk::CommandBufferLevel::ePrimary, 1);
		vk::raii::CommandBuffers cmdBufs(device, allocInfo);

		cmdBufs[0].begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
		Barrier::Transition(*cmdBufs[0], vulkanImage, originalState);
		cmdBufs[0].end();

		vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmdBufs[0]));
		queue.submit(submitInfo);
		queue.waitIdle();
	}

	return saved;
}

// ===========================================================================
// CaptureAllAttachments
// ===========================================================================

int Screenshot::CaptureAllAttachments(const vk::raii::Device& device,
                                       const vk::raii::PhysicalDevice& physicalDevice,
                                       vk::Queue queue,
                                       uint32_t queueFamilyIndex,
                                       RenderCache& renderCache,
                                       vk::Extent2D extent,
                                       const std::string& prefix)
{
	static constexpr AttachmentName kAttachmentNames[] = {
		AttachmentName::Position,
		AttachmentName::Normal,
		AttachmentName::Albedo,
		AttachmentName::MetallicRoughness,
		AttachmentName::IDBuffer,
		AttachmentName::HDRColor,
		AttachmentName::SSAO,
		AttachmentName::SSR,
	};

	int capturedCount = 0;

	for (const auto name : kAttachmentNames)
	{
		if (!renderCache.HasAttachment(name))
		{
			continue;
		}

		Image& image = renderCache.GetAttachment(name, extent);

		// Skip attachments that have never been written (current layout UNDEFINED).
		// Capturing them would leave them in TRANSFER_SRC_OPTIMAL, causing
		// validation errors when a subsequent render pass expects a usable layout.
		if (image.State() == ImageState::Undefined)
		{
			NEURUS_LOG("[Screenshot] Skipping " << AttachmentNameToString(name)
			           << " - layout is UNDEFINED (not yet written)");
			continue;
		}

		const std::string fileName = timestampedFilename(
			prefix + "_" + AttachmentNameToString(name), ".png");

		if (CaptureAttachment(device, physicalDevice, queue, queueFamilyIndex,
		                      image, fileName))
		{
			++capturedCount;
		}
	}

	return capturedCount;
}

} // namespace neurus
