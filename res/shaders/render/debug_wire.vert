#version 450
// ---------------------------------------------------------------------------
// Debug Wireframe Vertex Shader — VK_POLYGON_MODE_LINE over an existing mesh
//
// DebugMesh reuses the MeshGPU vertex and index buffers a Mesh already uploaded,
// so this shader must accept the full MeshData vertex layout even though only
// the position is read: the pipeline's vertex input state is shared with
// gbuffer.vert, and dropping attributes here would not shrink the stride.
//
// Rasterization does the wireframe work (polygonMode = eLine, requires the
// fillModeNonSolid feature). Edge width is therefore fixed at 1 px wherever
// wideLines is unavailable — which includes MoltenVK — so a thick outline has to
// be authored as DebugLine segments instead.
// ---------------------------------------------------------------------------

// --- Vertex inputs (matches MeshData layout: pos(3), normal(3), uv(2)) ---
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

// --- Camera UBO (set 0, binding 0) ---
layout(set = 0, binding = 0) uniform CameraUBO
{
	mat4 viewProj;
	mat4 view;
} camera;

// --- Per-draw push constants (one DebugWireMesh) ---
layout(push_constant) uniform PushConstants
{
	mat4 model;
	uint rgba;
	uint flags;
} pc;

// --- Inter-stage outputs ---
layout(location = 0) flat out vec4 vColor;

void main()
{
	gl_Position = camera.viewProj * pc.model * vec4(inPosition, 1.0);
	vColor = unpackUnorm4x8(pc.rgba);
}
