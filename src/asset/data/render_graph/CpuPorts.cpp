/**
 * @file CpuPorts.cpp
 * @brief Port extraction from ShaderStruct.
 */

#include "asset/data/render_graph/CpuPorts.hpp"

#include "asset/data/ShaderData.hpp"
#include "asset/data/render_graph/PassKinds.hpp"

#include <algorithm>
#include <array>
#include <utility>

namespace neurus {

namespace {

/// @brief The GLSL type string of a uniform declaration.
std::string UniformType(const S_Uniform& uniform)
{
	if (!uniform.actualType.empty())
		return uniform.actualType;
	return ShaderStruct::ParseType(uniform.type);
}

/// @brief Adds the sampler and image uniforms of one stage.
void AddUniformPorts(CpuPorts& result, const ShaderStruct& shader)
{
	for (const S_Uniform& uniform : shader.uniform_list)
	{
		// A negative binding is not a descriptor: a plain uniform, not a port.
		if (uniform.binding < 0)
			continue;

		const std::string type = UniformType(uniform);

		CpuPort port;
		port.name      = uniform.name;
		port.type      = type;
		port.binding   = static_cast<uint32_t>(uniform.binding);
		port.location  = 0;

		if (IsSamplerPortType(type))
		{
			port.direction = PortDirection::Input;
			result.Add(port);
		}
		else if (IsStorageImagePortType(type))
		{
			port.direction = PortDirection::Output;
			result.Add(port);
		}
	}
}

/// @brief Adds a fragment stage's layout(location = N) out declarations.
void AddFragmentOutputs(CpuPorts& result, const ShaderStruct& shader)
{
	for (const S_IO& output : shader.pass_list)
	{
		CpuPort port;
		port.name      = output.name;
		port.direction = PortDirection::Output;
		port.type      = output.typeName.empty() ? ShaderStruct::ParseType(output.type)
		                                         : output.typeName;
		port.binding   = 0;
		port.location  = static_cast<uint32_t>(std::max(output.location, 0));
		result.Add(port);
	}
}

} // anonymous namespace

const CpuPort* CpuPorts::Find(const std::string_view name,
                               const PortDirection direction) const
{
	for (const CpuPort& port : ports)
	{
		if (port.direction == direction && port.name == name)
			return &port;
	}
	return nullptr;
}

const CpuPort* CpuPorts::FindRole(const CpuPort& role) const
{
	for (const CpuPort& port : ports)
	{
		if (port.direction == role.direction && port.type == role.type
		    && port.binding == role.binding && port.location == role.location)
			return &port;
	}
	return nullptr;
}

void CpuPorts::Add(const CpuPort& port)
{
	const CpuPort* existing = Find(port.name, port.direction);
	if (!existing)
	{
		ports.push_back(port);
	}
	else if (existing->type != port.type || existing->binding != port.binding
	         || existing->location != port.location)
	{
		problems.push_back("port '" + port.name + "' is declared twice with different descriptor identity");
	}
}

void CpuPorts::Merge(const CpuPorts& other)
{
	for (const CpuPort& port : other.ports)
	{
		Add(port);
	}
	problems.insert(problems.end(), other.problems.begin(), other.problems.end());
}

void AddDeclaredPassOutputs(CpuPorts& ports, const PassKindInfo& kind)
{
	for (const DeclaredPort& port : kind.declaredOutputs)
	{
		ports.Add({std::string(port.name), PortDirection::Output, std::string(port.type), 0, 0});
	}
}

std::vector<std::string> PassExternalInputs(const std::string_view passKind)
{
	const PassKindInfo* kind = FindPassKind(passKind);
	if (!kind)
	{
		return {};
	}
	return {kind->externalInputs.begin(), kind->externalInputs.end()};
}

std::vector<CpuPort> CpuPorts::Inputs() const
{
	std::vector<CpuPort> inputs;
	for (const CpuPort& port : ports)
	{
		if (port.direction == PortDirection::Input)
			inputs.push_back(port);
	}
	return inputs;
}

std::vector<CpuPort> CpuPorts::Outputs() const
{
	std::vector<CpuPort> outputs;
	for (const CpuPort& port : ports)
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

CpuPorts ExtractPassPorts(std::string kind,
                           const std::span<const ShaderData* const> shaders,
                           const std::span<const CpuPort> declaredOutputs)
{
	CpuPorts result;
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

	for (const CpuPort& declared : declaredOutputs)
		result.Add(declared);

	return result;
}

} // namespace neurus
