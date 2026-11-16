#version 450
// FXAA — Full-iteration: luma gradient → direction → iterate edge line → stop at endpoint → interpolate.
// Requires bilinear sampler on gInput for sub-pixel texture() resample.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(binding = 0) uniform sampler2D gInput;
layout(rgba16f, binding = 1) uniform writeonly image2D outputFXAA;

layout(push_constant) uniform PushConstants {
	vec4  rcpFrame;
	float qualitySubpix;
	float edgeThreshold;
	float edgeThresholdMin;
} pc;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define STATE_DEFAULT  0
#define STATE_END      1
#define STATE_FLIP     2
#define STATE_BOUND    3
#define MAX_ITER       32
#define GRAY_THRESHOLD 0.04
#define EPSILON        0.000000001

// ---------------------------------------------------------------------------
// Helpers (basic types only — ShaderParser requirement)
// ---------------------------------------------------------------------------

bool IsOut(int x, int y, int w, int h)
{
	return x < 0 || x >= w || y < 0 || y >= h;
}

float Luma(vec4 c)
{
	c = c / (1.0 + c);  // HDR → LDR tone mapping
	return dot(c.rgb, vec3(0.299, 0.587, 0.114));
}

struct MarchInfo
{
	int steps;
	int code;
};

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

void main()
{
	ivec2 pixel  = ivec2(gl_GlobalInvocationID.xy);
	ivec2 extent = imageSize(outputFXAA);
	if (IsOut(pixel.x, pixel.y, extent.x, extent.y))
		return;

	// ---- 1. 3×3 luma sampling ----
	float luma[9];
	for (int i = 0; i < 3; ++i)
	for (int j = 0; j < 3; ++j)
		luma[i * 3 + j] = Luma(texelFetch(gInput, pixel + ivec2(j - 1, i - 1), 0));

	float lumaCenter = luma[4];
	float lumaMin = luma[0], lumaMax = luma[0];
	for (int i = 1; i < 9; ++i)
	{
		lumaMin = min(lumaMin, luma[i]);
		lumaMax = max(lumaMax, luma[i]);
	}
	float range = lumaMax - lumaMin;

	// ---- 2. Gradient direction (Sobel) ----
	float offX = 0.0, offY = 0.0;

	if (range >= max(pc.edgeThresholdMin, lumaMax * pc.edgeThreshold))
	{
		float gradX = -luma[0] + luma[2] - 2.0 * luma[3] + 2.0 * luma[5] - luma[6] + luma[8];
		float gradY =  luma[0] + 2.0 * luma[1] + luma[2] - luma[6] - 2.0 * luma[7] - luma[8];
		bool  isHorizontal = abs(gradY) >= abs(gradX);

		float neighborLo = isHorizontal ? luma[1] : luma[3];
		float neighborHi = isHorizontal ? luma[7] : luma[5];

		float offset = (neighborLo - lumaCenter) / max(neighborLo - neighborHi, EPSILON) - 0.5;
		offset = clamp(offset, -0.5, 0.5);

		if (isHorizontal)
			offY = offset;
		else
			offX = offset;
	}

	// ---- Early exit: no edge detected ----
	if (offX == 0.0 && offY == 0.0)
	{
		imageStore(outputFXAA, pixel, texelFetch(gInput, pixel, 0));
		return;
	}

	// ---- 3. Iterate along edge line to find endpoints ----
	// March along the edge direction: horizontal → left/right, vertical → up/down.
	// At each step, check:
	//   - Center pixel luma → FLIP if different from starting luma (crossed edge)
	//   - Off-axis neighbor luma → END if same as starting luma (left the edge)

	bool  isHorizontal = offX == 0.0;
	float offsetCenter = isHorizontal ? offY : offX;
	int   lineSign     = int(offsetCenter > 0.0 ? -1.0 : 1.0);

	MarchInfo left  = MarchInfo(0, STATE_DEFAULT);
	MarchInfo right = MarchInfo(0, STATE_DEFAULT);

	// --- Negative direction ---
	for (left.steps = 1; left.steps < MAX_ITER; ++left.steps)
	{
		int cx = isHorizontal ? pixel.x - left.steps : pixel.x;
		int cy = isHorizontal ? pixel.y              : pixel.y - left.steps;
		if (IsOut(cx, cy, extent.x, extent.y)) { left.code = STATE_BOUND; break; }

		float grayN1 = Luma(texelFetch(gInput, ivec2(cx, cy), 0));
		if (abs(grayN1 - lumaCenter) > GRAY_THRESHOLD) { left.code = STATE_FLIP; break; }

		int cx2 = isHorizontal ? cx : cx + lineSign;
		int cy2 = isHorizontal ? cy + lineSign : cy;
		float grayN2 = IsOut(cx2, cy2, extent.x, extent.y)
			? 0.0 : Luma(texelFetch(gInput, ivec2(cx2, cy2), 0));
		if (abs(grayN2 - lumaCenter) < GRAY_THRESHOLD) { left.code = STATE_END; break; }
	}

	// --- Positive direction ---
	for (right.steps = 1; right.steps < MAX_ITER; ++right.steps)
	{
		int cx = isHorizontal ? pixel.x + right.steps : pixel.x;
		int cy = isHorizontal ? pixel.y               : pixel.y + right.steps;
		if (IsOut(cx, cy, extent.x, extent.y)) { right.code = STATE_BOUND; break; }

		float grayN1 = Luma(texelFetch(gInput, ivec2(cx, cy), 0));
		if (abs(grayN1 - lumaCenter) > GRAY_THRESHOLD) { right.code = STATE_FLIP; break; }

		int cx2 = isHorizontal ? cx : cx + lineSign;
		int cy2 = isHorizontal ? cy + lineSign : cy;
		float grayN2 = IsOut(cx2, cy2, extent.x, extent.y)
			? 0.0 : Luma(texelFetch(gInput, ivec2(cx2, cy2), 0));
		if (abs(grayN2 - lumaCenter) < GRAY_THRESHOLD) { right.code = STATE_END; break; }
	}

	// ---- 4. Compute endpoint offsets from stop codes ----
	float leftOff  = offsetCenter;
	float rightOff = offsetCenter;

	if      (left.code == STATE_END)  leftOff  = 0.0;
	else if (left.code == STATE_FLIP) leftOff  = offsetCenter > 0.0 ? 0.5 : -0.5;

	if      (right.code == STATE_END)  rightOff = 0.0;
	else if (right.code == STATE_FLIP) rightOff = offsetCenter > 0.0 ? 0.5 : -0.5;

	// ---- 5. Interpolate between left and right endpoints ----
	float t = float(left.steps) / float(left.steps + right.steps);
	offsetCenter = mix(leftOff, rightOff, t);

	// ---- 6. Resample at sub-pixel position ----
	vec2 texCoord = (vec2(pixel) + vec2(0.5)) * pc.rcpFrame.xy;
	if (isHorizontal)
		texCoord.y -= offsetCenter * pc.rcpFrame.y;
	else
		texCoord.x -= offsetCenter * pc.rcpFrame.x;

	vec3 result = texture(gInput, texCoord).rgb;
	imageStore(outputFXAA, pixel, vec4(result, 1.0));
}
