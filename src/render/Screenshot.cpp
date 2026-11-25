#include "Screenshot.hpp"
#include "Image.hpp"
#include "RenderCache.hpp"
#include "SharedQueue.hpp"
#include "asset/data/ImageData.hpp"
#include "asset/PixelFormat.hpp"
#include "Texture.hpp"
#include "render/Barrier.hpp"
#include "core/Log.hpp"
#include "platform/PlatformSystem.hpp"

#include "PipelineBuilder.hpp"
#include "DescriptorManager.hpp"
#include "shader/ShaderLibrary.hpp"
#include "shader/ComputeShader.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>

namespace neurus {

// ---------------------------------------------------------------------------
// Local helpers (paths and format conversion)
// ---------------------------------------------------------------------------

/**
 * @brief Narrows @p path to a UTF-8 std::string for a log line, without throwing.
 *
 * std::filesystem::path::string() converts to the *native* narrow encoding, which
 * MSVC maps to the ANSI code page: it throws std::system_error for any character
 * outside it, so a non-ANSI install directory could kill a capture from a log
 * statement. u8string() does not have that failure, and on POSIX the native
 * narrow encoding is already UTF-8, so the bytes are exactly what string() gave.
 *
 * Logging only. The PNG writer opens a narrow `const char*` through the C
 * runtime, which on Windows reads it in the ANSI code page, so the paths handed
 * to it stay path::string(): UTF-8 bytes there would break every non-ASCII path
 * the code page can represent. A path it cannot represent throws inside the
 * capture, which the render thread catches.
 */
static std::string Utf8Path(const std::filesystem::path& path)
{
	const std::u8string utf8 = path.u8string();
	return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

/**
 * @brief Formats @p path as an absolute path for logging, without throwing.
 *
 * std::filesystem::absolute() reports the current directory on the caller's
 * terms; a failure here must never cost the screenshot itself, so the original
 * path is the fallback.
 */
static std::string AbsoluteForLog(const std::filesystem::path& path)
{
	std::error_code ec;
	const std::filesystem::path abs = std::filesystem::absolute(path, ec);
	return Utf8Path(ec ? path : abs);
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
                       SharedQueue& queue,
                       uint32_t queueFamilyIndex)
	: m_device(device)
	, m_physicalDevice(physicalDevice)
	, m_queue(&queue)
	, m_queueFamilyIndex(queueFamilyIndex)
{
}

// ===========================================================================
// Instance method: TakeScreenshot
// ===========================================================================

bool Screenshot::TakeScreenshot(Image& output,
                                vk::Format swapchainFormat,
                                vk::Extent2D swapchainExtent,
                                const std::filesystem::path& directory)
{
	const std::string path = Screenshot::timestampedFilename(
		(directory / "swapchain").string(), ".png");

	// Replay the display blit so both the sRGB conversion and scaling match
	// presentation when the scene image has a different extent.
	const ImageState sourceState = output.State();
	if (sourceState == ImageState::Undefined || sourceState == ImageState::Invalid)
		return false;
	Image capture(m_device, m_physicalDevice, swapchainExtent, swapchainFormat,
	              vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
	                  vk::ImageUsageFlagBits::eTransferSrc,
	              1u, Image::ImageType::e2D, "ViewportCapture");
	vk::raii::CommandPool pool(m_device,
		vk::CommandPoolCreateInfo(vk::CommandPoolCreateFlagBits::eTransient, m_queueFamilyIndex));
	vk::raii::CommandBuffers commands(m_device,
		vk::CommandBufferAllocateInfo(*pool, vk::CommandBufferLevel::ePrimary, 1));
	auto& cmd = commands.front();
	cmd.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	Barrier::Transition(*cmd, output, ImageState::TransferSrc);
	Barrier::Transition(*cmd, capture, ImageState::TransferDst);

	vk::ImageBlit region;
	region.srcSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
	region.dstSubresource = region.srcSubresource;
	region.srcOffsets[1] = vk::Offset3D(static_cast<int32_t>(output.Extent().width),
	                                    static_cast<int32_t>(output.Extent().height), 1);
	region.dstOffsets[1] = vk::Offset3D(static_cast<int32_t>(swapchainExtent.width),
	                                    static_cast<int32_t>(swapchainExtent.height), 1);
	cmd.blitImage(*output.ImageHandle(), vk::ImageLayout::eTransferSrcOptimal,
	              *capture.ImageHandle(), vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eLinear);
	Barrier::Transition(*cmd, output, sourceState);
	Barrier::Transition(*cmd, capture, ImageState::TransferSrc);
	cmd.end();
	m_queue->SubmitAndWait(vk::SubmitInfo({}, {}, {}, 1, &(*cmd)));

	const auto data = capture.ReadImageData(m_device, m_physicalDevice, *m_queue, m_queueFamilyIndex);
	const bool saved = data && data->IsValid() && data->SavePNG(path);
	if (saved)
	{
		NEURUS_LOG("[Screenshot] saved " << AbsoluteForLog(path));
	}
	else
	{
		// The old code dropped this false on the floor, so a broken screenshot
		// looked exactly like a silent success.
		NEURUS_ERR("[Screenshot] failed to write the swapchain screenshot to "
		           << AbsoluteForLog(path));
	}
	return saved;
}

// ===========================================================================
// Instance method: TakeScreenshotAllAttachments
// ===========================================================================

int Screenshot::TakeScreenshotAllAttachments(RenderCache& renderCache,
                                             vk::Extent2D extent,
                                             uint32_t cubemapResolution,
                                             const std::filesystem::path& directory)
{
	int count = 0;

	count = Screenshot::CaptureAllAttachments(m_device, m_physicalDevice,
	                                           *m_queue, m_queueFamilyIndex,
	                                           renderCache,
	                                           extent,
	                                           (directory / "gbuffer").string());

	// --- Export shadow maps: cubemaps → equirect, 2D → direct depth readback ---
	{
		const auto shadowUIDs = renderCache.GetShadowMapUIDs();
		for (int lightUID : shadowUIDs)
		{
			// Cubemaps (point lights) only; returns empty otherwise.
			{
				const std::string result = ExportShadowDepthEquirect(
					renderCache, lightUID, (directory / "shadow_cubemap").string(),
					cubemapResolution);
				if (!result.empty())
				{
					++count;
				}
			}
			// 2D depth maps (sun lights) only; returns empty otherwise.
			{
				const std::string result = ExportShadowDepth(
					renderCache, lightUID, (directory / "sun_shadow").string());
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
				// Lights beyond MAX_SHADOW_LAYERS get no layer - nothing to export,
				// and layer 0 belongs to another light.
				const std::optional<uint32_t> layer =
					renderCache.GetShadowIntensityLayerIndex(lightUID);
				if (!layer) continue;

				const std::string path = Screenshot::timestampedFilename(
					(directory / ("shadow_intensity_Light" + std::to_string(lightUID))).string(),
					".png");
				if (Screenshot::CaptureImageLayer(m_device, m_physicalDevice,
				                                   *m_queue, m_queueFamilyIndex,
				                                   *intensityArray, *layer, path))
				{
					++count;
				}
				else
				{
					NEURUS_ERR("[Screenshot] failed to write the shadow intensity layer of "
					           << "light " << lightUID << " to " << AbsoluteForLog(path));
				}
			}
		}
	}

	if (count > 0)
	{
		NEURUS_LOG("[Screenshot] saved " << count << " attachment capture(s) under "
		                                 << AbsoluteForLog(directory));
	}
	else
	{
		NEURUS_ERR("[Screenshot] no attachment could be captured under "
		           << AbsoluteForLog(directory));
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

	// A cubemap that was never rendered has nothing to project, and there is no
	// legal "original" state to return it to afterwards (UNDEFINED may only be a
	// source layout).
	const ImageState originalState = cubemap.State();
	if (originalState == ImageState::Undefined || originalState == ImageState::Invalid)
	{
		NEURUS_ERR("[ExportShadowDepthEquirect] Cubemap for lightUID=" << lightUID
		           << " was never rendered - skipping");
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
		                                  "res/shaders/convert/c2e.comp.glsl");
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

		// Return the shadow cubemap to the state it entered in. The compute
		// shader only samples it, but leaving it in ColorShaderRead would make
		// the tracked state disagree with the layout the shadow pass expects
		// (DepthShaderRead / DEPTH_STENCIL_READ_ONLY_OPTIMAL) and every later
		// barrier would be recorded against a state the image is not in.
		{
			Barrier::Transition(*cmd, cubemap, originalState);
		}

		cmd.end();

		vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmd));
		m_queue->SubmitAndWait(submitInfo);
	}

	// --- 6. Read back equirect as grayscale PNG ---
	auto equirectData = equirectImage.ReadImageData(
		m_device, m_physicalDevice, *m_queue, m_queueFamilyIndex);

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

	// ReadImageData transitions the depth map to TRANSFER_SRC and returns it to
	// its own state within the same submission, so the shadow pass still finds
	// the layout its tracked state promises. Do not restore it here: a second,
	// caller-side restore used to be gated on the PNG save, and the gate never
	// opened because the tracked state is not changed by the readback.
	//
	// A map that was never rendered (UNDEFINED) has nothing to export and is
	// refused inside ReadImageData.
	auto depthData = depthMap.ReadImageData(
		m_device, m_physicalDevice, *m_queue, m_queueFamilyIndex);

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
	const std::tm tm = platform::LocalTime(timeT);

	std::ostringstream oss;
	oss << prefix << "_"
	    << std::put_time(&tm, "%Y%m%d_%H%M%S")
	    << suffix;
	return oss.str();
}

// ===========================================================================
// CaptureAttachment
// ===========================================================================

bool Screenshot::CaptureAttachment(const vk::raii::Device& device,
                                    const vk::raii::PhysicalDevice& physicalDevice,
                                    SharedQueue& queue,
                                    uint32_t queueFamilyIndex,
                                     Image& vulkanImage,
                                    const std::string& path)
{
	if (vkFormatByteSize(vulkanImage.Format()) == 0)
	{
		NEURUS_ERR("[Screenshot] CaptureAttachment: format " << vk::to_string(vulkanImage.Format())
		           << " cannot be read back; not writing " << AbsoluteForLog(path));
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
                                    SharedQueue& queue,
                                    uint32_t queueFamilyIndex,
                                    Image& vulkanImage,
                                    uint32_t layerIndex,
                                    const std::string& path)
{
	if (vkFormatByteSize(vulkanImage.Format()) == 0)
	{
		return false;
	}

	// Read back the specified layer via the Layer() subresource range.
	// ReadImageData restores that range to its entry state inside the same
	// submission, so the array keeps the layout its tracked state promises and
	// no restore is needed here.
	const auto layerRange = vulkanImage.Layer(layerIndex);
	auto imageData = vulkanImage.ReadImageData(
		device, physicalDevice, queue, queueFamilyIndex, &layerRange);

	if (!imageData || !imageData->IsValid())
	{
		NEURUS_ERR("[Screenshot] CaptureImageLayer: readback failed for layer " << layerIndex);
		return false;
	}

	return imageData->SavePNG(path);
}

// ===========================================================================
// CaptureAllAttachments
// ===========================================================================

int Screenshot::CaptureAllAttachments(const vk::raii::Device& device,
                                       const vk::raii::PhysicalDevice& physicalDevice,
                                       SharedQueue& queue,
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
		AttachmentName::OverlayOutput,
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
