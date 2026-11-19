#pragma once
#include "passes/ComputePass.hpp"
#include "shader/ShaderLibrary.hpp"
#include "shader/ComputeShader.hpp"
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
	FXAAPass(const vk::raii::Device& device,
	         const vk::raii::PhysicalDevice& physicalDevice,
	         uint32_t numSets);
	PassStats Record(vk::CommandBuffer cmdBuf, const PassFrame& frame) override;

	/// Declares the ComposedOutput read and FXAAOutput write.
	PassIO GetIO() const override;

	/// @brief The FXAA compute shader.
	std::vector<const ShaderData*> GetShaders() const override;
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
