/**
 * @file GizmoDrawList.h
 * @brief Vulkan-free payload for the modal transform gizmo's guide geometry.
 *
 * This is the interaction counterpart to DebugDrawList. Both hold the same vertex
 * records (OverlayGeometry.h) and are drawn by the same shaders, but they are
 * separate containers because their lifetimes and authoring rules are opposite:
 *
 *   DebugDrawList  — retained, deterministic data visualization. Lives as long as
 *                    the scene objects it mirrors; changes rarely; revision-gated
 *                    so an unchanged list is never re-uploaded; toggleable by a
 *                    debug-visualization flag.
 *   GizmoDrawList  — transient interaction feedback. Exists only while a modal
 *                    transform gesture is active; rebuilt on every cursor move
 *                    and every camera change; must never be hidden by a debug
 *                    toggle, because it is the only thing telling the user what
 *                    the drag is doing.
 *
 * Sharing one payload would mean one revision counter and one enable flag serving
 * both, which is exactly the coupling that made a drag force a re-upload of every
 * scene debug primitive.
 *
 * Deliberately smaller than DebugDrawList, and each omission is a decision:
 *
 *   - No `revision` / `Touch()`. The peak payload is a handful of segments and one
 *     point, so GizmoCache writes it unconditionally: a revision compare costs
 *     more reasoning than the memcpy it would save.
 *   - No `xraySegmentStart` / `xrayPointStart`. Every modal guide must be visible
 *     through geometry, so GizmoPass disables depth testing for the whole
 *     pass and carries no depth attachment at all. There is nothing to partition.
 *   - No wire meshes. Wireframe is a debug concept (DebugWireMesh); a gizmo guide
 *     is never a mesh.
 *
 * Draw order matters here in a way it does not for DebugPass: with depth testing
 * off and alpha-over blending, whatever is submitted last lands on top. Segments
 * are drawn before points so the pivot dot sits above the axis line rather than
 * under it.
 *
 * Layer placement: the Vulkan-free scene layer, like DebugDrawList.h, so the
 * editor may write it and the renderer may read it without either including the
 * other. EditorContext forward-declares it.
 */

#pragma once

#include "scene/OverlayGeometry.h"

#include <vector>

namespace neurus
{

/**
 * @brief Everything the transform gizmo pass draws this frame.
 *
 * Produced by the editor-side gizmo while a modal gesture is active and published
 * through EditorContext::gizmo; a null pointer (not an empty list) is what tells
 * the renderer no gesture is in progress.
 */
struct GizmoDrawList
{
	/// @brief Constraint axis lines and rotation-arc chords, in world space.
	std::vector<OverlaySegment> segments;

	/// @brief Pivot marker and any handle dots, in world space.
	std::vector<OverlayPointSprite> points;

	/// @brief True when nothing needs drawing (lets the pass skip entirely).
	bool Empty() const { return segments.empty() && points.empty(); }

	/// @brief Drops all geometry but keeps the allocated capacity for the next rebuild.
	void Clear()
	{
		segments.clear();
		points.clear();
	}
};

} // namespace neurus
