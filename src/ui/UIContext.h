/**
 * @file UIContext.h
 * @brief Cross-panel UI refresh context carrying Editor/Profiler state.
 *
 * UIContext is a lightweight data struct assembled by the Application each frame
 * and consumed by UIPanel subclasses via their Refresh() method. It embeds the
 * shared SceneContext (scene + render config) produced by Editor::GetContext()
 * and adds the per-frame render profile returned by DeferredRenderer::DrawFrame().
 *
 * Architecture:
 * - No Qt dependency, no Vulkan dependency.
 * - Lives in src/ui/ but is populated by the Application layer, not the Editor.
 * - **Every member is a typed pointer named by forward declaration**, so this
 *   header still includes nothing beyond the snapshot it embeds, yet a panel
 *   reads what it needs with no cast and no cast-target to guess. The three
 *   profile/log/history members used to be `const void*`, each cast back at
 *   exactly one consumer; the UI layer already forward-declares two of these
 *   types elsewhere (ProfilingPanel.h, LogModel.h), so this is the established
 *   pattern rather than a new one.
 * - Growth is composition: a component that is not part of the frame's scene
 *   view gets a field here (or its own sibling struct), never a `void*` slot.
 *   See scene/SceneContext.h for the same rule stated at the snapshot.
 *
 * Null policy: `snapshot` answers for its own members (SceneContext::HasScene(),
 * HasCamera(), Settings()). The three pointers below are set by the Application
 * every frame in production, but a default-constructed UIContext — what a widget
 * test builds — leaves them null, so a panel must check before dereferencing.
 */

#pragma once

#include <vector>

#include "scene/SceneContext.h"

namespace neurus
{

class LogBuffer;         // src/core/Log.h
struct FrameProfile;     // src/render/ProfilingData.h — Vulkan-free
struct HistoryView;      // src/editor/operations/HistoryView.h

/**
 * @brief Read-only context passed to UIPanel::Refresh() each frame.
 *
 * Assembled by the Application from Editor::GetContext() plus the profile
 * returned by DeferredRenderer::DrawFrame().
 */
struct UIContext
{
	/** @brief The frame's scene view, shared by value with RenderContext. */
	SceneContext snapshot;

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

	/**
	 * @brief Returns all scene object UIDs in the scene's master obj_list.
	 *
	 * Collects every object's integer UID from `snapshot.scene`, or an empty
	 * vector when no scene is published. Panels resolve per-id details
	 * (name/type/visibility) via the Scene and compare the plain ints for dirty
	 * checks and lazy updates — no cached object pointers.
	 *
	 * @return Vector of int object UIDs (one per scene object).
	 */
	std::vector<int> GetObjectIDs() const;
};

} // namespace neurus
