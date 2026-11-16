#version 450
// ---------------------------------------------------------------------------
// Debug Wireframe Fragment Shader — flat per-object color
//
// Wireframe edges are drawn in one unlit color per DebugMesh; there is no
// lighting, no texture and no per-vertex variation, so the whole shader is the
// interpolant the vertex stage already unpacked from the push constant.
// ---------------------------------------------------------------------------

layout(location = 0) flat in vec4 vColor;

layout(location = 0) out vec4 outColor;

void main()
{
	outColor = vColor;
}
