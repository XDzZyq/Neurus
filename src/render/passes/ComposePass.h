/**
 * @file ComposePass.h
 * @brief Final compositing compute pass — blends gizmo highlight onto HDRColor
 *        and applies gamma correction.
 *
 * ComposePass reads the HDRColor and SelectionOutline attachments as combined
 * image samplers, blends the gizmo highlight (e.g. selected-object edge
 * outline) onto the HDRColor, applies gamma correction via push constant,
 * and writes the final composed output to the ComposedOutput attachment
 * (RGBA16F storage image).
 *
 * The ComposedOutput image is then blitted to the swapchain by the
 * DeferredRenderer.
 *
 * Architecture:
 * - Inherits from ComputePass for shared infrastructure (sampler, descriptor
 *   pool/sets, barrier transitions, dispatch logic).
 * - Owns the compute pipeline.
 * - Borrows RenderCache for HDRColor, SelectionOutline, and ComposedOutput images.
 *
 * @note No UBOs or per-frame uploads — all data is read from attachments.
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
 * @brief Final compositing compute pass.
 *
 * Reads HDRColor (RGBA16F) and SelectionOutline (R8_UNORM) as combined image
 * samplers, blends the highlight onto the HDR output, applies gamma
 * correction, and writes to ComposedOutput (RGBA16F storage image).
 *
 * Inherits shared compute-pass infrastructure from ComputePass.
 */
class ComposePass : public ComputePass
{
public:
	/**
	 * @brief Constructs the ComposePass and creates all GPU resources.
	 *
	 * Shaders are self-loaded via ShaderLibrary from GLSL source files.
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
	 *   1. Reads gamma from ctx.editor.config (RenderConfig::r_gamma).
	 *   2. Writes HDRColor, SelectionOutline, and ComposedOutput descriptors.
	 *   3. Transitions HDRColor/SelectionOutline to ColorShaderRead.
	 *   4. Transitions ComposedOutput to ShaderWrite.
	 *   5. Binds pipeline, descriptor set, push constants.
	 *   6. Dispatches ceil(width/16) x ceil(height/16) x 1 thread groups.
	 *   7. Transitions ComposedOutput to TransferSrc (ready for swapchain blit).
	 *
	 * @param cmdBuf  Command buffer in recording state.
	 * @param cache   Mutable render cache for attachment access.
	 * @param ctx     Per-frame context (render extent, frame index, config).
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/// Declares HDRColor + SelectionOutline reads and the ComposedOutput write
	/// for RenderGraph wiring.
	PassIO GetIO() const override;

	/**
	 * @brief Writes all descriptors (HDRColor, SelectionOutline, ComposedOutput)
	 *        into the specified set.
	 * @param setIndex  Index into p_descriptorSets (0 … numSets-1).
	 * @param extent    Render area dimensions.
	 * @param cache     Mutable render cache for attachment access.
	 */
	void WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache) override;

private:
	/**
	 * @brief Creates the descriptor set layout (3 bindings).
	 *
	 * Bindings:
	 *   0: HDRColor        (combined image sampler)
	 *   1: SelectionOutline   (combined image sampler)
	 *   2: ComposedOutput   (storage image, RGBA16F)
	 */
	static DescriptorSetLayout CreateDescriptorSetLayout(const vk::raii::Device& device);

	/**
	 * @brief Creates the compute pipeline.
	 * ShaderModule from self-loaded ComputeShader.
	 */
	void BuildPipeline(const vk::raii::Device& device,
	                   const std::string& debugName) override;

	// (Pipelines inherited from Pass — p_pipelines[0])

	// --- Self-loaded compute shader (via ShaderLibrary) ---
	std::unique_ptr<ComputeShader> p_shader;
};

} // namespace neurus
