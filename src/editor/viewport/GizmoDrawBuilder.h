/**
 * @file GizmoDrawBuilder.h
 * @brief Turns the transform gizmo — armed or resting — into the overlay payload
 *        GizmoPass draws.
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
 * ## Two things it draws, one geometry path
 *
 * A *resting* translate handle whenever something is selected and no gesture is
 * armed, and the *armed* gesture's guide while one is. They share every line of
 * geometry code: resting is exactly "Move mode, no axis chosen yet", so it reuses
 * the unconstrained branch and differs only in brightness (see Rebuild()). The
 * anchor differs though — the armed guide sits on the gesture's frozen Before()
 * snapshot, the resting handle on the object's live transform, which is why the
 * caller passes the latter in rather than the builder reaching for a Scene.
 *
 * ## What marks it dirty
 *
 * Everything that changes the picture, which is more than the things that change
 * the gesture: a cursor move during a gesture, a camera change, a resize, a
 * selection change and any edit that moves the selected object. Guide length, arc
 * radius and arrowhead size are fixed pixel budgets converted through
 * PixelsPerWorldUnit(), so a pure camera dolly changes the world-space geometry
 * even though nothing about the state machine moved.
 *
 * ## Empty, never null
 *
 * Nothing armed and nothing selected produces a cleared list rather than no list,
 * so the Editor can publish `&List()` unconditionally and GizmoPass's own early-out
 * on Empty() is what ends the frame. Nothing downstream has to distinguish null
 * from empty.
 *
 * Layer placement: editor, Vulkan-free and Qt-free.
 */

#pragma once

#include "scene/GizmoDrawList.h"

namespace neurus
{

class EditorViewport;
class TransformGizmo;
struct TransformSnapshot;

/**
 * @brief Flattens the resting handle or one modal gesture into segments and sprites.
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
	 *
	 * @param resting Where the resting translate handle sits — the selection's active
	 *                object — or nullptr when nothing is selected. Ignored while a
	 *                gesture is armed, since the armed guide anchors on Before().
	 *
	 * An invalid viewport, a pivot behind the eye, or nothing armed *and* nothing
	 * selected all clear the list and return.
	 */
	void Rebuild(const TransformGizmo& gizmo, const EditorViewport& vp,
	             const TransformSnapshot* resting);

	/// @brief This frame's guides. Published through EditorContext::gizmoDraw.
	const GizmoDrawList& List() const { return m_list; }

	bool IsDirty() const { return m_dirty; }

private:
	GizmoDrawList m_list;

	/// @brief Starts dirty so the first Edit() populates the list.
	bool m_dirty = true;
};

} // namespace neurus
