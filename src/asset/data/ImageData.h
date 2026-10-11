/**
 * @file ImageData.h
 * @brief CPU-side image data and pixel format helpers.
 *
 * Owns raw pixel bytes plus dimensions and format, with helpers for
 * half-float conversion, BGR swizzle and PNG/HDR output. Pure CPU data.
 */
#pragma once

#include "asset/PixelFormat.h"

#include <cereal/types/base_class.hpp>
#include <cereal/types/string.hpp>

#include "core/UID.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace neurus {

/**
 * @brief CPU-side image data (pooled resource).
 *
 * UID identity: serializes its source path and re-loads the content on load.
 * Non-copyable/non-movable.
 */
class ImageData : public UID
{
public:
	/**
	 * @brief Default constructor. Creates an invalid/empty ImageData.
	 */
	ImageData() = default;

	/**
	 * @brief Loads an image from a file path.
	 *
	 * HDR files (.hdr) load as RGBA32F; LDR files (.png, .bmp, .jpg, .tga)
	 * load as RGBA8S.
	 * @param path File path to the image.
	 */
	explicit ImageData(const std::string& path);

	/**
	 * @brief Copies raw pixel data into the owned vector.
	 * @param data        Non-owning pointer to raw pixel bytes.
	 * @param w           Image width in pixels.
	 * @param h           Image height in pixels.
	 * @param fmt         Pixel format.
	 * @param arrayLayers Number of array layers (default 1). Total copy size is
	 *                    w * h * PixelByteSize(fmt) * arrayLayers.
	 */
	ImageData(const void* data, uint32_t w, uint32_t h, PixelFormat fmt,
	          uint32_t arrayLayers = 1);

	// --- Validity ---

	/** @brief True if a valid image is loaded (non-zero dimensions, non-empty pixel data). */
	bool IsValid() const { return im_width > 0 && im_height > 0 && !im_pixelData.empty(); }

	/**
	 * @brief (Re)loads the image content from disk.
	 *
	 * Called from the path constructor and at the end of serialize(load);
	 * no-op when the path is empty.
	 */
	void ReloadContent();

	/**
	 * @brief Cereal serialization - UID + source path, then content reload.
	 * @tparam Archive Cereal archive type (input or output).
	 * @param ar Archive to serialize to/from.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::base_class<UID>(this), CEREAL_NVP(m_path));
		if constexpr (Archive::is_loading::value)
		{
			ReloadContent();
		}
	}

	// --- Getters ---

	const std::vector<uint8_t>& GetPixelData() const { return im_pixelData; }
	uint32_t GetWidth() const { return im_width; }
	uint32_t GetHeight() const { return im_height; }
	PixelFormat GetFormat() const { return im_format; }

	// -------------------------------------------------------------------
	// Format helpers (convenience wrappers over PixelFormat)
	// -------------------------------------------------------------------

	/** @brief Bytes per pixel for the owned format. */
	uint32_t PixelByteSize() const { return neurus::PixelByteSize(im_format); }

	/** @brief Number of colour channels for the owned format. */
	uint32_t ChannelCount() const { return neurus::ChannelCount(im_format); }

	/**
	 * @brief Converts RGBA16F half-float data to RGBA8.
	 * @param remapSigned If true, remap [-1,1] to [0,1] on RGB and keep the
	 *                    background transparent.
	 */
	static std::vector<uint8_t> ConvertHalfToU8(const void* data,
	                                             uint32_t width,
	                                             uint32_t height,
	                                             bool remapSigned = false);

	/**
	 * @brief Converts RGBA32F float data to RGBA8.
	 * @param remapSigned If true, remap [-1,1] to [0,1] on RGB and keep the
	 *                    background transparent.
	 */
	static std::vector<uint8_t> ConvertFloatToU8(const void* data,
	                                              uint32_t width,
	                                              uint32_t height,
	                                              bool remapSigned = false);

	/** @brief Swaps the R and B channels in place for BGRA data. */
	static void SwizzleBGRtoRGB(void* data, uint32_t width,
	                            uint32_t height, uint32_t channels);

	// -------------------------------------------------------------------
	// Save (member functions - use the owned pixel data)
	// -------------------------------------------------------------------

	/**
	 * @brief Writes the owned pixel data to a PNG file (CPU-side only).
	 * @param path        Output file path.
	 * @param remapSigned If true, remap signed values for normal maps.
	 * @return true on success.
	 */
	bool SavePNG(const std::string& path, bool remapSigned = false) const;

	/**
	 * @brief Writes the owned pixel data to a Radiance .hdr file.
	 *
	 * Expects RGBA32F pixel data; output is RGBE-encoded.
	 * @param path Output file path (.hdr extension recommended).
	 * @return true on success.
	 */
	bool SaveHDR(const std::string& path) const;

	/**
	 * @brief Returns the source image path (serialized, relative to asset dir).
	 * @return Const reference to the stored path (empty for procedural data).
	 */
	const std::string& GetPath() const { return m_path; }

private:
	// --- Internal helpers ---

	static float HalfToFloat(uint16_t half);
	static void EnsureDirectory(const std::string& filePath);

	/**
	 * @brief Loads pixel data from file, filling im_pixelData, im_width, im_height, im_format.
	 * @param path File path to the image.
	 */
	void LoadFromPath(const std::string& path);

	// --- Data ---

	std::string m_path;               ///< Source image path (relative to asset dir; serialized)
	std::vector<uint8_t> im_pixelData;  ///< Owning pixel data (raw bytes, format-dependent byte count)
	uint32_t im_width = 0;
	uint32_t im_height = 0;
	PixelFormat im_format{};
};

} // namespace neurus
