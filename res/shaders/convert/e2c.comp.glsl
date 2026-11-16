#version 450
// ---------------------------------------------------------------------------
// Equirectangular → Cubemap Conversion (compute)
//
// Converts a 2D equirectangular HDR panorama into a cubemap (2D array with
// 6 layers, one per face).  Uses the standard spherical-to-direction mapping:
//
//   longitude = (uv.x - 0.5) * 2 * PI
//   latitude  = (uv.y - 0.5) * PI
//   dir = (cos(lat)*cos(lon), sin(lat), -cos(lat)*sin(lon))
//
// Face directions (Vulkan cubemap convention, +X right-handed):
//   +X(0) → ( 1, 0, 0), up=(0,-1,0)   -X(1) → (-1, 0, 0), up=(0,-1,0)
//   +Y(2) → ( 0, 1, 0), up=(0, 0,1)   -Y(3) → ( 0,-1, 0), up=(0, 0,-1)
//   +Z(4) → ( 0, 0, 1), up=(0,-1,0)   -Z(5) → ( 0, 0,-1), up=(0,-1,0)
//
// Dispatch: face_res / 4, face_res / 4, 6
// ---------------------------------------------------------------------------

layout(local_size_x = 4, local_size_y = 4, local_size_z = 1) in;

layout(binding = 0) uniform sampler2D u_Equirect;
layout(rgba32f, binding = 1) uniform writeonly image2DArray u_CubeOut;

const float PI  = 3.14159265359;
const float INV_2PI = 1.0 / (2.0 * PI);
const float INV_PI  = 1.0 / PI;

// ---------------------------------------------------------------------------
// Convert direction vector to equirectangular UV [0,1] (Z-up: +Z=up, +Y=forward)
// Note: Z negated because the view matrix from glm::lookAt produces
// view-space +Y = world +Z, but the camera forward maps to -Z in view space.
// ---------------------------------------------------------------------------
vec2 genHdrUV(vec3 dir)
{
	float longitude = atan(dir.x, dir.y);
	float latitude  = asin(-dir.z);
	return vec2(longitude * INV_2PI, latitude * INV_PI) + 0.5;
}

// ---------------------------------------------------------------------------
// Face direction matrices  (forward, right, up)
// All point the forward axis toward the cube face centre.
// ---------------------------------------------------------------------------
const mat3 faceDir[6] = {
	// +X → forward = ( 1, 0, 0), right = (0, 0,-1), up = (0,-1, 0)
	mat3(1,0,0,  0,0,-1,  0,-1,0),
	// -X → forward = (-1, 0, 0), right = (0, 0, 1), up = (0,-1, 0)
	mat3(-1,0,0,  0,0,1,  0,-1,0),
	// +Y → forward = ( 0, 1, 0), right = (1, 0, 0), up = (0, 0, 1)
	mat3(0,1,0,  1,0,0,  0,0,1),
	// -Y → forward = ( 0,-1, 0), right = (1, 0, 0), up = (0, 0,-1)
	mat3(0,-1,0,  1,0,0,  0,0,-1),
	// +Z → forward = ( 0, 0, 1), right = (1, 0, 0), up = (0,-1, 0)
	mat3(0,0,1,  1,0,0,  0,-1,0),
	// -Z → forward = ( 0, 0,-1), right = (-1,0, 0), up = (0,-1, 0)
	mat3(0,0,-1,  -1,0,0,  0,-1,0)
};

// ---------------------------------------------------------------------------
void main()
{
	ivec3 gid = ivec3(gl_GlobalInvocationID.xy, gl_WorkGroupID.z);
	ivec2 faceSize = imageSize(u_CubeOut).xy;
	int face = int(gl_WorkGroupID.z);

	if (gid.x >= faceSize.x || gid.y >= faceSize.y)
		return;

	// UV from pixel centre
	vec2 uv = (vec2(gid.xy) + 0.5) / vec2(faceSize);
	vec2 ndc = uv * 2.0 - 1.0;  // [-1, 1]

	// Build ray direction for this face
	vec3 dir = normalize(faceDir[face][0] + ndc.x * faceDir[face][1] + ndc.y * faceDir[face][2]);

	// Sample equirectangular map
	vec4 color = texture(u_Equirect, genHdrUV(dir));

	imageStore(u_CubeOut, gid, color);
}
