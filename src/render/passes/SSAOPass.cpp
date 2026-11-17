/**
 * @file SSAOPass.cpp
 * @brief Screen-Space Ambient Occlusion compute pass implementation.
 */

#include "RenderCache.hpp"
#include "passes/SSAOPass.hpp"

#include "../PipelineBuilder.hpp"
#include "DescriptorLayout.hpp"
#include "Image.hpp"
#include "render/Barrier.hpp"
#include "RenderContext.hpp"
#include "RenderFrameSnapshot.hpp"
#include "shaders/ShaderLibrary.hpp"
#include "shaders/ComputeShader.hpp"
#include "core/Log.hpp"

#include "scene/Camera.hpp"
#include "scene/Scene.hpp"

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
                   SharedQueue& graphicsQueue,
                   uint32_t queueFamilyIndex)
	: SSAOPass(device, physicalDevice, numSets, graphicsQueue, queueFamilyIndex,
	           ShaderLibrary::LoadComputeShader(
	               "ssao", "res/shaders/compute/ssao.comp.glsl"))
{
}

SSAOPass::SSAOPass(const vk::raii::Device& device,
                   const vk::raii::PhysicalDevice& physicalDevice,
                   uint32_t numSets,
                   SharedQueue& graphicsQueue,
                   uint32_t queueFamilyIndex,
                   std::unique_ptr<ComputeShader> shader)
	: ComputePass(device, physicalDevice, MakeDescriptorLayout(device, *shader), numSets)
	, p_shader(std::move(shader))
{
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

	// Samples get an increasing radius: scale = lerp(0.1, 1.0, t*t), which
	// clusters near samples for fine detail and spreads the far ones.
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
		// Random unit direction, used to rotate the tangent plane
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
// Pipeline creation
// ---------------------------------------------------------------------------

void SSAOPass::BuildPipeline(const vk::raii::Device& device,
                              const std::string& debugName)
{
	if (!p_shader)
	{
		throw std::runtime_error("SSAOPass: Compute shader not loaded or invalid");
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

void SSAOPass::CollectGraphBuffers(RenderCache& cache, uint32_t frameIndex,
                                   std::vector<GraphBufferBinding>& out) const
{
	(void)cache;
	(void)frameIndex;

	out.push_back({4, p_paramsUBO->GetDescriptorInfo(),
	               vk::DescriptorType::eUniformBuffer});
	out.push_back({5, p_noiseUBO->GetDescriptorInfo(),
	               vk::DescriptorType::eUniformBuffer});
}

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

PassStats SSAOPass::Record(vk::CommandBuffer cmdBuf, const PassFrame& frame)
{
	PassStats stats{};

	const vk::Extent2D renderExtent{frame.ctx.width, frame.ctx.height};
	const uint32_t    frameIndex   = frame.frameIndex;

	{
		SSAOParamsGpu params{};
		// The camera comes from the frame snapshot, not the Scene: the editor
		// publishes whichever camera the frame is rendered through.
		const RenderFrameSnapshot& snapshot = frame.ctx.Frame();
		if (!snapshot.HasCamera())
		{
			NEURUS_LOG("[SSAOPass] No camera in RenderContext, skipping");
			return stats;
		}
		const glm::mat4 viewProj = snapshot.camera.projection * snapshot.camera.view;
		const glm::mat4 view = snapshot.camera.view;
		const glm::vec3 cameraPos = snapshot.camera.position;

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

	Image* gPosition = frame.resources.Input("gPosition");
	Image* gNormal   = frame.resources.Input("gNormal");
	Image* gAlpha    = frame.resources.Input("gAlpha");
	Image* output    = frame.resources.Output("outputSSAO");
	if (!gPosition || !gNormal || !gAlpha || !output)
	{
		NEURUS_ERR("[SSAOPass] graph did not provide gPosition/gNormal/gAlpha/outputSSAO");
		return stats;
	}

	Barrier::Transition(cmdBuf, *gPosition, ImageState::ColorShaderRead);
	Barrier::Transition(cmdBuf, *gNormal,   ImageState::ColorShaderRead);
	Barrier::Transition(cmdBuf, *gAlpha,    ImageState::ColorShaderRead);
	Barrier::Transition(cmdBuf, *output,    ImageState::ShaderWrite);

	cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, *p_pipelines[0].pipeline);

	cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
	                          *p_pipelines[0].pipelineLayout,
	                          0,                                    // firstSet
	                          {p_descriptorSets[frameIndex].handle()},
	                          {});

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

	const uint32_t groupCountX = (renderExtent.width  + 15) / 16;
	const uint32_t groupCountY = (renderExtent.height + 15) / 16;
	++stats.dispatches;
	cmdBuf.dispatch(groupCountX, groupCountY, 1);

	// Return SSAO to ColorShaderRead for the lighting pass.
	Barrier::Transition(cmdBuf, *output, ImageState::ColorShaderRead);

	return stats;
}

PassIO SSAOPass::GetIO() const
{
	PassIO io;
	io.name = "SSAOPass";
	return io;
}

std::vector<const ShaderData*> SSAOPass::GetShaders() const
{
	return {p_shader.get()};
}

} // namespace neurus
