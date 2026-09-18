/**
 * @file ComposePass.cpp
 * @brief Final compositing compute pass implementation.
 */

#include "RenderCache.h"
#include "passes/ComposePass.h"

#include "../PipelineBuilder.h"
#include "../RenderConfig.h"
#include "Image.h"
#include "render/Barrier.h"
#include "RenderContext.h"
#include "shaders/ShaderLibrary.h"
#include "shaders/ComputeShader.h"
#include "core/Log.h"

#include <stdexcept>
#include <string>

namespace neurus {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ComposePass::ComposePass(const vk::raii::Device& device,
                         const vk::raii::PhysicalDevice& physicalDevice,
                         uint32_t numSets)
	: ComputePass(device, physicalDevice,
	              ComposePass::CreateDescriptorSetLayout(device), numSets)
	// --- Self-load compute shader via ShaderLibrary ---
	, p_shader(
		ShaderLibrary::LoadComputeShader("compose",
		                                  NEURUS_SHADER_DIR "compute/compose.comp"))
{
	// --- Create pipeline from self-loaded shader ---
	BuildPipeline(device, "ComposePass");

	NEURUS_LOG("[ComposePass] numSets=" << numSets
	           << " shader=" << (p_shader ? "OK" : "FAIL"));

#ifdef _DEBUG
	for (uint32_t i = 0; i < numSets; ++i)
	{
		const std::string dsName = "ComposePass_Set" + std::to_string(i);
		p_descriptorSets[i].SetDebugName(dsName.c_str());
	}
#endif
}

// ---------------------------------------------------------------------------
// Descriptor set layout
// ---------------------------------------------------------------------------

DescriptorSetLayout ComposePass::CreateDescriptorSetLayout(const vk::raii::Device& device)
{
	return BuildLayout()
		// HDRColor input (combined image sampler)
		.AddBinding(0,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// GizmoHighlight input (combined image sampler)
		.AddBinding(1,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// ComposedOutput (storage image, RGBA16F)
		.AddBinding(2,
		            vk::DescriptorType::eStorageImage,
		            vk::ShaderStageFlagBits::eCompute)
		.Build(device);
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

void ComposePass::BuildPipeline(const vk::raii::Device& device,
                                const std::string& debugName)
{
	// --- Guard: shader must be valid ---
	if (!p_shader)
	{
		throw std::runtime_error("ComposePass: Compute shader not loaded or invalid");
	}

	// --- Compile and create temporary shader module ---
	auto spv = ShaderLibrary::Compile(p_shader->GetStage(ShaderType::COMPUTE),
	                                  ShaderType::COMPUTE, debugName);
	vk::ShaderModuleCreateInfo smCI({}, spv);
	vk::raii::ShaderModule module(device, smCI);
	vk::PipelineShaderStageCreateInfo stageCI({}, vk::ShaderStageFlagBits::eCompute, *module, "main");

	// --- Push constant range (1 float = 4 bytes) ---
	vk::PushConstantRange pushRange(
		vk::ShaderStageFlagBits::eCompute,
		0,
		sizeof(float));  // gamma

	// --- Build compute pipeline ---
	PipelineBuilder builder;
	p_pipelines.push_back(
		builder.AddShaderStage(stageCI)
			.SetDebugName(debugName.c_str())
			.AddDescriptorSetLayout(*p_descriptorSetLayout.layout())
			.AddPushConstantRange(pushRange)
			.BuildComputePipeline(device));
}

// ---------------------------------------------------------------------------
// Descriptor writes
// ---------------------------------------------------------------------------

void ComposePass::WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache)
{
	DescriptorSet& dstSet = p_descriptorSets[setIndex];

	// --- Write HDRColor input (combined image sampler) ---
	{
		const auto& hdrAtt = cache.GetAttachment(AttachmentName::HDRColor, extent);

		vk::DescriptorImageInfo imageInfo(
			*p_sampler,                              // sampler
			*hdrAtt.ImageViewHandle(),               // imageView
			vk::ImageLayout::eShaderReadOnlyOptimal  // imageLayout
		);

		dstSet.WriteImage(0, imageInfo,
		                  vk::DescriptorType::eCombinedImageSampler);
	}

	// --- Write GizmoHighlight input (combined image sampler) ---
	{
		const auto& gizmoAtt = cache.GetAttachment(AttachmentName::GizmoHighlight, extent);

		vk::DescriptorImageInfo imageInfo(
			*p_sampler,                              // sampler
			*gizmoAtt.ImageViewHandle(),             // imageView
			vk::ImageLayout::eShaderReadOnlyOptimal  // imageLayout
		);

		dstSet.WriteImage(1, imageInfo,
		                  vk::DescriptorType::eCombinedImageSampler);
	}

	// --- Write ComposedOutput (storage image) ---
	{
		const auto& compAtt = cache.GetAttachment(AttachmentName::ComposedOutput, extent);

		vk::DescriptorImageInfo imageInfo(
			nullptr,                      // sampler (not used for storage images)
			*compAtt.ImageViewHandle(),   // imageView
			vk::ImageLayout::eGeneral     // imageLayout
		);

		dstSet.WriteImage(2, imageInfo,
		                  vk::DescriptorType::eStorageImage);
	}
}

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

PassStats ComposePass::Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx)
{
	PassStats stats{};

	const vk::Extent2D renderExtent{ctx.width, ctx.height};
	const uint32_t    frameIndex   = ctx.frameIndex;

	// --- Read gamma from config ---
	const auto* config = static_cast<const RenderConfig*>(ctx.editor.config);
	const float gamma = config ? config->r_gamma : 1.0f;

	// --- 1. Write descriptor set for this frame slot ---
	WriteDescriptors(frameIndex, renderExtent, cache);

	// --- 2. Transition input attachments to ShaderRead and output to ShaderWrite ---
	{
		// HDRColor: current state → ColorShaderRead
		auto& hdrAtt = cache.GetAttachment(AttachmentName::HDRColor, renderExtent);
		Barrier::Transition(cmdBuf, hdrAtt, ImageState::ColorShaderRead);

		// GizmoHighlight: current state → ColorShaderRead
		auto& gizmoAtt = cache.GetAttachment(AttachmentName::GizmoHighlight, renderExtent);
		Barrier::Transition(cmdBuf, gizmoAtt, ImageState::ColorShaderRead);

		// ComposedOutput: current state → ShaderWrite (compute write)
		auto& compAtt = cache.GetAttachment(AttachmentName::ComposedOutput, renderExtent);
		Barrier::Transition(cmdBuf, compAtt, ImageState::ShaderWrite);
	}

	// --- 3. Bind compute pipeline ---
	cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, *p_pipelines[0].pipeline);

	// --- 4. Bind descriptor set ---
	cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
	                          *p_pipelines[0].pipelineLayout,
	                          0,                                    // firstSet
	                          {p_descriptorSets[frameIndex].handle()},
	                          {});

	// --- 5. Push constants (gamma) ---
	cmdBuf.pushConstants<float>(
		*p_pipelines[0].pipelineLayout,
		vk::ShaderStageFlagBits::eCompute,
		0,
		gamma);

	// --- 6. Dispatch ---
	const uint32_t groupCountX = (renderExtent.width  + 15) / 16;
	const uint32_t groupCountY = (renderExtent.height + 15) / 16;
	++stats.dispatches;
	cmdBuf.dispatch(groupCountX, groupCountY, 1);

	// ComposedOutput is deliberately left in ShaderWrite. Pre-transitioning it to
	// the state the *next* consumer is expected to want (TransferSrc for the
	// swapchain blit) would swallow this dispatch's writes: the consumer's own
	// barrier would then read TransferSrc, giving it a src access scope of
	// TransferRead, which names no writes and makes none of them visible. Whoever
	// consumes ComposedOutput — DebugPass, FXAAPass or the blit — transitions it
	// itself and so gets a real (ComputeShader, ShaderWrite) → its-own-access
	// dependency. Without that, DebugPass's overlay raced this dispatch on
	// MoltenVK and each frame kept a random subset of the overlay's tiles.

	return stats;
}

PassIO ComposePass::GetIO() const
{
	// Binding metadata is indicative only: ComposePass still writes its own
	// descriptors. Resource names drive RenderGraph edges — HDRColor (from
	// LightingPass) and GizmoHighlight (from GizmoPass) are in-graph inputs;
	// ComposedOutput feeds FXAAPass / the swapchain blit.
	PassIO io;
	io.name  = "ComposePass";
	io.reads = {
		{AttachmentName::HDRColor,       0, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
		{AttachmentName::GizmoHighlight, 1, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
	};
	io.writes = {
		{AttachmentName::ComposedOutput, 2, vk::DescriptorType::eStorageImage, vk::ImageLayout::eGeneral},
	};
	return io;
}

} // namespace neurus
