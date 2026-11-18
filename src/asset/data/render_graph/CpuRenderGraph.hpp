/**
 * @file CpuRenderGraph.hpp
 * @brief The render graph document: a generic graph whose sockets are shader ports.
 *
 * This is the CPU-side document the Editor owns and edits. Nodes are pass
 * instances; their input and output sockets mirror the ports the node's shader
 * declares, and an output socket carries the resource declaration the renderer
 * plans its image from. Edges are ordinary `Graph` connections, so the document
 * has no string references between nodes: `Node::name` is the user-visible id and
 * the renderer resolves edges through sockets.
 *
 * The type is Vulkan-free and carries no renderer state. The render request
 * hands the renderer a copy, and its uploader maps every node and connection onto
 * a live GPU graph.
 */

#pragma once

#include "asset/data/render_graph/GraphOutputTag.hpp"
#include "asset/data/render_graph/PassPorts.hpp"
#include "core/Graph.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace neurus {

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

/// @brief The render graph document: sockets are shader ports, nodes are passes.
using CpuRenderGraph = Graph<CpuPort, CpuNode>;

} // namespace neurus
