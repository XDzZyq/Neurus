/**
 * @file SSAOPass.h
 * @brief Screen-Space Ambient Occlusion compute pass.
 *
 * Reads the G-Buffer (world position, view-space normal, alpha mask) and
 * computes per-pixel ambient occlusion via hemisphere sampling. The result is
 * written to the SSAO attachment (R8_UNORM).
 *
 * @note Hemisphere sampling with 16 kernel samples, world-space depth
 *       comparison, and a lightweight 2-pixel neighbour blur.
 */

#pragma once

#include "passes/ComputePass.h"
#include "../buffers/GPUBuffer.h"
#include "../shaders/ShaderLibrary.h"
#include "../shaders/ComputeShader.h"
#include "../buffers/UniformBuffer.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <array>
#include <memory>
#include <vector>

namespace neurus {

// --- Forward declarations ---
class RenderCache;
class SharedQueue;

// ---------------------------------------------------------------------------
// GPU-side data structures (std140-compatible)
// ---------------------------------------------------------------------------

/**
 * @brief Single kernel sample (tangent-space direction, std140 as vec4).
 *
 * The kernel is generated on CPU as hemisphere samples biased toward
 * the normal (z in [0, 1]).  Each entry is padded to 16 bytes for UBO.
 */
struct alignas(16) KernelSampleGpu
{
	float x, y, z;   ///< Tangent-space direction (unit hemisphere)
	float _pad;       ///< std140 padding to 16 bytes
};
static_assert(sizeof(KernelSampleGpu) == 16, "KernelSampleGpu must be 16 bytes (std140)");

/**
 * @brief SSAO parameters UBO - camera matrices + kernel (updated per frame).
 *
 * Layout (std140):
 *   mat4  viewProj       offset 0   (64 bytes)
 *   mat4  view           offset 64  (64 bytes)
 *   vec4  cameraPos      offset 128 (16 bytes, w unused)
 *   KernelSampleGpu[16]  offset 144 (256 bytes)
 *   Total: 400 bytes.
 */
struct alignas(16) SSAOParamsGpu
{
	float  viewProj[16];                        ///< projection * view (column-major)
	float  view[16];                            ///< view matrix (column-major)
	float  camX, camY, camZ, camW;              ///< camera world-space position + pad
	KernelSampleGpu kernelSamples[16];          ///< Hemisphere kernel samples
};
static_assert(sizeof(SSAOParamsGpu) == 400, "SSAOParamsGpu must be 400 bytes (std140)");

/**
 * @brief Noise rotation entry (16 bytes, std140).
 *
 * The noise buffer contains 16x16 = 256 random rotation vectors that
 * randomise the tangent-plane orientation per pixel, hiding banding
 * artefacts from the regular hemisphere sampling pattern.
 */
struct alignas(16) NoiseEntryGpu
{
	float x, y, z;   ///< Random rotation direction
	float _pad;       ///< std140 padding to 16 bytes
};
static_assert(sizeof(NoiseEntryGpu) == 16, "NoiseEntryGpu must be 16 bytes (std140)");

// ---------------------------------------------------------------------------
// SSAOPass
// ---------------------------------------------------------------------------

/**
 * @brief Screen-space ambient occlusion compute pass.
 *
 * Reads the G-Buffer (Position, Normal, Albedo) as combined image samplers,
 * performs hemisphere sampling in world-space, applies a lightweight
 * neighbour blur, and writes the occlusion factor (R8) to the SSAO attachment.
 */
class SSAOPass : public ComputePass
{
public:
	/** Default kernel size and noise dimensions. */
	static constexpr uint32_t kDefaultKernelLength = 16;
	static constexpr uint32_t kNoiseSize           = 16;
	static constexpr uint32_t kNoiseEntryCount     = kNoiseSize * kNoiseSize;  // 256
	static constexpr uint32_t kMaxKernelSamples    = 16;

	/**
	 * @brief Constructs the SSAO pass and creates all GPU resources.
	 *
	 * @param device            Logical device (retained reference).
	 * @param physicalDevice    Physical device (for sampler creation).
	 * @param numSets           Number of descriptor sets (one per in-flight frame).
	 * @param graphicsQueue     Graphics queue for the noise UBO staging upload;
	 *                          the upload waits its own submit.
	 * @param queueFamilyIndex  Queue family index for staging command pool.
	 *
	 * @throws std::runtime_error if shader or pipeline creation fails.
	 */
	SSAOPass(const vk::raii::Device& device,
	         const vk::raii::PhysicalDevice& physicalDevice,
	         uint32_t numSets,
	         SharedQueue& graphicsQueue,
	         uint32_t queueFamilyIndex);

	// -------------------------------------------------------------------
	// Recording
	// -------------------------------------------------------------------

	/**
	 * @brief Records the SSAO compute dispatch into a command buffer.
	 *
	 * Uploads the per-frame camera + kernel UBO, writes this frame's
	 * descriptors, transitions the G-Buffer to ColorShaderRead and SSAO to
	 * ShaderWrite, dispatches ceil(w/16) x ceil(h/16) groups, then returns SSAO
	 * to ColorShaderRead.
	 *
	 * @param cmdBuf  Command buffer in recording state.
	 * @param cache   Render cache for attachment access.
	 * @param ctx     Per-frame context (render extent, frame index).
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/// Declares the G-Buffer reads (Position/Normal/Albedo) and the SSAO write.
	PassIO GetIO() const override;

	/**
	 * @brief Writes all descriptors (image + buffer) into the specified set.
	 * @param setIndex  Index into p_descriptorSets (0 .. numSets-1).
	 */
	void WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache) override;

private:
	/**
	 * @brief Creates the descriptor set layout (6 bindings).
	 *
	 * 0-2: G-Buffer Position/Normal/Albedo (combined image samplers),
	 * 3: outputSSAO (storage image, R8), 4: SSAOParams UBO, 5: NoiseBuffer UBO.
	 */
	static DescriptorSetLayout CreateDescriptorSetLayout(const vk::raii::Device& device);

	/** @brief Creates the compute pipeline from the self-loaded shader. */
	void BuildPipeline(const vk::raii::Device& device,
	                   const std::string& debugName) override;

	/**
	 * @brief Generates the hemisphere kernel samples.
	 *
	 * 16 tangent-space directions biased toward the normal (z > 0), distributed
	 * with increasing radius so near samples cluster close to the centre.
	 *
	 * @return Array of 16 kernel samples in std140-compatible layout.
	 */
	static std::array<KernelSampleGpu, kMaxKernelSamples> GenerateKernel();

	/**
	 * @brief Generates 256 random rotation vectors for the noise buffer.
	 *
	 * Each entry is a random direction on the unit sphere, used to randomise
	 * the tangent-plane orientation per pixel.
	 *
	 * @return Array of 256 noise entries in std140-compatible layout.
	 */
	static std::array<NoiseEntryGpu, kNoiseEntryCount> GenerateNoise();

	// (Pipelines inherited from Pass - p_pipelines[0])

	// --- Self-loaded compute shader (via ShaderLibrary) ---
	std::unique_ptr<ComputeShader> p_shader;

	// --- Owned UBOs ---
	std::unique_ptr<UniformBuffer<SSAOParamsGpu>> p_paramsUBO;   ///< SSAO params (camera + kernel), host-visible
	std::unique_ptr<GPUBuffer> p_noiseUBO;    ///< Noise rotation vectors, device-local

	// --- Kernel samples (persistent across frames) ---
	std::array<KernelSampleGpu, kMaxKernelSamples> p_kernelSamples{};

	// --- Push constant data ---
	int32_t p_kernelLength = kDefaultKernelLength;
	float   p_radius       = 0.15f;
	int32_t p_noiseSize    = kNoiseSize;
};

} // namespace neurus
