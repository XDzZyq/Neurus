/**
 * @file UIContext.h
 * @brief Cross-panel UI refresh context carrying the frame's snapshot data.
 *
 * UIContext is a lightweight data struct assembled once per frame and consumed
 * by UIPanel subclasses via their Refresh() method. It embeds a scene snapshot
 * and adds pointers to the frame profile, the log ring buffer and the undo/redo
 * snapshot.
 *
 * Null policy: `snapshot` answers for its own members, but the three pointers
 * may be null, so a consumer must check before dereferencing.
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
 */
struct UIContext
{
	/** @brief The frame's scene snapshot. */
	SceneContext snapshot;

	/** @brief The latest per-frame render profile, or nullptr. */
	const FrameProfile* profile = nullptr;

	/**
	 * @brief The core log ring buffer, or nullptr.
	 */
	const LogBuffer* log = nullptr;

	/**
	 * @brief The current undo/redo snapshot, or nullptr.
	 */
	const HistoryView* history = nullptr;

	/**
	 * @brief Returns all scene object UIDs in the scene's master obj_list.
	 *
	 * Returns plain ints so callers can compare them for dirty checks and
	 * resolve per-id details lazily, without caching object pointers.
	 *
	 * @return Vector of int object UIDs (one per scene object).
	 */
	std::vector<int> GetObjectIDs() const;
};

} // namespace neurus
