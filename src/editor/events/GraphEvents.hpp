#pragma once

#include "asset/data/render_graph/RenderGraphTopology.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace neurus {

class RenderGraphData;
struct GraphEditState;
struct CpuGraphDiagnostic;
struct ShaderUnit;
enum class ShaderType;

/// Flushes staged writes before reading sockets. An optional sink collects a diagnostic.
struct RenderGraphFlushRequested { std::vector<CpuGraphDiagnostic>* diagnostics = nullptr; };
struct RenderGraphStageEditRequested
{
	int uid;
	ShaderType stage;
	ShaderUnit& candidate;
	bool fromStruct;
	bool& handled;
};
struct RenderGraphAddRequested { std::string kind, id; std::string* createdId; };
struct RenderGraphRemoveRequested { std::string node; };
struct RenderGraphRenameRequested { std::string node, name; };
struct RenderGraphConnectRequested { GraphEndpoint source, destination; };
struct RenderGraphDisconnectRequested { GraphEndpoint destination; };
struct RenderGraphTagRequested { GraphEndpoint output; GraphOutputTag tag; };
struct RenderGraphShaderRequested { std::string node; std::optional<int> uid; };
struct RenderGraphCreateShaderRequested { std::string node; int* uid; };
struct RenderGraphEditRestored { GraphEditState& state; bool forward; };

/// @brief Materializes the effective default graph as a project resource.
struct RenderGraphCreateRequested {};

/**
 * @brief Replays explicit graph creation during history traversal.
 *
 * Undo returns the detached graph and redo consumes it synchronously. The
 * history entry owns the detached resource while the project uses its default.
 */
struct RenderGraphCreationRestored
{
	int uid;
	bool present;
	std::shared_ptr<RenderGraphData>& detached;
};

} // namespace neurus
