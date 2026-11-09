/**
 * @file IBLPass.cpp
 * @brief Image-Based Lighting generation pass implementation - pure compute service.
 */

#include "passes/IBLPass.h"

#include "../PipelineBuilder.h"
#include "Image.h"
#include "render/Barrier.h"
#include "render/SharedQueue.h"
#include "shaders/ShaderLibrary.h"
#include "shaders/ComputeShader.h"
#include "core/Log.h"

#include <cstring>
#include <stdexcept>

namespace neurus {

// ---------------------------------------------------------------------------
// Push constant struct (shared by both irradiance and specular shaders)
// ---------------------------------------------------------------------------

/** @brief Push constants for IBL convolution compute shaders (16 bytes). */
struct alignas(16) IBLPushConstants
{
	int32_t mipLevel;      ///< Target mip level (unused by irradiance)
	int32_t maxStep;       ///< Diamond-pattern hemisphere step count
	float   roughnessSq;   ///< Roughness squared (unused by irradiance)
	float   _pad;          ///< Padding to 16 bytes
};
static_assert(sizeof(IBLPushConstants) == 16, "IBLPushConstants must be 16 bytes");

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

IBLPass::IBLPass(const vk::raii::Device& device,
                 const vk::raii::PhysicalDevice& physicalDevice)
	: ComputePass(device, physicalDevice, IBLPass::CreateDescriptorSetLayout(device), 1)
	// --- Self-load compute shaders via ShaderLibrary ---
	, p_irradianceShader(
		ShaderLibrary::LoadComputeShader("irradiance",
		                                  "res/shaders/compute/irradiance_conv.comp"))
	, p_specularShader(
		ShaderLibrary::LoadComputeShader("importance_samp",
		                                  "res/shaders/compute/importance_samp.comp"))
{
	BuildPipeline(device, "IBLPass");

	NEURUS_LOG("[IBLPass] irradiance=" << (p_irradianceShader ? "OK" : "FAIL")
	           << " specular=" << (p_specularShader ? "OK" : "FAIL"));
}

IBLPass::~IBLPass() = default;

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

void IBLPass::Generate(SharedQueue& queue, uint32_t queueFamilyIndex,
                       const Image& equirectImage, Image& diffuseOut, Image& specularOut)
{
	NEURUS_LOG("[IBLPass] Generating IBL cubemaps from equirect "
	           << equirectImage.Extent().width << "x" << equirectImage.Extent().height);

	// --- Create local equirect sampler ---
	vk::raii::Sampler equirectSampler = CreateEquirectSampler(*p_device);

	// --- Create per-mip ImageViews for specular cubemap ---
	std::vector<vk::raii::ImageView> specularMipViews;
	specularMipViews.reserve(kSpecularMipLevels);
	for (uint32_t mip = 0; mip < kSpecularMipLevels; ++mip)
	{
		const vk::ImageSubresourceRange subresourceRange(
			vk::ImageAspectFlagBits::eColor,
			mip,     // baseMipLevel
			1,       // levelCount
			0,       // baseArrayLayer
			6);      // layerCount (6 faces)

		const vk::ImageViewCreateInfo viewCI(
			{},
			*specularOut.ImageHandle(),
			vk::ImageViewType::e2DArray,
			vk::Format::eR32G32B32A32Sfloat,
			vk::ComponentMapping(),
			subresourceRange);

		specularMipViews.push_back(vk::raii::ImageView(*p_device, viewCI));
	}

	// The shader binds the cube as an image2DArray.
	vk::ImageSubresourceRange diffSubresource(
		vk::ImageAspectFlagBits::eColor,
		0, 1,    // baseMipLevel=0, levelCount=1
		0, 6);   // baseArrayLayer=0, layerCount=6
	vk::ImageViewCreateInfo diffViewCI(
		{},
		*diffuseOut.ImageHandle(),
		vk::ImageViewType::e2DArray,
		vk::Format::eR32G32B32A32Sfloat,
		vk::ComponentMapping(),
		diffSubresource);
	vk::raii::ImageView diffArrayView(*p_device, diffViewCI);

	// --- Create transient command pool ---
	const vk::CommandPoolCreateInfo poolCI(
		vk::CommandPoolCreateFlagBits::eTransient | vk::CommandPoolCreateFlagBits::eResetCommandBuffer, queueFamilyIndex);
	vk::raii::CommandPool cmdPool(*p_device, poolCI);

	const vk::CommandBufferAllocateInfo allocInfo(
		*cmdPool, vk::CommandBufferLevel::ePrimary, 1);
	vk::raii::CommandBuffers cmdBufs(*p_device, allocInfo);

	// ===================================================================
	// Pass 1: Diffuse irradiance convolution (1 dispatch)
	// ===================================================================
	{
		vk::CommandBufferBeginInfo beginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
		cmdBufs[0].begin(beginInfo);

		// Explicit subresource overload: no m_state update on the const input.
		Barrier::Transition(*cmdBufs[0],
		                    const_cast<Image&>(equirectImage),
		                    ImageState::ColorShaderRead,
		                    equirectImage.AllSubresources());

		Barrier::Transition(*cmdBufs[0], diffuseOut, ImageState::ShaderWrite);

		WriteDescriptors(equirectImage, equirectSampler, diffArrayView);

		const uint32_t groupsX = (kDiffuseFaceRes + 3) / 4;
		const uint32_t groupsY = (kDiffuseFaceRes + 3) / 4;
		const uint32_t groupsZ = 6;  // 6 faces

		dispatchCompute(cmdBufs[0],
		                p_pipelines[0].pipeline,
		                *p_pipelines[0].pipelineLayout,
		                groupsX, groupsY, groupsZ,
		                /*mipLevel=*/0,
		                kDefaultIrradianceSteps,
		                /*roughnessSq=*/0.0f);

		// Make the compute writes visible to later reads.
		Barrier::Transition(*cmdBufs[0], diffuseOut, ImageState::ColorShaderRead);
		cmdBufs[0].end();

		vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmdBufs[0]));
		queue.SubmitAndWait(submitInfo);

		NEURUS_LOG("[IBLPass] Diffuse irradiance convolution complete");
	}

	// ===================================================================
	// Pass 2: Specular prefilter (8 dispatches, one per mip)
	// ===================================================================
	for (uint32_t mip = 0; mip < kSpecularMipLevels; ++mip)
	{
		const float roughness = static_cast<float>(mip) / static_cast<float>(kSpecularMipLevels - 1);
		const float roughnessSq = roughness * roughness;

		cmdBufs[0].begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

		// Explicit subresource range: no m_state update.
		Barrier::Transition(*cmdBufs[0], specularOut, ImageState::ShaderWrite,
		                    vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,
		                                              mip, 1, 0, 6));

		WriteDescriptors(equirectImage, equirectSampler, specularMipViews[mip]);

		const uint32_t faceRes = kSpecularFaceRes >> mip;
		const uint32_t groupsX = (faceRes + 3) / 4;
		const uint32_t groupsY = (faceRes + 3) / 4;
		const uint32_t groupsZ = 6;

		dispatchCompute(cmdBufs[0],
		                p_pipelines[1].pipeline,
		                *p_pipelines[1].pipelineLayout,
		                groupsX, groupsY, groupsZ,
		                static_cast<int32_t>(mip),
		                kDefaultSpecularSteps,
		                roughnessSq);

		{
			const auto barrier = vk::ImageMemoryBarrier2(
				vk::PipelineStageFlagBits2::eComputeShader,
				vk::AccessFlagBits2::eShaderWrite,
				vk::PipelineStageFlagBits2::eComputeShader,
				vk::AccessFlagBits2::eShaderRead,
				vk::ImageLayout::eGeneral,
				vk::ImageLayout::eGeneral,
				VK_QUEUE_FAMILY_IGNORED,
				VK_QUEUE_FAMILY_IGNORED,
				*specularOut.ImageHandle(),
				vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,
				                          mip, 1, 0, 6));
			const vk::DependencyInfo depInfo({}, {}, {}, barrier);
			cmdBufs[0].pipelineBarrier2(depInfo);
		}

		cmdBufs[0].end();

		vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmdBufs[0]));
		queue.SubmitAndWait(submitInfo);
	}

	// --- Final: transition cubemaps to SHADER_READ_ONLY_OPTIMAL ---
	{
		cmdBufs[0].begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));

		Barrier::Transition(*cmdBufs[0], diffuseOut, ImageState::ColorShaderRead);

		Barrier::Transition(*cmdBufs[0], specularOut, ImageState::ColorShaderRead);

		cmdBufs[0].end();

		vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmdBufs[0]));
		queue.SubmitAndWait(submitInfo);
	}

	NEURUS_LOG("[IBLPass] IBL generation complete - diffuse "
	           << kDiffuseFaceRes << "^2, specular "
	           << kSpecularFaceRes << "^2 x " << kSpecularMipLevels << " mips");
}

// ---------------------------------------------------------------------------
// Descriptor set layout
// ---------------------------------------------------------------------------

DescriptorSetLayout IBLPass::CreateDescriptorSetLayout(const vk::raii::Device& device)
{
	return BuildLayout()
		// Equirect input (combined image sampler)
		.AddBinding(0,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// Cubemap output (storage image - image2DArray)
		.AddBinding(1,
		            vk::DescriptorType::eStorageImage,
		            vk::ShaderStageFlagBits::eCompute)
		.Build(device);
}

// ---------------------------------------------------------------------------
// Sampler factories
// ---------------------------------------------------------------------------

vk::raii::Sampler IBLPass::CreateEquirectSampler(const vk::raii::Device& device)
{
	vk::SamplerCreateInfo samplerCI(
		{},
		vk::Filter::eLinear,
		vk::Filter::eLinear,
		vk::SamplerMipmapMode::eNearest,
		vk::SamplerAddressMode::eClampToEdge,
		vk::SamplerAddressMode::eClampToEdge,
		vk::SamplerAddressMode::eClampToEdge,
		0.0f,
		VK_FALSE,
		0.0f,
		VK_FALSE,
		vk::CompareOp::eAlways,
		0.0f,
		0.0f,
		vk::BorderColor::eFloatTransparentBlack,
		VK_FALSE);

	return vk::raii::Sampler(device, samplerCI);
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

void IBLPass::BuildPipeline(const vk::raii::Device& device,
                             const std::string& debugName)
{
	auto buildOne = [&](std::unique_ptr<ComputeShader>& computeShader,
	                    const char* subName) -> Pipeline
	{
		if (!computeShader)
		{
			throw std::runtime_error(std::string("IBLPass: ") + subName + " shader not loaded or invalid");
		}

		auto spv = ShaderLibrary::Compile(computeShader->GetStage(ShaderType::COMPUTE),
		                                  ShaderType::COMPUTE, subName);
		vk::ShaderModuleCreateInfo smCI({}, spv);
		vk::raii::ShaderModule module(device, smCI);
		vk::PipelineShaderStageCreateInfo stageCI({}, vk::ShaderStageFlagBits::eCompute, *module, "main");

		PipelineBuilder builder;

		vk::PushConstantRange pushRange(
			vk::ShaderStageFlagBits::eCompute,
			0,
			sizeof(IBLPushConstants));

		return builder.AddShaderStage(stageCI)
			.SetDebugName(subName)
			.AddDescriptorSetLayout(*p_descriptorSetLayout.layout())
			.AddPushConstantRange(pushRange)
			.BuildComputePipeline(device);
	};

	p_pipelines.push_back(buildOne(p_irradianceShader, (debugName + "::Irradiance").c_str()));
	p_pipelines.push_back(buildOne(p_specularShader,   (debugName + "::Specular").c_str()));
}

// ---------------------------------------------------------------------------
// Descriptor writes
// ---------------------------------------------------------------------------

void IBLPass::WriteDescriptors(const Image& equirectImage,
                               const vk::raii::Sampler& equirectSampler,
                               const vk::raii::ImageView& outputView)
{
	auto& dstSet = p_descriptorSets[0];

	{
		vk::DescriptorImageInfo imageInfo(
			equirectSampler,
			*equirectImage.ImageViewHandle(),
			vk::ImageLayout::eShaderReadOnlyOptimal);

		dstSet.WriteImage(0, imageInfo,
		                   vk::DescriptorType::eCombinedImageSampler);
	}

	{
		vk::DescriptorImageInfo imageInfo(
			nullptr,
			outputView,
			vk::ImageLayout::eGeneral);

		dstSet.WriteImage(1, imageInfo,
		                   vk::DescriptorType::eStorageImage);
	}
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void IBLPass::dispatchCompute(vk::CommandBuffer cmdBuf,
                               const vk::raii::Pipeline& pipeline,
                               vk::PipelineLayout pipelineLayout,
                               uint32_t groupCountX,
                               uint32_t groupCountY,
                               uint32_t groupCountZ,
                               int32_t mipLevel,
                               int32_t maxStep,
                               float roughnessSq)
{
	cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);

	cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
	                          pipelineLayout,
	                          0,
	                          {p_descriptorSets[0].handle()},
	                          {});

	IBLPushConstants pc = {};
	pc.mipLevel    = mipLevel;
	pc.maxStep     = maxStep;
	pc.roughnessSq = roughnessSq;

	cmdBuf.pushConstants<IBLPushConstants>(
		pipelineLayout,
		vk::ShaderStageFlagBits::eCompute,
		0,
		pc);

	cmdBuf.dispatch(groupCountX, groupCountY, groupCountZ);
}

} // namespace neurus
