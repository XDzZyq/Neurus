#pragma once

#include "../Image.hpp"

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <memory>

namespace neurus {

// Forward declarations
enum LightType : int;    // defined in scene/Light.hpp

/**
 * @brief Per-light GPU shadow resources owned by the Renderer layer.
 *
 * Holds shadow depth map, optional colour cubemap for multiview, and layer
 * index into the shared shadow intensity array.
 *
 * Non-copyable (GPU resource handles are move-only).
 */
struct LightGPU
{
	std::unique_ptr<Image> shadowDepthMap;
	std::unique_ptr<Image> shadowColorMap;
	uint32_t intensityLayer = 0;
};

} // namespace neurus
