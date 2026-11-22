#pragma once

#include "render/resources/ShaderGPU.hpp"
#include "passes/ComputePass.hpp"
#include <vulkan/vulkan_raii.hpp>
#include <memory>

namespace neurus {
class RenderCache;

struct FXAAPushConstants {
	float rcpWidth, rcpHeight, pad0, pad1;
	float qualitySubpix, edgeThreshold, edgeThresholdMin;
};
static_assert(sizeof(FXAAPushConstants) == 28,
              "FXAAPushConstants must match the GLSL push_constant block");

class FXAAPass : public ComputePass {
public:
	FXAAPass(RenderCache& cache, const vk::raii::Device& device,
	         const vk::raii::PhysicalDevice& physicalDevice,
	         uint32_t numSets);
	PassStats Record(vk::CommandBuffer cmdBuf, const PassFrame& frame) override;

	/// Declares the ComposedOutput read and FXAAOutput write.
	PassIO GetIO() const override;

	/// @brief The FXAA compute shader.
	std::vector<const ShaderGPU*> GetShaders() const override;
public:
	/// @brief Constructs from a selected uploaded shader.
	FXAAPass(const vk::raii::Device& device,
	         const vk::raii::PhysicalDevice& physicalDevice,
	         uint32_t numSets,
	         std::shared_ptr<const ShaderGPU> shader);

private:

	void BuildPipeline(const vk::raii::Device& device, const std::string& name) override;
	static vk::raii::Sampler CreateBilinearSampler(const vk::raii::Device& device);

	std::shared_ptr<const ShaderGPU> p_shader;
	vk::raii::Sampler              p_bilinearSampler = nullptr;
	bool                           p_hasBilinear     = false;
};
} // namespace neurus
