/**
 * @file GizmoPass.h
 * @brief Overlay raster pass that draws the modal transform gizmo's guides.
 *
 * The name this file occupies used to belong to the selection-outline compute pass,
 * renamed to SelectionOutlinePass in its own commit so that "gizmo" means exactly
 * one thing in this renderer: the interactive transform handle.
 *
 * GizmoPass is the single consumer of GizmoDrawList and runs **last** in the frame,
 * after DebugPass, into the same post-AA image with LOAD_OP_LOAD. Two consecutive
 * raster passes writing one attachment need no new barrier code: Barrier::Transition
 * does not early-out when before == after, so transitioning a target DebugPass
 * already left in ColorAttachment still emits the write-after-write dependency.
 *
 * ## Deliberately simpler than DebugPass, in three ways
 *
 *  1. **No depth attachment at all.** Every modal guide is meant to be visible
 *     through geometry — a constraint line the object hides is a line that cannot be
 *     read — so there is nothing to depth-test against and no reason to name Depth in
 *     GetIO(). That removes SetDepthFormat, SetDepthStencil and the
 *     eDepthTestEnable dynamic state, and lets beginRendering pass a null depth
 *     attachment.
 *  2. **No x-ray partition.** X-ray is the *only* thing DebugDrawList's segment /
 *     point partition exists for, and with depth off unconditionally every primitive
 *     is already x-ray. GizmoDrawList therefore stores no xraySegmentStart and this
 *     pass issues exactly two draws.
 *  3. **No wire pipeline.** A guide is lines and one point sprite; there is no
 *     MeshGPU in the picture, hence no vertex layout, no drawIndexed and no
 *     72-byte push block.
 *
 * ## Shared shaders, separate payload
 *
 * The pipelines are built over the same overlay_line / overlay_point pair DebugPass
 * uses, because the hard parts — screen-space quad expansion for a constant *pixel*
 * line width (MoltenVK caps lineWidth at 1.0 and exposes no line-rasterization
 * modes) and analytic edge antialiasing — are already solved and tested there. Only
 * the payload differs, so the descriptor layout and the 16-byte push block are
 * copied unchanged; ShaderLibrary's first argument is a log tag rather than a cache
 * key, so "GizmoLine" and "DebugLine" coexist over one file.
 *
 * Upload strategy: the pass uploads nothing. Geometry lives in RenderCache's
 * GizmoCache and the camera in its CameraGPU, both written once per frame by
 * DeferredRenderer::recordFrame() before any pass records.
 *
 * The pass is **always** in the graph and returns early on a null or empty payload,
 * so entering and leaving a modal never recompiles the DAG — the precedent DebugPass
 * already sets. No RenderConfig field gates it.
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
 * Mirrors the `PushConstants` block in overlay_line.vert and overlay_point.vert —
 * the same block DebugPushConstants mirrors, declared separately so neither pass
 * includes the other's header for it.
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
 * @brief Draws GizmoDrawList over the final shaded image, depth test off.
 *
 * Owns pipelines, a descriptor layout, a pool and one set per frame in flight — and
 * no buffers: the camera UBO and the segment/point SSBOs belong to RenderCache
 * (CameraGPU, GizmoCache).
 *
 * Descriptor set 0 (identical to DebugPass, because the shaders are the same):
 *   binding 0  CameraUBO      (uniform buffer, vertex)  → RenderCache::GetCameraGPU()
 *   binding 1  SegmentBuffer  (storage buffer, vertex)  → RenderCache::GetGizmoCache()
 *   binding 2  PointBuffer    (storage buffer, vertex)  → RenderCache::GetGizmoCache()
 */
class GizmoPass : public Pass
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
	GizmoPass(const vk::raii::Device& device,
	          const vk::raii::PhysicalDevice& physicalDevice,
	          uint32_t framesInFlight);

	/**
	 * @brief Chooses the color attachment the guides draw into.
	 *
	 * The same tail attachment DebugPass targets: ComposedOutput (the default), or
	 * FXAAOutput when FXAAPass runs. Must be called before RenderGraph::AddPass(),
	 * which caches GetIO() at registration.
	 */
	void SetTarget(AttachmentName target) { p_target = target; }

	/**
	 * @brief The attachment this pass draws into, i.e. the last image of the frame.
	 *
	 * DeferredRenderer uses it as the swapchain blit source. Correct whether or not
	 * anything was drawn: Record() either leaves the target in ColorAttachment or
	 * does not touch it, and the blit transitions it either way.
	 */
	AttachmentName GetTarget() const { return p_target; }

	/**
	 * @brief Records the gizmo overlay for this frame.
	 *
	 *   1. Returns immediately when there is no gesture, leaving every image exactly
	 *      as the previous pass left it.
	 *   2. Points this frame's descriptor set at the cache's current camera UBO and
	 *      gizmo buffers (their handles change whenever the geometry outgrows them).
	 *   3. Transitions the target to ColorAttachment and begins rendering with
	 *      LOAD_OP_LOAD on color and no depth attachment at all.
	 *   4. Draws segments, then points — draw order is what decides what lands on
	 *      top, with depth off and alpha-over blending.
	 *   5. Leaves the target in ColorAttachment for the swapchain blit to consume.
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/**
	 * @brief Declares the target, read and written in place. Nothing else.
	 *
	 * The one place this pass visibly diverges from DebugPass, which also reads
	 * Depth. Naming the target on both sides is what orders this pass after whichever
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
	 * DebugPass::ConfigureCommonState minus every depth-related call: the post-chain
	 * color format, alpha-over blending, no culling (a guide is two-sided by nature),
	 * and set 0. No depth format, no depth-stencil state and no eDepthTestEnable —
	 * with no depth attachment bound, enabling the test would be invalid rather than
	 * merely useless.
	 */
	void ConfigureCommonState(PipelineBuilder& builder);

	// --- Pipeline slots within Pass::p_pipelines ---
	static constexpr size_t kLinePipeline  = 0;
	static constexpr size_t kPointPipeline = 1;

	// --- Descriptor resources (pool must outlive the sets: declare it first) ---
	DescriptorSetLayout p_layout;
	DescriptorPool p_descriptorPool;
	std::vector<DescriptorSet> p_descriptorSets;  ///< One per frame in flight.

	// --- Self-loaded shaders (via ShaderLibrary), shared with DebugPass ---
	std::unique_ptr<RenderShader> p_lineShader;
	std::unique_ptr<RenderShader> p_pointShader;

	/// @brief Device pointSizeRange[1], queried once and pushed to the point shader.
	float p_maxPointSizePx = 1.0f;

	/// @brief Color target: ComposedOutput, or FXAAOutput when FXAA runs (SetTarget).
	AttachmentName p_target = AttachmentName::ComposedOutput;
};

} // namespace neurus
