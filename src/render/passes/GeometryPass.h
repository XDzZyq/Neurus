/**
 * @file GeometryPass.h
 * @brief G-Buffer geometry pass - renders scene meshes to MRT attachments.
 *
 * Records the first phase of the deferred rendering pipeline: draws all opaque
 * geometry into the G-Buffer colour attachments (Position, Normal, Albedo,
 * MetallicRoughness, IDBuffer) and the Depth attachment using
 * VK_KHR_dynamic_rendering.
 *
 * @note No PBR lighting - only geometry data is written to the G-Buffer.
 */

#pragma once

#include "../DescriptorManager.h"
#include "../PipelineBuilder.h"
#include "../buffers/BufferLayout.h"
#include "../resources/CameraGPU.h"
#include "../shaders/ShaderLibrary.h"
#include "../shaders/RenderShader.h"
#include "Pass.h"

#include <glm/glm.hpp>
#include <span>
#include <vulkan/vulkan_raii.hpp>

#include <vector>

namespace neurus {

/**
 * @brief G-Buffer geometry pass using dynamic rendering.
 *
 * Creates a graphics pipeline with 5 MRT colour attachments (Position, Normal,
 * Albedo, MetallicRoughness, IDBuffer) + depth.
 *
 * Non-copyable, movable.
 */
class GeometryPass : public Pass
{
public:
	/**
	 * @brief Constructs the geometry pass and creates all GPU resources.
	 *
	 * @param device              Logical device (retained reference).
	 * @param physicalDevice      Physical device (for format queries).
	 *
	 * @throws std::runtime_error if shader or pipeline creation fails.
	 */
	GeometryPass(const vk::raii::Device& device,
	             const vk::raii::PhysicalDevice& physicalDevice);

	/**
	 * @brief Records the G-Buffer draw commands into a command buffer.
	 *
	 * Uploads the camera UBO, begins the G_BUFFER dynamic rendering pass, draws
	 * each visible mesh with its per-mesh or default pipeline, then ends the
	 * pass.
	 *
	 * @param cmdBuf          Command buffer in recording state.
	 * @param cache           Render cache for MeshGPU and attachment lookups.
	 * @param ctx             Per-frame context (scene, camera, extent).
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/// Declares the G-Buffer + IDBuffer + Depth writes; this pass has no image
	/// reads (camera data comes via a UBO).
	PassIO GetIO() const override;

	/**
	 * @brief Returns the camera descriptor set layout (set 0).
	 */
	const DescriptorSetLayout& GetCameraLayout() const { return p_cameraLayout; }

private:
	/**
	 * @brief Creates the camera descriptor set layout (set 0, binding 0: CameraUBO).
	 */
	static DescriptorSetLayout CreateCameraLayout(const vk::raii::Device& device);

	/**
	 * @brief Creates the graphics pipeline using PipelineBuilder.
	 * ShaderModules from self-loaded RenderShader.
	 */
	void BuildPipeline(const vk::raii::Device& device,
	                   const std::string& debugName) override;

	/**
	 * @brief Creates a per-mesh graphics pipeline from a custom Shader.
	 *
	 * Builds a graphics pipeline matching the default G-Buffer attachment
	 * formats and vertex layout.
	 *
	 * @param shader    The CPU-side Shader containing stage source code.
	 * @param stageType Which shader stage to compile (VERTEX for graphics).
	 * @return Fully constructed Pipeline, or an empty Pipeline on failure.
	 */
	Pipeline CreatePerMeshPipeline(const Shader& shader, ShaderType stageType);
	void ConfigureGBufferPipeline(PipelineBuilder& builder);

	// --- Descriptor resources ---
	DescriptorSetLayout p_cameraLayout;             ///< Set 0 layout definition

	// --- Descriptor pool + set naming the shared camera UBO ---
	// The buffer itself is owned by the cache; this pass only binds it.
	DescriptorPool p_descriptorPool;
	DescriptorSet p_cameraDescriptorSet;

	// (Pipelines inherited from Pass - p_pipelines[0])

	// --- Self-loaded render shader (via ShaderLibrary) ---
	std::unique_ptr<RenderShader> p_shader;

	// --- Vertex input layout ---
	BufferLayout p_vertexLayout;

	// --- Dynamic rendering (G_BUFFER-specific) ---
	void BeginPass(vk::CommandBuffer cmdBuf,
	               std::span<const vk::ImageView> colorImageViews,
	               const vk::ImageView* pDepthImageView,
	               std::span<const vk::ClearValue> clearValues,
	               vk::Extent2D renderExtent);

	void EndPass(vk::CommandBuffer cmdBuf);
};

} // namespace neurus
