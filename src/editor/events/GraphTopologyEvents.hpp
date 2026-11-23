#pragma once

#include "asset/data/render_graph/GraphEndpoint.hpp"
#include "asset/data/render_graph/GraphOutputTag.hpp"

#include <cstddef>
#include <optional>

namespace neurus::project { struct RenderGraphNodeRecord; }

namespace neurus {

// CPU document mutations shared by live requests and history replay. References are borrowed during Emit.
struct GraphNodeMembershipChanged
{
	int graphUid;
	const project::RenderGraphNodeRecord& node;
	std::size_t index;
	bool present;
};

struct GraphNodeRenamed
{
	int graphUid;
	const std::string& from;
	const std::string& to;
};

struct GraphProducerChanged
{
	int graphUid;
	const GraphEndpoint& destination;
	std::optional<GraphEndpoint> before, after;
};

struct GraphOutputTagChanged
{
	int graphUid;
	const GraphEndpoint& output;
	const std::optional<GraphOutputTag>& tag;
};

struct GraphNodeShaderChanged
{
	int graphUid;
	const std::string& node;
	int shaderUid;
};

} // namespace neurus
