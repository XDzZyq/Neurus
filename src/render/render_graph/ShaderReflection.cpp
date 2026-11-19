/**
 * @file ShaderReflection.cpp
 * @brief Converts the CPU shader interface into Vulkan layout declarations.
 */

#include "render/render_graph/ShaderReflection.hpp"
#include "shader/ShaderSnapshotBuilder.hpp"

#include "asset/data/ShaderData.hpp"
#include "asset/data/shader/ShaderSnapshot.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <string>

namespace neurus {

namespace {

/// @brief Converts the snapshot's stage mask to Vulkan shader stage flags.
vk::ShaderStageFlags StageFlags(const uint32_t mask)
{
	vk::ShaderStageFlags flags;
	constexpr std::array<ShaderType, 4> stages = {
		ShaderType::VERTEX, ShaderType::FRAGMENT,
		ShaderType::COMPUTE, ShaderType::GEOMETRY,
	};
	for (const ShaderType stage : stages)
	{
		if (!(mask & (1u << static_cast<uint32_t>(stage))))
			continue;
		switch (stage)
		{
		case ShaderType::VERTEX:   flags |= vk::ShaderStageFlagBits::eVertex; break;
		case ShaderType::FRAGMENT: flags |= vk::ShaderStageFlagBits::eFragment; break;
		case ShaderType::COMPUTE:  flags |= vk::ShaderStageFlagBits::eCompute; break;
		case ShaderType::GEOMETRY: flags |= vk::ShaderStageFlagBits::eGeometry; break;
		}
	}
	return flags;
}

/// @brief Converts the CPU descriptor kind without inspecting live shader IR.
vk::DescriptorType DescriptorType(const DescriptorKind kind)
{
	switch (kind)
	{
	case DescriptorKind::CombinedImageSampler: return vk::DescriptorType::eCombinedImageSampler;
	case DescriptorKind::StorageImage:         return vk::DescriptorType::eStorageImage;
	case DescriptorKind::UniformBuffer:        return vk::DescriptorType::eUniformBuffer;
	case DescriptorKind::StorageBuffer:        return vk::DescriptorType::eStorageBuffer;
	}
	throw std::invalid_argument("ShaderReflection: unknown descriptor kind");
}

} // anonymous namespace

ReflectedDescriptorLayout ReflectDescriptorLayout(
	const std::span<const ShaderData* const> shaders,
	const std::span<const uint32_t> optionalBindings)
{
	// Reflection and validation belong to the CPU snapshot builder. This bridge
	// only translates descriptors and merges the interfaces of shader variants.
	std::map<uint32_t, DescriptorBinding> byBinding;
	for (const ShaderData* shader : shaders)
	{
		if (!shader)
			continue;

		const ShaderSnapshot snapshot = MakeShaderSnapshot(*shader, optionalBindings);
		for (const DescriptorBinding& binding : snapshot.bindings)
		{
			const auto [it, inserted] = byBinding.emplace(binding.binding, binding);
			if (inserted)
				continue;
			if (it->second.kind != binding.kind || it->second.count != binding.count)
			{
				throw std::runtime_error(
					"ShaderReflection: binding " + std::to_string(binding.binding)
					+ " is declared with conflicting descriptor types or counts");
			}
			it->second.stageMask |= binding.stageMask;
		}
	}

	ReflectedDescriptorLayout layout;
	layout.bindings.reserve(byBinding.size());
	layout.bindingFlags.reserve(byBinding.size());
	for (const auto& [index, binding] : byBinding)
	{
		layout.bindings.emplace_back(index, DescriptorType(binding.kind),
		                            binding.count, StageFlags(binding.stageMask));
		layout.bindingFlags.push_back(
			binding.partiallyBound
				? vk::DescriptorBindingFlags(vk::DescriptorBindingFlagBits::ePartiallyBound)
				: vk::DescriptorBindingFlags{});
	}
	return layout;
}

std::vector<vk::DescriptorSetLayoutBinding> DescriptorBindingsFromShaders(
	const std::span<const ShaderData* const> shaders,
	const std::span<const uint32_t> optionalBindings)
{
	return ReflectDescriptorLayout(shaders, optionalBindings).bindings;
}

vk::PushConstantRange PushConstantRangeFromShaders(
	const std::span<const ShaderData* const> shaders,
	const vk::ShaderStageFlags stages)
{
	uint32_t size = 0;
	for (const ShaderData* shader : shaders)
	{
		if (!shader)
			continue;
		const ShaderSnapshot snapshot = MakeShaderSnapshot(*shader);
		for (const PushConstantMember& member : snapshot.pushConstants)
			size = std::max(size, member.offset + member.size);
	}
	return size == 0 ? vk::PushConstantRange{} : vk::PushConstantRange(stages, 0, size);
}

} // namespace neurus
