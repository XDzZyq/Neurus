/**
 * @file GraphDocument.hpp
 * @brief Serialized form of the CPU render graph, and the built-in default.
 *
 * The in-memory document is a `Graph` of sockets and payloads, which cannot be
 * serialized directly: nodes own their sockets and the edges are pointers. The
 * record types here are the flat, archive-friendly form, and the two converters
 * are the only place the document crosses that boundary.
 *
 * Input sockets are derived from the node's shader on load, so a shader change
 * re-derives them. Output sockets and their tags are saved, because a tag is
 * user data no shader declares.
 */

#pragma once

#include "asset/data/render_graph/RenderGraphData.hpp"
#include "asset/data/render_graph/PortSource.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/optional.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

namespace neurus {

/**
 * @brief One saved output socket.
 */
struct CpuPortRecord
{
	std::string                   name;
	std::string                   type;
	std::uint32_t                 binding = 0;
	std::uint32_t                 location = 0;
	std::optional<GraphOutputTag> tag;

	bool operator==(const CpuPortRecord&) const = default;

	/**
	 * @brief Serializes the port.
	 * @tparam Archive Cereal archive type.
	 * @param ar Archive to read or write.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("name", name),
		   cereal::make_nvp("type", type),
		   cereal::make_nvp("binding", binding),
		   cereal::make_nvp("location", location),
		   cereal::make_nvp("tag", tag));
	}
};

/**
 * @brief One saved node: its document id, pass, shader, layout and outputs.
 */
struct CpuNodeRecord
{
	std::string                name;
	std::string                pass;
	int                        shaderUid = 0;
	float                      x = 0.0f;
	float                      y = 0.0f;
	std::vector<CpuPortRecord> outputs;

	bool operator==(const CpuNodeRecord&) const = default;

	/**
	 * @brief Serializes the node.
	 * @tparam Archive Cereal archive type.
	 * @param ar Archive to read or write.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("name", name),
		   cereal::make_nvp("pass", pass),
		   cereal::make_nvp("shader", shaderUid),
		   cereal::make_nvp("x", x),
		   cereal::make_nvp("y", y),
		   cereal::make_nvp("outputs", outputs));
	}
};

/**
 * @brief One saved connection: producer port to consumer port, by node name.
 */
struct CpuEdgeRecord
{
	std::string from;
	std::string fromPort;
	std::string to;
	std::string toPort;

	bool operator==(const CpuEdgeRecord&) const = default;

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
 * @brief The whole document in its saved form.
 */
struct CpuGraphRecord
{
	std::vector<CpuNodeRecord> nodes;
	std::vector<CpuEdgeRecord> edges;

	bool operator==(const CpuGraphRecord&) const = default;

	/**
	 * @brief Serializes the node and edge lists.
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
 * @brief Flattens a live document into its saved form.
 * @param graph  Document to read; it is not modified.
 */
CpuGraphRecord ToRecord(const RenderGraphData& graph);

/**
 * @brief Rebuilds a document from its saved form.
 *
 * Nodes and output sockets come from the record; input sockets are derived from
 * each node's pass kind through @p ports. Every refused connection, unknown node
 * or missing port appends a message to @p problems instead of throwing, so a
 * partly broken file still opens and validation can report the rest.
 *
 * @param record    Saved document.
 * @param ports     Pass-kind port source.
 * @param problems  Receives one message per recoverable inconsistency.
 */
RenderGraphData FromRecord(const CpuGraphRecord& record, const IPortSource& ports,
                          std::vector<std::string>& problems);

/**
 * @brief Builds the built-in seven-node scene graph.
 *
 * The default is the fallback when a project has no saved document, exactly like
 * `CreateDefaultScene`. Node shaders are left unset (uid 0); the Editor assigns
 * a pooled shader to each node after the resource pool is ready.
 *
 * @param ports  Pass-kind port source.
 * @throws std::runtime_error when a built-in pass kind has no ports.
 */
RenderGraphData MakeDefaultCpuGraph(const IPortSource& ports);

/**
 * @brief Drops conditional nodes whose condition is off, with their connections.
 *
 * A pass kind declares its condition in `PassKindInfo`; `FXAAPass` is off when
 * FXAA is disabled. Nodes of unknown kinds are left in place so validation can
 * report them.
 *
 * @param graph  Document to prune in place.
 * @param fxaa   Whether the FXAA condition is on.
 */
void ApplyConditions(RenderGraphData& graph, bool fxaa);

/**
 * @brief Stable hash of the topology-affecting document content.
 *
 * Covers node names, pass kinds, shader identities, output tags and every
 * connection. Editor layout (`x`, `y`) is excluded: moving a node must not
 * recompile the graph.
 *
 * @param graph  Document to hash.
 */
std::uint64_t CpuGraphHash(const RenderGraphData& graph);

} // namespace neurus
