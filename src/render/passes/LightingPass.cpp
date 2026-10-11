/**
 * @file LightingPass.cpp
 * @brief PBR lighting pass implementation.
 */

#include "passes/LightingPass.h"

#include "../PipelineBuilder.h"
#include "RenderCache.h"
#include "RenderContext.h"
#include "Image.h"
#include "render/Barrier.h"
#include "core/RenderConfig.h"
#include "shaders/ShaderLibrary.h"
#include "shaders/ComputeShader.h"
#include "Texture.h"

#include "core/Log.h"

#include "scene/Light.h"
#include "scene/Scene.h"

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
	: ComputePass(device, physicalDevice,
	              LightingPass::CreateDescriptorSetLayout(device), numSets)
	// --- Self-load compute shader via ShaderLibrary ---
	, p_shader(
		ShaderLibrary::LoadComputeShader("pbr_lighting",
		                                  "res/shaders/compute/pbr_lighting.comp"))
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
// Descriptor set layout
// ---------------------------------------------------------------------------

DescriptorSetLayout LightingPass::CreateDescriptorSetLayout(const vk::raii::Device& device)
{
	return BuildLayout()
		// G-Buffer inputs (combined image samplers)
		.AddBinding(0,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		.AddBinding(1,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		.AddBinding(2,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		.AddBinding(3,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// Output HDR colour (storage image)
		.AddBinding(4,
		            vk::DescriptorType::eStorageImage,
		            vk::ShaderStageFlagBits::eCompute)
		// Light SSBO (PARTIALLY_BOUND - valid to skip update when no lights)
		.AddBindingWithFlags(5,
		                     vk::DescriptorType::eStorageBuffer,
		                     vk::ShaderStageFlagBits::eCompute,
		                     vk::DescriptorBindingFlagBits::ePartiallyBound)
		// SunLight SSBO (PARTIALLY_BOUND - valid to skip update when no sun lights)
		.AddBindingWithFlags(6,
		                     vk::DescriptorType::eStorageBuffer,
		                     vk::ShaderStageFlagBits::eCompute,
		                     vk::DescriptorBindingFlagBits::ePartiallyBound)
		// SSAO occlusion input (combined image sampler)
		.AddBinding(7,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// IBL diffuse irradiance cubemap (combined image sampler)
		.AddBinding(8,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// IBL specular prefiltered cubemap (combined image sampler)
		.AddBinding(9,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// Shadow array (sampler2DArray, single layered image)
		.AddBinding(10,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// SpotLight SSBO (PARTIALLY_BOUND - valid to skip update when no spot lights)
		.AddBindingWithFlags(11,
		                     vk::DescriptorType::eStorageBuffer,
		                     vk::ShaderStageFlagBits::eCompute,
		                     vk::DescriptorBindingFlagBits::ePartiallyBound)
		.Build(device);
}

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

	vk::PushConstantRange pushRange(
		vk::ShaderStageFlagBits::eCompute,
		0,
		sizeof(LightingPushConstants));  // 176 bytes

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

void LightingPass::WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache)
{
	DescriptorSet& dstSet = p_descriptorSets[setIndex];

	const std::array<AttachmentName, 4> gBufferInputs = {
		AttachmentName::Position,
		AttachmentName::Normal,
		AttachmentName::Albedo,
		AttachmentName::MetallicRoughness,
	};

	for (uint32_t i = 0; i < 4; ++i)
	{
		const auto& attachment = cache.GetAttachment(gBufferInputs[i], extent);

		vk::DescriptorImageInfo imageInfo(
			*p_sampler,                              // sampler
			*attachment.ImageViewHandle(),           // imageView
			vk::ImageLayout::eShaderReadOnlyOptimal  // imageLayout
		);

		dstSet.WriteImage(i, imageInfo,
		                  vk::DescriptorType::eCombinedImageSampler);
	}

	// --- Write HDR colour output (storage image) ---
	{
		const auto& hdrColor = cache.GetAttachment(AttachmentName::HDRColor, extent);

		vk::DescriptorImageInfo imageInfo(
			nullptr,                              // sampler (not used for storage images)
			*hdrColor.ImageViewHandle(),          // imageView
			vk::ImageLayout::eGeneral             // imageLayout
		);

		dstSet.WriteImage(4, imageInfo,
		                  vk::DescriptorType::eStorageImage);
	}

	// --- Write point light SSBO ---
	{
		const auto* lightingCache = cache.GetLightingCache();
		const GPUBuffer* pointSSBO = lightingCache ? lightingCache->GetPointLightSSBO() : nullptr;

		if (pointSSBO)
		{
			dstSet.WriteBuffer(5, pointSSBO->GetDescriptorInfo(),
			                   vk::DescriptorType::eStorageBuffer);
		}
		// When pointSSBO is nullptr, binding 5 is left un-updated.
		// PARTIALLY_BOUND flag makes this safe because lightCount=0
		// guarantees the shader never accesses binding 5.
	}

	// --- Write sun light SSBO ---
	{
		const auto* lightingCache = cache.GetLightingCache();
		const GPUBuffer* sunSSBO = lightingCache ? lightingCache->GetSunLightSSBO() : nullptr;

		if (sunSSBO)
		{
			dstSet.WriteBuffer(6, sunSSBO->GetDescriptorInfo(),
			                   vk::DescriptorType::eStorageBuffer);
		}
		// When sunSSBO is nullptr, PARTIALLY_BOUND makes this safe
		// because sunLightCount=0 guarantees the shader never accesses binding 6.
	}

	// --- Write spot light SSBO ---
	{
		const auto* lightingCache = cache.GetLightingCache();
		const GPUBuffer* spotSSBO = lightingCache ? lightingCache->GetSpotLightSSBO() : nullptr;

		if (spotSSBO)
		{
			dstSet.WriteBuffer(11, spotSSBO->GetDescriptorInfo(),
			                   vk::DescriptorType::eStorageBuffer);
		}
		// When spotSSBO is nullptr, PARTIALLY_BOUND makes this safe
		// because spotLightCount=0 guarantees the shader never accesses binding 11.
	}

	// --- Write SSAO attachment (combined image sampler) ---
	{
		const auto& ssao = cache.GetAttachment(AttachmentName::SSAO, extent);

		vk::DescriptorImageInfo imageInfo(
			*p_sampler,                              // sampler
			*ssao.ImageViewHandle(),                 // imageView
			vk::ImageLayout::eShaderReadOnlyOptimal  // imageLayout
		);

		dstSet.WriteImage(7, imageInfo,
		                  vk::DescriptorType::eCombinedImageSampler);
	}

	// --- Write shadow intensity array (binding 10, sampler2DArray) ---
	//     Single layered image - no per-layer dummy images needed.
	{
		auto& shadowArray = cache.GetShadowIntensityArray(extent);

		vk::DescriptorImageInfo imageInfo(
			*p_sampler,
			*shadowArray.ImageViewHandle(),           // 2D_ARRAY view
			vk::ImageLayout::eShaderReadOnlyOptimal);

		dstSet.WriteImage(10, imageInfo,
		                  vk::DescriptorType::eCombinedImageSampler);
	}
}

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

PassStats LightingPass::Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx)
{
	PassStats stats{};

	// A frame recorded before a scene exists is legal, and this pass
	// dereferences `scene` below (light_list, env_list), so the check belongs
	// here rather than at each use.
	if (!ctx.HasScene())
	{
		NEURUS_LOG("[LightingPass] No scene, skipping");
		return stats;
	}
	const Scene* scene = ctx.scene;

	// The viewing camera comes from the context, never from the Scene: the
	// editor injects the camera it wants rendered. Re-deriving it here would
	// split this pass's invProjView from the CameraGPU the vertex stage used.
	const Camera* cam = ctx.camera;
	if (!cam)
	{
		NEURUS_LOG("[LightingPass] No camera in RenderContext, skipping");
		return stats;
	}
	const glm::vec3 cameraPos = cam->GetPosition();
	const glm::mat4 viewMatrix = cam->GetViewMatrix();
	const glm::mat4 invProjView = glm::inverse(cam->GetProjectionMatrix() * cam->GetViewMatrix());
	const vk::Extent2D renderExtent{ctx.width, ctx.height};
	const uint32_t frameIndex = ctx.frameIndex;

	WriteDescriptors(frameIndex, renderExtent, cache);

	// IBL cubemap descriptors (bindings 8-9).
	{
		DescriptorSet& dstSet = p_descriptorSets[frameIndex];
		const bool hasEnv = !scene->env_list.empty();

		// Try to write real environment cubemap descriptors
		bool wroteIrradiance = false;
		bool wroteSpecular = false;

		if (hasEnv)
		{
			auto& env = scene->env_list.begin()->second;
			const EnvironmentGPU* envGPU = cache.GetEnvironmentGPU(env->GetObjectID());

			if (envGPU && envGPU->diffuseTexture && envGPU->diffuseTexture->GetImage())
			{
				vk::DescriptorImageInfo irrInfo(
					*envGPU->diffuseTexture->GetSampler(),
					*envGPU->diffuseTexture->GetImage()->ImageViewHandle(),
					vk::ImageLayout::eShaderReadOnlyOptimal);
				dstSet.WriteImage(8, irrInfo, vk::DescriptorType::eCombinedImageSampler);
				wroteIrradiance = true;
			}

			if (envGPU && envGPU->specularTexture && envGPU->specularTexture->GetImage())
			{
				vk::DescriptorImageInfo specInfo(
					*envGPU->specularTexture->GetSampler(),
					*envGPU->specularTexture->GetImage()->ImageViewHandle(),
					vk::ImageLayout::eShaderReadOnlyOptimal);
				dstSet.WriteImage(9, specInfo, vk::DescriptorType::eCombinedImageSampler);
				wroteSpecular = true;
			}
		}

		// If either IBL binding was not written (no env, or its GPU data is
		// missing), bind the empty placeholder cube instead.
		if (!wroteIrradiance || !wroteSpecular)
		{
			if (!p_emptyInitialized)
			{
				Barrier::Transition(cmdBuf, *p_emptyCube->GetImage(), ImageState::TransferDst);

				vk::ClearColorValue black(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
				cmdBuf.clearColorImage(*p_emptyCube->GetImage()->ImageHandle(), vk::ImageLayout::eTransferDstOptimal,
				                       black, vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 6));

				Barrier::Transition(cmdBuf, *p_emptyCube->GetImage(), ImageState::ColorShaderRead);
				p_emptyInitialized = true;
			}

			vk::DescriptorImageInfo dummyInfo(*p_emptyCube->GetSampler(), *p_emptyCube->GetImage()->ImageViewHandle(),
			                                   vk::ImageLayout::eShaderReadOnlyOptimal);
			if (!wroteIrradiance)
				dstSet.WriteImage(8, dummyInfo, vk::DescriptorType::eCombinedImageSampler);
			if (!wroteSpecular)
				dstSet.WriteImage(9, dummyInfo, vk::DescriptorType::eCombinedImageSampler);
		}
	}

	// Transition all images for compute shader access.
	{
		const std::array<AttachmentName, 4> gBufferInputs = {
			AttachmentName::Position,
			AttachmentName::Normal,
			AttachmentName::Albedo,
			AttachmentName::MetallicRoughness,
		};

		for (size_t i = 0; i < 4; ++i)
		{
			auto& attachment = cache.GetAttachment(gBufferInputs[i], renderExtent);
			Barrier::Transition(cmdBuf, attachment, ImageState::ColorShaderRead);
		}

		auto& hdrColor = cache.GetAttachment(AttachmentName::HDRColor, renderExtent);
		Barrier::Transition(cmdBuf, hdrColor, ImageState::ShaderWrite);

		// SSAO never written this frame: clear to 1.0 (no occlusion) first.
		auto& ssao = cache.GetAttachment(AttachmentName::SSAO, renderExtent);
		if (ssao.State() == ImageState::Undefined)
		{
			Barrier::Transition(cmdBuf, ssao, ImageState::TransferDst);

			vk::ClearColorValue clearWhite(std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f});
			cmdBuf.clearColorImage(*ssao.ImageHandle(),
			                       vk::ImageLayout::eTransferDstOptimal,
			                       clearWhite,
			                       vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,
			                                                  0, 1, 0, 1));
		}
		Barrier::Transition(cmdBuf, ssao, ImageState::ColorShaderRead);

		// ShadowIntensity: transition the whole intensity array to ColorShaderRead.
		{
			auto& shadowArray = cache.GetShadowIntensityArray(renderExtent);
			if (shadowArray.State() == ImageState::Undefined)
			{
				Barrier::Transition(cmdBuf, shadowArray, ImageState::TransferDst);

				vk::ClearColorValue clearBlack(std::array<float, 4>{0.0f, 0.0f, 0.0f, 0.0f});
				for (uint32_t layer = 0; layer < RenderCache::MAX_SHADOW_LAYERS; ++layer)
				{
					cmdBuf.clearColorImage(*shadowArray.ImageHandle(),
					                       vk::ImageLayout::eTransferDstOptimal,
					                       clearBlack,
					                       vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor,
					                                                  0, 1, layer, 1));
				}
			}
			Barrier::Transition(cmdBuf, shadowArray, ImageState::ColorShaderRead);
		}
	}

	cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, *p_pipelines[0].pipeline);

	cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
	                          *p_pipelines[0].pipelineLayout,
	                          0,                                    // firstSet
	                          {p_descriptorSets[frameIndex].handle()},
	                          {});

	{
		LightingPushConstants pc = {};

		const auto* lightingCache = cache.GetLightingCache();
		pc.lightCount    = lightingCache ? static_cast<int32_t>(lightingCache->GetPointLightCount()) : 0;
		pc.sunLightCount = lightingCache ? static_cast<int32_t>(lightingCache->GetSunLightCount()) : 0;
		pc.spotLightCount = lightingCache ? static_cast<int32_t>(lightingCache->GetSpotLightCount()) : 0;

		pc.camX = cameraPos.x;
		pc.camY = cameraPos.y;
		pc.camZ = cameraPos.z;
		// Copy view matrix (column-major, same as GLSL)
		const float* vm = &viewMatrix[0][0];
		for (int i = 0; i < 16; ++i)
		{
			pc.view[i] = vm[i];
		}

		pc.iblEnabled = !scene->env_list.empty() ? 1 : 0;

		// Transparent background (checkerboard).
		pc.transEnabled = ctx.Settings().r_transparent ? 1 : 0;

		// Copy inverse(proj * view) matrix for skybox background ray
		const float* ipv = &invProjView[0][0];
		for (int i = 0; i < 16; ++i)
		{
			pc.invProjView[i] = ipv[i];
		}

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
	{
		auto& hdrColor = cache.GetAttachment(AttachmentName::HDRColor, renderExtent);
		Barrier::Transition(cmdBuf, hdrColor, ImageState::ColorShaderRead);
	}

	return stats;
}

PassIO LightingPass::GetIO() const
{
	// Binding metadata is indicative only: LightingPass still writes its own
	// descriptors. Resource names drive the RenderGraph edges.
	PassIO io;
	io.name  = "LightingPass";
	io.reads = {
		{AttachmentName::Position,          0, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
		{AttachmentName::Normal,            1, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
		{AttachmentName::Albedo,            2, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
		{AttachmentName::MetallicRoughness, 3, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
		{AttachmentName::SSAO,              4, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
		// Establishes the ShadowIntensity to Lighting ordering edge.
		{AttachmentName::ShadowIntensity,   5, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
	};
	io.writes = {
		{AttachmentName::HDRColor, 6, vk::DescriptorType::eStorageImage, vk::ImageLayout::eGeneral},
	};
	return io;
}

} // namespace neurus
