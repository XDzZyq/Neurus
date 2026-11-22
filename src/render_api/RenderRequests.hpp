/**
 * @file RenderRequests.hpp
 * @brief GPU work the Editor asks the renderer to do, as owned values.
 *
 * The renderer is the only code that touches a VkQueue, waits on one, or writes
 * the RenderCache's resource maps. Everything the Editor used to do to them
 * directly - upload a mesh, allocate a light's shadow maps, regenerate IBL,
 * rewrite the lighting SSBOs, warm a per-mesh pipeline, drop the outgoing
 * scene's resources - is posted instead as one of the requests below and
 * executed by DeferredRenderer at the top of DrawFrame, in posting order.
 *
 * Single ownership is the point, not tidiness: every device uploads through the
 * graphics queue family, so an upload submitted from one thread while a frame is
 * submitted from another is a race on the one queue handle that no cache lock
 * repairs. With every submit issued by the renderer, the render thread has only
 * to hand the requests over with their published frame - DrawFrame already runs
 * them after its fence wait, before it draws.
 *
 * Payload rule, binding for every request type added here: a payload is a
 * value, or a `shared_ptr<const T>` to data nobody mutates after creation. It is
 * never a Scene or Mesh pointer. A graph retains shared shader references for
 * identity only: rendering reads their UID and uses separately uploaded GPU data.
 * Requests never carry a callable, because a closure
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

#include "scene/Light.hpp"
#include "asset/data/RenderGraphData.hpp"
#include "asset/data/render_graph/PassPorts.hpp"
#include "asset/data/shader/ShaderStage.hpp"
#include "asset/data/shader/ShaderStruct.hpp"
#include "render_api/RenderFrameSnapshot.hpp"

namespace neurus
{

class ImageData;
class MeshData;
class ShaderData;

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
 * The Editor snapshots scene light values; the renderer converts them to its
 * GPU layouts and writes the buffers.
 */
struct UpdateLightingRequest
{
	std::unordered_map<int, LightSnapshot> lights;
};

/**
 * @brief Rewrite one light's SSBO entry without rebuilding the arrays.
 *
 * Executed after any earlier UpdateLightingRequest, so a light added and then
 * edited within one Edit() is found.
 */
struct UpdateLightRequest
{
	int           uid = 0;
	LightSnapshot light;
};

/** @brief Compiled bytes and reflection metadata for one GPU shader stage. */
struct ShaderStageUpload
{
	std::vector<uint32_t> spv;
	ShaderStruct parsed;
};

/**
 * @brief Upload a shader version before graph mapping or mesh pipeline use.
 *
 * Owns compiled bytes, descriptor metadata and the extracted image ports.
 * Later CPU edits cannot alter a queued upload.
 */
struct UploadShaderRequest
{
	int shaderId = 0;
	int version = 0;
	ShaderType primaryType = ShaderType::VERTEX;
	std::unordered_map<ShaderType, ShaderStageUpload> stages;
	PassPorts ports;  ///< Image-port contract extracted on the CPU.
};

/** @brief Copies compiled stages and descriptor metadata and derives the image ports. */
UploadShaderRequest MakeUploadShaderRequest(const ShaderData& shader);

/**
 * @brief Make sure the per-mesh pipeline for one shader version exists.
 *
 * Selects a previously uploaded GPU shader by identity and version. The
 * UploadShaderRequest must precede this request in FIFO order.
 */
struct EnsurePipelineRequest
{
	int                   shaderId = 0;
	int                   version  = 0;
};

/**
 * @brief Release every scene-scoped GPU resource of the outgoing scene.
 *
 * Drains the in-flight frames first: cached entries own RAII resources that a
 * recorded frame may still be reading.
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

/**
 * @brief Replace the scene graph document and compile it.
 *
 * Posted by the Editor when a project loads or the document changes. The
 * renderer keeps its previous graph when compilation fails and reports the
 * diagnostics; the request still restarts accumulation.
 *
 * A null graph selects the cached built-in document; a nonnull empty graph
 * deliberately renders no scene passes. The document is immutable once posted
 * (`shared_ptr<const RenderGraphData>`). Each explicit node shader is a
 * shared CPU reference retained for identity; the renderer reads only its UID
 * and resolves uploaded GPU data. A null node shader selects a built-in.
 * Shader uploads precede this request in FIFO order.
 */
struct CompileRenderGraphRequest
{
	std::shared_ptr<const RenderGraphData> graph;
};

/** @brief Copies one scene light into an owned request/frame value. */
LightSnapshot MakeLightSnapshot(const Light& light);

/** @brief Snapshots visible, rendered lights for an ordered GPU update. */
UpdateLightingRequest MakeUpdateLightingRequest(
	const std::unordered_map<int, std::shared_ptr<Light>>& lights);

/// @brief Any request the renderer executes on the Editor's behalf.
using RenderRequest = std::variant<UploadShaderRequest,
                                   UploadMeshRequest,
                                   CreateLightGpuRequest,
                                   UploadEnvironmentRequest,
                                   UpdateLightingRequest,
                                   UpdateLightRequest,
                                   EnsurePipelineRequest,
                                   DropSceneResourcesRequest,
                                   ResetAccumulationRequest,
                                   RedrawOverlaysRequest,
                                   CompileRenderGraphRequest>;

/**
 * @brief Snapshot what a per-mesh pipeline build needs out of a live Shader.
 *
 * Copies only the id and version; compiled bytes travel through the preceding
 * UploadShaderRequest.
 */
EnsurePipelineRequest MakeEnsurePipelineRequest(const ShaderData& shader);

} // namespace neurus
