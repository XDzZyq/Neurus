/**
 * @file SelectionOutlinePass.h
 * @brief Selected-object edge highlight compute pass.
 *
 * SelectionOutlinePass reads the IDBuffer (R32_UINT) and performs 3×3 edge detection
 * for the active selected object.  Pixels belonging to the active object
 * whose neighbours differ (different object ID or background) are flagged
 * as edges and written to the SelectionOutline attachment (R8_UNORM).
 *
 * Architecture:
 * - Inherits from ComputePass for shared infrastructure (sampler, descriptor
 *   pool/sets, barrier transitions, dispatch logic).
 * - Owns the compute pipeline (no UBOs — only image bindings + push constant).
 * - Borrows RenderCache for IDBuffer and SelectionOutline image views.
 *
 * @note Simple pass with no per-frame uploads.  The single push constant
 *       (activeObjectId) is read directly from RenderContext each frame.
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

/**
 * @brief Selected-object edge highlight compute pass.
 *
 * Reads the IDBuffer (R32_UINT) as a usampler2D, compares each pixel
 * against its 8 neighbours, and writes the edge mask (R8) to the
 * SelectionOutline attachment.
 *
 * Inherits shared compute-pass infrastructure from ComputePass.
 */
class SelectionOutlinePass : public ComputePass
{
public:
	/**
	 * @brief Constructs the selection outline pass and creates all GPU resources.
	 *
	 * Shaders are self-loaded via ShaderLibrary from GLSL source files.
	 *
	 * @param device            Logical device (retained reference).
	 * @param physicalDevice    Physical device (for sampler creation).
	 * @param numSets           Number of descriptor sets (one per in-flight frame).
	 *
	 * @throws std::runtime_error if shader or pipeline creation fails.
	 */
	SelectionOutlinePass(const vk::raii::Device& device,
	          const vk::raii::PhysicalDevice& physicalDevice,
	          uint32_t numSets);

	// -------------------------------------------------------------------
	// Recording
	// -------------------------------------------------------------------

	/**
	 * @brief Records the selection outline compute dispatch into a command buffer.
	 *
	 *   1. Updates per-frame data (push constant from ctx.activeObjectId).
	 *   2. Writes descriptors for this frame slot.
	 *   3. Transitions SelectionOutline attachment to ShaderWrite for compute write.
	 *   4. Binds pipeline, descriptor set, push constants (uint32_t activeObjectId).
	 *   5. Dispatches ceil(width/16) × ceil(height/16) × 1 thread groups.
	 *   6. Transitions SelectionOutline to ColorShaderRead for downstream passes.
	 *
	 * @param cmdBuf  Command buffer in recording state.
	 * @param cache   Render cache for attachment access.
	 * @param ctx     Per-frame context (render extent, frame index, activeObjectId).
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/**
	 * @brief Declares the image attachments this pass reads and writes.
	 *
	 * Reads:  IDBuffer       @ slot 0 (combined image sampler, eShaderReadOnlyOptimal)
	 * Writes: SelectionOutline @ slot 1 (storage image, eGeneral)
	 *
	 * Drives both the RenderGraph DAG wiring and the DescriptorBinder image
	 * writes in WriteDescriptors().
	 */
	PassIO GetIO() const override;

	/**
	 * @brief Writes all descriptors (image bindings) into the specified set.
	 *
	 * Bindings:
	 *   0: IDBuffer (combined image sampler, R32_UINT)
	 *   1: SelectionOutline (storage image, R8_UNORM)
	 *
	 * @param setIndex  Index into p_descriptorSets (0 … numSets-1).
	 * @param extent    Render extent for attachment lookup.
	 * @param cache     Render cache for IDBuffer / SelectionOutline access.
	 */
	void WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache) override;

private:
	/**
	 * @brief Creates the descriptor set layout (2 bindings).
	 *
	 * Bindings:
	 *   0: gIDBuffer        (combined image sampler, usampler2D)
	 *   1: outputHighlight  (storage image, R8) — the GLSL name is unchanged; only
 *                        the attachment it is bound to was renamed.
	 */
	static DescriptorSetLayout CreateDescriptorSetLayout(const vk::raii::Device& device);

	/**
	 * @brief Creates the compute pipeline.
	 * ShaderModule from self-loaded ComputeShader.
	 */
	void BuildPipeline(const vk::raii::Device& device,
	                   const std::string& debugName) override;

	// --- Self-loaded compute shader (via ShaderLibrary) ---
	std::unique_ptr<ComputeShader> p_shader;
};

} // namespace neurus
