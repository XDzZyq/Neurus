#version 450
// ---------------------------------------------------------------------------
// Debug Line Vertex Shader — screen-space quad expansion
//
// Each DebugSegment in the SSBO is drawn as six vertices (two triangles) that
// form a screen-space quad of the requested pixel width. Wide-line rasterization
// is not used: Metal (hence MoltenVK) clamps lineWidth to 1.0 and exposes none
// of VK_KHR_line_rasterization's rectangular/smooth/stippled modes, so the quad
// is the only way to get thickness, antialiasing and stipple on every platform.
//
// The draw is issued as vkCmdDraw(6 * count, 1, 6 * firstSegment, 0), so
// gl_VertexIndex is already absolute: gl_VertexIndex / 6 selects the segment and
// gl_VertexIndex % 6 the quad corner. That is what lets the depth-tested and
// x-ray halves of the list be drawn as two ranges of one buffer.
//
// Layout contract: `Segment` below mirrors neurus::DebugSegment
// (src/scene/DebugDrawList.h) field for field, including its padding.
// ---------------------------------------------------------------------------

// --- DebugFlag bits (must match neurus::DebugFlag) ---
const uint FLAG_SCREEN_SPACE_SIZE = 8u;

struct Segment
{
	vec3  a;
	float width;
	vec3  b;
	uint  rgba;
	uint  flags;
	uint  _pad0;
	uint  _pad1;
	uint  _pad2;
};

// --- Camera UBO (set 0, binding 0) ---
layout(set = 0, binding = 0) uniform CameraUBO
{
	mat4 viewProj;
	mat4 view;
} camera;

// --- Segment storage (set 0, binding 1) ---
layout(set = 0, binding = 1, std430) readonly buffer SegmentBuffer
{
	Segment segments[];
};

// --- Per-frame push constants (shared with debug_point.vert) ---
layout(push_constant) uniform PushConstants
{
	vec2  viewportSize;    // Render target size in pixels.
	float projScaleY;      // Pixels per world unit at w = 1 (halfHeight * proj[1][1]).
	float maxPointSizePx;  // Device pointSizeRange[1]; unused here.
} pc;

// --- Inter-stage outputs ---
layout(location = 0) flat out vec4  vColor;
layout(location = 1)      out float vSide;     // -1..1 across the line width
layout(location = 2)      out float vArcPx;    // Screen-space distance along the segment
layout(location = 3) flat out float vWidthPx;
layout(location = 4) flat out uint  vFlags;

// Quad corners are derived arithmetically rather than looked up in a const array:
// ShaderLibrary round-trips every shader through ShaderParser/ShaderGenerator, and
// that IR has no representation for a multi-line `vec2[6](...)` initializer — it
// would be regenerated truncated. Arithmetic also drops a 48-byte constant.
//
// Corner index q addresses the quad as (endpoint, side):
//   q = 0 -> (a, -1)   q = 1 -> (a, +1)   q = 2 -> (b, -1)   q = 3 -> (b, +1)
// The six vertices are emitted in the order 0,1,2, 3,2,1 — two triangles covering
// the quad — precisely so the mapping is `corner < 3 ? corner : 6 - corner`.
// Winding is irrelevant here: the pipeline culls nothing.

/// Smallest clip-space w treated as being in front of the camera.
const float kMinW = 1e-4;

void main()
{
	const uint segIndex = uint(gl_VertexIndex) / 6u;
	const uint corner   = uint(gl_VertexIndex) % 6u;

	// 0,1,2, 3,2,1 -> quad corner index, then to (endpoint, side) signs.
	const uint quadIdx = corner < 3u ? corner : 6u - corner;
	const vec2 kQuad   = vec2(quadIdx >= 2u ? 1.0 : -1.0,
	                         (quadIdx & 1u) == 1u ? 1.0 : -1.0);

	Segment s = segments[segIndex];

	vec4 clipA = camera.viewProj * vec4(s.a, 1.0);
	vec4 clipB = camera.viewProj * vec4(s.b, 1.0);

	// Both endpoints behind the camera: emit a degenerate vertex outside the
	// clip volume (z < 0 with GLM_FORCE_DEPTH_ZERO_TO_ONE) so nothing rasterizes.
	// Dividing by a negative w without this would mirror the segment on screen.
	if (clipA.w < kMinW && clipB.w < kMinW)
	{
		gl_Position = vec4(0.0, 0.0, -1.0, 1.0);
		vColor    = vec4(0.0);
		vSide     = 0.0;
		vArcPx    = 0.0;
		vWidthPx  = 1.0;
		vFlags    = 0u;
		return;
	}

	// Exactly one endpoint behind the camera: slide it along the segment up to
	// the near plane. Interpolating in homogeneous space keeps the on-screen
	// direction of the visible part exact.
	if (clipA.w < kMinW)
		clipA = mix(clipA, clipB, (kMinW - clipA.w) / (clipB.w - clipA.w));
	else if (clipB.w < kMinW)
		clipB = mix(clipB, clipA, (kMinW - clipB.w) / (clipA.w - clipB.w));

	const vec2 halfVp  = pc.viewportSize * 0.5;
	const vec2 screenA = (clipA.xy / clipA.w) * halfVp;
	const vec2 screenB = (clipB.xy / clipB.w) * halfVp;

	const vec2  delta = screenB - screenA;
	const float lenPx = length(delta);
	const vec2  dir   = lenPx > 1e-6 ? delta / lenPx : vec2(1.0, 0.0);
	const vec2  nrm   = vec2(-dir.y, dir.x);

	// Line thickness is always in pixels — see DebugSegment::width. World-space
	// thickness would need a per-fragment depth-dependent width, which no
	// screen-space quad can represent; use point sprites when a world-sized
	// marker is wanted (they honour FLAG_SCREEN_SPACE_SIZE).
	const float widthPx = max(s.width, 1.0);
	const float halfW   = widthPx * 0.5;

	const bool  atB     = kQuad.x > 0.0;
	const vec4  clip    = atB ? clipB : clipA;
	const vec2  screen  = atB ? screenB : screenA;

	// Extend each end by half the width so the quad has square caps; without
	// them a 1-pixel segment shorter than its width would vanish.
	const vec2 offset = nrm * halfW * kQuad.y
	                  + dir * halfW * (atB ? 1.0 : -1.0);

	// Back to clip space: undo the halfVp scale and re-apply this endpoint's w.
	gl_Position = vec4(((screen + offset) / halfVp) * clip.w, clip.z, clip.w);

	vColor   = unpackUnorm4x8(s.rgba);
	vSide    = kQuad.y;
	vArcPx   = atB ? lenPx + halfW : -halfW;
	vWidthPx = widthPx;
	vFlags   = s.flags;
}
