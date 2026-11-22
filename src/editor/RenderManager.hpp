/**
 * @file RenderManager.hpp
 * @brief Editor-owned render state: the graph document, its shaders and the config.
 *
 * The Editor tells the renderer what to draw through two values: `RenderConfig`
 * (copied into every frame snapshot) and `RenderGraphTopology` (published as a
 * compile request). Stored config and graph pointers stay null when the project
 * selects defaults. Effective reads use cached built-ins without adding them to
 * the project's resource pool or serialization.
 *
 * It is a state service, not a controller: it does not record operations, emit
 * events or mark the project dirty. Controllers and the Editor write through
 * `EditConfig()`, `SetGraph()` and `FinishGraphLoad()`, and the Editor owns the undo and
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

class BuiltInRenderData;
class RenderRequestSink;
class ResourceManager;
class ShaderData;
struct CpuGraphDiagnostic;

/**
 * @brief Owns optional project render data and publishes topology plus owned GPU uploads.
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
	 * @param renderDefaults Shared CPU defaults, or null to create a local provider.
	 */
	RenderManager(ResourceManager& resources, RenderRequestSink* sink,
	              std::shared_ptr<const BuiltInRenderData> renderDefaults);
	~RenderManager();

	RenderManager(const RenderManager&)            = delete;
	RenderManager& operator=(const RenderManager&) = delete;
	RenderManager(RenderManager&&)                 = delete;
	RenderManager& operator=(RenderManager&&)      = delete;

	/** @brief Selects default settings by clearing the stored config; the graph is untouched. */
	void ResetConfig();

	/**
	 * @brief Selects the cached built-in graph without storing it, preserving render settings.
	 *
	 * Publishes a null document; Renderer resolves it through its built-in provider.
	 * Missing legacy graph records remain absent on the next save.
	 */
	void ResetGraph();

	/// @brief Drops the outgoing selection before deserializing a new project.
	void BeginGraphLoad();

	/// @brief Resolves pooled shader links and the active graph after the pool loads.
	void FinishGraphLoad();

	/// @brief Materializes the effective default once as a project-owned resource.
	std::shared_ptr<RenderGraphData> CreateGraph(bool publish = true);
	const BuiltInRenderData& Defaults() const { return *m_renderDefaults; }
	std::vector<CpuGraphDiagnostic> Diagnostics() const;

	/// @brief Replays membership of the resource retained by a graph creation operation.
	void RestoreCreatedGraph(const std::shared_ptr<RenderGraphData>& graph);
	void RemoveCreatedGraph(const std::shared_ptr<RenderGraphData>& graph, bool publish = true);

	/**
	 * @brief Replaces the document and publishes it.
	 *
	 * Missing shader references select built-ins without populating the pool.
	 */
	bool SetGraph(RenderGraphTopology graph);

	const RenderConfig& Config() const { return m_config ? *m_config : DefaultRenderConfig(); }
	RenderConfig& EditConfig();
	std::shared_ptr<RenderConfig>& ConfigData() { return m_config; }
	const std::shared_ptr<RenderConfig>& ConfigData() const { return m_config; }

	/// @brief Effective read-only graph; reading a default never materializes project data.
	const RenderGraphTopology& Graph() const;
	const std::shared_ptr<RenderGraphData>& GraphData() const { return m_graph; }

	/// @brief Uploads distinct selected shaders, then publishes cloned topology with shared references.
	void Publish();
	void UploadShader(const ShaderData& shader);

	/// Takes ownership of a pass shader created by an edit or restored from history.
	void RegisterShader(const std::shared_ptr<ShaderData>& shader);
	/// Releases a shader only after no pooled graph or mesh references it.
	std::shared_ptr<ShaderData> RemoveUnusedShader(int uid);

	/// @brief Whether an explicit graph node references this pooled shader.
	bool ReferencesShader(int uid) const;

	/// @brief Record read from a project, applied by the Editor's FinishLoad.
	std::optional<project::GraphSelection>& PendingRecord() { return m_pending; }

	/// @brief Problems from the last load and validation pass.
	const std::vector<std::string>& Problems() const { return m_problems; }

private:
	ResourceManager&   m_resources;
	RenderRequestSink* m_sink;
	std::shared_ptr<const BuiltInRenderData> m_renderDefaults;
	std::shared_ptr<RenderConfig> m_config;
	std::shared_ptr<RenderGraphData> m_graph;
	std::optional<project::GraphSelection> m_pending;
	std::vector<std::string> m_problems;
};

} // namespace neurus
