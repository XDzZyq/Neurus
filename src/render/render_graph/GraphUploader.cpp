/**
 * @file GraphUploader.cpp
 * @brief Document-to-runtime-graph service: mapping, validation and planning.
 */

#include "render/render_graph/GraphUploader.hpp"

#include "render/RenderCache.hpp"
#include "render/render_graph/PassFactory.hpp"
#include "render/render_graph/RenderGraph.hpp"
#include "render/render_graph/ResourceTag.hpp"
#include "render_api/RenderRequests.hpp"

#include "asset/data/RenderGraphData.hpp"
#include "asset/data/render_graph/CpuGraphValidation.hpp"

#include "core/Graph.hpp"
#include "core/GraphMorphism.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <unordered_map>

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

/// @brief Dirty key over the published shader snapshots.
std::uint64_t ShaderSnapshotsHash(const std::vector<ShaderSnapshot>& shaders)
{
	constexpr std::uint64_t kOffset = 14695981039346656037ull;
	constexpr std::uint64_t kPrime  = 1099511628211ull;

	std::uint64_t hash = kOffset;
	const auto mix = [&hash](const std::uint64_t value)
	{
		hash ^= value;
		hash *= kPrime;
	};

	for (const ShaderSnapshot& snapshot : shaders)
	{
		mix(static_cast<std::uint64_t>(snapshot.shaderUid));
		mix(static_cast<std::uint64_t>(snapshot.version));
	}
	mix(shaders.size());
	return hash;
}

/// @brief The policy the service's permanent morphism applies.
RenderGraphMorphism::Policy MakePolicy(PassFactory& factory)
{
	RenderGraphMorphism::Policy policy;

	policy.createNode = [&factory](RenderGraph::GraphT& target,
	                               const RenderGraphData::NodeType& node) -> GpuNode*
	{
		Pass* pass = factory.Find(node.data.pass);
		const PassPorts* ports = factory.FindPassPorts(node.data.pass);
		if (!pass || !ports)
			return nullptr;

		GpuNode* gpu = RenderGraph::MakePassNode(
			target, pass, *ports, factory.ExternalInputs(node.data.pass), node.name);
		if (gpu)
		{
			// The runtime node keeps the document id so diagnostics and the
			// resource keys name the graph the user edited.
			gpu->name = node.name;
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
	explicit Impl(PassFactory& passFactory)
		: factory(passFactory)
		, morphism(MakePolicy(passFactory))
	{
	}

	PassFactory&                          factory;
	RenderGraphMorphism                   morphism;   ///< Permanent, reused per document.
	RenderGraphData                       source;     ///< The pruned document the morphism mapped.
	std::shared_ptr<const RenderGraphData> document;
	std::vector<ShaderSnapshot>           shaders;
	bool                                  newDocument = false;
};

GraphUploader::GraphUploader(PassFactory& factory)
	: m_impl(std::make_unique<Impl>(factory))
{
}

GraphUploader::~GraphUploader() = default;

void GraphUploader::SetPending(const CompileRenderGraphRequest& request)
{
	m_impl->document    = request.graph;
	m_impl->shaders     = request.shaders;
	m_impl->newDocument = true;
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

std::uint64_t GraphUploader::ShaderHash() const
{
	return ShaderSnapshotsHash(m_impl->shaders);
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

	for (const auto& node : m_impl->source.nodes)
	{
		if (node->data.shaderUid == 0)
			continue;

		const bool present = std::any_of(
			m_impl->shaders.begin(), m_impl->shaders.end(),
			[&node](const ShaderSnapshot& snapshot)
			{
				return snapshot.shaderUid == node->data.shaderUid;
			});
		if (!present)
		{
			result.errors.push_back(
				{node->name, {}, "no shader snapshot for uid "
				                 + std::to_string(node->data.shaderUid)});
		}
	}

	if (!result.errors.empty())
		return result;

	for (const CpuGraphDiagnostic& diagnostic :
	     ValidateCpuGraph(m_impl->source, m_impl->factory))
	{
		result.errors.push_back({diagnostic.node, diagnostic.port, diagnostic.message});
	}

	if (!result.errors.empty())
		return result;

	RenderGraph::GraphT target = m_impl->morphism.Map(m_impl->source);
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
