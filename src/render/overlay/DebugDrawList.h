/**
 * @file DebugDrawList.h
 * @brief Vulkan-free flattened buffer of debug geometry handed to the renderer.
 *
 * DebugDrawList is the single transport for viewport debug geometry. Producers
 * are retained, stateful objects; their contents are flattened into this list,
 * and `revision` is bumped whenever it changes, so the renderer need not
 * re-upload a list that did not move. Vulkan-free and layer-neutral.
 *
 * Positions are always world-space: producers bake their object's transform in
 * while flattening. Only DebugWireMesh keeps a matrix, because its geometry is
 * never copied.
 */

#pragma once

#include "render/overlay/OverlayGeometry.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace neurus
{

/**
 * @name Debug-domain spellings of the shared overlay records
 *
 * Aliases, not distinct types: the record layout is shared, and what separates
 * debug from gizmo geometry is the payload container, not the record.
 * @{
 */
using DebugFlag        = OverlayFlag;
using DebugSegment     = OverlaySegment;
using DebugPointSprite = OverlayPointSprite;
using DebugPointShape  = OverlayPointShape;

/// @brief Alias of PackOverlayColor, spelled for debug producers.
inline uint32_t PackDebugColor(const glm::vec4& color)
{
	return PackOverlayColor(color);
}
/** @} */

/**
 * @brief A wireframe overlay drawn from an already-uploaded mesh.
 *
 * Drawn with VK_POLYGON_MODE_LINE over the mesh's existing vertex and index
 * buffers, so only the object id, transform and color travel here.
 */
struct DebugWireMesh
{
	glm::mat4 model{1.0f};             ///< Local-to-world transform.
	uint32_t rgba{0xFFFFFFFFu};        ///< Packed wireframe color.
	uint32_t flags{DebugFlag::None};   ///< DebugFlag bits (only XRay is meaningful).
	int meshObjectId{-1};              ///< Scene object id that supplies the geometry.
	uint32_t _pad{0u};
};

/**
 * @brief Everything the debug pass draws this frame.
 *
 * Segments and points are partitioned during assembly so that all depth-tested
 * primitives precede all XRay ones; `xraySegmentStart` / `xrayPointStart`
 * record the split.
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
	 * Never reset: the renderer skips re-uploading when the revision it last
	 * saw matches, so repeating a value would make a real change invisible.
	 */
	uint64_t revision = 0;

	/// @brief Marks the contents as changed. Call after any edit, including Clear().
	void Touch() { ++revision; }

	/// @brief True when nothing needs drawing.
	bool Empty() const
	{
		return segments.empty() && points.empty() && wireMeshes.empty();
	}

	/**
	 * @brief Drops all geometry but keeps the allocated capacity for next frame.
	 * @note Does not Touch(); the caller refills and touches once at the end.
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
