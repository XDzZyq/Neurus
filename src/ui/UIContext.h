/**
 * @file UIContext.h
 * @brief Cross-panel UI refresh context carrying Editor/Profiler state.
 *
 * UIContext is a lightweight data struct assembled each frame and consumed by
 * UIPanel subclasses via their Refresh() method. It carries the two pieces of
 * Editor state a panel may see (the scene and the render settings) plus the
 * per-frame render profile. Qt-free by construction.
 *
 * Two writers, each filling only its own half: the Editor owns the document
 * state it is the source of (scene, config), and the Application owns everything
 * frame-scoped or service-backed (profile, log, history). A new member belongs
 * to whoever owns its source, and the Editor half must be plain assignable
 * fields: UpdateUIContext() runs in neurus_editor, which does not link neurus_ui
 * and so may not call a UIContext symbol defined there.
 *
 * Null policy: `scene` must be checked (HasScene()), `config` must not be read
 * directly - go through Settings(), which never returns null. The three
 * Application-filled pointers are set every frame in production, but a
 * default-constructed UIContext leaves them null, so a panel must check before
 * dereferencing.
 */

#pragma once

#include <vector>

namespace neurus
{

class RenderConfig;      // src/core/RenderConfig.h - Vulkan-free
class Scene;             // src/scene/Scene.h
class LogBuffer;         // src/core/Log.h
struct FrameProfile;     // src/render/ProfilingData.h - Vulkan-free
struct HistoryView;      // src/editor/operations/HistoryView.h

/**
 * @brief Read-only context passed to UIPanel::Refresh() each frame.
 *
 * Filled by the Editor (scene, config) and the Application (profile, log,
 * history) each frame.
 */
struct UIContext
{
	// --- Editor half - filled by Editor::UpdateUIContext() ---

	/// @brief The scene being edited, or nullptr. Check HasScene() first.
	const Scene* scene = nullptr;

	/**
	 * @brief The Editor-owned render settings, or nullptr.
	 *
	 * Read it through Settings(), never directly: that keeps RenderConfig's
	 * in-class member initializers the only place a default is written and
	 * gives a UI reader the same fallback a render pass applies.
	 */
	const RenderConfig* config = nullptr;

	// --- Application half - frame-scoped and service-backed ---

	/** @brief The latest per-frame render profile, or nullptr. Set by the Application. */
	const FrameProfile* profile = nullptr;

	/** @brief The core log ring buffer, or nullptr. Set by the Application. */
	const LogBuffer* log = nullptr;

	/** @brief The current undo/redo snapshot, or nullptr. Set by the Application. */
	const HistoryView* history = nullptr;

	// --- Null policy for the Editor half ---

	/** @brief True when a scene was published; required before touching `scene`. */
	bool HasScene() const { return scene != nullptr; }

	/**
	 * @brief This panel's render settings, or the built-in defaults.
	 *
	 * Never null, so a panel needs no branch and no fallback literal of its own.
	 * Defined in UIContext.cpp, since the complete RenderConfig type is needed
	 * there and this header deliberately only forward-declares it.
	 */
	const RenderConfig& Settings() const;

	/**
	 * @brief Returns all scene object UIDs in the scene's master obj_list.
	 *
	 * Empty when no scene is published. Panels resolve per-id details through
	 * the Scene and compare the plain ints for dirty checks and lazy updates -
	 * no cached object pointers.
	 *
	 * @return Vector of int object UIDs (one per scene object).
	 */
	std::vector<int> GetObjectIDs() const;
};

} // namespace neurus
