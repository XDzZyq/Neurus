#pragma once

#include <vulkan/vulkan_raii.hpp>

namespace neurus {

/**
 * @brief Identifies the type of pipeline: graphics (Geometry) or Compute.
 */
enum class PipelineType
{
	Geometry,
	Compute
};

/**
 * @brief Owned GPU pipeline and its associated pipeline layout.
 *
 * Carries both handles so the caller never manages the pipeline layout
 * separately from the pipeline.
 *
 * Non-copyable (vk::raii types are move-only), movable.
 */
struct Pipeline
{
	vk::raii::Pipeline       pipeline       = nullptr;
	vk::raii::PipelineLayout pipelineLayout = nullptr;
	PipelineType             type           = PipelineType::Compute;
};

} // namespace neurus
