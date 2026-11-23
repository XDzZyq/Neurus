/**
 * @file test_resource_tag.cpp
 * @brief Tag enums to Vulkan image configuration.
 */

#include <gtest/gtest.h>

#include "render/RenderCache.hpp"
#include "render/render_graph/ResourceTag.hpp"

#include <stdexcept>

namespace {

neurus::GraphOutputTag Tag(const neurus::GraphFormat format,
                           const neurus::GraphUsage usage,
                           const neurus::GraphExtent extent = neurus::GraphExtent::Render,
                           const neurus::GraphLayers layers = neurus::GraphLayers::Single)
{
	neurus::GraphOutputTag tag;
	tag.format = format;
	tag.usage  = usage;
	tag.extent = extent;
	tag.layers = layers;
	return tag;
}

} // anonymous namespace

TEST(ResourceTag, Key)
{
	EXPECT_EQ(neurus::ResourceKey("geometry", "gPosition"), "geometry/gPosition");
	EXPECT_EQ(neurus::ResourceKey("a", "b/c"), "a/b/c");
}

TEST(ResourceTag, Formats)
{
	EXPECT_EQ(neurus::FormatFromTag(Tag(neurus::GraphFormat::Rgba16f,
	                                    neurus::GraphUsage::ColorSampled)),
	          vk::Format::eR16G16B16A16Sfloat);
	EXPECT_EQ(neurus::FormatFromTag(Tag(neurus::GraphFormat::Rgba8Srgb,
	                                    neurus::GraphUsage::ColorSampled)),
	          vk::Format::eR8G8B8A8Srgb);
	EXPECT_EQ(neurus::FormatFromTag(Tag(neurus::GraphFormat::Rgba8Unorm,
	                                    neurus::GraphUsage::ColorSampled)),
	          vk::Format::eR8G8B8A8Unorm);
	EXPECT_EQ(neurus::FormatFromTag(Tag(neurus::GraphFormat::R8Unorm,
	                                    neurus::GraphUsage::StorageSampled)),
	          vk::Format::eR8Unorm);
	EXPECT_EQ(neurus::FormatFromTag(Tag(neurus::GraphFormat::R32Uint,
	                                    neurus::GraphUsage::ColorSampled)),
	          vk::Format::eR32Uint);
	EXPECT_EQ(neurus::FormatFromTag(Tag(neurus::GraphFormat::D32Sfloat,
	                                    neurus::GraphUsage::DepthSampled)),
	          vk::Format::eD32Sfloat);
}

TEST(ResourceTag, Usages)
{
	const vk::ImageUsageFlags color =
		vk::ImageUsageFlagBits::eColorAttachment
		| vk::ImageUsageFlagBits::eSampled
		| vk::ImageUsageFlagBits::eTransferSrc;
	const vk::ImageUsageFlags storage =
		vk::ImageUsageFlagBits::eStorage
		| vk::ImageUsageFlagBits::eSampled
		| vk::ImageUsageFlagBits::eTransferSrc
		| vk::ImageUsageFlagBits::eTransferDst;
	const vk::ImageUsageFlags depth =
		vk::ImageUsageFlagBits::eDepthStencilAttachment
		| vk::ImageUsageFlagBits::eSampled
		| vk::ImageUsageFlagBits::eTransferSrc;

	EXPECT_EQ(neurus::UsageFromTag(Tag(neurus::GraphFormat::Rgba16f,
	                                   neurus::GraphUsage::ColorSampled)), color);
	EXPECT_EQ(neurus::UsageFromTag(Tag(neurus::GraphFormat::Rgba16f,
	                                   neurus::GraphUsage::StorageSampled)), storage);
	EXPECT_EQ(neurus::UsageFromTag(Tag(neurus::GraphFormat::D32Sfloat,
	                                   neurus::GraphUsage::DepthSampled)), depth);
	EXPECT_EQ(neurus::UsageFromTag(Tag(neurus::GraphFormat::None,
	                                   neurus::GraphUsage::Logical)), vk::ImageUsageFlags{});
}

TEST(ResourceTag, ImageTypes)
{
	EXPECT_EQ(neurus::ImageTypeFromTag(Tag(neurus::GraphFormat::Rgba16f,
	                                       neurus::GraphUsage::ColorSampled)),
	          neurus::Image::ImageType::e2D);
	EXPECT_EQ(neurus::ImageTypeFromTag(Tag(neurus::GraphFormat::D32Sfloat,
	                                       neurus::GraphUsage::DepthSampled)),
	          neurus::Image::ImageType::eDepthStencil);
	EXPECT_EQ(neurus::ImageTypeFromTag(Tag(neurus::GraphFormat::R8Unorm,
	                                       neurus::GraphUsage::StorageSampled,
	                                       neurus::GraphExtent::Render,
	                                       neurus::GraphLayers::Lights)),
	          neurus::Image::ImageType::eArray);
}

TEST(ResourceTag, LayerCounts)
{
	EXPECT_EQ(neurus::LayerCountFromTag(Tag(neurus::GraphFormat::Rgba16f,
	                                        neurus::GraphUsage::ColorSampled)), 1u);
	EXPECT_EQ(neurus::LayerCountFromTag(Tag(neurus::GraphFormat::R8Unorm,
	                                        neurus::GraphUsage::StorageSampled,
	                                        neurus::GraphExtent::Render,
	                                        neurus::GraphLayers::Lights)),
	          neurus::RenderCache::MAX_SHADOW_LAYERS);
}

TEST(ResourceTag, Logical)
{
	EXPECT_TRUE(neurus::IsLogicalTag(Tag(neurus::GraphFormat::None,
	                                     neurus::GraphUsage::Logical)));
	EXPECT_FALSE(neurus::IsLogicalTag(Tag(neurus::GraphFormat::Rgba16f,
	                                      neurus::GraphUsage::ColorSampled)));
}

TEST(ResourceTag, LogicalFormatThrows)
{
	EXPECT_THROW((void)neurus::FormatFromTag(Tag(neurus::GraphFormat::None,
	                                             neurus::GraphUsage::Logical)),
	             std::invalid_argument);
}
