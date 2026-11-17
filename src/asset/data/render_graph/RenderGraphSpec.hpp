/**
 * @file RenderGraphSpec.hpp
 * @brief Vulkan-free document describing the scene render graph.
 *
 * Nodes are pass instances; edges are producer-port to consumer-port links.
 * Output-port metadata is user data: a fragment output declares only its GLSL
 * type, and a compute image may omit its format, so format, usage, extent and
 * layer policy live on the node's output tags rather than in the shader.
 *
 * The document is owned by the Editor, persisted in the project and handed to
 * the renderer as a compile request. It is deliberately not part of
 * RenderConfig, which is copied into every published frame snapshot.
 */

#pragma once

#include "asset/data/render_graph/GraphOutputTag.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/map.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

namespace neurus {

/**
 * @brief One pass instance in the document.
 *
 * @c pass is the pass kind the renderer's registry resolves; @c outputs is
 * keyed by output-port name, and a tag entry is only meaningful for a port the
 * shader really declares.
 */
struct GraphNodeSpec
{
	std::string                            id;
	std::string                            pass;
	std::map<std::string, GraphOutputTag>  outputs;

	bool operator==(const GraphNodeSpec&) const = default;

	/**
	 * @brief Serializes the node id, pass kind and output tags.
	 * @tparam Archive Cereal archive type.
	 * @param ar Archive to read or write.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("id", id),
		   cereal::make_nvp("pass", pass),
		   cereal::make_nvp("outputs", outputs));
	}
};

/**
 * @brief One producer-port to consumer-port edge.
 */
struct GraphEdgeSpec
{
	std::string from;
	std::string fromPort;
	std::string to;
	std::string toPort;

	bool operator==(const GraphEdgeSpec&) const = default;

	/**
	 * @brief Serializes the edge endpoints.
	 * @tparam Archive Cereal archive type.
	 * @param ar Archive to read or write.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("from", from),
		   cereal::make_nvp("fromPort", fromPort),
		   cereal::make_nvp("to", to),
		   cereal::make_nvp("toPort", toPort));
	}
};

/**
 * @brief The scene render graph document.
 */
struct RenderGraphSpec
{
	std::vector<GraphNodeSpec> nodes;
	std::vector<GraphEdgeSpec> edges;

	/**
	 * @brief Finds a node by id.
	 * @return Non-owning pointer, or nullptr when the id is absent.
	 */
	const GraphNodeSpec* FindNode(std::string_view id) const;
	GraphNodeSpec*       FindNode(std::string_view id);

	/**
	 * @brief Finds a node's index by id.
	 * @return Index into @c nodes, or std::nullopt when absent.
	 */
	std::optional<size_t> IndexOf(std::string_view id) const;

	bool operator==(const RenderGraphSpec&) const = default;

	/**
	 * @brief Serializes nodes and edges.
	 * @tparam Archive Cereal archive type.
	 * @param ar Archive to read or write.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("nodes", nodes),
		   cereal::make_nvp("edges", edges));
	}
};

/**
 * @brief Builds today's scene graph as a document.
 *
 * Geometry, shadow depth, shadow intensity, SSAO, lighting, compose and FXAA,
 * with the edges the renderer's hard-coded graph wired before the document
 * existed. The default is the seed an old project falls back to.
 */
RenderGraphSpec MakeDefaultRenderGraphSpec();

/**
 * @brief Drops the FXAA node and every edge that names it when @p fxaa is false.
 *
 * FXAA is the only conditional node in Stage 1; conditions are hard-coded until
 * the Render Graph Editor can author them.
 */
RenderGraphSpec PruneConditionalNodes(const RenderGraphSpec& spec, bool fxaa);

/**
 * @brief Stable 64-bit FNV-1a hash of ids, pass kinds, tags and edges.
 *
 * Node output tags are fed in map order and edges in document order, so the
 * hash changes with topology, pass selection and resource declarations.
 */
std::uint64_t RenderGraphSpecHash(const RenderGraphSpec& spec);

} // namespace neurus
