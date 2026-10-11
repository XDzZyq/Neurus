/**
 * @file LightingCache.h
 * @brief GPU-side light SSBO storage and push constants.
 *
 * LightingCache manages point light, sun light, and spot light SSBOs
 * (storage buffers) and defines the std140-compatible GPU data structures
 * used by the PBR lighting compute shader.  Separated from LightingPass so
 * the SSBO lifecycle can be managed independently of the compute pass.
 *
 * The structs defined here (PointLightStruct, SunLightStruct,
 * SpotLightStruct, LightingPushConstants) are byte-for-byte replacements
 * for the original PointLightGpu / SunLightGpu types in LightingPass.h.
 * Renamed to avoid collision with the scene-layer LightGPU struct.
 */

#pragma once

#include "../buffers/ArrayBuffer.h"

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <vector>

namespace neurus {

// ---------------------------------------------------------------------------
// GPU-side data structures (std140-compatible)
// ---------------------------------------------------------------------------

/**
 * @brief Point light data uploaded to the GPU SSBO.
 *
 * std140-compatible layout matching the GLSL PointLight struct (48 bytes,
 * alignas(16)).
 */
struct alignas(16) PointLightStruct
{
	float colorR, colorG, colorB;   ///< RGB colour (linear)
	float power;                     ///< Luminous intensity
	float posX, posY, posZ;         ///< World-space position
	float radius;                    ///< Physical radius
	int32_t shadowMapIndex = -1;     ///< Index into shadow maps array; -1 = no shadow
	float _pad3[3];                 ///< Padding to 48 bytes (16-byte aligned struct)
};
static_assert(sizeof(PointLightStruct) == 48, "PointLightStruct must be 48 bytes (std140)");

/**
 * @brief Sun (directional) light data uploaded to the GPU SSBO.
 *
 * std140-compatible layout matching the GLSL SunLight struct (48 bytes,
 * alignas(16)).
 */
struct alignas(16) SunLightStruct
{
	float directionX, directionY, directionZ; ///< Light direction (world-space)
	float power;                               ///< Luminous intensity
	float colorR, colorG, colorB;              ///< RGB colour (linear)
	int32_t shadowMapIndex = -1;               ///< Index into shadow maps array; -1 = no shadow
	float _pad[4];                             ///< Padding to 48 bytes (16-byte aligned struct)
};
static_assert(sizeof(SunLightStruct) == 48, "SunLightStruct must be 48 bytes (std140)");

/**
 * @brief Spot light data uploaded to the GPU SSBO.
 *
 * Cone-shaped light with position, direction and inner/outer cone-angle
 * cosines. std140-compatible layout matching the GLSL SpotLight struct
 * (64 bytes, alignas(16)).
 */
struct alignas(16) SpotLightStruct
{
	float posX, posY, posZ;                   ///< World-space position
	float power;                               ///< Luminous intensity
	float dirX, dirY, dirZ;                    ///< Cone axis direction (world-space, normalized)
	float radius;                              ///< Physical radius for soft shadow jitter
	float colorR, colorG, colorB;              ///< RGB colour (linear)
	float innerCutoff;                         ///< cos(inner cone half-angle) -- full brightness inside
	float outerCutoff;                         ///< cos(outer cone half-angle) -- falloff to zero
	int32_t shadowMapIndex = -1;               ///< Index into shadow maps array; -1 = no shadow
	float _pad[2];                             ///< Padding to 64 bytes (16-byte aligned struct)
};
static_assert(sizeof(SpotLightStruct) == 64, "SpotLightStruct must be 64 bytes (std140)");

/**
 * @brief Push constants for the PBR lighting compute shader.
 *
 * std430 layout matching the GLSL push_constant block (176 bytes). Must NOT
 * use alignas.
 */
struct LightingPushConstants
{
	int32_t  lightCount;            ///< Number of active point lights in SSBO
	int32_t  sunLightCount;         ///< Number of active sun (directional) lights in SSBO
	int32_t  spotLightCount;        ///< Number of active spot lights in SSBO
	float    _pad0;                 ///< Padding to align cameraPos at offset 16
	float    camX, camY, camZ;      ///< Camera world-space position
	float    _pad1;                 ///< Padding (vec4 → 16 bytes)
	float    view[16];              ///< View matrix (for normal transform VS→WS)
	int32_t  iblEnabled;            ///< IBL enabled flag (0 = disabled, 1 = enabled)
	int32_t  transEnabled;          ///< Transparent background flag (0 = opaque, 1 = transparent)
	float    _pad2[2];              ///< Padding to align invProjView at offset 112 (16-byte alignment)
	float    invProjView[16];       ///< Inverse of (projection * view) matrix for skybox ray
};
static_assert(sizeof(LightingPushConstants) == 176, "LightingPushConstants must be 176 bytes");

// ---------------------------------------------------------------------------
// LightingCache
// ---------------------------------------------------------------------------

/**
 * @brief Manages light SSBO storage for the PBR lighting pipeline.
 *
 * Owns one ArrayBuffer per light kind. The full-rebuild updaters take
 * pre-sorted vectors with shadowMapIndex already stamped by the caller; the
 * single-element updaters overwrite one SSBO element without a full rebuild.
 *
 * Non-copyable.  All borrowed Vulkan handles must outlive this object.
 */
class LightingCache
{
public:
	/**
	 * @brief Constructs the light GPU storage manager.
	 *
	 * The underlying GPU buffers are allocated lazily on the first upload.
	 *
	 * @param device            Logical device (retained reference).
	 * @param physicalDevice    Physical device (for buffer memory queries).
	 * @param graphicsQueue     Graphics queue for staging uploads.
	 * @param queueFamilyIndex  Queue family index for staging command pool.
	 */
	LightingCache(const vk::raii::Device& device,
	            const vk::raii::PhysicalDevice& physicalDevice,
	            vk::Queue graphicsQueue,
	            uint32_t queueFamilyIndex);

	// Non-copyable — owns GPU resources (reference members prevent move-assign)
	LightingCache(const LightingCache&) = delete;
	LightingCache& operator=(const LightingCache&) = delete;

	// Movable
	LightingCache(LightingCache&&) noexcept = default;

	// --- Full SSBO rebuild ---

	/**
	 * @brief Creates or updates the point light SSBO from a pre-built vector.
	 *
	 * The vector must already be sorted, with shadowMapIndex populated by the
	 * caller. An empty vector releases the buffer.
	 *
	 * @param lights  Point light GPU structs (sorted, shadow indices set).
	 */
	void UpdatePointLights(const std::vector<PointLightStruct>& lights);

	/**
	 * @brief Creates or updates the sun light SSBO from a pre-built vector.
	 *
	 * Same semantics as UpdatePointLights().
	 *
	 * @param lights  Sun light GPU structs (sorted, shadow indices set).
	 */
	void UpdateSunLights(const std::vector<SunLightStruct>& lights);

	/**
	 * @brief Creates or updates the spot light SSBO from a pre-built vector.
	 *
	 * Same semantics as UpdatePointLights().
	 *
	 * @param lights  Spot light GPU structs (sorted, shadow indices set).
	 */
	void UpdateSpotLights(const std::vector<SpotLightStruct>& lights);

	// --- Per-light update (single element, no full rebuild) ---

	/**
	 * @brief Overwrites a single point light element at the given SSBO index.
	 *
	 * No full re-upload; the caller supplies the index and a struct with
	 * shadowMapIndex already stamped.
	 *
	 * @param light  Updated point light struct.
	 * @param index  Zero-based SSBO element index.
	 */
	void UpdatePointLight(const PointLightStruct& light, uint32_t index);

	/**
	 * @brief Overwrites a single sun light element at the given SSBO index.
	 *
	 * Same semantics as UpdatePointLight().
	 *
	 * @param light  Updated sun light struct.
	 * @param index  Zero-based SSBO element index.
	 */
	void UpdateSunLight(const SunLightStruct& light, uint32_t index);

	/**
	 * @brief Overwrites a single spot light element at the given SSBO index.
	 *
	 * Same semantics as UpdatePointLight().
	 *
	 * @param light  Updated spot light struct.
	 * @param index  Zero-based SSBO element index.
	 */
	void UpdateSpotLight(const SpotLightStruct& light, uint32_t index);

	// --- Accessors ---

	/** @brief Returns the point light SSBO, or nullptr when no lights exist. */
	const GPUBuffer* GetPointLightSSBO() const;

	/** @brief Returns the sun light SSBO, or nullptr when no sun lights exist. */
	const GPUBuffer* GetSunLightSSBO() const;

	/** @brief Returns the spot light SSBO, or nullptr when no spot lights exist. */
	const GPUBuffer* GetSpotLightSSBO() const;

	/** @brief Number of point lights in the SSBO. */
	uint32_t GetPointLightCount() const;

	/** @brief Number of sun lights in the SSBO. */
	uint32_t GetSunLightCount() const;

	/** @brief Number of spot lights in the SSBO. */
	uint32_t GetSpotLightCount() const;

private:
	// --- Owned GPU buffers ---
	ArrayBuffer<PointLightStruct> m_pointLightSSBO;
	ArrayBuffer<SunLightStruct> m_sunLightSSBO;
	ArrayBuffer<SpotLightStruct> m_spotLightSSBO;
};

} // namespace neurus
