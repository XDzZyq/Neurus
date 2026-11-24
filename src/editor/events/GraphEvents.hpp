#pragma once

#include "asset/data/render_graph/GraphEndpoint.hpp"
#include "asset/data/render_graph/GraphOutputTag.hpp"

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neurus {

struct CpuGraphDiagnostic;

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
/// Releases a newly allocated resource if its creating edit fails to commit.
struct GraphCreationDiscarded { int uid; };

/// @brief Materializes the effective default graph as a project resource.
struct RenderGraphCreateRequested { std::string* error = nullptr; };

/**
 * @brief Adds or removes the authored graph from the project.
 *
 * The graph remains pooled while the project uses its default.
 */
struct RenderGraphMembershipChanged
{
	int uid;
	bool present;
};

} // namespace neurus
