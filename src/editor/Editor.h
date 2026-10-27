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
struct RenderContext;   // render/RenderContext.h - Vulkan-free frame view (renderer-facing)
struct ShaderCreateRequested;
struct ScriptContext;   // script/ScriptContext.h - Vulkan/Qt-free script view
struct UIContext;       // ui/UIContext.h         - Qt-free frame view (UI-facing)
}

namespace neurus {

/**
 * @brief Editor orchestrator - owns scene and render state, scene lifecycle,
 *        scene mutations, and UI signal wiring.
 *
 * Owns Scene, RenderConfig, the resource pool, the viewport and the controllers.
 * Scene persistence is driven from outside through BeginLoad()/FinishLoad() and
 * NewScene(); GPU uploads go through non-owning renderer/upload pointers.
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
	 * content is a separate step (CreateDefaultScene()). The viewport's own camera
	 * is re-minted here and is not scene content, so the viewport is never black.
	 *
	 * Uploads nothing; a caller that adds content calls UploadSceneResources().
	 */
	void NewScene();

	/**
	 * @brief Adds the default starter content to the CURRENT scene.
	 *
	 * Purely additive - a deactivated scene camera, the starter mesh, a point
	 * light, an IBL environment and one debug object of each kind. It resets
	 * nothing, so call NewScene() first for a fresh document. Uploads nothing.
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
	 * The pool is serialized first and the Scene resolves its ID references
	 * against it on load.
	 */
	ResourceManager& GetResourceManager() { return *m_resources; }

	/**
	 * @brief Returns the editor's viewport: its camera, screen space and overlays.
	 *
	 * The Editor exposes no camera API of its own; all of that is a viewport
	 * question, and the viewport is the one object that answers it.
	 */
	EditorViewport& GetViewport() { return m_viewport; }
	const EditorViewport& GetViewport() const { return m_viewport; }

	/**
	 * @brief Fills the renderer-facing view of this frame: what is being rendered.
	 *
	 * Writes the semantic half - scene, config, camera and both overlay payloads -
	 * and deliberately nothing else: the frame half (extent, frame index, jitter,
	 * iteration) is stamped by recordFrame() from the swapchain and its counters.
	 */
	void UpdateRenderContext(RenderContext& ctx) const;

	/**
	 * @brief Fills the UI-facing view of this frame: the document, and nothing else.
	 *
	 * Only the two members the UI layer is entitled to (scene, config). The
	 * Application adds the frame-scoped members the Editor does not own.
	 *
	 * @warning Assigns fields only - it must never call a UIContext method. This
	 *          translation unit is neurus_editor, which does not link neurus_ui
	 *          (neurus_ui links this one), so such a call would not resolve.
	 */
	void UpdateUIContext(UIContext& ctx) const;

	/**
	 * @brief Fills the script-facing view: the borrowed scene, render config and
	 *        resource pool.
	 *
	 * Mirrors UpdateUIContext - three field assignments, nothing else. The caller
	 * builds a ScriptContext for one script run and discards it afterwards.
	 */
	void UpdateScriptContext(ScriptContext& ctx) const;

	/**
	 * @brief Returns a read-only snapshot of the undo/redo history.
	 */
	HistoryView GetHistory() const { return ed_operations.GetHistoryView(); }

	/**
	 * @brief Returns the undo/redo manager (for project history persistence).
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

	/**
	 * @brief The single deferred entry point: the UI boundary.
	 *
	 * Everything inside the editor dispatches synchronously, so a script or a
	 * controller observes its own mutation before the call returns. Only the
	 * Qt-facing UI hands events over here, and Edit() drains them once per frame.
	 */
	template<typename Event>
	void OnUIEvent(const Event& e)
	{
		ed_eventBus.Enqueue<Event>(e);
	}

	/**
	 * @brief Uploads the current scene's GPU resources: meshes, lights, debug
	 *        meshes, the environment's IBL cubemaps, and the light SSBO.
	 *
	 * The single "make this scene drawable" step. Every part of it skips work that
	 * is already cached, and keeping it in one piece is what stops a caller from
	 * uploading half a scene. Objects outside the scene are uploaded on demand
	 * when they re-enter.
	 */
	void UploadSceneResources();

	void UploadLighting();

private:
	/**
	 * @brief Drops the outgoing scene's GPU resources, draining the device first.
	 *
	 * The render cache is keyed by object UID, and UIDs are restored from the
	 * project file, so entries left behind by the previous scene shadow whatever
	 * comes back under the same id. Called by the two paths that replace the
	 * scene, before the pool is cleared.
	 */
	void DropSceneGpuResources();

	/**
	 * @brief Uploads the scene environment's IBL cubemaps if not already cached.
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
	 * @brief Adds the default scene's demo debug objects.
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
	 * once per frame (SyncCamera/RebuildOverlays), and nothing else.
	 */
	EditorViewport        m_viewport;

	// --- Editor infrastructure ---
	EventQueue ed_eventBus;                        ///< Editor-owned event dispatch queue.
	OperationManager ed_operations;                ///< Undo/redo history over event-replay ops.

	/**
	 * @brief The controller-facing interfaces + editor singleton access.
	 *
	 * Declared AFTER the pieces it references and BEFORE the controller list, so
	 * it outlives the controllers' event subscriptions (handler lambdas capture it
	 * by value).
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
