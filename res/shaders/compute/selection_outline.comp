#version 450
// ---------------------------------------------------------------------------
// SelectionOutline Compute Shader — 3×3 edge detection on IDBuffer
//
// Reads the IDBuffer (R32_UINT) and compares each pixel against its 8
// neighbours. If the centre pixel matches activeObjectId AND any neighbour
// differs, the pixel is flagged as an edge (1.0 → R8_UNORM output).
//
// Only operates on pixels within the active object's region — all other
// pixels are written as 0.0 (no highlight).
//
// Dispatch: ceil(width/16), ceil(height/16), 1
// ---------------------------------------------------------------------------

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// --- IDBuffer input ---
layout(binding = 0) uniform usampler2D gIDBuffer;

// --- Selection outline output (R8) ---
layout(r8, binding = 1) uniform writeonly image2D outputHighlight;

// --- Push constants ---
layout(push_constant) uniform PushConstants
{
	uint activeObjectId;    // 0 = nothing selected (skip all)
};

void main()
{
	ivec2 pixelCoord = ivec2(gl_GlobalInvocationID.xy);
	ivec2 imgSize    = imageSize(outputHighlight);

	if (pixelCoord.x >= imgSize.x || pixelCoord.y >= imgSize.y)
		return;

	// Early-out: nothing selected
	if (activeObjectId == 0u)
	{
		imageStore(outputHighlight, pixelCoord, vec4(0.0));
		return;
	}

	// Read centre pixel
	uint centre = texelFetch(gIDBuffer, pixelCoord, 0).r;

	// Quick skip: this pixel doesn't belong to the active object
	if (centre != activeObjectId)
	{
		imageStore(outputHighlight, pixelCoord, vec4(0.0));
		return;
	}

	// 3×3 kernel — check if any neighbour differs from centre
	bool isEdge = false;
	for (int dy = -1; dy <= 1 && !isEdge; ++dy)
	{
		for (int dx = -1; dx <= 1 && !isEdge; ++dx)
		{
			if (dx == 0 && dy == 0) continue;
			ivec2 neighbourCoord = pixelCoord + ivec2(dx, dy);

			// Clamp to image bounds for border pixels
			neighbourCoord = clamp(neighbourCoord, ivec2(0), imgSize - 1);

			uint neighbour = texelFetch(gIDBuffer, neighbourCoord, 0).r;
			if (neighbour != centre)
				isEdge = true;
		}
	}

	float highlight = isEdge ? 1.0 : 0.0;
	imageStore(outputHighlight, pixelCoord, vec4(highlight));
}
