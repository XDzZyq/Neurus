/**
 * @file DebugPass.h
 * @brief Overlay raster pass that draws every viewport debug primitive.
 *
 * Draws the debug overlay into the overlay image (OverlayOutput under
 * OverlayEngine), with LOAD_OP_LOAD so the picture underneath survives. Reads
 * the G-Buffer depth so debug geometry is occluded unless it asks not to be.
 * Three pipelines: lines, points and wireframe; x-ray is a dynamic depth-test
 * toggle, not a fourth pipeline.
 */

#pragma once

#include "../DescriptorManager.h"
#include "../PipelineBuilder.h"
#include "../buffers/BufferLayout.h"
#include "../shaders/RenderShader.h"
#include "Pass.h"

#include <glm/glm.hpp>
#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <memory>
#include <vector>

namespace neurus {

struct DebugDrawList;

/**
 * @brief Push constants shared by the line and point pipelines (16 B).
 *
 * Mirrors the `PushConstants` block in overlay_line.vert and overlay_point.vert.
 * Both shaders declare the same block so the two pipelines can share one layout.
 */
struct DebugPushConstants
{
	/// @brief Render target size in pixels; the quad expansion works in this space.
	glm::vec2 viewportSize{0.0f, 0.0f};

	/// @brief Pixels per world unit at w = 1, i.e. |proj[1][1]| * height * 0.5.
	float projScaleY{1.0f};

	/// @brief Device pointSizeRange[1]; gl_PointSize is clamped to it (exceeding it is UB).
	float maxPointSizePx{1.0f};
};

static_assert(sizeof(DebugPushConstants) == 16, "must match the GLSL PushConstants block");

/**
 * @brief Push constants for the wireframe pipeline (72 B).
 *
 * Mirrors the `PushConstants` block in debug_wire.vert. Wireframes carry a
 * matrix here rather than baked world positions, because their geometry is
 * drawn straight from the mesh buffers and never copied.
 */
struct DebugWirePushConstants
{
	glm::mat4 model{1.0f};       ///< Local-to-world transform.
	uint32_t rgba{0xFFFFFFFFu};  ///< Packed color (see PackDebugColor).
	uint32_t flags{0u};          ///< DebugFlag bits (only XRay is meaningful).
};

static_assert(sizeof(DebugWirePushConstants) == 72, "must match the GLSL PushConstants block");

/**
 * @brief Draws the debug overlay over the final shaded image.
 *
 * Owns pipelines, a descriptor layout, a pool and one set per frame in flight;
 * the camera UBO and the segment/point SSBOs are bound from the cache.
 *
 * Descriptor set 0:
 *   binding 0  CameraUBO      (uniform buffer, vertex)
 *   binding 1  SegmentBuffer  (storage buffer, vertex)
 *   binding 2  PointBuffer    (storage buffer, vertex)
 */
class DebugPass : public Pass
{
public:
	/**
	 * @brief Constructs the pass and all its GPU resources.
	 *
	 * @param device          Logical device (retained reference).
	 * @param physicalDevice  Physical device (memory types + pointSizeRange).
	 * @param framesInFlight  Number of descriptor sets to allocate; must equal the
	 *                        renderer's frames in flight, since a set is rewritten
	 *                        while the previous frame may still be reading its own.
	 * @throws std::runtime_error if a shader fails to load or a pipeline to build.
	 */
	DebugPass(const vk::raii::Device& device,
	          const vk::raii::PhysicalDevice& physicalDevice,
	          uint32_t framesInFlight);

	/**
	 * @brief Chooses the color attachment the overlay draws into.
	 *
	 * OverlayOutput when the pass is driven by OverlayEngine (what production
	 * does), ComposedOutput by default. Any color attachment sharing
	 * ComposedOutput's format and usage works, which is what the pipelines are
	 * built against. Must be called before the pass is registered with the render
	 * graph, which caches GetIO() at registration.
	 */
	void SetTarget(AttachmentName target) { p_target = target; }

	/**
	 * @brief The attachment this pass draws into, i.e. the last image of the frame.
	 *
	 * Correct whether or not the overlay drew anything: Record() either leaves
	 * the target in ColorAttachment or does not touch it, and the swapchain
	 * blit transitions it either way.
	 */
	AttachmentName GetTarget() const { return p_target; }

	/**
	 * @brief Records the debug overlay for this frame.
	 *
	 * Returns immediately when there is nothing to draw, leaving every image as
	 * the previous pass left it. Otherwise binds this frame's set, transitions
	 * the target to ColorAttachment and Depth to DepthAttachment, begins
	 * rendering with LOAD_OP_LOAD on both, then draws lines, points and
	 * wireframes with a depth-test toggle per x-ray range. Leaves the target in
	 * ColorAttachment.
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/**
	 * @brief Declares the target (read + written in place) and Depth (read).
	 *
	 * Only the resource identities matter: they order this pass after whichever
	 * pass last wrote the target.
	 */
	PassIO GetIO() const override;

private:
	/// @brief Creates the set-0 layout: camera UBO + segment SSBO + point SSBO.
	static DescriptorSetLayout CreateLayout(const vk::raii::Device& device);

	/// @brief Builds the line, point and wireframe pipelines, in that order.
	void BuildPipeline(const vk::raii::Device& device, const std::string& debugName) override;

	/**
	 * @brief Applies the settings every debug pipeline shares.
	 *
	 * The post-chain color format, alpha-over blending, depth read without
	 * depth write against the G-Buffer depth, no culling, and eDepthTestEnable
	 * as dynamic state so x-ray needs no second pipeline.
	 */
	void ConfigureCommonState(PipelineBuilder& builder);

	// --- Pipeline slots within Pass::p_pipelines ---
	static constexpr size_t kLinePipeline  = 0;
	static constexpr size_t kPointPipeline = 1;
	static constexpr size_t kWirePipeline  = 2;

	// --- Descriptor resources (pool must outlive the sets: declare it first) ---
	DescriptorSetLayout p_layout;
	DescriptorPool p_descriptorPool;
	std::vector<DescriptorSet> p_descriptorSets;  ///< One per frame in flight.

	// --- Self-loaded shaders (via ShaderLibrary) ---
	std::unique_ptr<RenderShader> p_lineShader;
	std::unique_ptr<RenderShader> p_pointShader;
	std::unique_ptr<RenderShader> p_wireShader;

	/// @brief MeshData's vertex layout, shared with gbuffer.vert (pos/normal/uv).
	BufferLayout p_meshVertexLayout;

	/// @brief Device pointSizeRange[1], queried once and pushed to the point shader.
	float p_maxPointSizePx = 1.0f;

	/// @brief Color target: OverlayOutput under OverlayEngine, ComposedOutput by default (SetTarget).
	AttachmentName p_target = AttachmentName::ComposedOutput;
};

} // namespace neurus


