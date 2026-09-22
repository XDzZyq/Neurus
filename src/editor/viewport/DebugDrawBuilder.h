/**
 * @file DebugDrawBuilder.h
 * @brief Flattens the scene's retained debug objects into a DebugDrawList.
 *
 * The debug overlay has no immediate-mode API: DebugLine, DebugPoints and
 * DebugMesh are ordinary scene objects that live as long as what they
 * visualize. This class is the one place that walks those pools and turns them
 * into the flat, GPU-shaped DebugDrawList the renderer consumes.
 *
 *   Scene (dLine_list / dPoints_list / dMesh_list)
 *        -> DebugDrawBuilder::Rebuild()
 *        -> DebugDrawList  (published via EditorContext::debugDraw)
 *
 * Because the producers are stateful, rebuilding is driven by the existing
 * "something visible changed" broadcast (RenderResetEvent) rather than by a
 * per-frame poll: Editor calls MarkDirty() from that subscription and Rebuild()
 * from Edit(), where a clean builder returns immediately. Exactly one Touch()
 * per rebuild lets DebugPass skip the GPU copy on every unchanged frame.
 *
 * Layer placement: editor layer. It reads the Vulkan-free scene layer and
 * writes the Vulkan-free DebugDrawList; it knows nothing about the renderer.
 */

#pragma once

#include <vector>

#include "scene/DebugDrawList.h"

namespace neurus
{

class Scene;
class DebugLine;
class DebugPoints;
class DebugMesh;

/**
 * @brief Owns the editor-side DebugDrawList and keeps it in sync with the scene.
 *
 * Usage (from Editor):
 * @code
 *   m_debugDraw.MarkDirty();          // on RenderResetEvent / scene swap
 *   m_debugDraw.Rebuild(*m_scene);    // in Edit(), no-op while clean
 *   ctx.debugDraw = &m_debugDraw.List();
 * @endcode
 */
class DebugDrawBuilder
{
public:
	DebugDrawBuilder() = default;

	// Non-copyable: holds the list the renderer points at.
	DebugDrawBuilder(const DebugDrawBuilder&) = delete;
	DebugDrawBuilder& operator=(const DebugDrawBuilder&) = delete;

	/**
	 * @brief Requests a rebuild on the next Rebuild() call.
	 *
	 * Cheap and idempotent — call it from every path that can change debug
	 * geometry rather than trying to work out whether it really did.
	 */
	void MarkDirty() { m_dirty = true; }

	/**
	 * @brief Re-flattens the scene's debug objects if marked dirty.
	 * @param scene Scene to read (never mutated).
	 *
	 * No-op while clean. On a rebuild the list is cleared, refilled with all
	 * depth-tested primitives followed by all x-ray ones, and Touch()ed once.
	 */
	void Rebuild(const Scene& scene);

	/** @brief Returns the flattened list for publication through EditorContext. */
	const DebugDrawList& List() const { return m_list; }

	/** @brief Returns true if a rebuild is pending. */
	bool IsDirty() const { return m_dirty; }

private:
	// Each Append* reads one debug object and pushes into either the main
	// vectors (depth-tested) or the x-ray scratch vectors, chosen by its XRay
	// flag; Rebuild concatenates the scratch halves at the end.
	void AppendDebugLine(const DebugLine& line);
	void AppendDebugPoints(const DebugPoints& points);
	void AppendDebugMesh(const DebugMesh& mesh);

	DebugDrawList m_list;

	/// X-ray primitives staged separately so the partition needs no sort.
	/// Members, not locals, so their capacity survives across rebuilds.
	std::vector<DebugSegment> m_xraySegments;
	std::vector<DebugPointSprite> m_xrayPoints;

	bool m_dirty = true;  ///< Starts dirty so the first Edit() populates the list.
};

} // namespace neurus
