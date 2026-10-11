/**
 * @file GizmoDrawBuilder.h
 * @brief Turns the transform gizmo - armed or resting - into the overlay payload
 *        GizmoPass draws.
 *
 * Owned by value by the Editor; it subscribes to nothing and mutates nothing, so
 * only a Rebuild() the owner asks for can change the picture.
 *
 * A *resting* translate handle is drawn whenever something is selected and no
 * gesture is armed, and the *armed* gesture's guide while one is. Resting is
 * exactly "Move mode, no axis chosen yet", so it reuses the unconstrained branch
 * and differs only in brightness. Both anchor on the object's **live** position,
 * so a translate gesture carries its own handle along; the **rotation** comes
 * from the gesture's frozen Before() snapshot, because guides that spun with the
 * object would destroy the reference a rotation is measured against.
 *
 * Guide length, arc radius and arrowhead size are fixed pixel budgets converted
 * through the viewport's pixels-per-world-unit, so a camera change marks it
 * dirty even though nothing about the gesture moved. Nothing armed and nothing
 * selected produces a cleared list rather than no list; nothing downstream has
 * to distinguish null from empty.
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
	 * @param live The anchor object's current transform - the gesture's target while
	 *             one is armed, the selection's active object otherwise - or nullptr
	 *             when there is none. A gesture whose target was deleted mid-drag
	 *             passes nullptr and falls back to the frozen Before() snapshot.
	 *
	 * An invalid viewport, a pivot behind the eye, or nothing armed *and* no @p live
	 * all clear the list and return.
	 */
	void Rebuild(const TransformGizmo& gizmo, const EditorViewport& vp,
	             const TransformSnapshot* live);

	/// @brief This frame's guides.
	const GizmoDrawList& List() const { return m_list; }

	bool IsDirty() const { return m_dirty; }

private:
	GizmoDrawList m_list;

	/// @brief Starts dirty so the first Edit() populates the list.
	bool m_dirty = true;
};

} // namespace neurus
