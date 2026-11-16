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
#include "ShaderReflection.hpp"

#include <array>
#include <initializer_list>
#include <span>

namespace neurus {

/**
 * @brief Reflects a descriptor set layout from one shader bundle.
 * @param device            Logical device.
 * @param shader            Shader whose stages declare the bindings.
 * @param optionalBindings  Bindings that get PARTIALLY_BOUND.
 */
inline DescriptorSetLayout MakeDescriptorLayout(
	const vk::raii::Device& device,
	const ShaderData& shader,
	const std::initializer_list<uint32_t> optionalBindings = {})
{
	const std::array<const ShaderData*, 1> shaders = {&shader};
	const ReflectedDescriptorLayout layout =
		ReflectDescriptorLayout(shaders, std::span<const uint32_t>(optionalBindings));

	return DescriptorSetLayout(device, layout.bindings, layout.bindingFlags);
}

/**
 * @brief Reflects a push-constant range from one shader bundle.
 * @param shader  Shader whose stages declare the members.
 * @param stages  Stage mask for the range.
 */
inline vk::PushConstantRange MakePushConstantRange(
	const ShaderData& shader,
	const vk::ShaderStageFlags stages)
{
	const std::array<const ShaderData*, 1> shaders = {&shader};
	return PushConstantRangeFromShaders(shaders, stages);
}

} // namespace neurus
