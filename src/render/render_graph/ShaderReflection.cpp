/**
 * @file ShaderReflection.cpp
 * @brief Descriptor layout and push-constant reflection from ShaderStruct.
 */

#include "render/render_graph/ShaderReflection.hpp"

#include "asset/data/ShaderData.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

namespace neurus {

namespace {

/// @brief The Vulkan stage flag for one shader stage.
vk::ShaderStageFlags StageFlags(const ShaderType type)
{
	switch (type)
	{
	case ShaderType::VERTEX:   return vk::ShaderStageFlagBits::eVertex;
	case ShaderType::FRAGMENT: return vk::ShaderStageFlagBits::eFragment;
	case ShaderType::COMPUTE:  return vk::ShaderStageFlagBits::eCompute;
	case ShaderType::GEOMETRY: return vk::ShaderStageFlagBits::eGeometry;
	}
	return {};
}

/// @brief The GLSL type string of a uniform declaration.
std::string UniformType(const S_Uniform& uniform)
{
	if (!uniform.actualType.empty())
		return uniform.actualType;
	return ShaderStruct::ParseType(uniform.type);
}

/// @brief The descriptor type of a sampler or image uniform, or nullopt when it is neither.
std::optional<vk::DescriptorType> DescriptorTypeOf(const S_Uniform& uniform)
{
	const std::string type = UniformType(uniform);
	if (type.starts_with("sampler"))
		return vk::DescriptorType::eCombinedImageSampler;
	if (type.starts_with("image"))
		return vk::DescriptorType::eStorageImage;
	return std::nullopt;
}

} // anonymous namespace

ReflectedDescriptorLayout ReflectDescriptorLayout(
	const std::span<const ShaderData* const> shaders,
	const std::span<const uint32_t> optionalBindings)
{
	// Sorted by binding, so the returned vectors are deterministic.
	std::map<uint32_t, vk::DescriptorSetLayoutBinding> byBinding;

	auto add = [&byBinding](const uint32_t binding,
	                        const vk::DescriptorType type,
	                        const vk::ShaderStageFlags stageFlags,
	                        const uint32_t count)
	{
		const auto it = byBinding.find(binding);
		if (it == byBinding.end())
		{
			byBinding.emplace(binding,
			                  vk::DescriptorSetLayoutBinding(binding, type, count, stageFlags));
			return;
		}

		if (it->second.descriptorType != type || it->second.descriptorCount != count)
		{
			throw std::runtime_error(
				"ShaderReflection: binding " + std::to_string(binding)
				+ " is declared with conflicting descriptor types or counts");
		}

		it->second.stageFlags |= stageFlags;
	};

	constexpr std::array<ShaderType, 4> kStages = {
		ShaderType::VERTEX,
		ShaderType::FRAGMENT,
		ShaderType::COMPUTE,
		ShaderType::GEOMETRY,
	};

	for (const ShaderData* shader : shaders)
	{
		if (!shader)
			continue;

		for (const ShaderType stage : kStages)
		{
			if (!shader->HasStage(stage))
				continue;

			const ShaderUnit* unit = shader->FindStage(stage);
			if (!unit)
				continue;

			const ShaderStruct& ir = unit->parsed;
			const vk::ShaderStageFlags stageFlags = StageFlags(stage);

			for (const S_Uniform& uniform : ir.uniform_list)
			{
				if (uniform.binding < 0)
					continue;

				const std::optional<vk::DescriptorType> type = DescriptorTypeOf(uniform);
				if (!type)
					continue;

				add(static_cast<uint32_t>(uniform.binding), *type, stageFlags,
				    static_cast<uint32_t>(std::max(uniform.count, 1)));
			}

			for (const S_StructDef& block : ir.ubuffer_list)
			{
				if (block.binding < 0)
					continue;
				add(static_cast<uint32_t>(block.binding),
				    vk::DescriptorType::eUniformBuffer, stageFlags, 1);
			}

			for (const S_StructDef& block : ir.SB_list)
			{
				if (block.binding < 0)
					continue;
				add(static_cast<uint32_t>(block.binding),
				    vk::DescriptorType::eStorageBuffer, stageFlags, 1);
			}
		}
	}

	const std::set<uint32_t> optional(optionalBindings.begin(), optionalBindings.end());

	ReflectedDescriptorLayout layout;
	layout.bindings.reserve(byBinding.size());
	layout.bindingFlags.reserve(byBinding.size());

	for (const auto& [binding, descriptor] : byBinding)
	{
		layout.bindings.push_back(descriptor);
		layout.bindingFlags.push_back(
			optional.contains(binding)
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

		for (const ShaderType stage : {ShaderType::VERTEX, ShaderType::FRAGMENT,
		                               ShaderType::COMPUTE, ShaderType::GEOMETRY})
		{
			if (!shader->HasStage(stage))
				continue;

			const ShaderUnit* unit = shader->FindStage(stage);
			if (!unit)
				continue;

			for (const S_PushConstant& member : unit->parsed.push_constants)
				size = std::max(size, member.offset + member.size);
		}
	}

	if (size == 0)
		return vk::PushConstantRange{};

	return vk::PushConstantRange(stages, 0, size);
}

} // namespace neurus
