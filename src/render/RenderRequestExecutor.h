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
 * executor needs only a device, a RenderCache, an UploadManager and a
 * GeometryPass, all of which a GPU test fixture already builds offscreen. The
 * one request DeferredRenderer keeps for itself is ResetAccumulationRequest,
 * which writes renderer state the executor does not own; the executor treats it
 * as a no-op so std::visit stays total and a stray reset is harmless.
 */

#pragma once

#include "RenderRequests.h"

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>

namespace neurus {

class RenderCache;
class UploadManager;
class GeometryPass;

/**
 * @brief Runs a RenderRequest on the renderer's device, cache and passes.
 *
 * Borrows everything it touches. The UploadManager and GeometryPass pointers may
 * be null - the renderer is wired up in stages and a request whose collaborator
 * is absent is logged once and skipped rather than crashing. Calls
 * device.waitIdle() directly, because it runs on the submitting thread: a
 * handler that frees or replaces a resource must drain first, and with
 * kMaxFramesInFlight == 1 that is enough (see DeferredRenderer::DrawFrame).
 */
class RenderRequestExecutor
{
public:
	/**
	 * @brief Constructs the executor over the renderer's borrowed resources.
	 *
	 * @param device              Logical device (borrowed, must outlive this).
	 * @param cache               Render cache holding the resource maps.
	 * @param uploader            Upload service, or null until the Application
	 *                            wires one in via SetUploadManager().
	 * @param geometryPass        Pass owning the per-mesh pipeline build, or null.
	 */
	RenderRequestExecutor(const vk::raii::Device& device,
	                      RenderCache& cache,
	                      UploadManager* uploader,
	                      GeometryPass* geometryPass);

	~RenderRequestExecutor() = default;

	// Non-copyable, non-movable (holds references to borrowed objects).
	RenderRequestExecutor(const RenderRequestExecutor&)            = delete;
	RenderRequestExecutor& operator=(const RenderRequestExecutor&) = delete;
	RenderRequestExecutor(RenderRequestExecutor&&)                 = delete;
	RenderRequestExecutor& operator=(RenderRequestExecutor&&)      = delete;

	/** @brief Execute one request, dispatched on its active variant alternative. */
	void Execute(const RenderRequest& request);

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
	void handle(const ResetAccumulationRequest& request);

	// --- Borrowed objects ---
	const vk::raii::Device& m_device;
	RenderCache&            m_cache;
	UploadManager*          m_uploader      = nullptr;
	GeometryPass*           m_geometryPass  = nullptr;

	// --- Latched "collaborator missing" reports (log once, not once per drain) ---
	bool m_reportedNullUploaderMesh = false;
	bool m_reportedNullUploaderLight = false;
	bool m_reportedNullUploaderEnv = false;
	bool m_reportedNullGeometryPass = false;
};

} // namespace neurus
