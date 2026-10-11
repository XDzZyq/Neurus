#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "controllers/Controllers.h"
#include "editor/viewport/EditorViewport.h"
#include "editor/events/EventBus.h"
#include "editor/operations/HistoryView.h"
#include "editor/operations/OperationManager.h"
#include "core/ResourceManager.h"
#include "core/RenderConfig.h"

// Forward declarations (no render headers!)
namespace neurus {
class DeferredRenderer;
class MeshData;
class Scene;
class UploadManager;
struct RenderContext;   // render/RenderContext.h — Vulkan-free frame view (renderer-facing)
struct ShaderCreateRequested;
struct UIContext;       // ui/UIContext.h         — Qt-free frame view (UI-facing)
}

namespace neurus {

/**
 * @brief Editor orchestrator — owns scene and render state, scene lifecycle,
 *        scene mutations, and UI signal wiring.
 *
 * Owns Scene and RenderConfig directly. Persistence (project load/save) lives
 * in the Application layer, which drives the scene load lifecycle via
 * BeginLoad()/FinishLoad() and NewScene().
 * Accesses DeferredRenderer and UploadManager via non-owning pointers
 * for mesh/light/IBL GPU uploads.
 */
class Editor
{
public:
	Editor(DeferredRenderer* renderer, UploadManager* uploadManager);
	~Editor();

	Editor(const Editor&) = delete;
	Editor& operator=(const Editor&) = delete;

	void Initialize();

	// --- Scene lifecycle ---

	/**
	 * @brief Resets to a brand-new, EMPTY document: no mesh, no light, no camera.
	 *
	 * Drains the GPU, drops the outgoing scene's resources, and clears the scene,
	 * the object pool, the undo history and the render config. It adds nothing:
	 * content is a separate step (CreateDefaultScene()), which is what makes
	 * "File > New" a new *project* — a new scene plus the default content — while
	 * this stays a new scene and nothing else.
	 *
	 * The viewport is still never black. The viewport's own camera is re-minted
	 * here (EditorViewport::EnsureCamera()) and is not scene content, so a scene
	 * with zero cameras is a legal, fully renderable document.
	 *
	 * Uploads nothing — an empty scene has nothing to upload. A caller that adds
	 * content calls UploadSceneResources() once it has finished adding.
	 */
	void NewScene();

	/**
	 * @brief Adds the default starter content to the CURRENT scene.
	 *
	 * Purely additive — a deactivated scene camera, the starter mesh, a point
	 * light, an IBL environment and one debug object of each kind. It resets
	 * nothing, so call NewScene() first for a fresh document.
	 *
	 * Uploads nothing either: the first-launch caller runs before the window is
	 * shown, where an upload would lose the surface.
	 *
	 * @param objPath Relative path of the starter mesh, e.g. "res/obj/sphere.obj".
	 */
	void CreateDefaultScene(const std::string& objPath);

	void BeginLoad();
	void FinishLoad();

	// --- Accessors ---
	Scene& GetScene();
	RenderConfig& GetRenderConfig() { return m_config; }
	bool IsDirty() const { return m_dirty; }
	void MarkDirty() { m_dirty = true; }
	void ClearDirty() { m_dirty = false; }

	/**
	 * @brief Returns the app-scoped ResourceManager (UID object pool).
	 *
	 * The Application registers a project::ResourceComponent + SceneComponent
	 * against this so the pool is saved first and the Scene resolves its ID
	 * references against it on load.
	 */
	ResourceManager& GetResourceManager() { return *m_resources; }

	/**
	 * @brief Returns the editor's viewport: its camera, screen space and overlays.
	 *
	 * The Editor deliberately exposes no camera API of its own. Which camera a
	 * frame is rendered through, what extent it is framed to, and what is drawn
	 * over it are all viewport questions, and the viewport is the one object that
	 * answers them — for the Application's resize and pick paths, for the
	 * project::EditorComponent that persists the viewport camera's UID, and for
	 * controllers (which reach it, and the gizmo it owns, through
	 * ControllerContext::viewport).
	 */
	EditorViewport& GetViewport() { return m_viewport; }
	const EditorViewport& GetViewport() const { return m_viewport; }

	/**
	 * @brief Fills the renderer-facing view of this frame: what is being rendered.
	 *
	 * Writes the semantic half of the context — scene, config, camera and both
	 * overlay payloads — and deliberately nothing else. The frame half (extent,
	 * frame index, jitter, iteration) is stamped by DeferredRenderer::recordFrame
	 * from the swapchain and its own counters, so the Editor can neither see nor
	 * reset the renderer's temporal state. See render/RenderContext.h for which
	 * half owns which field.
	 *
	 * Together with UpdateUIContext() this makes the Editor the single source of
	 * truth for what is being looked at, in both directions of the triangle
	 * Renderer <- Editor -> UI. Every member is typed — no consumer casts.
	 */
	void UpdateRenderContext(RenderContext& ctx) const;

	/**
	 * @brief Fills the UI-facing view of this frame: the document, and nothing else.
	 *
	 * Only the two members the UI layer is entitled to (scene, config). The
	 * Application then adds the frame-scoped members the Editor does not own
	 * (profile, log, history).
	 *
	 * @warning Assigns fields only — it must never call a UIContext method. This
	 *          translation unit is neurus_editor, which does not link neurus_ui
	 *          (neurus_ui links this one), so a call to a symbol defined there
	 *          would not resolve. See ui/UIContext.h for the writer split.
	 */
	void UpdateUIContext(UIContext& ctx) const;

	/**
	 * @brief Returns a read-only snapshot of the undo/redo history.
	 *
	 * The Application carries this into UIContext each frame so the History
	 * panel can display the stacks without touching Editor-owned operations.
	 * It is intentionally kept out of UIContext's Editor half (and out of
	 * RenderContext entirely) because the Renderer has no use for history state.
	 */
	HistoryView GetHistory() const { return ed_operations.GetHistoryView(); }

	/**
	 * @brief Returns the undo/redo manager (for project history persistence).
	 *
	 * The Application registers a project::HistoryComponent against this so the
	 * operation stacks are saved/loaded alongside the scene and render config.
	 */
	OperationManager& GetOperations() { return ed_operations; }

	template<typename T>
	void RegisterController()
	{
		auto ctrl = std::make_unique<T>();
		ctrl->Init(m_ctx);
		ed_controllers.push_back(std::move(ctrl));
	}

	void Edit();

	template<typename Event>
	void OnUIEvent(const Event& e){
		ed_eventBus.enqueue<Event>(e);
	}

	/**
	 * @brief Uploads the current scene's GPU resources: meshes, lights, debug
	 *        meshes, the environment's IBL cubemaps, and the light SSBO.
	 *
	 * The single "make this scene drawable" step, and the only upload entry point
	 * callers need. Every part of it skips work that is already cached, so a
	 * second call is nearly free - and keeping it in one piece is what stops a
	 * caller from uploading half a scene (the first-launch fallback used to get
	 * meshes and lights but no IBL). Objects outside the scene are uploaded on
	 * demand when they re-enter (SceneObjectGpuUploadRequested).
	 */
	void UploadSceneResources();

	void UploadLighting();

private:
	/**
	 * @brief Drops the outgoing scene's GPU resources, draining the device first.
	 *
	 * The render cache is keyed by object UID, and UIDs are restored from the
	 * project file (UID::serialize re-reads o_id), so entries left behind by the
	 * previous scene shadow whatever comes back under the same id: load project
	 * A, then project B, and B draws A's geometry, shadow maps and cubemaps.
	 * Called by the two paths that replace the scene, before the pool is cleared.
	 */
	void DropSceneGpuResources();

	/**
	 * @brief Uploads the scene environment's IBL cubemaps if not already cached.
	 *
	 * The environment half of UploadSceneResources(). The forced variant for a
	 * live change stays GenerateIBL() (EnvironmentChanged, re-entry).
	 */
	void UploadEnvironmentIBL();

	// --- Handlers called by EventQueue subscribers in Initialize() ---
	void OnMeshImport(const std::string& path);
	void OnCameraAdd();
	void OnLightAdd();
	void OnSunLightAdd();
	void OnSpotLightAdd();
	void OnCreateShader(const ShaderCreateRequested& e);
	void OnSceneObjectGpuUpload(int objectUid);

	/**
	 * @brief Adds the default scene's demo debug objects (see the .cpp for why).
	 * @param meshData The demo mesh's geometry, reused for the DebugMesh wireframe
	 *                 so the wire-mesh path is exercised on a fresh launch. May be null.
	 */
	void AddDefaultDebugObjects(const std::shared_ptr<MeshData>& meshData);

	// --- Owned state ---
	std::unique_ptr<Scene> m_scene;
	std::unique_ptr<ResourceManager> m_resources;  ///< App-scoped UID object pool
	RenderConfig          m_config;
	bool                  m_dirty = false;

	/**
	 * @brief The viewport: its own camera, its screen space, and its overlays.
	 *
	 * The Editor's only involvement with any of that is lifecycle: it hands the
	 * viewport the pool on each scene swap (EnsureCamera/ForgetCamera), the Scene
	 * once per frame (SyncCamera/RebuildOverlays), and nothing else. It holds no
	 * camera, no extent and no overlay state of its own.
	 */
	EditorViewport        m_viewport;

	// --- Editor infrastructure ---
	EventQueue ed_eventBus;                        ///< Editor-owned event dispatch queue.
	OperationManager ed_operations;                ///< Undo/redo history over event-replay ops.

	/**
	 * @brief The three controller-facing interfaces + editor singleton access.
	 *
	 * Declared AFTER the pieces it references (bus, operations, pool, scene,
	 * config, viewport) and BEFORE the controller list, so it outlives the
	 * controllers' event subscriptions (handler lambdas capture it by value).
	 */
	ControllerContext m_ctx{ ed_eventBus, *m_resources, ed_operations,
	                         [this]() { return m_scene.get(); },
	                         [this]() { return &m_config; },
	                         [this]() { return &m_viewport; } };

	std::vector<std::unique_ptr<Controllers>> ed_controllers;

	// --- Non-owning references ---
	DeferredRenderer* ed_renderer = nullptr;
	UploadManager* ed_uploadManager = nullptr;
};

} // namespace neurus
