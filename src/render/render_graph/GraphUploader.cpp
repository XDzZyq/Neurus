/**
 * @file GraphUploader.cpp
 * @brief Document-to-runtime-graph service: mapping, validation and planning.
 */

#include "render/render_graph/GraphUploader.hpp"

#include "render/RenderCache.hpp"
#include "render/render_graph/PassFactory.hpp"
#include "render/render_graph/RenderGraph.hpp"
#include "render/render_graph/ResourceTag.hpp"
#include "render/resources/ShaderGPU.hpp"
#include "shader/ShaderInterface.hpp"
#include "render_api/RenderRequests.hpp"

#include "asset/data/render_graph/RenderGraphTopology.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "asset/data/render_graph/PortSource.hpp"

#include "core/GraphMorphism.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace neurus {

namespace {

using GpuNode   = RenderGraph::NodeT;
using GpuOutput = RenderGraph::SocketOutT;
using GpuInput  = RenderGraph::SocketInT;

using RenderGraphMorphism = GraphMorphism<RenderGraphTopology, RenderGraph::GraphT>;

GpuOutput* FindOutput(GpuNode& node, const std::string_view name)
{
	for (const auto& output : node.outputs)
	{
		if (output->name == name)
			return output.get();
	}
	return nullptr;
}

GpuInput* FindInput(GpuNode& node, const std::string_view name)
{
	for (const auto& input : node.inputs)
	{
		if (input->name == name)
			return input.get();
	}
	return nullptr;
}

/** @brief Compares interfaces by identity rather than unordered stage traversal order. */
bool SamePorts(const PassPorts& actual, const PassPorts& expected)
{
	if (actual.ports.size() != expected.ports.size())
	{
		return false;
	}
	for (const PassPort& port : expected.ports)
	{
		if (!actual.FindRole(port))
		{
			return false;
		}
	}
	return true;
}

/**
 * @brief Stage 1 bridge from a default document resource key to its legacy attachment.
 *
 * The overlay graph and the existing attachment accessors still name
 * AttachmentName, so the default scene resources are physically the same images
 * under both identities. A custom node id has no legacy name and gets its own
 * image. Removed when the overlay's sockets move to ports.
 */
std::optional<AttachmentName> LegacyAttachmentFor(const std::string& key)
{
	static const std::unordered_map<std::string, AttachmentName> kAliases = {
		{"geometry/gPosition",         AttachmentName::Position},
		{"geometry/gNormal",           AttachmentName::Normal},
		{"geometry/gAlbedo",           AttachmentName::Albedo},
		{"geometry/gMetallicRoughness", AttachmentName::MetallicRoughness},
		{"shadow_intensity/outputShadow", AttachmentName::ShadowIntensity},
		{"ssao/outputSSAO",            AttachmentName::SSAO},
		{"lighting/outputImage",       AttachmentName::HDRColor},
		{"compose/outputComposed",     AttachmentName::ComposedOutput},
		{"fxaa/outputFXAA",            AttachmentName::FXAAOutput},
	};

	const auto it = kAliases.find(key);
	return it == kAliases.end() ? std::nullopt
	                            : std::optional<AttachmentName>(it->second);
}

/// @brief Resolves pass instances and socket names during graph compilation.
using PortContracts = std::unordered_map<int, PassPorts>;

RenderGraphMorphism::Policy MakePolicy(PassFactory& factory, const IPortSource& portCatalog,
                                      const PortContracts& contracts)
{
	RenderGraphMorphism::Policy policy;

	policy.createNode = [&factory, &portCatalog, &contracts](RenderGraph::GraphT& target,
	                               const RenderGraphTopology::NodeType& node) -> GpuNode*
	{
		const PassPorts* ports = portCatalog.FindPassPorts(node.data.pass);
		if (!ports)
		{
			return nullptr;
		}
		const std::optional<int> shaderId = node.data.shader
			? std::optional<int>(node.data.shader->GetObjectID()) : std::nullopt;
		auto owned = factory.Acquire(node.data.pass, node.name, shaderId);
		Pass* pass = owned.get();
		if (!pass)
		{
			return nullptr;
		}
		GpuNode* gpu = RenderGraph::MakePassNode(
			target, pass, *ports, portCatalog.ExternalInputs(node.data.pass), node.name);
		if (gpu)
		{
			// The runtime node keeps the document id so diagnostics and the
			// resource keys name the graph the user edited.
			gpu->name = node.name;
			gpu->data.ownedPass = std::move(owned);
			if (shaderId)
			{
				// Socket names follow the document. Payloads retain the canonical pass roles
				// consumed by Record() and the built-in descriptor binding plan.
				const auto contract = WithShaderPortNames(*ports, contracts.at(*shaderId));
				for (auto& socket : gpu->inputs)
					socket->name = contract.FindRole(socket->data)->name;
				for (auto& socket : gpu->outputs)
					socket->name = contract.FindRole(socket->data)->name;
			}
		}
		return gpu;
	};

	policy.createOutput = [](GpuNode& node, const RenderGraphTopology::SocketOutT& port)
		-> GpuOutput*
	{
		return FindOutput(node, port.name);
	};

	policy.createInput = [](GpuNode& node, const RenderGraphTopology::SocketInT& port)
		-> GpuInput*
	{
		return FindInput(node, port.name);
	};

	return policy;
}

} // anonymous namespace

GraphUploader::GraphUploader(PassFactory& factory, const IPortSource& portCatalog,
                             std::shared_ptr<const RenderGraphTopology> defaultGraph)
	: m_factory(factory)
	, m_portCatalog(portCatalog)
	, m_defaultGraph(std::move(defaultGraph))
{
	if (!m_defaultGraph)
	{
		throw std::invalid_argument("GraphUploader requires a default graph");
	}
	SetPending(CompileRenderGraphRequest{});
}

void GraphUploader::SetPending(const CompileRenderGraphRequest& request)
{
	auto document = request.graph ? request.graph : m_defaultGraph;
	if (m_document != document)
	{
		m_documentHash = document->Hash();
		m_document = std::move(document);
	}
}

GraphUploadResult GraphUploader::Compile(RenderCache& cache)
{
	GraphUploadResult result;

	const RenderGraphTopology& document = *m_document;

	PortContracts contracts;
	// Only the UID crosses from a live CPU shader reference into GPU mapping.
	for (const auto& node : document.nodes)
	{
		if (!node->data.shader)
		{
			continue;
		}
		try
		{
			const int shaderId = node->data.shader->GetObjectID();
			const auto selected = m_factory.ShaderFor(node->data.pass, shaderId);
			const auto expected = m_factory.ShaderFor(node->data.pass);
			if (!selected || !expected)
			{
				continue;
			}
			contracts.try_emplace(shaderId, selected->Ports());
			if (!SamePorts(selected->Ports(), expected->Ports()))
			{
				result.errors.push_back({node->name, {}, "uploaded shader changes the built-in port contract"});
			}
			if (selected->GetType() != expected->GetType())
				throw std::invalid_argument("uploaded shader changes the pass stage");
			for (const auto stage : {ShaderType::VERTEX, ShaderType::FRAGMENT, ShaderType::COMPUTE})
			{
				const auto* original = expected->FindStage(stage);
				const auto* actual = selected->FindStage(stage);
				if (!original && !actual)
					continue;
				if (!original || !actual)
					throw std::invalid_argument("uploaded shader changes the pass stages");
				const auto problem = PassShaderInterfaceProblem(original->parsed, original->spv,
				                                              actual->parsed, actual->spv, stage);
				if (!problem.empty())
					throw std::invalid_argument(problem);
			}
			if (selected->GetType() == ShaderType::VERTEX)
			{
				std::string error;
				if (!ValidateRenderShaderInterface(selected->GetStage(ShaderType::VERTEX).spv,
				                                   selected->GetStage(ShaderType::FRAGMENT).spv, error))
					throw std::invalid_argument(error);
			}
		}
		catch (const std::exception& error)
		{
			result.errors.push_back({node->name, {}, error.what()});
		}
	}
	if (!result.errors.empty())
	{
		return result;
	}

	const auto resolve = [&](const CpuNode& node)
	{
		const PassPorts& builtin = *m_portCatalog.FindPassPorts(node.pass);
		return node.shader ? WithShaderPortNames(builtin, contracts.at(node.shader->GetObjectID())) : builtin;
	};
	for (const auto& diagnostic : ValidateCpuGraph(document, m_portCatalog, resolve))
		result.errors.push_back({diagnostic.node, diagnostic.port, diagnostic.message});
	if (!result.errors.empty())
		return result;
	const auto active = document.ActiveNodes();

	RenderGraph::GraphT target;
	try
	{
		const RenderGraphMorphism morphism(MakePolicy(m_factory, m_portCatalog, contracts));
		auto mapping = morphism.Map(document);
		for (const auto& problem : mapping.images.Validate(document).problems)
			result.errors.push_back({{}, {}, problem});
		if (result.errors.empty())
			target = std::move(mapping.target);
	}
	catch (const std::exception& error)
	{
		result.errors.push_back({{}, {}, std::string("pass creation failed: ") + error.what()});
		return result;
	}

	if (!result.errors.empty())
		return result;

	auto graph = std::make_unique<RenderGraph>();
	graph->Adopt(std::move(target));

	try
	{
		graph->Compile(&active);
	}
	catch (const std::exception& error)
	{
		result.errors.push_back({{}, {}, error.what()});
		return result;
	}

	RenderCache::ResourcePlan plan;
	for (const auto& node : document.nodes)
	{
		if (!active.contains(node->name))
			continue;
		for (const auto& output : node->outputs)
		{
			if (!output->data.tag)
				continue;
			const std::string key = ResourceKey(node->name, output->name);
			plan.tags.emplace(key, *output->data.tag);
			if (const auto legacy = LegacyAttachmentFor(key))
				plan.Alias(*legacy, key);
		}
	}

	for (const auto& [name, endpoint] : document.sceneOutputs)
	{
		const std::string key = ResourceKey(endpoint.node, endpoint.port);
		if (name == "color")
			result.color = key;
		else if (name == "depth")
		{
			plan.Alias(AttachmentName::Depth, key);
			result.depth = true;
		}
		else if (name == "object_id")
		{
			plan.Alias(AttachmentName::IDBuffer, key);
			result.objectId = true;
		}
	}
	cache.InstallResourcePlan(std::move(plan));
	result.graph = std::move(graph);
	return result;
}

} // namespace neurus
