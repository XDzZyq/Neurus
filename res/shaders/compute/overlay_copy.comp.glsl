#version 450
// ---------------------------------------------------------------------------
// OverlayCopy Compute Shader - seeds the overlay image from the scene tail
//
// Copies the scene's converged tail image (ComposedOutput, or FXAAOutput when
// anti-aliasing is on) into OverlayOutput and blends the selection highlight
// mask onto it in one dispatch.
//
// Both halves have to happen in the same dispatch: the overlay rasters below
// draw over this image with LOAD_OP_LOAD, so OverlayOutput must never be written
// in place over the scene image (an overlay frame has to be able to redraw the
// converged picture without re-running the scene graph), and a second dispatch
// reading OverlayOutput while writing it would be a feedback loop - the same
// texel sampled and stored in one dispatch, whose result Vulkan leaves
// undefined.
//
// The highlight is blended here, after tone mapping and gamma, so the outline
// is deliberately not anti-aliased by FXAA: it is drawn on top of the final
// image, exactly like the debug and gizmo overlays.
//
// Dispatch: ceil(width/16), ceil(height/16), 1
// ---------------------------------------------------------------------------

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// --- Inputs ---
layout(binding = 0) uniform sampler2D gSceneTail;  // converged scene image
layout(binding = 1) uniform sampler2D gHighlight;  // selection edge mask (R8)

// --- Output ---
layout(rgba16f, binding = 2) uniform writeonly image2D outputOverlay;

// Highlight colour: orange, 50 % intensity - the value ComposePass used to
// blend before the outline moved out of the scene graph.
const vec3 kHighlightColour = vec3(1.0, 0.55, 0.0);
const float kHighlightAlpha = 0.5;

void main()
{
	ivec2 pixelCoord = ivec2(gl_GlobalInvocationID.xy);
	ivec2 imgSize    = imageSize(outputOverlay);

	if (pixelCoord.x >= imgSize.x || pixelCoord.y >= imgSize.y)
		return;

	vec4 scene     = texelFetch(gSceneTail, pixelCoord, 0);
	float highlight = texelFetch(gHighlight, pixelCoord, 0).r;

	// A no-op mix for mask value 0, so the copy is exact for unselected pixels.
	vec3 colour = mix(scene.rgb, kHighlightColour, highlight * kHighlightAlpha);

	imageStore(outputOverlay, pixelCoord, vec4(colour, scene.a));
}
