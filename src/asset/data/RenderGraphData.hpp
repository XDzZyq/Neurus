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
 * objects (`MeshData`, `ImageData`, `ShaderData`): plain state plus the
 * operations that belong to it. Its saved form lives with the project component
 * that writes it (`asset/components/GraphComponent.hpp`).
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
