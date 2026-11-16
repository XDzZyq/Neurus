#version 450
// ---------------------------------------------------------------------------
// Specular IBL Prefiltered Environment Map (compute)
//
// Prefilters the equirectangular HDR environment map for GGX specular IBL.
// Each mip level corresponds to a different roughness value (0..1 mapped
// to mip 0..7).  The prefilter uses importance sampling with a GGX
// distribution (importance-weighted hemisphere samples).
//
// Mip 0 (roughness=0) is an unfiltered copy (specular reflection at s=0).
// Roughness² scales the sampling kernel:
//   a = s² * i / maxStep,  b = s² * j / maxStep
//   weight = c / length(vec3(a, b, c))
//
// Reference: LearnOpenGL IBL Specular / OpenGL Importance_Samp_E2C
//
// Dispatch: face_res/4, face_res/4, 6   (one dispatch per mip)
// ---------------------------------------------------------------------------

layout(local_size_x = 4, local_size_y = 4, local_size_z = 1) in;

layout(binding = 0) uniform sampler2D u_Equirect;
layout(rgba32f, binding = 1) uniform writeonly image2DArray u_CubeOut;

layout(push_constant) uniform PushConstants
{
	int mipLevel;       // target mip level (0..7)
	int maxStep;        // max sample count per axis (e.g. 32)
	float roughnessSq;  // roughness² (precomputed)
	float _pad;
};
// Total: 16 bytes

const float PI      = 3.14159265359;
const float INV_2PI = 1.0 / (2.0 * PI);
const float INV_PI  = 1.0 / PI;

// --- Equirect UV from direction (Z-up: +Z=up, +Y=forward) ---
// Note: Z negated because the view matrix from glm::lookAt produces
// view-space +Y = world +Z, but the camera forward maps to -Z in view space.
vec2 genHdrUV(vec3 dir)
{
	float longitude = atan(dir.x, dir.y);
	float latitude  = asin(-dir.z);
	return vec2(longitude * INV_2PI, latitude * INV_PI) + 0.5;
}

// --- Face direction matrices ---
const mat3 faceDir[6] = {
	mat3(1,0,0,  0,0,-1,  0,-1,0),
	mat3(-1,0,0,  0,0,1,  0,-1,0),
	mat3(0,1,0,  1,0,0,  0,0,1),
	mat3(0,-1,0,  1,0,0,  0,0,-1),
	mat3(0,0,1,  1,0,0,  0,-1,0),
	mat3(0,0,-1,  -1,0,0,  0,-1,0)
};

void main()
{
	ivec3 gid = ivec3(gl_GlobalInvocationID.xy, gl_WorkGroupID.z);
	ivec2 faceSize = imageSize(u_CubeOut).xy;
	int face = int(gl_WorkGroupID.z);

	if (gid.x >= faceSize.x || gid.y >= faceSize.y)
		return;

	vec2 uv = (vec2(gid.xy) + 0.5) / vec2(faceSize);
	vec2 ndc = uv * 2.0 - 1.0;

	vec3 forward = faceDir[face][0];
	vec3 right   = faceDir[face][1];
	vec3 up      = faceDir[face][2];
	vec3 dir = normalize(forward + ndc.x * right + ndc.y * up);

	// --- Mip 0: pure copy (no filtering) ---
	if (roughnessSq < 0.0001)
	{
		vec4 color = texture(u_Equirect, genHdrUV(dir));
		imageStore(u_CubeOut, gid, color);
		return;
	}

	// --- GGX importance sampling ---
	vec3 prefilteredColor = vec3(0.0);
	float totalWeight     = 0.0;

	float stepSize = 1.0 / float(maxStep);

	for (int i = -maxStep; i <= maxStep; ++i)
	{
		float a = roughnessSq * float(i) * stepSize;

		for (int j = -maxStep; j <= maxStep; ++j)
		{
			float b = roughnessSq * float(j) * stepSize;
			float c = 1.0 - abs(a) - abs(b);

			if (c <= 0.0)
				continue;

			vec3 sampleOffset = vec3(a, b, c);
			float sampleWeight = c / length(sampleOffset);

			vec3 sampleDir = normalize(a * right + b * up + c * dir);

			prefilteredColor += texture(u_Equirect, genHdrUV(sampleDir)).rgb * sampleWeight;
			totalWeight += sampleWeight;
		}
	}

	prefilteredColor = (totalWeight > 0.0) ? prefilteredColor / totalWeight : vec3(0.0);

	imageStore(u_CubeOut, gid, vec4(prefilteredColor, 1.0));
}
