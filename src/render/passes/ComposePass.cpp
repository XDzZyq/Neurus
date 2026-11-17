/**
 * @file ComposePass.cpp
 * @brief Final scene compositing compute pass implementation.
 */

#include "RenderCache.hpp"
#include "passes/ComposePass.hpp"

#include "../PipelineBuilder.hpp"
#include "DescriptorLayout.hpp"
#include "core/RenderConfig.hpp"
#include "Image.hpp"
#include "render/Barrier.hpp"
#include "RenderContext.hpp"
#include "RenderFrameSnapshot.hpp"
#include "shaders/ShaderLibrary.hpp"
#include "shaders/ComputeShader.hpp"
#include "core/Log.hpp"

#include <stdexcept>
#include <string>

namespace neurus {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ComposePass::ComposePass(const vk::raii::Device& device,
                         const vk::raii::PhysicalDevice& physicalDevice,
                         uint32_t numSets)
	: ComposePass(device, physicalDevice, numSets,
	              ShaderLibrary::LoadComputeShader(
	                  "compose", NEURUS_SHADER_DIR "compute/compose.comp.glsl"))
{
}

ComposePass::ComposePass(const vk::raii::Device& device,
                         const vk::raii::PhysicalDevice& physicalDevice,
                         uint32_t numSets,
                         std::unique_ptr<ComputeShader> shader)
	: ComputePass(device, physicalDevice, MakeDescriptorLayout(device, *shader), numSets)
	, p_shader(std::move(shader))
{
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
// Pipeline creation
// ---------------------------------------------------------------------------

void ComposePass::BuildPipeline(const vk::raii::Device& device,
                                const std::string& debugName)
{
	if (!p_shader)
	{
		throw std::runtime_error("ComposePass: Compute shader not loaded or invalid");
	}

	auto spv = ShaderLibrary::Compile(p_shader->GetStage(ShaderType::COMPUTE),
	                                  ShaderType::COMPUTE, debugName);
	vk::ShaderModuleCreateInfo smCI({}, spv);
	vk::raii::ShaderModule module(device, smCI);
	vk::PipelineShaderStageCreateInfo stageCI({}, vk::ShaderStageFlagBits::eCompute, *module, "main");

	vk::PushConstantRange pushRange = MakePushConstantRange(
		*p_shader, vk::ShaderStageFlagBits::eCompute);

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


// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

PassStats ComposePass::Record(vk::CommandBuffer cmdBuf, const PassFrame& frame)
{
	PassStats stats{};

	const vk::Extent2D renderExtent{frame.ctx.width, frame.ctx.height};
	const uint32_t    frameIndex   = frame.frameIndex;

	// Settings() is never null; with no published config it resolves to
	// RenderConfig's own declared default.
	const float gamma = frame.ctx.Frame().Settings().r_gamma;

	Image* input  = frame.resources.Input("gHDRColor");
	Image* output = frame.resources.Output("outputComposed");
	if (!input || !output)
	{
		NEURUS_ERR("[ComposePass] graph did not provide gHDRColor/outputComposed");
		return stats;
	}

	Barrier::Transition(cmdBuf, *input, ImageState::ColorShaderRead);
	Barrier::Transition(cmdBuf, *output, ImageState::ShaderWrite);

	cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, *p_pipelines[0].pipeline);

	cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
	                          *p_pipelines[0].pipelineLayout,
	                          0,                                    // firstSet
	                          {p_descriptorSets[frameIndex].handle()},
	                          {});

	cmdBuf.pushConstants<float>(
		*p_pipelines[0].pipelineLayout,
		vk::ShaderStageFlagBits::eCompute,
		0,
		gamma);

	const uint32_t groupCountX = (renderExtent.width  + 15) / 16;
	const uint32_t groupCountY = (renderExtent.height + 15) / 16;
	++stats.dispatches;
	cmdBuf.dispatch(groupCountX, groupCountY, 1);

	// ComposedOutput is deliberately left in ShaderWrite. Pre-transitioning it
	// to the state the next consumer expects would swallow this dispatch's
	// writes: the consumer's own barrier would then name no compute writes and
	// make none of them visible. Each consumer transitions it itself, so it
	// gets a real (ComputeShader, ShaderWrite) -> its-own-access dependency.

	return stats;
}

PassIO ComposePass::GetIO() const
{
	PassIO io;
	io.name = "ComposePass";
	return io;
}

std::vector<const ShaderData*> ComposePass::GetShaders() const
{
	return {p_shader.get()};
}

} // namespace neurus
