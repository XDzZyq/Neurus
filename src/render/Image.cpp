#include "Image.h"
#include "Barrier.h"
#include "buffers/StagingBuffer.h"
#include "asset/PixelFormat.h"

#include "core/Log.h"

#include <stdexcept>
#include <algorithm>
#include <string>

namespace neurus {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Image::Image(const vk::raii::Device& device,
             const vk::raii::PhysicalDevice& physicalDevice,
             const vk::Extent2D extent,
             const vk::Format format,
             const vk::ImageUsageFlags usage,
             const uint32_t mipLevels,
             const ImageType imageType,
             const char* debugName,
             const bool arrayView,
             const uint32_t arrayLayers)
	: im_extent(extent)
	, im_format(format)
	, im_usage(usage)
	, im_mipLevels(mipLevels)
	, im_imageType(imageType)
	, im_arrayView(arrayView)
	, im_userArrayLayers(arrayLayers)
{
	createImage(device, physicalDevice);
	allocateAndBindMemory(device, physicalDevice);
	createImageView(device, debugName);

	if (im_imageType == ImageType::eCube)
	{
		createArrayView(device);
	}

	// --- Set debug name in Debug builds ---
#ifdef _DEBUG
	if (debugName && *debugName)
	{
		const vk::DebugUtilsObjectNameInfoEXT nameInfo(
			vk::ObjectType::eImage,
			reinterpret_cast<uint64_t>(static_cast<VkImage>(*im_image)),
			debugName);
		device.setDebugUtilsObjectNameEXT(nameInfo);

		{
			std::string memName(debugName);
			memName += "_Mem";
			vk::DebugUtilsObjectNameInfoEXT memNameInfo(
				vk::ObjectType::eDeviceMemory,
				reinterpret_cast<uint64_t>(static_cast<VkDeviceMemory>(*im_deviceMemory)),
				memName.c_str());
			device.setDebugUtilsObjectNameEXT(memNameInfo);
		}
	}
#endif

	{
		const char* typeStr = (im_imageType == ImageType::e2D) ? "2D" :
		                      (im_imageType == ImageType::eCube) ? "Cube" :
		                      (im_imageType == ImageType::eDepthStencil) ? "DepthStencil" : "Unknown";
		NEURUS_LOG("[Image] " << im_extent.width << "x" << im_extent.height
		          << " mips=" << im_mipLevels
		          << " type=" << typeStr
		          << " format=" << vk::to_string(im_format)
		          << " usage=" << vk::to_string(im_usage)
		          << " handle=0x" << std::hex << reinterpret_cast<uint64_t>(static_cast<VkImage>(*im_image)) << std::dec
		          << (debugName ? " name='" : "")
		          << (debugName ? debugName : "")
		          << (debugName ? "'" : ""));
	}
}

// ---------------------------------------------------------------------------
// Static factory: FromImageData
// ---------------------------------------------------------------------------

Image Image::FromImageData(const vk::raii::Device& device,
                          const vk::raii::PhysicalDevice& physicalDevice,
                          vk::Queue queue,
                          uint32_t queueFamilyIndex,
                          ImageData& imageData,
                          const char* debugName,
                          vk::ImageUsageFlags extraUsage,
                          uint32_t mipLevels)
{
	if (!imageData.IsValid())
	{
		NEURUS_ERR("[Image] FromImageData: invalid ImageData provided");
		Image bad;
		bad.im_state = ImageState::Invalid;
		return bad;
	}

	const vk::Format vkFmt = ToVkFormat(imageData.GetFormat());
	if (vkFmt == vk::Format::eUndefined)
	{
		NEURUS_ERR("[Image] FromImageData: unsupported PixelFormat");
		Image bad;
		bad.im_state = ImageState::Invalid;
		return bad;
	}

	Image image(
		device, physicalDevice,
		vk::Extent2D{imageData.GetWidth(), imageData.GetHeight()},
		vkFmt,
		vk::ImageUsageFlagBits::eSampled |
		    vk::ImageUsageFlagBits::eTransferDst |
		    extraUsage,
		mipLevels,
		ImageType::e2D,
		debugName);

	image.UploadImageData(device, physicalDevice, queue, queueFamilyIndex, imageData);

	return image;
}

// ---------------------------------------------------------------------------
// Image creation
// ---------------------------------------------------------------------------

void Image::createImage(const vk::raii::Device& device,
                              const vk::raii::PhysicalDevice& /*physicalDevice*/)
{
	vk::ImageCreateFlags createFlags;

	switch (im_imageType)
	{
	case ImageType::eCube:
		im_arrayLayers = 6;
		createFlags = vk::ImageCreateFlagBits::eCubeCompatible;
		break;
	case ImageType::eDepthStencil:
		im_arrayLayers = 1;
		createFlags = {};
		break;
	case ImageType::eArray:
		im_arrayLayers = im_userArrayLayers;
		createFlags = {};
		break;
	case ImageType::e2D:
	default:
		im_arrayLayers = 1;
		createFlags = {};
		break;
	}

	const vk::Extent3D extent3D(im_extent.width, im_extent.height, 1);
	const vk::ImageCreateInfo imageCI(
		createFlags,
		vk::ImageType::e2D,
		im_format,
		extent3D,
		im_mipLevels,
		im_arrayLayers,
		vk::SampleCountFlagBits::e1,
		vk::ImageTiling::eOptimal,
		im_usage,
		vk::SharingMode::eExclusive,
		{},
		vk::ImageLayout::eUndefined);

	im_image = vk::raii::Image(device, imageCI);
}

// ---------------------------------------------------------------------------
// Memory allocation & binding
// ---------------------------------------------------------------------------

void Image::allocateAndBindMemory(const vk::raii::Device& device,
                                        const vk::raii::PhysicalDevice& physicalDevice)
{
	const auto memReqs = im_image.getMemoryRequirements();
	const auto typeIndex = FindMemoryType(physicalDevice,
	                                      memReqs.memoryTypeBits,
	                                      vk::MemoryPropertyFlagBits::eDeviceLocal);

	const vk::MemoryAllocateInfo allocInfo(memReqs.size, typeIndex);
	im_deviceMemory = vk::raii::DeviceMemory(device, allocInfo);

	im_image.bindMemory(*im_deviceMemory, 0);
}

// ---------------------------------------------------------------------------
// Image view creation
// ---------------------------------------------------------------------------

void Image::createImageView(const vk::raii::Device& device, const char* debugName)
{
	vk::ImageViewType viewType;
	vk::ImageAspectFlags aspect;

	switch (im_imageType)
	{
	case ImageType::eCube:
		viewType = vk::ImageViewType::eCube;
		aspect = AspectFromFormat(im_format);
		break;
	case ImageType::eDepthStencil:
		viewType = vk::ImageViewType::e2D;
		aspect = AspectFromFormat(im_format);
		break;
	case ImageType::eArray:
		viewType = vk::ImageViewType::e2DArray;
		aspect = vk::ImageAspectFlagBits::eColor;
		break;
	case ImageType::e2D:
	default:
		// Default view is always 2D; when arrayView=true an additional 2D_ARRAY
		// view is created below. Aspect is derived from the format so depth-only
		// formats (D32) get the correct eDepth aspect, while color formats get
		// eColor.
		viewType = vk::ImageViewType::e2D;
		aspect = AspectFromFormat(im_format);
		break;
	}

	const vk::ImageSubresourceRange subresourceRange(
		aspect,
		0,               // baseMipLevel
		im_mipLevels,     // levelCount
		0,               // baseArrayLayer
		im_arrayLayers    // layerCount
	);

	const vk::ComponentMapping components; // identity mapping

	const vk::ImageViewCreateInfo viewCI(
		{},
		*im_image,
		viewType,
		im_format,
		components,
		subresourceRange);

	im_imageView = vk::raii::ImageView(device, viewCI);

	// --- If arrayView requested, create an additional 2D_ARRAY view ---
	if (im_imageType == ImageType::e2D && im_arrayView)
	{
		const vk::ImageViewCreateInfo arrayViewCI(
			{},
			*im_image,
			vk::ImageViewType::e2DArray,
			im_format,
			components,
			subresourceRange);
		im_arrayImageView = vk::raii::ImageView(device, arrayViewCI);
		im_hasArrayView = true;

#ifdef _DEBUG
		if (debugName && *debugName)
		{
			std::string arrayViewName(debugName);
			arrayViewName += "_ArrayView";
			vk::DebugUtilsObjectNameInfoEXT nameInfo(
				vk::ObjectType::eImageView,
				reinterpret_cast<uint64_t>(static_cast<VkImageView>(*im_arrayImageView)),
				arrayViewName.c_str());
			device.setDebugUtilsObjectNameEXT(nameInfo);
		}
#endif
	}

	// --- Set debug name on primary image view in Debug builds ---
#ifdef _DEBUG
	if (debugName && *debugName)
	{
		std::string viewName(debugName);
		viewName += "_View";
		vk::DebugUtilsObjectNameInfoEXT nameInfo(
			vk::ObjectType::eImageView,
			reinterpret_cast<uint64_t>(static_cast<VkImageView>(*im_imageView)),
			viewName.c_str());
		device.setDebugUtilsObjectNameEXT(nameInfo);
	}
#endif
}

// ---------------------------------------------------------------------------
// Mipmap generation via vkCmdBlitImage
// ---------------------------------------------------------------------------

void Image::GenerateMipmaps(const vk::raii::CommandBuffer& cmdBuf)
{
	if (im_mipLevels <= 1)
	{
		return;
	}

	const auto aspect = AspectFromFormat(im_format);

	int32_t mipWidth  = static_cast<int32_t>(im_extent.width);
	int32_t mipHeight = static_cast<int32_t>(im_extent.height);

	for (uint32_t i = 1; i < im_mipLevels; ++i)
	{
		// At this point, level (i-1) is guaranteed to be in TransferDst:
		//   - Iteration 1: set by the initial full-image barrier before GenerateMipmaps
		//   - Iteration N: level (i-1) was the blit destination in iteration (N-1)
		// We reset im_state so Barrier::Transition reads the correct "old" layout.

		// --- Transition level (i-1) TransferDst → TransferSrc ---
		im_state = ImageState::TransferDst;
		{
			vk::ImageSubresourceRange range(aspect, i - 1, 1, 0, im_arrayLayers);
			Barrier::Transition(*cmdBuf, *this, ImageState::TransferSrc, range);
		}

		const int32_t srcWidth  = mipWidth;
		const int32_t srcHeight = mipHeight;
		const int32_t dstWidth  = std::max(1, mipWidth / 2);
		const int32_t dstHeight = std::max(1, mipHeight / 2);

		vk::ImageBlit blit;
		blit.srcSubresource.aspectMask     = aspect;
		blit.srcSubresource.mipLevel       = i - 1;
		blit.srcSubresource.baseArrayLayer = 0;
		blit.srcSubresource.layerCount     = im_arrayLayers;
		blit.srcOffsets[0] = vk::Offset3D(0, 0, 0);
		blit.srcOffsets[1] = vk::Offset3D(srcWidth, srcHeight, 1);

		blit.dstSubresource.aspectMask     = aspect;
		blit.dstSubresource.mipLevel       = i;
		blit.dstSubresource.baseArrayLayer = 0;
		blit.dstSubresource.layerCount     = im_arrayLayers;
		blit.dstOffsets[0] = vk::Offset3D(0, 0, 0);
		blit.dstOffsets[1] = vk::Offset3D(dstWidth, dstHeight, 1);

		auto vulkanSrcState = Barrier::ToVulkanImageState(ImageState::TransferSrc);
		auto vulkanDstState = Barrier::ToVulkanImageState(ImageState::TransferDst);

		cmdBuf.blitImage(
			*im_image, vulkanSrcState.layout,
			*im_image, vulkanDstState.layout,
			{ blit },
			vk::Filter::eLinear);

		// --- Release level (i-1): transfer writes available to graphics usage
		//     (transfer-valid dstStage; see VUID-vkCmdPipelineBarrier2-dstStageMask-09676) ---
		im_state = ImageState::TransferSrc;
		{
			vk::ImageSubresourceRange range(aspect, i - 1, 1, 0, im_arrayLayers);
			Barrier::ReleaseToRead(*cmdBuf, *im_image, ImageState::TransferSrc, range);
		}

		mipWidth  = dstWidth;
		mipHeight = dstHeight;
	}

	// --- Release last mip level: transfer writes available to graphics usage ---
	im_state = ImageState::TransferDst;
	{
		vk::ImageSubresourceRange lastRange(aspect, im_mipLevels - 1, 1, 0, im_arrayLayers);
		Barrier::ReleaseToRead(*cmdBuf, *im_image, ImageState::TransferDst, lastRange);
	}

	// Update CPU-side state tracking to reflect the final layout
	im_state = ImageState::ColorShaderRead;
}

// ---------------------------------------------------------------------------
// CPU → GPU upload
// ---------------------------------------------------------------------------

void Image::UploadImageData(const vk::raii::Device& device,
                            const vk::raii::PhysicalDevice& physicalDevice,
                            vk::Queue queue,
                            uint32_t queueFamilyIndex,
                            const ImageData& imageData)
{
	const auto& pixelData = imageData.GetPixelData();
	const vk::DeviceSize bufferSize = static_cast<vk::DeviceSize>(pixelData.size());

	// --- Create staging buffer and upload pixel data ---
	StagingBuffer staging(device, physicalDevice,
	                      bufferSize, "ImageUploadStaging");
	staging.Upload(pixelData.data(), bufferSize);

	auto& cmd = staging.BeginStaging(queueFamilyIndex);

	// --- Transition image Undefined → TransferDst ---
	Barrier::Transition(cmd, *this, ImageState::TransferDst);

	// --- Copy buffer → image ---
	{
		vk::BufferImageCopy copyRegion;
		copyRegion.bufferOffset      = 0;
		copyRegion.bufferRowLength   = 0;
		copyRegion.bufferImageHeight = 0;
		copyRegion.imageSubresource  = vk::ImageSubresourceLayers(
			AspectFromFormat(im_format), 0, 0, im_arrayLayers);
		copyRegion.imageOffset = vk::Offset3D(0, 0, 0);
		copyRegion.imageExtent = vk::Extent3D(im_extent.width, im_extent.height, 1);

		cmd.copyBufferToImage(staging.buffer(), *im_image,
		                      vk::ImageLayout::eTransferDstOptimal,
		                      { copyRegion });
	}

	// --- Release mip level 0: make transfer writes available to graphics usage
	//     without a graphics-stage dstStage (invalid on transfer-only command
	//     pools per VUID-vkCmdPipelineBarrier2-dstStageMask-09676). The layout
	//     becomes ShaderReadOnlyOptimal; consuming passes re-transition on use. ---
	{
		vk::ImageSubresourceRange mip0Range(AspectFromFormat(im_format), 0, 1, 0, im_arrayLayers);
		Barrier::ReleaseToRead(cmd, *im_image, ImageState::TransferDst, mip0Range);
	}

	// Update CPU-side state tracking (other mips stay in TransferDst for mipmap generation)
	im_state = ImageState::ColorShaderRead;

	staging.EndStaging(queue);
}

// ---------------------------------------------------------------------------
// GPU readback
// ---------------------------------------------------------------------------

std::shared_ptr<ImageData> Image::ReadImageData(const vk::raii::Device& device,
                                                const vk::raii::PhysicalDevice& physicalDevice,
                                                vk::Queue queue,
                                                uint32_t queueFamilyIndex,
                                                const vk::ImageSubresourceRange* subresourceRange,
                                                vk::Extent2D readExtent)
{
	const uint32_t bytesPerPixel = PixelByteSize(im_format);

	if (bytesPerPixel == 0)
	{
		NEURUS_ERR("[Image] ReadImageData: unsupported format " << vk::to_string(im_format));
		return nullptr;
	}

	// Extent to read: explicit override or full image
	const vk::Extent2D copyExtent = (readExtent.width == 0 && readExtent.height == 0)
		? im_extent
		: readExtent;

	const auto range = subresourceRange
		? *subresourceRange
		: vk::ImageSubresourceRange(AspectFromFormat(im_format), 0, 1, 0, 1);

	const uint32_t layerCount = range.layerCount;
	const uint32_t baseLayer  = range.baseArrayLayer;

	const vk::DeviceSize imageSize = static_cast<vk::DeviceSize>(copyExtent.width) *
	                                 copyExtent.height * bytesPerPixel * layerCount;

	// --- Staging buffer (host-visible, transfer-dst for GPU→CPU) ---
	StagingBuffer staging(device, physicalDevice,
	                      imageSize, "ReadImageDataStaging",
	                      vk::BufferUsageFlagBits::eTransferDst);

	auto& cmd = staging.BeginStaging(queueFamilyIndex);

	// --- Transition the requested subresource range to TransferSrc ---
	Barrier::Transition(cmd, *this, ImageState::TransferSrc, range);

	// --- Copy image to buffer ---
	vk::BufferImageCopy copyRegion;
	copyRegion.bufferOffset      = 0;
	copyRegion.bufferRowLength   = 0;
	copyRegion.bufferImageHeight = 0;
	copyRegion.imageSubresource  = vk::ImageSubresourceLayers(
		AspectFromFormat(im_format), range.baseMipLevel, baseLayer, layerCount);
	copyRegion.imageOffset = vk::Offset3D(0, 0, 0);
	copyRegion.imageExtent = vk::Extent3D(copyExtent.width, copyExtent.height, 1);

	cmd.copyImageToBuffer(*im_image, vk::ImageLayout::eTransferSrcOptimal,
	                      staging.buffer(), { copyRegion });

	// --- Buffer barrier: transfer write → host read ---
	Barrier::Transition(cmd, staging, BufferState::HostRead);

	staging.EndStaging(queue);

	void* mapped = staging.Map();
	const PixelFormat pf = FromVkFormat(im_format);
	auto result = std::make_shared<ImageData>(mapped, copyExtent.width, copyExtent.height, pf, layerCount);
	staging.Unmap();

	return result;
}

// ---------------------------------------------------------------------------
// Single-pixel readback
// ---------------------------------------------------------------------------

std::vector<uint8_t> Image::PickPixel(const vk::raii::Device& device,
                                       const vk::raii::PhysicalDevice& physicalDevice,
                                       vk::Queue queue,
                                       uint32_t queueFamilyIndex,
                                       uint32_t x,
                                       uint32_t y,
                                       const vk::ImageSubresourceRange* subresourceRange) const
{
	const uint32_t bytesPerPixel = PixelByteSize(im_format);

	if (bytesPerPixel == 0)
	{
		NEURUS_ERR("[Image] PickPixel: unsupported format " << vk::to_string(im_format));
		return {};
	}

	// --- Bounds checking ---
	if (x >= im_extent.width || y >= im_extent.height)
	{
		NEURUS_ERR("[Image] PickPixel: pixel (" << x << ", " << y
		           << ") out of bounds [" << im_extent.width << "x" << im_extent.height << "]");
		return {};
	}

	const auto range = subresourceRange
		? *subresourceRange
		: vk::ImageSubresourceRange(AspectFromFormat(im_format), 0, 1, 0, 1);

	const vk::DeviceSize bufferSize = bytesPerPixel;

	// --- Save original state for restoration after readback ---
	const ImageState originalState = im_state;

	// --- Staging buffer (host-visible, transfer-dst) ---
	StagingBuffer staging(device, physicalDevice,
	                      bufferSize, "PickPixelStaging",
	                      vk::BufferUsageFlagBits::eTransferDst);

	auto& cmd = staging.BeginStaging(queueFamilyIndex);

	// --- Transition the subresource to TransferSrc ---
	// Use the raw-image overload (before/after) to avoid mutating im_state,
	// keeping PickPixel truly const.
	Barrier::Transition(cmd, *im_image, im_state, ImageState::TransferSrc, range);

	// --- Copy single pixel (1×1 region at (x,y)) to staging buffer ---
	vk::BufferImageCopy copyRegion;
	copyRegion.bufferOffset      = 0;
	copyRegion.bufferRowLength   = 0;
	copyRegion.bufferImageHeight = 0;
	copyRegion.imageSubresource  = vk::ImageSubresourceLayers(
		AspectFromFormat(im_format), range.baseMipLevel, range.baseArrayLayer, range.layerCount);
	copyRegion.imageOffset = vk::Offset3D(static_cast<int32_t>(x), static_cast<int32_t>(y), 0);
	copyRegion.imageExtent = vk::Extent3D(1, 1, 1);

	cmd.copyImageToBuffer(*im_image, vk::ImageLayout::eTransferSrcOptimal,
	                      staging.buffer(), { copyRegion });

	// --- Buffer barrier: transfer write → host read ---
	Barrier::Transition(cmd, staging, BufferState::HostRead);

	// --- Restore the image subresource back to its original state ---
	// Done before EndStaging so it's part of the same submission (one submit + one waitIdle).
	Barrier::Transition(cmd, *im_image, ImageState::TransferSrc, originalState, range);

	staging.EndStaging(queue);

	// --- Read back the single pixel ---
	std::vector<uint8_t> result(bytesPerPixel);
	void* mapped = staging.Map();
	std::memcpy(result.data(), mapped, bytesPerPixel);
	staging.Unmap();

	return result;
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

vk::ImageAspectFlags Image::AspectFromFormat(const vk::Format format)
{
	switch (format)
	{
	// Depth + Stencil formats
	case vk::Format::eD16UnormS8Uint:
	case vk::Format::eD24UnormS8Uint:
	case vk::Format::eD32SfloatS8Uint:
		return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;

	// Depth-only formats
	case vk::Format::eD16Unorm:
	case vk::Format::eD32Sfloat:
	case vk::Format::eX8D24UnormPack32:
		return vk::ImageAspectFlagBits::eDepth;

	// Stencil-only
	case vk::Format::eS8Uint:
		return vk::ImageAspectFlagBits::eStencil;

	// Default: color
	default:
		return vk::ImageAspectFlagBits::eColor;
	}
}

uint32_t Image::FindMemoryType(const vk::raii::PhysicalDevice& physicalDevice,
                                     const uint32_t typeFilter,
                                     const vk::MemoryPropertyFlags properties)
{
	const auto memProps = physicalDevice.getMemoryProperties();
	for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
	{
		if ((typeFilter & (1u << i)) &&
		    (memProps.memoryTypes[i].propertyFlags & properties) == properties)
		{
			return i;
		}
	}

	throw std::runtime_error("Image: failed to find suitable memory type.");
}

// ---------------------------------------------------------------------------
// ImageViewArrayHandle
// ---------------------------------------------------------------------------

const vk::raii::ImageView& Image::ImageViewArrayHandle() const
{
	if (im_hasArrayView)
	{
		return im_arrayImageView;
	}
	return im_imageView;
}

// ---------------------------------------------------------------------------
// Cube-only views
// ---------------------------------------------------------------------------

const vk::raii::ImageView& Image::ArrayView() const
{
	return im_cubeArrayView;
}

// ---------------------------------------------------------------------------
// Format conversion helpers
// ---------------------------------------------------------------------------

vk::Format Image::ToVkFormat(const PixelFormat fmt)
{
	switch (fmt)
	{
	case PixelFormat::Undefined:    return vk::Format::eUndefined;
	case PixelFormat::RGBA8U:       return vk::Format::eR8G8B8A8Unorm;
	case PixelFormat::RGBA8S:       return vk::Format::eR8G8B8A8Srgb;
	case PixelFormat::RGBA32F:      return vk::Format::eR32G32B32A32Sfloat;
	case PixelFormat::RGBA16F:      return vk::Format::eR16G16B16A16Sfloat;
	case PixelFormat::R8U:          return vk::Format::eR8Unorm;
	case PixelFormat::D32F:         return vk::Format::eD32Sfloat;
	case PixelFormat::BGRA8U:       return vk::Format::eB8G8R8A8Unorm;
	case PixelFormat::BGRA8S:       return vk::Format::eB8G8R8A8Srgb;
	case PixelFormat::RGBA16U:      return vk::Format::eR16G16B16A16Unorm;
	case PixelFormat::RGBA16SN:     return vk::Format::eR16G16B16A16Snorm;
	case PixelFormat::R8S:          return vk::Format::eR8Srgb;
	}
	return vk::Format::eUndefined;
}

PixelFormat Image::FromVkFormat(const vk::Format format)
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

uint32_t Image::PixelByteSize(const vk::Format format)
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

void Image::createArrayView(const vk::raii::Device& device)
{
	const auto aspect = AspectFromFormat(im_format);
	vk::ImageViewCreateInfo ci({}, *im_image,
		vk::ImageViewType::e2DArray, im_format,
		vk::ComponentMapping(),
		vk::ImageSubresourceRange(aspect, 0, 1, 0, 6));
	im_cubeArrayView = vk::raii::ImageView(device, ci);
}

// ---------------------------------------------------------------------------
// Subresource range helpers
// ---------------------------------------------------------------------------

vk::ImageSubresourceRange Image::AllSubresources() const
{
	return { AspectFromFormat(im_format), 0, im_mipLevels, 0, im_arrayLayers };
}

vk::ImageSubresourceRange Image::Mip(uint32_t level) const
{
	return { AspectFromFormat(im_format), level, 1, 0, im_arrayLayers };
}

vk::ImageSubresourceRange Image::Mips(uint32_t base, uint32_t count) const
{
	return { AspectFromFormat(im_format), base, count, 0, im_arrayLayers };
}

vk::ImageSubresourceRange Image::Layer(uint32_t layer) const
{
	return { AspectFromFormat(im_format), 0, im_mipLevels, layer, 1 };
}

vk::ImageSubresourceRange Image::Layers(uint32_t base, uint32_t count) const
{
	return { AspectFromFormat(im_format), 0, im_mipLevels, base, count };
}

} // namespace neurus
