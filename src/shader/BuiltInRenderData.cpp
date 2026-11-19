#include "shader/BuiltInRenderData.hpp"

#include "shader/ComputeShader.hpp"
#include "shader/RenderShader.hpp"
#include "shader/ShaderLibrary.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/render_graph/PassShaders.hpp"
#include "core/ResourceManager.hpp"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace neurus {

struct BuiltInRenderData::Impl
{
	ResourceManager resources;
	std::unique_ptr<PassShaders> ports;
	std::shared_ptr<const RenderGraphData> graph;
	std::unordered_map<std::string, std::shared_ptr<const ShaderData>> shaders;
};

const BuiltInRenderData& BuiltInRenderData::Get()
{
	static const BuiltInRenderData builtins;
	return builtins;
}

BuiltInRenderData::BuiltInRenderData() : m_impl(std::make_unique<Impl>())
{
	const auto load = [this](const PassShaderSource& source) -> std::shared_ptr<ShaderData>
	{
		std::shared_ptr<ShaderData> shader;
		if (!source.computePath.empty())
		{
			auto compute = ShaderLibrary::LoadComputeShader(std::string(source.name), std::string(source.computePath));
			if (!compute || !compute->CompileToSpv())
				throw std::runtime_error("Cannot construct built-in shader '" + std::string(source.name) + "'");
			shader = std::move(compute);
		}
		else
		{
			auto render = ShaderLibrary::LoadRenderShader(std::string(source.name),
				std::string(source.vertexPath), std::string(source.fragmentPath));
			if (!render || !render->CompileToSpv())
				throw std::runtime_error("Cannot construct built-in shader '" + std::string(source.name) + "'");
			shader = std::move(render);
		}
		m_impl->shaders.emplace(std::string(source.name), shader);
		return shader;
	};
	std::vector<std::string> problems;
	m_impl->ports = std::make_unique<PassShaders>(m_impl->resources, load, problems);
	if (!problems.empty())
		throw std::runtime_error(problems.front());

	auto graph = std::make_shared<RenderGraphData>(RenderGraphData::Default(*m_impl->ports));
	m_impl->graph = std::move(graph);
}

BuiltInRenderData::~BuiltInRenderData() = default;

const std::shared_ptr<const RenderGraphData>& BuiltInRenderData::Graph() const
{
	return m_impl->graph;
}

const IPortSource& BuiltInRenderData::Ports() const
{
	return *m_impl->ports;
}

std::shared_ptr<const RenderShader> BuiltInRenderData::RenderShaderFor(const std::string_view name) const
{
	return std::dynamic_pointer_cast<const RenderShader>(m_impl->shaders.at(std::string(name)));
}

std::shared_ptr<const ComputeShader> BuiltInRenderData::ComputeShaderFor(const std::string_view name) const
{
	return std::dynamic_pointer_cast<const ComputeShader>(m_impl->shaders.at(std::string(name)));
}

} // namespace neurus
