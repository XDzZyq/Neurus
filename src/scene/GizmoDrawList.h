/**
 * @file GizmoDrawList.h
 * @brief Vulkan-free payload for the modal transform gizmo's guide geometry.
 *
 * Holds transient interaction feedback that exists only while a modal
 * transform gesture is active, rebuilt on every cursor move and camera change.
 * Deliberately smaller than DebugDrawList: no revision counter (the payload is
 * a handful of primitives and is written unconditionally), no x-ray split
 * (every guide must be visible through geometry), and no wire meshes.
 *
 * Draw order matters: with depth testing off and alpha-over blending, whatever
 * is submitted last lands on top, so segments are drawn before points and the
 * pivot dot sits above the axis line rather than under it.
 */

#pragma once

#include "scene/OverlayGeometry.h"

#include <vector>

namespace neurus
{

/**
 * @brief Everything the transform gizmo pass draws this frame.
 *
 * Produced while a modal gesture is active and published unconditionally:
 * with nothing armed the list is cleared rather than absent, and the pass
 * early-outs on Empty().
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
