/**
 * @file ResourceTag.cpp
 * @brief Tag string to Vulkan image configuration.
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
	if (tag.format == "rgba16f")    return vk::Format::eR16G16B16A16Sfloat;
	if (tag.format == "rgba8_srgb") return vk::Format::eR8G8B8A8Srgb;
	if (tag.format == "rgba8_unorm") return vk::Format::eR8G8B8A8Unorm;
	if (tag.format == "r8_unorm")   return vk::Format::eR8Unorm;
	if (tag.format == "r32_uint")   return vk::Format::eR32Uint;
	if (tag.format == "d32_sfloat") return vk::Format::eD32Sfloat;

	throw std::invalid_argument(
		"ResourceTag: unknown output format '" + tag.format + "'");
}

vk::ImageUsageFlags UsageFromTag(const GraphOutputTag& tag)
{
	if (tag.usage == "color+sampled")
	{
		return vk::ImageUsageFlagBits::eColorAttachment
		     | vk::ImageUsageFlagBits::eSampled
		     | vk::ImageUsageFlagBits::eTransferSrc;
	}
	if (tag.usage == "storage+sampled")
	{
		return vk::ImageUsageFlagBits::eStorage
		     | vk::ImageUsageFlagBits::eSampled
		     | vk::ImageUsageFlagBits::eTransferSrc;
	}
	if (tag.usage == "depth+sampled")
	{
		return vk::ImageUsageFlagBits::eDepthStencilAttachment
		     | vk::ImageUsageFlagBits::eSampled
		     | vk::ImageUsageFlagBits::eTransferSrc;
	}
	if (tag.usage == "logical")
		return {};

	throw std::invalid_argument(
		"ResourceTag: unknown output usage '" + tag.usage + "'");
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
	if (tag.layers == "lights")
		return RenderCache::MAX_SHADOW_LAYERS;
	if (tag.layers == "single")
		return 1;

	throw std::invalid_argument(
		"ResourceTag: unknown layer policy '" + tag.layers + "'");
}

bool IsLogicalTag(const GraphOutputTag& tag)
{
	return tag.usage == "logical";
}

} // namespace neurus
