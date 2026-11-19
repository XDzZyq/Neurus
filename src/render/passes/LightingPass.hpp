/**
 * @file LightingPass.hpp
 * @brief PBR lighting pass - compute shader reading the G-Buffer and
 *        evaluating the Cook-Torrance GGX BRDF per light with IBL support.
 *
 * Reads the G-Buffer attachments and evaluates direct PBR lighting plus IBL
 * ambient into a single HDR colour attachment, dispatched at 16x16 thread
 * groups.
 *
 * @note Descriptor set layout: 12 bindings (G-Buffer, storage output, light
 *       SSBOs, SSAO, IBL cubemaps, shadow array).
 */

#pragma once

#include "passes/ComputePass.hpp"
#include "../DescriptorManager.hpp"
#include "../buffers/GPUBuffer.hpp"
#include "shader/ShaderLibrary.hpp"
#include "shader/ComputeShader.hpp"

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <memory>
#include <unordered_map>

namespace neurus {

// --- Forward declarations ---
class RenderCache;
class PipelineBuilder;
class Texture;

// ---------------------------------------------------------------------------
// LightingPass
// ---------------------------------------------------------------------------

/**
 * @brief PBR lighting compute pass.
 *
 * Reads the G-Buffer (Position, Normal, Albedo, MetallicRoughness) as combined
 * image samplers, dispatches the PBR lighting compute shader, and writes HDR
 * colour to the output image. Light SSBOs live in the cache, not here.
 *
 * Non-copyable, movable.
 */
class LightingPass : public ComputePass
{
public:
	/**
	 * @brief Constructs the lighting pass and creates all GPU resources.
	 *
	 * @param device            Logical device (retained reference).
	 * @param physicalDevice    Physical device (for sampler creation).
	 * @param numSets           Number of descriptor sets to allocate (one per
	 *                          in-flight frame). Must match kMaxFramesInFlight
	 *                          in the renderer.
	 *
	 * @throws std::runtime_error if shader or pipeline creation fails.
	 */
	LightingPass(const vk::raii::Device& device,
	             const vk::raii::PhysicalDevice& physicalDevice,
	             uint32_t numSets);

	~LightingPass() override;

	LightingPass(LightingPass&&) noexcept = default;
	LightingPass& operator=(LightingPass&&) noexcept = default;

	/// @brief Maximum number of shadow-casting lights (sampler2DArray layers).
	static constexpr uint32_t MAX_SHADOW_LIGHTS = 4;

	// -------------------------------------------------------------------
	// Recording
	// -------------------------------------------------------------------

	/**
	 * @brief Records the PBR lighting compute dispatch into a command buffer.
	 *
	 * Transitions the G-Buffer, SSAO and shadow-intensity inputs to
	 * ColorShaderRead and HDRColor to ShaderWrite, writes this frame's
	 * descriptors, pushes light counts and camera data, dispatches
	 * ceil(w/16) x ceil(h/16) groups, then returns HDRColor to ColorShaderRead.
	 *
	 * @param cmdBuf          Command buffer in recording state.
	 * @param ctx             Per-frame context (camera position, view matrix,
	 *                        invProjView, render extent, frame index).
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, const PassFrame& frame) override;

	/// Declares the G-Buffer, SSAO and ShadowIntensity reads and the HDRColor
	/// write. (IBL cubemaps are not modeled as bindings here.)
	PassIO GetIO() const override;

	/// @brief The PBR lighting compute shader.
	std::vector<const ShaderData*> GetShaders() const override;

	/// @brief IBL cubemaps, which are produced outside this graph.
	std::vector<std::string> ExternalInputPorts() const override;

	/// @brief Light SSBOs, written by the graph into bindings 5, 6 and 11.
	void CollectGraphBuffers(RenderCache& cache, uint32_t frameIndex,
	                         std::vector<GraphBufferBinding>& out) const override;

	/// @brief IBL cubemaps (bindings 8 and 9), resolved from the environment.
	void CollectGraphExternalImages(RenderCache& cache, const RenderContext& ctx,
	                                std::vector<GraphExternalImage>& out) const override;

private:
	/// @brief Delegating constructor that receives the already-loaded shader.
	LightingPass(const vk::raii::Device& device,
	             const vk::raii::PhysicalDevice& physicalDevice,
	             uint32_t numSets,
	             std::shared_ptr<const ComputeShader> shader);

	/** @brief Creates the compute pipeline from the self-loaded shader. */
	void BuildPipeline(const vk::raii::Device& device,
	                   const std::string& debugName) override;

	// (Pipelines inherited from Pass - p_pipelines[0])

	// --- Self-loaded compute shader (via ShaderLibrary) ---
	std::shared_ptr<const ComputeShader> p_shader;

	// --- Empty cubemap placeholder for IBL bindings when no env exists (1x1x6, black) ---
	std::unique_ptr<Texture> p_emptyCube;
	bool p_emptyInitialized = false;
};
} // namespace neurus
