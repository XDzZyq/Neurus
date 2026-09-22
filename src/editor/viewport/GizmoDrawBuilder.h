/**
 * @file GizmoDrawBuilder.h
 * @brief Turns a live TransformGizmo gesture into the overlay payload GizmoPass draws.
 *
 * The exact sibling of DebugDrawBuilder — a list, a dirty flag and a Rebuild() —
 * and owned by value by the Editor in the same way, one builder per overlay payload.
 * It is deliberately *not* a controller: it subscribes to nothing and mutates
 * nothing, so the only thing that can change the picture is a Rebuild() the Editor
 * asks for.
 *
 * ## Why this is a separate class from TransformGizmo
 *
 * The two have unrelated reasons to change: the delta math changes when the
 * interaction rules change, the geometry changes when the guide's *look* changes.
 * Fused, one class would carry ~13 public methods, past the point CLAUDE.md asks
 * whether a class is doing too much. Split, the state machine stays at ten and the
 * builder needs nothing from it beyond Mode(), Axis() and Before() plus the free
 * GizmoAxisDirection() — no accessor per axis.
 *
 * ## What marks it dirty
 *
 * The three things that change the picture, which is more than the three that
 * change the gesture: a cursor move, a camera change *and* a resize. Guide length
 * and arc radius are fixed pixel budgets converted through PixelsPerWorldUnit(),
 * so a pure camera dolly changes the world-space geometry even though the state
 * machine did not move at all.
 *
 * ## Empty, never null
 *
 * An inactive gizmo produces a cleared list rather than no list, so the Editor can
 * publish `&List()` unconditionally and GizmoPass's own early-out on Empty() is
 * what ends the frame. Nothing downstream has to distinguish null from empty.
 *
 * Layer placement: editor, Vulkan-free and Qt-free.
 */

#pragma once

#include "scene/GizmoDrawList.h"

namespace neurus
{

class EditorViewport;
class TransformGizmo;

/**
 * @brief Flattens one modal gesture into segments and point sprites.
 *
 * Non-copyable: the renderer holds a pointer to the list this object owns.
 */
class GizmoDrawBuilder
{
public:
	GizmoDrawBuilder() = default;
	GizmoDrawBuilder(const GizmoDrawBuilder&) = delete;
	GizmoDrawBuilder& operator=(const GizmoDrawBuilder&) = delete;

	/// @brief Request a rebuild on the next Rebuild() call.
	void MarkDirty() { m_dirty = true; }

	/**
	 * @brief Regenerate the list from @p gizmo, sized against @p vp. No-op when clean.
	 *
	 * Called at the end of Editor::Edit(), right after DebugDrawBuilder::Rebuild().
	 * An inactive gesture, an invalid viewport or a pivot behind the eye all clear
	 * the list and return.
	 */
	void Rebuild(const TransformGizmo& gizmo, const EditorViewport& vp);

	/// @brief This frame's guides. Published through EditorContext::gizmoDraw.
	const GizmoDrawList& List() const { return m_list; }

	bool IsDirty() const { return m_dirty; }

private:
	GizmoDrawList m_list;

	/// @brief Starts dirty so the first Edit() populates the list.
	bool m_dirty = true;
};

} // namespace neurus
