/**
 * @file GraphUploader.hpp
 * @brief Render-side service that maps the Editor's document onto pass instances.
 *
 * Retains the published CPU document. Each compile maps it onto a runtime
 * graph and plans the resources declared by its output tags.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace neurus {

class IPortSource;
class PassFactory;
class RenderCache;
class RenderGraph;
class RenderGraphData;
struct CompileRenderGraphRequest;

/**
 * @brief One upload or document problem, named by node and port.
 *
 * Render-owned copy of a document diagnostic, so the renderer never has to
 * include the data layer's validation type.
 */
struct GraphDiagnostic
{
	std::string node;
	std::string port;
	std::string message;

	bool operator==(const GraphDiagnostic&) const = default;
};

/**
 * @brief Outcome of one compile.
 *
 * @c graph is null when @c errors is not empty; the caller keeps the previous
 * graph drawing.
 */
struct GraphUploadResult
{
	std::unique_ptr<RenderGraph> graph;
	bool                         fxaaPresent = false;
	std::vector<GraphDiagnostic> errors;
};

/**
 * @brief Maps published documents onto the pass factory's instances.
 *
 * Non-copyable, non-movable.
 */
class GraphUploader
{
public:
	/**
	 * @brief Binds the service to the renderer's pass instances.
	 * @param factory       Runtime pass factory (borrowed).
	 * @param portCatalog   Built-in port contracts (borrowed).
	 * @param defaultGraph  Nonnull built-in topology used for null graph requests.
	 */
	GraphUploader(PassFactory& factory, const IPortSource& portCatalog,
	              std::shared_ptr<const RenderGraphData> defaultGraph);

	GraphUploader(const GraphUploader&)            = delete;
	GraphUploader& operator=(const GraphUploader&) = delete;
	GraphUploader(GraphUploader&&)                 = delete;
	GraphUploader& operator=(GraphUploader&&)      = delete;

	/**
	 * @brief Replaces the current topology with a published document.
	 *
	 * The request is copied in; the caller may drop its own reference.
	 */
	void SetPending(const CompileRenderGraphRequest& request);

	/// @brief Cached hash of the published topology and selected shader UIDs.
	std::uint64_t DocumentHash() const { return m_documentHash; }

	/**
	 * @brief Compiles the current document into a candidate runtime graph.
	 *
	 * Validates the document against the port catalog, maps and compiles the
	 * runtime graph, and plans images from the document's tags. On failure the
	 * result carries diagnostics and no graph.
	 *
	 * @param cache  Resource cache whose plans follow the document.
	 */
	GraphUploadResult Compile(RenderCache& cache);

private:
	PassFactory& m_factory;
	const IPortSource& m_portCatalog;
	const std::shared_ptr<const RenderGraphData> m_defaultGraph;
	std::shared_ptr<const RenderGraphData> m_document;
	std::uint64_t m_documentHash = 0;
};

} // namespace neurus
