/**
 * @file GraphUploader.hpp
 * @brief Render-side service that maps the Editor's document onto pass instances.
 *
 * The renderer does not read the Editor's document type: this service owns the
 * document and its shader snapshots, the permanent `GraphMorphism` from that
 * document to the runtime graph, and the resource plan that follows from it.
 * `DeferredRenderer` only asks for a compiled graph and a verdict.
 *
 * Like `UploadManager`, the service is long-lived: constructed once with the
 * pass factory, it keeps its policy and its mapping across uploads and clears
 * only the images when a new document arrives.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace neurus {

class PassFactory;
class RenderCache;
class RenderGraph;
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
 * Non-copyable, non-movable (the policy keeps pointers to this service).
 */
class GraphUploader
{
public:
	/**
	 * @brief Binds the service to the renderer's pass instances.
	 * @param factory  Pass factory; borrowed and must outlive this service.
	 */
	explicit GraphUploader(PassFactory& factory);
	~GraphUploader();

	GraphUploader(const GraphUploader&)            = delete;
	GraphUploader& operator=(const GraphUploader&) = delete;
	GraphUploader(GraphUploader&&)                 = delete;
	GraphUploader& operator=(GraphUploader&&)      = delete;

	/**
	 * @brief Replaces the current document and snapshots with a published one.
	 *
	 * The request is copied in; the caller may drop its own reference.
	 */
	void SetPending(const CompileRenderGraphRequest& request);

	/**
	 * @brief True once for every published request.
	 *
	 * The frame consumes the flag, so a document is compiled exactly once even
	 * when the pipeline signature also changed.
	 */
	bool ConsumeNewDocument();

	/// @brief True once a document has been published.
	bool HasDocument() const;

	/// @brief Topology hash of the current document; 0 when none.
	std::uint64_t DocumentHash() const;

	/// @brief Identity and version hash of the current shader snapshots.
	std::uint64_t ShaderHash() const;

	/**
	 * @brief Compiles the current document into a candidate runtime graph.
	 *
	 * Validates the document against the factory's ports, maps it through the
	 * permanent morphism, plans the cache's images from the document's tags, and
	 * adopts and compiles the runtime graph. On failure the result names every
	 * problem and carries no graph.
	 *
	 * @param fxaa   Whether the FXAA condition is on; the node is pruned otherwise.
	 * @param cache  Resource cache whose plans follow the document.
	 */
	GraphUploadResult Compile(bool fxaa, RenderCache& cache);

private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};

} // namespace neurus
