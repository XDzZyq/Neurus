/**
 * @file GraphUploader.cpp
 * @brief Document-to-runtime-graph service: mapping, validation and planning.
 */

#include "render/render_graph/GraphUploader.hpp"

#include "render/RenderCache.hpp"
#include "render/render_graph/PassFactory.hpp"
#include "render/render_graph/RenderGraph.hpp"
#include "render/render_graph/ResourceTag.hpp"
#include "render/render_graph/ShaderReflection.hpp"
#include "render_api/RenderRequests.hpp"

#include "asset/data/RenderGraphData.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"
#include "asset/data/render_graph/PortSource.hpp"

#include "core/Graph.hpp"
#include "core/GraphMorphism.hpp"

#include <algorithm>
#include <array>
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

using RenderGraphMorphism = GraphMorphism<RenderGraphData, RenderGraph::GraphT>;

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
		const PassPort* found = actual.Find(port.name, port.direction);
		if (!found || !(*found == port))
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
		{"geometry/gObjectID",         AttachmentName::IDBuffer},
		{"geometry/depth",             AttachmentName::Depth},
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

/// @brief The policy the service's permanent morphism applies.
RenderGraphMorphism::Policy MakePolicy(PassFactory& factory, const IPortSource& portCatalog)
{
	RenderGraphMorphism::Policy policy;

	policy.createNode = [&factory, &portCatalog](RenderGraph::GraphT& target,
	                               const RenderGraphData::NodeType& node) -> GpuNode*
	{
		const PassPorts* ports = portCatalog.FindPassPorts(node.data.pass);
		if (!ports)
		{
			return nullptr;
		}
		const std::optional<int> shaderId = node.data.shader
			? std::optional<int>(node.data.shader->GetObjectID()) : std::nullopt;
		auto owned = factory.Acquire(node.data.pass, node.name, shaderId);
		Pass* pass = owned ? owned.get() : factory.Find(node.data.pass);
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
		}
		return gpu;
	};

	policy.createOutput = [](GpuNode& node, const RenderGraphData::SocketOutT& port)
		-> GpuOutput*
	{
		return FindOutput(node, port.name);
	};

	policy.createInput = [](GpuNode& node, const RenderGraphData::SocketInT& port)
		-> GpuInput*
	{
		return FindInput(node, port.name);
	};

	return policy;
}

} // anonymous namespace

struct GraphUploader::Impl
{
	Impl(PassFactory& passFactory, const IPortSource& ports,
	     std::shared_ptr<const RenderGraphData> builtInGraph)
		: factory(passFactory)
		, portCatalog(ports)
		, morphism(MakePolicy(passFactory, ports))
		, defaultGraph(std::move(builtInGraph))
	{
	}

	PassFactory&                          factory;
	const IPortSource&                    portCatalog;
	RenderGraphMorphism                   morphism;   ///< Permanent, reused per document.
	RenderGraphData                       source;     ///< Owned topology used by the morphism.
	std::shared_ptr<const RenderGraphData> defaultGraph;
	std::shared_ptr<const RenderGraphData> document;
	bool                                  newDocument = false;
};

GraphUploader::GraphUploader(PassFactory& factory, const IPortSource& portCatalog,
                             std::shared_ptr<const RenderGraphData> defaultGraph)
	: m_impl(std::make_unique<Impl>(factory, portCatalog, std::move(defaultGraph)))
{
	if (!m_impl->defaultGraph)
	{
		throw std::invalid_argument("GraphUploader requires a default graph");
	}
	SetPending(CompileRenderGraphRequest{});
}

GraphUploader::~GraphUploader() = default;

void GraphUploader::SetPending(const CompileRenderGraphRequest& request)
{
	auto document = request.graph ? request.graph : m_impl->defaultGraph;

	const bool changed = !m_impl->document || m_impl->document->Hash() != document->Hash();
	// Equivalent topology may carry different resource owners after project load.
	// Replace the publication even when no topology recompilation is needed.
	m_impl->document = std::move(document);
	m_impl->newDocument = m_impl->newDocument || changed;
}

bool GraphUploader::ConsumeNewDocument()
{
	const bool consumed = m_impl->newDocument;
	m_impl->newDocument = false;
	return consumed;
}

bool GraphUploader::HasDocument() const
{
	return static_cast<bool>(m_impl->document);
}

std::uint64_t GraphUploader::DocumentHash() const
{
	return m_impl->document ? m_impl->document->Hash() : 0;
}

GraphUploadResult GraphUploader::Compile(RenderCache& cache)
{
	GraphUploadResult result;

	if (!m_impl->document)
	{
		result.errors.push_back({{}, {}, "no render graph document"});
		return result;
	}

	// The morphism maps an owned copy: its images point into the source, which
	// must outlive the mapping, while the Editor's request stays immutable.
	m_impl->source = CloneGraph(*m_impl->document);

	for (const CpuGraphDiagnostic& diagnostic : ValidateCpuGraph(m_impl->source, m_impl->portCatalog))
	{
		result.errors.push_back({diagnostic.node, diagnostic.port, diagnostic.message});
	}
	if (!result.errors.empty())
	{
		return result;
	}

	// Only the UID crosses from a live CPU shader reference into GPU mapping.
	for (const auto& node : m_impl->source.nodes)
	{
		if (!node->data.shader)
		{
			continue;
		}
		try
		{
			const int shaderId = node->data.shader->GetObjectID();
			const auto selected = m_impl->factory.ShaderFor(node->data.pass, shaderId);
			const auto expected = m_impl->factory.ShaderFor(node->data.pass);
			if (!selected || !expected)
			{
				continue;
			}
			if (!SamePorts(selected->Ports(), expected->Ports()))
			{
				result.errors.push_back({node->name, {}, "uploaded shader changes the built-in port contract"});
			}
			const std::array<const ShaderGPU*, 1> actualStages = {selected.get()};
			const std::array<const ShaderGPU*, 1> expectedStages = {expected.get()};
			if (ReflectDescriptorLayout(actualStages).bindings != ReflectDescriptorLayout(expectedStages).bindings)
			{
				result.errors.push_back({node->name, {}, "uploaded shader changes the descriptor contract"});
			}
			if (PushConstantRangeFromShaders(actualStages, vk::ShaderStageFlagBits::eAll).size
				!= PushConstantRangeFromShaders(expectedStages, vk::ShaderStageFlagBits::eAll).size)
			{
				result.errors.push_back({node->name, {}, "uploaded shader changes the push-constant contract"});
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

	RenderGraph::GraphT target;
	try
	{
		target = m_impl->morphism.Map(m_impl->source);
	}
	catch (const std::exception& error)
	{
		result.errors.push_back({{}, {}, std::string("pass creation failed: ") + error.what()});
		return result;
	}
	for (const std::string& problem : m_impl->morphism.Validate().problems)
		result.errors.push_back({{}, {}, problem});

	if (!result.errors.empty())
		return result;

	// Plan the logical cache from the document's tags only after the mapping
	// succeeded: a failed upload leaves the previous plans and graph in place.
	cache.CleanScreenSpace();
	cache.ClearResourcePlans();

	for (const auto& node : m_impl->source.nodes)
	{
		for (const auto& output : node->outputs)
		{
			if (output->data.tag)
				cache.PlanResource(ResourceKey(node->name, output->name), *output->data.tag);
		}
	}

	for (const auto& node : m_impl->source.nodes)
	{
		for (const auto& output : node->outputs)
		{
			if (!output->data.tag)
				continue;
			const std::string key = ResourceKey(node->name, output->name);
			if (const std::optional<AttachmentName> legacy = LegacyAttachmentFor(key))
				cache.AliasResourceToAttachment(*legacy, key);
		}
	}

	result.fxaaPresent = std::any_of(
		m_impl->source.nodes.begin(), m_impl->source.nodes.end(),
		[](const auto& node) { return node->data.pass == "FXAAPass"; });

	auto graph = std::make_unique<RenderGraph>();
	graph->Adopt(std::move(target));

	try
	{
		graph->Compile();
	}
	catch (const std::exception& error)
	{
		result.errors.push_back({{}, {}, error.what()});
		return result;
	}

	result.graph = std::move(graph);
	return result;
}

} // namespace neurus
