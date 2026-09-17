#include "Editor.h"

#include "editor/events/InputEvents.h"
#include "editor/events/AssetEvents.h"
#include "editor/events/OperationEvents.h"
#include "editor/events/ConfigEvents.h"
#include "editor/events/CameraEvents.h"
#include "editor/events/EditorEvents.h"
#include "editor/events/ShaderEvents.h"
#include "editor/events/SceneEvents.h"

#include "editor/controllers/CameraController.h"
#include "editor/controllers/RenderConfigController.h"
#include "editor/controllers/ShaderController.h"
#include "editor/controllers/SceneController.h"
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

	// Load IBL environment now that the scene is available
	OnIBLLoad();

	// --- Register controllers ---
	// All four now take only the ControllerContext: no providers are needed
	// because the context carries the event dispatch, the pooled-object lookup,
	// the operation sink, the scene, and the render config.
	RegisterController<CameraController>();
	RegisterController<ShaderController>();
	RegisterController<SceneController>();
	RegisterController<RenderConfigController>();

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
		auto* cam = const_cast<Camera*>(GetScene().GetActiveCamera());
		if (!cam) return;

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
	ed_eventBus.subscribe<MousePressEvent>([this](const MousePressEvent& e) {
		if (e.button != Input::Middle) return;
		if (auto* cam = const_cast<Camera*>(GetScene().GetActiveCamera()))
			ed_eventBus.enqueue(CameraDragBegin{cam->GetObjectID()});
	});
	ed_eventBus.subscribe<MouseReleaseEvent>([this](const MouseReleaseEvent& e) {
		if (e.button != Input::Middle) return;
		if (auto* cam = const_cast<Camera*>(GetScene().GetActiveCamera()))
			ed_eventBus.enqueue(CameraDragEnd{cam->GetObjectID()});
	});

	ed_eventBus.subscribe<MouseScrollEvent>([this](const MouseScrollEvent& e) {
		auto* cam = const_cast<Camera*>(GetScene().GetActiveCamera());
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

	// --- Subscribe to RenderResetEvent to reset temporal accumulation ---
	ed_eventBus.subscribe<RenderResetEvent>([this](const RenderResetEvent&) {
		if (ed_renderer)
			ed_renderer->ResetShadowAccumulation();

		// Same signal, second consumer: this is the codebase's existing "something
		// visible changed" broadcast, so it is exactly when the debug overlay may
		// have gone stale. Marking is O(1); the rebuild happens once in Edit().
		m_debugDraw.MarkDirty();
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

	// r_debug_draw gates publication rather than graph topology: with a null list
	// DebugPass returns before touching any image, so toggling the overlay never
	// rebuilds the RenderGraph.
	ctx.debugDraw = m_config.RequiresDebugDraw() ? &m_debugDraw.List() : nullptr;
	return ctx;
}

// =========================================================================
// Scene lifecycle
// =========================================================================

void Editor::CreateDefaultScene(const std::string& objPath)
{
	m_scene = std::make_unique<Scene>();
	m_resources->Clear();
	m_config = RenderConfig{};

	auto camera = m_resources->Load<Camera>();
	camera->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
	camera->cam_tar = glm::vec3(0.0f, 0.0f, 0.0f);
	m_scene->UseCamera(camera);

	auto meshData = m_resources->Load<MeshData>(objPath);
	auto mesh = m_resources->Load<Mesh>(meshData);
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
	axes->SetSmooth(true);
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
		wire->SetScale(glm::vec3(1.02f));
		wire->SetColor(glm::vec4(0.4f, 1.0f, 0.5f, 1.0f));
		wire->SetOpacity(0.9f);
		m_scene->UseDebugMesh(wire);
	}
}

void Editor::NewScene()
{
	try
	{
		// Drain GPU work before destroying the old scene's GPU resources.
		if (ed_renderer)
		{
			ed_renderer->WaitIdle();
		}

		m_scene = std::make_unique<Scene>();
		m_resources->Clear(); // No stale pooled object may leak into a save.
		m_config = RenderConfig{};
		m_dirty = false;
		ed_operations.Clear(); // History does not span scenes.
		m_debugDraw.MarkDirty(); // the previous scene's debug objects are gone
		NEURUS_LOG("[Editor] Created new scene.");

		UploadSceneResources();

		// Generate IBL for the new environment
		OnIBLLoad();
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("Failed to create new scene: " << e.what());
	}
}

void Editor::BeginLoad()
{
	// Drain GPU work before destroying the old scene's GPU resources.
	if (ed_renderer)
		ed_renderer->WaitIdle();

	m_scene = std::make_unique<Scene>();
	m_resources->Clear(); // Pool is restored from the project file next.
	m_config = RenderConfig{};
	ed_operations.Clear(); // History does not span scenes.
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
	m_debugDraw.MarkDirty(); // debug objects came back from the project file
	UploadSceneResources();
	OnIBLLoad();
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
		camera->cam_tar = glm::vec3(0.0f, 0.0f, 0.0f);
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

void Editor::OnIBLLoad()
{
	Scene* scene = m_scene.get();
	if (!scene)
	{
		NEURUS_ERR("[Editor] OnIBLLoad: no scene available");
		return;
	}
	if (!ed_uploadManager || !ed_renderer)
	{
		NEURUS_ERR("[Editor] OnIBLLoad: UploadManager or Renderer not available");
		return;
	}

	// IBL is only enabled when the project provides an environment
	if (scene->env_list.empty())
	{
		NEURUS_LOG("[Editor] No environment in scene 鈥?IBL disabled (black background)");
		return;
	}

	auto env = scene->env_list.begin()->second;
	NEURUS_LOG("[Editor] Using environment (ID " << env->GetObjectID() << ")");

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

	// The light SSBO remains a scene projection (built from scene->light_list).
	UploadLighting();

	NEURUS_LOG("[Editor] Uploaded scene resources to GPU");
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

void Editor::HandleResize(uint32_t width, uint32_t height)
{
	auto* cam = GetScene().GetActiveCamera();
	if (!cam) return;

	ed_eventBus.enqueue(CameraResizeEvent{cam->GetObjectID(),
	                                      static_cast<int>(width),
	                                      static_cast<int>(height)});
}

// =========================================================================
// Edit() 鈥?process all enqueued events (called from newFrame)
// =========================================================================

void Editor::Edit()
{
	ed_eventBus.Process();

	// After the queue is drained, so a scene change and its debug-overlay
	// consequences land in the same frame. Returns immediately when clean, which
	// is the common case: debug objects are stateful and rarely move.
	if (m_scene)
		m_debugDraw.Rebuild(*m_scene);
}

} // namespace neurus

