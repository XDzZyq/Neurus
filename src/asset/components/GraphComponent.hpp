/**
 * @file GraphComponent.hpp
 * @brief Project component that stores the scene render graph document.
 *
 * The live document is a `Graph` whose nodes own their sockets and whose edges
 * are pointers, so it cannot be archived directly. This header owns the flat
 * record that can: the `RenderGraph*Record` types are the `proj_render_graph`
 * schema, and `ToRecord`/`FromRecord` are the only crossing between it and the
 * live graph. Keeping the schema next to the component keeps the wire format
 * with the key and the save/load path.
 */

#pragma once

#include "asset/Serializable.hpp"
#include "asset/data/render_graph/GraphOutputTag.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/optional.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

namespace neurus
{
class IPortSource;
class RenderGraphData;
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
RenderGraphRecord ToRecord(const RenderGraphData& graph);

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
 */
RenderGraphData FromRecord(const RenderGraphRecord& record, const IPortSource& ports,
                           std::vector<std::string>& problems);

/**
 * @brief Saves and loads the render graph document under `proj_render_graph`.
 *
 * The live document cannot be serialized directly, so the component reads a
 * flat record and the Editor applies it once the pool is restored. A project
 * written before the document existed, or a corrupt node, leaves no pending
 * record and the Editor builds the built-in default.
 */
class GraphComponent : public Serializable
{
public:
	/**
	 * @brief Binds the component to the Editor's document slots.
	 * @param pending  Receives the record read from the file.
	 * @param graph    Live document, read on save.
	 */
	GraphComponent(std::optional<RenderGraphRecord>& pending, const RenderGraphData& graph);

	const char* Key() const noexcept override { return "proj_render_graph"; }
	void Save(cereal::JSONOutputArchive& ar) const override;
	void Load(cereal::JSONInputArchive& ar) override;

private:
	std::optional<RenderGraphRecord>* m_pending;
	const RenderGraphData*            m_graph;
};

} // namespace neurus::project
