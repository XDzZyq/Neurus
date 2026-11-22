#pragma once

#include <memory>

namespace neurus {

class RenderGraphData;

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
