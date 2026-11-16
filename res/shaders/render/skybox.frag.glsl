#version 450
// ---------------------------------------------------------------------------
// Skybox Fragment Shader — Environment Background
//
// Computes a ray direction from the camera through the pixel, samples the
// prefiltered specular cubemap at mip 0 (sharp reflection), and writes
// the result.  Rendered as a fullscreen pass after geometry.
//
// Writes directly to the HDRColor attachment via the framebuffer.
// ---------------------------------------------------------------------------

layout(location = 0) in vec2 v_TexCoord;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform samplerCube u_Skybox;

layout(push_constant) uniform PushConstants
{
	mat4 invProjView;  // inverse(projection * view)
};

void main()
{
	// Reconstruct world-space ray direction from NDC and inverse VP
	// v_TexCoord goes from [0,1] → NDC [-1,1]
	vec4 clipPos = vec4(v_TexCoord * 2.0 - 1.0, 0.0, 1.0);
	vec4 worldDir = invProjView * clipPos;
	worldDir /= worldDir.w;  // perspective divide

	vec3 dir = normalize(worldDir.xyz);

	outColor = texture(u_Skybox, dir, 0.0);
}
