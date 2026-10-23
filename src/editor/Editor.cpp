#include "Editor.h"

#include "editor/events/InputEvents.h"
#include "editor/events/AssetEvents.h"
#include "editor/events/OperationEvents.h"
#include "editor/events/ConfigEvents.h"
#include "editor/events/CameraEvents.h"
#include "editor/events/EditorEvents.h"
#include "editor/events/GizmoEvents.h"
#include "editor/events/ShaderEvents.h"
#include "editor/events/SceneEvents.h"

#include "editor/controllers/CameraController.h"
#include "editor/controllers/RenderConfigController.h"
#include "editor/controllers/ShaderController.h"
#include "editor/controllers/SceneController.h"
#include "editor/controllers/TransformGizmoController.h"
#include "editor/operations/ShaderOperations.h"
#include "editor/events/EventBus.h"

#include "render/RenderRequestSink.h"
#include "render/RenderRequests.h"
#include "render/UploadManager.h"

#include "render/RenderFrameSnapshot.h"
#include "render/shaders/RenderShader.h"
#include "render/shaders/ShaderLibrary.h"
#include "ui/UIContext.h"

#include "core/Log.h"
#include "asset/data/ImageData.h"
#include "asset/data/AssetPath.h"
#include "asset/data/MeshData.h"
#include "scene/Camera.h"
#include "scene/DebugLine.h"
#include "scene/DebugMesh.h"
#include "scene/DebugPoints.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"
#include "scene/Transform.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
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
 * The intent events carry a uid rather than a pointer; 0 is legal to enqueue,
 * and the controller drops an intent it cannot resolve.
 */
int ActiveUid(const Scene& scene)
{
	const ObjectID* active = scene.selections.GetActiveObject();
	return active ? active->GetObjectID() : 0;
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

	ed_eventBus.subscribe<MeshImportEvent>([this](const MeshImportEvent& e) {
		ed_eventBus.enqueue(RenderResetEvent{});
		OnMeshImport(e.path);
	});
	ed_eventBus.subscribe<CameraAddEvent>([this](const CameraAddEvent&) {
		ed_eventBus.enqueue(RenderResetEvent{});
		OnCameraAdd();
	});
	ed_eventBus.subscribe<LightAddEvent>([this](const LightAddEvent&) {
		ed_eventBus.enqueue(RenderResetEvent{});
		OnLightAdd();
	});
	ed_eventBus.subscribe<SunLightAddEvent>([this](const SunLightAddEvent&) {
		ed_eventBus.enqueue(RenderResetEvent{});
		OnSunLightAdd();
	});
	ed_eventBus.subscribe<SpotLightAddEvent>([this](const SpotLightAddEvent&) {
		ed_eventBus.enqueue(RenderResetEvent{});
		OnSpotLightAdd();
	});

	// Shader creation constructs a pooled RenderShader, so it is Editor-owned
	// (like mesh/light/camera adds); ShaderController keeps only pool-free
	// handlers.
	ed_eventBus.subscribe<ShaderCreateRequested>([this](const ShaderCreateRequested& e) {
		ed_eventBus.enqueue(RenderResetEvent{});
		OnCreateShader(e);
	});

	// Undo/redo of Create Shader: redo relinks the pooled RenderShader by UID;
	// undo drops the reference (the pool keeps the shader). No version bump: the
	// per-mesh (objectId, version) pipeline cache handles the relink.
	ed_eventBus.subscribe<ShaderLinkRestored>([this](const ShaderLinkRestored& e) {
		auto mesh = m_resources->Get<Mesh>(e.objectUid);
		if (!mesh) return;
		auto shader = m_resources->Get<RenderShader>(e.shaderId);
		if (!shader)
		{
			NEURUS_ERR("[Editor] ShaderLinkRestored: shader " << e.shaderId << " not pooled");
			return;
		}
		mesh->SetObjShader(shader);
		ed_eventBus.enqueue(RenderResetEvent{});
	});
	ed_eventBus.subscribe<ShaderUnlinkRestored>([this](const ShaderUnlinkRestored& e) {
		auto mesh = m_resources->Get<Mesh>(e.objectUid);
		if (!mesh) return;
		mesh->SetObjShader(nullptr);
		ed_eventBus.enqueue(RenderResetEvent{});
	});

	// Undo/redo replay their inverse events synchronously (never queued), so
	// the mutation applies in-place and cannot reorder against live input.
	ed_eventBus.subscribe<UndoRequested>([this](const UndoRequested&) {
		ed_operations.Undo();
	});
	ed_eventBus.subscribe<RedoRequested>([this](const RedoRequested&) {
		ed_operations.Redo();
	});


	// --- Register controllers ---
	// All five take only the ControllerContext, which carries the event dispatch,
	// the pooled-object lookup, the operation sink, the scene, the render config,
	// the viewport and the gizmo state.
	//
	// TransformGizmoController comes AFTER CameraController on purpose: handlers
	// dispatch in registration order, so the gizmo's MouseMoveEvent handler sees a
	// camera the camera controller has already updated this frame.
	RegisterController<CameraController>();
	RegisterController<TransformGizmoController>();
	RegisterController<ShaderController>();
	RegisterController<SceneController>();
	RegisterController<RenderConfigController>();

	// --- Keep the viewport's camera pointer alive across a deletion ---
	// Registered after the controllers so it runs after SceneController's own
	// handler has actually removed the object. Edit() syncs the camera once per
	// frame, but Process() drains re-entrantly enqueued events in the same call,
	// so deleting the activated camera mid-Process would otherwise leave the
	// viewport holding a dangling pointer for the rest of that Edit().
	ed_eventBus.subscribe<SceneObjectDeleteRequested>([this](const SceneObjectDeleteRequested&) {
		m_viewport.SyncCamera(GetScene());
	});

	// --- Subscribe to EnvironmentChanged to regenerate IBL cubemaps on demand ---
	ed_eventBus.subscribe<EnvironmentChanged>([this](const EnvironmentChanged& e) {
		ed_eventBus.enqueue(RenderResetEvent{});
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
	ed_eventBus.subscribe<SceneObjectGpuUploadRequested>([this](const SceneObjectGpuUploadRequested& e) {
		OnSceneObjectGpuUpload(e.objectUid);
	});

	ed_eventBus.subscribe<MouseMoveEvent>([this](const MouseMoveEvent& e) {
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
				ed_eventBus.enqueue(CameraPushEvent{cam->GetObjectID(), e.delta.x, e.delta.y});
			else if (e.modifiers & Input::Mod_Shift)
				ed_eventBus.enqueue(CameraSlideEvent{cam->GetObjectID(), e.delta.x, e.delta.y});
			else
				ed_eventBus.enqueue(CameraRotateEvent{cam->GetObjectID(), e.delta.x, e.delta.y});
		}
	});

	// Middle-button press/release bound the orbit/pan/dolly drag gesture so it
	// collapses to one undo entry. Left and right close a modal transform gesture
	// instead - enqueued unconditionally and dropped by the controller when no
	// gesture is live.
	ed_eventBus.subscribe<MousePressEvent>([this](const MousePressEvent& e) {
		if (e.button == Input::Left)  { ed_eventBus.enqueue(GizmoConfirmed{}); return; }
		if (e.button == Input::Right) { ed_eventBus.enqueue(GizmoCancelled{}); return; }
		if (e.button != Input::Middle) return;
		if (const Camera* cam = m_viewport.GetCamera())
			ed_eventBus.enqueue(CameraDragBegin{cam->GetObjectID()});
	});
	ed_eventBus.subscribe<MouseReleaseEvent>([this](const MouseReleaseEvent& e) {
		if (e.button != Input::Middle) return;
		if (const Camera* cam = m_viewport.GetCamera())
			ed_eventBus.enqueue(CameraDragEnd{cam->GetObjectID()});
	});

	ed_eventBus.subscribe<MouseScrollEvent>([this](const MouseScrollEvent& e) {
		const Camera* cam = m_viewport.GetCamera();
		if (!cam) return;

		if (std::abs(e.delta) > 0.001f)
			ed_eventBus.enqueue(CameraZoomEvent{cam->GetObjectID(), e.delta});
	});

	// --- Pure UI->Editor intents: wrap the active scene, forward dedicated events ---
	ed_eventBus.subscribe<ObjectClicked>([this](const ObjectClicked& e) {
		ed_eventBus.enqueue(ObjectSelected{ e.objectUid, e.modifiers });
	});
	ed_eventBus.subscribe<DeleteRequested>([this](const DeleteRequested&) {
		ed_eventBus.enqueue(ObjectDeleteRequested{});
	});

	// --- Viewport keystrokes -> modal transform intents ---
	// Every branch enqueues UNCONDITIONALLY: Process() is FIFO and drains
	// re-entrantly enqueued events within the same call, so an IsActive() gate
	// would silently eat an axis key that arrived in the same frame as its mode
	// key. The controller is the single place that decides an intent is meaningless.
	ed_eventBus.subscribe<KeyPressEvent>([this](const KeyPressEvent& e) {
		switch (e.key)
		{
		case Input::Key_G: ed_eventBus.enqueue(GizmoModeRequested{ActiveUid(GetScene()), GizmoMode::Move});   break;
		case Input::Key_R: ed_eventBus.enqueue(GizmoModeRequested{ActiveUid(GetScene()), GizmoMode::Rotate}); break;
		case Input::Key_S: ed_eventBus.enqueue(GizmoModeRequested{ActiveUid(GetScene()), GizmoMode::Scale});  break;

		case Input::Key_X: ed_eventBus.enqueue(GizmoAxisRequested{GizmoAxis::X}); break;
		case Input::Key_Y: ed_eventBus.enqueue(GizmoAxisRequested{GizmoAxis::Y}); break;
		case Input::Key_Z: ed_eventBus.enqueue(GizmoAxisRequested{GizmoAxis::Z}); break;

		// W is the fourth member of that set, not a separate feature: it names the
		// screen as the constraint, which is exactly GizmoAxis::None - the view plane
		// for Move, uniform for Scale, the view axis for Rotate.
		case Input::Key_W: ed_eventBus.enqueue(GizmoAxisRequested{GizmoAxis::None}); break;

		case Input::Key_Return: ed_eventBus.enqueue(GizmoConfirmed{}); break;
		case Input::Key_Escape: ed_eventBus.enqueue(GizmoCancelled{}); break;

		default: break;
		}
	});

	// --- Subscribe to RenderResetEvent to reset temporal accumulation ---
	ed_eventBus.subscribe<RenderResetEvent>([this](const RenderResetEvent&) {
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
	// and warms. The event is enqueued from the compile handlers and drained in
	// this same Edit(), before the frame is handed off, so no frame draws with
	// the pass default. Keyed on the shader's object id.
	ed_eventBus.subscribe<ShaderCompiled>([this](const ShaderCompiled& e) {
		if (!ed_renderer) return;
		auto shader = m_resources->Get<Shader>(e.shaderObjectId);
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
	ed_eventBus.subscribe<ObjectSelected>([this](const ObjectSelected&) {
		m_viewport.MarkGizmoDirty();
	});
	ed_eventBus.subscribe<ObjectDeselected>([this](const ObjectDeselected&) {
		m_viewport.MarkGizmoDirty();
	});
	ed_eventBus.subscribe<SelectionChanged>([this](const SelectionChanged&) {
		m_viewport.MarkGizmoDirty();
	});

	// --- SceneController GPU-sync + dirty subscriptions ---
	ed_eventBus.subscribe<SceneModified>([this](const SceneModified&) {
		m_dirty = true;
	});

	ed_eventBus.subscribe<LightGpuChanged>([this](const LightGpuChanged& e) {
		if (!ed_renderer) return;
		auto light = m_resources->Get<Light>(e.objectUid);
		if (!light) return;
		// Pure CPU conversion here, then the renderer rewrites the one SSBO entry.
		ed_renderer->Post(UpdateLightRequest{e.objectUid, UploadManager::UploadLighting(*light)});
	});

	ed_eventBus.subscribe<LightingRebuild>([this](const LightingRebuild&) {
		UploadLighting();
	});

	NEURUS_LOG("[Editor] Initialized");
}

Scene& Editor::GetScene()
{
	return *m_scene;
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
		m_viewport.SyncCamera(*m_scene);

		// An empty document is its own saved baseline, not an unsaved edit.
		m_dirty = false;

		// New scene => the old flattened overlay is stale, and so is the handle: it
		// is anchored on the outgoing document's selection, and the builder only
		// clears its list inside a dirty Rebuild().
		m_viewport.MarkOverlaysDirty();

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
	m_viewport.SyncCamera(*m_scene);
	// Debug objects came back from the project file, and the restored selection
	// anchors a new handle.
	m_viewport.MarkOverlaysDirty();
	UploadSceneResources(); // meshes, lights, debug meshes, IBL, light SSBO
}


void Editor::OnMeshImport(const std::string& path)
{
	try {
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
		auto mesh = m_resources->Load<Mesh>(meshData);

		ed_eventBus.enqueue(SceneObjectAddRequested{mesh->GetObjectID()});
		NEURUS_LOG("[Editor] Imported mesh: " << storedPath);
	}
	catch (const std::exception& e) {
		NEURUS_ERR("Failed to import mesh: " << e.what());
	}
}

void Editor::OnCameraAdd()
{
	try {
		auto camera = m_resources->Load<Camera>();
		camera->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
		// SetTarPos, not a raw cam_tar write: the view matrix is cached eagerly, so
		// assigning the field would leave the cache looking at the old target.
		camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
		ed_eventBus.enqueue(SceneObjectAddRequested{camera->GetObjectID()});
		NEURUS_LOG("[Editor] Added camera at (0, -5, 2)");
	}
	catch (const std::exception& e) {
		NEURUS_ERR("Failed to add camera: " << e.what());
	}
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
		ed_eventBus.enqueue(SceneObjectAddRequested{light->GetObjectID()});
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
		ed_eventBus.enqueue(SceneObjectAddRequested{light->GetObjectID()});
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
		ed_eventBus.enqueue(SceneObjectAddRequested{light->GetObjectID()});
		NEURUS_LOG("[Editor] Added spot light at (0, 0, 6) pointing down");
	}
	catch (const std::exception& e) {
		NEURUS_ERR("Failed to add spot light: " << e.what());
	}
}

void Editor::OnCreateShader(const ShaderCreateRequested& e)
{
	auto mesh = m_resources->Get<Mesh>(e.objectUid);
	if (!mesh)
	{
		NEURUS_ERR("[Editor] OnCreateShader: not a mesh");
		return;
	}
	if (mesh->o_shader)
	{
		NEURUS_LOG("[Editor] Mesh already has a shader");
		return;
	}

	const int objectId = mesh->GetObjectID();
	const std::string shaderName = "MeshShader_" + std::to_string(objectId);

	try
	{
		// Load<T> constructs + registers the pooled RenderShader; parse explicitly
		// here. Paths use the "res/..." prefix, resolved from the working directory
		// by the shader pipeline.
		auto shader = m_resources->Load<RenderShader>(
			shaderName, "res/shaders/render/gbuffer.vert", "res/shaders/render/gbuffer.frag");
		if (!shader->ParseAndGenerate())
		{
			NEURUS_ERR("[Editor] Failed to create default shader for mesh " << objectId);
			m_resources->Remove(shader->GetObjectID());
			return;
		}

		mesh->SetObjShader(shader); // o_shader + o_shaderId (pooled reference)

		// The link is the undoable fact: a pool-preserving membership toggle -
		// undo drops the reference (the pool keeps the shader), redo relinks it.
		ed_operations.Submit(std::make_unique<ShaderLinkOp>(
			mesh->GetObjectID(), shader->GetObjectID(), true));

		// Compile both stages to SPIR-V and bump version on success.
		auto& s = *mesh->o_shader;
		bool allOk = true;
		if (s.HasStage(ShaderType::VERTEX))
		{
			auto& unit = s.GetStage(ShaderType::VERTEX);
			unit.spv = ShaderLibrary::Compile(unit, ShaderType::VERTEX, s.GetName());
			if (unit.spv.empty()) { allOk = false; }
			else { unit.BumpVersion(); }
		}
		if (s.HasStage(ShaderType::FRAGMENT))
		{
			auto& unit = s.GetStage(ShaderType::FRAGMENT);
			unit.spv = ShaderLibrary::Compile(unit, ShaderType::FRAGMENT, s.GetName());
			if (unit.spv.empty()) { allOk = false; }
			else { unit.BumpVersion(); }
		}
		if (allOk)
		{
			s.BumpVersion();
			ed_eventBus.enqueue(ShaderCompiled{ s.GetObjectID() });
		}

		NEURUS_LOG("[Editor] Created shader for mesh " << objectId << ": " << shaderName);
	}
	catch (const std::exception& ex)
	{
		NEURUS_ERR("[Editor] Exception creating shader: " << ex.what());
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
// Edit() - process all enqueued events
// =========================================================================

void Editor::Edit()
{
	// Chosen BEFORE the queue is drained, and every frame rather than on change:
	// the gizmo's projection math and the renderer's camera data both read the
	// viewport's one published pointer, so they cannot disagree about which camera
	// the frame belongs to. The delete subscription re-syncs it mid-Process, since
	// Process() can delete the activated camera within this very call.
	m_viewport.SyncCamera(GetScene());

	ed_eventBus.Process();

	// Chosen AGAIN, because the drain above can have changed which camera the frame
	// belongs to: activating, deactivating or deleting a scene camera all land
	// inside Process(). The overlay rebuild below runs at most once per dirty flag,
	// so rebuilding against the camera the frame STARTED with would leave geometry
	// at the previous camera's scale and screen position until something else
	// happened to dirty it.
	m_viewport.SyncCamera(GetScene());

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
		// published (RenderRequestSink.h) - posting after would cost one tick.
		if (ed_renderer)
			ed_renderer->Post(RedrawOverlaysRequest{});
	}
}

} // namespace neurus

