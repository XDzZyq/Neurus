/**
 * @file SSAOPass.cpp
 * @brief Screen-Space Ambient Occlusion compute pass implementation.
 */

#include "RenderCache.h"
#include "passes/SSAOPass.h"

#include "../PipelineBuilder.h"
#include "Image.h"
#include "render/Barrier.h"
#include "RenderContext.h"
#include "shaders/ShaderLibrary.h"
#include "shaders/ComputeShader.h"
#include "core/Log.h"

#include "scene/Camera.h"
#include "scene/Scene.h"

#include <array>
#include <cstring>
#include <stdexcept>
#include <string>

namespace neurus {

// ---------------------------------------------------------------------------
// Random number helpers (inline, local to this TU)
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief Simple deterministic random number generator (xorshift32).
 *
 * Used to produce reproducible kernel samples and noise vectors
 * so that reference-image regression tests are deterministic.
 */
class DeterministicRNG
{
public:
	explicit DeterministicRNG(uint32_t seed = 0xDEADBEEF) : rng_state(seed) {}

	float rand01()
	{
		rng_state ^= rng_state << 13;
		rng_state ^= rng_state >> 17;
		rng_state ^= rng_state << 5;
		return static_cast<float>(rng_state) / static_cast<float>(UINT32_MAX);
	}

	float rand11()
	{
		return rand01() * 2.0f - 1.0f;
	}

	glm::vec3 rand3n()
	{
		return glm::normalize(glm::vec3(rand11(), rand11(), rand11()));
	}

	/**
	 * @brief Random unit hemisphere direction biased toward the normal (z > 0).
	 */
	glm::vec3 rand3nh()
	{
		glm::vec3 v(rand11(), rand11(), rand11());
		v.z = v.z * 0.5f + 0.5f;  // bias z toward [0, 1]
		return glm::normalize(v);
	}

private:
	uint32_t rng_state;
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SSAOPass::SSAOPass(const vk::raii::Device& device,
                   const vk::raii::PhysicalDevice& physicalDevice,
                   uint32_t numSets,
                   vk::Queue graphicsQueue,
                   uint32_t queueFamilyIndex)
	: ComputePass(device, physicalDevice,
	              SSAOPass::CreateDescriptorSetLayout(device), numSets)
	// --- Self-load compute shader via ShaderLibrary ---
	, p_shader(
		ShaderLibrary::LoadComputeShader("ssao",
		                                  "res/shaders/compute/ssao.comp"))
{
	// --- Create pipeline from self-loaded shader ---
	BuildPipeline(device, "SSAOPass");

	NEURUS_LOG("[SSAOPass] numSets=" << numSets
	           << " kernelLength=" << kDefaultKernelLength
	           << " qfi=" << queueFamilyIndex
	           << " shader=" << (p_shader ? "OK" : "FAIL"));

	// --- Generate and upload kernel + initial camera data ---
	{
		p_kernelSamples = GenerateKernel();
		SSAOParamsGpu initialParams = {};
		for (size_t i = 0; i < kMaxKernelSamples; ++i)
		{
			initialParams.kernelSamples[i] = p_kernelSamples[i];
		}

		p_paramsUBO = std::make_unique<UniformBuffer<SSAOParamsGpu>>(
			device, physicalDevice, "SSAOParamsUBO");
		p_paramsUBO->Upload(initialParams);

		NEURUS_LOG("[SSAOPass] Created params UBO (" << sizeof(SSAOParamsGpu) << " bytes, "
		           << kMaxKernelSamples << " kernel samples)");
	}

	// --- Generate and upload noise rotation vectors ---
	{
		const auto noise = GenerateNoise();

		p_noiseUBO = std::make_unique<GPUBuffer>(
			device, physicalDevice,
			graphicsQueue, queueFamilyIndex,
			sizeof(noise),
			vk::BufferUsageFlagBits::eUniformBuffer,
			"SSAONoiseUBO");
		p_noiseUBO->Upload(noise.data(), sizeof(noise));

		NEURUS_LOG("[SSAOPass] Created noise UBO (" << sizeof(noise) << " bytes, "
		           << kNoiseEntryCount << " entries)");
	}

#ifdef _DEBUG
	for (uint32_t i = 0; i < numSets; ++i)
	{
		const std::string dsName = "SSAOPass_Set" + std::to_string(i);
		p_descriptorSets[i].SetDebugName(dsName.c_str());
	}
#endif
}

// ---------------------------------------------------------------------------

std::array<KernelSampleGpu, SSAOPass::kMaxKernelSamples> SSAOPass::GenerateKernel()
{
	DeterministicRNG rng(0xDEADBEEF);
	std::array<KernelSampleGpu, kMaxKernelSamples> kernel;

	// Generate hemisphere samples with increasing radius:
	//   scale = lerp(0.1, 1.0, (i / (N-1))²)
	// This clusters samples near the centre for fine detail and spreads
	// far samples for large-scale occlusion.
	for (uint32_t i = 0; i < kMaxKernelSamples; ++i)
	{
		const float t = static_cast<float>(i) / static_cast<float>(kMaxKernelSamples - 1);
		const float scale = 0.1f + 0.9f * t * t;

		const glm::vec3 dir = scale * rng.rand3nh();

		kernel[i].x   = dir.x;
		kernel[i].y   = dir.y;
		kernel[i].z   = dir.z;
		kernel[i]._pad = 0.0f;
	}

	NEURUS_LOG("[SSAOPass] Generated " << kMaxKernelSamples << " hemisphere kernel samples");
	return kernel;
}

// ---------------------------------------------------------------------------
// Noise generation
// ---------------------------------------------------------------------------

std::array<NoiseEntryGpu, SSAOPass::kNoiseEntryCount> SSAOPass::GenerateNoise()
{
	DeterministicRNG rng(0xCAFEBABE);
	std::array<NoiseEntryGpu, kNoiseEntryCount> noise;

	for (uint32_t i = 0; i < kNoiseEntryCount; ++i)
	{
		// Random unit direction — used to rotate the tangent plane
		const glm::vec3 dir = rng.rand3n();

		noise[i].x   = dir.x;
		noise[i].y   = dir.y;
		noise[i].z   = dir.z;
		noise[i]._pad = 0.0f;
	}

	NEURUS_LOG("[SSAOPass] Generated " << kNoiseEntryCount << " noise rotation entries");
	return noise;
}

// ---------------------------------------------------------------------------
// Descriptor set layout
// ---------------------------------------------------------------------------

DescriptorSetLayout SSAOPass::CreateDescriptorSetLayout(const vk::raii::Device& device)
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
		// SSAO output (storage image)
		.AddBinding(3,
		            vk::DescriptorType::eStorageImage,
		            vk::ShaderStageFlagBits::eCompute)
		// SSAO params UBO (camera + kernel)
		.AddBinding(4,
		            vk::DescriptorType::eUniformBuffer,
		            vk::ShaderStageFlagBits::eCompute)
		// Noise UBO
		.AddBinding(5,
		            vk::DescriptorType::eUniformBuffer,
		            vk::ShaderStageFlagBits::eCompute)
		.Build(device);
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

void SSAOPass::BuildPipeline(const vk::raii::Device& device,
                              const std::string& debugName)
{
	// --- Guard: shader must be valid ---
	if (!p_shader)
	{
		throw std::runtime_error("SSAOPass: Compute shader not loaded or invalid");
	}

	// --- Compile and create temporary shader module ---
	auto spv = ShaderLibrary::Compile(p_shader->GetStage(ShaderType::COMPUTE),
	                                  ShaderType::COMPUTE, debugName);
	vk::ShaderModuleCreateInfo smCI({}, spv);
	vk::raii::ShaderModule module(device, smCI);
	vk::PipelineShaderStageCreateInfo stageCI({}, vk::ShaderStageFlagBits::eCompute, *module, "main");

	// --- Push constant range (4 ints = 16 bytes) ---
	vk::PushConstantRange pushRange(
		vk::ShaderStageFlagBits::eCompute,
		0,
		4 * sizeof(int32_t));  // kernelLength, radius (float), noiseSize, frameIndex

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

void SSAOPass::WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache)
{
	DescriptorSet& dstSet = p_descriptorSets[setIndex];

	// --- Write G-Buffer input descriptors (combined image samplers) ---
	{
		const std::array<AttachmentName, 3> gBufferInputs = {
			AttachmentName::Position,
			AttachmentName::Normal,
			AttachmentName::Albedo,   // alpha stored in albedo.a or separate?
		};

		for (uint32_t i = 0; i < 3; ++i)
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
	}

	// --- Write SSAO output (storage image) ---
	{
		const auto& ssaoAtt = cache.GetAttachment(AttachmentName::SSAO, extent);

		vk::DescriptorImageInfo imageInfo(
			nullptr,                              // sampler (not used for storage images)
			*ssaoAtt.ImageViewHandle(),           // imageView
			vk::ImageLayout::eGeneral             // imageLayout
		);

		dstSet.WriteImage(3, imageInfo,
		                  vk::DescriptorType::eStorageImage);
	}

	// --- Write SSAO params UBO ---
	{
		dstSet.WriteBuffer(4, p_paramsUBO->GetDescriptorInfo(),
		                   vk::DescriptorType::eUniformBuffer);
	}

	// --- Write noise UBO ---
	{
		dstSet.WriteBuffer(5, p_noiseUBO->GetDescriptorInfo(),
		                   vk::DescriptorType::eUniformBuffer);
	}
}

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

PassStats SSAOPass::Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx)
{
	PassStats stats{};

	const vk::Extent2D renderExtent{ctx.width, ctx.height};
	const uint32_t    frameIndex   = ctx.frameIndex;

	// --- Cast scene UID to Scene* for access to Scene-specific members ---
	// (Nothing here needs the Scene any more: the camera arrives through the
	// context. Kept out rather than kept unused.)

	// --- 0. Update per-frame SSAO params UBO (camera matrices + kernel) ---
	{
		SSAOParamsGpu params{};
		// The viewing camera comes from the context, not from the Scene: the Editor
		// injects whichever camera the frame is being rendered through. See the
		// fuller note in LightingPass::Record().
		const Camera* cam = ctx.editor.camera;
		if (!cam)
		{
			NEURUS_LOG("[SSAOPass] No camera in EditorContext, skipping");
			return stats;
		}
		const glm::mat4 viewProj = cam->GetProjectionMatrix() * cam->GetViewMatrix();
		const glm::mat4 view = cam->GetViewMatrix();
		const glm::vec3 cameraPos = cam->GetPosition();

		const float* vp = &viewProj[0][0];
		for (int i = 0; i < 16; ++i) params.viewProj[i] = vp[i];
		const float* vm = &view[0][0];
		for (int i = 0; i < 16; ++i) params.view[i] = vm[i];
		params.camX = cameraPos.x;
		params.camY = cameraPos.y;
		params.camZ = cameraPos.z;
		params.camW = 0.0f;
		for (size_t i = 0; i < kMaxKernelSamples; ++i)
			params.kernelSamples[i] = p_kernelSamples[i];

		p_paramsUBO->Upload(params);
	}

	// --- 1. Write descriptor set for this frame slot ---
	WriteDescriptors(frameIndex, renderExtent, cache);

	// --- 2. Transition G-Buffer images to ShaderRead and SSAO attachment to ShaderWrite ---
	{
		const std::array<AttachmentName, 3> gBufferInputs = {
			AttachmentName::Position,
			AttachmentName::Normal,
			AttachmentName::Albedo,
		};

		for (size_t i = 0; i < 3; ++i)
		{
			auto& attachment = cache.GetAttachment(gBufferInputs[i], renderExtent);
			Barrier::Transition(cmdBuf, attachment, ImageState::ColorShaderRead);
		}

		// SSAO attachment: current state → ShaderWrite (compute write)
		auto& ssaoAtt = cache.GetAttachment(AttachmentName::SSAO, renderExtent);
		Barrier::Transition(cmdBuf, ssaoAtt, ImageState::ShaderWrite);
	}

	// --- 3. Bind compute pipeline ---
	cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, *p_pipelines[0].pipeline);

	// --- 4. Bind descriptor set ---
	cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
	                          *p_pipelines[0].pipelineLayout,
	                          0,                                    // firstSet
	                          {p_descriptorSets[frameIndex].handle()},
	                          {});

	// --- 5. Push constants ---
	{
		struct SSAOPushConstants
		{
			int32_t kernelLength;
			float   radius;
			int32_t noiseSize;
			int32_t frameIndex;
		};

		SSAOPushConstants pc = {};
		pc.kernelLength = p_kernelLength;
		pc.radius       = p_radius;
		pc.noiseSize    = p_noiseSize;
		pc.frameIndex   = static_cast<int32_t>(frameIndex);

		cmdBuf.pushConstants<SSAOPushConstants>(
			*p_pipelines[0].pipelineLayout,
			vk::ShaderStageFlagBits::eCompute,
			0,
			pc);
	}

	// --- 6. Dispatch ---
	const uint32_t groupCountX = (renderExtent.width  + 15) / 16;
	const uint32_t groupCountY = (renderExtent.height + 15) / 16;
	++stats.dispatches;
	cmdBuf.dispatch(groupCountX, groupCountY, 1);

	// --- 7. Transition SSAO output: General → ShaderRead for lighting pass ---
	{
		auto& ssaoAtt = cache.GetAttachment(AttachmentName::SSAO, renderExtent);
		Barrier::Transition(cmdBuf, ssaoAtt, ImageState::ColorShaderRead);
	}

	return stats;
}

PassIO SSAOPass::GetIO() const
{
	// Binding metadata is indicative only: SSAOPass still writes its own
	// descriptors in WriteDescriptors(). Resource names drive RenderGraph
	// edge wiring (Position/Normal/Albedo are external inputs from
	// GeometryPass; SSAO feeds LightingPass).
	PassIO io;
	io.name  = "SSAOPass";
	io.reads = {
		{AttachmentName::Position, 0, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
		{AttachmentName::Normal,   1, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
		{AttachmentName::Albedo,   2, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
	};
	io.writes = {
		{AttachmentName::SSAO, 3, vk::DescriptorType::eStorageImage, vk::ImageLayout::eGeneral},
	};
	return io;
}

} // namespace neurus
