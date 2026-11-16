#version 450
// ---------------------------------------------------------------------------
// Cubemap → Equirectangular Conversion (compute)
//
// For each pixel in the output equirectangular panorama, computes the
// corresponding direction vector, samples the input cubemap, and writes
// the result.  Useful for saving cubemaps back to a single 2D .hdr file.
//
// Dispatch: ceil(eWidth/16), ceil(eHeight/16), 1
// ---------------------------------------------------------------------------

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 0) uniform samplerCube u_CubeInput;
layout(rgba32f, binding = 1) uniform writeonly image2D u_EquiOut;

const float PI  = 3.14159265359;

void main()
{
	ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
	ivec2 imgSize = imageSize(u_EquiOut);

	if (gid.x >= imgSize.x || gid.y >= imgSize.y)
		return;

	vec2 uv = (vec2(gid) + 0.5) / vec2(imgSize);

	// Convert UV to direction (Z-up: +Z=up, +Y=forward)
	// Note: Z negated to match genHdrUV's asin(-dir.z)
	float longitude = (uv.x - 0.5) * 2.0 * PI;
	float latitude  = (uv.y - 0.5) * PI;

	vec3 dir;
	dir.x = cos(latitude) * sin(longitude);
	dir.y = cos(latitude) * cos(longitude);
	dir.z = -sin(latitude);

	vec4 color = texture(u_CubeInput, dir);

	imageStore(u_EquiOut, gid, color);
}
