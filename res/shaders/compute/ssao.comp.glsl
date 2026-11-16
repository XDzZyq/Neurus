#version 450
// ---------------------------------------------------------------------------
// SSAO Compute Shader — Screen-Space Ambient Occlusion
//
// Reads G-Buffer (world position, view-space normal, alpha mask) and
// computes occlusion via hemisphere sampling in world-space.
// Writes R8 occlusion factor to the SSAO attachment.
//
// Dispatch: ceil(width/16), ceil(height/16), 1
// ---------------------------------------------------------------------------

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// --- G-Buffer inputs (combined image samplers) ---
layout(binding = 0) uniform sampler2D gPosition;
layout(binding = 1) uniform sampler2D gNormal;
layout(binding = 2) uniform sampler2D gAlpha;

// --- SSAO output ---
layout(r8, binding = 3) uniform writeonly image2D outputSSAO;

// --- SSAO parameters UBO ---
//     Contains camera matrices + kernel samples (updated per frame)
struct KernelSample { vec4 v; };
layout(std140, binding = 4) uniform SSAOParams
{
	mat4 viewProj;                       // 64 bytes, offset 0
	mat4 view;                           // 64 bytes, offset 64
	vec4 cameraPos;                      // 16 bytes, offset 128 (w unused)
	KernelSample kernelSamples[16];      // 16 * 16 = 256 bytes, offset 144
};
// Total: 400 bytes

// --- Noise rotation vectors (static, uploaded once) ---
struct NoiseEntry { vec4 rotation; };
layout(std140, binding = 5) uniform NoiseBuffer
{
	NoiseEntry entries[256];             // 256 * 16 = 4096 bytes
};

// --- Push constants ---
layout(push_constant) uniform PushConstants
{
	int  kernelLength;   // offset 0  (number of active kernel samples, ≤ 16)
	float radius;        // offset 4  (SSAO sample radius)
	int  noiseSize;      // offset 8  (noise texture dimension, 16)
	int  frameIndex;     // offset 12 (frame counter for noise cycling)
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

void main()
{
	ivec2 pixelCoord = ivec2(gl_GlobalInvocationID.xy);
	ivec2 imgSize    = imageSize(outputSSAO);

	// Discard out-of-bounds threads
	if (pixelCoord.x >= imgSize.x || pixelCoord.y >= imgSize.y)
	{
		return;
	}

	vec2 uv = (vec2(pixelCoord) + 0.5) / vec2(imgSize);

	// --- Sample G-Buffer ---
	vec3 worldPos = textureLod(gPosition, uv, 0.0).rgb;
	vec3 normalVS = textureLod(gNormal,   uv, 0.0).rgb;
	float alpha   = textureLod(gAlpha,    uv, 0.0).a;

	// Early-out: background pixels (no geometry)
	if (alpha < 0.5)
	{
		imageStore(outputSSAO, pixelCoord, vec4(1.0));  // no occlusion
		return;
	}

	// --- Transform normal from view-space to world-space ---
	// view matrix is orthonormal for a lookAt camera,
	// so inverse(rotation) == transpose(rotation)
	mat3 viewRot = mat3(view);
	vec3 normalWS = normalize(transpose(viewRot) * normalVS);
	// Note: for a view matrix that includes translation, the upper-left 3x3
	// is the rotation part. transpose(inverse(viewRot)) == viewRot for orthonormal.

	// --- Random rotation vector from noise buffer ---
	ivec2 noiseCoord = pixelCoord % noiseSize;
	int noiseIdx = noiseCoord.y * noiseSize + noiseCoord.x;
	// Cycle through noise entries using frame index
	noiseIdx = (noiseIdx + frameIndex * 37) % (noiseSize * noiseSize);
	vec3 randVec = entries[noiseIdx].rotation.xyz;

	// --- Build TBN matrix ---
	// tangent: random direction perpendicular to normal
	vec3 tangent = normalize(cross(normalWS, randVec));
	// Guard against degenerate cross product (normal parallel to randVec)
	if (length(cross(normalWS, randVec)) < 0.001)
	{
		tangent = normalize(cross(normalWS, vec3(0.0, 0.0, 1.0)));
	}
	vec3 bitangent = cross(normalWS, tangent);
	mat3 TBN = mat3(tangent, bitangent, normalWS);

	// --- Hemisphere sampling ---
	float occlusion = 0.0;

	for (int i = 0; i < kernelLength; ++i)
	{
		// Transform tangent-space kernel sample to world-space
		vec3 offset = TBN * kernelSamples[i].v.xyz * radius;

		// World-space sample position
		vec3 samplePos = worldPos + offset;

		// Project to screen
		vec4 clipPos  = viewProj * vec4(samplePos, 1.0);
		vec3 ndc      = clipPos.xyz / clipPos.w;
		vec2 screenUV = ndc.xy * 0.5 + 0.5;

		// Depth comparison: distance from camera
		float sampleDepth   = distance(samplePos, cameraPos.xyz);
		float occluderDepth = distance(
			textureLod(gPosition, screenUV, 0.0).rgb, cameraPos.xyz);

		float delta = occluderDepth - sampleDepth;
		// delta <= 0 → occluder is in front of (or at) sample → potential occlusion
		// delta > 0  → occluder is behind sample → no occlusion
		float ao = delta <= 0.0
			? smoothstep(0.0, 1.0, abs(delta / radius))
			: 1.0;

		// Range check: discard samples outside screen
		if (screenUV.x < 0.0 || screenUV.x > 1.0 ||
		    screenUV.y < 0.0 || screenUV.y > 1.0)
		{
			ao = 1.0;  // no occlusion from off-screen samples
		}

		ao *= ao;  // sharpen
		occlusion += ao;
	}

	occlusion /= float(kernelLength);

	// Invert: 1 = no occlusion, 0 = fully occluded
	float aoFactor = 1.0 - occlusion;

	imageStore(outputSSAO, pixelCoord, vec4(aoFactor));
}
