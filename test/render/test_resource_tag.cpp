/**
 * @file test_resource_tag.cpp
 * @brief Tag string to Vulkan image configuration.
 */

#include <gtest/gtest.h>

#include "render/RenderCache.hpp"
#include "render/render_graph/ResourceTag.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace {

neurus::GraphOutputTag Tag(std::string format, std::string usage,
                           std::string extent = "render",
                           std::string layers = "single")
{
	neurus::GraphOutputTag tag;
	tag.format = std::move(format);
	tag.usage  = std::move(usage);
	tag.extent = std::move(extent);
	tag.layers = std::move(layers);
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
	EXPECT_EQ(neurus::FormatFromTag(Tag("rgba16f", "color+sampled")),
	          vk::Format::eR16G16B16A16Sfloat);
	EXPECT_EQ(neurus::FormatFromTag(Tag("rgba8_srgb", "color+sampled")),
	          vk::Format::eR8G8B8A8Srgb);
	EXPECT_EQ(neurus::FormatFromTag(Tag("rgba8_unorm", "color+sampled")),
	          vk::Format::eR8G8B8A8Unorm);
	EXPECT_EQ(neurus::FormatFromTag(Tag("r8_unorm", "storage+sampled")),
	          vk::Format::eR8Unorm);
	EXPECT_EQ(neurus::FormatFromTag(Tag("r32_uint", "color+sampled")),
	          vk::Format::eR32Uint);
	EXPECT_EQ(neurus::FormatFromTag(Tag("d32_sfloat", "depth+sampled")),
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
		| vk::ImageUsageFlagBits::eTransferSrc;
	const vk::ImageUsageFlags depth =
		vk::ImageUsageFlagBits::eDepthStencilAttachment
		| vk::ImageUsageFlagBits::eSampled
		| vk::ImageUsageFlagBits::eTransferSrc;

	EXPECT_EQ(neurus::UsageFromTag(Tag("rgba16f", "color+sampled")), color);
	EXPECT_EQ(neurus::UsageFromTag(Tag("rgba16f", "storage+sampled")), storage);
	EXPECT_EQ(neurus::UsageFromTag(Tag("d32_sfloat", "depth+sampled")), depth);
	EXPECT_EQ(neurus::UsageFromTag(Tag("", "logical")), vk::ImageUsageFlags{});
}

TEST(ResourceTag, ImageTypes)
{
	EXPECT_EQ(neurus::ImageTypeFromTag(Tag("rgba16f", "color+sampled")),
	          neurus::Image::ImageType::e2D);
	EXPECT_EQ(neurus::ImageTypeFromTag(Tag("d32_sfloat", "depth+sampled")),
	          neurus::Image::ImageType::eDepthStencil);
	EXPECT_EQ(neurus::ImageTypeFromTag(Tag("r8_unorm", "storage+sampled", "render", "lights")),
	          neurus::Image::ImageType::eArray);
}

TEST(ResourceTag, LayerCounts)
{
	EXPECT_EQ(neurus::LayerCountFromTag(Tag("rgba16f", "color+sampled")), 1u);
	EXPECT_EQ(neurus::LayerCountFromTag(Tag("r8_unorm", "storage+sampled", "render", "lights")),
	          neurus::RenderCache::MAX_SHADOW_LAYERS);
}

TEST(ResourceTag, Logical)
{
	EXPECT_TRUE(neurus::IsLogicalTag(Tag("", "logical")));
	EXPECT_FALSE(neurus::IsLogicalTag(Tag("rgba16f", "color+sampled")));
}

TEST(ResourceTag, UnknownFormatThrows)
{
	EXPECT_THROW((void)neurus::FormatFromTag(Tag("nope", "color+sampled")),
	             std::invalid_argument);
}

TEST(ResourceTag, UnknownUsageThrows)
{
	EXPECT_THROW((void)neurus::UsageFromTag(Tag("rgba16f", "nope")),
	             std::invalid_argument);
}

TEST(ResourceTag, UnknownLayersThrows)
{
	EXPECT_THROW((void)neurus::LayerCountFromTag(
	                 Tag("rgba16f", "color+sampled", "render", "nope")),
	             std::invalid_argument);
}
