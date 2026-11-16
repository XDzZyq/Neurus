#version 450

// --- Inter-stage inputs ---
layout(location = 0) in vec3 fragWorldPos;
layout(location = 1) in vec3 fragNormalVS;
layout(location = 2) in vec2 fragUV;
layout(location = 3) flat in uint fragObjectID;

// --- MRT outputs (5-colour G-Buffer) ---
layout(location = 0) out vec4 gPosition;
layout(location = 1) out vec4 gNormal;
layout(location = 2) out vec4 gAlbedo;
layout(location = 3) out vec4 gMetallicRoughness;
layout(location = 4) out uint gObjectID;

void main()
{
	// World-space position (w = 1 ensures correct depth reconstruction)
	gPosition = vec4(fragWorldPos, 1.0);

	// View-space normal (already normalized in vertex shader)
	gNormal = vec4(normalize(fragNormalVS), 0.0);

	// Default PBR base colour (white)
	gAlbedo = vec4(1.0, 1.0, 1.0, 1.0);

	// Default metallic = 0.0 (dielectric), roughness = 0.5 (medium)
	gMetallicRoughness = vec4(0.0, 0.5, 0.0, 1.0);

	// Object ID (rasterized from vertex shader push constant)
	gObjectID = fragObjectID;
}
