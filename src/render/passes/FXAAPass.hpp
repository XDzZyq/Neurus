#pragma once
#include "passes/ComputePass.hpp"
#include "../shaders/ShaderLibrary.hpp"
#include "../shaders/ComputeShader.hpp"
#include <vulkan/vulkan_raii.hpp>
#include <memory>

namespace neurus {
class RenderCache;

struct FXAAPushConstants {
	float rcpWidth, rcpHeight, pad0, pad1;
	float qualitySubpix, edgeThreshold, edgeThresholdMin, pad2;
};

class FXAAPass : public ComputePass {
public:
	FXAAPass(const vk::raii::Device& device,
	         const vk::raii::PhysicalDevice& physicalDevice,
	         uint32_t numSets);
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/// Declares the ComposedOutput read and FXAAOutput write.
	PassIO GetIO() const override;

	/// @brief The FXAA compute shader.
	std::vector<const ShaderData*> GetShaders() const override;
	void WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache) override;
private:
	/// @brief Delegating constructor that receives the already-loaded shader.
	FXAAPass(const vk::raii::Device& device,
	         const vk::raii::PhysicalDevice& physicalDevice,
	         uint32_t numSets,
	         std::unique_ptr<ComputeShader> shader);

	void BuildPipeline(const vk::raii::Device& device, const std::string& name) override;
	static vk::raii::Sampler CreateBilinearSampler(const vk::raii::Device& device);

	std::unique_ptr<ComputeShader> p_shader;
	vk::raii::Sampler              p_bilinearSampler = nullptr;
	bool                           p_hasBilinear     = false;
};
} // namespace neurus
