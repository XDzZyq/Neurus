// Must define platform before including Vulkan headers


#include "shared/TestVulkanShared.h"

#include <gtest/gtest.h>

#include "render/Image.h"
#include "render/Barrier.h"

#include <vulkan/vulkan_raii.hpp>

using namespace neurus;

/**
 * @brief Tests for Image - RAII image creation, layout transition, and mipmap generation.
 *
 * @note These tests require a Vulkan 1.4-capable GPU. They will be skipped
 *       in CI environments without GPU access.
 */
class ImageTest : public VulkanTestShared
{
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_F(ImageTest, Create2D_ValidHandles)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	const vk::Extent2D extent(256, 256);
	auto& pd = PhysicalDevice();

	Image image(*m_device, pd, extent, vk::Format::eR8G8B8A8Unorm,
	                  vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
	                  1);

	EXPECT_TRUE(*image.ImageHandle());
	EXPECT_TRUE(*image.ImageViewHandle());
	EXPECT_TRUE(*image.DeviceMemoryHandle());
	EXPECT_EQ(image.Extent(), extent);
	EXPECT_EQ(image.Format(), vk::Format::eR8G8B8A8Unorm);
	EXPECT_EQ(image.MipLevels(), 1u);
	EXPECT_EQ(image.ArrayLayers(), 1u);
}

TEST_F(ImageTest, Create2D_WithMipLevels)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	const vk::Extent2D extent(512, 512);
	const uint32_t mipLevels = 4;
	auto& pd = PhysicalDevice();

	Image image(*m_device, pd, extent, vk::Format::eR8G8B8A8Unorm,
	                  vk::ImageUsageFlagBits::eSampled |
	                      vk::ImageUsageFlagBits::eTransferSrc |
	                      vk::ImageUsageFlagBits::eTransferDst,
	                  mipLevels);

	EXPECT_TRUE(*image.ImageHandle());
	EXPECT_EQ(image.MipLevels(), mipLevels);
}

TEST_F(ImageTest, CreateCube_ValidHandles)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();
	const vk::Extent2D extent(128, 128);

	Image image(*m_device, pd, extent, vk::Format::eR8G8B8A8Unorm,
	                  vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
	                  1,
	                  Image::ImageType::eCube);

	EXPECT_EQ(image.ArrayLayers(), 6u);
}

TEST_F(ImageTest, CreateDepthStencil_ValidHandles)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();
	const vk::Extent2D extent(1024, 768);

	// Check if depth format is supported
	auto fmtProps = pd.getFormatProperties(vk::Format::eD32Sfloat);
	if (!(fmtProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment))
	{
		GTEST_SKIP() << "D32_SFLOAT depth attachment not supported.";
	}

	Image image(*m_device, pd, extent, vk::Format::eD32Sfloat,
	                  vk::ImageUsageFlagBits::eDepthStencilAttachment |
	                      vk::ImageUsageFlagBits::eSampled,
	                  1,
	                  Image::ImageType::eDepthStencil);

	EXPECT_TRUE(*image.ImageHandle());
	EXPECT_TRUE(*image.ImageViewHandle());
	EXPECT_TRUE(*image.DeviceMemoryHandle());
	EXPECT_EQ(image.Format(), vk::Format::eD32Sfloat);
}

// ---------------------------------------------------------------------------
// Layout transitions
// ---------------------------------------------------------------------------

TEST_F(ImageTest, TransitionLayout_Basic)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();
	const vk::Extent2D extent(64, 64);

	Image image(*m_device, pd, extent, vk::Format::eR8G8B8A8Unorm,
	                  vk::ImageUsageFlagBits::eSampled |
	                      vk::ImageUsageFlagBits::eColorAttachment |
	                      vk::ImageUsageFlagBits::eTransferDst,
	                  1);

	{
		auto& cmd = BeginCmd();
		// UNDEFINED → ColorAttachment
		Barrier::Transition(*cmd, image, ImageState::ColorAttachment);

		// ColorAttachment → ShaderRead
		Barrier::Transition(*cmd, image, ImageState::ColorShaderRead);
		EndSubmitWait(cmd);
	}

	SUCCEED();
}

TEST_F(ImageTest, TransitionLayout_MipLevels)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();
	const vk::Extent2D extent(256, 256);

	Image image(*m_device, pd, extent, vk::Format::eR8G8B8A8Unorm,
	                  vk::ImageUsageFlagBits::eSampled |
	                      vk::ImageUsageFlagBits::eTransferSrc |
	                      vk::ImageUsageFlagBits::eTransferDst,
	                  4);

	{
		auto& cmd = BeginCmd();
		// Transition all 4 mip levels
		vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 0, 4, 0, 1);
		Barrier::Transition(*cmd, image, ImageState::TransferDst, range);
		EndSubmitWait(cmd);
	}

	SUCCEED();
}

TEST_F(ImageTest, TransitionLayout_SingleMipLevel)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();
	const vk::Extent2D extent(128, 128);

	Image image(*m_device, pd, extent, vk::Format::eR8G8B8A8Unorm,
	                  vk::ImageUsageFlagBits::eSampled |
	                      vk::ImageUsageFlagBits::eTransferSrc |
	                      vk::ImageUsageFlagBits::eTransferDst,
	                  4);

	{
		auto& cmd = BeginCmd();
		// Transition only mip level 1
		vk::ImageSubresourceRange range(vk::ImageAspectFlagBits::eColor, 1, 1, 0, 1);
		Barrier::Transition(*cmd, image, ImageState::TransferSrc, range);
		EndSubmitWait(cmd);
	}

	SUCCEED();
}

// ---------------------------------------------------------------------------
// Readback restores the image to the state it entered in
// ---------------------------------------------------------------------------

TEST_F(ImageTest, ReadImageData_RestoresStateAndContent)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();
	const vk::Extent2D extent(4, 4);

	// Distinct per-pixel values so a second read can be compared byte for byte.
	std::vector<uint8_t> pixels(static_cast<size_t>(extent.width) * extent.height * 4);
	for (size_t i = 0; i < pixels.size(); i += 4)
	{
		pixels[i + 0] = static_cast<uint8_t>(i * 3);
		pixels[i + 1] = static_cast<uint8_t>(255 - i);
		pixels[i + 2] = static_cast<uint8_t>(i);
		pixels[i + 3] = 255;
	}
	ImageData src(pixels.data(), extent.width, extent.height, PixelFormat::RGBA8U);
	Image image = Image::FromImageData(*m_device, pd, *m_queue, m_graphicsQueueFamily, src);
	ASSERT_EQ(image.State(), ImageState::ColorShaderRead);

	auto first = image.ReadImageData(*m_device, pd, *m_queue, m_graphicsQueueFamily);
	ASSERT_NE(first, nullptr);
	ASSERT_TRUE(first->IsValid());
	EXPECT_EQ(image.State(), ImageState::ColorShaderRead)
		<< "ReadImageData returns the image to the state it entered in";

	// Second read: the tracked state still matches the real layout, so this is a
	// real transition (not a no-op against a stale layout) and the contents are
	// unchanged. A readback that stranded the image in TRANSFER_SRC would make
	// the next transition a silent no-op.
	auto second = image.ReadImageData(*m_device, pd, *m_queue, m_graphicsQueueFamily);
	ASSERT_NE(second, nullptr);
	EXPECT_EQ(first->GetPixelData(), second->GetPixelData());
	EXPECT_EQ(image.State(), ImageState::ColorShaderRead);
}

TEST_F(ImageTest, ReadImageData_RefusesUnwrittenImage)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();
	Image image(*m_device, pd, vk::Extent2D(4, 4), vk::Format::eR8G8B8A8Unorm,
	            vk::ImageUsageFlagBits::eSampled |
	                vk::ImageUsageFlagBits::eTransferSrc,
	            1);
	ASSERT_EQ(image.State(), ImageState::Undefined);

	auto data = image.ReadImageData(*m_device, pd, *m_queue, m_graphicsQueueFamily);

	// Nothing to read, and no barrier: returning to UNDEFINED as a newLayout is
	// invalid, so the transition is never recorded.
	EXPECT_EQ(data, nullptr);
	EXPECT_EQ(image.State(), ImageState::Undefined);
}

// ---------------------------------------------------------------------------
// Mipmap generation
// ---------------------------------------------------------------------------

TEST_F(ImageTest, GenerateMipmaps_Completes)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();

	// Check that blitting from a linear-tiled format is supported
	auto fmtProps = pd.getFormatProperties(vk::Format::eR8G8B8A8Unorm);
	if (!(fmtProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitSrc) ||
	    !(fmtProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst))
	{
		GTEST_SKIP() << "RGBA8 blit not supported by GPU.";
	}

	const vk::Extent2D extent(256, 256);

	Image image(*m_device, pd, extent, vk::Format::eR8G8B8A8Unorm,
	                  vk::ImageUsageFlagBits::eSampled |
	                      vk::ImageUsageFlagBits::eTransferSrc |
	                      vk::ImageUsageFlagBits::eTransferDst,
	                  4);

	{
		auto& cmd = BeginCmd();
		// First transition to TRANSFER_DST so GenerateMipmaps can blit into lower levels
		Barrier::Transition(*cmd, image, ImageState::TransferDst);

		image.GenerateMipmaps(cmd);
		EndSubmitWait(cmd);
	}

	SUCCEED();
}

// ---------------------------------------------------------------------------
// Non-copyable, movable
// ---------------------------------------------------------------------------

TEST_F(ImageTest, NonCopyable)
{
	static_assert(!std::is_copy_constructible_v<Image>,
	              "Image must not be copy-constructible");
	static_assert(!std::is_copy_assignable_v<Image>,
	              "Image must not be copy-assignable");
	SUCCEED();
}

TEST_F(ImageTest, Movable)
{
	static_assert(std::is_move_constructible_v<Image>,
	              "Image must be move-constructible");
	static_assert(std::is_move_assignable_v<Image>,
	              "Image must be move-assignable");
	SUCCEED();
}
