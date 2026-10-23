/**
 * @file RenderRequestExecutor.cpp
 * @brief RenderRequestExecutor implementation.
 */

#include "RenderRequestExecutor.h"

#include "RenderCache.h"
#include "UploadManager.h"

#include "resources/EnvironmentGPU.h"
#include "resources/LightGPU.h"
#include "resources/MeshGPU.h"
#include "resources/PipelineCache.h"

#include "passes/GeometryPass.h"

#include "asset/data/MeshData.h"
#include "core/Log.h"

#include <variant>

namespace neurus {

RenderRequestExecutor::RenderRequestExecutor(const vk::raii::Device& device,
                                             RenderCache& cache,
                                             UploadManager* uploader,
                                             GeometryPass* geometryPass)
	: m_device(device)
	, m_cache(cache)
	, m_uploader(uploader)
	, m_geometryPass(geometryPass)
{
}

void RenderRequestExecutor::Execute(const RenderRequest& request)
{
	// The lambda forwards to the matching private handle() overload; defined
	// here inside a member so it reaches the private handlers.
	std::visit([this](const auto& r) { handle(r); }, request);
}

// ---------------------------------------------------------------------------
// Mesh upload
// ---------------------------------------------------------------------------

void RenderRequestExecutor::handle(const UploadMeshRequest& request)
{
	// Already resident: posting twice is harmless by design, so this is the
	// common case, not an error.
	if (m_cache.GetMeshGPU(request.objectId)) return;

	if (!m_uploader)
	{
		if (!m_reportedNullUploaderMesh)
		{
			m_reportedNullUploaderMesh = true;
			NEURUS_ERR("[RenderRequestExecutor] UploadMeshRequest with no UploadManager - skipping");
		}
		return;
	}

	// A null payload is the same error UploadManager reports for a mesh with no
	// MeshData: name it and skip, never upload an empty slot.
	if (!request.data)
	{
		NEURUS_ERR("[RenderRequestExecutor] UploadMeshRequest for objectId="
		           << request.objectId << " has null MeshData - skipping");
		return;
	}

	auto meshGPU = m_uploader->UploadMeshData(*request.data);
	m_cache.UseMeshGPU(request.objectId, std::move(meshGPU));
	NEURUS_LOG("[RenderRequestExecutor] Uploaded MeshGPU for objectId=" << request.objectId);
}

// ---------------------------------------------------------------------------
// Light shadow-map allocation
// ---------------------------------------------------------------------------

void RenderRequestExecutor::handle(const CreateLightGpuRequest& request)
{
	// The use_shadow gate stays in the Editor (it reads the Scene); a request
	// only reaches here for a shadow-casting light. Resident is a no-op.
	if (m_cache.GetLightGPU(request.uid)) return;

	if (!m_uploader)
	{
		if (!m_reportedNullUploaderLight)
		{
			m_reportedNullUploaderLight = true;
			NEURUS_ERR("[RenderRequestExecutor] CreateLightGpuRequest with no UploadManager - skipping");
		}
		return;
	}

	auto lightGPU = m_uploader->UploadLight(request.uid, request.type);
	m_cache.UseLightGPU(request.uid, std::move(lightGPU));
	NEURUS_LOG("[RenderRequestExecutor] Created LightGPU for lightUID=" << request.uid);
}

// ---------------------------------------------------------------------------
// Environment upload / IBL regeneration
// ---------------------------------------------------------------------------

void RenderRequestExecutor::handle(const UploadEnvironmentRequest& request)
{
	// Resident and not asked to regenerate: the idempotent scene-upload path,
	// where regenerating cubemaps is the most expensive step to repeat.
	const bool resident = m_cache.GetEnvironmentGPU(request.objectId) != nullptr;
	if (resident && !request.regenerate) return;

	if (!m_uploader)
	{
		if (!m_reportedNullUploaderEnv)
		{
			m_reportedNullUploaderEnv = true;
			NEURUS_ERR("[RenderRequestExecutor] UploadEnvironmentRequest with no UploadManager - skipping");
		}
		return;
	}

	// Regenerating over a resident entry REPLACES it, destroying the old
	// cubemaps at once - the same hazard as a pipeline replace, so drain first.
	// This argument depends on kMaxFramesInFlight == 1 (see DrawFrame).
	if (resident)
		m_device.waitIdle();

	auto envGPU = m_uploader->UploadEnvironment(request.equirect);
	m_cache.UseEnvironmentGPU(request.objectId, std::move(envGPU));
	NEURUS_LOG("[RenderRequestExecutor] Uploaded EnvironmentGPU for objectId=" << request.objectId
	           << " (regenerate=" << (request.regenerate ? "yes" : "no") << ")");
}

// ---------------------------------------------------------------------------
// Lighting SSBO rewrite (full) / single-light rewrite
// ---------------------------------------------------------------------------

void RenderRequestExecutor::handle(const UpdateLightingRequest& request)
{
	m_cache.UpdateLighting(request.lights);
}

void RenderRequestExecutor::handle(const UpdateLightRequest& request)
{
	m_cache.UpdateLight(request.uid, request.light);
}

// ---------------------------------------------------------------------------
// Per-mesh pipeline warm
// ---------------------------------------------------------------------------

void RenderRequestExecutor::handle(const EnsurePipelineRequest& request)
{
	if (!m_geometryPass)
	{
		if (!m_reportedNullGeometryPass)
		{
			m_reportedNullGeometryPass = true;
			NEURUS_ERR("[RenderRequestExecutor] EnsurePipelineRequest with no GeometryPass - skipping");
		}
		return;
	}

	// A store that REPLACES an entry destroys the displaced Pipeline at once, so
	// drain first. Get ignores the version, GetPipeline does not: an entry under
	// the id that fails the version check is one a store would replace. A first
	// insert destroys nothing, so the drain is bounded to shader recompiles.
	// This argument depends on kMaxFramesInFlight == 1 (see DrawFrame).
	PipelineCache& pipelines = m_cache.GetPipelineCache();
	const bool replaces = pipelines.Get(request.shaderId) != nullptr
	                   && pipelines.GetPipeline(request.shaderId, request.version) == nullptr;
	if (replaces)
		m_device.waitIdle();

	m_geometryPass->EnsureMeshPipeline(m_cache, request);
}

// ---------------------------------------------------------------------------
// Drop the outgoing scene's resources
// ---------------------------------------------------------------------------

void RenderRequestExecutor::handle(const DropSceneResourcesRequest&)
{
	// Cached entries own vk::raii resources the last frame may still read, so
	// drain before releasing them. kMaxFramesInFlight == 1 makes the wait exact.
	m_device.waitIdle();
	m_cache.RemoveSceneResources();
	NEURUS_LOG("[RenderRequestExecutor] Dropped scene GPU resources");
}

// ---------------------------------------------------------------------------
// Accumulation reset (owned by DeferredRenderer; no-op here)
// ---------------------------------------------------------------------------

void RenderRequestExecutor::handle(const ResetAccumulationRequest&)
{
	// DeferredRenderer executes this one itself - it writes renderer state the
	// executor does not own. The no-op keeps Execute's std::visit total so the
	// executor can be handed any request kind without a crash.
}

// ---------------------------------------------------------------------------
// Overlay-only redraw (owned by DeferredRenderer; no-op here)
// ---------------------------------------------------------------------------

void RenderRequestExecutor::handle(const RedrawOverlaysRequest&)
{
	// DeferredRenderer intercepts this one too: it only sets the renderer's
	// "overlays stale" flag, which decides the next frame's work (see NextWork).
	// There is no GPU resource for the executor to touch - the overlay graph
	// re-runs the passes it already owns against the image already on the GPU.
	// The no-op keeps Execute's std::visit total.
}

} // namespace neurus
