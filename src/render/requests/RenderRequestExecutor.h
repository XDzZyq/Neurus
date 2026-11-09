/**
 * @file RenderRequestExecutor.h
 * @brief Executes RenderRequests against the renderer's GPU resources.
 *
 * The request handlers that used to live in the Editor (upload a mesh, allocate
 * a light's shadow maps, regenerate IBL, rewrite the lighting SSBOs, warm a
 * per-mesh pipeline, drop the outgoing scene) move here, off the Editor and onto
 * the thread that submits. DeferredRenderer owns one and delegates to it from
 * the per-frame drain.
 *
 * This is the testable seam. DeferredRenderer cannot be constructed without a
 * window surface, so its request handling could not be exercised headless; the
 * executor needs only a frame drain, a RenderCache, an UploadManager and a
 * GeometryPass, all of which a GPU test fixture already builds offscreen.
 *
 * It handles the GPU kinds only. `ResetAccumulationRequest` and
 * `RedrawOverlaysRequest` change renderer state instead of resources, so the
 * owning `RenderRequestProcessor` intercepts them before Execute(); the
 * constrained template makes that split a type rule rather than a convention.
 */

#pragma once

#include "RenderRequests.h"

#include <cstdint>
#include <functional>
#include <type_traits>

namespace neurus {

class RenderCache;
class UploadManager;
class GeometryPass;

/**
 * @brief Runs a RenderRequest against the renderer's cache and passes.
 *
 * Borrows everything it touches. The UploadManager and GeometryPass pointers may
 * be null - the renderer is wired up in stages and a request whose collaborator
 * is absent is logged once and skipped rather than crashing.
 *
 * A handler that frees or replaces memory a recorded frame may still read calls
 * the injected frame drain before it does so: every frame fence has to be
 * waited. Draining the frames is the strongest wait such a handler needs -
 * uploads and screenshots wait their own submits, so nothing else can still be
 * running against the resource being replaced. A mesh or environment entry that
 * is replaced or removed is the exception: the cache installs a fresh refcounted
 * table (shared_ptr<const MeshTable>/EnvironmentTable) and each submitted frame
 * holds the table it drew with until its fence, so nothing is freed under it and
 * no drain is needed. (A mesh upload over a resident id is a no-op, so a mesh
 * entry only goes away with the scene.) Light-SSBO growth drains only when a
 * kind outgrows its allocation - a first allocation destroys nothing.
 */
class RenderRequestExecutor
{
public:
	/**
	 * @brief Constructs the executor over the renderer's borrowed resources.
	 *
	 * @param drainFrames         Waits every in-flight frame fence; called by the
	 *                            handlers that free or replace a resource that is
	 *                            not refcounted (a replaced pipeline, light-SSBO
	 *                            growth, the scene drop), never by a mesh or
	 *                            environment entry that is replaced or removed.
	 * @param cache               Render cache holding the resource maps.
	 * @param uploader            Upload service, or null until the Application
	 *                            wires one in via SetUploadManager().
	 * @param geometryPass        Pass owning the per-mesh pipeline build, or null.
	 */
	RenderRequestExecutor(std::function<void()> drainFrames,
	                      RenderCache& cache,
	                      UploadManager* uploader,
	                      GeometryPass* geometryPass);

	~RenderRequestExecutor() = default;

	// Non-copyable, non-movable (holds references to borrowed objects).
	RenderRequestExecutor(const RenderRequestExecutor&)            = delete;
	RenderRequestExecutor& operator=(const RenderRequestExecutor&) = delete;
	RenderRequestExecutor(RenderRequestExecutor&&)                 = delete;
	RenderRequestExecutor& operator=(RenderRequestExecutor&&)      = delete;

	/**
	 * @brief Execute one GPU request, dispatched on its static type.
	 *
	 * Deliberately not a `RenderRequest` overload: the processor has already
	 * peeled the two renderer-state kinds, so a state request is a compile
	 * error here rather than a silent no-op.
	 */
	template <typename TRequest>
	    requires(!std::is_same_v<TRequest, ResetAccumulationRequest> &&
	             !std::is_same_v<TRequest, RedrawOverlaysRequest>)
	void Execute(const TRequest& request)
	{
		handle(request);
	}

	/** @brief Set the upload service once the Application has created it. */
	void SetUploadManager(UploadManager* uploader) { m_uploader = uploader; }

private:
	// --- Per-kind handlers (resolved by Execute's std::visit) ---
	void handle(const UploadMeshRequest& request);
	void handle(const CreateLightGpuRequest& request);
	void handle(const UploadEnvironmentRequest& request);
	void handle(const UpdateLightingRequest& request);
	void handle(const UpdateLightRequest& request);
	void handle(const EnsurePipelineRequest& request);
	void handle(const DropSceneResourcesRequest& request);

	// --- Borrowed objects ---
	std::function<void()> m_drainFrames;
	RenderCache&          m_cache;
	UploadManager*        m_uploader      = nullptr;
	GeometryPass*         m_geometryPass  = nullptr;

	// --- Latched "collaborator missing" reports (log once, not once per drain) ---
	bool m_reportedNullUploaderMesh = false;
	bool m_reportedNullUploaderLight = false;
	bool m_reportedNullUploaderEnv = false;
	bool m_reportedNullGeometryPass = false;
};

} // namespace neurus
