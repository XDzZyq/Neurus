#include "shader/BuiltInRenderData.hpp"

#include "shader/ComputeShader.hpp"
#include "shader/RenderShader.hpp"
#include "shader/ShaderLibrary.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/render_graph/PassKinds.hpp"
#include "asset/data/render_graph/PortSource.hpp"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neurus {

namespace {

std::shared_ptr<const ShaderData> LoadDefaultShader(const PassShaderSource& source)
{
	const std::string name(source.name);
	if (!source.computePath.empty())
	{
		auto shader = ShaderLibrary::LoadComputeShader(name, std::string(source.computePath));
		if (shader && shader->CompileToSpv())
		{
			return std::shared_ptr<const ShaderData>(std::move(shader));
		}
	}
	else
	{
		auto shader = ShaderLibrary::LoadRenderShader(name, std::string(source.vertexPath),
		                                              std::string(source.fragmentPath));
		if (shader && shader->CompileToSpv())
		{
			return std::shared_ptr<const ShaderData>(std::move(shader));
		}
	}
	throw std::runtime_error("Cannot construct built-in shader '" + name + "'");
}

} // namespace

struct BuiltInRenderData::Impl final : IPortSource
{
	Impl();

	const PassPorts* FindPassPorts(const std::string_view passKind) const override
	{
		const auto found = ports.find(std::string(passKind));
		return found == ports.end() ? nullptr : &found->second;
	}

	std::vector<std::string> ExternalInputs(const std::string_view passKind) const override
	{
		return PassExternalInputs(passKind);
	}

	std::shared_ptr<const RenderGraphData> graph;
	std::unordered_map<std::string, std::shared_ptr<const ShaderData>> shaders;
	std::unordered_map<std::string, PassPorts> ports;
};

BuiltInRenderData::Impl::Impl()
{
	for (const PassKindInfo& kind : kPassKinds)
	{
		std::vector<const ShaderData*> passShaders;
		passShaders.reserve(kind.shaders.size());
		for (const PassShaderSource& source : kind.shaders)
		{
			auto [entry, inserted] = shaders.try_emplace(std::string(source.name));
			if (inserted)
			{
				entry->second = LoadDefaultShader(source);
			}
			passShaders.push_back(entry->second.get());
		}

		PassPorts contract = ExtractPassPorts(std::string(kind.name), passShaders);
		AddDeclaredPassOutputs(contract, kind);
		if (!contract.problems.empty())
		{
			throw std::runtime_error("Invalid port contract for '" + contract.kind + "': " + contract.problems.front());
		}
		ports.emplace(std::string(kind.name), std::move(contract));
	}
	graph = std::make_shared<const RenderGraphData>(RenderGraphData::Default(*this));
}

BuiltInRenderData::BuiltInRenderData() = default;
BuiltInRenderData::~BuiltInRenderData() = default;

const BuiltInRenderData::Impl& BuiltInRenderData::Data() const
{
	std::call_once(m_initialize, [this]
	{
		m_impl = std::make_unique<const Impl>();
	});
	return *m_impl;
}

const std::shared_ptr<const RenderGraphData>& BuiltInRenderData::Graph() const
{
	return Data().graph;
}

const IPortSource& BuiltInRenderData::Ports() const
{
	return Data();
}

std::shared_ptr<const ShaderData> BuiltInRenderData::ShaderFor(const std::string_view name) const
{
	const auto& shaders = Data().shaders;
	const auto found = shaders.find(std::string(name));
	return found == shaders.end() ? nullptr : found->second;
}

} // namespace neurus
