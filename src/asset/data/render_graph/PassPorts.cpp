/**
 * @file PassPorts.cpp
 * @brief Port extraction from ShaderStruct.
 */

#include "asset/data/render_graph/PassPorts.hpp"

#include "asset/data/ShaderData.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace neurus {

namespace {

/// @brief Adds a port unless an identical declaration already exists.
void AddPort(PassPorts& result, const PassPort& port)
{
	auto it = std::find_if(result.ports.begin(), result.ports.end(),
		[&port](const PassPort& existing)
		{
			return existing.direction == port.direction && existing.name == port.name;
		});

	if (it == result.ports.end())
	{
		result.ports.push_back(port);
		return;
	}

	if (it->type != port.type || it->binding != port.binding || it->location != port.location)
	{
		result.problems.push_back(
			"port '" + port.name + "' is declared twice with different descriptor identity");
	}
}

/// @brief The GLSL type string of a uniform declaration.
std::string UniformType(const S_Uniform& uniform)
{
	if (!uniform.actualType.empty())
		return uniform.actualType;
	return ShaderStruct::ParseType(uniform.type);
}

/// @brief Adds the sampler and image uniforms of one stage.
void AddUniformPorts(PassPorts& result, const ShaderStruct& shader)
{
	for (const S_Uniform& uniform : shader.uniform_list)
	{
		// A negative binding is not a descriptor: a plain uniform, not a port.
		if (uniform.binding < 0)
			continue;

		const std::string type = UniformType(uniform);

		PassPort port;
		port.name      = uniform.name;
		port.type      = type;
		port.binding   = static_cast<uint32_t>(uniform.binding);
		port.location  = 0;

		if (IsSamplerPortType(type))
		{
			port.direction = PortDirection::Input;
			AddPort(result, port);
		}
		else if (IsStorageImagePortType(type))
		{
			port.direction = PortDirection::Output;
			AddPort(result, port);
		}
	}
}

/// @brief Adds a fragment stage's layout(location = N) out declarations.
void AddFragmentOutputs(PassPorts& result, const ShaderStruct& shader)
{
	for (const S_IO& output : shader.pass_list)
	{
		PassPort port;
		port.name      = output.name;
		port.direction = PortDirection::Output;
		port.type      = output.typeName.empty() ? ShaderStruct::ParseType(output.type)
		                                         : output.typeName;
		port.binding   = 0;
		port.location  = static_cast<uint32_t>(std::max(output.location, 0));
		AddPort(result, port);
	}
}

} // anonymous namespace

const PassPort* PassPorts::Find(const std::string_view name,
                                  const PortDirection direction) const
{
	for (const PassPort& port : ports)
	{
		if (port.direction == direction && port.name == name)
			return &port;
	}
	return nullptr;
}

std::vector<PassPort> PassPorts::Inputs() const
{
	std::vector<PassPort> inputs;
	for (const PassPort& port : ports)
	{
		if (port.direction == PortDirection::Input)
			inputs.push_back(port);
	}
	return inputs;
}

std::vector<PassPort> PassPorts::Outputs() const
{
	std::vector<PassPort> outputs;
	for (const PassPort& port : ports)
	{
		if (port.direction == PortDirection::Output)
			outputs.push_back(port);
	}
	return outputs;
}

bool IsSamplerPortType(const std::string_view type)
{
	return type.starts_with("sampler");
}

bool IsStorageImagePortType(const std::string_view type)
{
	return type.starts_with("image");
}

PassPorts ExtractPassPorts(std::string kind,
                           const std::span<const ShaderData* const> shaders,
                           const std::span<const PassPort> declaredOutputs)
{
	PassPorts result;
	result.kind = std::move(kind);

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

			if (stage == ShaderType::FRAGMENT)
				AddFragmentOutputs(result, unit->parsed);

			AddUniformPorts(result, unit->parsed);
		}
	}

	for (const PassPort& declared : declaredOutputs)
		AddPort(result, declared);

	return result;
}

} // namespace neurus
