/**
 * @file GraphOutputTag.hpp
 * @brief The resource declaration carried by one graph output socket.
 *
 * A shader declares an image's GLSL type but not its format, usage, extent or
 * layer policy: a fragment output is just `vec4`, and a compute image may be
 * any storage format. Those four facts are user data on the output socket, so
 * they are a closed set of enums here rather than free-form strings. The
 * renderer maps them to Vulkan in `ResourceTag`.
 *
 * A @ref GraphUsage::Logical tag is an ordering token: the dependency must be
 * honoured but no image is allocated. Its format is @ref GraphFormat::None.
 */

#pragma once

#include <cstdint>

namespace neurus {

/// @brief Image format an output socket allocates.
enum class GraphFormat : std::uint8_t
{
	None,        ///< No image: only a logical tag.
	Rgba16f,
	Rgba8Srgb,
	Rgba8Unorm,
	R8Unorm,
	R32Uint,
	D32Sfloat,
};

/// @brief Vulkan usage family an output socket needs.
enum class GraphUsage : std::uint8_t
{
	Logical,          ///< Ordering token, no image.
	ColorSampled,     ///< Color attachment plus sampled.
	StorageSampled,   ///< Storage image plus sampled.
	DepthSampled,     ///< Depth attachment plus sampled.
};

/// @brief Which frame extent the output is allocated at.
enum class GraphExtent : std::uint8_t
{
	Render,
	Shadow,
};

/// @brief How many array layers the output image has.
enum class GraphLayers : std::uint8_t
{
	Single,
	Lights,   ///< One layer per shadow-casting light.
};

/**
 * @brief One output socket's resource declaration.
 */
struct GraphOutputTag
{
	GraphFormat format = GraphFormat::None;
	GraphUsage  usage  = GraphUsage::Logical;
	GraphExtent extent = GraphExtent::Render;
	GraphLayers layers = GraphLayers::Single;

	bool operator==(const GraphOutputTag&) const = default;
};

} // namespace neurus
