#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "asset/data/ImageData.hpp"
#include "asset/data/AssetPath.hpp"
#include "core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace neurus {

// ===========================================================================
// Internal helpers
// ===========================================================================

float ImageData::HalfToFloat(uint16_t half)
{
	const uint32_t sign     = (half & 0x8000u) >> 15;
	const uint32_t exponent = (half & 0x7C00u) >> 10;
	const uint32_t mantissa =  half & 0x03FFu;

	uint32_t f32;
	if (exponent == 0)
	{
		if (mantissa == 0) { f32 = sign << 31; }
		else
		{
			uint32_t m = mantissa;
			int e = -14;
			while ((m & 0x0400u) == 0) { m <<= 1; --e; }
			m &= 0x03FFu;
			f32 = (sign << 31) | ((uint32_t)(e + 127) << 23) | (m << 13);
		}
	}
	else if (exponent == 0x1F)
	{
		f32 = (sign << 31) | 0x7F800000u | (mantissa << 13);
	}
	else
	{
		f32 = (sign << 31) | ((uint32_t)(exponent - 15 + 127) << 23) | (mantissa << 13);
	}

	float result;
	std::memcpy(&result, &f32, sizeof(float));
	return result;
}

void ImageData::EnsureDirectory(const std::string& filePath)
{
	namespace fs = std::filesystem;
	const fs::path parent = fs::path(filePath).parent_path();
	if (!parent.empty() && !fs::exists(parent))
		fs::create_directories(parent);
}

// ===========================================================================
// Data conversion
// ===========================================================================

std::vector<uint8_t> ImageData::ConvertHalfToU8(const void* data,
                                                 uint32_t width,
                                                 uint32_t height,
                                                 bool remapSigned)
{
	const auto* src = static_cast<const uint16_t*>(data);
	const size_t pixelCount = static_cast<size_t>(width) * height;
	std::vector<uint8_t> result(pixelCount * 4);

	for (size_t i = 0; i < pixelCount; ++i)
	{
		// Background pixels: clear value (0, 0, 0, 0) — leave transparent.
		const bool isBackground = (src[i * 4 + 0] == 0)
			&& (src[i * 4 + 1] == 0)
			&& (src[i * 4 + 2] == 0);

		for (int c = 0; c < 4; ++c)
		{
			uint16_t raw = src[i * 4 + c];
			float val = HalfToFloat(raw);
			if (remapSigned && c < 3 && !isBackground)
				val = (val + 1.0f) * 0.5f;
			val = (val < 0.0f) ? 0.0f : ((val > 1.0f) ? 1.0f : val);
			result[i * 4 + c] = static_cast<uint8_t>(val * 255.0f + 0.5f);
		}
		if (!isBackground)
			result[i * 4 + 3] = 255;
	}
	return result;
}

std::vector<uint8_t> ImageData::ConvertFloatToU8(const void* data,
                                                 uint32_t width,
                                                 uint32_t height,
                                                 bool remapSigned)
{
	const auto* src = static_cast<const float*>(data);
	const size_t pixelCount = static_cast<size_t>(width) * height;
	std::vector<uint8_t> result(pixelCount * 4);

	for (size_t i = 0; i < pixelCount; ++i)
	{
		// Background pixels: clear value (0, 0, 0) — leave transparent.
		const bool isBackground = (src[i * 4 + 0] == 0.0f)
			&& (src[i * 4 + 1] == 0.0f)
			&& (src[i * 4 + 2] == 0.0f);

		for (int c = 0; c < 4; ++c)
		{
			float val = src[i * 4 + c];
			if (remapSigned && c < 3 && !isBackground)
				val = (val + 1.0f) * 0.5f;
			val = (val < 0.0f) ? 0.0f : ((val > 1.0f) ? 1.0f : val);
			result[i * 4 + c] = static_cast<uint8_t>(val * 255.0f + 0.5f);
		}
		if (!isBackground)
			result[i * 4 + 3] = 255;
	}
	return result;
}

void ImageData::SwizzleBGRtoRGB(void* data, uint32_t width,
                                 uint32_t height, uint32_t channels)
{
	auto* bytes = static_cast<uint8_t*>(data);
	const size_t pixelCount = static_cast<size_t>(width) * height;
	for (size_t i = 0; i < pixelCount; ++i)
	{
		uint8_t tmp = bytes[i * channels + 0];
		bytes[i * channels + 0] = bytes[i * channels + 2];
		bytes[i * channels + 2] = tmp;
	}
}

// ===========================================================================
// Constructors
// ===========================================================================

ImageData::ImageData(const std::string& path)
	: m_path(path)
{
	ReloadContent();
}

void ImageData::ReloadContent()
{
	if (m_path.empty())
		return;

	im_pixelData.clear();
	im_width = 0;
	im_height = 0;
	im_format = {};
	LoadFromPath(ResolveAssetPath(m_path));
}

ImageData::ImageData(const void* data, uint32_t w, uint32_t h, PixelFormat fmt,
                     uint32_t arrayLayers)
	: im_width(w)
	, im_height(h)
	, im_format(fmt)
{
	if (!data || w == 0 || h == 0 || arrayLayers == 0)
		return;

	const uint32_t bpp = neurus::PixelByteSize(fmt);
	if (bpp == 0)
		return;

	const size_t byteCount = static_cast<size_t>(w) * h * bpp * arrayLayers;
	im_pixelData.resize(byteCount);
	std::memcpy(im_pixelData.data(), data, byteCount);
}

// ===========================================================================
// SavePNG (member function — uses owned pixel data)
// ===========================================================================

bool ImageData::SavePNG(const std::string& path, bool remapSigned) const
{
	const uint32_t channels = ChannelCount();
	// R32U conversion produces 4-channel RGBA, override channel count
	const uint32_t outChannels = (im_format == PixelFormat::R32U) ? 4u : channels;
	std::vector<uint8_t> pngData;

	if (im_format == PixelFormat::RGBA16F ||
	    im_format == PixelFormat::RGBA16U ||
	    im_format == PixelFormat::RGBA16SN)
	{
		pngData = ConvertHalfToU8(im_pixelData.data(), im_width, im_height, remapSigned);
	}
	else if (im_format == PixelFormat::R32U)
	{
		// Convert R32_UINT → RGBA8: map each uint32 ID to a visible colour.
		const auto* src = static_cast<const uint32_t*>(
			static_cast<const void*>(im_pixelData.data()));
		const size_t pixelCount = static_cast<size_t>(im_width) * im_height;
		pngData.resize(pixelCount * 4);
		for (size_t i = 0; i < pixelCount; ++i)
		{
			const uint32_t id = src[i];
			pngData[i * 4 + 0] = static_cast<uint8_t>(id & 0xFF);          // R
			pngData[i * 4 + 1] = static_cast<uint8_t>((id >> 8) & 0xFF);   // G
			pngData[i * 4 + 2] = static_cast<uint8_t>((id >> 16) & 0xFF);  // B
			pngData[i * 4 + 3] = (id == 0) ? 0 : 255;                     // A=0 for background
		}
	}
	else
	{
		pngData = im_pixelData;
		if (neurus::IsBGRFormat(im_format) && channels >= 3)
			SwizzleBGRtoRGB(pngData.data(), im_width, im_height, channels);
	}

	EnsureDirectory(path);
	const int stride = static_cast<int>(im_width) * static_cast<int>(outChannels);
	return stbi_write_png(path.c_str(),
	                      static_cast<int>(im_width),
	                      static_cast<int>(im_height),
	                      static_cast<int>(outChannels),
	                      pngData.data(), stride) != 0;
}

// ===========================================================================
// SaveHDR (member function — uses owned pixel data)
// ===========================================================================

bool ImageData::SaveHDR(const std::string& path) const
{
	const auto* src = reinterpret_cast<const float*>(im_pixelData.data());

	std::string out;
	out.reserve(256 + static_cast<size_t>(im_width) * im_height * 4);

	// --- Radiance header ---
	out += "#?RADIANCE\n";
	out += "FORMAT=32-bit_rle_rgbe\n";
	out += "\n";
	out += "-Y " + std::to_string(im_height) + " +X " + std::to_string(im_width) + "\n";

	// --- RGBE pixel data (flat, non-RLE) ---
	for (uint32_t y = 0; y < im_height; ++y)
	{
		for (uint32_t x = 0; x < im_width; ++x)
		{
			const size_t idx = (static_cast<size_t>(y) * im_width + x) * 4;
			const float r = src[idx + 0];
			const float g = src[idx + 1];
			const float b = src[idx + 2];

			// Find maximum channel and encode as RGBE
			const float maxVal = std::max({r, g, b});

			if (maxVal < 1e-32f)
			{
				out.push_back(static_cast<char>(0));
				out.push_back(static_cast<char>(0));
				out.push_back(static_cast<char>(0));
				out.push_back(static_cast<char>(0));
			}
			else
			{
				int e = 0;
				float m = std::frexp(maxVal, &e);
				const float scale = (m * 255.9999f) / maxVal;

				const uint8_t re = static_cast<uint8_t>(r * scale);
				const uint8_t ge = static_cast<uint8_t>(g * scale);
				const uint8_t be = static_cast<uint8_t>(b * scale);
				const uint8_t ee = static_cast<uint8_t>(e + 128);

				out.push_back(static_cast<char>(re));
				out.push_back(static_cast<char>(ge));
				out.push_back(static_cast<char>(be));
				out.push_back(static_cast<char>(ee));
			}
		}
	}

	// --- Write file ---
	EnsureDirectory(path);

	std::ofstream file(path, std::ios::binary);
	if (!file.is_open())
	{
		return false;
	}

	file.write(out.data(), out.size());
	return file.good();
}

// ===========================================================================
// LoadFromPath (private — fills member fields)
// ===========================================================================

void ImageData::LoadFromPath(const std::string& path)
{
	if (stbi_is_hdr(path.c_str()))
	{
		int w = 0, h = 0, c = 0;
		float* data = stbi_loadf(path.c_str(), &w, &h, &c, 4);
		if (!data || w <= 0 || h <= 0)
		{
			NEURUS_ERR("[ImageData] Failed to load HDR image from path: " << path);
			return;
		}

		NEURUS_LOG("[ImageData] Loaded HDR: " << path << " (" << w << "x" << h << ", " << c << " channels)");

		im_width = static_cast<uint32_t>(w);
		im_height = static_cast<uint32_t>(h);
		im_format = PixelFormat::RGBA32F;

		const size_t byteCount = static_cast<size_t>(w) * static_cast<size_t>(h) * 4 * sizeof(float);
		im_pixelData.resize(byteCount);
		std::memcpy(im_pixelData.data(), data, byteCount);
		stbi_image_free(data);
	}
	else
	{
		int w = 0, h = 0, c = 0;
		stbi_uc* data = stbi_load(path.c_str(), &w, &h, &c, 4);
		if (!data || w <= 0 || h <= 0)
		{
			NEURUS_ERR("[ImageData] Failed to load LDR image from path: " << path);
			return;
		}

		NEURUS_LOG("[ImageData] Loaded LDR: " << path << " (" << w << "x" << h << ", " << c << " channels)");

		im_width = static_cast<uint32_t>(w);
		im_height = static_cast<uint32_t>(h);
		im_format = PixelFormat::RGBA8S;

		const size_t byteCount = static_cast<size_t>(w) * static_cast<size_t>(h) * 4;
		im_pixelData.resize(byteCount);
		std::memcpy(im_pixelData.data(), data, byteCount);
		stbi_image_free(data);
	}
}

} // namespace neurus
