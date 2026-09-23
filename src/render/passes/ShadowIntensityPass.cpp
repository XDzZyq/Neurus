/**
 * @file ShadowIntensityPass.cpp
 * @brief Per-pixel point-light shadow intensity compute pass implementation.
 */

#include "RenderCache.h"
#include "passes/ShadowIntensityPass.h"
#include "render/RenderConfig.h"

#include "../PipelineBuilder.h"
#include "Image.h"
#include "render/Barrier.h"
#include "RenderContext.h"
#include "core/Log.h"

#include "scene/Light.h"
#include "scene/Scene.h"

#include "render/TemporalAccumulator.h"
#include "render/HaltonSequence.h"

#include "shaders/ComputeShader.h"
#include "shaders/ShaderLibrary.h"

#include <glm/gtc/matrix_transform.hpp>

#include <stdexcept>
#include <string>

namespace neurus {

// ---------------------------------------------------------------------------
// Push constant structs — compile-time verified
// ---------------------------------------------------------------------------

namespace {

// Point light shadow eval push constants (48 bytes).
// Must match shadow_eval.comp layout exactly.
struct ShadowEvalPushConstants
{
    float lightPosX, lightPosY, lightPosZ;  // 12 bytes
    float farPlane;                          //  4 bytes
    float bias;                              //  4 bytes
    int32_t layerIndex;                      //  4 bytes
    float jitterX, jitterY, jitterZ;        // 12 bytes
    float lightRadius;                       //  4 bytes
    float alpha;                             //  4 bytes
    int32_t frameCount;                      //  4 bytes
};  // Total: 48 bytes
static_assert(sizeof(ShadowEvalPushConstants) >= 44,
              "ShadowEvalPushConstants must be at least 44 bytes");

// Sun shadow eval push constants (96 bytes C++ sizeof).
// GLSL layout: mat4(64B) + float(4B) + int(4B) + float(4B)*3 + float(4B) + float(4B) + int(4B) = 96B
// C++ sizeof: 96B due to glm::mat4 16-byte alignment.
struct SunShadowEvalPushConstants
{
    glm::mat4 lightViewProj;   // offset 0,  64 bytes
    float     bias;            // offset 64, 4 bytes
    int32_t   layerIndex;      // offset 68, 4 bytes
    float     jitterX;         // offset 72, 4 bytes
    float     jitterY;         // offset 76, 4 bytes
    float     jitterZ;         // offset 80, 4 bytes
    float     lightRadius;     // offset 84, 4 bytes
    float     alpha;           // offset 88, 4 bytes
    int32_t   frameCount;      // offset 92, 4 bytes
};  // Total: 96 bytes

static_assert(sizeof(SunShadowEvalPushConstants) >= 96,
              "SunShadowEvalPushConstants must be at least 96 bytes");
static_assert(sizeof(SunShadowEvalPushConstants::lightViewProj) == 64,
              "glm::mat4 size mismatch");
static_assert(offsetof(SunShadowEvalPushConstants, bias) == 64,
              "bias offset mismatch");
static_assert(offsetof(SunShadowEvalPushConstants, layerIndex) == 68,
              "layerIndex offset mismatch");
static_assert(offsetof(SunShadowEvalPushConstants, jitterX) == 72,
              "jitterX offset mismatch");
static_assert(offsetof(SunShadowEvalPushConstants, jitterY) == 76,
              "jitterY offset mismatch");
static_assert(offsetof(SunShadowEvalPushConstants, jitterZ) == 80,
              "jitterZ offset mismatch");
static_assert(offsetof(SunShadowEvalPushConstants, lightRadius) == 84,
              "lightRadius offset mismatch");
static_assert(offsetof(SunShadowEvalPushConstants, alpha) == 88,
              "alpha offset mismatch");
static_assert(offsetof(SunShadowEvalPushConstants, frameCount) == 92,
              "frameCount offset mismatch");

} // anon

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

ShadowIntensityPass::ShadowIntensityPass(const vk::raii::Device& device,
                                         const vk::raii::PhysicalDevice& physicalDevice,
                                         uint32_t numSets)
	: ComputePass(device, physicalDevice,
	              // Allocate kSetsPerFrameSlot descriptor sets per in-flight
	              // frame (one per shadow-casting light) so the per-light loop
	              // never updates a set still bound from an earlier light — see
	              // the static_assert below tying this to MAX_SHADOW_LAYERS.
	              ShadowIntensityPass::CreateDescriptorSetLayout(device),
	              numSets * kSetsPerFrameSlot)
	, p_sunDescSetLayout(CreateSunDescriptorSetLayout(device))
{
	// Every shadow-casting light needs its own descriptor set per frame; the
	// per-light loop selects (frameIndex * kSetsPerFrameSlot + lightIndex).
	// Keep the pool sized to the shadow-layer cap so no two active lights ever
	// share a set within a single command buffer (which would alias descriptors
	// and drop a light's shadow).
	static_assert(kSetsPerFrameSlot == RenderCache::MAX_SHADOW_LAYERS,
	              "kSetsPerFrameSlot must match RenderCache::MAX_SHADOW_LAYERS");
	// --- Load point-light cubemap eval shader via ShaderLibrary ---
	p_pointLightShader = ShaderLibrary::LoadComputeShader(
		"shadow_eval", "res/shaders/compute/shadow_eval.comp");

	if (!p_pointLightShader)
	{
		throw std::runtime_error("[ShadowIntensityPass] Failed to load 'shadow_eval' compute shader");
	}

	// --- Load sun-light 2D eval shader via ShaderLibrary ---
	p_sunLightShader = ShaderLibrary::LoadComputeShader(
		"sun_shadow_eval", "res/shaders/compute/sun_shadow_eval.comp");

	if (!p_sunLightShader)
	{
		throw std::runtime_error("[ShadowIntensityPass] Failed to load 'sun_shadow_eval' compute shader");
	}

	NEURUS_LOG("[ShadowIntensityPass] numSets=" << numSets
	           << " farPlane=" << Light::point_shadow_far);

	// --- Build both pipelines (cubemap + sun) ---
	BuildPipeline(device, "ShadowIntensityPass");

	const uint32_t sunSetCount = numSets * kSetsPerFrameSlot;

	// --- Sun shadow sampler (clamp-to-border, black border for out-of-bounds UV) ---
	p_sunShadowSampler = CreateSunShadowSampler(device, physicalDevice);

	// --- Sun descriptor pool + sets ---
	p_sunDescPool = DescriptorPool(device,
	                               sunSetCount,
	                               DescriptorPool::CalculatePoolSizes({&p_sunDescSetLayout}, sunSetCount));
	p_sunDescSets = p_sunDescPool.Allocate(p_sunDescSetLayout, sunSetCount);

	NEURUS_LOG("[ShadowIntensityPass] Sun pipeline created, "
	           << sunSetCount << " sun descriptor sets allocated");

#ifdef _DEBUG
	for (uint32_t i = 0; i < numSets * kSetsPerFrameSlot; ++i)
	{
		const std::string dsName = "ShadowIntensityPass_Set" + std::to_string(i);
		p_descriptorSets[i].SetDebugName(dsName.c_str());
	}
	for (uint32_t i = 0; i < sunSetCount; ++i)
	{
		const std::string dsName = "ShadowIntensityPass_SunSet" + std::to_string(i);
		p_sunDescSets[i].SetDebugName(dsName.c_str());
	}
#endif
}

// ---------------------------------------------------------------------------
// Descriptor set layout
// ---------------------------------------------------------------------------

DescriptorSetLayout ShadowIntensityPass::CreateDescriptorSetLayout(const vk::raii::Device& device)
{
	return BuildLayout()
		// Binding 0: G-Buffer world-space position (combined image sampler)
		.AddBinding(0,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// Binding 1: Shadow depth cubemap (combined image sampler, samplerCube)
		.AddBinding(1,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// Binding 2: Shadow intensity output (storage image, R8)
		.AddBinding(2,
		            vk::DescriptorType::eStorageImage,
		            vk::ShaderStageFlagBits::eCompute)
		.Build(device);
}

// ---------------------------------------------------------------------------
// Sun descriptor set layout (identical bindings types, but for sun 2D path)
// ---------------------------------------------------------------------------

DescriptorSetLayout ShadowIntensityPass::CreateSunDescriptorSetLayout(const vk::raii::Device& device)
{
	return BuildLayout()
		// Binding 0: G-Buffer world-space position (combined image sampler)
		.AddBinding(0,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// Binding 1: Sun shadow depth map 2D (combined image sampler, sampler2D)
		.AddBinding(1,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// Binding 2: Shadow intensity output (storage image, R8)
		.AddBinding(2,
		            vk::DescriptorType::eStorageImage,
		            vk::ShaderStageFlagBits::eCompute)
		.Build(device);
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

void ShadowIntensityPass::BuildPipeline(const vk::raii::Device& device,
                                        const std::string& debugName)
{
	// --- Point-light cubemap pipeline ---
	{
		if (!p_pointLightShader)
		{
			throw std::runtime_error("ShadowIntensityPass: Point-light shader not loaded or invalid");
		}

		auto spv = ShaderLibrary::Compile(p_pointLightShader->GetStage(ShaderType::COMPUTE),
		                                  ShaderType::COMPUTE, debugName);
		vk::ShaderModuleCreateInfo smCI({}, spv);
		vk::raii::ShaderModule module(device, smCI);
		vk::PipelineShaderStageCreateInfo stageCI({}, vk::ShaderStageFlagBits::eCompute, *module, "main");

		vk::PushConstantRange pushRange(
			vk::ShaderStageFlagBits::eCompute,
			0,
			sizeof(ShadowEvalPushConstants));   // 40 bytes (was 24)

		PipelineBuilder builder;
		p_pipelines.push_back(
			builder.AddShaderStage(stageCI)
				.SetDebugName(debugName.c_str())
				.AddDescriptorSetLayout(*p_descriptorSetLayout.layout())
				.AddPushConstantRange(pushRange)
				.BuildComputePipeline(device));
	}

	// --- Sun-light 2D pipeline ---
	{
		if (!p_sunLightShader)
		{
			throw std::runtime_error("ShadowIntensityPass: Sun-light shader not loaded or invalid");
		}

		auto spv = ShaderLibrary::Compile(p_sunLightShader->GetStage(ShaderType::COMPUTE),
		                                  ShaderType::COMPUTE, debugName + "_Sun");
		vk::ShaderModuleCreateInfo smCI2({}, spv);
		vk::raii::ShaderModule module2(device, smCI2);
		vk::PipelineShaderStageCreateInfo stageCI2({}, vk::ShaderStageFlagBits::eCompute, *module2, "main");

		vk::PushConstantRange pushRange(
			vk::ShaderStageFlagBits::eCompute,
			0,
			sizeof(SunShadowEvalPushConstants));   // 96 bytes (was 80)

		PipelineBuilder sunBuilder;
		p_pipelines.push_back(
			sunBuilder.AddShaderStage(stageCI2)
				.SetDebugName((debugName + "_Sun").c_str())
				.AddDescriptorSetLayout(*p_sunDescSetLayout.layout())
				.AddPushConstantRange(pushRange)
				.BuildComputePipeline(device));
	}
}

// ---------------------------------------------------------------------------
// Sun shadow sampler (clamp-to-border, black = unshadowed)
// ---------------------------------------------------------------------------

vk::raii::Sampler ShadowIntensityPass::CreateSunShadowSampler(
	const vk::raii::Device& device,
	const vk::raii::PhysicalDevice& /*physicalDevice*/)
{
	vk::SamplerCreateInfo samplerCI(
		{},                                        // flags
		vk::Filter::eNearest,                      // magFilter
		vk::Filter::eNearest,                      // minFilter
		vk::SamplerMipmapMode::eNearest,            // mipmapMode
		vk::SamplerAddressMode::eClampToBorder,     // addressModeU
		vk::SamplerAddressMode::eClampToBorder,     // addressModeV
		vk::SamplerAddressMode::eClampToBorder,     // addressModeW
		0.0f,                                       // mipLodBias
		VK_FALSE,                                   // anisotropyEnable
		0.0f,                                       // maxAnisotropy
		VK_FALSE,                                   // compareEnable
		vk::CompareOp::eAlways,                     // compareOp
		0.0f,                                       // minLod
		0.0f,                                       // maxLod
		vk::BorderColor::eFloatOpaqueBlack,         // borderColor (0.0 = unshadowed)
		VK_FALSE                                    // unnormalizedCoordinates
	);

	return vk::raii::Sampler(device, samplerCI);
}

// ---------------------------------------------------------------------------
// Descriptor writes
// ---------------------------------------------------------------------------

void ShadowIntensityPass::WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache)
{
	DescriptorSet& dstSet = p_descriptorSets[setIndex];

	// --- Binding 0: G-Buffer world-space position (combined image sampler) ---
	{
		const auto& posAtt = cache.GetAttachment(AttachmentName::Position, extent);

		vk::DescriptorImageInfo imageInfo(
			*p_sampler,                              // sampler
			*posAtt.ImageViewHandle(),               // imageView
			vk::ImageLayout::eShaderReadOnlyOptimal  // imageLayout
		);

		dstSet.WriteImage(0, imageInfo,
		                  vk::DescriptorType::eCombinedImageSampler);
	}

	// --- Binding 1: Shadow depth cubemap (combined image sampler, samplerCube) ---
	{
		LightGPU* lgpu = cache.GetLightGPU(p_currentLightUID);
		if (!lgpu || !lgpu->shadowDepthMap) return;
		auto& shadowCube = *lgpu->shadowDepthMap;

		vk::DescriptorImageInfo imageInfo(
			*p_sampler,                                     // sampler
			*shadowCube.ImageViewHandle(),                  // imageView (cube type, depth aspect)
			vk::ImageLayout::eDepthStencilReadOnlyOptimal   // imageLayout (matches DepthShaderRead)
		);

		dstSet.WriteImage(1, imageInfo,
		                  vk::DescriptorType::eCombinedImageSampler);
	}

	// --- Binding 2: Shadow intensity output (storage image, R8, 2D_ARRAY) ---
	{
		auto& shadowIntensity = cache.GetShadowIntensityArray(extent);

		vk::DescriptorImageInfo imageInfo(
			nullptr,                               // sampler (not used for storage images)
			*shadowIntensity.ImageViewHandle(),    // imageView (2D_ARRAY)
			vk::ImageLayout::eGeneral              // imageLayout
		);

		dstSet.WriteImage(2, imageInfo,
		                  vk::DescriptorType::eStorageImage);
	}
}

// ---------------------------------------------------------------------------
// Sun descriptor writes (2D shadow map at binding 1)
// ---------------------------------------------------------------------------

void ShadowIntensityPass::WriteSunDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache)
{
	DescriptorSet& dstSet = p_sunDescSets[setIndex];

	// --- Binding 0: G-Buffer world-space position (combined image sampler) ---
	{
		const auto& posAtt = cache.GetAttachment(AttachmentName::Position, extent);

		vk::DescriptorImageInfo imageInfo(
			*p_sampler,                              // sampler (nearest, clamp-to-edge)
			*posAtt.ImageViewHandle(),               // imageView
			vk::ImageLayout::eShaderReadOnlyOptimal  // imageLayout
		);

		dstSet.WriteImage(0, imageInfo,
		                  vk::DescriptorType::eCombinedImageSampler);
	}

	// --- Binding 1: Sun shadow depth map (combined image sampler, sampler2D) ---
	{
		LightGPU* slgpu = cache.GetLightGPU(p_currentLightUID);
		if (!slgpu || !slgpu->shadowDepthMap) return;
		auto& sunShadowMap = *slgpu->shadowDepthMap;

		vk::DescriptorImageInfo imageInfo(
			*p_sunShadowSampler,                             // sampler (nearest, clamp-to-border, black)
			*sunShadowMap.ImageViewHandle(),                 // imageView (2D, depth aspect)
			vk::ImageLayout::eDepthStencilReadOnlyOptimal   // imageLayout (matches DepthShaderRead)
		);

		dstSet.WriteImage(1, imageInfo,
		                  vk::DescriptorType::eCombinedImageSampler);
	}

	// --- Binding 2: Shadow intensity output (storage image, R8, 2D_ARRAY) ---
	{
		auto& shadowIntensity = cache.GetShadowIntensityArray(extent);

		vk::DescriptorImageInfo imageInfo(
			nullptr,                               // sampler (not used for storage images)
			*shadowIntensity.ImageViewHandle(),    // imageView (2D_ARRAY)
			vk::ImageLayout::eGeneral              // imageLayout
		);

		dstSet.WriteImage(2, imageInfo,
		                  vk::DescriptorType::eStorageImage);
	}
}

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

PassStats ShadowIntensityPass::Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx)
{
	PassStats stats{};

	const vk::Extent2D renderExtent{ctx.width, ctx.height};
	const uint32_t    frameIndex   = ctx.frameIndex;

	// --- Early out: no scene ---
	if (!ctx.editor.scene)
	{
		NEURUS_LOG("[ShadowIntensityPass] No scene, skipping");
		return stats;
	}

	// --- Cast scene UID to Scene* for access to Scene-specific members ---
	const auto* scene = static_cast<const Scene*>(ctx.editor.scene);
	const auto* config = static_cast<const RenderConfig*>(ctx.editor.config);
	const float shadowBias = config ? config->r_shadow_bias : 0.0005f;

	{
		int shadowCount = 0;
		for (const auto& [uid, light] : scene->light_list)
		{
			if (light && light->use_shadow && light->is_viewport && light->is_rendered)
			{
				auto pos = light->GetPosition();
				shadowCount++;
			}
		}
	}

	// --- 1. Transition G-Buffer Position to ColorShaderRead (once for all lights) ---
	{
		auto& posAtt = cache.GetAttachment(AttachmentName::Position, renderExtent);
		Barrier::Transition(cmdBuf, posAtt, ImageState::ColorShaderRead);
	}

	// --- 2. Transition the shadow intensity array to ShaderWrite (once) ---
	{
		auto& shadowArray = cache.GetShadowIntensityArray(renderExtent);
		Barrier::Transition(cmdBuf, shadowArray, ImageState::ShaderWrite);
	}

	// --- 3. Bind compute pipeline (once for all lights) ---
	cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, *p_pipelines[0].pipeline);

	// --- 4. Dispatch shadow evaluation for each shadow-casting light ---
	//     Alternates between two descriptor sets per frame slot so that updating
	//     the descriptor for light N never touches the set that is still bound
	//     from light N-1.  Without this alternation, updating a bound descriptor
	//     set invalidates the command buffer (VUID-00059 chain).
	uint32_t lightIndex = 0;
	for (const auto& [uid, light] : scene->light_list)
	{
		// Skip non-shadow-casting, invisible, and non-cubemap lights (sun lights handled separately)
		if (!light || !light->use_shadow) continue;
		if (!light->is_viewport || !light->is_rendered) continue;
		if (light->light_type != LightType::POINTLIGHT &&
		    light->light_type != LightType::SPOTLIGHT) continue;

		p_currentLightUID = uid;

		// --- Transition shadow depth cubemap: post-ShadowDepthPass → DepthShaderRead ---
		{
			LightGPU* lgpu = cache.GetLightGPU(uid);
			if (!lgpu || !lgpu->shadowDepthMap) continue;
			auto& shadowCube = *lgpu->shadowDepthMap;
			Barrier::Transition(cmdBuf, shadowCube, ImageState::DepthShaderRead);
		}

		// --- Select descriptor set: alternate between 2 sets per frame slot ---
		const uint32_t setIdx = frameIndex * kSetsPerFrameSlot + (lightIndex % kSetsPerFrameSlot);

		// --- Write descriptor set ---
		WriteDescriptors(setIdx, renderExtent, cache);

		// --- Bind descriptor set ---
		cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
		                          *p_pipelines[0].pipelineLayout,
		                          0,
		                          {p_descriptorSets[setIdx].handle()},
		                          {});

		// --- Push constants ---
		{
			const auto& pos = light->GetPosition();
			const uint32_t layer = cache.GetShadowIntensityLayer(uid, renderExtent);

			// Read alpha mode from config
			ShadowAlphaMode alphaMode = ShadowAlphaMode::MovingAvg;
			if (ctx.editor.config)
			{
				const auto* cfg = static_cast<const RenderConfig*>(ctx.editor.config);
				alphaMode = static_cast<ShadowAlphaMode>(cfg->r_sampling_mode);
			}

			ShadowEvalPushConstants pc = {};
			pc.lightPosX   = pos.x;
			pc.lightPosY   = pos.y;
			pc.lightPosZ   = pos.z;
			pc.farPlane    = (light->light_type == LightType::SPOTLIGHT)
				? Light::spot_shadow_far
				: Light::point_shadow_far;
			pc.bias        = shadowBias;
			pc.layerIndex  = static_cast<int32_t>(layer);
			pc.jitterX     = ctx.jitter.x;
			pc.jitterY     = ctx.jitter.y;
			pc.jitterZ     = ctx.jitter.z;
			pc.lightRadius = light->light_radius;
			pc.alpha       = ComputeShadowAlpha(alphaMode, ctx.iteration);
			pc.frameCount  = static_cast<int32_t>(ctx.iteration);

			cmdBuf.pushConstants<ShadowEvalPushConstants>(
				*p_pipelines[0].pipelineLayout,
				vk::ShaderStageFlagBits::eCompute,
				0,
				pc);
		}

		// --- Dispatch ---
		const uint32_t groupCountX = (renderExtent.width  + 15) / 16;
		const uint32_t groupCountY = (renderExtent.height + 15) / 16;
		++stats.dispatches;
		cmdBuf.dispatch(groupCountX, groupCountY, 1);

		++lightIndex;
	}

	// --- 4b. Dispatch sun (directional) shadow evaluation for each shadow-casting sun light ---
	//        Uses a separate pipeline and descriptor sets for the 2D shadow path.
	uint32_t sunLightIndex = 0;
	for (const auto& [uid, light] : scene->light_list)
	{
		// Only process visible sun lights that cast shadows
		if (!light || !light->use_shadow) continue;
		if (!light->is_viewport || !light->is_rendered) continue;
		if (light->light_type != LightType::SUNLIGHT) continue;

		p_currentLightUID = uid;

		// --- Transition sun shadow depth map: post-ShadowDepthPass → DepthShaderRead ---
		{
			LightGPU* slgpu = cache.GetLightGPU(uid);
			if (!slgpu || !slgpu->shadowDepthMap) continue;
			auto& sunShadowMap = *slgpu->shadowDepthMap;
			Barrier::Transition(cmdBuf, sunShadowMap, ImageState::DepthShaderRead);
		}

		// --- Select sun descriptor set: alternate between 2 sets per frame slot ---
		const uint32_t setIdx = frameIndex * kSetsPerFrameSlot + (sunLightIndex % kSetsPerFrameSlot);

		// --- Write sun descriptor set ---
		WriteSunDescriptors(setIdx, renderExtent, cache);

		// --- Bind sun pipeline + descriptor set ---
		cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, *p_pipelines[1].pipeline);
		cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
		                          *p_pipelines[1].pipelineLayout,
		                          0,
		                          {p_sunDescSets[setIdx].handle()},
		                          {});

		// --- Sun push constants (mat4 lightViewProj + bias + layerIndex + jitter + alpha + frameCount) ---
		{
			const glm::vec3 lightDir = glm::normalize(light->GetDirection());
			const float field = Light::sun_shadow_field;
			const float nearPlane = Light::sun_shadow_near;
			const float farPlane = Light::sun_shadow_far;

			// Orthographic projection covering ±field around the origin
			const glm::mat4 lightProj = glm::ortho(-field, field, -field, field, nearPlane, farPlane);

			// View matrix: match ShadowDepthPass sun-path convention.
			constexpr glm::vec3 kWorldUp(0.0f, 0.0f, 1.0f);
			constexpr glm::vec3 kAltUp(1.0f, 0.0f, 0.0f);
			const glm::vec3 up = (glm::abs(glm::dot(lightDir, kWorldUp)) > 0.999f)
				? kAltUp : kWorldUp;

			// Use camera target as ortho centre (same as ShadowDepthPass).
			// Still reads the Scene rather than ctx.editor.camera - see the full note
			// in LightingPass::Record(). These four pass-local reads move together,
			// in one commit, with the viewport-owned free camera.
			const Camera* activeCam = scene->GetActiveCamera();
			const glm::vec3 center = activeCam->cam_tar;
			const glm::vec3 lightEye = center - lightDir * Light::sun_depth_range;
			const glm::mat4 lightView = glm::lookAt(lightEye, center, up);
			const glm::mat4 lightViewProj = lightProj * lightView;

			const uint32_t layer = cache.GetShadowIntensityLayer(uid, renderExtent);

			// Read alpha mode from config
			ShadowAlphaMode alphaMode = ShadowAlphaMode::MovingAvg;
			if (ctx.editor.config)
			{
				const auto* cfg = static_cast<const RenderConfig*>(ctx.editor.config);
				alphaMode = static_cast<ShadowAlphaMode>(cfg->r_sampling_mode);
			}

			SunShadowEvalPushConstants pc = {};
			pc.lightViewProj = lightViewProj;
			pc.bias          = shadowBias;
			pc.layerIndex    = static_cast<int32_t>(layer);
			pc.jitterX       = ctx.jitter.x;
			pc.jitterY       = ctx.jitter.y;
			pc.jitterZ       = ctx.jitter.z;
			pc.lightRadius   = light->light_radius / (2.0f * Light::sun_shadow_field);
			pc.alpha         = ComputeShadowAlpha(alphaMode, ctx.iteration);
			pc.frameCount    = static_cast<int32_t>(ctx.iteration);

			cmdBuf.pushConstants<SunShadowEvalPushConstants>(
				*p_pipelines[1].pipelineLayout,
				vk::ShaderStageFlagBits::eCompute,
				0,
				pc);
		}

		// --- Dispatch ---
		const uint32_t groupCountX = (renderExtent.width  + 15) / 16;
		const uint32_t groupCountY = (renderExtent.height + 15) / 16;
		++stats.dispatches;
		cmdBuf.dispatch(groupCountX, groupCountY, 1);

		++sunLightIndex;
	}

	// --- 5. Transition shadow intensity array: General → ColorShaderRead for lighting pass ---
	{
		auto& shadowArray = cache.GetShadowIntensityArray(renderExtent);
		Barrier::Transition(cmdBuf, shadowArray, ImageState::ColorShaderRead);
	}

	return stats;
}

PassIO ShadowIntensityPass::GetIO() const
{
	// Reads the G-Buffer Position and the per-light shadow depth maps (the
	// logical ShadowDepth bundle); writes the unified shadow-intensity 2D
	// array. Per-light maps and intensity layers are addressed internally via
	// LightGPU / GetShadowIntensityArray, so binding metadata is indicative.
	PassIO io;
	io.name  = "ShadowIntensityPass";
	io.reads = {
		{ AttachmentName::Position },
		{ AttachmentName::ShadowDepth },
	};
	io.writes = {
		{ AttachmentName::ShadowIntensity },
	};
	return io;
}

} // namespace neurus
