/**
 * @file DebugDrawBuilder.cpp
 * @brief Scene debug objects -> DebugDrawList flattening.
 */

#include "DebugDrawBuilder.h"

#include "scene/DebugLine.h"
#include "scene/DebugMesh.h"
#include "scene/DebugPoints.h"
#include "scene/ObjectID.h"
#include "scene/Scene.h"

namespace neurus
{

namespace
{

/**
 * @brief Folds an object's opacity into its color and packs the result.
 *
 * Opacity is a separate knob on every debug object purely for authoring
 * convenience; the GPU only ever sees one alpha, so it is multiplied in here.
 */
uint32_t PackTinted(const glm::vec4& color, float opacity)
{
	return PackDebugColor(glm::vec4(color.r, color.g, color.b, color.a * opacity));
}

/// @brief Maps DebugPoints::PointType to the shader's shape id.
/// @note CUBE never reaches here — it is decomposed into segments instead.
uint32_t ShapeOf(DebugPoints::PointType type)
{
	switch (type)
	{
	case DebugPoints::PointType::RHOMBUS: return DebugPointShape::Rhombus;
	case DebugPoints::PointType::CIR:     return DebugPointShape::Circle;
	default:                              return DebugPointShape::Square;
	}
}

/**
 * @brief Emits the 12 edges of an axis-aligned cube into `out`.
 * @param out    Destination segment vector.
 * @param center Cube center, in world space.
 * @param half   Half extent along each axis, in world units.
 * @param width  Edge width in pixels.
 * @param rgba   Packed edge color.
 * @param flags  Per-segment DebugFlag bits.
 *
 * PointType::CUBE is the one point shape with no sprite form: a screen-aligned
 * sprite cannot show a cube's orientation. Drawing its wireframe keeps the
 * shape meaningful at the cost of 12 segments per point.
 */
void AppendCubeEdges(std::vector<DebugSegment>& out,
                     const glm::vec3& center,
                     float half,
                     float width,
                     uint32_t rgba,
                     uint32_t flags)
{
	// The 8 corners, indexed so bit 0 = +X, bit 1 = +Y, bit 2 = +Z.
	glm::vec3 c[8];
	for (int i = 0; i < 8; ++i)
	{
		c[i] = center + glm::vec3((i & 1) ? half : -half,
		                          (i & 2) ? half : -half,
		                          (i & 4) ? half : -half);
	}

	// Every corner pair differing in exactly one bit is an edge: 12 of them.
	static constexpr int kEdges[12][2] = {
		{0, 1}, {2, 3}, {4, 5}, {6, 7},  // along X
		{0, 2}, {1, 3}, {4, 6}, {5, 7},  // along Y
		{0, 4}, {1, 5}, {2, 6}, {3, 7},  // along Z
	};

	for (const auto& e : kEdges)
	{
		DebugSegment seg;
		seg.a = c[e[0]];
		seg.b = c[e[1]];
		seg.width = width;
		seg.rgba = rgba;
		seg.flags = flags;
		out.push_back(seg);
	}
}

/**
 * @brief Whether an object's visibility flags let it into the overlay.
 *
 * `DebugDrawList` has no per-primitive enable bit — DebugPass draws whole ranges
 * — so hiding a debug object means not flattening it in the first place. The two
 * flags are ANDed the way every other pool consumer ANDs them (GeometryPass,
 * ShadowDepthPass, UploadManager): the overlay is editor-only, so `is_viewport`
 * is the flag that obviously applies, but honouring only one of the pair would
 * make the Outliner's two toggles mean different things for a debug row than for
 * a mesh row.
 *
 * The rebuild after a toggle is already arranged: `SceneController` enqueues
 * `RenderResetEvent`, whose handler calls MarkDirty().
 */
bool IsVisible(const ObjectID& obj)
{
	return obj.is_viewport && obj.is_rendered;
}

} // namespace

// ---------------------------------------------------------------------------
// Rebuild
// ---------------------------------------------------------------------------

void DebugDrawBuilder::Rebuild(const Scene& scene)
{
	if (!m_dirty)
		return;

	m_list.Clear();
	m_xraySegments.clear();
	m_xrayPoints.clear();

	for (const auto& [id, obj] : scene.dLine_list)
	{
		(void)id;
		if (obj && IsVisible(*obj))
			AppendDebugLine(*obj);
	}
	for (const auto& [id, obj] : scene.dPoints_list)
	{
		(void)id;
		if (obj && IsVisible(*obj))
			AppendDebugPoints(*obj);
	}
	for (const auto& [id, obj] : scene.dMesh_list)
	{
		(void)id;
		if (obj && IsVisible(*obj))
			AppendDebugMesh(*obj);
	}

	// Depth-tested primitives are already in the main vectors; appending the
	// x-ray halves now gives the partition DebugPass draws from, with no sort.
	m_list.xraySegmentStart = static_cast<uint32_t>(m_list.segments.size());
	m_list.xrayPointStart = static_cast<uint32_t>(m_list.points.size());
	m_list.segments.insert(m_list.segments.end(), m_xraySegments.begin(), m_xraySegments.end());
	m_list.points.insert(m_list.points.end(), m_xrayPoints.begin(), m_xrayPoints.end());

	m_list.Touch();
	m_dirty = false;
}

// ---------------------------------------------------------------------------
// Per-object flattening
// ---------------------------------------------------------------------------

void DebugDrawBuilder::AppendDebugLine(const DebugLine& line)
{
	const std::vector<glm::vec3>& verts = line.GetVertices();
	if (verts.size() < 2)
		return;  // a lone vertex is not a segment

	const bool xray = line.GetXRay();
	uint32_t flags = xray ? DebugFlag::XRay : DebugFlag::None;
	// Smoothing is derived, not authored: it fades alpha towards the quad edge,
	// which would soften the hard ends a dash is made of. So a solid line is
	// antialiased and a stippled one is not — the two are never both set.
	flags |= line.GetStipple() ? DebugFlag::Stipple : DebugFlag::Smooth;

	const glm::mat4 model = line.GetModelMatrix();
	const uint32_t rgba = PackTinted(line.GetColor(), line.GetOpacity());
	std::vector<DebugSegment>& out = xray ? m_xraySegments : m_list.segments;

	// Vertices are segment endpoint pairs; a trailing odd vertex is dropped.
	for (size_t i = 0; i + 1 < verts.size(); i += 2)
	{
		DebugSegment seg;
		seg.a = glm::vec3(model * glm::vec4(verts[i], 1.0f));
		seg.b = glm::vec3(model * glm::vec4(verts[i + 1], 1.0f));
		seg.width = line.GetWidth();
		seg.rgba = rgba;
		seg.flags = flags;
		out.push_back(seg);
	}
}

void DebugDrawBuilder::AppendDebugPoints(const DebugPoints& points)
{
	const std::vector<glm::vec3>& pts = points.GetPoints();
	if (pts.empty())
		return;

	const bool xray = points.GetXRay();
	// Sprites have no dash style to protect, so the derived smoothing of
	// AppendDebugLine is unconditional here: every shape mask is an analytic
	// distance the fragment shader can fade, and a rhombus or circle edge is
	// visibly stepped without it.
	uint32_t flags = (xray ? DebugFlag::XRay : DebugFlag::None) | DebugFlag::Smooth;
	if (points.GetProjectionMode() == 0)
		flags |= DebugFlag::ScreenSpaceSize;

	const glm::mat4 model = points.GetModelMatrix();
	const uint32_t rgba = PackTinted(points.GetColor(), points.GetOpacity());

	// CUBE has no sprite form: emit wireframe cubes as segments instead. Their
	// size is world-space by definition, so ScreenSpaceSize does not apply.
	if (points.GetPointType() == DebugPoints::PointType::CUBE)
	{
		std::vector<DebugSegment>& out = xray ? m_xraySegments : m_list.segments;
		// Cube edges are solid by construction — DebugPoints has no line style —
		// so they take the same smoothing a solid DebugLine gets.
		const uint32_t segFlags = (xray ? DebugFlag::XRay : DebugFlag::None) | DebugFlag::Smooth;
		const float half = points.GetScale() * 0.5f;
		for (const glm::vec3& p : pts)
			AppendCubeEdges(out, glm::vec3(model * glm::vec4(p, 1.0f)), half, 1.0f, rgba, segFlags);
		return;
	}

	std::vector<DebugPointSprite>& out = xray ? m_xrayPoints : m_list.points;
	const uint32_t shape = ShapeOf(points.GetPointType());
	for (const glm::vec3& p : pts)
	{
		DebugPointSprite sprite;
		sprite.p = glm::vec3(model * glm::vec4(p, 1.0f));
		sprite.size = points.GetScale();
		sprite.rgba = rgba;
		sprite.shape = shape;
		sprite.flags = flags;
		out.push_back(sprite);
	}
}

void DebugDrawBuilder::AppendDebugMesh(const DebugMesh& mesh)
{
	// Wireframe is drawn from the mesh's MeshGPU, which only exists once the
	// geometry has been uploaded; without it there is nothing to reference.
	if (!mesh.HasGeometry())
		return;

	DebugWireMesh wire;
	wire.model = mesh.GetModelMatrix();
	wire.rgba = PackTinted(mesh.GetColor(), mesh.GetOpacity());
	// No Smooth bit here, unlike segments and sprites: a wire mesh is rasterized
	// by PolygonMode::eLine, so the fragment stage gets no distance-to-edge to
	// fade — debug_wire.frag is a flat color write with nothing to branch on.
	// Setting the flag would only advertise a behaviour no consumer implements.
	wire.flags = mesh.GetXRay() ? DebugFlag::XRay : DebugFlag::None;
	wire.meshObjectId = mesh.GetObjectID();

	// Wire meshes are not partitioned: there are few of them and DebugPass
	// already switches pipeline per mesh to honour its depth mode.
	m_list.wireMeshes.push_back(wire);
}

} // namespace neurus
