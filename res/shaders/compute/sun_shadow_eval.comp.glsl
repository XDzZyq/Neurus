#version 450
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// --- G-Buffer input: world-space position ---
layout(binding = 0) uniform sampler2D gPosition;

// --- Sun shadow depth map (non-compare, 2D, .r = depth in [0,1]) ---
layout(binding = 1) uniform sampler2D u_SunShadowMap;

// --- Output / accumulation: R8 shadow intensity (2D_ARRAY), read+write ---
layout(r8, binding = 2) uniform image2DArray outputShadow;

// --- Push constants: 96 bytes GLSL ---
layout(push_constant) uniform PushConstants
{
    mat4  lightViewProj;    // offset 0,  64 bytes
    float bias;             // offset 64, 4 bytes
    int   layerIndex;       // offset 68, 4 bytes
    float jitterX;          // offset 72, 4 bytes
    float jitterY;          // offset 76, 4 bytes
    float jitterZ;          // offset 80, 4 bytes
    float lightRadius;      // offset 84, 4 bytes
    float alpha;            // offset 88, 4 bytes
    int   frameCount;       // offset 92, 4 bytes
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
    vec4 lightSpace   = lightViewProj * vec4(worldPos, 1.0);
    vec3 ndc          = lightSpace.xyz / lightSpace.w;

    // Map NDC.xy from [-1,1] to [0,1] UV
    vec2 shadowUV = ndc.xy * 0.5 + 0.5;

    // Jitter the shadow map UV sample (sub-pixel offset equivalent to
    // directional light direction jitter)
    vec2 jitteredUV = shadowUV + vec2(jitterX, jitterY) * lightRadius;

    float shadowDepth  = textureLod(u_SunShadowMap, jitteredUV, 0.0).r;

    // Fragments outside shadow frustum (UV or depth) are treated as lit
    const bool out_of_range = jitteredUV.x<0.0 || jitteredUV.x>1.0 || jitteredUV.y<0.0 || jitteredUV.y>1.0;
    const bool out_of_depth = ndc.z > 1.0 || ndc.z < 0.0;
    const bool in_shadow = !out_of_range && !out_of_depth && (ndc.z - bias > shadowDepth);
    float shadowSample = in_shadow ? 1.0 : 0.0;

    // Temporal accumulation: read previous value, EMA blend, write back
    float prevAccum = imageLoad(outputShadow, ivec3(pixelCoord, layerIndex)).r;
    float newAccum  = (frameCount == 0) ? shadowSample
                    : mix(prevAccum, shadowSample, alpha);

    imageStore(outputShadow, ivec3(pixelCoord, layerIndex), vec4(newAccum));
}
