#include "shader/BuiltInRenderData.hpp"

#include "shader/ComputeShader.hpp"
#include "shader/RenderShader.hpp"
#include "shader/ShaderLibrary.hpp"

#include "asset/ResourceManager.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/render_graph/PassKinds.hpp"
#include "asset/data/render_graph/PortSource.hpp"

#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neurus {

namespace {

std::shared_ptr<ShaderData> LoadDefaultShader(const PassShaderSource& source)
{
	const std::string name(source.name);
	if (!source.computePath.empty())
	{
		auto shader = ShaderLibrary::LoadComputeShader(name, std::string(source.computePath));
		if (shader && shader->CompileToSpv())
		{
			return shader;
		}
	}
	else
	{
		auto shader = ShaderLibrary::LoadRenderShader(name, std::string(source.vertexPath),
		                                              std::string(source.fragmentPath));
		if (shader && shader->CompileToSpv())
		{
			return shader;
		}
	}
	throw std::runtime_error("Cannot construct built-in shader '" + name + "'");
}

} // namespace

struct BuiltInRenderData::Impl final : IPortSource
{
	Impl();

	const CPUPorts* FindPassPorts(const std::string_view passKind) const override
	{
		const auto found = ports.find(std::string(passKind));
		return found == ports.end() ? nullptr : &found->second;
	}

	std::vector<std::string> ExternalInputs(const std::string_view passKind) const override
	{
		return PassExternalInputs(passKind);
	}

	ResourceManager resources;
	std::shared_ptr<const RenderGraphData> graph;
	std::unordered_map<std::string, std::shared_ptr<const ShaderData>> shaders;
	std::unordered_map<std::string, CPUPorts> ports;
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
				auto shader = LoadDefaultShader(source);
				resources.Register(shader);
				entry->second = std::move(shader);
			}
			passShaders.push_back(entry->second.get());
		}

		CPUPorts contract = ExtractPassPorts(std::string(kind.name), passShaders);
		AddDeclaredPassOutputs(contract, kind);
		if (!contract.problems.empty())
		{
			throw std::runtime_error("Invalid port contract for '" + contract.kind + "': " + contract.problems.front());
		}
		ports.emplace(std::string(kind.name), std::move(contract));
	}
	graph = resources.Load<RenderGraphData>(*this);
}

struct BuiltInRenderData::SharedState
{
	std::once_flag initialize;
	std::unique_ptr<const Impl> data;
};

BuiltInRenderData::BuiltInRenderData() : m_state(std::make_shared<SharedState>()) {}
BuiltInRenderData::~BuiltInRenderData() = default;

const BuiltInRenderData::Impl& BuiltInRenderData::Data() const
{
	std::call_once(m_state->initialize, [this]
	{
		m_state->data = std::make_unique<const Impl>();
	});
	return *m_state->data;
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

std::shared_ptr<ShaderData> BuiltInRenderData::CreatePassShader(std::string_view passKind, const std::string& name) const
{
	const auto* kind = FindPassKind(passKind);
	if (!kind || kind->shaders.empty())
		throw std::invalid_argument("Unknown pass shader recipe: " + std::string(passKind));
	const auto source = ShaderFor(kind->shaders.front().name);
	auto copyUnit = [&](ShaderType stage)
	{
		ShaderUnit unit = source->GetStage(stage);
		unit.path.clear();
		return unit;
	};
	if (source->GetType() == ShaderType::COMPUTE)
	{
		auto shader = std::make_shared<ComputeShader>(name);
		shader->GetStage(ShaderType::COMPUTE) = copyUnit(ShaderType::COMPUTE);
		return shader;
	}
	return std::make_shared<RenderShader>(name, copyUnit(ShaderType::VERTEX), copyUnit(ShaderType::FRAGMENT));
}

} // namespace neurus
