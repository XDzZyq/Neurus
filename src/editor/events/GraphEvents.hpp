#pragma once

#include "asset/data/render_graph/RenderGraphTopology.hpp"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neurus {

class RenderGraphData;
struct CpuGraphDiagnostic;
class ShaderData;

/// Reply slots and edit payloads are borrowed for synchronous dispatch. A
/// controller writes a failure to error when supplied and throws otherwise.
struct RenderGraphFlushRequested
{
	std::vector<CpuGraphDiagnostic>* diagnostics = nullptr;
	std::string* error = nullptr;
};

/// Validate the completed document and publish its changed resources once.
struct RenderGraphCommitRequested
{
	bool topologyChanged = false;
	std::set<int> shaderUids;
};

struct RenderGraphAddRequested
{
	std::string kind, id;
	std::string* createdId;
	std::string* error = nullptr;
};
struct RenderGraphRemoveRequested
{
	std::string node;
	std::string* error = nullptr;
};
struct RenderGraphRenameRequested
{
	std::string node, name;
	std::string* error = nullptr;
};
struct RenderGraphConnectRequested
{
	GraphEndpoint source, destination;
	std::string* error = nullptr;
};
struct RenderGraphDisconnectRequested
{
	GraphEndpoint destination;
	std::string* error = nullptr;
};
struct RenderGraphTagRequested
{
	GraphEndpoint output;
	GraphOutputTag tag;
	std::string* error = nullptr;
};
struct RenderGraphShaderRequested
{
	std::string node;
	std::optional<int> uid;
	std::string* error = nullptr;
};
struct RenderGraphCreateShaderRequested
{
	std::string node;
	int* uid;
	std::string* error = nullptr;
};
/// Undo retains a detached shader for redo, including across a save and reload.
struct PassShaderMembershipChanged
{
	int uid;
	bool present;
	std::shared_ptr<ShaderData>& detached;
};

/// @brief Materializes the effective default graph as a project resource.
struct RenderGraphCreateRequested { std::string* error = nullptr; };

/**
 * @brief Adds or removes the authored graph from the project.
 *
 * Undo returns the detached graph and redo consumes it synchronously. The
 * history entry owns the detached resource while the project uses its default.
 */
struct RenderGraphMembershipChanged
{
	int uid;
	bool present;
	std::shared_ptr<RenderGraphData>& detached;
};

} // namespace neurus
