/**
 * @file SelectionOutlinePass.h
 * @brief Selected-object edge highlight compute pass.
 *
 * Reads the IDBuffer (R32_UINT) and performs 3x3 edge detection for the active
 * selected object. Pixels belonging to it whose neighbours differ (different
 * object ID or background) are flagged as edges and written to the
 * SelectionOutline attachment (R8_UNORM).
 *
 * @note No per-frame uploads; the single push constant (activeObjectId) is
 *       read directly from the context each frame.
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
 * Reads the IDBuffer (R32_UINT) as a usampler2D, compares each pixel against
 * its 8 neighbours, and writes the edge mask (R8) to the SelectionOutline
 * attachment.
 */
class SelectionOutlinePass : public ComputePass
{
public:
	/**
	 * @brief Constructs the selection outline pass and creates all GPU resources.
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
	 * Writes this frame's descriptors, transitions IDBuffer to ColorShaderRead
	 * and SelectionOutline to ShaderWrite, pushes the active object ID, then
	 * dispatches ceil(w/16) x ceil(h/16) groups and returns SelectionOutline to
	 * ColorShaderRead.
	 *
	 * @param cmdBuf  Command buffer in recording state.
	 * @param cache   Render cache for attachment access.
	 * @param ctx     Per-frame context (render extent, frame index, activeObjectId).
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/**
	 * @brief Declares the image attachments this pass reads and writes.
	 *
	 * Reads: IDBuffer at slot 0; writes: SelectionOutline at slot 1. These also
	 * feed the image writes in WriteDescriptors().
	 */
	PassIO GetIO() const override;

	/**
	 * @brief Writes all descriptors (image bindings) into the specified set.
	 *
	 * @param setIndex  Index into p_descriptorSets (0 .. numSets-1).
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
	 *   1: outputHighlight  (storage image, R8)
	 */
	static DescriptorSetLayout CreateDescriptorSetLayout(const vk::raii::Device& device);

	/** @brief Creates the compute pipeline from the self-loaded shader. */
	void BuildPipeline(const vk::raii::Device& device,
	                   const std::string& debugName) override;

	// --- Self-loaded compute shader (via ShaderLibrary) ---
	std::unique_ptr<ComputeShader> p_shader;
};

} // namespace neurus
