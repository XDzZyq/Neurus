/**
 * @file DebugDrawList.h
 * @brief Vulkan-free flattened buffer of debug geometry handed to the renderer.
 *
 * DebugDrawList is the single transport for all viewport debug geometry. There is
 * no immediate-mode API behind it: every producer is a retained, stateful object
 * that lives as long as what it visualizes, exactly like the Qt widgets it sits
 * next to. Something is drawn because an object exists and says so, never because
 * a function was called this frame.
 *
 *   scene debug objects (DebugLine / DebugPoints / DebugMesh)  ─┐
 *   editor-owned visualizers (camera frustum, light frustum,    ├─> DebugDrawList
 *   selection bounds — retained objects mutated on events)     ─┘
 *
 * The Editor flattens those objects into this list and publishes it through
 * EditorContext::debugDraw; DebugPass copies the vectors into shader-readable
 * buffers and issues one draw per primitive kind.
 *
 * Because the producers are stateful, the list only changes when one of them
 * changes. `revision` is bumped by whoever rebuilds it so DebugPass can compare
 * it against the value it last uploaded and skip the copy entirely on the frames
 * — the overwhelming majority — where nothing moved.
 *
 * Layer placement: this header lives in the Vulkan-free scene layer (like
 * EditorContext) so the editor may write it and the renderer may read it
 * without either including the other.
 *
 * GPU layout contract: the vertex records themselves live in OverlayGeometry.h,
 * because the modal transform gizmo draws with the same records and the same
 * shaders while keeping its own payload (GizmoDrawList) — opposite lifetime,
 * opposite authoring rules. The names below are aliases, so every existing
 * DebugSegment / DebugFlag::XRay site keeps working and sizeof() is unchanged.
 *
 * Positions are always world-space (see OverlayGeometry.h for why the transform
 * is baked in rather than indexed). Only DebugWireMesh keeps a matrix, because
 * its geometry is never copied — it is drawn straight from the mesh's existing
 * GPU buffers with the transform in a push constant.
 */

#pragma once

#include "scene/OverlayGeometry.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace neurus
{

/**
 * @name Debug-domain spellings of the shared overlay records
 *
 * Aliases, not distinct types. Two structurally identical types would force a
 * converting copy at the cache boundary and stop DebugCache and GizmoCache
 * sharing a memcpy shape. The separation that matters is the payload container
 * (this file vs GizmoDrawList.h), not the record.
 * @{
 */
using DebugFlag        = OverlayFlag;
using DebugSegment     = OverlaySegment;
using DebugPointSprite = OverlayPointSprite;
using DebugPointShape  = OverlayPointShape;

/// @brief See PackOverlayColor. Kept so debug producers need no include change.
inline uint32_t PackDebugColor(const glm::vec4& color)
{
	return PackOverlayColor(color);
}
/** @} */

/**
 * @brief A wireframe overlay drawn from an already-uploaded mesh.
 *
 * Wireframe uses VK_POLYGON_MODE_LINE over the mesh's existing MeshGPU vertex
 * and index buffers, so no geometry is duplicated on the CPU and nothing is
 * re-uploaded per frame: only the object id, transform and color travel here.
 */
struct DebugWireMesh
{
	glm::mat4 model{1.0f};             ///< Local-to-world transform.
	uint32_t rgba{0xFFFFFFFFu};        ///< Packed wireframe color.
	uint32_t flags{DebugFlag::None};   ///< DebugFlag bits (only XRay is meaningful).
	int meshObjectId{-1};              ///< Scene object id whose MeshGPU supplies the geometry.
	uint32_t _pad{0u};
};

/**
 * @brief Everything the debug pass draws this frame.
 *
 * Segments and points are partitioned during assembly so that all depth-tested
 * primitives precede all XRay ones; `xraySegmentStart` / `xrayPointStart` record
 * the split. That lets DebugPass cover the whole frame in four draws (two
 * depth-tested, two x-ray) regardless of primitive count or authoring order.
 */
struct DebugDrawList
{
	std::vector<DebugSegment> segments;
	std::vector<DebugPointSprite> points;
	std::vector<DebugWireMesh> wireMeshes;

	/// @brief Index of the first XRay segment; == segments.size() when there are none.
	uint32_t xraySegmentStart = 0;

	/// @brief Index of the first XRay point; == points.size() when there are none.
	uint32_t xrayPointStart = 0;

	/**
	 * @brief Bumped by Touch() whenever the contents change.
	 *
	 * Debug objects are stateful, so on most frames this list is identical to the
	 * previous one and re-copying it into GPU memory would be pure waste.
	 * DebugPass remembers the revision it last uploaded per frame-in-flight and
	 * copies only on a mismatch. Never reset: it must not repeat a value the
	 * renderer has already seen, or a real change would be skipped.
	 */
	uint64_t revision = 0;

	/// @brief Marks the contents as changed. Call after any edit, including Clear().
	void Touch() { ++revision; }

	/// @brief True when nothing at all needs drawing (lets DebugPass skip entirely).
	bool Empty() const
	{
		return segments.empty() && points.empty() && wireMeshes.empty();
	}

	/**
	 * @brief Drops all geometry but keeps the allocated capacity for next frame.
	 * @note Does not Touch(); the caller refills the list and touches once at the
	 *       end, so a rebuild that produces identical geometry still counts as one
	 *       revision rather than two.
	 */
	void Clear()
	{
		segments.clear();
		points.clear();
		wireMeshes.clear();
		xraySegmentStart = 0;
		xrayPointStart = 0;
	}
};

} // namespace neurus
