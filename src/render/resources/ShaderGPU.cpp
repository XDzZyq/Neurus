/**
 * @file ShaderGPU.cpp
 * @brief Shader module construction and uploaded port metadata.
 */

#include "render/resources/ShaderGPU.hpp"

#include "asset/data/render_graph/PassKinds.hpp"

#include <stdexcept>
#include <utility>

namespace neurus
{

ShaderGPU::ShaderGPU(const vk::raii::Device& device, const UploadShaderRequest& request)
	: m_id(request.shaderId)
	, m_version(request.version)
	, m_type(request.primaryType)
	, m_stages(request.stages)
	, m_ports(request.ports)
{
	if (!m_ports.problems.empty())
	{
		throw std::invalid_argument(m_ports.problems.front());
	}
	for (const auto& [stage, data] : m_stages)
	{
		if (data.spv.empty())
		{
			throw std::invalid_argument("Shader upload has an empty stage");
		}
		m_modules.emplace(stage, vk::raii::ShaderModule(device, vk::ShaderModuleCreateInfo({}, data.spv)));
	}
}

const ShaderStageUpload* ShaderGPU::FindStage(const ShaderType stage) const
{
	const auto found = m_stages.find(stage);
	return found == m_stages.end() ? nullptr : &found->second;
}

CPUPorts CollectUploadedPassPorts(std::string kind, const std::span<const ShaderGPU* const> shaders)
{
	CPUPorts result;
	result.kind = std::move(kind);
	for (const ShaderGPU* shader : shaders)
	{
		if (shader)
		{
			result.Merge(shader->Ports());
		}
	}
	if (const PassKindInfo* info = FindPassKind(result.kind))
	{
		AddDeclaredPassOutputs(result, *info);
	}
	return result;
}

} // namespace neurus
