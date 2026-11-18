/**
 * @file RenderGraphData.hpp
 * @brief The render graph document: a graph whose sockets are shader ports.
 *
 * This is the CPU-side document the Editor owns and edits. Nodes are pass
 * instances; their input and output sockets mirror the ports the node's shader
 * declares, and an output socket carries the resource declaration the renderer
 * plans its image from. Edges are ordinary `Graph` connections, so the document
 * has no string references between nodes: `Node::name` is the user-visible id and
 * the renderer resolves edges through sockets.
 *
 * The type is Vulkan-free and carries no renderer state, like the other data
 * objects (`MeshData`, `ImageData`, `ShaderData`): plain state plus its saved
 * form. The render request hands the renderer a copy, and its uploader maps every
 * node and connection onto a live GPU graph.
 *
 * @note Unlike the pooled data resources, this document is not a `UID` object:
 *       the Editor owns exactly one and its identity is the Editor's.
 */

#pragma once

#include "asset/data/render_graph/GraphOutputTag.hpp"
#include "asset/data/render_graph/PassPorts.hpp"
#include "core/Graph.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <cereal/cereal.hpp>
#include <cereal/types/optional.hpp>
#include <cereal/types/string.hpp>
#include <cereal/types/vector.hpp>

namespace neurus {

class IPortSource;

/**
 * @brief Socket payload of the CPU graph: one shader port.
 *
 * Mirrors @ref ShaderPort, plus the resource declaration an output owns. The
 * declaration is user data: a shader names a GLSL type, never an image format.
 * An input never carries one; @c tag is only meaningful for an output.
 */
struct CpuPort
{
	std::string    name;
	PortDirection  direction = PortDirection::Input;
	std::string    type;          ///< "sampler2D", "image2D", "vec4", "uint"
	uint32_t       binding = 0;   ///< set-0 binding; 0 for a fragment output
	uint32_t       location = 0;  ///< fragment output location; 0 otherwise
	std::optional<GraphOutputTag> tag;   ///< outputs only

	bool operator==(const CpuPort&) const = default;
};

/**
 * @brief Node payload of the CPU graph: one pass instance plus its editor state.
 *
 * The node's document id is `Graph::Node::name`, which the graph editor and the
 * script API use. A pass kind names a built-in factory entry; @c shaderUid is the
 * pooled shader the node draws with, and the position is graph-editor layout that
 * the renderer never reads.
 */
struct CpuNode
{
	std::string pass;
	int         shaderUid = 0;
	float       x = 0.0f;
	float       y = 0.0f;

	bool operator==(const CpuNode&) const = default;
};

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
	std::string                          name;
	std::string                          pass;
	int                                  shaderUid = 0;
	float                                x = 0.0f;
	float                                y = 0.0f;
	std::vector<RenderGraphPortRecord>   outputs;

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
 * @brief The render graph document: sockets are shader ports, nodes are passes.
 *
 * The document is the graph. Deriving rather than wrapping keeps the generic
 * `Graph` API (`nodes`, `AddNode`, `Connect`, `TopologicalSort`) and the
 * `CloneGraph`/`GraphMorphism` templates usable unchanged, while the type name
 * says what the data is.
 */
class RenderGraphData : public Graph<CpuPort, CpuNode>
{
public:
	using GraphT = Graph<CpuPort, CpuNode>;

	/**
	 * @brief Flattens the document into its saved form.
	 *
	 * Node fields and output sockets with their tags are written; input sockets
	 * are not, because a load derives them from the node's shader.
	 */
	RenderGraphRecord ToRecord() const;

	/**
	 * @brief Rebuilds a document from its saved form.
	 *
	 * Nodes and output sockets come from the record; input sockets are derived
	 * from each node's pass kind through @p ports. Every refused connection,
	 * unknown node or missing port appends a message to @p problems instead of
	 * throwing, so a partly broken file still opens and validation can report
	 * the rest.
	 *
	 * @param record    Saved document.
	 * @param ports     Pass-kind port source.
	 * @param problems  Receives one message per recoverable inconsistency.
	 */
	static RenderGraphData FromRecord(const RenderGraphRecord& record,
	                                  const IPortSource& ports,
	                                  std::vector<std::string>& problems);

	/**
	 * @brief Builds the built-in seven-node scene graph.
	 *
	 * The default is the fallback when a project has no saved document, exactly
	 * like `CreateDefaultScene`. Node shaders are left unset (uid 0); the Editor
	 * assigns a pooled shader to each node after the resource pool is ready.
	 *
	 * @param ports  Pass-kind port source.
	 * @throws std::runtime_error when a built-in pass kind has no ports.
	 */
	static RenderGraphData Default(const IPortSource& ports);

	/**
	 * @brief Drops conditional nodes whose condition is off, with their edges.
	 *
	 * A pass kind declares its condition in `PassKindInfo`; `FXAAPass` is off
	 * when FXAA is disabled. Nodes of unknown kinds are left in place so
	 * validation can report them.
	 *
	 * @param fxaa  Whether the FXAA condition is on.
	 */
	void ApplyConditions(bool fxaa);

	/**
	 * @brief Stable hash of the topology-affecting content.
	 *
	 * Covers node names, pass kinds, shader identities, output tags and every
	 * connection. Editor layout (`x`, `y`) is excluded: moving a node must not
	 * recompile the graph.
	 */
	std::uint64_t Hash() const;
};

} // namespace neurus
