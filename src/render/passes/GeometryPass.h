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

#include <unordered_set>
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
	 * Binds the cache's camera UBO, begins the G_BUFFER dynamic rendering pass,
	 * draws each visible mesh with its per-mesh or default pipeline, then ends
	 * the pass.
	 *
	 * Looks per-mesh pipelines up and never builds one: a cache miss draws with
	 * the pass default. EnsureMeshPipeline() owns the building, off this path.
	 *
	 * @param cmdBuf          Command buffer in recording state.
	 * @param cache           Render cache for MeshGPU and attachment lookups.
	 * @param ctx             Per-frame context (scene, camera, extent).
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/**
	 * @brief Compiles the per-mesh G-Buffer pipeline for @p shader if it is missing.
	 *
	 * The pre-warm entry point, and the only place a per-mesh pipeline is built:
	 * compilation reads the live Shader's SPIR-V, so it must run on the Editor
	 * thread that owns it, before the frame is handed off. Idempotent.
	 *
	 * @param cache  Render cache owning the cross-frame pipeline cache.
	 * @param shader CPU-side Shader to compile (borrowed for the call only).
	 * @return true if a pipeline for @p shader is cached when this returns;
	 *         false if the shader carries no usable SPIR-V yet - not an error,
	 *         Record() draws such a mesh with the pass default.
	 */
	bool EnsureMeshPipeline(RenderCache& cache, const Shader& shader);

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
	 * formats and vertex layout. Called only from EnsureMeshPipeline(), i.e.
	 * only off the frame path.
	 *
	 * @param shader    The CPU-side Shader containing stage SPIR-V.
	 * @return Fully constructed Pipeline, or an empty Pipeline when the shader
	 *         carries no SPIR-V for any supported stage.
	 */
	Pipeline CreatePerMeshPipeline(const Shader& shader);
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

	/// Shader ids whose missing per-mesh pipeline has already been reported, so
	/// an uncompiled shader costs one log line instead of one per mesh per
	/// frame. Version-agnostic: only a successful EnsureMeshPipeline() clears an
	/// entry, so a shader that keeps failing to compile does not re-report.
	std::unordered_set<int> p_reportedMissingPipelines;

	// --- Dynamic rendering (G_BUFFER-specific) ---
	void BeginPass(vk::CommandBuffer cmdBuf,
	               std::span<const vk::ImageView> colorImageViews,
	               const vk::ImageView* pDepthImageView,
	               std::span<const vk::ClearValue> clearValues,
	               vk::Extent2D renderExtent);

	void EndPass(vk::CommandBuffer cmdBuf);
};

} // namespace neurus
