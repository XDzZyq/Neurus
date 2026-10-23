/**
 * @file RenderRequests.h
 * @brief GPU work the Editor asks the renderer to do, as owned values.
 *
 * The renderer is the only code that touches a VkQueue, the device-wide wait,
 * or the RenderCache's resource maps. Everything the Editor used to do to them
 * directly - upload a mesh, allocate a light's shadow maps, regenerate IBL,
 * rewrite the lighting SSBOs, warm a per-mesh pipeline, drop the outgoing
 * scene's resources - is posted instead as one of the requests below and
 * executed by DeferredRenderer at the top of DrawFrame, in posting order.
 *
 * Single ownership is the point, not tidiness. On MoltenVK the "transfer
 * queue" resolves to the graphics queue, so an upload submitted from one thread
 * while a frame is submitted from another is a race on the queue itself, which
 * no cache lock repairs. With every submit issued by the renderer, a render
 * thread later only has to move the drain, not make it safe.
 *
 * Payload rule, binding for every request type added here: a payload is a
 * value, or a `shared_ptr<const T>` to data nobody mutates after creation. It is
 * never a Scene, Mesh or Shader pointer - the Editor may edit or delete those
 * before the request is drained - and never a callable, because a closure
 * capturing the Editor would run Editor code wherever the renderer runs.
 *
 * Ordering is part of the contract: requests are executed FIFO, so an upload
 * posted after DropSceneResourcesRequest lands in the new scene, and one posted
 * before it is discarded with the old one.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <variant>
#include <vector>

#include "render/resources/LightingCache.h"
#include "scene/Light.h"

namespace neurus
{

class ImageData;
class MeshData;
class Shader;

/// @brief One light's GPU-side struct, as LightingCache stores it.
using LightStructVariant = std::variant<PointLightStruct, SunLightStruct, SpotLightStruct>;

/**
 * @brief Upload a mesh's geometry into the RenderCache slot for @c objectId.
 *
 * Shared by Mesh and DebugMesh, which carry the same MeshData and land in the
 * same slot. A no-op when the slot is already resident, so posting twice is
 * harmless - the Editor cannot see residency and does not try to.
 */
struct UploadMeshRequest
{
	int                              objectId = 0;
	std::shared_ptr<const MeshData>  data;
};

/**
 * @brief Allocate a shadow-casting light's shadow maps.
 *
 * No CPU payload: the maps are GPU-only images, sized by the light type. Still
 * a request because it writes the RenderCache's light map. A no-op when the
 * light already has its maps.
 */
struct CreateLightGpuRequest
{
	int       uid  = 0;
	LightType type = LightType::NONELIGHT;
};

/**
 * @brief Upload an environment's equirect and generate its IBL cubemaps.
 *
 * A null or invalid @c equirect selects the renderer's fallback gradient, the
 * same as a missing source file did before.
 *
 * @c regenerate distinguishes the two reasons to post one. The idempotent
 * scene-upload path leaves it false, so an environment already resident is
 * skipped - regenerating cubemaps is the most expensive step of a scene upload.
 * An explicit environment change sets it true, because the cached cubemaps are
 * exactly what has gone stale.
 */
struct UploadEnvironmentRequest
{
	int                               objectId   = 0;
	std::shared_ptr<const ImageData>  equirect;
	bool                              regenerate = false;
};

/**
 * @brief Rebuild every light SSBO and the uid-to-index maps from scratch.
 *
 * The Editor converts its lights to GPU structs (pure CPU work) and hands over
 * the result by value; the renderer only writes it.
 */
struct UpdateLightingRequest
{
	std::unordered_map<int, LightStructVariant> lights;
};

/**
 * @brief Rewrite one light's SSBO entry without rebuilding the arrays.
 *
 * Executed after any earlier UpdateLightingRequest, so a light added and then
 * edited within one Edit() is found.
 */
struct UpdateLightRequest
{
	int                uid = 0;
	LightStructVariant light;
};

/**
 * @brief Make sure the per-mesh pipeline for one shader version exists.
 *
 * Carries its own copy of the SPIR-V rather than the Shader, so a recompile
 * that lands before the drain cannot change the code under a version it no
 * longer belongs to. An empty stage is skipped, as for a live Shader.
 */
struct EnsurePipelineRequest
{
	int                   shaderId = 0;
	int                   version  = 0;
	std::vector<uint32_t> vertexSpv;
	std::vector<uint32_t> fragmentSpv;
};

/**
 * @brief Release every scene-scoped GPU resource of the outgoing scene.
 *
 * Drains the device first: cached entries own RAII resources that the last
 * frame may still be reading.
 */
struct DropSceneResourcesRequest
{
};

/**
 * @brief Restart progressive shadow accumulation from the next frame.
 *
 * Not GPU work, but it writes renderer state, and once the renderer runs on
 * its own thread a direct call from the Editor would be a data race. Routing it
 * here keeps the rule simple: the Editor never calls into the renderer except
 * to post.
 */
struct ResetAccumulationRequest
{
};

/**
 * @brief Redraw the overlays (selection outline, debug objects, gizmo) on the
 *        next published frame, without restarting accumulation.
 *
 * Posted when only overlay content changed. Once accumulation has converged the
 * renderer draws no scene frames, so this is what makes a selection or a gizmo
 * guide change reach the screen: the overlay engine redraws over the converged
 * scene image. Every other request restarts accumulation.
 */
struct RedrawOverlaysRequest
{
};

/// @brief Any request the renderer executes on the Editor's behalf.
using RenderRequest = std::variant<UploadMeshRequest,
                                   CreateLightGpuRequest,
                                   UploadEnvironmentRequest,
                                   UpdateLightingRequest,
                                   UpdateLightRequest,
                                   EnsurePipelineRequest,
                                   DropSceneResourcesRequest,
                                   ResetAccumulationRequest,
                                   RedrawOverlaysRequest>;

/**
 * @brief Snapshot what a per-mesh pipeline build needs out of a live Shader.
 *
 * Copies the id, the version and the vertex/fragment SPIR-V at the moment of
 * the call, so the request stays correct however the Shader changes later.
 */
EnsurePipelineRequest MakeEnsurePipelineRequest(const Shader& shader);

} // namespace neurus
