/**
 * @file test_render_cache_resources.cpp
 * @brief Tag-planned cache resources and the legacy attachment bridge.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.hpp"

#include "render/Image.hpp"
#include "render/RenderCache.hpp"

#include <stdexcept>
#include <string>
#include <utility>

using namespace neurus;

namespace {

GraphOutputTag Tag(const GraphFormat format, const GraphUsage usage,
                   const GraphExtent extent = GraphExtent::Render,
                   const GraphLayers layers = GraphLayers::Single)
{
	GraphOutputTag tag;
	tag.format = format;
	tag.usage  = usage;
	tag.extent = extent;
	tag.layers = layers;
	return tag;
}

} // anonymous namespace

class RenderCacheResourcesTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		VulkanTestShared::SetUp();
		if (!HasVulkan())
			return;
		m_cache = std::make_unique<RenderCache>(*m_device, PhysicalDevice());
	}

	void TearDown() override
	{
		m_cache.reset();
		VulkanTestShared::TearDown();
	}

	std::unique_ptr<RenderCache> m_cache;
};

TEST_F(RenderCacheResourcesTest, PlanAndCreate)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	const vk::Extent2D extent(320, 200);
	m_cache->PlanResource("geometry/gPosition", Tag(GraphFormat::Rgba16f, GraphUsage::ColorSampled));

	EXPECT_TRUE(m_cache->HasResource("geometry/gPosition"));
	EXPECT_EQ(m_cache->FindPlannedResource("geometry/gPosition"), nullptr);

	Image& image = m_cache->GetResource("geometry/gPosition", extent);
	EXPECT_EQ(image.Extent(), extent);
	EXPECT_EQ(image.Format(), vk::Format::eR16G16B16A16Sfloat);
	EXPECT_EQ(image.ArrayLayers(), 1u);
	EXPECT_EQ(m_cache->FindPlannedResource("geometry/gPosition"), &image);
	EXPECT_NE(m_cache->GetResourceTag("geometry/gPosition"), nullptr);
}

TEST_F(RenderCacheResourcesTest, LayersCreateArray)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	const vk::Extent2D extent(320, 200);
	m_cache->PlanResource("shadow_intensity/outputShadow",
	                      Tag(GraphFormat::R8Unorm, GraphUsage::StorageSampled,
	                          GraphExtent::Render, GraphLayers::Lights));

	Image& image = m_cache->GetResource("shadow_intensity/outputShadow", extent);
	EXPECT_EQ(image.Format(), vk::Format::eR8Unorm);
	EXPECT_EQ(image.Type(), Image::ImageType::eArray);
	EXPECT_EQ(image.ArrayLayers(), RenderCache::MAX_SHADOW_LAYERS);
}

TEST_F(RenderCacheResourcesTest, DepthCreatesDepthStencil)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	const vk::Extent2D extent(320, 200);
	m_cache->PlanResource("geometry/depth", Tag(GraphFormat::D32Sfloat, GraphUsage::DepthSampled));

	Image& image = m_cache->GetResource("geometry/depth", extent);
	EXPECT_EQ(image.Format(), vk::Format::eD32Sfloat);
	EXPECT_EQ(image.Type(), Image::ImageType::eDepthStencil);
}

TEST_F(RenderCacheResourcesTest, LogicalIsNotAllocated)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	const vk::Extent2D extent(320, 200);
	m_cache->PlanResource("shadow_depth/shadow_depth", Tag(GraphFormat::None, GraphUsage::Logical));

	EXPECT_TRUE(m_cache->HasResource("shadow_depth/shadow_depth"));
	EXPECT_EQ(m_cache->FindPlannedResource("shadow_depth/shadow_depth"), nullptr);
	EXPECT_THROW((void)m_cache->GetResource("shadow_depth/shadow_depth", extent),
	             std::invalid_argument);
}

TEST_F(RenderCacheResourcesTest, UnplannedThrows)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	EXPECT_FALSE(m_cache->HasResource("nope/nope"));
	EXPECT_THROW((void)m_cache->GetResource("nope/nope", vk::Extent2D(16, 16)),
	             std::invalid_argument);
}

TEST_F(RenderCacheResourcesTest, CleanScreenSpaceRecreates)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	m_cache->PlanResource("ssao/outputSSAO", Tag(GraphFormat::R8Unorm, GraphUsage::StorageSampled));
	m_cache->GetResource("ssao/outputSSAO", vk::Extent2D(320, 200));

	m_cache->CleanScreenSpace();
	EXPECT_EQ(m_cache->FindPlannedResource("ssao/outputSSAO"), nullptr);
	EXPECT_TRUE(m_cache->HasResource("ssao/outputSSAO"));

	Image& recreated = m_cache->GetResource("ssao/outputSSAO", vk::Extent2D(640, 400));
	EXPECT_EQ(recreated.Extent(), vk::Extent2D(640, 400));
}

TEST_F(RenderCacheResourcesTest, LegacyAliasSharesTheImage)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	const vk::Extent2D extent(320, 200);
	m_cache->PlanResource("compose/outputComposed", Tag(GraphFormat::Rgba16f, GraphUsage::StorageSampled));
	m_cache->AliasResourceToAttachment(AttachmentName::ComposedOutput,
	                                   "compose/outputComposed");

	Image& resource = m_cache->GetResource("compose/outputComposed", extent);
	Image& attachment = m_cache->GetAttachment(AttachmentName::ComposedOutput, extent);

	EXPECT_EQ(&resource, &attachment);
	EXPECT_EQ(resource.Format(), vk::Format::eR16G16B16A16Sfloat);
	EXPECT_EQ(m_cache->FindPlannedResource("compose/outputComposed"), &resource);
}

TEST_F(RenderCacheResourcesTest, TagConfigWinsOverConfigFor)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	const vk::Extent2D extent(320, 200);
	m_cache->PlanResource("fxaa/outputFXAA", Tag(GraphFormat::R8Unorm, GraphUsage::StorageSampled));
	m_cache->AliasResourceToAttachment(AttachmentName::FXAAOutput, "fxaa/outputFXAA");

	Image& attachment = m_cache->GetAttachment(AttachmentName::FXAAOutput, extent);
	EXPECT_EQ(attachment.Format(), vk::Format::eR8Unorm);
}

TEST_F(RenderCacheResourcesTest, ShadowIntensityDelegates)
{
	if (!HasVulkan())
		GTEST_SKIP() << "No Vulkan-capable GPU found.";

	const vk::Extent2D extent(320, 200);
	m_cache->PlanResource("shadow_intensity/outputShadow",
	                      Tag(GraphFormat::R8Unorm, GraphUsage::StorageSampled,
	                          GraphExtent::Render, GraphLayers::Lights));
	m_cache->AliasResourceToAttachment(AttachmentName::ShadowIntensity,
	                                   "shadow_intensity/outputShadow");

	Image& array = m_cache->GetShadowIntensityArray(extent);
	Image& resource = m_cache->GetResource("shadow_intensity/outputShadow", extent);

	EXPECT_EQ(&array, &resource);
	EXPECT_EQ(array.ArrayLayers(), RenderCache::MAX_SHADOW_LAYERS);
}
