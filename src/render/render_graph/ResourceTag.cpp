/**
 * @file ResourceTag.cpp
 * @brief Tag enums to Vulkan image configuration.
 */

#include "render/render_graph/ResourceTag.hpp"

#include "render/RenderCache.hpp"

#include <stdexcept>
#include <string>

namespace neurus {

std::string ResourceKey(const std::string_view node, const std::string_view port)
{
	return std::string(node) + "/" + std::string(port);
}

vk::Format FormatFromTag(const GraphOutputTag& tag)
{
	switch (tag.format)
	{
	case GraphFormat::Rgba16f:    return vk::Format::eR16G16B16A16Sfloat;
	case GraphFormat::Rgba8Srgb:  return vk::Format::eR8G8B8A8Srgb;
	case GraphFormat::Rgba8Unorm: return vk::Format::eR8G8B8A8Unorm;
	case GraphFormat::R8Unorm:    return vk::Format::eR8Unorm;
	case GraphFormat::R32Uint:    return vk::Format::eR32Uint;
	case GraphFormat::D32Sfloat:  return vk::Format::eD32Sfloat;
	case GraphFormat::None:
		break;
	}

	throw std::invalid_argument(
		"ResourceTag: a logical output tag declares no image format");
}

vk::ImageUsageFlags UsageFromTag(const GraphOutputTag& tag)
{
	switch (tag.usage)
	{
	case GraphUsage::ColorSampled:
		return vk::ImageUsageFlagBits::eColorAttachment
		     | vk::ImageUsageFlagBits::eSampled
		     | vk::ImageUsageFlagBits::eTransferSrc;
	case GraphUsage::StorageSampled:
		return vk::ImageUsageFlagBits::eStorage
		     | vk::ImageUsageFlagBits::eSampled
		     | vk::ImageUsageFlagBits::eTransferSrc;
	case GraphUsage::DepthSampled:
		return vk::ImageUsageFlagBits::eDepthStencilAttachment
		     | vk::ImageUsageFlagBits::eSampled
		     | vk::ImageUsageFlagBits::eTransferSrc;
	case GraphUsage::Logical:
		return {};
	}

	throw std::invalid_argument("ResourceTag: unknown output usage");
}

Image::ImageType ImageTypeFromTag(const GraphOutputTag& tag)
{
	if (FormatFromTag(tag) == vk::Format::eD32Sfloat)
		return Image::ImageType::eDepthStencil;
	if (LayerCountFromTag(tag) > 1)
		return Image::ImageType::eArray;
	return Image::ImageType::e2D;
}

uint32_t LayerCountFromTag(const GraphOutputTag& tag)
{
	switch (tag.layers)
	{
	case GraphLayers::Lights:
		return RenderCache::MAX_SHADOW_LAYERS;
	case GraphLayers::Single:
		return 1;
	}

	throw std::invalid_argument("ResourceTag: unknown layer policy");
}

bool IsLogicalTag(const GraphOutputTag& tag)
{
	return tag.usage == GraphUsage::Logical;
}

} // namespace neurus
