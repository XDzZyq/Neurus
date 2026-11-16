#version 450

// ---------------------------------------------------------------------------
// PBR Lighting Compute Shader
//
// Reads the G-Buffer (position, normal, albedo, metallic/roughness) and
// evaluates Cook-Torrance GGX BRDF for each point light in the SSBO.
// Writes HDR colour output to a storage image.
//
// Dispatch: ceil(width/16), ceil(height/16), 1
// ---------------------------------------------------------------------------

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

// --- G-Buffer inputs (combined image samplers) ---
layout(binding = 0) uniform sampler2D gPosition;
layout(binding = 1) uniform sampler2D gNormal;
layout(binding = 2) uniform sampler2D gAlbedo;
layout(binding = 3) uniform sampler2D gMetallicRoughness;

// --- HDR colour output ---
layout(rgba16f, binding = 4) uniform writeonly image2D outputImage;

// --- SSAO input (combined image sampler) ---
layout(binding = 7) uniform sampler2D U_AO;

// --- IBL cubemap samplers (optional – valid even when IBL disabled) ---
layout(binding = 8) uniform samplerCube U_Irradiance;
layout(binding = 9) uniform samplerCube U_Prefiltered;

// --- Shadow intensity (R8: 1.0 = fully shadowed, 0.0 = fully lit) ---
layout(binding = 10) uniform sampler2DArray U_ShadowArray;

// --- Point light SSBO (std140) ---
struct PointLight
{
	vec3 color;     // offset 0,  size 12
	float power;    // offset 12 (std140 vec3 → 16-byte aligned stride)

	vec3 pos;       // offset 16, size 12
	float radius;   // offset 28, size 4

	int shadowMapIndex; // offset 32, size 4 — index into shadow cubemap array; -1 == no shadow
	float _pad0;
	float _pad1;
	float _pad2;		// Do not use float[3] for padding; std140 requires 16-byte alignment for vec3, so we use 4 floats instead.
}; // struct size 48 (std140, 16-byte aligned)

// --- Sun (directional) light struct (std140) ---
// Layout matches C++ SunLightGpu: 48 bytes, 16-byte aligned.
// direction: world-space direction FROM the light (in shader: L = normalize(-direction))
// shadowMapIndex: layer index in U_ShadowArray (sampler2DArray), -1 = no shadow cast
struct SunLight
{
	vec3 direction;      // offset 0,  size 12 (std140 vec3 → 16-byte aligned stride)
	float power;         // offset 12, size 4

	vec3 color;          // offset 16, size 12 (std140 vec3 → 16-byte aligned stride)
	int shadowMapIndex;  // offset 28, size 4 — index into shadow map array; -1 == no shadow

	float _pad0;         // offset 32, size 4 — explicit padding to align struct
	float _pad1;         // offset 36, size 4 — explicit padding to align struct
}; // struct size 48 (std140, 16-byte aligned — implicit 8-byte pad at offsets 40-47)

// --- Spot light struct (std140) ---
// Layout matches C++ SpotLightStruct: 64 bytes, 16-byte aligned.
// dir: world-space direction FROM the spotlight (the axis the cone faces along).
// innerCutoff / outerCutoff: cosines of the inner/outer half-angles
// (innerCutoff > outerCutoff; smoothstep(outer, inner, cos_theta) gives the
//  angular attenuation from 0 at the outer edge to 1 inside the inner cone).
struct SpotLight
{
	vec3 pos;            // offset 0,  size 12
	float power;         // offset 12, size 4

	vec3 dir;            // offset 16, size 12
	float radius;        // offset 28, size 4

	vec3 color;          // offset 32, size 12
	float innerCutoff;   // offset 44, size 4 (cos of inner half-angle)

	float outerCutoff;   // offset 48, size 4 (cos of outer half-angle)
	int shadowMapIndex;  // offset 52, size 4 — index into shadow cubemap array; -1 == no shadow
	float _pad0;         // offset 56, size 4
	float _pad1;         // offset 60, size 4
}; // struct size 64 (std140, 16-byte aligned)

layout(std140, binding = 6) buffer SunLightBuffer
{
	SunLight sunLights[];
};

layout(std140, binding = 5) buffer LightBuffer
{
	PointLight lights[];
};

layout(std140, binding = 11) buffer SpotLightBuffer
{
	SpotLight spotLights[];
};

// --- Push constants ---
layout(push_constant) uniform PushConstants
{
	int lightCount;       // offset 0
	int sunLightCount;    // offset 4
	int spotLightCount;   // offset 8 – repurposed from former _pad0 slot
	float _pad0;          // offset 12 – padding before vec4
	vec4 cameraPos;       // offset 16 (vec4 = 16-byte aligned)
	mat4 view;            // offset 32 (mat4 = 64 bytes)
	int iblEnabled;       // offset 96 (0 = IBL disabled, 1 = enabled)
	int transEnabled;     // offset 100 (0 = opaque, 1 = transparent background)
	                      // implicit padding to align mat4 at 16 bytes (offset 112)
	mat4 invProjView;     // offset 112 – inverse(proj * view), for skybox ray
};
// Total: 176 bytes (unchanged — spotLightCount reuses existing padding slot)

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

const float PI = 3.14159265359;

// ---------------------------------------------------------------------------
// Cook-Torrance GGX BRDF functions
// ---------------------------------------------------------------------------

/**
 * @brief Trowbridge-Reitz (GGX) normal distribution function.
 * @param NdotH Cosine of angle between normal and half-vector.
 * @param a    Roughness squared (alpha = roughness^2).
 * @return NDF value.
 */
float DistributionGGX(float NdotH, float a)
{
	float a2     = a * a;
	float NdotH2 = NdotH * NdotH;

	float denom = NdotH2 * (a2 - 1.0) + 1.0;
	denom = PI * denom * denom;

	return a2 / max(denom, 1e-6);
}

/**
 * @brief Schlick-GGX geometry sub-function.
 * @param NdotX Cosine of angle between normal and direction (V or L).
 * @param k    Roughness remapping: k = (roughness + 1)^2 / 8 (direct lighting).
 * @return G1 value.
 */
float GeometrySchlickGGX(float NdotX, float k)
{
	return NdotX / (NdotX * (1.0 - k) + k);
}

/**
 * @brief Smith geometry function (height-correlated).
 * @param NdotV Cosine between normal and view direction.
 * @param NdotL Cosine between normal and light direction.
 * @param k    Roughness remapping parameter.
 * @return Combined geometry attenuation value.
 */
float GeometrySmith(float NdotV, float NdotL, float k)
{
	return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

/**
 * @brief Fresnel-Schlick approximation.
 * @param cosTheta Cosine of angle between half-vector and view direction (HdotV).
 * @param F0       Surface reflectance at normal incidence (0°).
 * @return Fresnel reflectance.
 */
vec3 FresnelSchlick(float cosTheta, vec3 F0)
{
	return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ---------------------------------------------------------------------------
// Main compute entry point
// ---------------------------------------------------------------------------

void main()
{
	ivec2 pixelCoord = ivec2(gl_GlobalInvocationID.xy);
	ivec2 imageSize2D = imageSize(outputImage);

	// Discard out-of-bounds threads
	if (pixelCoord.x >= imageSize2D.x || pixelCoord.y >= imageSize2D.y)
	{
		return;
	}

	// --- Sample G-Buffer ---
	vec2 uv = (vec2(pixelCoord) + 0.5) / vec2(imageSize2D);

	vec4  posSample  = textureLod(gPosition, uv, 0.0);
	vec3  worldPos   = posSample.xyz;
	vec4  nrmSample  = textureLod(gNormal, uv, 0.0);
	vec3  normalVS   = nrmSample.xyz;
	vec3  albedo     = textureLod(gAlbedo, uv, 0.0).rgb;
	vec4  mrSample   = textureLod(gMetallicRoughness, uv, 0.0);
	float metallic   = clamp(mrSample.r, 0.001, 0.999);
	float roughness  = clamp(mrSample.g, 0.001, 0.999);

	// Early out: clear-value pixels (no geometry rendered) → skybox or transparent
	if (posSample.w < 0.5)
	{
		// Compute ray direction from camera through this pixel
		vec2 ndc = uv * 2.0 - 1.0;
		vec4 clipPos = vec4(ndc, 1.0, 1.0); // far-plane in clip space
		vec4 worldPos = invProjView * clipPos;
		vec3 rayDir = normalize(worldPos.xyz / worldPos.w - cameraPos.xyz);

		vec3 skyColor = textureLod(U_Prefiltered, rayDir, 0.0).rgb;
		// When transparency is enabled, write alpha=0 so the compose pass
		// can render the checkerboard background (even if IBL skybox is present).
		float alpha = (transEnabled == 0) ? 1.0 : 0.0;
		imageStore(outputImage, pixelCoord, vec4(skyColor, alpha));
		return;
	}

	// --- Transform normal from view-space to world-space ---
	// normalWS  = transpose(inverse(view)) * normalVS
	// For an orthogonal rotation part (typical lookAt camera):
	//   inv(view_rot) = transpose(view_rot)
	// Therefore: normalWS = mat3(view) * normalVS  (after normalization)
	// Note: mat3(view) extracts the upper-left 3x3 rotation part.
	mat3 invViewRot = transpose(mat3(view));
	vec3 normalWS = normalize(invViewRot * normalVS);

	// --- View direction ---
	vec3 V = normalize(cameraPos.xyz - worldPos);
	float NdotV = max(dot(normalWS, V), 0.001);

	// --- Base reflectance ---
	// Dielectric F0 = ~0.04, metallic uses albedo as F0
	vec3 F0 = mix(vec3(0.04), albedo, metallic);

	// --- Roughness remapping for direct lighting ---
	float roughness2 = roughness * roughness;
	float k = (roughness + 1.0) * (roughness + 1.0) / 8.0;

	// --- Accumulate lighting ---
	vec3 Lo = vec3(0.0);

	for (int i = 0; i < lightCount; ++i)
	{
		PointLight light = lights[i];

		// Per-light shadow: sample shadow array only if this light casts shadows
		float shadow = 0.0;
		if (light.shadowMapIndex >= 0)
		{
			shadow = texture(U_ShadowArray, vec3(uv, float(light.shadowMapIndex))).r;
		}

		vec3 toLight  = light.pos - worldPos;
		float dist    = length(toLight);
		vec3 L        = toLight / dist;

		float NdotL = max(dot(normalWS, L), 0.0);
		if (NdotL <= 0.0)
		{
			continue;
		}

		// --- Attenuation (inverse-square falloff) ---
		float attenuation = 1.0 / (dist * dist);

		// --- Incident radiance (attenuated by shadow) ---
		vec3 radiance = light.power * light.color * attenuation * NdotL * (1.0 - shadow);

		// --- Cook-Torrance BRDF ---
		vec3 H = normalize(L + V);
		float NdotH = max(dot(normalWS, H), 0.0);
		float HdotV = max(dot(H, V), 0.0);

		// D: Normal Distribution Function (Trowbridge-Reitz / GGX)
		float D = DistributionGGX(NdotH, roughness2);

		// G: Geometry function (Smith with Schlick-GGX)
		float G = GeometrySmith(NdotV, NdotL, k);

		// F: Fresnel (Schlick)
		vec3 F = FresnelSchlick(HdotV, F0);

		// --- Specular ---
		vec3 numerator    = D * G * F;
		float denominator = 4.0 * NdotV * NdotL + 0.0001;
		vec3 specular     = numerator / denominator;

		// --- Diffuse ---
		vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

		// --- Accumulate ---
		Lo += (kD * albedo / PI + specular) * radiance;
	}

	// --- Sun (directional) light loop ---
	// Directional lights have uniform attenuation (no inverse-square falloff).
	// Direction is stored as world-space direction FROM the light;
	// compute L = normalize(-direction) to get the direction TO the light for BRDF.
	for (int j = 0; j < sunLightCount; ++j)
	{
		SunLight sun = sunLights[j];

		vec3 L = normalize(-sun.direction);
		float NdotL = max(dot(normalWS, L), 0.0);
		if (NdotL <= 0.0)
		{
			continue;
		}

		// --- Half-vector for specular ---
		vec3 H = normalize(L + V);
		float NdotH = max(dot(normalWS, H), 0.0);
		float HdotV = max(dot(H, V), 0.0);

		// --- Cook-Torrance BRDF (same as point lights) ---
		float D = DistributionGGX(NdotH, roughness2);
		float G = GeometrySmith(NdotV, NdotL, k);
		vec3 F = FresnelSchlick(HdotV, F0);

		vec3 numerator    = D * G * F;
		float denominator = 4.0 * NdotV * NdotL + 0.0001;
		vec3 specular     = numerator / denominator;

		vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

		// --- Radiance: uniform attenuation (no distance falloff) ---
		vec3 radiance = sun.power * sun.color * NdotL;

		// --- Shadow evaluation from 2D array (same array as point lights) ---
		float shadow = 0.0;
		if (sun.shadowMapIndex >= 0)
		{
			shadow = texture(U_ShadowArray, vec3(uv, float(sun.shadowMapIndex))).r;
		}
		radiance *= (1.0 - shadow);

		// --- Accumulate ---
		Lo += (kD * albedo / PI + specular) * radiance;
	}

	// --- Spot light loop ---
	// Spot lights combine point-light distance attenuation with angular falloff:
	//   spotFactor = smoothstep(outerCutoff, innerCutoff, dot(spot.dir, -L))
	// where L = direction FROM surface TO light, so (-L) points from light to
	// surface; taking the dot with the spotlight's facing direction gives the
	// cosine of the angle between the cone axis and the surface point.
	// innerCutoff > outerCutoff (both are cosines), so the smoothstep produces
	// 1.0 inside the inner cone and 0.0 outside the outer cone.
	for (int s = 0; s < spotLightCount; ++s)
	{
		SpotLight spot = spotLights[s];

		vec3 toLight = spot.pos - worldPos;
		float dist   = length(toLight);
		vec3 L       = toLight / max(dist, 1e-6);

		// Angular attenuation (cone falloff)
		float cosTheta   = dot(normalize(spot.dir), -L);
		float spotFactor = smoothstep(spot.outerCutoff, spot.innerCutoff, cosTheta);
		if (spotFactor <= 0.0)
		{
			continue;
		}

		float NdotL = max(dot(normalWS, L), 0.0);
		if (NdotL <= 0.0)
		{
			continue;
		}

		// Per-light shadow (cubemap layer)
		float shadow = 0.0;
		if (spot.shadowMapIndex >= 0)
		{
			shadow = texture(U_ShadowArray, vec3(uv, float(spot.shadowMapIndex))).r;
		}

		// Inverse-square distance attenuation (matches PointLight)
		float attenuation = 1.0 / (dist * dist);

		vec3 radiance = spot.power * spot.color * attenuation * spotFactor
		              * NdotL * (1.0 - shadow);

		// Cook-Torrance BRDF (same terms as point lights)
		vec3 H = normalize(L + V);
		float NdotH = max(dot(normalWS, H), 0.0);
		float HdotV = max(dot(H, V), 0.0);

		float D = DistributionGGX(NdotH, roughness2);
		float G = GeometrySmith(NdotV, NdotL, k);
		vec3 F  = FresnelSchlick(HdotV, F0);

		vec3 numerator    = D * G * F;
		float denominator = 4.0 * NdotV * NdotL + 0.0001;
		vec3 specular     = numerator / denominator;

		vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);

		Lo += (kD * albedo / PI + specular) * radiance;
	}

	// --- IBL (Image-Based Lighting) ---
	vec3 ibl = vec3(0.0);
	if (iblEnabled != 0)
	{
		// Diffuse IBL: sample irradiance cubemap with surface normal
		float kD_ibl = (1.0 - metallic);
		vec3 diffuseIBL = texture(U_Irradiance, normalWS).rgb * albedo * kD_ibl;

		// Specular IBL: sample prefiltered cubemap with reflection vector
		vec3 R = reflect(-V, normalWS);
		// Roughness 0..1 maps to mip 0..(numMips-1), where numMips = 8
		float specularMip = roughness * 7.0;
		vec3 prefilteredColor = textureLod(U_Prefiltered, R, specularMip).rgb;

		// Simplified Fresnel (no LUT)
		vec3 F_ibl = F0 + (1.0 - F0) * pow(clamp(1.0 - NdotV, 0.0, 1.0), 5.0);

		ibl = diffuseIBL + prefilteredColor * F_ibl;
	}

	// --- Write output ---
	float ao = texture(U_AO, uv).r;       // 1.0 = no occlusion, 0.0 = fully occluded
	vec3 ambient = vec3(0.03) * albedo * ao;
	vec3 colour = ambient + ibl + Lo;

	imageStore(outputImage, pixelCoord, vec4(colour, 1.0));
}
