#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

#include "render/RenderCache.h"
#include "render/Image.h"

using namespace neurus;

/**
 * @brief Tests for RenderCache - G-Buffer and post-FX attachment creation, resize, and named access.
 *
 * @note These tests require a Vulkan 1.4-capable GPU. They will be skipped
 *       in CI environments without GPU access.
 */
class RenderCacheTest : public VulkanTestShared
{
protected:
	// SetUp/TearDown inherited from VulkanTestShared
};

// ---------------------------------------------------------------------------
// Creation - G-Buffer attachments
// ---------------------------------------------------------------------------

TEST_F(RenderCacheTest, CreateGBuffer_AllAttachmentsHaveCorrectFormatAndExtent)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	const vk::Extent2D extent(1920, 1080);
	auto& pd = PhysicalDevice();

	// Depth format support check
	auto depthFmtProps = pd.getFormatProperties(vk::Format::eD32Sfloat);
	if (!(depthFmtProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment))
	{
		GTEST_SKIP() << "D32_SFLOAT depth attachment not supported.";
	}

	RenderCache manager(*m_device, pd);
	manager.SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), *m_queue, m_graphicsQueueFamily));

	// Lazy creation via first GetAttachment call
	manager.GetAttachment(AttachmentName::Position, extent);

	// --- Position (RGBA16_SFLOAT) ---
	{
		const auto& attachment = manager.GetAttachment(AttachmentName::Position, extent);
		EXPECT_EQ(attachment.Extent(), extent);
		EXPECT_EQ(attachment.Format(), vk::Format::eR16G16B16A16Sfloat);
		EXPECT_EQ(attachment.MipLevels(), 1u);
		EXPECT_EQ(attachment.ArrayLayers(), 1u);
		EXPECT_NE(attachment.Type(), Image::ImageType::eDepthStencil);
	}

	// --- Normal (RGBA16_SFLOAT) ---
	{
		const auto& attachment = manager.GetAttachment(AttachmentName::Normal, extent);
		EXPECT_EQ(attachment.Extent(), extent);
		EXPECT_EQ(attachment.Format(), vk::Format::eR16G16B16A16Sfloat);
		EXPECT_EQ(attachment.MipLevels(), 1u);
	}

	// --- Albedo (RGBA8_SRGB) ---
	{
		const auto& attachment = manager.GetAttachment(AttachmentName::Albedo, extent);
		EXPECT_EQ(attachment.Extent(), extent);
		EXPECT_EQ(attachment.Format(), vk::Format::eR8G8B8A8Srgb);
		EXPECT_EQ(attachment.MipLevels(), 1u);
	}

	// --- MetallicRoughness (RGBA8_UNORM) ---
	{
		const auto& attachment = manager.GetAttachment(AttachmentName::MetallicRoughness, extent);
		EXPECT_EQ(attachment.Extent(), extent);
		EXPECT_EQ(attachment.Format(), vk::Format::eR8G8B8A8Unorm);
		EXPECT_EQ(attachment.MipLevels(), 1u);
	}

	// --- Depth (D32_SFLOAT) ---
	{
		const auto& attachment = manager.GetAttachment(AttachmentName::Depth, extent);
		EXPECT_EQ(attachment.Extent(), extent);
		EXPECT_EQ(attachment.Format(), vk::Format::eD32Sfloat);
		EXPECT_EQ(attachment.MipLevels(), 1u);
		EXPECT_EQ(attachment.Type(), Image::ImageType::eDepthStencil);
	}
}

// ---------------------------------------------------------------------------
// Creation - Post-FX attachments
// ---------------------------------------------------------------------------

TEST_F(RenderCacheTest, CreatePostFX_AllAttachmentsHaveCorrectFormatAndExtent)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	const vk::Extent2D extent(1920, 1080);
	auto& pd = PhysicalDevice();

	RenderCache manager(*m_device, pd);
	manager.SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), *m_queue, m_graphicsQueueFamily));

	// Lazy creation via first GetAttachment call
	manager.GetAttachment(AttachmentName::Position, extent);

	// --- HDRColor (RGBA16F) ---
	{
		const auto& attachment = manager.GetAttachment(AttachmentName::HDRColor, extent);
		EXPECT_EQ(attachment.Extent(), extent);
		EXPECT_EQ(attachment.Format(), vk::Format::eR16G16B16A16Sfloat);
		EXPECT_EQ(attachment.MipLevels(), 1u);
	}

	// --- SSAO (R8) ---
	{
		const auto& attachment = manager.GetAttachment(AttachmentName::SSAO, extent);
		EXPECT_EQ(attachment.Extent(), extent);
		EXPECT_EQ(attachment.Format(), vk::Format::eR8Unorm);
		EXPECT_EQ(attachment.MipLevels(), 1u);
	}

	// --- SSR (RGBA16F) ---
	{
		const auto& attachment = manager.GetAttachment(AttachmentName::SSR, extent);
		EXPECT_EQ(attachment.Extent(), extent);
		EXPECT_EQ(attachment.Format(), vk::Format::eR16G16B16A16Sfloat);
		EXPECT_EQ(attachment.MipLevels(), 1u);
	}
}

// ---------------------------------------------------------------------------
// Resize
// ---------------------------------------------------------------------------

TEST_F(RenderCacheTest, Resize_AllAttachmentsHaveNewExtent)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();

	auto depthFmtProps = pd.getFormatProperties(vk::Format::eD32Sfloat);
	if (!(depthFmtProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment))
	{
		GTEST_SKIP() << "D32_SFLOAT depth attachment not supported.";
	}

	const vk::Extent2D initialExtent(1920, 1080);
	const vk::Extent2D newExtent(3840, 2160);

	RenderCache manager(*m_device, pd);
	manager.SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), *m_queue, m_graphicsQueueFamily));

	// Lazy creation at initial extent via first GetAttachment
	manager.GetAttachment(AttachmentName::Position, initialExtent);
	manager.GetAttachment(AttachmentName::Depth, initialExtent);
	manager.GetAttachment(AttachmentName::HDRColor, initialExtent);

	// All attachments have initial extent
	EXPECT_EQ(manager.GetAttachment(AttachmentName::Position, initialExtent).Extent(), initialExtent);
	EXPECT_EQ(manager.GetAttachment(AttachmentName::Depth, initialExtent).Extent(), initialExtent);
	EXPECT_EQ(manager.GetAttachment(AttachmentName::HDRColor, initialExtent).Extent(), initialExtent);

	// Simulate resize: clear screen-space, then lazy re-create at new extent
	manager.CleanScreenSpace();

	// Trigger lazy re-creation at new extent
	manager.GetAttachment(AttachmentName::Position, newExtent);
	manager.GetAttachment(AttachmentName::Normal, newExtent);
	manager.GetAttachment(AttachmentName::Albedo, newExtent);
	manager.GetAttachment(AttachmentName::MetallicRoughness, newExtent);
	manager.GetAttachment(AttachmentName::Depth, newExtent);
	manager.GetAttachment(AttachmentName::HDRColor, newExtent);
	manager.GetAttachment(AttachmentName::SSAO, newExtent);
	manager.GetAttachment(AttachmentName::SSR, newExtent);

	// All attachments have new extent
	EXPECT_EQ(manager.GetAttachment(AttachmentName::Position, newExtent).Extent(), newExtent);
	EXPECT_EQ(manager.GetAttachment(AttachmentName::Normal, newExtent).Extent(), newExtent);
	EXPECT_EQ(manager.GetAttachment(AttachmentName::Albedo, newExtent).Extent(), newExtent);
	EXPECT_EQ(manager.GetAttachment(AttachmentName::MetallicRoughness, newExtent).Extent(), newExtent);
	EXPECT_EQ(manager.GetAttachment(AttachmentName::Depth, newExtent).Extent(), newExtent);
	EXPECT_EQ(manager.GetAttachment(AttachmentName::HDRColor, newExtent).Extent(), newExtent);
	EXPECT_EQ(manager.GetAttachment(AttachmentName::SSAO, newExtent).Extent(), newExtent);
	EXPECT_EQ(manager.GetAttachment(AttachmentName::SSR, newExtent).Extent(), newExtent);
}

// ---------------------------------------------------------------------------
// Named access - all attachment names reachable
// ---------------------------------------------------------------------------

TEST_F(RenderCacheTest, GetAllAttachments_ByEnumAndString)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();

	auto depthFmtProps = pd.getFormatProperties(vk::Format::eD32Sfloat);
	if (!(depthFmtProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment))
	{
		GTEST_SKIP() << "D32_SFLOAT depth attachment not supported.";
	}

	const vk::Extent2D extent(1280, 720);
	RenderCache manager(*m_device, pd);
	manager.SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), *m_queue, m_graphicsQueueFamily));

	// Lazy creation via first GetAttachment call
	manager.GetAttachment(AttachmentName::Position, extent);

	// G-Buffer
	EXPECT_TRUE(*manager.GetAttachment(AttachmentName::Position, extent).ImageHandle());
	EXPECT_TRUE(*manager.GetAttachment(AttachmentName::Normal, extent).ImageHandle());
	EXPECT_TRUE(*manager.GetAttachment(AttachmentName::Albedo, extent).ImageHandle());
	EXPECT_TRUE(*manager.GetAttachment(AttachmentName::MetallicRoughness, extent).ImageHandle());
	EXPECT_TRUE(*manager.GetAttachment(AttachmentName::Depth, extent).ImageHandle());

	// Post-FX
	EXPECT_TRUE(*manager.GetAttachment(AttachmentName::HDRColor, extent).ImageHandle());
	EXPECT_TRUE(*manager.GetAttachment(AttachmentName::SSAO, extent).ImageHandle());
	EXPECT_TRUE(*manager.GetAttachment(AttachmentName::SSR, extent).ImageHandle());
}

// ---------------------------------------------------------------------------
// Non-copyable, movable
// ---------------------------------------------------------------------------

TEST_F(RenderCacheTest, NonCopyable)
{
	static_assert(!std::is_copy_constructible_v<RenderCache>,
	              "RenderCache must not be copy-constructible");
	static_assert(!std::is_copy_assignable_v<RenderCache>,
	              "RenderCache must not be copy-assignable");
	SUCCEED();
}

TEST_F(RenderCacheTest, Movable)
{
	static_assert(std::is_move_constructible_v<RenderCache>,
	              "RenderCache must be move-constructible");
	static_assert(std::is_move_assignable_v<RenderCache>,
	              "RenderCache must be move-assignable");
	SUCCEED();
}

// ---------------------------------------------------------------------------
// HasAttachment check
// ---------------------------------------------------------------------------

TEST_F(RenderCacheTest, HasAttachment_TrueForCreatedAttachments)
{
	if (!HasVulkan())
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	auto& pd = PhysicalDevice();

	auto depthFmtProps = pd.getFormatProperties(vk::Format::eD32Sfloat);
	if (!(depthFmtProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment))
	{
		GTEST_SKIP() << "D32_SFLOAT depth attachment not supported.";
	}

	const vk::Extent2D extent(800, 600);
	RenderCache manager(*m_device, pd);
	manager.SetLightingCache(
		std::make_unique<neurus::LightingCache>(*m_device, PhysicalDevice(), *m_queue, m_graphicsQueueFamily));

	// Lazy creation via GetAttachment calls
	manager.GetAttachment(AttachmentName::Position, extent);
	manager.GetAttachment(AttachmentName::Depth, extent);
	manager.GetAttachment(AttachmentName::HDRColor, extent);
	manager.GetAttachment(AttachmentName::SSAO, extent);

	EXPECT_TRUE(manager.HasAttachment(AttachmentName::Position));
	EXPECT_TRUE(manager.HasAttachment(AttachmentName::Depth));
	EXPECT_TRUE(manager.HasAttachment(AttachmentName::HDRColor));
	EXPECT_TRUE(manager.HasAttachment(AttachmentName::SSAO));
}
