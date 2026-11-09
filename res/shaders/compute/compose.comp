#version 450
// ---------------------------------------------------------------------------
// Compose Compute Shader - tone map the HDR scene image
//
// Reads HDRColor (RGBA16F), applies gamma correction, and writes the final
// composed image to the output attachment. This is the last scene pass: the
// selection highlight, the debug overlay and the gizmo are blended over the
// result afterwards by the overlay engine, which is why nothing here knows
// about them (and why FXAA, which sits between compose and the overlays, never
// sees an overlay pixel).
//
// Dispatch: ceil(width/16), ceil(height/16), 1
// ---------------------------------------------------------------------------

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// --- Input ---
layout(binding = 0) uniform sampler2D gHDRColor;

// --- Output ---
layout(rgba16f, binding = 1) uniform writeonly image2D outputComposed;

// --- Push constants ---
layout(push_constant) uniform PushConstants
{
	float gamma;     // gamma correction factor (default 2.2)
};

// Checkerboard background for transparent/empty regions
const int kCheckerSize   = 8;
const vec3 kCheckerGray1 = vec3(0.0);
const vec3 kCheckerGray2 = vec3(0.01);

void main()
{
	ivec2 pixelCoord = ivec2(gl_GlobalInvocationID.xy);
	ivec2 imgSize    = imageSize(outputComposed);

	if (pixelCoord.x >= imgSize.x || pixelCoord.y >= imgSize.y)
		return;

	// Read input (RGBA for background detection)
	vec4 hdrSample = texelFetch(gHDRColor, pixelCoord, 0);

	// --- Background detection (alpha = 0 means no geometry rendered) ---
	bool isBackground = (hdrSample.a < 0.01);

	if (isBackground)
	{
		// Checkerboard grid
		ivec2 cell = pixelCoord / kCheckerSize;
		bool isGray1 = (cell.x % 8 > 3) != (cell.y % 8 > 3);
		vec3 bgColor = isGray1 ? kCheckerGray1 : kCheckerGray2;

		// Gamma-correct the background too (consistent look)
		float invGamma = 1.0 / max(gamma, 0.01);
		bgColor = pow(max(bgColor, vec3(0.0)), vec3(invGamma));
		imageStore(outputComposed, pixelCoord, vec4(bgColor, 1.0));
		return;
	}

	// Gamma correction
	float invGamma = 1.0 / max(gamma, 0.01);
	vec3 composed = pow(max(hdrSample.rgb, vec3(0.0)), vec3(invGamma));

	imageStore(outputComposed, pixelCoord, vec4(composed, 1.0));
}
