/**
 * @file IBLPass.h
 * @brief Image-Based Lighting generation pass - pure compute service.
 *
 * Converts an equirectangular HDR image into diffuse irradiance and specular
 * prefiltered cubemaps for PBR IBL evaluation. Does not own cubemap resources:
 * the caller provides the output Images. Generation is one-shot, not per-frame.
 */

#pragma once

#include "../DescriptorManager.h"
#include "../shaders/ShaderLibrary.h"
#include "../shaders/ComputeShader.h"
#include "ComputePass.h"

#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <vector>

namespace neurus {

// --- Forward declarations ---
class Image;
class RenderCache;

/**
 * @brief IBL generation pass - equirect HDR to diffuse + specular cubemaps.
 *
 * Non-copyable, movable.
 */
class IBLPass : public ComputePass
{
public:
	/** Diffuse irradiance cubemap face resolution. */
	static constexpr uint32_t kDiffuseFaceRes = 64;
	/** Specular prefiltered cubemap face resolution (mip 0). */
	static constexpr uint32_t kSpecularFaceRes = 2048;
	/** Specular cubemap mip level count (roughness 0..1 maps to mip 0..7). */
	static constexpr uint32_t kSpecularMipLevels = 8;
	/** Default max step count for irradiance convolution. */
	static constexpr int32_t kDefaultIrradianceSteps = 64;
	/** Default max step count for specular prefilter. */
	static constexpr int32_t kDefaultSpecularSteps = 32;

	/**
	 * @brief Constructs the IBL pass - creates samplers, descriptor sets,
	 *        and compute pipelines.
	 *
	 * Does not create cubemap Images - the caller provides those to Generate().
	 *
	 * @param device          Logical device (retained reference).
	 * @param physicalDevice  Physical device for memory/sampler queries.
	 */
	IBLPass(const vk::raii::Device& device,
	        const vk::raii::PhysicalDevice& physicalDevice);

	~IBLPass();

	/** @brief No-op: IBL generation is one-shot via Generate(), not per-frame. */
	PassStats Record(vk::CommandBuffer cmd, RenderCache& cache, const RenderContext& ctx) override
	{
		(void)cmd;
		(void)cache;
		(void)ctx;
		return {};
	}

	/** @brief No-op: descriptor writes happen inside Generate(). */
	void WriteDescriptors(uint32_t, vk::Extent2D, RenderCache&) override {}

	// -------------------------------------------------------------------
	// Generation
	// -------------------------------------------------------------------

	/**
	 * @brief Generates diffuse + specular cubemaps from an equirect Image.
	 *
	 * The equirect Image must be 2D, R32G32B32A32_SFLOAT, in
	 * SHADER_READ_ONLY_OPTIMAL layout (or will be transitioned).
	 *
	 * The caller must pre-create the output Images:
	 * - diffuseOut:  64x64, 6-layer cube, 1 mip, R32G32B32A32_SFLOAT
	 * - specularOut: 2048x2048, 6-layer cube, 8 mips, R32G32B32A32_SFLOAT
	 *   (both must have eStorage | eSampled usage)
	 *
	 * Records one-shot command buffers: one irradiance dispatch, then one
	 * specular prefilter dispatch per mip.
	 *
	 * @param queue            Compute-capable queue for one-shot command submits.
	 * @param queueFamilyIndex Family of @p queue, for the transient command pool.
	 * @param equirectImage  Equirectangular HDR panorama (2D image).
	 * @param diffuseOut     Pre-created diffuse irradiance cubemap Image.
	 * @param specularOut    Pre-created specular prefiltered cubemap Image.
	 */
	void Generate(vk::Queue queue, uint32_t queueFamilyIndex,
	              const Image& equirectImage, Image& diffuseOut, Image& specularOut);

	/** @brief Static factory: creates a linear-clamp equirect sampler. */
	static vk::raii::Sampler CreateEquirectSampler(const vk::raii::Device& device);

private:
	// --- Pipeline / descriptor helpers ---
	static DescriptorSetLayout CreateDescriptorSetLayout(const vk::raii::Device& device);

	void BuildPipeline(const vk::raii::Device& device,
	                   const std::string& debugName) override;

	void WriteDescriptors(const Image& equirectImage,
	                      const vk::raii::Sampler& equirectSampler,
	                      const vk::raii::ImageView& outputView);

	// --- Dispatch helpers ---
	void dispatchCompute(vk::CommandBuffer cmdBuf,
	                     const vk::raii::Pipeline& pipeline,
	                     vk::PipelineLayout pipelineLayout,
	                     uint32_t groupCountX,
	                     uint32_t groupCountY,
	                     uint32_t groupCountZ,
	                     int32_t mipLevel,
	                     int32_t maxStep,
	                     float roughnessSq);

	// --- Self-loaded compute shaders (via ShaderLibrary) ---
	std::unique_ptr<ComputeShader> p_irradianceShader;
	std::unique_ptr<ComputeShader> p_specularShader;

	// (Pipelines inherited from Pass - p_pipelines[0]=irradiance, p_pipelines[1]=specular)
};

} // namespace neurus
