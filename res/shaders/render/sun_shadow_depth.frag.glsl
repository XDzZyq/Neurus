#version 450
// ---------------------------------------------------------------------------
// Sun Shadow Depth Fragment Shader — Directional Light Orthographic Depth Pass
//
// Depth-only output for an orthographic projection.
// gl_FragDepth is set to gl_FragCoord.z, which is already in Vulkan's NDC
// [0, 1] depth range for orthographic projection — no manual computation needed.
//
// early_fragment_tests enables depth-test-before-shader optimisation,
// dropping fragments that would fail the depth test before entering main().
// ---------------------------------------------------------------------------

layout(early_fragment_tests) in;

void main()
{
	gl_FragDepth = gl_FragCoord.z;
}
