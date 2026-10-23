#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"
#include "render/Screenshot.h"
#include "render/Image.h"
#include "render/Barrier.h"
#include "render/RenderCache.h"
#include "asset/data/ImageData.h"

#include <vulkan/vulkan_raii.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

using namespace neurus;

/**
 * @brief Tests for Screenshot - GPU image capture to PNG via staging buffer.
 *
 * Creates an Image, uploads known pixel data, captures to PNG,
 * and verifies file existence and non-zero size.
 *
 * @note These tests require a Vulkan 1.4-capable GPU. They will be skipped
 *       in CI environments without GPU access.
 */
class ScreenshotTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
	}

	void TearDown() override
	{
		// Clean up test output file
		if (!m_testOutputPath.empty() && std::filesystem::exists(m_testOutputPath))
		{
			std::filesystem::remove(m_testOutputPath);
		}
		VulkanTestShared::TearDown();
	}

	/** @brief Whether D32_SFLOAT can be a transfer source (readback) on this GPU. */
	static bool DepthReadbackSupported(const vk::raii::PhysicalDevice& pd)
	{
		return static_cast<bool>(pd.getFormatProperties(vk::Format::eD32Sfloat)
		                             .optimalTilingFeatures &
		                         vk::FormatFeatureFlagBits::eTransferSrc);
	}

	/**
	 * @brief Registers a sun-light shadow depth map shaped exactly like
	 *        UploadManager's, and returns it.
	 *
	 * @param lightUID UID to register the light under.
	 * @param extent   Shadow map resolution.
	 * @param rendered true  - leave it where ShadowDepthPass leaves it
	 *                         (rendered, DepthShaderRead);
	 *                 false - leave it never-written (Undefined).
	 */
	neurus::Image* RegisterSunShadowMap(neurus::RenderCache& cache,
	                                    int lightUID,
	                                    vk::Extent2D extent,
	                                    bool rendered)
	{
		auto depthMap = std::make_unique<neurus::Image>(
			*m_device, PhysicalDevice(), extent,
			vk::Format::eD32Sfloat,
			vk::ImageUsageFlagBits::eDepthStencilAttachment |
				vk::ImageUsageFlagBits::eSampled |
				vk::ImageUsageFlagBits::eTransferSrc,
			1,
			neurus::Image::ImageType::e2D,
			("SunShadowDepth_Light_" + std::to_string(lightUID)).c_str());

		if (rendered)
		{
			// The states ShadowDepthPass walks through for a sun cascade.
			auto& cmd = BeginCmd();
			neurus::Barrier::Transition(*cmd, *depthMap, neurus::ImageState::DepthAttachment);
			neurus::Barrier::Transition(*cmd, *depthMap, neurus::ImageState::DepthShaderRead);
			EndSubmitWait(cmd);
		}

		neurus::LightGPU lgpu;
		lgpu.shadowDepthMap = std::move(depthMap);
		cache.UseLightGPU(lightUID, std::move(lgpu));
		return cache.GetLightGPU(lightUID)->shadowDepthMap.get();
	}

	std::string m_testOutputPath;
};

// ---------------------------------------------------------------------------
// CaptureAttachment - RGBA8 image → PNG
// ---------------------------------------------------------------------------

TEST_F(ScreenshotTest, CaptureAttachment_RGBA8_WritesPngFile)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = m_physicalDevices[m_selectedPdIndex];
	const vk::Extent2D extent(64, 64);

	// --- Create an Image with red pixel data ---
	const size_t pixelCount = static_cast<size_t>(extent.width) * extent.height;
	std::vector<uint8_t> redPixels(pixelCount * 4);
	for (size_t i = 0; i < pixelCount; ++i)
	{
		redPixels[i * 4 + 0] = 255;  // R
		redPixels[i * 4 + 1] = 0;    // G
		redPixels[i * 4 + 2] = 0;    // B
		redPixels[i * 4 + 3] = 255;  // A
	}
	ImageData imgData(redPixels.data(), extent.width, extent.height, PixelFormat::RGBA8U);
	Image image = Image::FromImageData(*m_device, pd, m_queue, m_graphicsQueueFamily, imgData);
	ASSERT_NE(image.State(), ImageState::Invalid);

	// --- Capture to PNG ---
	m_testOutputPath = "screenshots/test_rgba8.png";
	bool result = Screenshot::CaptureAttachment(*m_device, pd,
	                                             m_queue, m_graphicsQueueFamily,
	                                             image, m_testOutputPath);
	EXPECT_TRUE(result);

	// --- Verify file exists and is non-empty ---
	EXPECT_TRUE(std::filesystem::exists(m_testOutputPath));
	auto fileSize = std::filesystem::file_size(m_testOutputPath);
	EXPECT_GT(fileSize, 0u) << "PNG file should be non-empty";
}

// ---------------------------------------------------------------------------
// CaptureAttachment - RGBA16F image → PNG (half-float conversion)
// ---------------------------------------------------------------------------

TEST_F(ScreenshotTest, CaptureAttachment_RGBA16F_WritesPngFile)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = m_physicalDevices[m_selectedPdIndex];

	// Check RGBA16_SFLOAT format support
	auto fmtProps = pd.getFormatProperties(vk::Format::eR16G16B16A16Sfloat);
	if (!(fmtProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eTransferSrc))
	{
		GTEST_SKIP() << "RGBA16_SFLOAT not supported as transfer source on this GPU.";
	}

	const vk::Extent2D extent(32, 32);

	// --- Upload half-float data (0.5, 0.25, 0.75, 1.0) ---
	// Half-float 0.5 = 0x3800, 0.25 = 0x3400, 0.75 = 0x3A00, 1.0 = 0x3C00
	const size_t pixelCount = static_cast<size_t>(extent.width) * extent.height;
	std::vector<uint16_t> halfData(pixelCount * 4);
	const uint16_t h0_5  = 0x3800;
	const uint16_t h0_25 = 0x3400;
	const uint16_t h0_75 = 0x3A00;
	const uint16_t h1_0  = 0x3C00;
	for (size_t i = 0; i < pixelCount; ++i)
	{
		halfData[i * 4 + 0] = h0_5;
		halfData[i * 4 + 1] = h0_25;
		halfData[i * 4 + 2] = h0_75;
		halfData[i * 4 + 3] = h1_0;
	}
	ImageData imgData(halfData.data(), extent.width, extent.height, PixelFormat::RGBA16F);
	Image image = Image::FromImageData(*m_device, pd, m_queue, m_graphicsQueueFamily, imgData);
	ASSERT_NE(image.State(), ImageState::Invalid);

	// --- Capture to PNG ---
	m_testOutputPath = "screenshots/test_rgba16f.png";
	bool result = Screenshot::CaptureAttachment(*m_device, pd,
	                                             m_queue, m_graphicsQueueFamily,
	                                             image, m_testOutputPath);
	EXPECT_TRUE(result);

	// --- Verify file exists and is non-empty ---
	EXPECT_TRUE(std::filesystem::exists(m_testOutputPath));
	auto fileSize = std::filesystem::file_size(m_testOutputPath);
	EXPECT_GT(fileSize, 0u) << "PNG file should be non-empty";
}

// ---------------------------------------------------------------------------
// CaptureAttachment - handles non-existent directory (auto-creation)
// ---------------------------------------------------------------------------

TEST_F(ScreenshotTest, CaptureAttachment_AutoCreatesDirectory)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = m_physicalDevices[m_selectedPdIndex];
	const vk::Extent2D extent(16, 16);

	const size_t pixelCount = static_cast<size_t>(extent.width) * extent.height;
	std::vector<uint8_t> pixels(pixelCount * 4, 128);
	ImageData imgData(pixels.data(), extent.width, extent.height, PixelFormat::RGBA8U);
	Image image = Image::FromImageData(*m_device, pd, m_queue, m_graphicsQueueFamily, imgData);
	ASSERT_NE(image.State(), ImageState::Invalid);

	// --- Use a nested directory that doesn't exist ---
	const std::string nestedPath = "screenshots/nested/subdir/test_auto_dir.png";

	// Clean up potential leftover
	if (std::filesystem::exists("screenshots/nested"))
	{
		std::filesystem::remove_all("screenshots/nested");
	}

	m_testOutputPath = nestedPath;
	bool result = Screenshot::CaptureAttachment(*m_device, pd,
	                                             m_queue, m_graphicsQueueFamily,
	                                             image, nestedPath);
	EXPECT_TRUE(result);
	EXPECT_TRUE(std::filesystem::exists(nestedPath));
	auto fileSize = std::filesystem::file_size(nestedPath);
	EXPECT_GT(fileSize, 0u) << "PNG file should be non-empty";

	// Clean up nested test directory
	if (std::filesystem::exists("screenshots/nested"))
	{
		std::filesystem::remove_all("screenshots/nested");
		m_testOutputPath.clear();
	}
}

// ---------------------------------------------------------------------------
// ExportShadowDepth - leaves the depth map where the shadow pass left it
// ---------------------------------------------------------------------------

TEST_F(ScreenshotTest, ExportShadowDepth_RestoresDepthMapState)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();
	if (!DepthReadbackSupported(pd))
	{
		GTEST_SKIP() << "D32_SFLOAT is not a transfer source on this GPU.";
	}

	RenderCache cache(*m_device, pd);
	constexpr int kLightUID = 7;
	const vk::Extent2D extent(32, 32);

	Image* depthMap = RegisterSunShadowMap(cache, kLightUID, extent, /*rendered=*/true);
	ASSERT_NE(depthMap, nullptr);
	ASSERT_EQ(depthMap->State(), ImageState::DepthShaderRead);

	Screenshot screenshot(*m_device, pd, m_queue, m_graphicsQueueFamily);
	m_testOutputPath = screenshot.ExportShadowDepth(cache, kLightUID,
	                                                "screenshots/test_sun_shadow");

	EXPECT_FALSE(m_testOutputPath.empty()) << "the export should write a PNG";
	if (!m_testOutputPath.empty())
	{
		EXPECT_TRUE(std::filesystem::exists(m_testOutputPath));
	}

	// The readback must hand the map back in the state it entered in. Stranded in
	// TRANSFER_SRC (the old code skipped its restore because the tracked state was
	// never updated), the next shadow pass records its barrier from a state the
	// image is not in, and the sampling pass then draws from a transfer layout
	// (VUID-vkCmdDraw-None-09600).
	EXPECT_EQ(depthMap->State(), ImageState::DepthShaderRead)
		<< "ExportShadowDepth must not change the depth map's state";
}

TEST_F(ScreenshotTest, ExportShadowDepth_UnwrittenMapReturnsEmpty)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();
	RenderCache cache(*m_device, pd);
	constexpr int kLightUID = 8;

	Image* depthMap = RegisterSunShadowMap(cache, kLightUID, vk::Extent2D(32, 32),
	                                       /*rendered=*/false);
	ASSERT_NE(depthMap, nullptr);
	ASSERT_EQ(depthMap->State(), ImageState::Undefined);

	Screenshot screenshot(*m_device, pd, m_queue, m_graphicsQueueFamily);

	// A map that was never rendered has nothing to export, and reading it would
	// have to restore it to UNDEFINED - an invalid newLayout.
	EXPECT_TRUE(screenshot.ExportShadowDepth(cache, kLightUID,
	                                         "screenshots/test_sun_shadow").empty());
	EXPECT_EQ(depthMap->State(), ImageState::Undefined);
}

// ---------------------------------------------------------------------------
// CaptureAttachment - non-copyable class check
// ---------------------------------------------------------------------------
TEST_F(ScreenshotTest, NonCopyable)
{
	static_assert(!std::is_copy_constructible_v<Screenshot>,
	              "Screenshot must not be copy-constructible");
	static_assert(!std::is_copy_assignable_v<Screenshot>,
	              "Screenshot must not be copy-assignable");
	static_assert(!std::is_move_constructible_v<Screenshot>,
	              "Screenshot must not be move-constructible (stores references)");
	static_assert(!std::is_move_assignable_v<Screenshot>,
	              "Screenshot must not be move-assignable (stores references)");
	SUCCEED();
}
