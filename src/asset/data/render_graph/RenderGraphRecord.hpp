/** @file RenderGraphRecord.hpp
 *  @brief Portable node and edge records for a pooled graph or a legacy inline document.
 */
#pragma once

#include "asset/data/render_graph/GraphOutputTag.hpp"
#include "core/Serialize.hpp"

#include <cereal/types/optional.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neurus
{
class IPortSource;
class IResourceLookup;
class RenderGraphTopology;

/// Stores the image declaration on each serialized output socket.
template<class Archive>
void serialize(Archive& ar, GraphOutputTag& tag)
{
	ar(cereal::make_nvp("format", tag.format),
	   cereal::make_nvp("usage", tag.usage),
	   cereal::make_nvp("extent", tag.extent),
	   cereal::make_nvp("layers", tag.layers));
}
}

namespace neurus::project
{

/**
 * @brief One saved output socket.
 */
struct RenderGraphPortRecord
{
	std::string                   name;
	std::string                   type;
	std::uint32_t                 binding = 0;
	std::uint32_t                 location = 0;
	std::optional<GraphOutputTag> tag;

	bool operator==(const RenderGraphPortRecord&) const = default;

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
struct RenderGraphNodeRecord
{
	std::string                        name;
	std::string                        pass;
	int                                shaderUid = 0;
	float                              x = 0.0f;
	float                              y = 0.0f;
	std::vector<RenderGraphPortRecord> outputs;

	bool operator==(const RenderGraphNodeRecord&) const = default;

	/**
	 * @brief Serializes the node.
	 * @tparam Archive Cereal archive type.
	 * @param ar Archive to read or write.
	 */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("name", name),
		   cereal::make_nvp("pass", pass));
		if constexpr (Archive::is_loading::value)
			archive::OptionalField(ar, cereal::make_nvp("shader", shaderUid));
		else if (shaderUid != 0)
			ar(cereal::make_nvp("shader", shaderUid));
		ar(cereal::make_nvp("x", x),
		   cereal::make_nvp("y", y),
		   cereal::make_nvp("outputs", outputs));
	}
};

/**
 * @brief One saved connection: producer port to consumer port, by node name.
 */
struct RenderGraphEdgeRecord
{
	std::string from;
	std::string fromPort;
	std::string to;
	std::string toPort;

	bool operator==(const RenderGraphEdgeRecord&) const = default;

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
struct RenderGraphRecord
{
	std::vector<RenderGraphNodeRecord> nodes;
	std::vector<RenderGraphEdgeRecord> edges;

	bool operator==(const RenderGraphRecord&) const = default;

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
 * @brief Flattens a live document into the project's record.
 *
 * Node fields and output sockets with their tags are written; input sockets are
 * not, because a load derives them from the node's shader.
 *
 * @param graph  Document to read; it is not modified.
 */
RenderGraphRecord ToRecord(const RenderGraphTopology& graph);

/**
 * @brief Rebuilds a live document from a saved record.
 *
 * Nodes and output sockets come from the record; input sockets are derived from
 * each node's pass kind through @p ports. Every refused connection, unknown node
 * or missing port appends a message to @p problems instead of throwing, so a
 * partly broken file still opens and validation can report the rest.
 *
 * @param record    Saved document.
 * @param ports     Pass-kind port source.
 * @param problems  Receives one message per recoverable inconsistency.
 * @param resources Optional restored pool; missing or invalid shaders select defaults.
 */
RenderGraphTopology FromRecord(const RenderGraphRecord& record, const IPortSource& ports,
                               std::vector<std::string>& problems,
                               const IResourceLookup* resources = nullptr);

} // namespace neurus::project
