/**
 * @file LightingPass.cpp
 * @brief PBR lighting pass implementation.
 */

#include "passes/LightingPass.hpp"

#include "../PipelineBuilder.hpp"
#include "asset/data/render_graph/PassKinds.hpp"
#include "DescriptorLayout.hpp"
#include "RenderCache.hpp"
#include "render_api/RenderContext.hpp"
#include "render_api/RenderFrameSnapshot.hpp"
#include "Image.hpp"
#include "render/Barrier.hpp"
#include "core/RenderConfig.hpp"
#include "asset/shader/ShaderLibrary.hpp"
#include "asset/data/shader/ComputeShader.hpp"
#include "Texture.hpp"

#include "core/Log.hpp"

#include "scene/Light.hpp"
#include "scene/Scene.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace neurus {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LightingPass::LightingPass(const vk::raii::Device& device,
                           const vk::raii::PhysicalDevice& physicalDevice,
                           uint32_t numSets)
	: LightingPass(device, physicalDevice, numSets,
	               ShaderLibrary::LoadComputeShader(
	                   "pbr_lighting", "res/shaders/compute/pbr_lighting.comp.glsl"))
{
}

LightingPass::LightingPass(const vk::raii::Device& device,
                           const vk::raii::PhysicalDevice& physicalDevice,
                           uint32_t numSets,
                           std::unique_ptr<ComputeShader> shader)
	: ComputePass(device, physicalDevice,
	              MakeDescriptorLayout(device, *shader, {5, 6, 11}), numSets)
	, p_shader(std::move(shader))
{
	BuildPipeline(device, "LightingPass");

	// --- Create 1x1 black cubemap empty placeholder for IBL bindings when no env exists ---
	auto emptyImage = std::make_unique<Image>(
		device, physicalDevice,
		vk::Extent2D{1, 1},
		vk::Format::eR8G8B8A8Unorm,
		vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
		1, Image::ImageType::eCube,
		"LightingPass_EmptyCube");
		auto emptySampler = CreateSampler(device, physicalDevice);
	p_emptyCube = std::make_unique<Texture>(
			Texture::FromImage(std::move(emptyImage), std::move(emptySampler)));

	NEURUS_LOG("[LightingPass] numSets=" << numSets
	           << " shader=" << (p_shader ? "OK" : "FAIL"));

#ifdef _DEBUG
	for (uint32_t i = 0; i < numSets; ++i)
	{
		const std::string dsName = "LightingPass_Set" + std::to_string(i);
		p_descriptorSets[i].SetDebugName(dsName.c_str());
	}
#endif
}

LightingPass::~LightingPass() = default;

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

void LightingPass::BuildPipeline(const vk::raii::Device& device,
                                  const std::string& debugName)
{
	if (!p_shader)
	{
		throw std::runtime_error("LightingPass: Compute shader not loaded or invalid");
	}

	auto spv = ShaderLibrary::Compile(p_shader->GetStage(ShaderType::COMPUTE),
	                                  ShaderType::COMPUTE, debugName);
	vk::ShaderModuleCreateInfo smCI({}, spv);
	vk::raii::ShaderModule module(device, smCI);
	vk::PipelineShaderStageCreateInfo stageCI({}, vk::ShaderStageFlagBits::eCompute, *module, "main");

	vk::PushConstantRange pushRange = MakePushConstantRange(
		*p_shader, vk::ShaderStageFlagBits::eCompute);  // 176 bytes

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

PassStats LightingPass::Record(vk::CommandBuffer cmdBuf, const PassFrame& frame)
{
	PassStats stats{};

	const RenderFrameSnapshot& snapshot = frame.ctx.Frame();
	if (!snapshot.HasScene())
	{
		NEURUS_LOG("[LightingPass] No scene, skipping");
		return stats;
	}
	if (!snapshot.HasCamera())
	{
		NEURUS_LOG("[LightingPass] No camera in RenderContext, skipping");
		return stats;
	}

	const glm::vec3 cameraPos = snapshot.camera.position;
	const glm::mat4 viewMatrix = snapshot.camera.view;
	const glm::mat4 invProjView = glm::inverse(snapshot.camera.projection * snapshot.camera.view);
	const vk::Extent2D renderExtent{frame.ctx.width, frame.ctx.height};
	const uint32_t frameIndex = frame.frameIndex;

	Image* gPosition         = frame.resources.Input("gPosition");
	Image* gNormal           = frame.resources.Input("gNormal");
	Image* gAlbedo           = frame.resources.Input("gAlbedo");
	Image* gMetallicRoughness = frame.resources.Input("gMetallicRoughness");
	Image* ssao              = frame.resources.Input("U_AO");
	Image* shadowArray       = frame.resources.Input("U_ShadowArray");
	Image* hdrColor          = frame.resources.Output("outputImage");
	if (!gPosition || !gNormal || !gAlbedo || !gMetallicRoughness
	    || !ssao || !shadowArray || !hdrColor)
	{
		NEURUS_ERR("[LightingPass] graph did not provide the full input/output set");
		return stats;
	}

	// The graph writes the IBL descriptor before Record(); make sure the empty
	// fallback cube has been cleared and transitioned before it is sampled.
	bool needsDummy = true;
	if (snapshot.HasEnvironment())
	{
		const EnvironmentGPU* envGPU = frame.cache.GetEnvironmentGPU(snapshot.environmentId);
		needsDummy = !(envGPU && envGPU->diffuseTexture && envGPU->diffuseTexture->GetImage()
		               && envGPU->specularTexture && envGPU->specularTexture->GetImage());
	}
	if (needsDummy && !p_emptyInitialized)
	{
		Barrier::Transition(cmdBuf, *p_emptyCube->GetImage(), ImageState::TransferDst);

		vk::ClearColorValue black(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
		cmdBuf.clearColorImage(*p_emptyCube->GetImage()->ImageHandle(),
		                       vk::ImageLayout::eTransferDstOptimal,
		                       black,
		                       vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,
		                                                 0, 1, 0, 6));

		Barrier::Transition(cmdBuf, *p_emptyCube->GetImage(), ImageState::ColorShaderRead);
		p_emptyInitialized = true;
	}

	Barrier::Transition(cmdBuf, *gPosition,          ImageState::ColorShaderRead);
	Barrier::Transition(cmdBuf, *gNormal,            ImageState::ColorShaderRead);
	Barrier::Transition(cmdBuf, *gAlbedo,            ImageState::ColorShaderRead);
	Barrier::Transition(cmdBuf, *gMetallicRoughness, ImageState::ColorShaderRead);
	Barrier::Transition(cmdBuf, *hdrColor,           ImageState::ShaderWrite);

	// SSAO never written this frame: clear to 1.0 (no occlusion) first.
	if (ssao->State() == ImageState::Undefined)
	{
		Barrier::Transition(cmdBuf, *ssao, ImageState::TransferDst);

		vk::ClearColorValue clearWhite(std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f});
		cmdBuf.clearColorImage(*ssao->ImageHandle(),
		                       vk::ImageLayout::eTransferDstOptimal,
		                       clearWhite,
		                       vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,
		                                                 0, 1, 0, 1));
	}
	Barrier::Transition(cmdBuf, *ssao, ImageState::ColorShaderRead);

	// ShadowIntensity: transition the whole intensity array to ColorShaderRead.
	if (shadowArray->State() == ImageState::Undefined)
	{
		Barrier::Transition(cmdBuf, *shadowArray, ImageState::TransferDst);

		vk::ClearColorValue clearBlack(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
		for (uint32_t layer = 0; layer < RenderCache::MAX_SHADOW_LAYERS; ++layer)
		{
			cmdBuf.clearColorImage(*shadowArray->ImageHandle(),
			                       vk::ImageLayout::eTransferDstOptimal,
			                       clearBlack,
			                       vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,
			                                                 0, 1, layer, 1));
		}
	}
	Barrier::Transition(cmdBuf, *shadowArray, ImageState::ColorShaderRead);

	cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, *p_pipelines[0].pipeline);

	cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
	                          *p_pipelines[0].pipelineLayout,
	                          0,                                    // firstSet
	                          {p_descriptorSets[frameIndex].handle()},
	                          {});

	{
		LightingPushConstants pc = {};

		const auto* lightingCache = frame.cache.GetLightingCache();
		pc.lightCount     = lightingCache ? static_cast<int32_t>(lightingCache->GetPointLightCount()) : 0;
		pc.sunLightCount  = lightingCache ? static_cast<int32_t>(lightingCache->GetSunLightCount()) : 0;
		pc.spotLightCount = lightingCache ? static_cast<int32_t>(lightingCache->GetSpotLightCount()) : 0;

		pc.camX = cameraPos.x;
		pc.camY = cameraPos.y;
		pc.camZ = cameraPos.z;
		const float* vm = &viewMatrix[0][0];
		for (int i = 0; i < 16; ++i)
			pc.view[i] = vm[i];

		pc.iblEnabled   = snapshot.HasEnvironment() ? 1 : 0;
		pc.transEnabled = snapshot.Settings().r_transparent ? 1 : 0;

		const float* ipv = &invProjView[0][0];
		for (int i = 0; i < 16; ++i)
			pc.invProjView[i] = ipv[i];

		cmdBuf.pushConstants<LightingPushConstants>(
			*p_pipelines[0].pipelineLayout,
			vk::ShaderStageFlagBits::eCompute,
			0,
			pc);
	}

	const uint32_t groupCountX = (renderExtent.width  + 15) / 16;
	const uint32_t groupCountY = (renderExtent.height + 15) / 16;
	++stats.dispatches;
	cmdBuf.dispatch(groupCountX, groupCountY, 1);

	// Return HDRColor to a shader-readable layout for the later passes.
	Barrier::Transition(cmdBuf, *hdrColor, ImageState::ColorShaderRead);

	return stats;
}

void LightingPass::CollectGraphBuffers(RenderCache& cache, uint32_t frameIndex,
                                       std::vector<GraphBufferBinding>& out) const
{
	(void)frameIndex;

	const LightingCache* lighting = cache.GetLightingCache();
	if (!lighting)
		return;

	if (const GPUBuffer* point = lighting->GetPointLightSSBO())
		out.push_back({5, point->GetDescriptorInfo(), vk::DescriptorType::eStorageBuffer});
	if (const GPUBuffer* sun = lighting->GetSunLightSSBO())
		out.push_back({6, sun->GetDescriptorInfo(), vk::DescriptorType::eStorageBuffer});
	if (const GPUBuffer* spot = lighting->GetSpotLightSSBO())
		out.push_back({11, spot->GetDescriptorInfo(), vk::DescriptorType::eStorageBuffer});
}

void LightingPass::CollectGraphExternalImages(RenderCache& cache, const RenderContext& ctx,
                                              std::vector<GraphExternalImage>& out) const
{
	const RenderFrameSnapshot& frame = ctx.Frame();
	bool wroteIrradiance = false;
	bool wroteSpecular = false;

	if (frame.HasEnvironment())
	{
		const EnvironmentGPU* envGPU = cache.GetEnvironmentGPU(frame.environmentId);

		if (envGPU && envGPU->diffuseTexture && envGPU->diffuseTexture->GetImage())
		{
			out.push_back({
				8,
				vk::DescriptorImageInfo(*envGPU->diffuseTexture->GetSampler(),
				                        *envGPU->diffuseTexture->GetImage()->ImageViewHandle(),
				                        vk::ImageLayout::eShaderReadOnlyOptimal),
				vk::DescriptorType::eCombinedImageSampler,
			});
			wroteIrradiance = true;
		}

		if (envGPU && envGPU->specularTexture && envGPU->specularTexture->GetImage())
		{
			out.push_back({
				9,
				vk::DescriptorImageInfo(*envGPU->specularTexture->GetSampler(),
				                        *envGPU->specularTexture->GetImage()->ImageViewHandle(),
				                        vk::ImageLayout::eShaderReadOnlyOptimal),
				vk::DescriptorType::eCombinedImageSampler,
			});
			wroteSpecular = true;
		}
	}

	if (!wroteIrradiance || !wroteSpecular)
	{
		const vk::DescriptorImageInfo dummy(
			*p_emptyCube->GetSampler(),
			*p_emptyCube->GetImage()->ImageViewHandle(),
			vk::ImageLayout::eShaderReadOnlyOptimal);

		if (!wroteIrradiance)
			out.push_back({8, dummy, vk::DescriptorType::eCombinedImageSampler});
		if (!wroteSpecular)
			out.push_back({9, dummy, vk::DescriptorType::eCombinedImageSampler});
	}
}

PassIO LightingPass::GetIO() const
{
	PassIO io;
	io.name = "LightingPass";
	return io;
}

std::vector<const ShaderData*> LightingPass::GetShaders() const
{
	return {p_shader.get()};
}

std::vector<std::string> LightingPass::ExternalInputPorts() const
{
	std::vector<std::string> names;
	for (const std::string_view name : FindPassKind("LightingPass")->externalInputs)
		names.emplace_back(name);
	return names;
}

} // namespace neurus
