/**
 * @file DescriptorLayout.hpp
 * @brief Builds a pass's descriptor set layout from the shader IR.
 *
 * A pass keeps its descriptor sets and binding calls; only the layout's
 * declarations move to reflection, so a shader edit cannot drift from a
 * hand-written binding list.
 */

#pragma once

#include "DescriptorManager.hpp"
#include "render_graph/ShaderReflection.hpp"

#include <array>
#include <span>
#include <memory>
#include <stdexcept>

namespace neurus {

/** @brief Requires an immutable shader before constructor layout evaluation. */
template<typename Shader>
inline const Shader& RequireShader(const std::shared_ptr<const Shader>& shader)
{
	if (!shader)
		throw std::invalid_argument("Pass construction requires a shader");
	return *shader;
}

/**
 * @brief Reflects a descriptor set layout from one shader bundle.
 * @param device            Logical device.
 * @param shader            Shader whose stages declare the bindings.
 * @param optionalBindings  Bindings that get PARTIALLY_BOUND.
 */
template<typename Shader>
inline DescriptorSetLayout MakeDescriptorLayout(
	const vk::raii::Device& device,
	const Shader& shader,
	const std::span<const uint32_t> optionalBindings = {})
{
	const std::array<const Shader*, 1> shaders = {&shader};
	const ReflectedDescriptorLayout layout =
		ReflectDescriptorLayout(shaders, optionalBindings);

	return DescriptorSetLayout(device, layout.bindings, layout.bindingFlags);
}

/**
 * @brief Reflects a push-constant range from one shader bundle.
 * @param shader  Shader whose stages declare the members.
 * @param stages  Stage mask for the range.
 */
template<typename Shader>
inline vk::PushConstantRange MakePushConstantRange(
	const Shader& shader,
	const vk::ShaderStageFlags stages)
{
	const std::array<const Shader*, 1> shaders = {&shader};
	vk::PushConstantRange range = PushConstantRangeFromShaders(shaders, stages);

	// The IR size is the last declared member's end. Vulkan push-constant
	// ranges and the C++ blocks that feed them are 16-byte aligned
	// (MeshPushConstants is alignas(16)), so round the range up. Pushing a
	// block smaller than the rounded range is valid; pushing a block larger
	// than the range is VUID-vkCmdPushConstants-offset-01795.
	range.size = (range.size + 15u) & ~15u;
	return range;
}

} // namespace neurus
