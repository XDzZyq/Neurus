#include "Editor.hpp"

#include "editor/events/InputEvents.hpp"
#include "editor/events/AssetEvents.hpp"
#include "editor/events/OperationEvents.hpp"
#include "editor/events/ConfigEvents.hpp"
#include "editor/events/CameraEvents.hpp"
#include "editor/events/EditorEvents.hpp"
#include "editor/events/GizmoEvents.hpp"
#include "editor/events/ShaderEvents.hpp"
#include "editor/events/SceneEvents.hpp"

#include "editor/controllers/CameraController.hpp"
#include "editor/controllers/MutationNotify.hpp"
#include "editor/controllers/ObjectController.hpp"
#include "editor/controllers/RenderConfigController.hpp"
#include "editor/controllers/ScriptController.hpp"
#include "editor/controllers/ShaderController.hpp"
#include "editor/controllers/SceneController.hpp"
#include "editor/controllers/TransformGizmoController.hpp"
#include "editor/events/EventBus.hpp"

#include "render/requests/RenderRequestSink.hpp"
#include "render/requests/RenderRequests.hpp"
#include "render/UploadManager.hpp"

#include "render/RenderFrameSnapshot.hpp"
#include "render/shaders/RenderShader.hpp"
#include "ui/UIContext.hpp"
#include "script/ScriptContext.hpp"

#include "core/Log.hpp"
#include "asset/data/ImageData.hpp"
#include "asset/data/AssetPath.hpp"
#include "asset/data/MeshData.hpp"
#include "asset/data/ShaderData.hpp"
#include "asset/data/shader/ShaderStage.hpp"
#include "asset/data/shader/ShaderUnit.hpp"
#include "asset/graph/CpuGraphValidation.hpp"
#include "asset/graph/PassKinds.hpp"
#include "asset/graph/PassShaders.hpp"
#include "asset/graph/ShaderSnapshot.hpp"
#include "render/shaders/ComputeShader.hpp"
#include "render/shaders/ShaderLibrary.hpp"
#include "scene/Camera.hpp"
#include "scene/DebugLine.hpp"
#include "scene/DebugMesh.hpp"
#include "scene/DebugPoints.hpp"
#include "scene/Environment.hpp"
#include "scene/Light.hpp"
#include "scene/Mesh.hpp"
#include "scene/Scene.hpp"
#include "scene/Transform.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace neurus {

namespace {

/**
 * @brief Uniform scale applied to the starter mesh (and its wireframe copy).
 *
 * The asset is exported at a ~7.44-unit radius while the starter scene is
 * authored in unit space, so left raw the camera sits INSIDE the mesh and every
 * debug object is buried in it. Normalising the mesh is the fix rather than
 * pushing the camera back, because the debug overlay is the point of this scene.
 */
constexpr float kStarterMeshScale = 0.135f;

/**
 * @brief Posts a mesh geometry upload into the slot keyed by @p objectId.
 *
 * Shared by Mesh and DebugMesh, which carry the same MeshData and land in the
 * same slot. The renderer skips an already-resident slot, so the Editor no
 * longer checks residency - it cannot see it. Callers skip a null geometry.
 */
void PostMeshUpload(RenderRequestSink& renderer, int objectId, std::shared_ptr<const MeshData> data)
{
	renderer.Post(UploadMeshRequest{objectId, std::move(data)});
}

/**
 * @brief Posts a shadow-map allocation when the light casts shadows.
 *
 * The use_shadow test reads the Scene, so it stays here; the renderer only
 * allocates the GPU-only maps for the light type.
 */
void PostLightUpload(RenderRequestSink& renderer, const Light& light)
{
	if (!light.use_shadow) return;
	renderer.Post(CreateLightGpuRequest{light.GetObjectID(), light.light_type});
}

/**
 * @brief Posts an environment's equirect upload and IBL (re)generation.
 *
 * @p regenerate is false on the idempotent scene-upload path (skip if resident)
 * and true when the environment itself changed and its cubemaps are stale.
 */
void PostEnvironmentUpload(RenderRequestSink& renderer, const Environment& env, bool regenerate)
{
	renderer.Post(UploadEnvironmentRequest{env.GetObjectID(), env.GetEquirectData(), regenerate});
}

/**
 * @brief UID of the scene's active object, or 0 when nothing is selected.
 *
 * The intent events carry a uid rather than a pointer; 0 is legal to emit,
 * and the controller drops an intent it cannot resolve.
 */
int ActiveUid(const Scene& scene)
{
	const ObjectID* active = scene.selections.GetActiveObject();
	return active ? active->GetObjectID() : 0;
}

/**
 * @brief Resolves one shader stage from a pooled source or a source path.
 *
 * Editor-owned import helper: it reads the pool and the pool write follows this
 * resolution. A pooled source contributes the stage matching @p type; a path
 * must carry the matching extension; an empty path uses @p defaultPath.
 *
 * @param resources Editor-owned pool.
 * @param sourceUid Pooled shader supplying the stage, or 0 to use @p path.
 * @param path Source path; empty uses @p defaultPath.
 * @param type Stage to resolve.
 * @param defaultPath Fallback when @p path is empty.
 * @param error Reason on failure.
 * @return The resolved stage, or std::nullopt on failure.
 */
std::optional<neurus::ShaderUnit> ResolveShaderStage(neurus::ResourceManager& resources,
                                                     int sourceUid, const std::string& path,
                                                     neurus::ShaderType type,
                                                     const std::string& defaultPath,
                                                     std::string& error)
{
	if (sourceUid != 0)
	{
		auto source = resources.Get<neurus::ShaderData>(sourceUid);
		if (!source)
		{
			error = "shader source not found: " + std::to_string(sourceUid);
			return std::nullopt;
		}
		const neurus::ShaderUnit* unit = source->FindStage(type);
		if (!unit)
		{
			error = "shader source has no " +
			        neurus::ShaderData::TypeToString(type) + " stage";
			return std::nullopt;
		}
		return *unit;
	}

	return neurus::ShaderLibrary::LoadStageUnit(path.empty() ? defaultPath : path, type,
	                                            &error);
}

/**
 * @brief Registers a constructed shader in the pool under a unique name.
 *
 * A shader is built and compiled off-pool, so a failure never leaves a
 * half-made entry behind; the pool owns it from this call on. The unique name
 * is mirrored into the shader, the same way Load<T> does for a scene object.
 *
 * @param resources Editor-owned pool.
 * @param shader Constructed, compiled shader.
 * @param name Requested name; a collision gains the usual ".001" suffix.
 * @return The pooled uid carried by the link request and the undo operation.
 */
int PoolShader(neurus::ResourceManager& resources,
               const std::shared_ptr<neurus::ShaderData>& shader,
               const std::string& name)
{
	const int uid = resources.Register(shader);
	const std::string unique = resources.SetName(uid, name);
	shader->SetName(unique.empty() ? name : unique);
	return uid;
}

/**
 * @brief Creates the default shader for one source of a built-in pass kind.
 *
 * @param source  Shader source row.
 * @return Parsed shader, or nullptr when loading failed.
 */
std::shared_ptr<neurus::ShaderData> LoadPassKindShader(const neurus::PassShaderSource& source)
{
	if (!source.computePath.empty())
	{
		return neurus::ShaderLibrary::LoadComputeShader(
			std::string(source.name), std::string(source.computePath));
	}

	return neurus::ShaderLibrary::LoadRenderShader(
		std::string(source.name), std::string(source.vertexPath),
		std::string(source.fragmentPath));
}

} // anonymous namespace

Editor::Editor(RenderRequestSink* renderer)
	: m_scene(std::make_unique<Scene>())
	, m_resources(std::make_unique<ResourceManager>())
	, ed_operations(ed_eventBus)
	, ed_renderer(renderer)
{}

Editor::~Editor()
{
	// Nothing owned here needs a manual release.
}

void Editor::Initialize()
{
	// Mesh/light GPU upload happens after the window is shown and the surface is
	// ready: see UploadSceneResources(), called from the scene load lifecycle.

	ed_eventBus.Subscribe<MeshImportEvent>([this](const MeshImportEvent& e) {
		OnMeshImport(e.path);
		neurus::notify::Repaint(ed_eventBus);
	});

	// Shader lifecycle: the Editor owns the pool, so it creates/imports shader
	// assets here and emits ObjectShaderAddRequested for SceneController to
	// link. The UI asks for the default pair (ShaderCreateEvent), a script
	// composes a new bundle (ShaderCreateRequested), and load_shader imports an
	// asset with no mesh (ShaderImportRequested).
	ed_eventBus.Subscribe<ShaderCreateEvent>([this](const ShaderCreateEvent& e) {
		OnShaderCreateEvent(e);
	});
	ed_eventBus.Subscribe<ShaderCreateRequested>([this](const ShaderCreateRequested& e) {
		OnShaderCreate(e);
	});
	ed_eventBus.Subscribe<ShaderImportRequested>([this](const ShaderImportRequested& e) {
		OnShaderImportRequested(e);
	});
	ed_eventBus.Subscribe<CameraAddEvent>([this](const CameraAddEvent&) {
		OnCameraAdd();
		neurus::notify::Repaint(ed_eventBus);
	});
	ed_eventBus.Subscribe<LightAddEvent>([this](const LightAddEvent&) {
		OnLightAdd();
		neurus::notify::Repaint(ed_eventBus);
	});
	ed_eventBus.Subscribe<SunLightAddEvent>([this](const SunLightAddEvent&) {
		OnSunLightAdd();
		neurus::notify::Repaint(ed_eventBus);
	});
	ed_eventBus.Subscribe<SpotLightAddEvent>([this](const SpotLightAddEvent&) {
		OnSpotLightAdd();
		neurus::notify::Repaint(ed_eventBus);
	});

	// Script/command content creation. The Editor owns the ResourceManager, so
	// these requests land here; the creation itself still forwards the usual
	// SceneObjectAddRequested, which keeps the undo entry unchanged.
	ed_eventBus.Subscribe<MeshImportRequested>([this](const MeshImportRequested& e) {
		OnMeshImportRequested(e);
		neurus::notify::Repaint(ed_eventBus);
	});
	ed_eventBus.Subscribe<CameraAddRequested>([this](const CameraAddRequested& e) {
		OnCameraAddRequested(e);
		neurus::notify::Repaint(ed_eventBus);
	});
	ed_eventBus.Subscribe<ObjectDuplicateRequested>([this](const ObjectDuplicateRequested& e) {
		OnObjectDuplicateRequested(e);
		neurus::notify::Repaint(ed_eventBus);
	});

	// Shader creation, loading and removal are handled by ShaderController via
	// the event bus; the Editor supplies the mutable pool in ControllerContext.

	// Undo/redo of Create Shader: redo relinks the pooled RenderShader by UID;
	// undo drops the reference (the pool keeps the shader). No version bump: the
	// per-mesh (objectId, version) pipeline cache handles the relink.
	ed_eventBus.Subscribe<ShaderLinkRestored>([this](const ShaderLinkRestored& e) {
		auto mesh = m_resources->Get<Mesh>(e.objectUid);
		if (!mesh) return;
		auto shader = m_resources->Get<RenderShader>(e.shaderId);
		if (!shader)
		{
			NEURUS_ERR("[Editor] ShaderLinkRestored: shader " << e.shaderId << " not pooled");
			return;
		}
		mesh->SetObjShader(shader);
		neurus::notify::Mutated(ed_eventBus);
	});
	ed_eventBus.Subscribe<ShaderUnlinkRestored>([this](const ShaderUnlinkRestored& e) {
		auto mesh = m_resources->Get<Mesh>(e.objectUid);
		if (!mesh) return;
		mesh->SetObjShader(nullptr);
		neurus::notify::Mutated(ed_eventBus);
	});

	// Undo/redo replay their inverse events synchronously (never queued), so
	// the mutation applies in-place and cannot reorder against live input.
	ed_eventBus.Subscribe<UndoRequested>([this](const UndoRequested&) {
		ed_operations.Undo();
	});
	ed_eventBus.Subscribe<RedoRequested>([this](const RedoRequested&) {
		ed_operations.Redo();
	});


	// --- Register controllers ---
	// All seven take only the ControllerContext, which carries the event dispatch,
	// the pooled-object lookup, the operation sink, the scene, the render config,
	// the viewport and the gizmo state.
	//
	// TransformGizmoController and CameraController share no event type, so their
	// relative order has no dispatch consequence: the gizmo handles MouseMoveEvent
	// and reads the cursor from e.position, while the camera controller handles the
	// Camera* events the Editor derives from that same event.
	RegisterController<CameraController>();
	RegisterController<TransformGizmoController>();
	RegisterController<ShaderController>();
	RegisterController<SceneController>();
	RegisterController<ObjectController>();
	RegisterController<RenderConfigController>();
	// Bounds a script run: the run-boundary events open and close the operation
	// sink's script scope, so every write the run makes lands as one undo step.
	RegisterController<ScriptController>();

	// --- Keep the published camera correct across a deletion ---
	// Registered after the controllers, because both subscribers to this event run
	// inside ONE synchronous dispatch, in registration order: SceneController's
	// handler removes the objects, then this one republishes the active camera.
	// Without it, deleting the activated camera would leave the viewport pointing
	// at a camera that is no longer the document's view for the rest of Process().
	ed_eventBus.Subscribe<SceneObjectDeleteRequested>([this](const SceneObjectDeleteRequested&) {
		PublishActiveCamera();
	});

	// --- Subscribe to EnvironmentChanged to regenerate IBL cubemaps on demand ---
	ed_eventBus.Subscribe<EnvironmentChanged>([this](const EnvironmentChanged& e) {
		neurus::notify::Repaint(ed_eventBus);
		if (!ed_renderer) return;
		auto it = GetScene().env_list.find(e.envId);
		if (it != GetScene().env_list.end())
		{
			// regenerate=true: the environment itself changed, so its cached
			// cubemaps are exactly what has gone stale.
			PostEnvironmentUpload(*ed_renderer, *it->second, true);
		}
		else
		{
			NEURUS_ERR("[Editor] EnvironmentChanged: env ID " << e.envId << " not found");
		}
	});

	// --- On-demand GPU upload for objects entering the scene (live add or
	// undo/redo replay) and lights whose shadow was just enabled ---
	ed_eventBus.Subscribe<SceneObjectGpuUploadRequested>([this](const SceneObjectGpuUploadRequested& e) {
		OnSceneObjectGpuUpload(e.objectUid);
	});

	ed_eventBus.Subscribe<MouseMoveEvent>([this](const MouseMoveEvent& e) {
		// Retained FIRST and unconditionally: the cursor is a property of the
		// viewport, not of the scene, and the null-camera return below would
		// otherwise swallow it. The viewport dirties its own gizmo guide from
		// here - a live gesture follows the cursor through a pixel budget.
		m_viewport.SetCursor(e.position);

		const Camera* cam = m_viewport.GetCamera();
		if (!cam) return;

		// Orbit is suppressed for the duration of a modal gesture, which also keeps
		// the drag's anchor math camera-invariant.
		if (m_viewport.IsGizmoActive()) return;

		if (e.middleHeld)
		{
			if (e.modifiers & Input::Mod_Ctrl)
				ed_eventBus.Emit(CameraPushEvent{cam->GetObjectID(), e.delta.x, e.delta.y});
			else if (e.modifiers & Input::Mod_Shift)
				ed_eventBus.Emit(CameraSlideEvent{cam->GetObjectID(), e.delta.x, e.delta.y});
			else
				ed_eventBus.Emit(CameraRotateEvent{cam->GetObjectID(), e.delta.x, e.delta.y});
		}
	});

	// Middle-button press/release bound the orbit/pan/dolly drag gesture so it
	// collapses to one undo entry. Left and right close a modal transform gesture
	// instead - emitted unconditionally and dropped by the controller when no
	// gesture is live.
	ed_eventBus.Subscribe<MousePressEvent>([this](const MousePressEvent& e) {
		if (e.button == Input::Left)  { ed_eventBus.Emit(GizmoConfirmed{}); return; }
		if (e.button == Input::Right) { ed_eventBus.Emit(GizmoCancelled{}); return; }
		if (e.button != Input::Middle) return;
		if (const Camera* cam = m_viewport.GetCamera())
			ed_eventBus.Emit(CameraDragBegin{cam->GetObjectID()});
	});
	ed_eventBus.Subscribe<MouseReleaseEvent>([this](const MouseReleaseEvent& e) {
		if (e.button != Input::Middle) return;
		if (const Camera* cam = m_viewport.GetCamera())
			ed_eventBus.Emit(CameraDragEnd{cam->GetObjectID()});
	});

	ed_eventBus.Subscribe<MouseScrollEvent>([this](const MouseScrollEvent& e) {
		const Camera* cam = m_viewport.GetCamera();
		if (!cam) return;

		if (std::abs(e.delta) > 0.001f)
			ed_eventBus.Emit(CameraZoomEvent{cam->GetObjectID(), e.delta});
	});

	// --- Pure UI->Editor intents: wrap the active scene, forward dedicated events ---
	ed_eventBus.Subscribe<ObjectClicked>([this](const ObjectClicked& e) {
		ed_eventBus.Emit(ObjectSelected{ e.objectUid, e.modifiers });
	});
	ed_eventBus.Subscribe<DeleteRequested>([this](const DeleteRequested&) {
		// Stamp the gesture with the current selection: the controller takes an
		// explicit UID list, so the Delete key and a command share one handler.
		// An empty selection does nothing.
		std::vector<int> selected;
		for (const ObjectID* obj : GetScene().selections.GetSelectedList())
		{
			if (obj != nullptr)
				selected.push_back(obj->GetObjectID());
		}
		if (selected.empty()) return;
		ed_eventBus.Emit(DeleteObjectsRequested{std::move(selected)});
	});

	// --- Viewport keystrokes -> modal transform intents ---
	// Every branch emits UNCONDITIONALLY: an intent is a request, and the
	// controller is the single place that decides it is meaningless. Dispatch is
	// synchronous, so a mode key that arrived in the same frame has already been
	// applied to the gesture before the axis key behind it is emitted.
	ed_eventBus.Subscribe<KeyPressEvent>([this](const KeyPressEvent& e) {
		switch (e.key)
		{
		case Input::Key_G: ed_eventBus.Emit(GizmoModeRequested{ActiveUid(GetScene()), GizmoMode::Move});   break;
		case Input::Key_R: ed_eventBus.Emit(GizmoModeRequested{ActiveUid(GetScene()), GizmoMode::Rotate}); break;
		case Input::Key_S: ed_eventBus.Emit(GizmoModeRequested{ActiveUid(GetScene()), GizmoMode::Scale});  break;

		case Input::Key_X: ed_eventBus.Emit(GizmoAxisRequested{GizmoAxis::X}); break;
		case Input::Key_Y: ed_eventBus.Emit(GizmoAxisRequested{GizmoAxis::Y}); break;
		case Input::Key_Z: ed_eventBus.Emit(GizmoAxisRequested{GizmoAxis::Z}); break;

		// W is the fourth member of that set, not a separate feature: it names the
		// screen as the constraint, which is exactly GizmoAxis::None - the view plane
		// for Move, uniform for Scale, the view axis for Rotate.
		case Input::Key_W: ed_eventBus.Emit(GizmoAxisRequested{GizmoAxis::None}); break;

		case Input::Key_Return: ed_eventBus.Emit(GizmoConfirmed{}); break;
		case Input::Key_Escape: ed_eventBus.Emit(GizmoCancelled{}); break;

		default: break;
		}
	});

	// --- Subscribe to RenderResetEvent to reset temporal accumulation ---
	ed_eventBus.Subscribe<RenderResetEvent>([this](const RenderResetEvent&) {
		if (ed_renderer)
			ed_renderer->Post(ResetAccumulationRequest{});

		// Second consumer on the same signal: this is the "something visible
		// changed" broadcast, so it is exactly when the overlays may have gone
		// stale. Marking is O(1); the rebuild happens once in Edit(). The gizmo
		// guide rides the same broadcast, which is what makes it appear on arm,
		// follow a drag and vanish on confirm or cancel.
		m_viewport.MarkOverlaysDirty();
	});

	// --- Warm the per-mesh pipeline when a shader actually compiled ---
	//
	// A controller cannot reach the renderer by design, so the Editor subscribes
	// and warms. The compile handlers emit synchronously, so the request is
	// already posted when the compile returns - before the frame is handed off -
	// and no frame draws with the pass default. Keyed on the shader's object id.
	ed_eventBus.Subscribe<ShaderCompiled>([this](const ShaderCompiled& e) {
		if (!ed_renderer) return;
		auto shader = m_resources->Get<ShaderData>(e.shaderObjectId);
		if (!shader) return;
		ed_renderer->Post(MakeEnsurePipelineRequest(*shader));
	});

	// --- Selection moves the resting handle, and selection alone emits no
	// RenderResetEvent ---
	//
	// Selecting an object is not a scene modification, so the broadcast above
	// never fires for it; the handle still has to move to the newly active object.
	// All three paths are covered: a live click, an explicit deselect, and the
	// absolute set an undo/redo replay applies.
	ed_eventBus.Subscribe<ObjectSelected>([this](const ObjectSelected&) {
		m_viewport.MarkGizmoDirty();
	});
	ed_eventBus.Subscribe<ObjectDeselected>([this](const ObjectDeselected&) {
		m_viewport.MarkGizmoDirty();
	});
	ed_eventBus.Subscribe<SelectionChanged>([this](const SelectionChanged&) {
		m_viewport.MarkGizmoDirty();
	});

	// --- A native-window expose asks for one fresh present ---
	// Qt cannot repaint the Vulkan surface, and a converged renderer presents
	// nothing on its own, so the overlays are marked stale: Edit() then posts
	// RedrawOverlaysRequest and the image already drawn is presented again.
	ed_eventBus.Subscribe<ViewportExposed>([this](const ViewportExposed&) {
		m_viewport.MarkOverlaysDirty();
	});

	// --- Scene mutation GPU-sync + dirty subscriptions (SceneController /
	// ObjectController) ---
	ed_eventBus.Subscribe<SceneModified>([this](const SceneModified&) {
		m_dirty = true;
	});

	ed_eventBus.Subscribe<LightGpuChanged>([this](const LightGpuChanged& e) {
		if (!ed_renderer) return;
		auto light = m_resources->Get<Light>(e.objectUid);
		if (!light) return;
		// Pure CPU conversion here, then the renderer rewrites the one SSBO entry.
		ed_renderer->Post(UpdateLightRequest{e.objectUid, UploadManager::UploadLighting(*light)});
	});

	ed_eventBus.Subscribe<LightingRebuild>([this](const LightingRebuild&) {
		UploadLighting();
	});

	NEURUS_LOG("[Editor] Initialized");
}

Scene& Editor::GetScene()
{
	return *m_scene;
}

// =========================================================================
// Active camera - selection, not synchronization
// =========================================================================

const Camera* Editor::ActiveCamera() const
{
	// The scene's activated camera wins; the viewport camera is the fallback. The
	// two stay independent: this chooses one, it never copies state between them.
	const Scene& scene = *m_scene;
	if (const Camera* sceneCam = scene.GetActiveCamera())
		return sceneCam;
	return m_viewport.ViewportCamera();
}

void Editor::PublishActiveCamera(bool extentChanged)
{
	const Camera* active = ActiveCamera();
	const bool changed = active != m_viewport.GetCamera();

	m_viewport.SetCamera(active);
	if (!active) return;

	// A camera becoming active needs the current viewport aspect once (a fresh
	// Camera is 1x1); a resize needs it re-applied to the same camera. Both go
	// through the same event, and CameraController is the only writer.
	const glm::uvec2 size = m_viewport.Size();
	if ((changed || extentChanged) && size.x != 0u && size.y != 0u)
	{
		ed_eventBus.Emit(CameraResizeEvent{
			active->GetObjectID(),
			static_cast<int>(size.x), static_cast<int>(size.y)});
	}

	// A camera change rescales every fixed-pixel gizmo handle.
	if (changed)
		m_viewport.MarkGizmoDirty();
}

void Editor::HandleViewportResize(glm::uvec2 logical, glm::uvec2 renderExtent)
{
	m_viewport.HandleResize(logical, renderExtent);
	PublishActiveCamera(true);
}

// =========================================================================
// UpdateUIContext / BuildSnapshot - the one producer of every outward view
// of the frame
// =========================================================================

void Editor::BuildSnapshot(RenderFrameSnapshot& out) const
{
	// Supply the five sources and gate the debug list here, so production
	// behaviour is unchanged: r_debug_draw decides whether the overlay is
	// published.
	BuildSnapshot(m_scene.get(),
	              m_config,
	              m_viewport.GetCamera(),
	              m_config.RequiresDebugDraw() ? &m_viewport.DebugList() : nullptr,
	              &m_viewport.GizmoList(),
	              out);
}

void Editor::BuildSnapshot(const Scene* scene,
                           const RenderConfig& config,
                           const Camera* camera,
                           const DebugDrawList* debugDraw,
                           const GizmoDrawList* gizmoDraw,
                           RenderFrameSnapshot& out)
{
	// Cleared rather than default-constructed: the draw lists keep their
	// capacity, which is the whole point of refilling one snapshot per frame.
	out.Clear();

	// The frame the render layer reads, resolved to stable UIDs and read by
	// value rather than borrowed by pointer. This is the one producer of that
	// view: a reader may keep the result while the Editor edits on.
	out.config = config;

	if (camera)
	{
		out.hasCamera = true;
		// Matrices are copied, not re-derived from the scalars below: the Camera
		// caches them, and recomputing is how a reader ends up disagreeing with
		// the CameraGPU the vertex stage used.
		out.camera.view       = camera->GetViewMatrix();
		out.camera.projection = camera->GetProjectionMatrix();
		out.camera.position   = camera->GetPosition();
		out.camera.target     = camera->cam_tar;
		out.camera.width      = camera->cam_w;
		out.camera.height     = camera->cam_h;
		out.camera.fovDegrees = camera->cam_pers;
		out.camera.nearPlane  = camera->cam_near;
		out.camera.farPlane   = camera->cam_far;
	}

	// The caller gates the debug overlay: a non-null list is published, a null
	// one (debug draw off, or no scene yet) costs the snapshot nothing.
	if (debugDraw)
	{
		out.hasDebugDraw = true;
		out.debugDraw = *debugDraw;
	}

	// Copied when published; a null gizmo list leaves the cleared payload above.
	if (gizmoDraw)
		out.gizmoDraw = *gizmoDraw;

	if (!scene)
		return;   // a frame with no scene is legal, and is now complete

	out.hasScene = true;

	out.meshes.reserve(scene->mesh_list.size());
	for (const auto& [id, mesh] : scene->mesh_list)
	{
		if (!mesh) continue;

		MeshInstanceSnapshot inst;
		inst.objectId          = mesh->GetObjectID();
		inst.model             = mesh->GetModelMatrix();
		inst.normalMatrix      = mesh->GetNormalMatrix();
		inst.hasGeometry       = (mesh->o_mesh != nullptr);
		inst.visibleInViewport = mesh->is_viewport;
		inst.rendered          = mesh->is_rendered;
		// The per-mesh pipeline is cached under the shader id plus the shader
		// version, so those two numbers are the whole reference - the Shader
		// object itself never travels.
		if (mesh->o_shader)
		{
			inst.shaderObjectId = mesh->o_shader->GetObjectID();
			inst.shaderVersion  = mesh->o_shader->GetVersion();
		}
		out.meshes.push_back(inst);
	}

	out.lights.reserve(scene->light_list.size());
	for (const auto& [uid, light] : scene->light_list)
	{
		if (!light) continue;

		LightSnapshot ls;
		ls.objectId          = light->GetObjectID();
		ls.type              = light->light_type;
		ls.position          = light->GetPosition();
		ls.direction         = light->GetDirection();
		ls.color             = light->light_color;
		ls.power             = light->light_power;
		ls.radius            = light->light_radius;
		ls.spotCutoff        = light->spot_cutoff;
		ls.spotOuterCutoff   = light->spot_outer_cutoff;
		ls.areaRatio         = light->area_ratio;
		ls.useShadow         = light->use_shadow;
		ls.visibleInViewport = light->is_viewport;
		ls.rendered          = light->is_rendered;
		out.lights.push_back(ls);
	}

	// Sorted by UID, because the pools are unordered_maps: downstream the light
	// index picks a descriptor slot and the mesh order is the draw order, so two
	// snapshots of one unchanged scene must not differ in order.
	std::sort(out.meshes.begin(), out.meshes.end(),
	          [](const MeshInstanceSnapshot& l, const MeshInstanceSnapshot& r)
	          { return l.objectId < r.objectId; });
	std::sort(out.lights.begin(), out.lights.end(),
	          [](const LightSnapshot& l, const LightSnapshot& r)
	          { return l.objectId < r.objectId; });

	// One environment lights the frame. The scene pool is unordered and the
	// renderer reads whichever it finds first, so the lowest UID is chosen here
	// to make "first" mean the same thing on every frame.
	for (const auto& [id, env] : scene->env_list)
	{
		if (!env) continue;
		const int envId = env->GetObjectID();
		if (!out.hasEnvironment || envId < out.environmentId)
		{
			out.hasEnvironment = true;
			out.environmentId  = envId;
		}
	}

	// The selection travels as an id, and 0 means none - the same value the
	// outline pass pushes when nothing is selected.
	if (const ObjectID* active = scene->selections.GetActiveObject())
		out.activeObjectId = active->GetObjectID();
}

RenderFrameSnapshot Editor::BuildSnapshot() const
{
	RenderFrameSnapshot snap;
	BuildSnapshot(snap);
	return snap;
}

void Editor::UpdateUIContext(UIContext& ctx) const
{
	// Fields only - never a call into a UIContext method. UpdateUIContext is
	// compiled into neurus_editor, which does not link neurus_ui (that library
	// links *this* one), so referencing a UIContext symbol here would not resolve.
	ctx.scene = m_scene.get();
	ctx.config = &m_config;
}

void Editor::UpdateScriptContext(ScriptContext& ctx)
{
	// Same shape as UpdateUIContext: borrowed pointers, no symbol calls. The pool
	// is handed over as its read-only interface so a script can resolve names
	// and ids without reaching the Editor's mutable ResourceManager. Content
	// creation is not exposed here: a script asks for it through typed events.
	ctx.scene = m_scene.get();
	ctx.config = &m_config;
	ctx.resources = m_resources.get();
	ctx.graph = &m_renderGraph;
}

// =========================================================================
// Scene lifecycle
// =========================================================================

void Editor::NewScene()
{
	try
	{
		// The outgoing scene's GPU resources go with it (this drains the device
		// first). Nothing is uploaded in return: the new scene is empty.
		DropSceneGpuResources();

		m_scene = std::make_unique<Scene>();
		m_resources->Clear();
		m_config = RenderConfig{};
		ed_operations.Clear(); // History does not span scenes.

		// The pool just lost the old viewport camera; a new document gets a new one
		// at the default framing, already adopting the live viewport extent. This is
		// what keeps an empty scene renderable: the viewport camera is the view, not
		// scene content.
		m_viewport.ForgetCamera();
		m_viewport.EnsureCamera(*m_resources);
		PublishActiveCamera();

		// An empty document is its own saved baseline, not an unsaved edit.
		m_dirty = false;

		// New scene => the old flattened overlay is stale, and so is the handle: it
		// is anchored on the outgoing document's selection, and the builder only
		// clears its list inside a dirty Rebuild().
		m_viewport.MarkOverlaysDirty();

		ResetRenderGraph();

		NEURUS_LOG("[Editor] Created new scene.");
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("Failed to create new scene: " << e.what());
	}
}

void Editor::CreateDefaultScene(const std::string& objPath)
{
	// The starter scene ships a scene camera as demo content, deliberately left
	// DEACTIVATED: the viewport looks through its own camera until the user
	// activates one. A deactivated camera adopts the extent only when activated.
	auto camera = m_resources->Load<Camera>();
	camera->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
	camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	m_scene->UseCamera(camera);

	auto meshData = m_resources->Load<MeshData>(objPath);
	auto mesh = m_resources->Load<Mesh>(meshData);
	mesh->SetScale(glm::vec3(kStarterMeshScale)); // see kStarterMeshScale
	m_scene->UseMesh(mesh);

	auto light = m_resources->Load<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
	light->SetPosition(glm::vec3(3.0f, 3.0f, 3.0f));
	light->SetRadius(0.01f);
	m_scene->UseLight(light);

	// Paths are relative ("res/...") so project files stay portable; the resource
	// layer resolves them against the asset dir at load time.
	auto imageData = m_resources->Load<ImageData>("res/tex/hdr/room.hdr");
	auto env = m_resources->Load<Environment>(imageData);
	m_scene->UseEnvironment(env);

	AddDefaultDebugObjects(meshData);

	// This mutated a scene, so it is dirty by the same rule as any other edit; a
	// caller for which this content IS the baseline rebases with ClearDirty().
	m_dirty = true;
	m_viewport.MarkOverlaysDirty();  // the flattened overlay predates these objects
}

/**
 * @brief Populates the default scene with one debug object of each kind.
 *
 * Demo content, like the .obj and .hdr loaded above: it gives the overlay
 * something to draw out of the box, exercising depth occlusion, x-ray, stipple,
 * screen-space sprites and the CUBE decomposition on a fresh launch.
 */
void Editor::AddDefaultDebugObjects(const std::shared_ptr<MeshData>& meshData)
{
	// The axis spans, reused by the depth-tested and x-ray lines below so the two
	// overlap exactly and differ only in depth mode.
	const std::vector<glm::vec3> axisSpans = {
		{-4.0f, 0.0f, 0.0f}, {4.0f, 0.0f, 0.0f},
		{0.0f, -4.0f, 0.0f}, {0.0f, 4.0f, 0.0f},
		{0.0f, 0.0f, -4.0f}, {0.0f, 0.0f, 4.0f},
	};

	// Debug primitives are pooled objects like the starter content above, so
	// Load already registered their names: the Outliner and Python see unique
	// labels.
	//
	// World axes, thick and solid: the depth-tested reference. They pass through
	// the demo mesh, so the hidden halves are the depth-occlusion proof.
	auto axes = m_resources->Load<DebugLine>();
	axes->SetWidth(3.0f);
	axes->SetColor(glm::vec4(0.95f, 0.75f, 0.15f, 1.0f));
	axes->PushDebugLines(axisSpans);
	m_scene->UseDebugLine(axes);

	// The same spans as a thin dashed x-ray line: visible on top of the mesh
	// exactly where the solid one is occluded, showing both modes at once.
	auto xrayAxes = m_resources->Load<DebugLine>();
	xrayAxes->SetWidth(1.0f);
	xrayAxes->SetStipple(true);
	xrayAxes->SetXRay(true);
	xrayAxes->SetColor(glm::vec4(0.2f, 0.9f, 1.0f, 0.8f));
	xrayAxes->PushDebugLines(axisSpans);
	m_scene->UseDebugLine(xrayAxes);

	// Circular sprites at the axis tips, screen-space sized so they stay legible
	// at any zoom - the default projection mode, exercised here on purpose.
	auto tips = m_resources->Load<DebugPoints>();
	tips->SetPointType(DebugPoints::PointType::CIR);
	tips->SetScale(10.0f);
	tips->SetColor(glm::vec4(1.0f, 0.35f, 0.35f, 1.0f));
	tips->PushDebugPoints({
		{4.0f, 0.0f, 0.0f}, {0.0f, 4.0f, 0.0f}, {0.0f, 0.0f, 4.0f},
	});
	m_scene->UseDebugPoints(tips);

	// A wireframe cube around the light, via the CUBE point type: this is the
	// path that decomposes one point into 12 segments.
	auto lightBox = m_resources->Load<DebugPoints>();
	lightBox->SetPointType(DebugPoints::PointType::CUBE);
	lightBox->SetProjectionMode(1);  // world units - a cube has real extent
	lightBox->SetScale(0.4f);
	lightBox->SetColor(glm::vec4(1.0f, 1.0f, 0.6f, 1.0f));
	lightBox->PushDebugPoint({3.0f, 3.0f, 3.0f});
	m_scene->UseDebugPoints(lightBox);

	// A wireframe copy of the demo mesh, scaled slightly up and offset so it
	// reads as a shell rather than z-fighting with the shaded original. This is
	// the third primitive kind (PolygonMode::eLine over a real MeshGPU), so the
	// default scene exercises all of lines, points and wire meshes.
	if (meshData)
	{
		auto wire = m_resources->Load<DebugMesh>(meshData);
		wire->SetPosition(glm::vec3(2.5f, 0.0f, 0.0f));
		// Same normalisation as the shaded copy (kStarterMeshScale), 2% larger so
		// it reads as a shell. At 2.5 units out the two spheres (radius ~1) stay
		// clear of each other, so this is a sibling object, not a z-fighting skin.
		wire->SetScale(glm::vec3(kStarterMeshScale * 1.02f));
		wire->SetColor(glm::vec4(0.4f, 1.0f, 0.5f, 1.0f));
		wire->SetOpacity(0.9f);
		m_scene->UseDebugMesh(wire);
	}
}

void Editor::BeginLoad()
{
	// The outgoing scene's GPU resources go with it (this drains the device
	// first); the project's own objects are uploaded by FinishLoad().
	DropSceneGpuResources();

	m_scene = std::make_unique<Scene>();
	m_resources->Clear(); // Pool is restored from the project file next.
	m_config = RenderConfig{};
	ed_operations.Clear(); // History does not span scenes.
	// The Clear() above dropped the viewport camera. It is NOT re-created here: the
	// project file may name its own, and creating one now would leak an orphan into
	// the pool. FinishLoad() resolves or creates it once the file has been read.
	m_viewport.ForgetCamera();
	// The caller deserializes into GetScene()/GetRenderConfig() before FinishLoad().
}

void Editor::FinishLoad()
{
	// Pool restore happens during the project load: pooled resources are
	// deserialized and wired, and the scene then resolves its ID references.
	m_dirty = false;
	// Resolve the viewport camera the project named, or create one for a project
	// written before it existed. Both paths re-adopt the live extent rather than
	// trusting the archived aspect.
	m_viewport.EnsureCamera(*m_resources);
	PublishActiveCamera();
	// Debug objects came back from the project file, and the restored selection
	// anchors a new handle.
	m_viewport.MarkOverlaysDirty();
	UploadSceneResources(); // meshes, lights, debug meshes, IBL, light SSBO

	// The graph record was read before the pool and the scene were restored, so
	// it is applied last, when its shaders can be resolved.
	if (m_pendingGraph)
	{
		ReplaceRenderGraph(*m_pendingGraph);
		m_pendingGraph.reset();
	}
	else
	{
		ResetRenderGraph();
	}

	// A loaded document is the saved baseline, not an unsaved edit.
	m_dirty = false;
}

void Editor::ResetRenderGraph()
{
	m_graphProblems.clear();
	m_passShaders.reset();  // the pool was cleared; the old identities are stale
	m_passShaders = std::make_unique<PassShaders>(
		*m_resources, &LoadPassKindShader, m_graphProblems);

	m_renderGraph = MakeDefaultCpuGraph(*m_passShaders);
	AssignNodeShaders();
	PublishRenderGraph();
}

void Editor::AssignNodeShaders()
{
	for (const auto& node : m_renderGraph.nodes)
	{
		if (node->data.shaderUid != 0 && m_resources->Contains(node->data.shaderUid))
			continue;
		node->data.shaderUid = m_passShaders->ShaderUid(node->data.pass);
	}
}

void Editor::ReplaceRenderGraph(const CpuGraphRecord& record)
{
	m_graphProblems.clear();
	m_passShaders.reset();  // adopt the restored shaders by name
	m_passShaders = std::make_unique<PassShaders>(
		*m_resources, &LoadPassKindShader, m_graphProblems);

	std::vector<std::string> loadProblems;
	m_renderGraph = FromRecord(record, *m_passShaders, loadProblems);
	m_graphProblems.insert(m_graphProblems.end(), loadProblems.begin(), loadProblems.end());

	AssignNodeShaders();
	PublishRenderGraph();
}

void Editor::SetRenderGraph(CpuRenderGraph graph)
{
	if (!m_passShaders)
	{
		m_passShaders = std::make_unique<PassShaders>(
			*m_resources, &LoadPassKindShader, m_graphProblems);
	}

	m_renderGraph = std::move(graph);
	AssignNodeShaders();
	MarkDirty();
	PublishRenderGraph();
}

void Editor::PublishRenderGraph()
{
	if (!ed_renderer)
		return;

	for (const CpuGraphDiagnostic& diagnostic : ValidateCpuGraph(m_renderGraph, *m_passShaders))
	{
		NEURUS_ERR("[Editor] render graph: " << diagnostic.node << "." << diagnostic.port
		           << ": " << diagnostic.message);
	}

	// Snapshot every shader the document names, including the second shader of
	// the shadow passes, so the renderer never reads the Editor's pool. A shader
	// that fails to snapshot is reported and left out; the renderer then rejects
	// the node by name instead of drawing against a stale shader.
	std::vector<ShaderSnapshot> snapshots;
	std::unordered_map<int, bool> seen;
	for (const auto& node : m_renderGraph.nodes)
	{
		const PassKindInfo* kind = FindPassKind(node->data.pass);
		const std::span<const uint32_t> optional =
			kind ? kind->optionalBindings : std::span<const uint32_t>{};

		for (const int uid : m_passShaders->ShaderUids(node->data.pass))
		{
			if (!seen.emplace(uid, true).second)
				continue;

			const auto shader = m_resources->Get<ShaderData>(uid);
			if (!shader)
				continue;

			try
			{
				snapshots.push_back(MakeShaderSnapshot(*shader, optional));
			}
			catch (const std::exception& error)
			{
				NEURUS_ERR("[Editor] render graph: cannot snapshot shader " << uid
				           << " for node '" << node->name << "': " << error.what());
			}
		}
	}

	ed_renderer->Post(CompileRenderGraphRequest{
		std::make_shared<const CpuRenderGraph>(CloneGraph(m_renderGraph)),
		std::move(snapshots)});
}

void Editor::OnMeshImport(const std::string& path)
{
	std::string error;
	const int uid = ImportMeshObject(path, {}, error);
	if (uid == 0)
		NEURUS_ERR("Failed to import mesh: " << error);
}

void Editor::OnMeshImportRequested(const MeshImportRequested& e)
{
	ObjectCreateResult local;
	ObjectCreateResult& result = e.result != nullptr ? *e.result : local;
	result.uid = ImportMeshObject(e.path, e.name, result.error);
	if (result.uid == 0 && e.result == nullptr)
		NEURUS_ERR("Failed to import mesh: " << result.error);
}

int Editor::ImportMeshObject(const std::string& path, const std::string& name,
                             std::string& error)
{
	try {
		// A missing file is not an empty mesh. Check before the load: the caller
		// (command / GUI / script) reports the message verbatim, and nothing may
		// enter the pool for a file we could not even open. ResolveAssetPath
		// returns the path as given when nothing matches, so this still tests
		// what the caller named.
		std::error_code fsError;
		if (!std::filesystem::is_regular_file(ResolveAssetPath(path), fsError))
		{
			error = "Mesh file not found: " + path;
			return 0;
		}

		// Import = load the resource into the pool; the Add operation and the
		// MeshGPU upload happen on demand when the add is processed - no inline
		// upload here. A portable relative "res/..." path is stored when the file
		// lives under the res dir; paths outside it are stored as-is (warned).
		const std::string storedPath = MakePortableAssetPath(path);
		if (storedPath.rfind("res/", 0) != 0)
		{
			NEURUS_ERR("[Editor] Mesh import outside asset dir is stored as-is (non-portable): " << path);
		}

		auto meshData = m_resources->Load<MeshData>(storedPath);
		if (!meshData || meshData->GetVertexCount() == 0 || meshData->GetIndexCount() == 0)
		{
			// Project save serializes the whole pool, so a geometry-less MeshData
			// would be written out and re-parsed on every later open. Load always
			// creates a fresh UID, so removing this one cannot evict an earlier
			// successful load from the same path.
			if (meshData)
				m_resources->Remove(meshData->GetObjectID());
			error = "Mesh file contains no geometry: " + storedPath;
			return 0;
		}
		auto mesh = m_resources->Load<Mesh>(meshData);

		if (!name.empty())
		{
			const std::string unique = m_resources->SetName(mesh->GetObjectID(), name);
			mesh->o_name = unique.empty() ? name : unique;
		}

		ed_eventBus.Emit(SceneObjectAddRequested{mesh->GetObjectID()});
		NEURUS_LOG("[Editor] Imported mesh: " << storedPath << " as " << mesh->o_name);
		return mesh->GetObjectID();
	}
	catch (const std::exception& e) {
		error = e.what();
		return 0;
	}
}

void Editor::OnShaderCreate(const ShaderCreateRequested& e)
{
	ShaderRequestResult local;
	ShaderRequestResult& result = e.result != nullptr ? *e.result : local;

	auto fail = [&](std::string message) {
		result.error = std::move(message);
		if (e.result == nullptr) NEURUS_ERR("[Editor] Shader create failed: " << result.error);
		neurus::notify::Repaint(ed_eventBus);
	};

	auto mesh = m_resources->Get<Mesh>(e.objectUid);
	if (!mesh)
	{
		fail("shader target is not a mesh");
		return;
	}

	// Stage resolution/parsing lives in ShaderLibrary (via ShaderData for a
	// pooled source); the pool write stays here, where the Editor owns it.
	std::string error;
	auto vert = ResolveShaderStage(*m_resources, e.vertexSourceUid, e.vertexPath,
	                               ShaderType::VERTEX, kDefaultVertexPath, error);
	if (!vert)
	{
		fail(error);
		return;
	}
	auto frag = ResolveShaderStage(*m_resources, e.fragmentSourceUid, e.fragmentPath,
	                               ShaderType::FRAGMENT, kDefaultFragmentPath, error);
	if (!frag)
	{
		fail(error);
		return;
	}

	// Both stages are resolved first, so a shader is only pooled once it has
	// compiled: a failure leaves no half-made entry and nothing to undo.
	const std::string shaderName = "MeshShader_" + std::to_string(mesh->GetObjectID());
	auto shader = std::make_shared<RenderShader>(shaderName, std::move(*vert), std::move(*frag));

	std::string compileError;
	if (!shader->CompileToSpv(&compileError))
	{
		fail(compileError.empty() ? "shader compile failed" : compileError);
		return;
	}

	const int shaderUid = PoolShader(*m_resources, shader, shaderName);
	result.shaderUid = shaderUid;

	ed_eventBus.Emit(ObjectShaderAddRequested{ e.objectUid, shaderUid });
	ed_eventBus.Emit(ShaderCompiled{ shaderUid });
	neurus::notify::Mutated(ed_eventBus);
}

void Editor::OnShaderCreateEvent(const ShaderCreateEvent& e)
{
	ShaderCreateRequested request;
	request.objectUid = e.objectUid;
	OnShaderCreate(request);
}

void Editor::OnShaderImportRequested(const ShaderImportRequested& e)
{
	ShaderRequestResult local;
	ShaderRequestResult& result = e.result != nullptr ? *e.result : local;

	auto fail = [&](std::string message) {
		result.error = std::move(message);
		if (e.result == nullptr) NEURUS_ERR("[Editor] Shader load failed: " << result.error);
		neurus::notify::Repaint(ed_eventBus);
	};

	const auto stage = ShaderTypeFromPath(e.path);
	if (!stage || *stage == ShaderType::GEOMETRY)
	{
		fail(!stage ? "unsupported shader extension: " + e.path
		            : "geometry shaders are not supported: " + e.path);
		return;
	}

	const std::string shaderName = e.name.empty() ? e.path : e.name;
	try
	{
		// Parsed off-pool, then registered: a parse failure pools nothing, so
		// there is no entry to roll back and no half-made resource in a save.
		std::shared_ptr<ShaderData> shader;

		if (*stage == ShaderType::COMPUTE)
		{
			auto compute = std::make_shared<ComputeShader>(shaderName, e.path);
			shader = compute->ParseAndGenerate() ? compute : nullptr;
		}
		else
		{
			const std::string vertPath =
				*stage == ShaderType::VERTEX ? e.path : kDefaultVertexPath;
			const std::string fragPath =
				*stage == ShaderType::FRAGMENT ? e.path : kDefaultFragmentPath;
			auto render = std::make_shared<RenderShader>(shaderName, vertPath, fragPath);
			shader = render->ParseAndGenerate() ? render : nullptr;
		}

		if (!shader)
		{
			fail("failed to parse shader: " + e.path);
			return;
		}

		result.shaderUid = PoolShader(*m_resources, shader, shaderName);
	}
	catch (const std::exception& ex)
	{
		fail(ex.what());
		return;
	}

	neurus::notify::Mutated(ed_eventBus);
}

void Editor::OnCameraAdd()
{
	std::string error;
	if (AddCameraObject({}, error) == 0)
		NEURUS_ERR("Failed to add camera: " << error);
}

void Editor::OnCameraAddRequested(const CameraAddRequested& e)
{
	ObjectCreateResult local;
	ObjectCreateResult& result = e.result != nullptr ? *e.result : local;
	result.uid = AddCameraObject(e.name, result.error);
	if (result.uid == 0 && e.result == nullptr)
		NEURUS_ERR("Failed to add camera: " << result.error);
}

int Editor::AddCameraObject(const std::string& name, std::string& error)
{
	try {
		auto camera = m_resources->Load<Camera>();
		camera->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
		// SetTarPos, not a raw cam_tar write: the view matrix is cached eagerly, so
		// assigning the field would leave the cache looking at the old target.
		camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
		if (!name.empty())
		{
			const std::string unique = m_resources->SetName(camera->GetObjectID(), name);
			camera->o_name = unique.empty() ? name : unique;
		}
		ed_eventBus.Emit(SceneObjectAddRequested{camera->GetObjectID()});
		NEURUS_LOG("[Editor] Added camera at (0, -5, 2) as " << camera->o_name);
		return camera->GetObjectID();
	}
	catch (const std::exception& e) {
		error = e.what();
		return 0;
	}
}

int Editor::DuplicateObject(int sourceUid, const std::string& name, std::string& error)
{
	try {
		auto source = m_resources->Get<ObjectID>(sourceUid);
		if (!source)
		{
			error = "No pooled object with uid " + std::to_string(sourceUid) + ".";
			return 0;
		}
		if (source->IsLight())
		{
			error = "Duplicating lights is disabled while issue #60 is open.";
			return 0;
		}

		int uid = 0;
		if (auto* camera = dynamic_cast<Camera*>(source.get()))
		{
			auto copy = m_resources->Load<Camera>(
				camera->cam_w, camera->cam_h, camera->cam_pers, camera->cam_near, camera->cam_far);
			copy->SetPosition(camera->GetPosition());
			copy->SetRotation(camera->GetRotation());
			copy->SetScale(camera->GetScale());
			copy->SetTarPos(camera->cam_tar);
			uid = copy->GetObjectID();
		}
		else if (auto* mesh = dynamic_cast<Mesh*>(source.get()))
		{
			auto copy = m_resources->Load<Mesh>(mesh->o_mesh);
			copy->SetPosition(mesh->GetPosition());
			copy->SetRotation(mesh->GetRotation());
			copy->SetScale(mesh->GetScale());
			copy->using_shadow = mesh->using_shadow;
			copy->using_material = mesh->using_material;
			copy->using_sdf = mesh->using_sdf;
			copy->is_closure = mesh->is_closure;
			copy->o_material = mesh->o_material;
			if (mesh->o_shader)
				copy->SetObjShader(mesh->o_shader);
			uid = copy->GetObjectID();
		}
		else
		{
			error = "Duplicating this object type is not supported.";
			return 0;
		}

		auto pooled = m_resources->Get<ObjectID>(uid);
		if (!pooled)
		{
			error = "The duplicate resource is missing.";
			return 0;
		}
		const std::string resolvedName = name.empty() ? source->o_name + " Copy" : name;
		const std::string unique = m_resources->SetName(uid, resolvedName);
		pooled->o_name = unique.empty() ? resolvedName : unique;

		ed_eventBus.Emit(SceneObjectAddRequested{uid});
		return uid;
	}
	catch (const std::exception& e) {
		error = e.what();
		return 0;
	}
}

void Editor::OnObjectDuplicateRequested(const ObjectDuplicateRequested& e)
{
	ObjectCreateResult local;
	ObjectCreateResult& result = e.result != nullptr ? *e.result : local;
	result.uid = DuplicateObject(e.sourceUid, e.name, result.error);
	if (result.uid == 0 && e.result == nullptr)
		NEURUS_ERR("Failed to duplicate object: " << result.error);
}

void Editor::OnLightAdd()
{
	try {
		auto light = m_resources->Load<Light>(
			neurus::POINTLIGHT, 10.0f, glm::vec3(1.0f));
		light->SetPosition(glm::vec3(3.0f, 3.0f, 3.0f));
		light->SetRadius(0.01f);
		// The shadow-map upload happens on demand when the add is processed; the
		// light SSBO rebuild happens via LightingRebuild after the controller
		// registers the light in the scene.
		ed_eventBus.Emit(SceneObjectAddRequested{light->GetObjectID()});
		NEURUS_LOG("[Editor] Added point light at (3, 3, 3)");
	}
	catch (const std::exception& e) {
		NEURUS_ERR("Failed to add light: " << e.what());
	}
}

void Editor::OnSunLightAdd()
{
	try {
		auto light = m_resources->Load<Light>(
			neurus::SUNLIGHT, 5.0f, glm::vec3(1.0f, 0.95f, 0.8f));
		light->SetPosition(glm::vec3(0.0f, 0.0f, 10.0f));
		light->SetRotation(glm::vec3(-90.0f, 0.0f, 0.0f));
		light->use_shadow = true;
		// The shadow-map upload happens on demand when the add is processed; the
		// SSBO rebuild via LightingRebuild.
		ed_eventBus.Emit(SceneObjectAddRequested{light->GetObjectID()});
		NEURUS_LOG("[Editor] Added sun light at (0, 0, 10)");
	}
	catch (const std::exception& e) {
		NEURUS_ERR("Failed to add sun light: " << e.what());
	}
}

void Editor::OnSpotLightAdd()
{
	try {
		auto light = m_resources->Load<Light>(
			neurus::SPOTLIGHT, 30.0f, glm::vec3(1.0f, 0.75f, 0.4f));
		light->SetPosition(glm::vec3(0.0f, 0.0f, 6.0f));
		light->SetRotation(glm::vec3(-90.0f, 0.0f, 0.0f));
		light->SetRadius(0.01f);
		light->SetCutoff(0.95f);        // ~18 deg inner cone half-angle
		light->SetOuterCutoff(0.85f);   // ~32 deg outer cone half-angle
		light->use_shadow = true;
		// The shadow-map upload happens on demand when the add is processed; the
		// SSBO rebuild via LightingRebuild.
		ed_eventBus.Emit(SceneObjectAddRequested{light->GetObjectID()});
		NEURUS_LOG("[Editor] Added spot light at (0, 0, 6) pointing down");
	}
	catch (const std::exception& e) {
		NEURUS_ERR("Failed to add spot light: " << e.what());
	}
}

/**
 * @brief Uploads the GPU resources for an object entering the scene.
 *
 * GPU caches are scene-scoped, so objects re-added live or replayed from undo -
 * or a light whose shadow was just enabled - may lack cached resources. Upload
 * on demand through the shared helpers (skip if already cached); light SSBO
 * updates stay with LightingRebuild.
 */
void Editor::OnSceneObjectGpuUpload(int objectUid)
{
	if (!ed_renderer || objectUid == 0) return;

	if (auto mesh = m_resources->Get<Mesh>(objectUid))
	{
		if (mesh->o_mesh) PostMeshUpload(*ed_renderer, mesh->GetObjectID(), mesh->o_mesh);
	}
	else if (auto light = m_resources->Get<Light>(objectUid))
	{
		PostLightUpload(*ed_renderer, *light);
	}
	else if (auto env = m_resources->Get<Environment>(objectUid))
	{
		// regenerate=true: an environment entering the scene is a change, and its
		// cubemaps (if any cached under a reused id) are stale.
		PostEnvironmentUpload(*ed_renderer, *env, true);
	}
	else if (auto dMesh = m_resources->Get<DebugMesh>(objectUid))
	{
		if (dMesh->HasGeometry()) PostMeshUpload(*ed_renderer, dMesh->GetObjectID(), dMesh->o_mesh);
	}
}

void Editor::UploadEnvironmentIBL()
{
	Scene* scene = m_scene.get();
	if (!scene)
	{
		NEURUS_ERR("[Editor] UploadEnvironmentIBL: no scene available");
		return;
	}
	if (!ed_renderer)
	{
		NEURUS_ERR("[Editor] UploadEnvironmentIBL: Renderer not available");
		return;
	}

	// IBL is only enabled when the project provides an environment.
	if (scene->env_list.empty())
	{
		NEURUS_LOG("[Editor] No environment in scene - IBL disabled (black background)");
		return;
	}

	auto env = scene->env_list.begin()->second;
	const int envId = env->GetObjectID();

	// Posted with regenerate=false: the renderer skips an environment already
	// resident, because regenerating cubemaps is by far the most expensive step
	// of a scene upload, and residency is a fact only the renderer can see now.
	// The skip stays sound across a scene replacement: this scene's load posted
	// DropSceneResourcesRequest first, and FIFO execution evicts the old cache
	// before this upload runs - so a UID restored from the project file never
	// finds the previous scene's entry stale under it.
	NEURUS_LOG("[Editor] Using environment (ID " << envId << ")");

	// If the pooled pixels are unavailable (missing source file), the upload path
	// falls back to the procedural gradient.
	if (!env->GetEquirectData() || !env->GetEquirectData()->IsValid())
	{
		NEURUS_LOG("[Editor] Environment has no valid equirect data, using procedural fallback");
	}

	PostEnvironmentUpload(*ed_renderer, *env, false);
}

void Editor::UploadSceneResources()
{
	if (!ed_renderer) return;

	// Scene-scoped upload: GPU resources mirror the SCENE, not the pool, so memory
	// stays proportional to the scene. Pooled objects not in the scene are skipped
	// and uploaded on demand when re-added.
	for (const auto& [id, mesh] : m_scene->mesh_list)
	{
		if (!mesh || !mesh->o_mesh) continue;
		PostMeshUpload(*ed_renderer, mesh->GetObjectID(), mesh->o_mesh);

		// Scene load compiles shaders inside cereal's hook, which cannot reach
		// the event bus, so no ShaderCompiled fires. Warm each mesh's pipeline
		// once here instead.
		if (mesh->o_shader)
			ed_renderer->Post(MakeEnsurePipelineRequest(*mesh->o_shader));
	}

	for (const auto& [uid, light] : m_scene->light_list)
	{
		if (!light) continue;
		PostLightUpload(*ed_renderer, *light);
	}

	// DebugMesh wireframes need a MeshGPU too: the consumer skips any wire mesh
	// whose id resolves to no cached geometry.
	for (const auto& [uid, dMesh] : m_scene->dMesh_list)
	{
		if (!dMesh || !dMesh->HasGeometry()) continue;
		PostMeshUpload(*ed_renderer, dMesh->GetObjectID(), dMesh->o_mesh);
	}

	// The environment is scene content as well: without its cubemaps the scene
	// renders unlit.
	UploadEnvironmentIBL();

	// The light SSBO remains a scene projection.
	UploadLighting();

	NEURUS_LOG("[Editor] Uploaded scene resources to GPU");
}

/**
 * @brief Posts the request that releases the outgoing scene's GPU resources.
 *
 * The drain and the eviction happen renderer-side now. What the Editor still
 * guarantees is ORDER: this post precedes the new scene's upload posts, so FIFO
 * execution evicts the old scene before the new one uploads. That matters
 * because the cache is keyed by object UID and UIDs are restored from the
 * project file, so a reused id would otherwise shadow whatever comes back under
 * it. Both scene-replacing paths call this before clearing the pool.
 */
void Editor::DropSceneGpuResources()
{
	if (!ed_renderer) return;
	ed_renderer->Post(DropSceneResourcesRequest{});
}

void Editor::UploadLighting()
{
	if (!ed_renderer) return;
	// Pure CPU conversion of the whole light map here; the renderer rebuilds the
	// SSBOs from the value it receives.
	ed_renderer->Post(UpdateLightingRequest{UploadManager::UploadLighting(m_scene->light_list)});
}

// =========================================================================
// Edit() - drain the UI boundary's deferred events
// =========================================================================

void Editor::Edit()
{
	// Publish BEFORE the queue is drained, so anything that arrives this frame
	// (a click, a script write) mutates the camera the renderer is about to use.
	PublishActiveCamera();

	ed_eventBus.Process();

	// Publish AGAIN, because activation, deactivation, deletion and undo/redo all
	// land inside Process() and can change which camera the frame belongs to. The
	// overlay rebuild below must use the camera this frame ended up with.
	PublishActiveCamera();

	// After the queue is drained, so a scene change and its overlay consequences
	// land in the same frame. Which overlays are dirty and what they anchor on are
	// viewport questions - the Editor's part is only the timing.
	if (m_scene && m_viewport.RebuildOverlays(*m_scene))
	{
		// The rebuilt payload only reaches the screen if the renderer draws again,
		// and once accumulation has converged it draws nothing on its own. Posted
		// here rather than in the handlers that dirty the overlays: the rebuild is
		// what proves the overlay content actually changed, so an idle tick posts
		// nothing. It is still inside Edit(), i.e. before this tick's snapshot is
		// published (RenderRequestSink.hpp) - posting after would cost one tick.
		if (ed_renderer)
			ed_renderer->Post(RedrawOverlaysRequest{});
	}
}

} // namespace neurus

