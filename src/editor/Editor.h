#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "controllers/Controllers.h"
#include "editor/viewport/DebugDrawBuilder.h"
#include "editor/viewport/EditorViewport.h"
#include "editor/viewport/GizmoDrawBuilder.h"
#include "editor/viewport/TransformGizmo.h"
#include "editor/events/EventBus.h"
#include "editor/operations/HistoryView.h"
#include "editor/operations/OperationManager.h"
#include "core/ResourceManager.h"
#include "render/RenderConfig.h"
#include "scene/EditorContext.h"

// Forward declarations (no render headers!)
namespace neurus {
class Camera;
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
	 * Builds the same content as CreateDefaultScene(), then drains the GPU,
	 * clears undo history and re-uploads scene resources + IBL. The viewport is
	 * never black regardless of what the scene contains: the editor camera is
	 * re-created by the same path (see EnsureEditorCamera()).
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
	 * @brief Returns the camera the viewport looks through.
	 *
	 * The editor camera by default; the scene's *activated* camera when the
	 * Scene names one (Scene::GetActiveCamera()). Selection has nothing to do
	 * with it — selecting a scene camera does not change the view.
	 *
	 * Never null in practice: EnsureEditorCamera() runs on every scene swap, so
	 * a scene with zero cameras is a legal, fully renderable document.
	 */
	Camera* ViewCamera();
	const Camera* ViewCamera() const;

	/**
	 * @brief The editor camera's pool UID (0 before the first scene exists).
	 *
	 * The Application registers a project::EditorComponent against this pair so
	 * the editor camera's identity survives save/load. The camera *object* needs
	 * no help: it lives in the ResourceManager pool, which is serialized whole.
	 */
	int EditorCameraID() const { return m_editorCamUid; }

	/**
	 * @brief Restores the editor camera's UID from a project file.
	 *
	 * Only records the id — FinishLoad() resolves it against the pool (and
	 * creates a fresh camera when the id names nothing, e.g. a project written
	 * before this field existed).
	 */
	void RestoreEditorCameraID(int uid) { m_editorCamUid = uid; }

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

	/**
	 * @brief Records a viewport resize and re-frames the active camera.
	 *
	 * Takes both extents because EditorViewport needs both and they are two views
	 * of one resize: the logical size is the unit of every projection query (and
	 * of Camera::cam_w/cam_h), the render extent exists only for the ID-buffer
	 * readback's physical texels. Accepting them separately is how they drift.
	 *
	 * @param logical      Widget size in logical (Qt) pixels.
	 * @param renderExtent Swapchain size in physical pixels.
	 */
	void HandleResize(glm::uvec2 logical, glm::uvec2 renderExtent);

	/**
	 * @brief True while a modal transform gesture is live.
	 *
	 * Read by the Application to gate the viewport's selection pick: a confirming
	 * LMB click must not also re-select whatever is under the cursor, and must not
	 * pay that path's queue.waitIdle ID-buffer readback. Frame-granular and
	 * therefore safe, since gizmo state only ever changes inside Edit().
	 */
	bool IsGizmoActive() const { return m_gizmo.IsActive(); }

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
	 * @brief Re-applies the last known viewport extent to every view candidate.
	 *
	 * A camera created fresh, or restored from a project saved on a differently
	 * sized window, carries an aspect ratio that has nothing to do with the
	 * current viewport (a fresh Camera is 1x1). Only a window resize ever
	 * corrected that, so File > New used to render a squashed frame until the
	 * user dragged the window. Called from the two scene-swap paths.
	 *
	 * Applies to the editor camera *and* the activated scene camera, because
	 * either may become the view camera without a resize in between: fixing only
	 * the one currently in use means the other shows a stale aspect the moment
	 * activation is toggled.
	 *
	 * The extent comes from m_viewport.Size() — the viewport is the one place that
	 * remembers it.
	 */
	void ApplyViewportToViewCamera();

	/**
	 * @brief Resolves, or creates, the editor camera in the resource pool.
	 *
	 * Must run after every m_resources->Clear() — the pool owns the camera, so a
	 * clear drops it. When m_editorCamUid resolves to a pooled Camera (a loaded
	 * project) the saved pose is kept; otherwise a new pooled camera is created
	 * at the default framing.
	 *
	 * The editor camera is pooled but deliberately NOT scene content: it never
	 * enters Scene::cam_list, so it cannot be selected, deleted or activated, yet
	 * CameraController still reaches it by UID through the pool.
	 */
	void EnsureEditorCamera();

	/**
	 * @brief Where the resting translate handle sits: the selection's active object.
	 *
	 * @param out Filled with that object's transform when this returns true.
	 * @return false when nothing is selected, or when the active object is not a
	 *         Transform3D at all (an Environment is not).
	 *
	 * Read fresh every Edit() rather than cached: any edit can move the object, and a
	 * latched pointer could outlive it by a frame.
	 */
	bool RestingGizmoAnchor(TransformSnapshot& out) const;

	// --- Owned state ---
	std::unique_ptr<Scene> m_scene;
	std::unique_ptr<ResourceManager> m_resources;  ///< App-scoped UID object pool
	RenderConfig          m_config;
	bool                  m_dirty = false;

	/**
	 * @brief The camera the viewport looks through unless a scene camera is
	 *        activated. Pooled (hence serialized), but not scene content.
	 */
	std::shared_ptr<Camera> m_editorCamera;
	int                     m_editorCamUid = 0;

	/**
	 * @brief Flattened debug/gizmo geometry published through EditorContext.
	 *
	 * Marked dirty by the RenderResetEvent subscription (the existing "something
	 * visible changed" broadcast) and by every scene swap; rebuilt in Edit().
	 */
	DebugDrawBuilder      m_debugDraw;

	/**
	 * @brief The world<->screen service, and the Editor-side definition of "the
	 *        camera we are looking through".
	 *
	 * Pushed per frame at the top of Edit() rather than queried on demand, so the
	 * gizmo's projection math and the renderer's CameraGPU cannot disagree about
	 * which camera the frame belongs to.
	 */
	EditorViewport        m_viewport;

	/// Modal transform state (G/R/S). Inactive until a key arms it.
	TransformGizmo        m_gizmo;

	/**
	 * @brief The gizmo's guide geometry, published through EditorContext::gizmoDraw.
	 *
	 * Sibling of m_debugDraw, but dirtied by more: guide length and arc radius are
	 * fixed *pixel* budgets converted through PixelsPerWorldUnit(), so a cursor
	 * move, a camera change and a resize each change the world-space geometry even
	 * when the state machine has not moved. A selection change dirties it too — the
	 * resting translate handle follows the active object.
	 */
	GizmoDrawBuilder      m_gizmoDraw;

	// --- Editor infrastructure ---
	EventQueue ed_eventBus;                        ///< Editor-owned event dispatch queue.
	OperationManager ed_operations;                ///< Undo/redo history over event-replay ops.

	/**
	 * @brief The three controller-facing interfaces + editor singleton access.
	 *
	 * Declared AFTER the pieces it references (bus, operations, pool, scene,
	 * config, viewport, gizmo) and BEFORE the controller list, so it outlives the
	 * controllers' event subscriptions (handler lambdas capture it by value).
	 */
	ControllerContext m_ctx{ ed_eventBus, *m_resources, ed_operations,
	                         [this]() { return m_scene.get(); },
	                         [this]() { return &m_config; },
	                         [this]() { return &m_viewport; },
	                         [this]() { return &m_gizmo; } };

	std::vector<std::unique_ptr<Controllers>> ed_controllers;

	// --- Non-owning references ---
	DeferredRenderer* ed_renderer = nullptr;
	UploadManager* ed_uploadManager = nullptr;
};

} // namespace neurus
