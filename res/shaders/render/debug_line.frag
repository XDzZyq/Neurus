#version 450
// ---------------------------------------------------------------------------
// Debug Line Fragment Shader — stipple + edge antialiasing
//
// The vertex shader hands over two screen-space coordinates: vSide, which spans
// [-1, 1] across the quad's width, and vArcPx, the distance in pixels along the
// segment. Both are needed because the quad is expanded in pixel units, so
// dashes stay a constant on-screen length no matter how the segment is oriented
// or how far away it is.
//
// Blending is alpha-over (set in DebugPass); this shader only produces the
// coverage, it never reads the target.
// ---------------------------------------------------------------------------

// --- DebugFlag bits (must match neurus::DebugFlag) ---
const uint FLAG_STIPPLE = 2u;
const uint FLAG_SMOOTH  = 4u;

/// Dash period in pixels; the first half of each period is drawn.
const float kStipplePeriodPx = 8.0;

layout(location = 0) flat in vec4  vColor;
layout(location = 1)      in float vSide;
layout(location = 2)      in float vArcPx;
layout(location = 3) flat in float vWidthPx;
layout(location = 4) flat in uint  vFlags;

layout(location = 0) out vec4 outColor;

void main()
{
	if ((vFlags & FLAG_STIPPLE) != 0u)
	{
		// vArcPx can be negative inside the start cap, and fract() of a negative
		// number still lands in [0,1) in GLSL, so no offset is needed here.
		if (fract(vArcPx / kStipplePeriodPx) > 0.5)
			discard;
	}

	float alpha = vColor.a;

	if ((vFlags & FLAG_SMOOTH) != 0u)
	{
		// Distance from this fragment to the quad edge, in pixels: vSide covers
		// vWidthPx pixels over its [-1,1] range. Fading the outermost pixel is
		// enough to hide the staircase and costs no extra samples.
		const float edgePx = (1.0 - abs(vSide)) * vWidthPx * 0.5;
		alpha *= clamp(edgePx, 0.0, 1.0);
	}

	outColor = vec4(vColor.rgb, alpha);
}
