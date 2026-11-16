#version 450
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// --- G-Buffer input: world-space position ---
layout(binding = 0) uniform sampler2D gPosition;

// --- Shadow depth cubemap (non-compare, .r = linear depth) ---
layout(binding = 1) uniform samplerCube u_ShadowCube;

// --- Output / accumulation: R8 shadow intensity (2D_ARRAY), read+write ---
layout(r8, binding = 2) uniform image2DArray outputShadow;

// --- Push constants: 48 bytes ---
layout(push_constant) uniform PushConstants
{
    vec3  lightWorldPos;    // offset 0,  12 bytes
    float farPlane;         // offset 12, 4 bytes
    float bias;             // offset 16, 4 bytes
    int   layerIndex;       // offset 20, 4 bytes
    float jitterX;          // offset 24, 4 bytes
    float jitterY;          // offset 28, 4 bytes
    float jitterZ;          // offset 32, 4 bytes
    float lightRadius;      // offset 36, 4 bytes
    float alpha;            // offset 40, 4 bytes
    int   frameCount;       // offset 44, 4 bytes
};

void main()
{
    ivec2 pixelCoord = ivec2(gl_GlobalInvocationID.xy);
    ivec2 imgSize    = imageSize(outputShadow).xy;

    if (pixelCoord.x >= imgSize.x || pixelCoord.y >= imgSize.y) return;

    vec2 uv = (vec2(pixelCoord) + 0.5) / vec2(imgSize);
    vec4 posSample = textureLod(gPosition, uv, 0.0);

    // Background pixel (no geometry) -- keep previous accumulated value
    if (posSample.w < 0.5) return;

    vec3 worldPos     = posSample.xyz;

    // Jitter light position in world space using 3D random direction scaled by light radius
    vec3 jitteredLightPos   = lightWorldPos + vec3(jitterX, jitterY, jitterZ) * lightRadius;
    vec3 toLight            = worldPos - jitteredLightPos;
    float dist              = length(toLight);

    // Cubemap stores linear depth = distance / farPlane
    float closestDepth = texture(u_ShadowCube, normalize(toLight)).r;

    float currentDepth  = dist / farPlane;
    float shadowSample  = (currentDepth - bias > closestDepth) ? 1.0 : 0.0;

    // Temporal accumulation: read previous value, EMA blend, write back
    float prevAccum = imageLoad(outputShadow, ivec3(pixelCoord, layerIndex)).r;
    float newAccum  = (frameCount == 0) ? shadowSample
                    : mix(prevAccum, shadowSample, alpha);

    imageStore(outputShadow, ivec3(pixelCoord, layerIndex), vec4(newAccum));
}
