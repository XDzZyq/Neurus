#version 450
// ---------------------------------------------------------------------------
// Diffuse IBL Irradiance Convolution (compute)
//
// Convolves the equirectangular HDR environment map into a low-resolution
// diffuse irradiance cubemap.  Uses a diamond-pattern hemisphere sampling
// scheme where all samples that fall within the hemisphere contribute
// weighted by their impact angle (cos θ).
//
// Reference: LearnOpenGL IBL Diffuse / OpenGL E2C Irradiance
//
// Dispatch: face_res/4, face_res/4, 6   (e.g. 16, 16, 6 for 64² face)
// ---------------------------------------------------------------------------

layout(local_size_x = 4, local_size_y = 4, local_size_z = 1) in;

layout(binding = 0) uniform sampler2D u_Equirect;
layout(rgba32f, binding = 1) uniform writeonly image2DArray u_CubeOut;

layout(push_constant) uniform PushConstants
{
	int maxStep;    // max sample count per axis (e.g. 64)
	float _pad;
};
// Total: 8 bytes

const float PI         = 3.14159265359;
const float INV_2PI    = 1.0 / (2.0 * PI);
const float INV_PI     = 1.0 / PI;

// --- Equirect UV from direction (Z-up: +Z=up, +Y=forward) ---
// Note: Z negated because the view matrix from glm::lookAt produces
// view-space +Y = world +Z, but the camera forward maps to -Z in view space.
vec2 genHdrUV(vec3 dir)
{
	float longitude = atan(dir.x, dir.y);
	float latitude  = asin(-dir.z);
	return vec2(longitude * INV_2PI, latitude * INV_PI) + 0.5;
}

// --- Face direction matrices (same as E2C) ---
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

	// Ray direction for this cube face pixel
	vec3 forward = faceDir[face][0];
	vec3 right   = faceDir[face][1];
	vec3 up      = faceDir[face][2];
	vec3 dir = normalize(forward + ndc.x * right + ndc.y * up);

	vec3 irradiance = vec3(0.0);
	float totalWeight = 0.0;

	// --- Diamond-pattern hemisphere integral ---
	float stepSize = 1.0 / float(maxStep);

	for (int i = -maxStep; i <= maxStep; ++i)
	{
		float a = float(i) * stepSize;

		for (int j = -maxStep; j <= maxStep; ++j)
		{
			float b = float(j) * stepSize;
			float c = 1.0 - abs(a) - abs(b);

			if (c <= 0.0)
				continue;

			// Sample direction on hemisphere
			vec3 sampleDir = normalize(a * right + b * up + c * dir);

			// cosθ weighting (lambertian)
			float cosTheta = max(dot(sampleDir, dir), 0.0);

			irradiance += texture(u_Equirect, genHdrUV(sampleDir)).rgb * cosTheta;
			totalWeight += cosTheta;
		}
	}

	irradiance = (totalWeight > 0.0) ? irradiance / totalWeight : vec3(0.0);

	imageStore(u_CubeOut, gid, vec4(irradiance, 1.0));
}
