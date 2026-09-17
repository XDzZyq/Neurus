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
 * GPU layout contract: DebugSegment and DebugPointSprite are uploaded verbatim
 * as std430 SSBO arrays. Their field order and padding MUST stay in sync with
 * res/shaders/render/debug_line.vert and debug_point.vert; the static_asserts
 * below guard the sizes, not the field order.
 *
 * Positions are always world-space. Producers bake their object's transform in
 * while flattening rather than passing a matrix index the shader would
 * dereference: the CPU already touches every primitive to copy it, so folding a
 * mat4 multiply into that pass costs almost nothing and removes a whole SSBO,
 * descriptor binding and indirection from the GPU side. Only DebugWireMesh keeps
 * a matrix, because its geometry is never copied — it is drawn straight from the
 * mesh's existing GPU buffers with the transform in a push constant.
 */

#pragma once

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace neurus
{

/**
 * @brief Per-primitive bit flags, shared verbatim with the debug shaders.
 *
 * Plain uint32_t constants rather than an enum class: the value is written
 * straight into an SSBO field and tested with bitwise AND in GLSL, so the
 * enum-class conversion boilerplate would buy nothing.
 */
struct DebugFlag
{
	static constexpr uint32_t None = 0u;

	/// @brief Draw on top of everything (depth test disabled) instead of being occluded.
	static constexpr uint32_t XRay = 1u << 0;

	/// @brief Dashed line, phase computed from screen-space arc length.
	static constexpr uint32_t Stipple = 1u << 1;

	/// @brief Anti-alias edges by fading alpha near the primitive boundary.
	static constexpr uint32_t Smooth = 1u << 2;

	/**
	 * @brief Size is in pixels (constant on screen) rather than world units.
	 *
	 * Point sprites honour both settings. Segments do not: a screen-space quad
	 * has one width for its whole length, so a world-space thickness would need
	 * a per-fragment depth-dependent width it cannot represent. DebugSegment
	 * width is therefore always pixels, and this bit is ignored for segments.
	 */
	static constexpr uint32_t ScreenSpaceSize = 1u << 3;
};

/**
 * @brief Packs a linear RGBA color into a single 8-bit-per-channel word.
 *
 * Debug geometry is authored in display-referred color and drawn after
 * ComposePass has already tonemapped, so no gamma conversion happens here:
 * what is packed is what appears.
 *
 * @param color RGBA in [0,1]; values outside the range are clamped.
 * @return Color packed as 0xAABBGGRR (matches GLSL unpackUnorm4x8).
 */
inline uint32_t PackDebugColor(const glm::vec4& color)
{
	const glm::vec4 c = glm::clamp(color, glm::vec4(0.0f), glm::vec4(1.0f)) * 255.0f + 0.5f;
	return (static_cast<uint32_t>(c.r))
	     | (static_cast<uint32_t>(c.g) << 8)
	     | (static_cast<uint32_t>(c.b) << 16)
	     | (static_cast<uint32_t>(c.a) << 24);
}

/**
 * @brief One line segment. Expanded into a screen-space quad by the vertex shader.
 *
 * Thick lines are built as quads rather than rasterized as wide lines because
 * Metal (and therefore MoltenVK) caps lineWidth at 1.0 and exposes none of
 * VK_KHR_line_rasterization's rectangular/smooth/stippled modes. Quad expansion
 * costs six vertices per segment and in exchange gives arbitrary width plus
 * shader-side smoothing and stipple on every platform.
 */
struct DebugSegment
{
	glm::vec3 a{0.0f};                 ///< Start point, in world space.
	float width{1.0f};                 ///< Line width in pixels (see DebugFlag::ScreenSpaceSize).
	glm::vec3 b{0.0f};                 ///< End point, in world space.
	uint32_t rgba{0xFFFFFFFFu};        ///< Packed color (see PackDebugColor).
	uint32_t flags{DebugFlag::None};   ///< DebugFlag bits.
	uint32_t _pad[3]{0u, 0u, 0u};      ///< Pads to the 16-byte std430 struct alignment.
};

static_assert(sizeof(DebugSegment) == 48, "DebugSegment must stay 48 B to match its std430 SSBO layout");
static_assert(alignof(DebugSegment) == 4, "DebugSegment is memcpy'd verbatim; no host padding expected");

/**
 * @brief One point sprite, drawn as a native VK_PRIMITIVE_TOPOLOGY_POINT_LIST point.
 *
 * `size` becomes gl_PointSize (requires the largePoints feature, enabled in
 * VulkanContext::selectOptionalFeatures) and `shape` selects how the fragment
 * shader masks gl_PointCoord. DebugPoints::PointType::CUBE has no sprite form —
 * the producer decomposes it into 12 DebugSegments instead.
 */
struct DebugPointSprite
{
	glm::vec3 p{0.0f};                 ///< Position, in world space.
	float size{4.0f};                  ///< Sprite diameter (pixels if ScreenSpaceSize, else world units).
	uint32_t rgba{0xFFFFFFFFu};        ///< Packed color (see PackDebugColor).
	uint32_t shape{0u};                 ///< 0 = square, 1 = rhombus, 2 = circle (mirrors PointType).
	uint32_t flags{DebugFlag::None};   ///< DebugFlag bits.
	uint32_t _pad{0u};                 ///< Pads to the 16-byte std430 struct alignment.
};

static_assert(sizeof(DebugPointSprite) == 32, "DebugPointSprite must stay 32 B to match its std430 SSBO layout");

/**
 * @brief Sprite shape ids, kept numerically in sync with DebugPoints::PointType.
 */
struct DebugPointShape
{
	static constexpr uint32_t Square = 0u;
	static constexpr uint32_t Rhombus = 1u;
	static constexpr uint32_t Circle = 2u;
};

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
