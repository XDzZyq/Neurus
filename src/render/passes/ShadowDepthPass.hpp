/**
 * @file ShadowDepthPass.hpp
 * @brief Point-light shadow depth cubemap pass and sun-light orthographic shadow depth pass.
 *
 * Handles both point light cubemap shadow depth (multiview, SSBO) and
 * sun light orthographic shadow depth (single-view, depth-only).
 */

#pragma once

#include "passes/Pass.hpp"
#include "render_api/RenderContext.hpp"
#include "../Image.hpp"
#include "../DescriptorManager.hpp"
#include "../buffers/BufferLayout.hpp"
#include "../buffers/GPUBuffer.hpp"

#include <string>

#include <vulkan/vulkan_raii.hpp>
#include <glm/glm.hpp>
#include <memory>
#include <vector>

namespace neurus {

class RenderShader;
class ShaderData;
class SharedQueue;

class ShadowDepthPass : public Pass
{
public:
	static constexpr uint32_t kDefaultResolution = 1024;
	static constexpr uint32_t kSunResolution = 2048;
	static constexpr uint32_t kShadowFaceCount = 6;

	// Fixed far plane for the static projection matrix (must be >> any light's farPlane
	// to avoid clipping; the actual depth normalisation uses the per-light farPlane push
	// constant through gl_FragDepth override).
	static constexpr float kStaticFarPlane = 1000.0f;

	// 6 mat4 face view-projection matrices - 384 bytes in std430
	static constexpr vk::DeviceSize kFaceVPSize = kShadowFaceCount * sizeof(glm::mat4);

	// Push constant layout (80 bytes total):
	//   offset 0:  vec3 lightWorldPos  (12 bytes)
	//   offset 12: float farPlane       (4 bytes)
	//   offset 16: mat4  model          (64 bytes)
	static constexpr vk::DeviceSize kLightPushSize = 16;   // lightWorldPos + farPlane
	static constexpr vk::DeviceSize kModelPushOffset = 16; // model starts here
	static constexpr vk::DeviceSize kTotalPushSize = 80;   // 16 + 64

	struct LightPushData { float lpx, lpy, lpz; float farPlane; };
	static_assert(sizeof(LightPushData) == kLightPushSize,
	              "LightPushData must be exactly 16 bytes");

	ShadowDepthPass(const vk::raii::Device& device,
	                const vk::raii::PhysicalDevice& physicalDevice,
	                SharedQueue& graphicsQueue,
	                uint32_t queueFamilyIndex,
	                uint32_t resolution = kDefaultResolution);

	~ShadowDepthPass() override = default;
	ShadowDepthPass(const ShadowDepthPass&) = delete;
	ShadowDepthPass& operator=(const ShadowDepthPass&) = delete;
	ShadowDepthPass(ShadowDepthPass&&) noexcept = default;
	ShadowDepthPass& operator=(ShadowDepthPass&&) noexcept = default;

	using Pass::Record;
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& /*cache*/, const RenderContext& ctx) override;

	/// Declares the ShadowDepth bundle write; the per-light maps are addressed
	/// internally and appear as a single ordering token.
	PassIO GetIO() const override;

	/// @brief The multiview cubemap and sun 2D depth shaders.
	std::vector<const ShaderData*> GetShaders() const override;

	/// @brief The per-light shadow bundle, a logical ordering token.
	std::vector<PassPort> DeclaredOutputs() const override;

	/// @brief The face VP SSBO set, written by the graph.
	DescriptorSet* GraphDescriptorSet(uint32_t frameIndex) override
	{
		(void)frameIndex;
		return p_ssboSet.get();
	}

	/// @brief The face VP matrices at binding 0.
	void CollectGraphBuffers(RenderCache& cache, uint32_t frameIndex,
	                         std::vector<GraphBufferBinding>& out) const override;

	// --- Accessors ---
	uint32_t Resolution() const { return p_resolution; }

private:
	void createSSBOResources(const vk::raii::Device& device,
	                         const vk::raii::PhysicalDevice& physicalDevice,
	                         SharedQueue& queue, uint32_t qfi);
	void BuildPipeline(const vk::raii::Device& device,
	                   const std::string& debugName) override;

	// --- Parameters ---
	uint32_t p_resolution;
	SharedQueue* p_queue = nullptr;       ///< Graphics queue (for MeshGPU staging uploads)
	uint32_t p_queueFamilyIndex = 0;      ///< Queue family index (for MeshGPU staging uploads)

	// --- GPU resources ---
	// Static SSBO with 6 face VP matrices (computed once from origin; never changes)
	std::unique_ptr<GPUBuffer> p_faceVPs;
	DescriptorSetLayout p_ssboLayout;
	DescriptorPool p_ssboPool;
	std::unique_ptr<DescriptorSet> p_ssboSet;
	BufferLayout p_vtxLayout;

	// --- Shaders (self-loaded via ShaderLibrary) ---
	std::shared_ptr<const RenderShader> p_multiviewShader;  ///< Point-light cubemap depth+colour (multiview)
	std::shared_ptr<const RenderShader> p_sunShader;         ///< Sun-light orthographic depth-only
	// (Pipelines inherited from Pass - p_pipelines[0]=multiview, p_pipelines[1]=sun)

};

} // namespace neurus
