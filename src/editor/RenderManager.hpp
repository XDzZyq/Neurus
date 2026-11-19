/**
 * @file RenderManager.hpp
 * @brief Editor-owned render state: the graph document, its shaders and the config.
 *
 * The Editor tells the renderer what to draw through two values: `RenderConfig`
 * (copied into every frame snapshot) and `RenderGraphData` (published as a
 * compile request). This service owns both, plus the pooled pass shaders the
 * document's sockets mirror and the saved record a project load hands over, so
 * the "graph, shaders and snapshots stay in step" invariant has one owner.
 *
 * It is a state service, not a controller: it does not record operations, emit
 * events or mark the project dirty. Controllers and the Editor write through
 * `Config()`, `SetGraph()` and `Restore()`, and the Editor owns the undo and
 * dirty policy around those writes.
 */

#pragma once

#include "asset/components/GraphComponent.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "core/RenderConfig.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace neurus {

class PassShaders;
class RenderRequestSink;
class ResourceManager;

/**
 * @brief Owns the render graph document, its pass shaders and the render config.
 *
 * Non-copyable, non-movable (borrows the pool and the request sink).
 */
class RenderManager
{
public:
	/**
	 * @brief Binds the service to the Editor's pool and request sink.
	 * @param resources  App-scoped pool; must outlive this service.
	 * @param sink       Renderer request sink, or null for a headless editor.
	 */
	RenderManager(ResourceManager& resources, RenderRequestSink* sink);
	~RenderManager();

	RenderManager(const RenderManager&)            = delete;
	RenderManager& operator=(const RenderManager&) = delete;
	RenderManager(RenderManager&&)                 = delete;
	RenderManager& operator=(RenderManager&&)      = delete;

	/** @brief Restores default render settings without changing or publishing the graph. */
	void ResetConfig();

	/**
	 * @brief Restores the default graph and its shader bindings, preserving render settings.
	 *
	 * Pass shaders are reseeded or adopted from the current pool, and the graph
	 * is published. Missing legacy graph records use this path after config load.
	 */
	void ResetGraph();

	/**
	 * @brief Applies a saved document after a project load.
	 *
	 * The pool must already hold the project's shaders; `PassShaders` adopts them
	 * by name, so a load never duplicates its shaders. The config is left as the
	 * project restored it.
	 *
	 * @param record  Saved document.
	 */
	void Restore(const project::RenderGraphRecord& record);

	/**
	 * @brief Replaces the document and publishes it.
	 *
	 * Node shader ids are re-seeded when they are unset or stale.
	 */
	void SetGraph(RenderGraphData graph);

	RenderConfig& Config() { return m_config; }
	const RenderConfig& Config() const { return m_config; }

	/// @brief The live document; empty until ResetGraph/Restore/SetGraph.
	const RenderGraphData& Graph() const { return m_graph; }

	/// @brief Validates the document, snapshots its shaders and posts the request.
	void Publish();

	/// @brief Record read from a project, applied by the Editor's FinishLoad.
	std::optional<project::RenderGraphRecord>& PendingRecord() { return m_pending; }

	/// @brief Problems from the last seed, load and validation pass.
	const std::vector<std::string>& Problems() const { return m_problems; }

private:
	/// @brief Creates the pooled pass shaders, keeping the problems list current.
	void SeedShaders();

	/// @brief Gives every node whose shader is unset or stale its kind's shader.
	void AssignNodeShaders();

	ResourceManager&   m_resources;
	RenderRequestSink* m_sink;
	RenderConfig       m_config;
	RenderGraphData    m_graph;
	std::unique_ptr<PassShaders> m_passShaders;
	std::optional<project::RenderGraphRecord> m_pending;
	std::vector<std::string> m_problems;
};

} // namespace neurus
