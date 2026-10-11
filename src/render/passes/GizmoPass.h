/**
 * @file GizmoPass.h
 * @brief Overlay raster pass that draws the modal transform gizmo's guides.
 *
 * Draws the gizmo overlay over the final shaded image with LOAD_OP_LOAD and no
 * depth attachment, so guides stay visible through geometry. The pass always
 * sits in the graph and returns early when its payload is null or empty.
 */

#pragma once

#include "../DescriptorManager.h"
#include "../PipelineBuilder.h"
#include "../shaders/RenderShader.h"
#include "Pass.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace neurus {

struct GizmoDrawList;

/**
 * @brief Push constants shared by the line and point pipelines (16 B).
 *
 * Mirrors the `PushConstants` block in overlay_line.vert and
 * overlay_point.vert.
 */
struct GizmoPushConstants
{
	/// @brief Render target size in pixels; the quad expansion works in this space.
	glm::vec2 viewportSize{0.0f, 0.0f};

	/// @brief Pixels per world unit at w = 1, i.e. |proj[1][1]| * height * 0.5.
	float projScaleY{1.0f};

	/// @brief Device pointSizeRange[1]; gl_PointSize is clamped to it (exceeding it is UB).
	float maxPointSizePx{1.0f};
};

static_assert(sizeof(GizmoPushConstants) == 16, "must match the GLSL PushConstants block");

/**
 * @brief Draws the gizmo overlay over the final shaded image, depth test off.
 *
 * Owns pipelines, a descriptor layout, a pool and one set per frame in flight;
 * the camera UBO and the segment/point SSBOs are bound from the cache.
 *
 * Descriptor set 0:
 *   binding 0  CameraUBO      (uniform buffer, vertex)
 *   binding 1  SegmentBuffer  (storage buffer, vertex)
 *   binding 2  PointBuffer    (storage buffer, vertex)
 */
class GizmoPass : public Pass
{
public:
	/**
	 * @brief Constructs the pass and all its GPU resources.
	 *
	 * @param device          Logical device (retained reference).
	 * @param physicalDevice  Physical device (memory types + pointSizeRange).
	 * @param framesInFlight  Number of descriptor sets to allocate; must equal
	 *                        the renderer's frames in flight, since a set is
	 *                        rewritten while the previous frame may still read
	 *                        its own.
	 * @throws std::runtime_error if a shader fails to load or a pipeline to build.
	 */
	GizmoPass(const vk::raii::Device& device,
	          const vk::raii::PhysicalDevice& physicalDevice,
	          uint32_t framesInFlight);

	/**
	 * @brief Chooses the color attachment the guides draw into.
	 *
	 * ComposedOutput by default, or FXAAOutput when FXAA runs. Must be called
	 * before the pass is registered with the render graph, which caches
	 * GetIO() at registration time.
	 */
	void SetTarget(AttachmentName target) { p_target = target; }

	/**
	 * @brief The attachment this pass draws into, i.e. the last image of the frame.
	 *
	 * Correct whether or not anything was drawn: Record() either leaves the
	 * target in ColorAttachment or does not touch it, and the swapchain blit
	 * transitions it either way.
	 */
	AttachmentName GetTarget() const { return p_target; }

	/**
	 * @brief Records the gizmo overlay for this frame.
	 *
	 * Returns immediately when there is no gesture, leaving every image exactly
	 * as the previous pass left it. Otherwise binds this frame's set, starts
	 * rendering with LOAD_OP_LOAD and no depth attachment, draws segments then
	 * points, and leaves the target in ColorAttachment.
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/**
	 * @brief Declares the target, read and written in place.
	 *
	 * Naming the target on both sides is what orders this pass after whichever
	 * pass last wrote it.
	 */
	PassIO GetIO() const override;

private:
	/// @brief Creates the set-0 layout: camera UBO + segment SSBO + point SSBO.
	static DescriptorSetLayout CreateLayout(const vk::raii::Device& device);

	/// @brief Builds the line and point pipelines, in that order.
	void BuildPipeline(const vk::raii::Device& device, const std::string& debugName) override;

	/**
	 * @brief Applies the settings both gizmo pipelines share.
	 *
	 * The post-chain color format, alpha-over blending, no culling (a guide is
	 * two-sided by nature) and set 0. No depth format and no depth test: with
	 * no depth attachment bound, enabling the test would be invalid rather
	 * than merely useless.
	 */
	void ConfigureCommonState(PipelineBuilder& builder);

	// --- Pipeline slots within Pass::p_pipelines ---
	static constexpr size_t kLinePipeline  = 0;
	static constexpr size_t kPointPipeline = 1;

	// --- Descriptor resources (pool must outlive the sets: declare it first) ---
	DescriptorSetLayout p_layout;
	DescriptorPool p_descriptorPool;
	std::vector<DescriptorSet> p_descriptorSets;  ///< One per frame in flight.

	// --- Self-loaded shaders (via ShaderLibrary) ---
	std::unique_ptr<RenderShader> p_lineShader;
	std::unique_ptr<RenderShader> p_pointShader;

	/// @brief Device pointSizeRange[1], queried once and pushed to the point shader.
	float p_maxPointSizePx = 1.0f;

	/// @brief Color target: ComposedOutput, or FXAAOutput when FXAA runs (SetTarget).
	AttachmentName p_target = AttachmentName::ComposedOutput;
};

} // namespace neurus
