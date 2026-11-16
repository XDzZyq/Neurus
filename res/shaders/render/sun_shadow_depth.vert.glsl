#version 450
// ---------------------------------------------------------------------------
// Sun Shadow Depth Vertex Shader — Directional Light Orthographic Depth Pass
//
// Transforms vertex positions through a single orthographic light view-projection
// matrix provided via push constant. No inter-stage outputs are needed for
// depth-only rendering.
//
// Design:
//   - Non-multiview: a single mat4 lightViewProj push constant covers the
//     entire orthographic frustum (one draw per cascade if cascaded).
//   - No SSBO needed — just one mat4 push constant (64 bytes).
//   - Input matches MeshData GPU layout: location 0 = vec3 position.
// ---------------------------------------------------------------------------

// --- Vertex input (matches MeshData layout: pos(3), normal(3), uv(2)) ---
layout(location = 0) in vec3 inPosition;

// --- Push constants ---
layout(push_constant) uniform PushConstants
{
	mat4 lightViewProj;
} pc;

void main()
{
	gl_Position = pc.lightViewProj * vec4(inPosition, 1.0);
}
