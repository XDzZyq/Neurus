/**
 * @file RenderGraph.hpp
 * @brief DAG orchestrator that owns render-pass nodes and their port edges.
 *
 * Nodes are built on top of the template `Graph<ShaderPort, PassEntry>` from
 * `core/Graph.hpp`. A scene pass declares ports from its shader; an overlay
 * pass keeps its historical PassIO names and is treated as a legacy node whose
 * unconnected inputs are external.
 */

#pragma once

#include "../GPUProfiler.hpp"
#include "../ProfilingData.hpp"
#include "../RenderCache.hpp"
#include "../passes/Pass.hpp"
#include "asset/graph/PassPorts.hpp"
#include "core/Graph.hpp"
#include "PassBindings.hpp"
#include "PassResources.hpp"

#include <vulkan/vulkan_raii.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace neurus {

/**
 * @brief Owns render-graph nodes + wires them into an execution DAG.
 *
 * Non-copyable, movable. AddPass and Connect return non-owning pointers whose
 * lifetime matches the RenderGraph itself.
 */
class RenderGraph
{
public:
	/**
	 * @brief Payload stored on each node in the underlying core Graph<>.
	 *
	 * The graph caches the pass's `PassIO` and port table at registration time
	 * so Compile() and Connect() do not re-invoke `Pass::GetIO()`.
	 */
	struct PassEntry
	{
		std::string              id;               ///< Document node id, for resource keys.
		Pass*                    pass = nullptr;   ///< Provides Record().
		PassIO                   io;               ///< Cached I/O declaration.
		PassPorts                ports;            ///< Ports from the shader.
		std::vector<std::string> externalInputs;   ///< Declared out-of-graph inputs.
		bool                     legacyExternal = false; ///< PassIO-only overlay node.

		/// @brief Image descriptors the graph writes before Record().
		std::vector<GraphImageBinding> imageBindings;
	};

	using GraphT      = Graph<ShaderPort, PassEntry>;
	using NodeT       = typename GraphT::NodeType;
	using SocketInT   = typename GraphT::SocketInT;
	using SocketOutT  = typename GraphT::SocketOutT;

	RenderGraph() = default;
	~RenderGraph() = default;
	RenderGraph(const RenderGraph&)            = delete;
	RenderGraph& operator=(const RenderGraph&) = delete;
	RenderGraph(RenderGraph&&) noexcept        = default;
	RenderGraph& operator=(RenderGraph&&) noexcept = default;

	/**
	 * @brief Registers a pass and materializes its sockets.
	 *
	 * A pass with shaders (or declared outputs) gets port sockets; a pass with
	 * neither falls back to its PassIO names and allows external inputs.
	 *
	 * @param pass  Non-null pointer to the pass instance.
	 * @return Non-owning node handle; ownership stays with the RenderGraph.
	 * @throws std::runtime_error if `pass` is null.
	 */
	NodeT* AddPass(Pass* pass);

	/**
	 * @brief Registers a pass with a precomputed port table.
	 *
	 * The compile path already holds the table the validator used, so passing it
	 * here keeps the graph and the validator on exactly the same ports.
	 *
	 * @param pass            Non-null pointer to the pass instance.
	 * @param ports           Ports the compiler validated.
	 * @param externalInputs  Inputs the pass resolves outside the graph.
	 */
	NodeT* AddPass(Pass* pass, PassPorts ports,
	               std::vector<std::string> externalInputs = {},
	               std::string id = {});

	/**
	 * @brief Wires one named producer port to one named consumer port.
	 * @return false if either endpoint does not declare that port.
	 */
	bool Connect(NodeT* producer, std::string_view fromPort,
	             NodeT* consumer, std::string_view toPort);

	/**
	 * @brief Stage 1 compatibility overload for the overlay graph.
	 *
	 * Delegates to the port overload with `AttachmentNameToString(resource)`.
	 */
	bool Connect(NodeT* producer, AttachmentName resource, NodeT* consumer);

	/**
	 * @brief Validates that every input is connected or external, then caches
	 *        the topological execution order.
	 *
	 * @throws std::runtime_error naming the node and port on an unconnected input.
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
