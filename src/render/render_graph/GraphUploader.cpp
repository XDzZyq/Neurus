/**
 * @file GraphUploader.cpp
 * @brief Document-to-runtime-graph service: mapping, shader checks and planning.
 */

#include "render/render_graph/GraphUploader.hpp"

#include "render/RenderCache.hpp"
#include "render/render_graph/PassFactory.hpp"
#include "render/render_graph/RenderGraph.hpp"
#include "render/render_graph/ResourceTag.hpp"
#include "render/resources/ShaderGPU.hpp"
#include "render_api/RenderRequests.hpp"

#include "asset/data/RenderGraphData.hpp"
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

using RenderGraphMorphism = GraphMorphism<RenderGraphData, RenderGraph::GraphT>;

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
RenderGraphMorphism::Policy MakePolicy(PassFactory& factory, const IPortSource& portCatalog,
                                       const std::unordered_map<std::string, PublishedShader>& shaders)
{
	RenderGraphMorphism::Policy policy;

	policy.createNode = [&factory, &portCatalog, &shaders](const RenderGraphData::NodeType& node)
		-> std::optional<RenderGraph::PassBody>
	{
		const auto published = shaders.find(node.name);
		const std::optional<int> shaderId = published == shaders.end()
		                                        ? std::nullopt
		                                        : std::optional<int>(published->second.uid);
		auto owned = factory.Acquire(node.data.pass, node.name, shaderId);
		if (!owned)
			return std::nullopt;

		auto body = RenderGraph::MakePassBody(*owned, portCatalog.ExternalInputs(node.data.pass), node.name);
		body.ownedPass = std::move(owned);
		return body;
	};

	const auto mapSocket = [&portCatalog, &shaders](const auto& source) -> std::optional<RenderGraph::PassPort>
	{
		const auto& node = *source.owner;
		const auto* ports = portCatalog.FindPassPorts(node.data.pass);
		if (!ports)
			return std::nullopt;
		// A selected shader may rename its ports, so resolve it by descriptor role.
		const auto* role = shaders.contains(node.name)
		                       ? ports->FindRole(source.data)
		                       : ports->Find(source.name, source.data.direction);
		if (!role)
			throw std::runtime_error("no runtime role for socket '" + node.name + "." + source.name + "'");

		// Graph edges use the authored name. The runtime pass binds
		// resources by the canonical name for this shader role.
		auto port = RenderGraph::PassPort::FromCPUPort(source.data);
		port.name = role->name;
		return port;
	};
	policy.createOutput = mapSocket;
	policy.createInput = mapSocket;
	return policy;
}

} // anonymous namespace

GraphUploader::GraphUploader(PassFactory& factory, const IPortSource& portCatalog,
                             std::shared_ptr<const RenderGraphData> defaultGraph)
	: m_factory(factory)
	, m_portCatalog(portCatalog)
	, m_defaultGraph(std::move(defaultGraph))
{
	if (!m_defaultGraph)
	{
		throw std::invalid_argument("GraphUploader requires a default graph");
	}
	// Hashing reads only plain document data, but the default lives on the
	// constructing thread, so cache it here instead of touching it per compile.
	m_defaultHash = m_defaultGraph->Hash();
	SetPending(CompileRenderGraphRequest{});
}

void GraphUploader::SetPending(const CompileRenderGraphRequest& request)
{
	if (request.graph)
	{
		m_document = request.graph;
		m_shaders = request.shaders;
		m_documentHash = request.hash;
		return;
	}
	m_document = m_defaultGraph;
	m_shaders.clear();
	m_documentHash = m_defaultHash;
}

GraphUploadResult GraphUploader::Compile(RenderCache& cache)
{
	GraphUploadResult result;

	const RenderGraphData& document = *m_document;

	// The published clone must already be free of CPU shader references; a
	// reference here would make the render thread read the main-thread pool.
	for (const auto& node : document.nodes)
	{
		if (!node->data.shader)
			continue;
		result.errors.push_back({node->name, {}, "render graph node carries a CPU shader reference"});
	}
	if (!result.errors.empty())
	{
		return result;
	}

	// GPU-only guard: the published shader must name a resident upload of the
	// same version, type and stage set as the pass's built-in shader. CPU port,
	// descriptor and push-constant contracts are checked before publication.
	for (const auto& node : document.nodes)
	{
		const auto published = m_shaders.find(node->name);
		if (published == m_shaders.end())
			continue;
		try
		{
			const auto selected = m_factory.ShaderFor(node->data.pass, published->second.uid);
			const auto expected = m_factory.ShaderFor(node->data.pass);
			if (!selected || !expected)
			{
				continue;
			}
			if (selected->GetVersion() != published->second.version)
				throw std::invalid_argument("published shader version does not match the resident upload");
			if (selected->GetType() != expected->GetType())
				throw std::invalid_argument("uploaded shader changes the pass stage");
			for (const auto stage : {ShaderType::VERTEX, ShaderType::FRAGMENT,
			                         ShaderType::COMPUTE, ShaderType::GEOMETRY})
			{
				if (selected->HasStage(stage) != expected->HasStage(stage))
					throw std::invalid_argument("uploaded shader changes the pass stages");
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

	const auto active = document.ActiveNodes();

	RenderGraph::GraphT target;
	try
	{
		const RenderGraphMorphism morphism(MakePolicy(m_factory, m_portCatalog, m_shaders));
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
	for (const auto* node : graph->CompiledOrder())
	{
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
