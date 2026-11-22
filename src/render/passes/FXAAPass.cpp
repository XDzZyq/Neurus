#include "RenderCache.hpp"
#include "passes/FXAAPass.hpp"
#include "asset/data/render_graph/PassKinds.hpp"
#include "../PipelineBuilder.hpp"
#include "DescriptorLayout.hpp"
#include "core/RenderConfig.hpp"
#include "Barrier.hpp"
#include "Image.hpp"
#include "render_api/RenderContext.hpp"
#include "render_api/RenderFrameSnapshot.hpp"


#include "core/Log.hpp"
#include <stdexcept>

namespace neurus {

FXAAPass::FXAAPass(RenderCache& cache, const vk::raii::Device& device,
                   const vk::raii::PhysicalDevice& physicalDevice,
                   uint32_t numSets)
	: FXAAPass(device, physicalDevice, numSets,
	           cache.EnsureDefaultShader(kFxaaShaders[0].name))
{
}

FXAAPass::FXAAPass(const vk::raii::Device& device,
                   const vk::raii::PhysicalDevice& physicalDevice,
                   uint32_t numSets,
                   std::shared_ptr<const ShaderGPU> shader)
	: ComputePass(device, physicalDevice, MakeDescriptorLayout(device, RequireShader(shader)), numSets)
	, p_shader(std::move(shader))
{
	// Check format features: linear filtering required for sub-pixel texture() resample
	auto fmtProps = physicalDevice.getFormatProperties(vk::Format::eR16G16B16A16Sfloat);
	bool linearOk = (fmtProps.optimalTilingFeatures & vk::FormatFeatureFlagBits::eSampledImageFilterLinear)
	                == vk::FormatFeatureFlagBits::eSampledImageFilterLinear;

	if (linearOk)
	{
		p_bilinearSampler = CreateBilinearSampler(device);
		p_hasBilinear = true;
	}

	BuildPipeline(device, "FXAAPass");
	NEURUS_LOG("[FXAAPass] numSets=" << numSets
	           << " shader=" << (p_shader ? "OK" : "FAIL")
	           << " bilinear=" << (linearOk ? "yes" : "NO (fallback to nearest)"));
}

vk::raii::Sampler FXAAPass::CreateBilinearSampler(const vk::raii::Device& device)
{
	vk::SamplerCreateInfo ci(
		{},
		vk::Filter::eLinear,                             // magFilter - bilinear for sub-pixel resample
		vk::Filter::eLinear,                             // minFilter
		vk::SamplerMipmapMode::eNearest,
		vk::SamplerAddressMode::eClampToEdge,
		vk::SamplerAddressMode::eClampToEdge,
		vk::SamplerAddressMode::eClampToEdge,
		0.0f, VK_FALSE, 0.0f, VK_FALSE,
		vk::CompareOp::eAlways,
		0.0f, 0.0f,
		vk::BorderColor::eFloatTransparentBlack,
		VK_FALSE);
	return vk::raii::Sampler(device, ci);
}

void FXAAPass::BuildPipeline(const vk::raii::Device& device, const std::string& debugName)
{
	if (!p_shader)
		throw std::runtime_error("FXAAPass: shader invalid");
	const auto module = p_shader->Module(ShaderType::COMPUTE);
	vk::PipelineShaderStageCreateInfo stageCI({}, vk::ShaderStageFlagBits::eCompute, module, "main");
	vk::PushConstantRange pc = MakePushConstantRange(
		*p_shader, vk::ShaderStageFlagBits::eCompute);
	p_pipelines.push_back(
		PipelineBuilder()
			.AddShaderStage(stageCI)
			.SetDebugName(debugName.c_str())
			.AddDescriptorSetLayout(*p_descriptorSetLayout.layout())
			.AddPushConstantRange(pc)
			.BuildComputePipeline(device));
}


PassStats FXAAPass::Record(vk::CommandBuffer cmdBuf, const PassFrame& frame)
{
	PassStats stats{};

	const vk::Extent2D extent{frame.ctx.width, frame.ctx.height};
	const uint32_t fi = frame.frameIndex;

	// Settings() is never null and resolves to RenderConfig's own in-class
	// defaults, so the three tuning values below carry no literal of their own.
	const RenderConfig& cfg = frame.ctx.Frame().Settings();

	Image* input  = frame.resources.Input("gInput");
	Image* output = frame.resources.Output("outputFXAA");
	if (!input || !output)
	{
		NEURUS_ERR("[FXAAPass] graph did not provide gInput/outputFXAA");
		return stats;
	}

	Barrier::Transition(cmdBuf, *input, ImageState::ColorShaderRead);
	Barrier::Transition(cmdBuf, *output, ImageState::ShaderWrite);

	FXAAPushConstants pc = {};
	pc.rcpWidth       = 1.0f / float(extent.width);
	pc.rcpHeight      = 1.0f / float(extent.height);
	pc.qualitySubpix  = cfg.r_fxaa_subpix;
	pc.edgeThreshold  = cfg.r_fxaa_edge_threshold;
	pc.edgeThresholdMin = cfg.r_fxaa_edge_threshold_min;

	cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, *p_pipelines[0].pipeline);
	cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
	                          *p_pipelines[0].pipelineLayout, 0,
	                          {p_descriptorSets[fi].handle()}, {});
	cmdBuf.pushConstants<FXAAPushConstants>(
		*p_pipelines[0].pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, pc);

	const uint32_t gx = (extent.width  + 15) / 16;
	const uint32_t gy = (extent.height + 15) / 16;
	++stats.dispatches;
	cmdBuf.dispatch(gx, gy, 1);

	// A producer must not pre-transition its output into the consumer's
	// expected state; FXAAOutput stays in ShaderWrite and the swapchain blit
	// transitions it itself.

	return stats;
}

PassIO FXAAPass::GetIO() const
{
	PassIO io;
	io.name = "FXAAPass";
	return io;
}

std::vector<const ShaderGPU*> FXAAPass::GetShaders() const
{
	return {p_shader.get()};
}

} // namespace neurus
