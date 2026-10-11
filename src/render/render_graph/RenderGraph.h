/**
 * @file RenderGraph.h
 * @brief DAG orchestrator that owns render-pass nodes and their image edges.
 *
 * Nodes are built on top of the template `Graph<AttachmentName, PassEntry>`
 * from `core/Graph.h`, with one named socket per resource declared in the
 * pass's `PassIO`. The graph consumes passes through that plain POD only, so
 * passes stay unaware of the RenderGraph type.
 */

#pragma once

#include "../GPUProfiler.h"
#include "../ProfilingData.h"
#include "../RenderCache.h"
#include "../passes/Pass.h"
#include "core/Graph.h"

#include <vulkan/vulkan_raii.hpp>

#include <vector>

namespace neurus {

/**
 * @brief Owns render-graph nodes + wires them into an execution DAG.
 *
 * Non-copyable. AddPass and Connect return non-owning pointers whose
 * lifetime matches the RenderGraph itself.
 */
class RenderGraph
{
public:
	/**
	 * @brief Payload stored on each node in the underlying core Graph<>.
	 *
	 * The graph caches the pass's `PassIO` at registration time so
	 * Compile() and Connect() do not need to re-invoke `Pass::GetIO()`.
	 */
	struct PassEntry
	{
		Pass*  pass = nullptr; ///< Provides Record().
		PassIO io;             ///< Cached I/O declaration returned by pass->GetIO().
	};

	using GraphT = Graph<AttachmentName, PassEntry>;
	using NodeT  = typename GraphT::NodeType;

	RenderGraph() = default;
	~RenderGraph() = default;
	RenderGraph(const RenderGraph&)            = delete;
	RenderGraph& operator=(const RenderGraph&) = delete;

	/**
	 * @brief Register a pass with the graph and materialize its sockets
	 *        from `pass->GetIO()`.
	 * @param pass  Non-null pointer to the pass instance.
	 * @return Non-owning node handle; ownership stays with the RenderGraph.
	 * @throws std::runtime_error if `pass` is null.
	 */
	NodeT* AddPass(Pass* pass);

	/**
	 * @brief Wire `producer`'s output for `resource` into `consumer`'s
	 *        matching input.
	 * @return false if either endpoint has not declared that resource.
	 * @note   Self-loops and duplicate edges are rejected by the underlying
	 *         core Graph<> and surface here as `false`.
	 */
	bool Connect(NodeT* producer, AttachmentName resource, NodeT* consumer);

	/**
	 * @brief Validate connectivity and cache the topological execution order.
	 *
	 * Inputs without an in-graph producer are treated as external and are not
	 * an error; cycles surface as `std::runtime_error`.
	 */
	void Compile();

	/**
	 * @brief Walk the compiled order and invoke each pass's Record(), driving
	 *        GPU + CPU profiling around every pass.
	 *
	 * Clears and repopulates @p frameProfile.passes in execution order and
	 * recomputes its aggregate counters; GPU times are left for the caller.
	 *
	 * @pre    Compile() has been called since the last topology change.
	 * @throws std::runtime_error if the graph has not been compiled.
	 */
	void Execute(vk::CommandBuffer cmd,
	             RenderCache& cache,
	             const RenderContext& ctx,
	             GPUProfiler& profiler,
	             FrameProfile& frameProfile) const;

	/** @return Number of registered passes. */
	size_t PassCount() const { return m_graph.NodeCount(); }

	/** @return Cached execution order; empty if Compile() has not yet run. */
	const std::vector<NodeT*>& CompiledOrder() const { return m_compiled; }

	/** @brief Drop the cached order (call after edits before the next Compile). */
	void Reset();

	/**
	 * @brief Remove all nodes and clear the compiled order.
	 *
	 * Pass objects are non-owning and outlive the graph, so only the
	 * node/socket bookkeeping is discarded.
	 */
	void Clear();

private:
	GraphT               m_graph;
	std::vector<NodeT*>  m_compiled;
	bool                 m_isCompiled = false;
};

} // namespace neurus
