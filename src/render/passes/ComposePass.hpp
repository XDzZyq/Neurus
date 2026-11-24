/**
 * @file ComposePass.hpp
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
 *       Push constants carry gamma and the camera frustum size at unit depth.
 */

#pragma once

#include "render/resources/ShaderGPU.hpp"

#include "passes/ComputePass.hpp"

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
	ComposePass(RenderCache& cache, const vk::raii::Device& device,
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
	PassStats Record(vk::CommandBuffer cmdBuf, const PassFrame& frame) override;

	/// Declares the HDRColor read and the ComposedOutput write.
	PassIO GetIO() const override;

	/// @brief The tone-map and gamma compute shader.
	std::vector<const ShaderGPU*> GetShaders() const override;

public:
	/// @brief Constructs from a selected uploaded shader.
	ComposePass(const vk::raii::Device& device,
	            const vk::raii::PhysicalDevice& physicalDevice,
	            uint32_t numSets,
	            std::shared_ptr<const ShaderGPU> shader);

private:

	/** @brief Creates the compute pipeline from the self-loaded shader. */
	void BuildPipeline(const vk::raii::Device& device,
	                   const std::string& debugName) override;

	// (Pipelines inherited from Pass - p_pipelines[0])

	// --- Self-loaded compute shader (via ShaderLibrary) ---
	std::shared_ptr<const ShaderGPU> p_shader;
};

} // namespace neurus
