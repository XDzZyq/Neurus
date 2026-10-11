#pragma once

#include "asset/data/ImageData.h"
#include "asset/PixelFormat.h"

#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <type_traits>
#include <vector>

namespace neurus {

// Forward declaration for friend access
class Barrier;

/**
 * @brief High-level image state used by Barrier for layout transitions.
 *
 * Maps to the corresponding vk::ImageLayout, vk::PipelineStageFlags2 and
 * vk::AccessFlags2.
 */
enum class ImageState
{
	Undefined,       ///< Initial state, no valid data

	TransferSrc,     ///< Source for transfer (copy/blit) operations
	TransferDst,     ///< Destination for transfer (copy/blit) operations

	ColorAttachment, ///< Color render target
	DepthAttachment, ///< Depth/stencil render target

	ColorShaderRead,  ///< Sampled/input attachment read for color images
	DepthShaderRead,  ///< Sampled/input attachment read for depth images
	ShaderWrite,      ///< Storage image write in compute shaders

	Present,         ///< Ready for presentation

	Invalid,         ///< Image creation failed — no valid GPU resource
};

/**
 * @brief RAII wrapper around vk::raii::Image + vk::raii::DeviceMemory + vk::raii::ImageView.
 *
 * Owns image creation, memory allocation/binding, and view creation.
 * Tracks current logical state via ImageState for use with Barrier::Transition().
 *
 * ImageData is used only during construction — no CPU-side data is retained.
 *
 * Non-copyable, movable.
 */
class Image
{
	friend class Barrier;

public:
	/** @brief Type of image being created (determines view type and create flags). */
	enum class ImageType
	{
		e2D,            ///< Standard 2D image, arrayLayers = 1
		eCube,          ///< Cubemap: arrayLayers = 6, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
		eDepthStencil,  ///< Depth / stencil attachment; aspect determined by format
		eArray          ///< 2D array image; arrayLayers from constructor parameter, view = 2D_ARRAY
	};

	/**
	 * @brief Creates a GPU image from scratch (no pixel data upload).
	 *
	 * The image starts in ImageState::Undefined.  Use Barrier::Transition() to
	 * move it to a usable state, or call the ImageData constructor for automatic
	 * upload.
	 *
	 * @param device         Logical device.
	 * @param physicalDevice Physical device (for memory type queries and format support).
	 * @param extent         Image dimensions.
	 * @param format         Pixel format.
	 * @param usage          Usage flags (e.g. Sampled | TransferDst).
	 * @param mipLevels      Number of mip levels (1 = no mip chain).
	 * @param imageType      Image type (2D, Cube, Depth/Stencil).
	 * @param debugName      Optional debug name for the image.
	 * @param arrayView      If true and imageType is e2D, additionally creates
	 *                       a VK_IMAGE_VIEW_TYPE_2D_ARRAY view (accessible via
	 *                       ImageViewArrayHandle()).  The primary ImageViewHandle()
	 *                       always returns a 2D view.  Use ImageViewArrayHandle()
	 *                       when binding this image to a sampler2DArray shader.
	 * @param arrayLayers    Number of array layers (used when imageType = eArray).
	 */
	Image(const vk::raii::Device& device,
	      const vk::raii::PhysicalDevice& physicalDevice,
	      vk::Extent2D extent,
	      vk::Format format,
	      vk::ImageUsageFlags usage,
	      uint32_t mipLevels = 1,
	      ImageType imageType = ImageType::e2D,
	      const char* debugName = nullptr,
	      bool arrayView = false,
	      uint32_t arrayLayers = 1);

	/**
	 * @brief Creates a GPU image from CPU-side ImageData (loads from file and uploads).
	 *
	 * Uploads the pixel data via a staging buffer and leaves the image in
	 * ImageState::ColorShaderRead.
	 *
	 * @param device          Logical device.
	 * @param physicalDevice  Physical device for memory allocation.
	 * @param queue           Queue for staging upload.
	 * @param queueFamilyIndex Queue family index for transient command pools.
	 * @param imageData       CPU-side image data (must be valid).
	 * @param debugName       Optional debug name for the image.
	 * @param extraUsage      Additional usage flags (OR-ed with Sampled|TransferDst).
	 * @param mipLevels       Number of mipmap levels (default 1 = no mipmap chain).
	 * @return A fully initialised Image, or an Image with ImageState::Invalid on failure.
	 */
	static Image FromImageData(const vk::raii::Device& device,
	                           const vk::raii::PhysicalDevice& physicalDevice,
	                           vk::Queue queue,
	                           uint32_t queueFamilyIndex,
	                           ImageData& imageData,
	                           const char* debugName = "Image",
	                           vk::ImageUsageFlags extraUsage = {},
	                           uint32_t mipLevels = 1);

	/**
	 * @brief Default constructor - creates an empty/invalid Image.
	 *
	 * All GPU handles are null, which facilitates return-by-value from
	 * factory functions on failure paths.
	 */
	Image() = default;

	~Image() = default;

	// --- Non-copyable, movable ---
	Image(const Image&) = delete;
	Image& operator=(const Image&) = delete;
	Image(Image&&) noexcept = default;
	Image& operator=(Image&&) noexcept = default;

	/**
	 * @brief Generates mipmaps via vkCmdBlitImage.
	 *
	 * The image must be in ImageState::TransferDst (all levels). Level 0 is
	 * blitted to level 1, then 1 to 2, etc. All levels end in
	 * ImageState::ColorShaderRead.
	 *
	 * @note The image must have VK_IMAGE_USAGE_TRANSFER_SRC_BIT and
	 *       VK_IMAGE_USAGE_TRANSFER_DST_BIT.
	 *
	 * @param cmdBuf Command buffer to record blit commands into.
	 */
	void GenerateMipmaps(const vk::raii::CommandBuffer& cmdBuf);

	/**
	 * @brief Reads image data from the GPU back into an ImageData.
	 *
	 * Transitions the specified subresource range to ImageState::TransferSrc
	 * and reads the pixel data back through a staging buffer. The image is
	 * left in ImageState::TransferSrc - the caller transitions it back if
	 * needed.
	 *
	 * @param device            Logical device.
	 * @param physicalDevice    Physical device for memory allocation.
	 * @param queue             Queue for staging upload.
	 * @param queueFamilyIndex  Queue family index for transient command pools.
	 * @param subresourceRange  Subresource range to read.  nullptr means
	 *                          mip level 0, layer 0 only (single layer).
	 * @param readExtent        Extent to read (default {0,0} = full image extent).
	 *                          Useful for reading a subregion of a larger image
	 *                          (e.g. 256x256 from a 1024x1024 shadow cubemap).
	 * @return shared_ptr to ImageData with the pixel content, or nullptr on failure.
	 * @note Returns shared_ptr because ImageData is a non-movable UID-derived
	 *       data resource; readbacks are transient (not pooled) heap objects.
	 */
	std::shared_ptr<ImageData> ReadImageData(const vk::raii::Device& device,
	                                         const vk::raii::PhysicalDevice& physicalDevice,
	                                         vk::Queue queue,
	                                         uint32_t queueFamilyIndex,
	                                         const vk::ImageSubresourceRange* subresourceRange = nullptr,
	                                         vk::Extent2D readExtent = {});

	/**
	 * @brief Reads back a single pixel at the specified coordinates from the GPU.
	 *
	 * Reads a 1x1 pixel region at (@p x, @p y) through a minimal staging
	 * buffer, submitting and waiting synchronously. The image must have
	 * VK_IMAGE_USAGE_TRANSFER_SRC_BIT and is restored to its original state
	 * after the readback.
	 *
	 * @param device            Logical device.
	 * @param physicalDevice    Physical device for memory allocation.
	 * @param queue             Queue for staging upload.
	 * @param queueFamilyIndex  Queue family index for transient command pools.
	 * @param x                 Pixel x-coordinate (0 = left edge).
	 * @param y                 Pixel y-coordinate (0 = top edge, Vulkan convention).
	 * @param subresourceRange  Subresource range to read.  nullptr means
	 *                          mip level 0, layer 0 only.
	 * @return Raw pixel bytes (size = PixelByteSize(format)), or empty vector
	 *         on failure (OOB coordinates or unsupported format).
	 */
	std::vector<uint8_t> PickPixel(const vk::raii::Device& device,
	                               const vk::raii::PhysicalDevice& physicalDevice,
	                               vk::Queue queue,
	                               uint32_t queueFamilyIndex,
	                               uint32_t x,
	                               uint32_t y,
	                               const vk::ImageSubresourceRange* subresourceRange = nullptr) const;

	// --- Getters ---

	/** @brief Underlying vk::raii::Image handle. */
	const vk::raii::Image& ImageHandle() const { return im_image; }

	/** @brief Underlying vk::raii::ImageView handle (default view, always 2D for e2D images). */
	const vk::raii::ImageView& ImageViewHandle() const { return im_imageView; }

	/**
	 * @brief Underlying vk::raii::ImageView handle for 2D_ARRAY access.
	 *
	 * Valid only when the image was created with arrayView=true, or for eCube
	 * images (returns the multiview 2D_ARRAY view). For e2D images without
	 * arrayView=true, returns the same as ImageViewHandle().
	 */
	const vk::raii::ImageView& ImageViewArrayHandle() const;

	/**
	 * @brief 2D_ARRAY view covering all 6 layers for multiview rendering.
	 * @note Valid only for ImageType::eCube.
	 */
	const vk::raii::ImageView& ArrayView() const;

	/** @brief Underlying vk::raii::DeviceMemory handle. */
	const vk::raii::DeviceMemory& DeviceMemoryHandle() const { return im_deviceMemory; }

	/** @brief Image extent. */
	vk::Extent2D Extent() const { return im_extent; }

	/** @brief Image format. */
	vk::Format Format() const { return im_format; }

	/** @brief Number of mip levels. */
	uint32_t MipLevels() const { return im_mipLevels; }

	/** @brief Number of array layers. */
	uint32_t ArrayLayers() const { return im_arrayLayers; }

	/** @brief Image type. */
	ImageType Type() const { return im_imageType; }

	/** @brief Current logical image state. */
	ImageState State() const { return im_state; }

	// --- Format conversion helpers ---

	/**
	 * @brief Converts CPU-side PixelFormat to Vulkan vk::Format.
	 * @param fmt Pixel format.
	 * @return Equivalent vk::Format, or eUndefined if no mapping exists.
	 */
	static vk::Format ToVkFormat(PixelFormat fmt);

	/**
	 * @brief Converts Vulkan vk::Format to CPU-side PixelFormat.
	 * @param format Vulkan pixel format.
	 * @return Equivalent PixelFormat, or PixelFormat::Undefined if no mapping exists.
	 */
	static PixelFormat FromVkFormat(vk::Format format);

	/**
	 * @brief Computes bytes per pixel for a Vulkan format (GPU-side helper).
	 * @param format Vulkan pixel format.
	 * @return Byte count, or 0 if unsupported.
	 */
	static uint32_t PixelByteSize(vk::Format format);

	// --- Subresource range helpers ---

	/** @brief Subresource range covering the entire image (all mips, all layers). */
	vk::ImageSubresourceRange AllSubresources() const;

	/** @brief Subresource range for a single mip level (all layers). */
	vk::ImageSubresourceRange Mip(uint32_t level) const;

	/** @brief Subresource range for a contiguous range of mip levels (all layers). */
	vk::ImageSubresourceRange Mips(uint32_t base, uint32_t count) const;

	/** @brief Subresource range for a single array layer (all mips). */
	vk::ImageSubresourceRange Layer(uint32_t layer) const;

	/** @brief Subresource range for a contiguous range of array layers (all mips). */
	vk::ImageSubresourceRange Layers(uint32_t base, uint32_t count) const;

private:
	// --- Construction helpers ---
	void createImage(const vk::raii::Device& device,
	                 const vk::raii::PhysicalDevice& physicalDevice);
	void allocateAndBindMemory(const vk::raii::Device& device,
	                           const vk::raii::PhysicalDevice& physicalDevice);
	void createImageView(const vk::raii::Device& device, const char* debugName);
	void createArrayView(const vk::raii::Device& device);

	/**
	 * @brief Uploads ImageData to this GPU image via staging buffer.
	 *
	 * Transitions: Undefined -> TransferDst -> (copy) -> ColorShaderRead
	 * (mip 0 only). Other mip levels remain in TransferDst, ready for
	 * GenerateMipmaps().
	 */
	void UploadImageData(const vk::raii::Device& device,
	                     const vk::raii::PhysicalDevice& physicalDevice,
	                     vk::Queue queue,
	                     uint32_t queueFamilyIndex,
	                     const ImageData& imageData);

	/**
	 * @brief Returns the Vulkan image aspect flags for a given format.
	 */
	static vk::ImageAspectFlags AspectFromFormat(vk::Format format);

	/** @brief Memory type selection helper. */
	static uint32_t FindMemoryType(const vk::raii::PhysicalDevice& physicalDevice,
	                               uint32_t typeFilter,
	                               vk::MemoryPropertyFlags properties);

	// --- Resources ---
	vk::raii::Image im_image = nullptr;
	vk::raii::DeviceMemory im_deviceMemory = nullptr;
	vk::raii::ImageView im_imageView = nullptr;

	// --- Cube-only views ---
	vk::raii::ImageView im_cubeArrayView = nullptr;

	// --- 2D_ARRAY view (created when arrayView=true on e2D images) ---
	vk::raii::ImageView im_arrayImageView = nullptr;
	bool im_hasArrayView = false;

	// --- Metadata ---
	vk::Extent2D im_extent{};
	vk::Format im_format = vk::Format::eUndefined;
	vk::ImageUsageFlags im_usage;
	uint32_t im_mipLevels = 1;
	uint32_t im_arrayLayers = 1;
	ImageType im_imageType = ImageType::e2D;

	// --- If true, e2D images get a VK_IMAGE_VIEW_TYPE_2D_ARRAY view (for sampler2DArray) ---
	bool im_arrayView = false;

	// --- User-specified array layer count (0/1 = default from ImageType) ---
	uint32_t im_userArrayLayers = 0;

	// --- State tracking ---
	ImageState im_state = ImageState::Undefined;
};

} // namespace neurus
