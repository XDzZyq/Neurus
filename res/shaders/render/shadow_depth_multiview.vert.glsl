#version 450
#extension GL_EXT_multiview : require
// ---------------------------------------------------------------------------
// Shadow Depth Vertex Shader (Multiview) — Point Light Cubemap Depth Pass
//
// Uses gl_ViewIndex from VK_KHR_multiview to select the correct cubemap face
// view-projection matrix. All 6 faces are rendered in a single dynamic
// rendering pass via viewMask = 0x3F.
//
// Design:
//   - 6 static face view-projection matrices are stored in an SSBO
//     (computed once from origin with a fixed far plane).
//   - lightWorldPos and farPlane come via push constants (updated per light).
//   - The vertex is offset by lightWorldPos before applying faceVP,
//     equivalent to: proj * lookAt(lightPos, lightPos+dir, up) * worldPos.
// ---------------------------------------------------------------------------

// --- Vertex input (matches MeshData GPU layout: pos(3), normal(3), uv(2)) ---
layout(location = 0) in vec3 inPosition;

// --- Inter-stage output ---
layout(location = 0) out vec3 fragWorldPos;

// --- Static face view-projection matrices (set = 0, binding = 0, std430 SSBO) ---
// Computed from origin: proj * lookAt(0, faceDir, faceUp) for each of 6 cubemap faces.
layout(set = 0, binding = 0, std430) readonly buffer FaceMatrices
{
	mat4 faceVP[6];
};

// --- Push constants ---
// Updated at two frequencies:
//   per-light:  offset 0, 16 bytes (lightWorldPos + farPlane)
//   per-draw:   offset 16, 64 bytes (model matrix)
layout(push_constant) uniform PushConstants
{
	layout(offset = 0)  vec3 lightWorldPos;     // bytes 0-11
	layout(offset = 12) float farPlane;          // bytes 12-15 (unused in VS)
	layout(offset = 16) mat4 model;              // bytes 16-79
};

void main()
{
	vec4 worldPos = model * vec4(inPosition, 1.0);
	fragWorldPos = worldPos.xyz;

	// Offset vertex to light-local space, then apply static face view-projection
	vec4 localPos = vec4(worldPos.xyz - lightWorldPos, 1.0);
	gl_Position = faceVP[gl_ViewIndex] * localPos;
}
