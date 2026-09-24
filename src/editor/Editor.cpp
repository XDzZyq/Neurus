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

#include "render/DeferredRenderer.h"
#include "render/RenderCache.h"
#include "render/UploadManager.h"
#include "render/resources/LightGPU.h"
#include "render/resources/LightingCache.h"
#include "render/resources/MeshGPU.h"

#include "render/RenderContext.h"
#include "render/shaders/RenderShader.h"
#include "render/shaders/ShaderLibrary.h"
#include "ui/UIContext.h"

#include "core/Log.h"
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
 * res/obj/sphere.obj is a Blender icosphere exported at a ~7.44-unit radius,
 * while the rest of the starter scene is authored in unit space: the camera
 * stands 5.4 units out, the demo axes span +-4, the light box is 0.4 wide. Left
 * raw, the camera sits INSIDE the mesh and every debug object is buried in it.
 *
 * Normalising the mesh (1 / 7.44 ~= 0.135, so radius ~= 1) is the fix rather
 * than pushing the camera ~30 units back, because the debug overlay is the point
 * of this scene and distance would shrink it to a few pixels. The GPU tests take
 * the same approach (test_ibl_render.cpp scales the same asset by 0.25).
 */
constexpr float kStarterMeshScale = 0.135f;

/** @brief Single MeshGPU upload path: uploads the mesh geometry if not cached. */
void UploadMeshGpu(UploadManager& uploader, DeferredRenderer& renderer, const Mesh& mesh)
{
	const int objId = mesh.GetObjectID();
	auto& cache = renderer.GetRenderCache();
	if (cache.GetMeshGPU(objId)) return;
	auto meshGPU = uploader.UploadMesh(mesh);
	cache.UseMeshGPU(objId, std::move(meshGPU));
	NEURUS_LOG("[Editor] Uploaded MeshGPU for objectId=" << objId);
}

/**
 * @brief Single MeshGPU upload path for a DebugMesh (issue #22).
 *
 * DebugMesh derives from ObjectID + Transform3D, not from Mesh, so it cannot
 * reuse UploadMeshGpu() by upcast. It carries the same MeshData, though, so it
 * goes through UploadManager::UploadMeshData() and lands in the same
 * RenderCache MeshGPU slot keyed by object id — which is exactly what
 * DebugWireMesh::meshObjectId resolves against in DebugPass.
 */
void UploadDebugMeshGpu(UploadManager& uploader, DeferredRenderer& renderer, const DebugMesh& mesh)
{
	const int objId = mesh.GetObjectID();
	auto& cache = renderer.GetRenderCache();
	if (cache.GetMeshGPU(objId)) return;
	auto meshGPU = uploader.UploadMeshData(*mesh.o_mesh);
	cache.UseMeshGPU(objId, std::move(meshGPU));
	NEURUS_LOG("[Editor] Uploaded MeshGPU for DebugMesh objectId=" << objId);
}

/** @brief Single LightGPU upload path: uploads shadow maps if the light casts shadows. */
void UploadLightGpu(UploadManager& uploader, DeferredRenderer& renderer, const Light& light)
{
	if (!light.use_shadow) return;
	const int uid = light.GetObjectID();
	auto& cache = renderer.GetRenderCache();
	if (cache.GetLightGPU(uid)) return;
	auto lightGPU = uploader.UploadLight(light);
	cache.UseLightGPU(uid, std::move(lightGPU));
	NEURUS_LOG("[Editor] Uploaded LightGPU for lightUID=" << uid);
}

/** @brief IBL regeneration: uploads the environment's diffuse/specular cubemaps. */
void GenerateIBL(UploadManager& uploader, DeferredRenderer& renderer,
                 const std::shared_ptr<Environment>& env)
{
	auto envGPU = uploader.UploadEnvironment(*env,
	    renderer.GetGraphicsQueue(),
	    renderer.GetGraphicsQueueFamily());
	renderer.GetRenderCache().UseEnvironmentGPU(env->GetObjectID(), std::move(envGPU));
	NEURUS_LOG("[Editor] IBL generated for environment (ID " << env->GetObjectID() << ")");
}

/**
 * @brief UID of the scene's active object, or 0 when nothing is selected.
 *
 * The modal gizmo operates on the active object only (wave 1), and the intent
 * events carry a uid rather than a pointer. 0 is a legal value to enqueue: the
 * controller drops a GizmoModeRequested it cannot resolve, so the key handler stays
 * free of selection logic.
 */
int ActiveUid(const Scene& scene)
{
	const ObjectID* active = scene.selections.GetActiveObject();
	return active ? active->GetObjectID() : 0;
}

} // anonymous namespace

Editor::Editor(DeferredRenderer* renderer, UploadManager* uploadManager)
	: m_scene(std::make_unique<Scene>())
	, m_resources(std::make_unique<ResourceManager>())
	, ed_operations(ed_eventBus)
	, ed_renderer(renderer)
	, ed_uploadManager(uploadManager)
{}

Editor::~Editor()
{
	// m_scene and m_config will be destroyed by unique_ptr automatically.
}

void Editor::Initialize()
{
	// Note: Mesh/light GPU upload happens AFTER window is shown and surface
	// is ready 鈥?see UploadSceneResources() called from Application::Run()
	// and from the scene load lifecycle (BeginLoad/FinishLoad, NewScene).

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
	// (like mesh/light/camera adds). ShaderController keeps only pool-free
	// handlers (compile, code/struct edits, undo/redo replay).
	ed_eventBus.subscribe<ShaderCreateRequested>([this](const ShaderCreateRequested& e) {
		ed_eventBus.enqueue(RenderResetEvent{});
		OnCreateShader(e);
	});

	// Undo/redo of Create Shader (ShaderLinkOp replay): redo relinks the pooled
	// RenderShader by UID; undo drops the reference (the pool keeps the shader).
	// Editor-owned like the create path - pool access + RenderResetEvent. No
	// version bump: the panel dirty-check (-1 sentinel) and the per-mesh
	// (objectId, version) pipeline cache handle the relink.
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
	// All five take only the ControllerContext: no providers are needed because the
	// context carries the event dispatch, the pooled-object lookup, the operation
	// sink, the scene, the render config, the viewport and the gizmo state.
	//
	// TransformGizmoController comes AFTER CameraController on purpose: dispatch runs
	// handlers in registration order, so the gizmo's MouseMoveEvent handler sees a
	// camera the camera controller has already updated this frame.
	RegisterController<CameraController>();
	RegisterController<TransformGizmoController>();
	RegisterController<ShaderController>();
	RegisterController<SceneController>();
	RegisterController<RenderConfigController>();

	// --- Keep the viewport's camera pointer alive across a deletion ---
	// Registered after the controllers, so it runs after SceneController's own
	// handler has actually removed the object: dispatch is registration-ordered.
	// Edit() pushes the camera once per frame, but Process() drains re-entrantly
	// enqueued events in the same call, so deleting the activated camera mid-Process
	// would otherwise leave m_viewport holding a dangling pointer for the rest of
	// that Edit(). Re-pushing resolves to the editor camera, which is why deleting
	// every scene camera is a legal gesture rather than a black frame.
	// SceneOperations dispatches this event synchronously during undo/redo replay
	// too, which this handler covers for free.
	ed_eventBus.subscribe<SceneObjectDeleteRequested>([this](const SceneObjectDeleteRequested&) {
		m_viewport.SetCamera(ViewCamera());
	});

	// --- Subscribe to EnvironmentChanged to regenerate IBL cubemaps on demand ---
	ed_eventBus.subscribe<EnvironmentChanged>([this](const EnvironmentChanged& e) {
		ed_eventBus.enqueue(RenderResetEvent{});
		auto it = GetScene().env_list.find(e.envId);
		if (it != GetScene().env_list.end())
		{
			GenerateIBL(*ed_uploadManager, *ed_renderer, it->second);
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
		// otherwise swallow it. TransformGizmoController does not read it back (it
		// uses e.position, so it cannot be bitten by registration order) but every
		// query that does needs it fresh.
		m_viewport.SetCursor(e.position);

		// The guide's length and arc radius are fixed *pixel* budgets converted
		// through PixelsPerWorldUnit(), so the world-space geometry changes with the
		// cursor even when the state machine has not moved. Gated on IsActive() so an
		// idle mouse sweep does not dirty a list that is already empty.
		if (m_gizmo.IsActive())
			m_gizmoDraw.MarkDirty();

		auto* cam = ViewCamera();
		if (!cam) return;

		// Orbit is suppressed for the duration of a modal gesture, which also keeps
		// the drag's anchor math camera-invariant. Reading gizmo state is legitimate
		// here — it is forbidden only in the KEY handler, where a gate would swallow
		// an axis key arriving in the same Process() call as its mode key.
		if (m_gizmo.IsActive()) return;

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
	// collapses to one undo entry. The typed drag events flow through the same
	// controller chain as the camera moves themselves (no direct handling here).
	// Left and right close a modal transform gesture instead — enqueued
	// unconditionally, like the keys, and dropped by the controller when no gesture
	// is live.
	ed_eventBus.subscribe<MousePressEvent>([this](const MousePressEvent& e) {
		if (e.button == Input::Left)  { ed_eventBus.enqueue(GizmoConfirmed{}); return; }
		if (e.button == Input::Right) { ed_eventBus.enqueue(GizmoCancelled{}); return; }
		if (e.button != Input::Middle) return;
		if (auto* cam = ViewCamera())
			ed_eventBus.enqueue(CameraDragBegin{cam->GetObjectID()});
	});
	ed_eventBus.subscribe<MouseReleaseEvent>([this](const MouseReleaseEvent& e) {
		if (e.button != Input::Middle) return;
		if (auto* cam = ViewCamera())
			ed_eventBus.enqueue(CameraDragEnd{cam->GetObjectID()});
	});

	ed_eventBus.subscribe<MouseScrollEvent>([this](const MouseScrollEvent& e) {
		auto* cam = ViewCamera();
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
	// Every branch enqueues UNCONDITIONALLY. Nothing here reads gizmo state, and
	// that is load-bearing rather than stylistic: Process() is FIFO and drains
	// re-entrantly enqueued events within the same call, so an IsActive() gate would
	// silently eat an axis key that arrived in the same frame as its mode key. The
	// controller is the single place that decides an intent is meaningless.
	ed_eventBus.subscribe<KeyPressEvent>([this](const KeyPressEvent& e) {
		switch (e.key)
		{
		case Input::Key_G: ed_eventBus.enqueue(GizmoModeRequested{ActiveUid(GetScene()), GizmoMode::Move});   break;
		case Input::Key_R: ed_eventBus.enqueue(GizmoModeRequested{ActiveUid(GetScene()), GizmoMode::Rotate}); break;
		case Input::Key_S: ed_eventBus.enqueue(GizmoModeRequested{ActiveUid(GetScene()), GizmoMode::Scale});  break;

		case Input::Key_X: ed_eventBus.enqueue(GizmoAxisRequested{GizmoAxis::X}); break;
		case Input::Key_Y: ed_eventBus.enqueue(GizmoAxisRequested{GizmoAxis::Y}); break;
		case Input::Key_Z: ed_eventBus.enqueue(GizmoAxisRequested{GizmoAxis::Z}); break;

		case Input::Key_Return: ed_eventBus.enqueue(GizmoConfirmed{}); break;
		case Input::Key_Escape: ed_eventBus.enqueue(GizmoCancelled{}); break;

		default: break;
		}
	});

	// --- Subscribe to RenderResetEvent to reset temporal accumulation ---
	ed_eventBus.subscribe<RenderResetEvent>([this](const RenderResetEvent&) {
		if (ed_renderer)
			ed_renderer->ResetShadowAccumulation();

		// Same signal, second consumer: this is the codebase's existing "something
		// visible changed" broadcast, so it is exactly when the debug overlay may
		// have gone stale. Marking is O(1); the rebuild happens once in Edit().
		m_debugDraw.MarkDirty();

		// The gizmo guide rides the same broadcast, which is what makes it appear on
		// arm, follow the cursor through a drag, and vanish on confirm or cancel:
		// every TransformGizmoController handler enqueues a RenderResetEvent.
		m_gizmoDraw.MarkDirty();
	});

	// --- SceneController GPU-sync + dirty subscriptions ---
	ed_eventBus.subscribe<SceneModified>([this](const SceneModified&) {
		m_dirty = true;
	});

	ed_eventBus.subscribe<LightGpuChanged>([this](const LightGpuChanged& e) {
		auto light = m_resources->Get<Light>(e.objectUid);
		if (!light) return;
		auto gpuStruct = ed_uploadManager->UploadLighting(*light);
		ed_renderer->GetRenderCache().UpdateLight(e.objectUid, gpuStruct);
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
// GetContext 鈥?shared editor state (scene + config) for Render/UI contexts
// =========================================================================

EditorContext Editor::GetContext() const
{
	EditorContext ctx;
	ctx.scene = m_scene.get();
	ctx.config = &m_config;

	// The single Editor-side definition of "the camera we are looking through",
	// pushed at the top of Edit() — the editor camera unless a scene camera is
	// activated (Editor::ViewCamera()). Publishing it from here rather than letting
	// the renderer call Scene::GetActiveCamera() is what keeps every pass ignorant
	// of which of the two it is rendering through.
	ctx.camera = m_viewport.GetCamera();

	// r_debug_draw gates publication rather than graph topology: with a null list
	// DebugPass returns before touching any image, so toggling the overlay never
	// rebuilds the RenderGraph.
	ctx.debugDraw = m_config.RequiresDebugDraw() ? &m_debugDraw.List() : nullptr;

	// Published unconditionally, and deliberately behind no RenderConfig flag:
	// interaction feedback is not a debug visualization the user may hide. An
	// inactive gizmo yields a cleared list rather than no list (GizmoDrawBuilder's
	// "empty, never null" contract), and GizmoPass early-outs on an empty payload.
	ctx.gizmoDraw = &m_gizmoDraw.List();
	return ctx;
}

// =========================================================================
// Scene lifecycle
// =========================================================================

void Editor::CreateDefaultScene(const std::string& objPath)
{
	// The outgoing scene's GPU resources go with it (this drains the device
	// first); UploadSceneResources() uploads this scene's own afterwards.
	DropSceneGpuResources();

	m_scene = std::make_unique<Scene>();
	m_resources->Clear();
	m_config = RenderConfig{};

	// The pool just lost the old editor camera; a new document gets a new one at
	// the default framing.
	m_editorCamera.reset();
	m_editorCamUid = 0;
	EnsureEditorCamera();

	// The starter scene ships a scene camera as demo content, deliberately left
	// DEACTIVATED: the viewport looks through the editor camera until the user
	// activates one. Both start at the same pose, so this is invisible until the
	// user navigates.
	auto camera = m_resources->Load<Camera>();
	camera->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
	camera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	m_scene->UseCamera(camera);
	// A brand-new camera is born 1x1; the viewport is not. Adopt the live extent
	// here so the very first frame of a new document is already framed correctly
	// (File > New has no resize event of its own to piggyback on).
	ApplyViewportToViewCamera();

	auto meshData = m_resources->Load<MeshData>(objPath);
	auto mesh = m_resources->Load<Mesh>(meshData);
	mesh->SetScale(glm::vec3(kStarterMeshScale)); // see kStarterMeshScale
	m_scene->UseMesh(mesh);

	auto light = m_resources->Load<Light>(POINTLIGHT, 10.0f, glm::vec3(1.0f));
	light->SetPosition(glm::vec3(3.0f, 3.0f, 3.0f));
	light->SetRadius(0.01f);
	m_scene->UseLight(light);

	// Paths are relative ("res/...") so project files stay portable; the
	// resource layer resolves them against the asset dir at load time. The
	// pooled ImageData owns the path; the Environment only wraps it.
	auto imageData = m_resources->Load<ImageData>("res/tex/hdr/room.hdr");
	auto env = m_resources->Load<Environment>(imageData);
	m_scene->UseEnvironment(env);

	AddDefaultDebugObjects(meshData);

	m_dirty = true;
	m_debugDraw.MarkDirty(); // new scene => the old flattened overlay is stale
}

/**
 * @brief Populates the default scene with one debug object of each kind.
 *
 * Part of the demo content, exactly like the .obj and .hdr loaded above: it
 * gives the overlay something to draw out of the box, so a fresh launch is
 * enough to see (and screenshot) depth occlusion, x-ray, stipple, screen-space
 * point sprites and the CUBE decomposition without any UI interaction.
 */
void Editor::AddDefaultDebugObjects(const std::shared_ptr<MeshData>& meshData)
{
	// The axis spans, reused by the depth-tested and x-ray lines below so the
	// two overlap exactly and the difference between them is only the depth mode.
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
	// at any zoom — the default projection mode, exercised here on purpose.
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
	lightBox->SetProjectionMode(1);  // world units — a cube has real extent
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

void Editor::NewScene(const std::string& objPath)
{
	try
	{
		ed_operations.Clear(); // History does not span scenes.

		// New builds the SAME starter scene a first launch does (camera, mesh,
		// light, environment, demo debug objects) instead of an empty one, for
		// usability rather than correctness: an empty scene renders solid black
		// with nothing in the outliner to select, which reads as a crash rather
		// than as a fresh document. (A camera-less scene is perfectly legal — the
		// editor camera is what the viewport looks through.)
		// CreateDefaultScene() owns the reset (fresh Scene, pool Clear(),
		// editor camera, default RenderConfig, MarkDirty()), so it is not
		// repeated here.
		CreateDefaultScene(objPath);

		// CreateDefaultScene() marks the scene dirty because it MUTATES a scene;
		// as the content of a brand-new document it is the saved baseline, not an
		// unsaved edit (Application rebases its own dirty baseline in step).
		m_dirty = false;

		NEURUS_LOG("[Editor] Created new scene.");

		UploadSceneResources();
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("Failed to create new scene: " << e.what());
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
	// The Clear() above dropped the editor camera. It is NOT re-created here: the
	// project file may name its own (EditorComponent restores the uid, the pool
	// restores the object), and creating one now would leak an orphan into the
	// pool. FinishLoad() resolves or creates it once the file has been read.
	m_editorCamera.reset();
	m_editorCamUid = 0;
	// Application deserializes into GetScene()/GetRenderConfig() before FinishLoad().
}

void Editor::FinishLoad()
{
	// Pool restore happens during Project::Load: ResourceComponent deserializes
	// the pool and wires pooled data refs (MeshData/Shader/ImageData), and each
	// pooled RenderShader re-parses + recompiles to SPIR-V in its own
	// serialize(load). SceneComponent then resolves the scene's ID references.
	// Nothing mesh/shader-specific is needed here.
	m_dirty = false;
	// Resolve the editor camera the project named, or create one for a project
	// written before it existed. Must precede the aspect fix-up below.
	EnsureEditorCamera();
	// The archived aspect belongs to whatever window the project was saved from,
	// so re-adopt the live one instead of trusting it.
	ApplyViewportToViewCamera();
	m_debugDraw.MarkDirty(); // debug objects came back from the project file
	UploadSceneResources(); // meshes, lights, debug meshes, IBL, light SSBO
}


void Editor::OnMeshImport(const std::string& path)
{
	try {
		// Import = load the resource into the pool. The SceneController
		// registers it into the scene and records the undoable Add operation;
		// the MeshGPU upload happens on demand when the add is processed
		// (SceneObjectAddRequested -> SceneObjectGpuUploadRequested ->
		// OnSceneObjectGpuUpload) - no inline upload here.
		//
		// Store a portable relative "res/..." path when the file lives under
		// the res dir, so project files stay machine-independent. Paths
		// outside it are stored as-is (best effort, warned).
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
		// Shadow-map GPU upload happens on demand when the add is processed
		// (SceneObjectAddRequested -> SceneObjectGpuUploadRequested ->
		// OnSceneObjectGpuUpload); the light SSBO rebuild happens via
		// LightingRebuild after the controller registers the light in the scene.
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
		// Shadow-map GPU upload happens on demand when the add is processed
		// (SceneObjectAddRequested -> SceneObjectGpuUploadRequested ->
		// OnSceneObjectGpuUpload); SSBO rebuild via LightingRebuild.
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
		light->SetCutoff(0.95f);        // ~18掳 inner cone half-angle
		light->SetOuterCutoff(0.85f);   // ~32掳 outer cone half-angle
		light->use_shadow = true;
		// Shadow-map GPU upload happens on demand when the add is processed
		// (SceneObjectAddRequested -> SceneObjectGpuUploadRequested ->
		// OnSceneObjectGpuUpload); SSBO rebuild via LightingRebuild.
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
		// Load<T> constructs + registers the RenderShader (pooled UID); the
		// shader owns its own parsing, so parse explicitly here. Paths use the
		// "res/..." prefix (resolved from the working directory by the shader
		// pipeline), matching ShaderLibrary-created shaders.
		auto shader = m_resources->Load<RenderShader>(
			shaderName, "res/shaders/render/gbuffer.vert", "res/shaders/render/gbuffer.frag");
		if (!shader->ParseAndGenerate())
		{
			NEURUS_ERR("[Editor] Failed to create default shader for mesh " << objectId);
			m_resources->Remove(shader->GetObjectID());
			return;
		}

		mesh->SetObjShader(shader); // o_shader + o_shaderId (pooled reference)

		// The link is the undoable fact (independent of compile outcome): record
		// a pool-preserving membership toggle - undo drops the reference (pool
		// keeps the shader), redo relinks it by UID.
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
			s.BumpVersion();

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
 * GPU caches are scene-scoped (UploadSceneResources uploads only objects
 * present at load), so a mesh/light/environment re-added to the scene -
 * live add or undo/redo replay of a deletion - or a light whose shadow was
 * just enabled may lack cached GPU resources. Upload on demand via the
 * shared helpers (skip if already cached). Light SSBO updates stay with
 * LightingRebuild.
 */
void Editor::OnSceneObjectGpuUpload(int objectUid)
{
	if (!ed_uploadManager || !ed_renderer || objectUid == 0) return;

	if (auto mesh = m_resources->Get<Mesh>(objectUid))
	{
		if (mesh->o_mesh) UploadMeshGpu(*ed_uploadManager, *ed_renderer, *mesh);
	}
	else if (auto light = m_resources->Get<Light>(objectUid))
	{
		if (light->use_shadow) UploadLightGpu(*ed_uploadManager, *ed_renderer, *light);
	}
	else if (auto env = m_resources->Get<Environment>(objectUid))
	{
		GenerateIBL(*ed_uploadManager, *ed_renderer, env);
	}
	else if (auto dMesh = m_resources->Get<DebugMesh>(objectUid))
	{
		if (dMesh->HasGeometry()) UploadDebugMeshGpu(*ed_uploadManager, *ed_renderer, *dMesh);
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
	if (!ed_uploadManager || !ed_renderer)
	{
		NEURUS_ERR("[Editor] UploadEnvironmentIBL: UploadManager or Renderer not available");
		return;
	}

	// IBL is only enabled when the project provides an environment
	if (scene->env_list.empty())
	{
		NEURUS_LOG("[Editor] No environment in scene 鈥?IBL disabled (black background)");
		return;
	}

	auto env = scene->env_list.begin()->second;

	// Skip what is already cached: this is part of the idempotent upload path,
	// and regenerating cubemaps is by far its most expensive step (32 MB of
	// staging plus a 2048^2 x 8-mip convolution chain). The cached entry cannot
	// be stale here - DropSceneGpuResources() empties the cache whenever the
	// scene is replaced, which matters because UIDs are restored from the project
	// file and a reused id would otherwise shadow the incoming environment.
	// Rebuilding on a live change is GenerateIBL()'s job (EnvironmentChanged, and
	// an environment re-entering the scene).
	const int envId = env->GetObjectID();
	if (ed_renderer->GetRenderCache().GetEnvironmentGPU(envId))
		return;

	NEURUS_LOG("[Editor] Using environment (ID " << envId << ")");

	// The environment wraps a pooled ImageData (path owned by the data layer).
	// If the pooled pixels are unavailable (missing source file), fall back to
	// the procedural gradient inside UploadEnvironment - no path loading here.
	if (!env->GetEquirectData() || !env->GetEquirectData()->IsValid())
	{
		NEURUS_LOG("[Editor] Environment has no valid equirect data, using procedural fallback");
	}

	GenerateIBL(*ed_uploadManager, *ed_renderer, env);
}

void Editor::UploadSceneResources()
{
	if (!ed_uploadManager || !ed_renderer) return;

	// Scene-scoped upload: GPU resources mirror the SCENE, not the pool, so
	// memory stays proportional to the scene. Pooled objects not in the scene
	// (deleted before save, held only by undo history) are skipped; they are
	// uploaded on demand when re-added - see SceneObjectGpuUploadRequested
	// (SceneController add handler + shadow toggle) handled by
	// OnSceneObjectGpuUpload.
	for (const auto& [id, mesh] : m_scene->mesh_list)
	{
		if (!mesh || !mesh->o_mesh) continue;
		UploadMeshGpu(*ed_uploadManager, *ed_renderer, *mesh);
	}

	for (const auto& [uid, light] : m_scene->light_list)
	{
		if (!light) continue;
		UploadLightGpu(*ed_uploadManager, *ed_renderer, *light);
	}

	// DebugMesh wireframes need a MeshGPU too: DebugPass skips any DebugWireMesh
	// whose meshObjectId resolves to no cached vertex/index buffer.
	for (const auto& [uid, dMesh] : m_scene->dMesh_list)
	{
		if (!dMesh || !dMesh->HasGeometry()) continue;
		UploadDebugMeshGpu(*ed_uploadManager, *ed_renderer, *dMesh);
	}

	// The environment is scene content as well: without its cubemaps the scene
	// renders unlit while the Property panel still shows the environment.
	UploadEnvironmentIBL();

	// The light SSBO remains a scene projection (built from scene->light_list).
	UploadLighting();

	NEURUS_LOG("[Editor] Uploaded scene resources to GPU");
}

/**
 * @brief Drops the outgoing scene's GPU resources, draining the device first.
 *
 * The eviction itself belongs to RenderCache (RemoveSceneResources): it owns the
 * caches, and it is the only place that knows which of them are scene-scoped.
 * What the Editor contributes is the drain - the cached entries own vk::raii
 * resources, so releasing them while a frame that reads them is still executing
 * is a validation error. Both scene-replacing paths call this before clearing
 * the pool.
 */
void Editor::DropSceneGpuResources()
{
	if (!ed_renderer) return;

	ed_renderer->WaitIdle();
	ed_renderer->GetRenderCache().RemoveSceneResources();
}

void Editor::UploadLighting()
{
	if (ed_uploadManager && ed_renderer)
	{
		auto& scene = *m_scene;
		auto lightDict = ed_uploadManager->UploadLighting(scene.light_list);
		ed_renderer->GetRenderCache().UpdateLighting(lightDict);
	}
}

// =========================================================================
// HandleResize() 鈥?dispatch CameraResizeEvent via event bus
// =========================================================================

void Editor::HandleResize(glm::uvec2 logical, glm::uvec2 renderExtent)
{
	// Remembered unconditionally, BEFORE the camera check: the extent is a
	// property of the viewport, not of whatever scene happens to be loaded, and
	// every later scene (File > New, File > Open) needs it to frame its camera.
	// EditorViewport is now the one place that holds it.
	if (logical.x > 0 && logical.y > 0)
	{
		m_viewport.SetViewportSize(logical, renderExtent);

		// Constant-pixel guide geometry is a CPU computation against the viewport
		// height, so a resize changes the world-space guide even mid-gesture.
		m_gizmoDraw.MarkDirty();
	}

	// Both view candidates are re-framed, not just the one currently in use:
	// activation can switch the view camera without a resize in between, and a
	// camera that missed a resize would then present a stale aspect. The events
	// carry a UID and CameraController resolves it through the pool, so the editor
	// camera (pooled, but not scene content) is reachable by the ordinary event.
	if (m_editorCamera)
		ed_eventBus.enqueue(CameraResizeEvent{m_editorCamUid,
		                                      static_cast<int>(logical.x),
		                                      static_cast<int>(logical.y)});

	if (auto* cam = GetScene().GetActiveCamera())
		ed_eventBus.enqueue(CameraResizeEvent{cam->GetObjectID(),
		                                      static_cast<int>(logical.x),
		                                      static_cast<int>(logical.y)});
}

// =========================================================================
// ApplyViewportToViewCamera() 鈥?re-frame the cameras the Editor just seeded
// =========================================================================

void Editor::ApplyViewportToViewCamera()
{
	// Direct mutation rather than a CameraResizeEvent, because this runs while a
	// scene is being built: the camera must be correctly framed on the FIRST
	// frame, and the event queue is only drained in the next Edit(). It stays
	// inside the Editor's own scene, so no layer boundary is crossed.
	const glm::uvec2 size = m_viewport.Size();
	if (size.x == 0 || size.y == 0) return; // no viewport yet (startup)

	const float w = static_cast<float>(size.x);
	const float h = static_cast<float>(size.y);

	if (m_editorCamera)
		m_editorCamera->ChangeCamRatio(w, h);

	// Same both-cameras reasoning as HandleResize().
	if (auto* cam = GetScene().GetActiveCamera())
		cam->ChangeCamRatio(w, h);
}

// =========================================================================
// EnsureEditorCamera() 鈥?the viewport's own camera, pooled but not in the scene
// =========================================================================

void Editor::EnsureEditorCamera()
{
	if (m_editorCamUid != 0)
	{
		m_editorCamera = m_resources->Get<Camera>(m_editorCamUid);
		if (m_editorCamera) return; // loaded project: keep the saved pose
	}

	// No id, or an id the pool does not know (a project written before the editor
	// camera was persisted). Create one at the default framing.
	m_editorCamera = m_resources->Load<Camera>();
	m_editorCamUid = m_editorCamera->GetObjectID();
	m_editorCamera->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
	m_editorCamera->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	NEURUS_LOG("[Editor] Editor camera created (UID " << m_editorCamUid << ")");
}

// =========================================================================
// ViewCamera() 鈥?editor camera by default, activated scene camera when named
// =========================================================================

Camera* Editor::ViewCamera()
{
	if (Camera* sceneCam = GetScene().GetActiveCamera())
		return sceneCam;
	return m_editorCamera.get();
}

const Camera* Editor::ViewCamera() const
{
	if (const Camera* sceneCam = m_scene->GetActiveCamera())
		return sceneCam;
	return m_editorCamera.get();
}

// =========================================================================
// Edit() 鈥?process all enqueued events (called from newFrame)
// =========================================================================

void Editor::Edit()
{
	// Pushed BEFORE the queue is drained, and every frame rather than on change:
	// the gizmo's projection math and the renderer's CameraGPU both read this one
	// pointer, so they cannot disagree about which camera the frame belongs to. The
	// SceneObjectDeleteRequested subscription re-pushes it mid-Process, since
	// Process() can delete the activated camera within this very call.
	m_viewport.SetCamera(ViewCamera());

	ed_eventBus.Process();

	// After the queue is drained, so a scene change and its debug-overlay
	// consequences land in the same frame. Returns immediately when clean, which
	// is the common case: debug objects are stateful and rarely move.
	if (m_scene)
		m_debugDraw.Rebuild(*m_scene);

	// Same timing, same dirty-flag discipline. Rebuild() clears the list and returns
	// when no gesture is live, so a finished gesture's guide disappears on the frame
	// its confirm was processed.
	m_gizmoDraw.Rebuild(m_gizmo, m_viewport);
}

} // namespace neurus

