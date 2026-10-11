#pragma once

#include "Image.h"

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace neurus {

/**
 * @brief Configuration for the VkSampler created alongside a Texture.
 *
 * Defaults to linear filtering, repeat wrap, and mipmapped sampling.
 */
struct SamplerConfig
{
	vk::Filter magFilter = vk::Filter::eLinear;
	vk::Filter minFilter = vk::Filter::eLinear;
	vk::SamplerMipmapMode mipmapMode = vk::SamplerMipmapMode::eLinear;
	vk::SamplerAddressMode addressModeU = vk::SamplerAddressMode::eRepeat;
	vk::SamplerAddressMode addressModeV = vk::SamplerAddressMode::eRepeat;
	vk::SamplerAddressMode addressModeW = vk::SamplerAddressMode::eRepeat;
};

/**
 * @brief RAII texture wrapping an Image and a matching vk::raii::Sampler.
 *
 * Supports loading from file (via STB), from raw pixel data, and creating
 * images suitable for framebuffer attachments. Mipmaps are generated
 * automatically for sampled textures.
 *
 * Non-copyable, movable.
 */
class Texture
{
public:
	/** @brief Creates an empty, invalid Texture. Use factory methods to populate. */
	Texture() = default;

	~Texture() = default;

	// --- Non-copyable, movable ---
	Texture(const Texture&) = delete;
	Texture& operator=(const Texture&) = delete;
	Texture(Texture&&) noexcept = default;
	Texture& operator=(Texture&&) noexcept = default;

	// -------------------------------------------------------------------
	// Type alias
	// -------------------------------------------------------------------

	/** @brief Shared pointer to a Texture. */
	using TextureRes = std::shared_ptr<Texture>;

	// --- Factory methods ---

	/**
	 * @brief Creates a Texture by loading an image file via stb_image.
	 *
	 * @param device           Logical device.
	 * @param physicalDevice   Physical device for memory queries.
	 * @param queue            Graphics queue for staging upload submits.
	 * @param queueFamilyIndex Queue family index for transient command pools.
	 * @param path             File path to the image (PNG, JPG, HDR, etc.).
	 * @param format           Desired Vulkan format (e.g., eR8G8B8A8Srgb for LDR,
	 *                         eR32G32B32A32Sfloat for HDR).
	 * @param config           Optional sampler configuration.
	 * @return A fully uploaded, mipmapped Texture, or an invalid Texture on failure.
	 */
	static Texture FromFile(const vk::raii::Device& device,
	                        const vk::raii::PhysicalDevice& physicalDevice,
	                        vk::Queue queue,
	                        uint32_t queueFamilyIndex,
	                        const char* path,
	                        vk::Format format,
	                        const SamplerConfig& config = {});

	/**
	 * @brief Creates a Texture from raw in-memory pixel data.
	 *
	 * Uploads through a staging buffer and generates mipmaps.
	 *
	 * @param device           Logical device.
	 * @param physicalDevice   Physical device for memory queries.
	 * @param queue            Graphics queue for staging upload submits.
	 * @param queueFamilyIndex Queue family index for transient command pools.
	 * @param width            Image width in pixels.
	 * @param height           Image height in pixels.
	 * @param pixelData        Pointer to raw pixel data. Must match the format's
	 *                         expected byte layout (e.g., 4 bytes/pixel for RGBA8).
	 * @param format           Vulkan format of the pixel data.
	 * @param config           Optional sampler configuration.
	 * @return A fully uploaded, mipmapped Texture, or an invalid Texture on failure.
	 */
	static Texture FromData(const vk::raii::Device& device,
	                        const vk::raii::PhysicalDevice& physicalDevice,
	                        vk::Queue queue,
	                        uint32_t queueFamilyIndex,
	                        uint32_t width,
	                        uint32_t height,
	                        const void* pixelData,
	                        vk::Format format,
	const SamplerConfig& config = {});

	/**
	 * @brief Creates a Texture suitable for framebuffer attachment usage.
	 *
	 * The image is created with the specified usage flags. No pixel data is
	 * uploaded and no mipmaps are generated (1 mip level). A sampler is
	 * created only if the usage includes VK_IMAGE_USAGE_SAMPLED_BIT.
	 *
	 * @param device         Logical device.
	 * @param physicalDevice Physical device for memory queries.
	 * @param extent         Attachment dimensions.
	 * @param format         Attachment format.
	 * @param usage          Image usage flags (e.g., eColorAttachment | eSampled).
	 * @param config         Optional sampler configuration.
	 * @return A valid Texture or an invalid Texture on failure.
	 */
	static Texture ForAttachment(const vk::raii::Device& device,
	                             const vk::raii::PhysicalDevice& physicalDevice,
	                             vk::Extent2D extent,
	                             vk::Format format,
	                             vk::ImageUsageFlags usage,
	                             const SamplerConfig& config = {});

	/**
	 * @brief Wraps an existing Image and Sampler into a Texture.
	 *
	 * Ownership is transferred into the returned Texture. If image is nullptr
	 * an invalid Texture{} is returned.
	 *
	 * @param image   Unique pointer to an already-created Image (moved in).
	 * @param sampler A vk::raii::Sampler (moved in).
	 * @return A Texture owning the Image and Sampler, or an invalid Texture.
	 */
	static Texture FromImage(std::unique_ptr<Image> image, vk::raii::Sampler sampler);

	// --- Queries ---

	/** @brief True if the Texture contains a valid Image. */
	bool IsValid() const { return tex_image != nullptr; }

	/** @brief Underlying Image (nullptr if invalid). */
	Image* GetImage() const { return tex_image.get(); }

	/** @brief Underlying VkSampler handle. Only valid if HasSampler() is true. */
	const vk::raii::Sampler& GetSampler() const { return tex_sampler; }

	/** @brief True if a sampler was created for this Texture. */
	bool HasSampler() const { return *tex_sampler != vk::Sampler{}; }

	/** @brief Image extent. Returns {0,0} if invalid. */
	vk::Extent2D Extent() const { return tex_image ? tex_image->Extent() : vk::Extent2D{}; }

	/** @brief Image format. Returns eUndefined if invalid. */
	vk::Format Format() const { return tex_image ? tex_image->Format() : vk::Format::eUndefined; }

	/** @brief Number of mip levels. Returns 0 if invalid. */
	uint32_t MipLevels() const { return tex_image ? tex_image->MipLevels() : 0; }

	// -------------------------------------------------------------------
	// Texture loading (cached)
	// -------------------------------------------------------------------

	/**
	 * @brief Loads (or retrieves from cache) a texture from a file path.
	 * @return Shared pointer; may be invalid if loading fails.
	 */
	static TextureRes LoadTexture(const vk::raii::Device& device,
	                              const vk::raii::PhysicalDevice& physicalDevice,
	                              vk::Queue queue,
	                              uint32_t queueFamilyIndex,
	                              const char* path,
	                              vk::Format format,
	                              const SamplerConfig& config = {});

	/** @brief Removes a texture from the cache by path. */
	static void UnloadTexture(const std::string& path);

	/** @brief Clears all cached textures. */
	static void ClearCache();

	/** @brief Returns the number of cached textures. */
	static size_t CacheSize();

	// -------------------------------------------------------------------
	// Texture saving (GPU readback → PNG)
	// -------------------------------------------------------------------

	/**
	 * @brief Reads back a Texture from the GPU and writes it as PNG.
	 *
	 * @return true on success.
	 */
	static bool SaveTexture(const vk::raii::Device& device,
	                        const vk::raii::PhysicalDevice& physicalDevice,
	                        vk::Queue queue,
	                        uint32_t queueFamilyIndex,
	                        Texture& texture,
	                        const std::string& path,
	                        bool remapSigned = false);

	// -------------------------------------------------------------------
	// Image saving (GPU readback → PNG)
	// -------------------------------------------------------------------

	/**
	 * @brief Reads back an Image from the GPU and writes it as PNG.
	 *
	 * Handles layout transitions, GPU readback, format conversion,
	 * and PNG output.  The image is left in the layout it was found in.
	 *
	 * @param image       Image to capture (layout is temporarily transitioned).
	 * @param remapSigned Remap signed values for normal-map data.
	 * @return true on success.
	 */
	static bool SaveImage(Image& image,
	                      const vk::raii::Device& device,
	                      const vk::raii::PhysicalDevice& physicalDevice,
	                      vk::Queue queue,
	                      uint32_t queueFamilyIndex,
	                      const std::string& path,
	                      bool remapSigned = false);

private:
	/**
	 * @brief Internal helper: creates the Image, uploads data, generates
	 *        mipmaps, creates the sampler, and transitions to SHADER_READ_ONLY.
	 */
	static Texture createFromPixelData(const vk::raii::Device& device,
	                                   const vk::raii::PhysicalDevice& physicalDevice,
	                                   vk::Queue queue,
	                                   uint32_t queueFamilyIndex,
	                                   uint32_t width,
	                                   uint32_t height,
	                                   const void* pixelData,
	                                   vk::DeviceSize dataSize,
	                                   vk::Format format,
	                                   bool generateMipmaps,
	                                   const SamplerConfig& config);

	/**
	 * @brief Computes the maximum number of mip levels for the given dimensions.
	 */
	static uint32_t computeMipLevels(uint32_t width, uint32_t height);

	std::unique_ptr<Image> tex_image;
	vk::raii::Sampler tex_sampler = nullptr;

	// --- Texture cache ---
	static std::unordered_map<std::string, TextureRes> s_cache;
};

} // namespace neurus
