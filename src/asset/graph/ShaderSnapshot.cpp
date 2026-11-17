/**
 * @file ShaderSnapshot.cpp
 * @brief Flattens a pooled shader into the layer-boundary snapshot.
 */

#include "asset/graph/ShaderSnapshot.hpp"

#include "asset/data/ShaderData.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>

namespace neurus {

namespace {

constexpr std::array<ShaderType, 4> kStages = {
	ShaderType::VERTEX,
	ShaderType::FRAGMENT,
	ShaderType::COMPUTE,
	ShaderType::GEOMETRY,
};

uint32_t StageMask(const ShaderType type)
{
	return 1u << static_cast<uint32_t>(type);
}

std::string UniformType(const S_Uniform& uniform)
{
	if (!uniform.actualType.empty())
		return uniform.actualType;
	return ShaderStruct::ParseType(uniform.type);
}

std::optional<DescriptorKind> UniformDescriptorKind(const S_Uniform& uniform)
{
	const std::string type = UniformType(uniform);
	if (type.starts_with("sampler"))
		return DescriptorKind::CombinedImageSampler;
	if (type.starts_with("image"))
		return DescriptorKind::StorageImage;
	return std::nullopt;
}

} // anonymous namespace

ShaderSnapshot MakeShaderSnapshot(const ShaderData& shader,
                                  const std::span<const uint32_t> optionalBindings)
{
	ShaderSnapshot snapshot;
	snapshot.shaderUid = shader.GetObjectID();

	const std::array<const ShaderData*, 1> shaders = {&shader};
	const PassPorts ports = ExtractPassPorts(shader.GetName(), shaders);
	if (!ports.problems.empty())
		throw std::runtime_error("ShaderSnapshot: " + ports.problems.front());
	snapshot.ports = ports.ports;

	const std::set<uint32_t> optional(optionalBindings.begin(), optionalBindings.end());
	std::map<uint32_t, DescriptorBinding> byBinding;

	const auto addBinding = [&byBinding](const uint32_t binding,
	                                     const DescriptorKind kind,
	                                     const uint32_t stageMask,
	                                     const uint32_t count)
	{
		const auto it = byBinding.find(binding);
		if (it == byBinding.end())
		{
			DescriptorBinding reflected;
			reflected.binding   = binding;
			reflected.kind      = kind;
			reflected.stageMask = stageMask;
			reflected.count     = count;
			byBinding.emplace(binding, reflected);
			return;
		}

		if (it->second.kind != kind || it->second.count != count)
		{
			throw std::runtime_error(
				"ShaderSnapshot: binding " + std::to_string(binding)
				+ " is declared with conflicting types or counts");
		}
		it->second.stageMask |= stageMask;
	};

	std::set<std::tuple<std::string, uint32_t, uint32_t>> seenPushConstants;

	for (const ShaderType stage : kStages)
	{
		if (!shader.HasStage(stage))
			continue;

		const ShaderUnit* unit = shader.FindStage(stage);
		if (!unit)
			continue;

		const ShaderStruct& ir = unit->parsed;
		const uint32_t stageMask = StageMask(stage);

		for (const S_Uniform& uniform : ir.uniform_list)
		{
			if (uniform.binding < 0)
				continue;

			const std::optional<DescriptorKind> kind = UniformDescriptorKind(uniform);
			if (!kind)
				continue;

			addBinding(static_cast<uint32_t>(uniform.binding), *kind, stageMask,
			           static_cast<uint32_t>(std::max(uniform.count, 1)));
		}

		for (const S_StructDef& block : ir.ubuffer_list)
		{
			if (block.binding < 0)
				continue;
			addBinding(static_cast<uint32_t>(block.binding),
			           DescriptorKind::UniformBuffer, stageMask, 1);
		}

		for (const S_StructDef& block : ir.SB_list)
		{
			if (block.binding < 0)
				continue;
			addBinding(static_cast<uint32_t>(block.binding),
			           DescriptorKind::StorageBuffer, stageMask, 1);
		}

		for (const S_PushConstant& member : ir.push_constants)
		{
			const auto key = std::make_tuple(member.name, member.offset, member.size);
			if (!seenPushConstants.insert(key).second)
				continue;

			snapshot.pushConstants.push_back({
				member.name,
				member.typeName,
				member.offset,
				member.size,
			});
		}

		switch (stage)
		{
		case ShaderType::VERTEX:
			snapshot.vertexSpv = unit->spv;
			break;
		case ShaderType::FRAGMENT:
			snapshot.fragmentSpv = unit->spv;
			break;
		case ShaderType::COMPUTE:
			snapshot.computeSpv = unit->spv;
			break;
		case ShaderType::GEOMETRY:
			// No snapshot field: the built-in passes do not use a geometry stage.
			break;
		}

		snapshot.version = std::max(snapshot.version, unit->GetVersion());
	}

	std::sort(snapshot.pushConstants.begin(), snapshot.pushConstants.end(),
		[](const PushConstantMember& a, const PushConstantMember& b)
		{
			if (a.offset != b.offset)
				return a.offset < b.offset;
			return a.name < b.name;
		});

	for (const auto& [binding, reflected] : byBinding)
	{
		DescriptorBinding value = reflected;
		value.partiallyBound = optional.contains(binding);
		snapshot.bindings.push_back(value);
	}

	return snapshot;
}

} // namespace neurus
