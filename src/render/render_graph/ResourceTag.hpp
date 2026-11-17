/**
 * @file ResourceTag.hpp
 * @brief Converts a GraphOutputTag into Vulkan image configuration.
 *
 * The document stays Vulkan-free; this is the one renderer-side mapping from
 * the tag's format, usage, extent and layer enums to concrete Vulkan values.
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

/// @brief vk::Format for a tag's format.
/// @throws std::invalid_argument for a logical tag, which names no image.
vk::Format FormatFromTag(const GraphOutputTag& tag);

/// @brief Image usage for a tag's usage; empty for a logical tag.
vk::ImageUsageFlags UsageFromTag(const GraphOutputTag& tag);

/// @brief Image type: depth-stencil for a depth format, array for layers, else 2D.
Image::ImageType ImageTypeFromTag(const GraphOutputTag& tag);

/// @brief Array layer count: MAX_SHADOW_LAYERS for a light array, otherwise 1.
uint32_t LayerCountFromTag(const GraphOutputTag& tag);

/// @brief True when the tag describes an ordering token rather than an image.
bool IsLogicalTag(const GraphOutputTag& tag);

} // namespace neurus
