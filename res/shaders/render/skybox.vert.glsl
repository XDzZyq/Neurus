#version 450
// ---------------------------------------------------------------------------
// Skybox Vertex Shader — Fullscreen Triangle
//
// Generates a fullscreen triangle without vertex buffers, using
// gl_VertexIndex to produce screen-space coordinates.  The fragment
// shader computes ray directions from these coordinates.
//
// Triangle covers: (-1,-1), (3,-1), (-1,3) in NDC
// ---------------------------------------------------------------------------

vec2 positions[3] = vec2[](
	vec2(-1.0, -1.0),
	vec2( 3.0, -1.0),
	vec2(-1.0,  3.0)
);

layout(location = 0) out vec2 v_TexCoord;

void main()
{
	vec2 pos = positions[gl_VertexIndex];
	gl_Position = vec4(pos, 0.0, 1.0);
	v_TexCoord  = pos * 0.5 + 0.5;
}
