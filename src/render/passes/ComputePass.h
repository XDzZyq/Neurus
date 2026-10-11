/**
 * @file ComputePass.h
 * @brief Abstract compute shader pass base class.
 *
 * Extracts shared infrastructure (sampler, descriptor pool/sets, dispatch
 * helper) so subclasses only implement their own descriptor bindings and
 * compute pipelines.
 *
 * @note p_descriptorSetLayout is moved in via the constructor - subclasses
 *       create their own layout via their static CreateDescriptorSetLayout()
 *       factory before calling this constructor.
 */

#pragma once

#include "Pass.h"
#include "../DescriptorManager.h"

#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <string>
#include <vector>

namespace neurus {

class RenderCache;

/**
 * @brief Abstract base class for compute shader passes.
 *
 * Provides a nearest-neighbour sampler, descriptor pool/set lifecycle and a
 * compute dispatch helper. Subclasses supply their descriptor set layout,
 * WriteDescriptors() and pipeline, and implement Record().
 */
class ComputePass : public Pass
{
public:
	/**
	 * @brief Constructs the compute pass infrastructure.
	 *
	 * @param device              Logical device (retained reference).
	 * @param physicalDevice      Physical device (for sampler creation).
	 * @param descriptorSetLayout Descriptor set layout created by the subclass (moved in).
	 * @param numSets             Number of descriptor sets (one per in-flight frame).
	 */
	ComputePass(const vk::raii::Device& device,
	            const vk::raii::PhysicalDevice& physicalDevice,
	            DescriptorSetLayout&& descriptorSetLayout,
	            uint32_t numSets);

	~ComputePass() override = default;

	// --- Non-copyable, movable (inherited from Pass) ---
	ComputePass(ComputePass&&) noexcept = default;
	ComputePass& operator=(ComputePass&&) noexcept = default;

	/**
	 * @brief Writes all descriptor bindings into the specified set.
	 *
	 * Called every frame before dispatch; one set per in-flight frame avoids
	 * updating a set while the GPU is still reading it.
	 *
	 * @param setIndex  Index into p_descriptorSets (0 .. numSets-1).
	 */
	virtual void WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache) = 0;

protected:
	// -------------------------------------------------------------------
	// Factory helpers
	// -------------------------------------------------------------------

	/**
	 * @brief Creates the shared nearest-neighbour, clamp-to-edge sampler.
	 *
	 * @param device         Logical device.
	 * @param physicalDevice Physical device (unused, kept for API symmetry).
	 * @return A nearest-neighbour sampler.
	 */
	static vk::raii::Sampler CreateSampler(const vk::raii::Device& device,
	                                       const vk::raii::PhysicalDevice& physicalDevice);

	/**
	 * @brief Creates the descriptor pool, allocates descriptor sets, and
	 *        assigns debug names (in _DEBUG builds).
	 *
	 * @param numSets Number of descriptor sets to allocate.
	 */
	void CreateDescriptorSets(uint32_t numSets);

	// -------------------------------------------------------------------
	// Dispatch helper
	// -------------------------------------------------------------------

	/**
	 * @brief Binds the pipeline and descriptor set, pushes constants, and
	 *        dispatches at ceil(extent/16) x ceil(extent/16) thread groups.
	 *
	 * @param cmdBuf        Command buffer in recording state.
	 * @param extent        Render area dimensions (determines group count).
	 * @param pipeline      Compute pipeline to bind.
	 * @param layout        Pipeline layout (for descriptor set binding + push constants).
	 * @param descriptorSet Descriptor set to bind at set index 0.
	 * @param pushData      Pointer to push constant data (nullable if pushSize==0).
	 * @param pushSize      Size of push constant data in bytes.
	 */
	void DispatchCompute(vk::CommandBuffer cmdBuf,
	                     vk::Extent2D extent,
	                     const vk::raii::Pipeline& pipeline,
	                     vk::PipelineLayout layout,
	                     vk::DescriptorSet descriptorSet,
	                     const void* pushData,
	                     uint32_t pushSize);

	// -------------------------------------------------------------------
	// Members (accessible to subclasses)
	// -------------------------------------------------------------------

	/// Nearest-neighbour sampler for G-Buffer reads.
	vk::raii::Sampler p_sampler = nullptr;

	/// Descriptor set layout (moved in from subclass).
	DescriptorSetLayout p_descriptorSetLayout;

	/// Descriptor pool (one per pass, sized for numSets).
	DescriptorPool p_descriptorPool;

	/// Descriptor sets (one per in-flight frame).
	std::vector<DescriptorSet> p_descriptorSets;
};

// ===================================================================
// Inline implementations
// ===================================================================

inline ComputePass::ComputePass(const vk::raii::Device& device,
                                const vk::raii::PhysicalDevice& physicalDevice,
                                DescriptorSetLayout&& descriptorSetLayout,
                                uint32_t numSets)
	: Pass()
	, p_sampler(CreateSampler(device, physicalDevice))
	, p_descriptorSetLayout(std::move(descriptorSetLayout))
{
	p_device = &device;
	p_physicalDevice = &physicalDevice;
	CreateDescriptorSets(numSets);
}

inline vk::raii::Sampler ComputePass::CreateSampler(
	const vk::raii::Device& device,
	const vk::raii::PhysicalDevice& /*physicalDevice*/)
{
	vk::SamplerCreateInfo samplerCI(
		{},                                    // flags
		vk::Filter::eNearest,                  // magFilter
		vk::Filter::eNearest,                  // minFilter
		vk::SamplerMipmapMode::eNearest,        // mipmapMode
		vk::SamplerAddressMode::eClampToEdge,   // addressModeU
		vk::SamplerAddressMode::eClampToEdge,   // addressModeV
		vk::SamplerAddressMode::eClampToEdge,   // addressModeW
		0.0f,                                   // mipLodBias
		VK_FALSE,                               // anisotropyEnable
		0.0f,                                   // maxAnisotropy
		VK_FALSE,                               // compareEnable
		vk::CompareOp::eAlways,                 // compareOp
		0.0f,                                   // minLod
		0.0f,                                   // maxLod
		vk::BorderColor::eFloatTransparentBlack, // borderColor
		VK_FALSE                                // unnormalizedCoordinates
	);

	return vk::raii::Sampler(device, samplerCI);
}

inline void ComputePass::CreateDescriptorSets(uint32_t numSets)
{
	p_descriptorPool = DescriptorPool(*p_device,
	                                  numSets,
	                                  DescriptorPool::CalculatePoolSizes({&p_descriptorSetLayout}, numSets));
	p_descriptorSets = p_descriptorPool.Allocate(p_descriptorSetLayout, numSets);

#ifdef _DEBUG
	for (uint32_t i = 0; i < numSets; ++i)
	{
		const std::string dsName = "ComputePass_Set" + std::to_string(i);
		p_descriptorSets[i].SetDebugName(dsName.c_str());
	}
#endif
}

inline void ComputePass::DispatchCompute(
	vk::CommandBuffer cmdBuf,
	vk::Extent2D extent,
	const vk::raii::Pipeline& pipeline,
	vk::PipelineLayout layout,
	vk::DescriptorSet descriptorSet,
	const void* pushData,
	uint32_t pushSize)
{
	cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
	cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
	                          layout, 0, {descriptorSet}, {});

	if (pushData && pushSize > 0)
	{
		cmdBuf.pushConstants(layout,
		                     vk::ShaderStageFlagBits::eCompute,
		                     0, pushSize, pushData);
	}

	const uint32_t groupCountX = (extent.width  + 15) / 16;
	const uint32_t groupCountY = (extent.height + 15) / 16;
	cmdBuf.dispatch(groupCountX, groupCountY, 1);
}

} // namespace neurus
