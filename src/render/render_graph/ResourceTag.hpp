/**
 * @file ResourceTag.hpp
 * @brief Converts a GraphOutputTag's closed-set strings into Vulkan image config.
 *
 * The document stays Vulkan-free; this is the one renderer-side mapping from
 * "rgba16f", "color+sampled", "render" and "lights" to concrete enums.
 */

#pragma once

#include "../Image.hpp"

#include "asset/data/render_graph/RenderGraphSpec.hpp"

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace neurus {

/**
 * @brief "node/port", the identity of one planned image.
 */
std::string ResourceKey(std::string_view node, std::string_view port);

/// @brief vk::Format for a tag's format string.
/// @throws std::invalid_argument for an unknown format.
vk::Format FormatFromTag(const GraphOutputTag& tag);

/// @brief Image usage for a tag's usage string; empty for "logical".
/// @throws std::invalid_argument for an unknown usage.
vk::ImageUsageFlags UsageFromTag(const GraphOutputTag& tag);

/// @brief Image type: depth-stencil for a depth format, array for layers, else 2D.
Image::ImageType ImageTypeFromTag(const GraphOutputTag& tag);

/// @brief Array layer count: MAX_SHADOW_LAYERS for "lights", otherwise 1.
uint32_t LayerCountFromTag(const GraphOutputTag& tag);

/// @brief True when the tag describes an ordering token rather than an image.
bool IsLogicalTag(const GraphOutputTag& tag);

} // namespace neurus
