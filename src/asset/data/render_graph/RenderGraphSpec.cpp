/**
 * @file RenderGraphSpec.cpp
 * @brief Default graph, FXAA pruning and stable hashing for RenderGraphSpec.
 */

#include "asset/data/render_graph/RenderGraphSpec.hpp"

#include <algorithm>
#include <utility>

namespace neurus {

namespace {

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime  = 1099511628211ull;

/**
 * @brief Feeds one string plus a separator into an FNV-1a accumulator.
 *
 * The separator keeps "ab" + "c" distinct from "a" + "bc".
 */
void HashString(std::uint64_t& hash, std::string_view text)
{
	for (const char ch : text)
	{
		hash ^= static_cast<unsigned char>(ch);
		hash *= kFnvPrime;
	}
	hash ^= 0xFFu;
	hash *= kFnvPrime;
}

/// @brief Feeds a tag byte so node and edge records cannot collide structurally.
void HashTag(std::uint64_t& hash, unsigned char tag)
{
	hash ^= tag;
	hash *= kFnvPrime;
}

GraphOutputTag MakeTag(const GraphFormat format, const GraphUsage usage,
                       const GraphExtent extent = GraphExtent::Render,
                       const GraphLayers layers = GraphLayers::Single)
{
	GraphOutputTag tag;
	tag.format = format;
	tag.usage  = usage;
	tag.extent = extent;
	tag.layers = layers;
	return tag;
}

} // anonymous namespace

const GraphNodeSpec* RenderGraphSpec::FindNode(const std::string_view id) const
{
	const auto index = IndexOf(id);
	return index ? &nodes[*index] : nullptr;
}

GraphNodeSpec* RenderGraphSpec::FindNode(const std::string_view id)
{
	const auto index = IndexOf(id);
	return index ? &nodes[*index] : nullptr;
}

std::optional<size_t> RenderGraphSpec::IndexOf(const std::string_view id) const
{
	for (size_t i = 0; i < nodes.size(); ++i)
	{
		if (nodes[i].id == id)
			return i;
	}
	return std::nullopt;
}

RenderGraphSpec MakeDefaultRenderGraphSpec()
{
	RenderGraphSpec spec;

	auto addNode = [&spec](std::string id, std::string pass,
	                       std::map<std::string, GraphOutputTag> outputs)
	{
		spec.nodes.push_back({std::move(id), std::move(pass), std::move(outputs)});
	};

	auto addEdge = [&spec](std::string from, std::string fromPort,
	                       std::string to, std::string toPort)
	{
		spec.edges.push_back({std::move(from), std::move(fromPort),
		                      std::move(to), std::move(toPort)});
	};

	addNode("geometry", "GeometryPass",
	        {
	            {"gPosition",         MakeTag(GraphFormat::Rgba16f,    GraphUsage::ColorSampled)},
	            {"gNormal",           MakeTag(GraphFormat::Rgba16f,    GraphUsage::ColorSampled)},
	            {"gAlbedo",           MakeTag(GraphFormat::Rgba8Srgb,  GraphUsage::ColorSampled)},
	            {"gMetallicRoughness", MakeTag(GraphFormat::Rgba8Unorm, GraphUsage::ColorSampled)},
	            {"gObjectID",         MakeTag(GraphFormat::R32Uint,    GraphUsage::ColorSampled)},
	            {"depth",             MakeTag(GraphFormat::D32Sfloat,  GraphUsage::DepthSampled)},
	        });

	addNode("shadow_depth", "ShadowDepthPass",
	        {
	            // Per-light cubemaps and sun maps are owned by LightGPU; the
	            // graph carries the dependency as a logical token.
	            {"shadow_depth", MakeTag(GraphFormat::None, GraphUsage::Logical)},
	        });

	addNode("shadow_intensity", "ShadowIntensityPass",
	        {
	            {"outputShadow", MakeTag(GraphFormat::R8Unorm, GraphUsage::StorageSampled,
	                                     GraphExtent::Render, GraphLayers::Lights)},
	        });

	addNode("ssao", "SSAOPass",
	        {
	            {"outputSSAO", MakeTag(GraphFormat::R8Unorm, GraphUsage::StorageSampled)},
	        });

	addNode("lighting", "LightingPass",
	        {
	            {"outputImage", MakeTag(GraphFormat::Rgba16f, GraphUsage::StorageSampled)},
	        });

	addNode("compose", "ComposePass",
	        {
	            {"outputComposed", MakeTag(GraphFormat::Rgba16f, GraphUsage::StorageSampled)},
	        });

	addNode("fxaa", "FXAAPass",
	        {
	            {"outputFXAA", MakeTag(GraphFormat::Rgba16f, GraphUsage::StorageSampled)},
	        });

	// Geometry fans out to SSAO, shadow intensity and lighting.
	addEdge("geometry", "gPosition", "ssao", "gPosition");
	addEdge("geometry", "gNormal", "ssao", "gNormal");
	addEdge("geometry", "gAlbedo", "ssao", "gAlpha");
	addEdge("geometry", "gPosition", "shadow_intensity", "gPosition");

	// The shadow bundle feeds both the cubemap and the sun-map variants.
	addEdge("shadow_depth", "shadow_depth", "shadow_intensity", "u_ShadowCube");
	addEdge("shadow_depth", "shadow_depth", "shadow_intensity", "u_SunShadowMap");

	addEdge("geometry", "gPosition", "lighting", "gPosition");
	addEdge("geometry", "gNormal", "lighting", "gNormal");
	addEdge("geometry", "gAlbedo", "lighting", "gAlbedo");
	addEdge("geometry", "gMetallicRoughness", "lighting", "gMetallicRoughness");
	addEdge("ssao", "outputSSAO", "lighting", "U_AO");
	addEdge("shadow_intensity", "outputShadow", "lighting", "U_ShadowArray");

	addEdge("lighting", "outputImage", "compose", "gHDRColor");
	addEdge("compose", "outputComposed", "fxaa", "gInput");

	return spec;
}

RenderGraphSpec PruneConditionalNodes(const RenderGraphSpec& spec, const bool fxaa)
{
	if (fxaa)
		return spec;

	RenderGraphSpec pruned;
	pruned.nodes.reserve(spec.nodes.size());
	for (const GraphNodeSpec& node : spec.nodes)
	{
		if (node.pass == "FXAAPass")
			continue;
		pruned.nodes.push_back(node);
	}

	pruned.edges.reserve(spec.edges.size());
	for (const GraphEdgeSpec& edge : spec.edges)
	{
		if (!pruned.FindNode(edge.from) || !pruned.FindNode(edge.to))
			continue;
		pruned.edges.push_back(edge);
	}

	return pruned;
}

std::uint64_t RenderGraphSpecHash(const RenderGraphSpec& spec)
{
	std::uint64_t hash = kFnvOffset;

	HashTag(hash, 0x01);
	HashString(hash, std::to_string(spec.nodes.size()));
	for (const GraphNodeSpec& node : spec.nodes)
	{
		HashTag(hash, 0x02);
		HashString(hash, node.id);
		HashString(hash, node.pass);
		HashString(hash, std::to_string(node.outputs.size()));
		for (const auto& [port, tag] : node.outputs)
		{
			HashTag(hash, 0x03);
			HashString(hash, port);
			HashTag(hash, static_cast<unsigned char>(tag.format));
			HashTag(hash, static_cast<unsigned char>(tag.usage));
			HashTag(hash, static_cast<unsigned char>(tag.extent));
			HashTag(hash, static_cast<unsigned char>(tag.layers));
		}
	}

	HashTag(hash, 0x04);
	HashString(hash, std::to_string(spec.edges.size()));
	for (const GraphEdgeSpec& edge : spec.edges)
	{
		HashTag(hash, 0x05);
		HashString(hash, edge.from);
		HashString(hash, edge.fromPort);
		HashString(hash, edge.to);
		HashString(hash, edge.toPort);
	}

	return hash;
}

} // namespace neurus
