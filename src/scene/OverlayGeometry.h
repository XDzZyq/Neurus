/**
 * @file OverlayGeometry.h
 * @brief Vertex records shared by every screen-space overlay payload.
 *
 * These are the primitives the overlay shaders (res/shaders/render/overlay_line.*
 * and overlay_point.*) consume verbatim as std430 SSBO arrays. They are defined
 * here, apart from any particular payload container, because two unrelated
 * features draw with them:
 *
 *   - DebugDrawList  — retained, deterministic data visualization (issue #22).
 *                      Lives as long as the scene objects it mirrors.
 *   - GizmoDrawList  — transient interaction feedback for a modal transform.
 *                      Lives for the duration of one gesture.
 *
 * The two have opposite lifetimes and opposite authoring rules, so they stay
 * separate containers. What they legitimately share is the *record layout* and
 * therefore the shader program: one vertex record, two payloads. Duplicating the
 * record would force a converting copy at the cache boundary and stop the two
 * caches sharing a memcpy shape, for no gain.
 *
 * GPU layout contract: field order and padding MUST stay in sync with
 * overlay_line.vert and overlay_point.vert. The static_asserts below guard the
 * sizes, not the field order.
 *
 * Positions are always world-space. Producers bake their object's transform in
 * while flattening rather than passing a matrix index the shader would
 * dereference: the CPU already touches every primitive to copy it, so folding a
 * mat4 multiply into that pass costs almost nothing and removes a whole SSBO,
 * descriptor binding and indirection from the GPU side.
 *
 * Layer placement: the Vulkan-free scene layer, like EditorContext.h, so the
 * editor may write these records and the renderer may read them without either
 * including the other.
 */

#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace neurus
{

/**
 * @brief Per-primitive bit flags, shared verbatim with the overlay shaders.
 *
 * Plain uint32_t constants rather than an enum class: the value is written
 * straight into an SSBO field and tested with bitwise AND in GLSL, so the
 * enum-class conversion boilerplate would buy nothing.
 */
struct OverlayFlag
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
	 * a per-fragment depth-dependent width it cannot represent. OverlaySegment
	 * width is therefore always pixels, and this bit is ignored for segments.
	 */
	static constexpr uint32_t ScreenSpaceSize = 1u << 3;
};

/**
 * @brief Packs a linear RGBA color into a single 8-bit-per-channel word.
 *
 * Overlay geometry is authored in display-referred color and drawn after
 * ComposePass has already tonemapped, so no gamma conversion happens here:
 * what is packed is what appears.
 *
 * @param color RGBA in [0,1]; values outside the range are clamped.
 * @return Color packed as 0xAABBGGRR (matches GLSL unpackUnorm4x8).
 */
inline uint32_t PackOverlayColor(const glm::vec4& color)
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
struct OverlaySegment
{
	glm::vec3 a{0.0f};                   ///< Start point, in world space.
	float width{1.0f};                   ///< Line width in pixels (see OverlayFlag::ScreenSpaceSize).
	glm::vec3 b{0.0f};                   ///< End point, in world space.
	uint32_t rgba{0xFFFFFFFFu};          ///< Packed color (see PackOverlayColor).
	uint32_t flags{OverlayFlag::None};   ///< OverlayFlag bits.
	uint32_t _pad[3]{0u, 0u, 0u};        ///< Pads to the 16-byte std430 struct alignment.
};

static_assert(sizeof(OverlaySegment) == 48, "OverlaySegment must stay 48 B to match its std430 SSBO layout");
static_assert(alignof(OverlaySegment) == 4, "OverlaySegment is memcpy'd verbatim; no host padding expected");

/**
 * @brief One point sprite, drawn as a native VK_PRIMITIVE_TOPOLOGY_POINT_LIST point.
 *
 * `size` becomes gl_PointSize (requires the largePoints feature, enabled in
 * VulkanContext::selectOptionalFeatures) and `shape` selects how the fragment
 * shader masks gl_PointCoord. A cube has no sprite form — the producer
 * decomposes it into 12 OverlaySegments instead.
 */
struct OverlayPointSprite
{
	glm::vec3 p{0.0f};                   ///< Position, in world space.
	float size{4.0f};                    ///< Sprite diameter (pixels if ScreenSpaceSize, else world units).
	uint32_t rgba{0xFFFFFFFFu};          ///< Packed color (see PackOverlayColor).
	uint32_t shape{0u};                  ///< 0 = square, 1 = rhombus, 2 = circle (mirrors OverlayPointShape).
	uint32_t flags{OverlayFlag::None};   ///< OverlayFlag bits.
	uint32_t _pad{0u};                   ///< Pads to the 16-byte std430 struct alignment.
};

static_assert(sizeof(OverlayPointSprite) == 32, "OverlayPointSprite must stay 32 B to match its std430 SSBO layout");

/**
 * @brief Sprite shape ids, kept numerically in sync with DebugPoints::PointType.
 */
struct OverlayPointShape
{
	static constexpr uint32_t Square = 0u;
	static constexpr uint32_t Rhombus = 1u;
	static constexpr uint32_t Circle = 2u;
};

} // namespace neurus
