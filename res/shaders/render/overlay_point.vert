#version 450
// ---------------------------------------------------------------------------
// Overlay Point Vertex Shader — native point sprites
//
// One vertex per OverlayPointSprite, drawn with VK_PRIMITIVE_TOPOLOGY_POINT_LIST.
// Unlike lines, points do not need quad expansion: gl_PointSize is honoured by
// every backend we target (MoltenVK reports pointSizeRange [1, 511]), and the
// fragment shader can mask gl_PointCoord to get the sprite shape for free.
//
// gl_PointSize MUST be written in Vulkan — unlike desktop GL there is no
// fixed-function default, and a size above 1.0 requires the largePoints
// feature (enabled in VulkanContext::selectOptionalFeatures).
//
// Shared by every overlay payload: DebugPass draws a DebugDrawList with it and
// TransformGizmoPass draws a GizmoDrawList, because both hold the same records.
//
// Layout contract: `PointSprite` below mirrors neurus::OverlayPointSprite
// (src/scene/OverlayGeometry.h) field for field, including its padding.
// ---------------------------------------------------------------------------

// --- OverlayFlag bits (must match neurus::OverlayFlag) ---
const uint FLAG_SCREEN_SPACE_SIZE = 8u;

struct PointSprite
{
	vec3  p;
	float size;
	uint  rgba;
	uint  shape;
	uint  flags;
	uint  _pad;
};

// --- Camera UBO (set 0, binding 0) ---
layout(set = 0, binding = 0) uniform CameraUBO
{
	mat4 viewProj;
	mat4 view;
} camera;

// --- Point storage (set 0, binding 2) ---
layout(set = 0, binding = 2, std430) readonly buffer PointBuffer
{
	PointSprite points[];
};

// --- Per-frame push constants (shared with overlay_line.vert) ---
layout(push_constant) uniform PushConstants
{
	vec2  viewportSize;    // Render target size in pixels.
	float projScaleY;      // Pixels per world unit at w = 1 (halfHeight * proj[1][1]).
	float maxPointSizePx;  // Device pointSizeRange[1]; sizes are clamped to it.
} pc;

// --- Inter-stage outputs ---
layout(location = 0) flat out vec4 vColor;
layout(location = 1) flat out uint vShape;
layout(location = 2) flat out uint vFlags;

/// Smallest clip-space w treated as being in front of the camera.
const float kMinW = 1e-4;

void main()
{
	PointSprite s = points[uint(gl_VertexIndex)];

	const vec4 clip = camera.viewProj * vec4(s.p, 1.0);
	gl_Position = clip;

	// World-sized sprites shrink with distance; screen-sized ones do not. The
	// CPU supplies projScaleY (half the viewport height times proj[1][1]) so the
	// perspective divide is the only work left here.
	float sizePx = s.size;
	if ((s.flags & FLAG_SCREEN_SPACE_SIZE) == 0u)
		sizePx = s.size * pc.projScaleY / max(clip.w, kMinW);

	// Exceeding pointSizeRange[1] is undefined behaviour, not a soft clamp, so
	// the limit travels in from the device rather than being hardcoded.
	gl_PointSize = clamp(sizePx, 1.0, pc.maxPointSizePx);

	vColor = unpackUnorm4x8(s.rgba);
	vShape = s.shape;
	vFlags = s.flags;
}
