/**
 * @file UIContext.h
 * @brief Cross-panel UI refresh context carrying Editor/Profiler state.
 *
 * UIContext is a lightweight data struct assembled each frame and consumed by
 * UIPanel subclasses via their Refresh() method. It carries the two pieces of
 * Editor state a panel may see (the scene and the render settings) plus the
 * per-frame render profile returned by DeferredRenderer::DrawFrame().
 *
 * Architecture:
 * - **No Qt dependency, by construction.** Like RenderContext is Vulkan-free:
 *   this is one-way state from the Editor to the UI layer, it carries no
 *   framework type, and a member that needed a QWidget/QModelIndex/QString to
 *   express would mean the context had started describing the toolkit instead of
 *   the frame. That is what keeps it cheap to build from `neurus_editor`, which
 *   links Qt6::Core but no widget module.
 * - **Two writers, and each fills only its own half** — the same split
 *   RenderContext uses. The Editor owns the document state it is the source of;
 *   the Application owns everything frame-scoped or service-backed:
 *
 *       Editor::UpdateUIContext()   scene, config
 *       Application                 profile, log, history
 *
 *   Rule for a new member: if the Editor owns its source it is the Editor's, and
 *   it must be a plain assignable field (UpdateUIContext runs in neurus_editor,
 *   which does not link neurus_ui and so may not call UCSymbols defined there).
 *   Anything else goes in the Application's half.
 * - **Every member is a typed pointer named by forward declaration**, so this
 *   header still includes nothing beyond <vector>, yet a panel reads what it
 *   needs with no cast and no cast-target to guess. The profile/log/history
 *   members used to be `const void*`, each cast back at exactly one consumer; the
 *   UI layer already forward-declares two of these types elsewhere
 *   (ProfilingPanel.h, LogModel.h), so this is the established pattern rather
 *   than a new one.
 *
 * Null policy: `scene` must be checked (HasScene()), `config` must not be read
 * directly — go through Settings(), which never returns null. The three pointers
 * below are set by the Application every frame in production, but a
 * default-constructed UIContext — what a widget test builds — leaves them null,
 * so a panel must check before dereferencing.
 *
 * Why the UI does not simply hold a RenderContext: that type carries the camera
 * and both overlay payloads, none of which any panel reads. Keeping the UI's view
 * a separate two-field struct is what makes "the gizmo is not scene data" a
 * property of the type rather than a convention.
 */

#pragma once

#include <vector>

namespace neurus
{

class RenderConfig;      // src/core/RenderConfig.h — Vulkan-free
class Scene;             // src/scene/Scene.h
class LogBuffer;         // src/core/Log.h
struct FrameProfile;     // src/render/ProfilingData.h — Vulkan-free
struct HistoryView;      // src/editor/operations/HistoryView.h

/**
 * @brief Read-only context passed to UIPanel::Refresh() each frame.
 *
 * Filled by the Editor (scene, config) and the Application (profile, log,
 * history) each frame.
 */
struct UIContext
{
	// --- Editor half — injected by Editor::UpdateUIContext() ---

	/// @brief The scene being edited, or nullptr. Check HasScene() first.
	const Scene* scene = nullptr;

	/**
	 * @brief The Editor-owned render settings, or nullptr.
	 *
	 * Read it through Settings(), never directly: that is what keeps
	 * RenderConfig's in-class member initializers the only place a default is
	 * written, and it is the same fallback RenderContext::Settings() applies, so
	 * a panel and a pass cannot disagree about an unset value.
	 */
	const RenderConfig* config = nullptr;

	// --- Application half — frame-scoped and service-backed ---

	/**
	 * @brief The latest per-frame render profile, or nullptr.
	 *        Set by the Application from DeferredRenderer::DrawFrame() and read by
	 *        the ProfilingPanel.
	 */
	const FrameProfile* profile = nullptr;

	/**
	 * @brief The core log ring buffer, or nullptr.
	 *        Set by the Application each frame; the LogPanel polls it for new
	 *        entries.
	 */
	const LogBuffer* log = nullptr;

	/**
	 * @brief The current undo/redo snapshot, or nullptr.
	 *        Set by the Application from Editor::GetHistory().
	 */
	const HistoryView* history = nullptr;

	// --- Null policy for the Editor half ---

	/** @brief True when a scene was published; required before touching `scene`. */
	bool HasScene() const { return scene != nullptr; }

	/**
	 * @brief This panel's render settings, or the built-in defaults.
	 *
	 * Never null, so a panel needs no branch and no fallback literal of its own.
	 * Defined in UIContext.cpp, since the complete type is needed there and this
	 * header deliberately only forward-declares RenderConfig.
	 */
	const RenderConfig& Settings() const;

	/**
	 * @brief Returns all scene object UIDs in the scene's master obj_list.
	 *
	 * Collects every object's integer UID from `scene`, or an empty vector when no
	 * scene is published. Panels resolve per-id details (name/type/visibility) via
	 * the Scene and compare the plain ints for dirty checks and lazy updates — no
	 * cached object pointers.
	 *
	 * @return Vector of int object UIDs (one per scene object).
	 */
	std::vector<int> GetObjectIDs() const;
};

} // namespace neurus
