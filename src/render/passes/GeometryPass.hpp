/**
 * @file GeometryPass.hpp
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

#include "render/resources/ShaderGPU.hpp"

#include "../DescriptorManager.hpp"
#include "../PipelineBuilder.hpp"
#include "../buffers/BufferLayout.hpp"
#include "../resources/CameraGPU.hpp"
#include "Pass.hpp"

#include <glm/glm.hpp>
#include <span>
#include <vulkan/vulkan_raii.hpp>

#include <unordered_set>
#include <vector>

namespace neurus {

struct EnsurePipelineRequest;  // render/requests/RenderRequests.hpp

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
	GeometryPass(RenderCache& cache, const vk::raii::Device& device,
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
	PassStats Record(vk::CommandBuffer cmdBuf, const PassFrame& frame) override;

	/// @brief The camera set 0, written by the graph with the shared camera UBO.
	DescriptorSet* GraphDescriptorSet(uint32_t frameIndex) override
	{
		(void)frameIndex;
		return &p_cameraDescriptorSet;
	}

	/// @brief The shared camera UBO at binding 0.
	void CollectGraphBuffers(RenderCache& cache, uint32_t frameIndex,
	                         std::vector<GraphBufferBinding>& out) const override;

	/**
	 * @brief Builds the per-mesh pipeline from an already uploaded shader version.
	 * @param cache Cache owning shader modules and the resulting pipeline.
	 * @param request Stable shader UID and uploaded version to consume.
	 * @return True if the pipeline exists; false when its upload is absent.
	 */
	bool EnsureMeshPipeline(RenderCache& cache, const EnsurePipelineRequest& request);

	/// Declares the G-Buffer + IDBuffer + Depth writes; this pass has no image
	/// reads (camera data comes via a UBO).
	PassIO GetIO() const override;

	/// @brief The built-in G-Buffer shader.
	std::vector<const ShaderGPU*> GetShaders() const override;

	/**
	 * @brief Returns the camera descriptor set layout (set 0).
	 */
	const DescriptorSetLayout& GetCameraLayout() const { return p_cameraLayout; }

public:
	/// @brief Constructs from a cached uploaded shader.
	GeometryPass(const vk::raii::Device& device,
	             const vk::raii::PhysicalDevice& physicalDevice,
	             std::shared_ptr<const ShaderGPU> shader);

private:

	/**
	 * @brief Creates the graphics pipeline using PipelineBuilder.
	 * Shader modules from the selected cached GPU shader.
	 */
	void BuildPipeline(const vk::raii::Device& device,
	                   const std::string& debugName) override;

	/** @brief Builds a G-Buffer pipeline from uploaded shader modules. */
	Pipeline CreatePerMeshPipeline(const ShaderGPU& shader);
	void ConfigureGBufferPipeline(PipelineBuilder& builder);

	// --- Descriptor resources ---
	DescriptorSetLayout p_cameraLayout;             ///< Set 0 layout definition

	// --- Descriptor pool + set naming the shared camera UBO ---
	// The buffer itself is owned by the cache; this pass only binds it.
	DescriptorPool p_descriptorPool;
	DescriptorSet p_cameraDescriptorSet;

	// (Pipelines inherited from Pass - p_pipelines[0])

	// --- Cached uploaded shader ---
	std::shared_ptr<const ShaderGPU> p_shader;

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
