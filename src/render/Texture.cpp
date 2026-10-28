#include "Texture.h"
#include "buffers/StagingBuffer.h"
#include "asset/data/ImageData.h"
#include "render/Barrier.h"
#include "render/SharedQueue.h"

#include "core/Log.h"

#include <stb_image.h>

#include <stdexcept>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

namespace neurus {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

/**
 * @brief Returns the byte size of a single pixel for a given format.
 */
static vk::DeviceSize pixelByteSize(vk::Format format)
{
	switch (format)
	{
	case vk::Format::eR8G8B8A8Srgb:
	case vk::Format::eR8G8B8A8Unorm:
		return 4;
	case vk::Format::eR32G32B32A32Sfloat:
		return 16;
	case vk::Format::eR32G32B32Sfloat:
		return 12;
	case vk::Format::eR32G32Sfloat:
		return 8;
	case vk::Format::eR32Sfloat:
		return 4;
	default:
		// Conservative fallback - treat as RGBA8
		return 4;
	}
}

/**
 * @brief Creates a vk::raii::Sampler from the given config and mip level count.
 */
static vk::raii::Sampler createSampler(const vk::raii::Device& device,
                                       const SamplerConfig& config,
                                       uint32_t mipLevels)
{
	if (mipLevels == 0)
	{
		mipLevels = 1;
	}

	const vk::SamplerCreateInfo samplerCI(
		{},                                          // flags
		config.magFilter,
		config.minFilter,
		config.mipmapMode,
		config.addressModeU,
		config.addressModeV,
		config.addressModeW,
		0.0f,                                        // mipLodBias
		VK_FALSE,                                    // anisotropyEnable
		0.0f,                                        // maxAnisotropy
		VK_FALSE,                                    // compareEnable
		vk::CompareOp::eNever,
		0.0f,                                        // minLod
		static_cast<float>(mipLevels),               // maxLod
		vk::BorderColor::eFloatOpaqueBlack,
		VK_FALSE);                                   // unnormalizedCoordinates

	return vk::raii::Sampler(device, samplerCI);
}

// ---------------------------------------------------------------------------
// Factory: FromFile
// ---------------------------------------------------------------------------

Texture Texture::FromFile(const vk::raii::Device& device,
                          const vk::raii::PhysicalDevice& physicalDevice,
                          SharedQueue& queue,
                          const uint32_t queueFamilyIndex,
                          const char* path,
                          const vk::Format format,
                          const SamplerConfig& config)
{
	int width = 0, height = 0, channels = 0;
	stbi_uc* data = nullptr;
	float* dataf = nullptr;
	const void* pixelData = nullptr;
	vk::DeviceSize dataSize = 0;

	// Determine whether to load as HDR (float) or LDR (byte)
	// Auto-detect from file extension (.hdr, .exr) or explicit float format
	const std::string pathStr(path);
	const bool isHdrExt = (pathStr.find(".hdr") != std::string::npos) ||
	                      (pathStr.find(".HDR") != std::string::npos);
	const bool isHdr = isHdrExt || (format == vk::Format::eR32G32B32A32Sfloat);

	if (isHdr)
	{
		dataf = stbi_loadf(path, &width, &height, &channels, 4); // force RGBA
		pixelData = dataf;
	}
	else
	{
		data = stbi_load(path, &width, &height, &channels, 4); // force RGBA
		pixelData = data;
	}

	if (!pixelData || width <= 0 || height <= 0)
	{
		// Free if partial allocation occurred
		if (isHdr) { stbi_image_free(dataf); }
		else       { stbi_image_free(data); }
		return Texture{}; // invalid
	}

	// Force HDR format when auto-detected from extension
	const vk::Format effectiveFormat = (isHdrExt && !isHdr) ? vk::Format::eR32G32B32A32Sfloat : format;

	dataSize = static_cast<vk::DeviceSize>(width) * static_cast<vk::DeviceSize>(height) * pixelByteSize(effectiveFormat);

	Texture tex = createFromPixelData(device, physicalDevice, queue, queueFamilyIndex,
	                                  static_cast<uint32_t>(width),
	                                  static_cast<uint32_t>(height),
	                                  pixelData, dataSize, effectiveFormat,
	                                  /*generateMipmaps=*/true, config);

	if (isHdr) { stbi_image_free(dataf); }
	else       { stbi_image_free(data); }

	return tex;
}

// ---------------------------------------------------------------------------
// Factory: FromData
// ---------------------------------------------------------------------------

Texture Texture::FromData(const vk::raii::Device& device,
                          const vk::raii::PhysicalDevice& physicalDevice,
                          SharedQueue& queue,
                          const uint32_t queueFamilyIndex,
                          const uint32_t width,
                          const uint32_t height,
                          const void* pixelData,
                          const vk::Format format,
                          const SamplerConfig& config)
{
	if (!pixelData || width == 0 || height == 0)
	{
		return Texture{}; // invalid
	}

	const vk::DeviceSize dataSize = static_cast<vk::DeviceSize>(width)
	                                * static_cast<vk::DeviceSize>(height)
	                                * pixelByteSize(format);

	return createFromPixelData(device, physicalDevice, queue, queueFamilyIndex,
	                           width, height, pixelData, dataSize, format,
	                           /*generateMipmaps=*/true, config);
}

// ---------------------------------------------------------------------------
// Factory: ForAttachment
// ---------------------------------------------------------------------------

Texture Texture::ForAttachment(const vk::raii::Device& device,
                               const vk::raii::PhysicalDevice& physicalDevice,
                               const vk::Extent2D extent,
                               const vk::Format format,
                               const vk::ImageUsageFlags usage,
                               const SamplerConfig& config)
{
	if (extent.width == 0 || extent.height == 0)
	{
		return Texture{};
	}

	Texture tex;

	try
	{
		tex.tex_image = std::make_unique<Image>(
			device, physicalDevice,
			extent, format, usage,
			/*mipLevels=*/1,
			Image::ImageType::e2D);

		if (usage & vk::ImageUsageFlagBits::eSampled)
		{
			tex.tex_sampler = createSampler(device, config, 1);
		}
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("Texture::ForAttachment failed: " << e.what());
		return Texture{};
	}

	return tex;
}

// ---------------------------------------------------------------------------
// Factory: FromImage
// ---------------------------------------------------------------------------

Texture Texture::FromImage(std::unique_ptr<Image> image, vk::raii::Sampler sampler)
{
	if (!image)
	{
		return Texture{};
	}

	Texture tex;
	tex.tex_image = std::move(image);
	tex.tex_sampler = std::move(sampler);
	return tex;
}

// ---------------------------------------------------------------------------
// Internal: createFromPixelData
// ---------------------------------------------------------------------------

Texture Texture::createFromPixelData(const vk::raii::Device& device,
                                     const vk::raii::PhysicalDevice& physicalDevice,
                                     SharedQueue& queue,
                                     const uint32_t queueFamilyIndex,
                                     const uint32_t width,
                                     const uint32_t height,
                                     const void* pixelData,
                                     const vk::DeviceSize dataSize,
                                     const vk::Format format,
                                     const bool generateMipmaps,
                                     const SamplerConfig& config)
{
	Texture tex;

	const uint32_t mipLevels = generateMipmaps
		? computeMipLevels(width, height)
		: 1u;

	try
	{
		ImageData imageData(pixelData, width, height, Image::FromVkFormat(format));

		// --- 1. Create and upload Image via FromImageData ---
		if (generateMipmaps && mipLevels > 1)
		{
			tex.tex_image = std::make_unique<Image>(Image::FromImageData(
				device, physicalDevice, queue, queueFamilyIndex,
				imageData, "Tex", vk::ImageUsageFlagBits::eTransferSrc, mipLevels));

			// --- 2. Generate mipmaps ---
			StagingBuffer staging(device, physicalDevice, 1, "TexMipStaging");
			auto& cmd = staging.BeginStaging(queueFamilyIndex);
			Barrier::Transition(cmd, *tex.tex_image, ImageState::TransferDst);
			tex.tex_image->GenerateMipmaps(cmd);
			staging.EndStaging(queue);
		}
		else
		{
			tex.tex_image = std::make_unique<Image>(Image::FromImageData(
				device, physicalDevice, queue, queueFamilyIndex,
				imageData, "Tex"));
		}
		tex.tex_sampler = createSampler(device, config, mipLevels);
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("Texture::createFromPixelData failed: " << e.what());
		return Texture{};
	}

	return tex;
}

// ---------------------------------------------------------------------------
// computeMipLevels
// ---------------------------------------------------------------------------

uint32_t Texture::computeMipLevels(const uint32_t width, const uint32_t height)
{
	const uint32_t maxDim = std::max(width, height);
	return static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(maxDim)))) + 1u;
}

// ===========================================================================
// SaveImage (GPU readback → PNG, takes Image)
// ===========================================================================

bool Texture::SaveImage(Image& image,
                         const vk::raii::Device& device,
                         const vk::raii::PhysicalDevice& physicalDevice,
                         SharedQueue& queue,
                         uint32_t queueFamilyIndex,
                         const std::string& path,
                         bool remapSigned)
{
	const ImageState prevState = image.State();

	// --- 1. Transition to TRANSFER_SRC ---
	{
		vk::CommandPoolCreateInfo poolCI(vk::CommandPoolCreateFlagBits::eTransient,
		                                 queueFamilyIndex);
		vk::raii::CommandPool cmdPool(device, poolCI);
		vk::CommandBufferAllocateInfo allocInfo(*cmdPool, vk::CommandBufferLevel::ePrimary, 1);
		vk::raii::CommandBuffers cmdBufs(device, allocInfo);

		cmdBufs[0].begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

		Barrier::Transition(*cmdBufs[0], image, ImageState::TransferSrc);

		cmdBufs[0].end();

		vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmdBufs[0]));
		queue.SubmitAndWait(submitInfo);
	}

	// --- 2. Read back ---
	auto imageData = image.ReadImageData(device, physicalDevice, queue, queueFamilyIndex);

	if (!imageData || !imageData->IsValid()) return false;

	// --- 3. Transition back ---
	if (prevState != ImageState::Undefined)
	{
		vk::CommandPoolCreateInfo poolCI(vk::CommandPoolCreateFlagBits::eTransient,
		                                 queueFamilyIndex);
		vk::raii::CommandPool cmdPool(device, poolCI);
		vk::CommandBufferAllocateInfo allocInfo(*cmdPool, vk::CommandBufferLevel::ePrimary, 1);
		vk::raii::CommandBuffers cmdBufs(device, allocInfo);

		cmdBufs[0].begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

		Barrier::Transition(*cmdBufs[0], image, prevState);

		cmdBufs[0].end();

		vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmdBufs[0]));
		queue.SubmitAndWait(submitInfo);
	}

	// --- 4. Save pixel data ---
	return imageData->SavePNG(path, remapSigned);
}

// ===========================================================================
// SaveTexture (convenience - delegates to SaveImage)
// ===========================================================================

bool Texture::SaveTexture(const vk::raii::Device& device,
                           const vk::raii::PhysicalDevice& physicalDevice,
                           SharedQueue& queue,
                           uint32_t queueFamilyIndex,
                           Texture& texture,
                           const std::string& path,
                           bool remapSigned)
{
	Image* img = texture.GetImage();
	if (!img) return false;
	return SaveImage(*img, device, physicalDevice, queue, queueFamilyIndex, path, remapSigned);
}

// ===========================================================================
// Texture cache
// ===========================================================================

std::unordered_map<std::string, Texture::TextureRes> Texture::s_cache;

Texture::TextureRes Texture::LoadTexture(
	const vk::raii::Device& device,
	const vk::raii::PhysicalDevice& physicalDevice,
	SharedQueue& queue,
	uint32_t queueFamilyIndex,
	const char* path,
	vk::Format format,
	const SamplerConfig& config)
{
	std::string key(path);
	auto it = s_cache.find(key);
	if (it != s_cache.end())
		return it->second;

	auto tex = std::make_shared<Texture>(
		Texture::FromFile(device, physicalDevice, queue, queueFamilyIndex, path, format, config));
	s_cache[key] = tex;
	return tex;
}

void Texture::UnloadTexture(const std::string& path)
{
	s_cache.erase(path);
}

void Texture::ClearCache()
{
	s_cache.clear();
}

size_t Texture::CacheSize()
{
	return s_cache.size();
}

} // namespace neurus
