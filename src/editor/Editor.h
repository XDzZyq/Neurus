#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "controllers/Controllers.h"
#include "editor/DebugDrawBuilder.h"
#include "editor/events/EventBus.h"
#include "editor/operations/HistoryView.h"
#include "editor/operations/OperationManager.h"
#include "core/ResourceManager.h"
#include "render/RenderConfig.h"
#include "scene/EditorContext.h"

// Forward declarations (no render headers!)
namespace neurus {
class DeferredRenderer;
class MeshData;
class Scene;
class UploadManager;
struct ShaderCreateRequested;
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
	void CreateDefaultScene(const std::string& objPath);

	/**
	 * @brief Resets to a brand-new document holding the default starter scene.
	 *
	 * Builds the same content as CreateDefaultScene() (so the viewport is never
	 * black and the scene never lacks a camera - see the scene invariant in
	 * editor.instructions.md), then drains the GPU, clears undo history and
	 * re-uploads scene resources + IBL.
	 *
	 * @param objPath Relative path of the starter mesh, e.g. "res/obj/sphere.obj".
	 */
	void NewScene(const std::string& objPath);
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
	 * @brief Returns the shared editor state (scene + render config).
	 *
	 * The Application embeds this into both RenderContext and UIContext each
	 * frame, so the Editor is the single source of truth for scene/config and
	 * never builds a RenderContext or UIContext itself.
	 */
	EditorContext GetContext() const;

	/**
	 * @brief Returns a read-only snapshot of the undo/redo history.
	 *
	 * The Application carries this into UIContext each frame so the History
	 * panel can display the stacks without touching Editor-owned operations.
	 * It is intentionally kept out of EditorContext because the Renderer has
	 * no use for history state.
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

	void HandleResize(uint32_t width, uint32_t height);

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

	/**
	 * @brief Re-applies the last known viewport extent to the active camera.
	 *
	 * A camera created by CreateDefaultScene(), or restored from a project saved
	 * on a differently sized window, carries an aspect ratio that has nothing to
	 * do with the current viewport (a fresh Camera is 1x1). Only a window resize
	 * ever corrected that, so File > New used to render a squashed frame until the
	 * user dragged the window. Called from the two scene-swap paths.
	 */
	void ApplyViewportToActiveCamera();

	// --- Owned state ---
	std::unique_ptr<Scene> m_scene;
	std::unique_ptr<ResourceManager> m_resources;  ///< App-scoped UID object pool
	RenderConfig          m_config;
	bool                  m_dirty = false;

	/// Last viewport extent seen by HandleResize(); 0 until the window is shown.
	uint32_t              m_viewportW = 0;
	uint32_t              m_viewportH = 0;

	/**
	 * @brief Flattened debug/gizmo geometry published through EditorContext.
	 *
	 * Marked dirty by the RenderResetEvent subscription (the existing "something
	 * visible changed" broadcast) and by every scene swap; rebuilt in Edit().
	 */
	DebugDrawBuilder      m_debugDraw;

	// --- Editor infrastructure ---
	EventQueue ed_eventBus;                        ///< Editor-owned event dispatch queue.
	OperationManager ed_operations;                ///< Undo/redo history over event-replay ops.

	/**
	 * @brief The three controller-facing interfaces + editor singleton access.
	 *
	 * Declared AFTER the pieces it references (bus, operations, pool, scene,
	 * config) and BEFORE the controller list, so it outlives the controllers'
	 * event subscriptions (handler lambdas capture it by value).
	 */
	ControllerContext m_ctx{ ed_eventBus, *m_resources, ed_operations,
	                         [this]() { return m_scene.get(); },
	                         [this]() { return &m_config; } };

	std::vector<std::unique_ptr<Controllers>> ed_controllers;

	// --- Non-owning references ---
	DeferredRenderer* ed_renderer = nullptr;
	UploadManager* ed_uploadManager = nullptr;
};

} // namespace neurus
