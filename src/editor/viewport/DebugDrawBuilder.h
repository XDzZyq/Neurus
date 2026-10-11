/**
 * @file DebugDrawBuilder.h
 * @brief Flattens the scene's retained debug objects into a DebugDrawList.
 *
 * Debug objects are ordinary scene objects that live as long as what they
 * visualize, so there is no immediate-mode API; this class is the one place that
 * walks the debug pools and turns them into the flat, GPU-shaped list. Rebuild is
 * driven by a dirty flag rather than a per-frame poll, and touches the list once
 * per rebuild so the GPU copy can be skipped on unchanged frames.
 */

#pragma once

#include <vector>

#include "render/overlay/DebugDrawList.h"

namespace neurus
{

class Scene;
class DebugLine;
class DebugPoints;
class DebugMesh;

/**
 * @brief Owns the editor-side DebugDrawList and keeps it in sync with the scene.
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
	 * Cheap and idempotent: call it from every path that can change debug
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

	/** @brief Returns the flattened list. */
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
