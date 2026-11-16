/**
 * @file TestReferenceImage.hpp
 * @brief Shared reference-image regression test utilities.
 *
 * Provides ReferencePath, ComparePixels, and CheckReferenceOrGenerate
 * for the first-run-generates / second-run-compares pattern used by
 * GPU reference-image regression tests.
 */
#pragma once

#include "asset/data/ImageData.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

namespace neurus {
namespace test {

/// @brief Encapsulates the reference image base directory and path construction.
struct ReferencePath
{
	/// @brief Base directory for reference images, relative from CTest CWD (CMAKE_BINARY_DIR).
	/// Computed at CMake configure time via file(RELATIVE_PATH ...).
	static constexpr const char* kReferenceDir = TEST_RELATIVE_PROJECT_DIR "/test/render/reference/";

	/// @brief Build a full reference path by concatenating the base dir with a relative suffix.
	static std::string Make(const std::string& relative)
	{
		return std::string(kReferenceDir) + relative;
	}
};

/**
 * @brief Compare two RGBA8 images pixel-by-pixel.
 * @return Number of pixels where any channel exceeds maxDiffPerChannel.
 */
inline int ComparePixels(
	const uint8_t* a, const uint8_t* b,
	int width, int height,
	int maxDiffPerChannel = 2)
{
	const size_t pixelCount = static_cast<size_t>(width) * height * 4;
	int badPixels = 0;
	for (size_t i = 0; i < pixelCount; i += 4)
	{
		for (int c = 0; c < 4; ++c)
		{
			if (std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])) > maxDiffPerChannel)
			{
				++badPixels;
				break;
			}
		}
	}
	return badPixels;
}

/**
 * @brief Reference image regression check.
 *
 * Caller must have already saved the captured image to `refPath + ".tmp"`.
 *
 * - If reference doesn't exist: renames .tmp → refPath, returns -1
 *   (caller should issue GTEST_SKIP)
 * - If reference exists: loads both via ImageData(path) constructor,
 *   compares via ComparePixels. Passes (returns 0) if the fraction of
 *   differing pixels is within maxBadPixelRatio, tolerating small
 *   platform-specific floating-point/texture-sampling differences (e.g.
 *   GPU vendor, driver, or MoltenVK vs. native Vulkan) around specular
 *   highlights and shadow edges. Removes .tmp only when the check passes.
 *
 * @param refPath           Full path to the reference PNG file.
 * @param maxDiffPerChannel  Per-channel pixel tolerance (default 2).
 * @param maxBadPixelRatio   Maximum fraction (0..1) of pixels allowed to
 *                           exceed maxDiffPerChannel before the test fails
 *                           (default 0.0 = every pixel must match exactly
 *                           within tolerance). E.g. 0.01 allows up to 1%
 *                           of pixels to differ.
 * @return -1 if reference was just generated (caller: GTEST_SKIP),
 *         >=0 bad pixel count otherwise (0 means the check passed, even
 *         if some pixels differed but stayed within maxBadPixelRatio).
 */
inline int CheckReferenceOrGenerate(const std::string& refPath,
                                     int maxDiffPerChannel = 2,
                                     double maxBadPixelRatio = 0.0)
{
	const std::string tmpPath = refPath + ".tmp";

	if (!std::filesystem::exists(refPath))
	{
		// First run — rename .tmp to reference
		std::filesystem::create_directories(std::filesystem::path(refPath).parent_path());
		std::rename(tmpPath.c_str(), refPath.c_str());
		return -1;
	}

	// Second run — load and compare
	auto tmpResult = ImageData(tmpPath);
	auto refResult = ImageData(refPath);

	if (!tmpResult.IsValid() || !refResult.IsValid())
		return -2;  // load failure

	const int width  = static_cast<int>(tmpResult.GetWidth());
	const int height = static_cast<int>(tmpResult.GetHeight());

	int bad = ComparePixels(
		tmpResult.GetPixelData().data(), refResult.GetPixelData().data(),
		width, height, maxDiffPerChannel);

	const size_t totalPixels = static_cast<size_t>(width) * height;
	const size_t maxAllowedBad =
		static_cast<size_t>(maxBadPixelRatio * static_cast<double>(totalPixels));
	const bool passed = static_cast<size_t>(bad) <= maxAllowedBad;

	if (passed)
	{
		std::remove(tmpPath.c_str());
		return 0;
	}
	return bad;
}

/**
 * @brief Compare two RGBA32F float images pixel-by-pixel.
 * @return Number of pixels where any channel's absolute difference exceeds
 *         maxDiffPerChannel.
 */
inline int ComparePixelsFloat(
	const float* a, const float* b,
	int width, int height,
	float maxDiffPerChannel = 0.02f)
{
	const size_t floatCount = static_cast<size_t>(width) * height * 4;
	int badPixels = 0;
	for (size_t i = 0; i < floatCount; i += 4)
	{
		for (int c = 0; c < 4; ++c)
		{
			if (std::fabs(a[i + c] - b[i + c]) > maxDiffPerChannel)
			{
				++badPixels;
				break;
			}
		}
	}
	return badPixels;
}

/**
 * @brief Reference image regression check for RGBA32F .hdr files.
 *
 * Mirror of CheckReferenceOrGenerate but compares float pixel data with an
 * absolute per-channel tolerance, tolerating the RGBE round-trip quantization
 * of the Radiance HDR format and cross-platform floating-point differences
 * (MoltenVK/Metal vs. native Vulkan). Caller must have saved the captured
 * image to `refPath + ".tmp"`.
 *
 * @param refPath           Full path to the reference .hdr file.
 * @param maxDiffPerChannel Absolute per-channel float tolerance (default 0.02).
 * @param maxBadPixelRatio  Maximum fraction (0..1) of pixels allowed to exceed
 *                          the tolerance before the test fails (default 0.02).
 * @return -1 if reference was just generated (caller: GTEST_SKIP),
 *         -2 on load failure,
 *         >=0 bad pixel count otherwise (0 means the check passed).
 */
inline int CheckReferenceOrGenerateHDR(const std::string& refPath,
                                        float maxDiffPerChannel = 0.02f,
                                        double maxBadPixelRatio = 0.02)
{
	const std::string tmpPath = refPath + ".tmp";

	if (!std::filesystem::exists(refPath))
	{
		std::filesystem::create_directories(std::filesystem::path(refPath).parent_path());
		std::rename(tmpPath.c_str(), refPath.c_str());
		return -1;
	}

	auto tmpResult = ImageData(tmpPath);
	auto refResult = ImageData(refPath);

	if (!tmpResult.IsValid() || !refResult.IsValid())
		return -2;
	if (tmpResult.GetWidth() != refResult.GetWidth()
		|| tmpResult.GetHeight() != refResult.GetHeight())
		return -2;

	const int width  = static_cast<int>(tmpResult.GetWidth());
	const int height = static_cast<int>(tmpResult.GetHeight());

	int bad = ComparePixelsFloat(
		reinterpret_cast<const float*>(tmpResult.GetPixelData().data()),
		reinterpret_cast<const float*>(refResult.GetPixelData().data()),
		width, height, maxDiffPerChannel);

	const size_t totalPixels = static_cast<size_t>(width) * height;
	const size_t maxAllowedBad =
		static_cast<size_t>(maxBadPixelRatio * static_cast<double>(totalPixels));
	const bool passed = static_cast<size_t>(bad) <= maxAllowedBad;

	if (passed)
	{
		std::remove(tmpPath.c_str());
		return 0;
	}
	return bad;
}

} // namespace test
} // namespace neurus
