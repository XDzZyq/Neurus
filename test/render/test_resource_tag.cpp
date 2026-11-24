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

TEST(ResourceTag, PhysicalImageConfiguration)
{
	using namespace neurus;
	using ImageType = Image::ImageType;
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

	struct ImageCase
	{
		const char* name;
		GraphOutputTag tag;
		vk::Format format;
		vk::ImageUsageFlags usage;
		ImageType type;
		uint32_t layers;
	};
	const ImageCase cases[] = {
		{"hdr", Tag(GraphFormat::Rgba16f, GraphUsage::ColorSampled),
		 vk::Format::eR16G16B16A16Sfloat, color, ImageType::e2D, 1},
		{"sRGB", Tag(GraphFormat::Rgba8Srgb, GraphUsage::ColorSampled),
		 vk::Format::eR8G8B8A8Srgb, color, ImageType::e2D, 1},
		{"unorm", Tag(GraphFormat::Rgba8Unorm, GraphUsage::ColorSampled),
		 vk::Format::eR8G8B8A8Unorm, color, ImageType::e2D, 1},
		{"ID", Tag(GraphFormat::R32Uint, GraphUsage::ColorSampled),
		 vk::Format::eR32Uint, color, ImageType::e2D, 1},
		{"shadow array", Tag(GraphFormat::R8Unorm, GraphUsage::StorageSampled,
		                     GraphExtent::Render, GraphLayers::Lights),
		 vk::Format::eR8Unorm, storage, ImageType::eArray, RenderCache::MAX_SHADOW_LAYERS},
		{"depth", Tag(GraphFormat::D32Sfloat, GraphUsage::DepthSampled),
		 vk::Format::eD32Sfloat, depth, ImageType::eDepthStencil, 1},
	};
	for (const auto& entry : cases)
	{
		SCOPED_TRACE(entry.name);
		EXPECT_FALSE(IsLogicalTag(entry.tag));
		EXPECT_EQ(FormatFromTag(entry.tag), entry.format);
		EXPECT_EQ(UsageFromTag(entry.tag), entry.usage);
		EXPECT_EQ(ImageTypeFromTag(entry.tag), entry.type);
		EXPECT_EQ(LayerCountFromTag(entry.tag), entry.layers);
	}
}

TEST(ResourceTag, LogicalHasNoImageConfiguration)
{
	const auto tag = Tag(neurus::GraphFormat::None, neurus::GraphUsage::Logical);
	EXPECT_TRUE(neurus::IsLogicalTag(tag));
	EXPECT_EQ(neurus::UsageFromTag(tag), vk::ImageUsageFlags{});
	EXPECT_THROW((void)neurus::FormatFromTag(tag), std::invalid_argument);
}
