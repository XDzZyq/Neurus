/**
 * @file ComposePass.h
 * @brief Final scene compositing compute pass - tone maps HDRColor.
 *
 * Reads the HDRColor attachment as a combined image sampler, applies gamma
 * correction via push constant, and writes the final composed output to the
 * ComposedOutput attachment (RGBA16F storage image).
 *
 * The selection highlight is not part of this pass: it is blended by the
 * overlay engine, after tone mapping and gamma, so the outline is never touched
 * by FXAA and can be redrawn without re-running the scene graph. Compose is the
 * last scene pass and knows nothing about overlays.
 *
 * @note No UBOs or per-frame uploads - all data is read from attachments.
 *       The only push constant is a single float (gamma).
 */

#pragma once

#include "passes/ComputePass.h"
#include "../shaders/ShaderLibrary.h"
#include "../shaders/ComputeShader.h"

#include <vulkan/vulkan_raii.hpp>

#include <memory>

namespace neurus {

// --- Forward declarations ---
class RenderCache;

// ---------------------------------------------------------------------------
// ComposePass
// ---------------------------------------------------------------------------

/**
 * @brief Final scene compositing compute pass.
 *
 * Reads HDRColor (RGBA16F) as a combined image sampler, applies gamma
 * correction, and writes to ComposedOutput (RGBA16F storage image).
 */
class ComposePass : public ComputePass
{
public:
	/**
	 * @brief Constructs the ComposePass and creates all GPU resources.
	 *
	 * @param device            Logical device (retained reference).
	 * @param physicalDevice    Physical device (for sampler creation).
	 * @param numSets           Number of descriptor sets (one per in-flight frame).
	 */
	ComposePass(const vk::raii::Device& device,
	            const vk::raii::PhysicalDevice& physicalDevice,
	            uint32_t numSets);

	// -------------------------------------------------------------------
	// Recording
	// -------------------------------------------------------------------

	/**
	 * @brief Records the compose compute dispatch into a command buffer.
	 *
	 * Writes the descriptors, transitions the input to ColorShaderRead and
	 * ComposedOutput to ShaderWrite, then dispatches ceil(w/16) x ceil(h/16)
	 * thread groups. Gamma comes from ctx.Frame().Settings().r_gamma.
	 *
	 * @param cmdBuf  Command buffer in recording state.
	 * @param cache   Mutable render cache for attachment access.
	 * @param ctx     Per-frame context (render extent, frame index, config).
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/// Declares the HDRColor read and the ComposedOutput write.
	PassIO GetIO() const override;

	/**
	 * @brief Writes both descriptors (HDRColor, ComposedOutput) into the
	 *        specified set.
	 * @param setIndex  Index into p_descriptorSets (0 .. numSets-1).
	 * @param extent    Render area dimensions.
	 * @param cache     Mutable render cache for attachment access.
	 */
	void WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache) override;

private:
	/** @brief Creates the descriptor set layout (2 bindings). */
	static DescriptorSetLayout CreateDescriptorSetLayout(const vk::raii::Device& device);

	/** @brief Creates the compute pipeline from the self-loaded shader. */
	void BuildPipeline(const vk::raii::Device& device,
	                   const std::string& debugName) override;

	// (Pipelines inherited from Pass - p_pipelines[0])

	// --- Self-loaded compute shader (via ShaderLibrary) ---
	std::unique_ptr<ComputeShader> p_shader;
};

} // namespace neurus
