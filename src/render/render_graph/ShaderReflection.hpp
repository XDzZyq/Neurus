/**
 * @file ShaderReflection.hpp
 * @brief Vulkan descriptor layouts and push-constant ranges from ShaderStruct.
 *
 * A pass no longer hand-writes its set layout: binding number, descriptor type,
 * stage flags, array count and optional (PARTIALLY_BOUND) status all come from
 * the parsed shader IR that the pass already owns.
 */

#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace neurus {

class ShaderData;
class ShaderGPU;

/**
 * @brief A reflected descriptor set layout: bindings and aligned per-binding flags.
 */
struct ReflectedDescriptorLayout
{
	std::vector<vk::DescriptorSetLayoutBinding> bindings;
	std::vector<vk::DescriptorBindingFlags>     bindingFlags;  ///< same order as bindings
};

/**
 * @brief Reflects one descriptor set layout from every stage of every shader.
 *
 * Bindings declared by several stages merge their stage flags. A binding
 * declared twice with a different descriptor type or count is a shader bug and
 * throws, naming the binding.
 *
 * @param shaders          Shaders owned by the pass; null entries are skipped.
 * @param optionalBindings Bindings that get PARTIALLY_BOUND (the light SSBOs).
 */
ReflectedDescriptorLayout ReflectDescriptorLayout(
	std::span<const ShaderData* const> shaders,
	std::span<const uint32_t> optionalBindings = {});

/**
 * @brief The push-constant range covering every declared member, across stages.
 *
 * The size is the maximum of `offset + size` over every member. @p stages is the
 * stage mask. Returns a zero-size range when no stage declares a member.
 */
vk::PushConstantRange PushConstantRangeFromShaders(
	std::span<const ShaderData* const> shaders,
	vk::ShaderStageFlags stages);

/** @brief Builds Vulkan descriptor declarations from uploaded shader metadata. */
ReflectedDescriptorLayout ReflectDescriptorLayout(std::span<const ShaderGPU* const> shaders,
                                                   std::span<const uint32_t> optionalBindings = {});
/** @brief Covers the push-constant members retained by uploaded shader resources. */
vk::PushConstantRange PushConstantRangeFromShaders(std::span<const ShaderGPU* const> shaders,
                                                  vk::ShaderStageFlags stages);
} // namespace neurus
