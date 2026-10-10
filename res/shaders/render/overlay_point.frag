#version 450
// ---------------------------------------------------------------------------
// Overlay Point Fragment Shader — gl_PointCoord shape masking
//
// gl_PointCoord runs [0,1]² across the rasterized point, so every sprite shape
// is a discard test on the distance from its centre. All three shapes are
// centre-symmetric, which is why the origin convention (upper-left in Vulkan,
// lower-left in GL) does not matter here.
//
// A cube has no entry: it is not a sprite at all, the producer decomposes it
// into 12 OverlaySegments instead.
// ---------------------------------------------------------------------------

// --- Shape ids (must match neurus::OverlayPointShape) ---
const uint SHAPE_SQUARE  = 0u;
const uint SHAPE_RHOMBUS = 1u;
const uint SHAPE_CIRCLE  = 2u;

// --- OverlayFlag bits (must match neurus::OverlayFlag) ---
const uint FLAG_SMOOTH = 4u;

layout(location = 0) flat in vec4 vColor;
layout(location = 1) flat in uint vShape;
layout(location = 2) flat in uint vFlags;

layout(location = 0) out vec4 outColor;

void main()
{
	// Signed offset from the sprite centre, in [-0.5, 0.5].
	const vec2 c = gl_PointCoord - vec2(0.5);

	// Distance to the shape boundary in the same units, positive inside. Squares
	// use the Chebyshev norm, rhombi the Manhattan norm, circles the Euclidean
	// one — the three unit balls of the p-norms we need.
	float inside;
	if (vShape == SHAPE_RHOMBUS)
		inside = 0.5 - (abs(c.x) + abs(c.y));
	else if (vShape == SHAPE_CIRCLE)
		inside = 0.5 - length(c);
	else
		inside = 0.5 - max(abs(c.x), abs(c.y));

	if (inside < 0.0)
		discard;

	float alpha = vColor.a;

	if ((vFlags & FLAG_SMOOTH) != 0u)
	{
		// fwidth() converts the normalized distance into pixels, which is what
		// makes one edge pixel's worth of fade correct at any sprite size.
		const float aa = fwidth(inside);
		alpha *= aa > 0.0 ? clamp(inside / aa, 0.0, 1.0) : 1.0;
	}

	outColor = vec4(vColor.rgb, alpha);
}
