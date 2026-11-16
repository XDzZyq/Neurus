#version 450

// --- Vertex inputs (matches MeshData layout: pos(3), normal(3), uv(2)) ---
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

// --- Inter-stage outputs ---
layout(location = 0) out vec3 fragWorldPos;
layout(location = 1) out vec3 fragNormalVS;
layout(location = 2) out vec2 fragUV;
layout(location = 3) flat out uint fragObjectID;

// --- Camera UBO (set 0, binding 0) ---
layout(set = 0, binding = 0) uniform CameraUBO
{
	mat4 viewProj;
	mat4 view;
} camera;

// --- Per-draw push constants ---
layout(push_constant) uniform PushConstants
{
	mat4 model;
	mat4 normalMatrix;  // mat3 stored as mat4 for 16-byte alignment
	uint objectID;
} pc;

void main()
{
	vec4 worldPos = pc.model * vec4(inPosition, 1.0);
	gl_Position = camera.viewProj * worldPos;

	fragWorldPos = worldPos.xyz;

	// Transform normal to view space via:
	//   normalVS = view * normalMatrix * inNormal
	// where normalMatrix = transpose(inverse(mat3(model)))
	fragNormalVS = normalize(mat3(camera.view) * mat3(pc.normalMatrix) * inNormal);

	fragUV = inUV;

	// Forward objectID to fragment shader for ID buffer rasterization
	fragObjectID = pc.objectID;
}
