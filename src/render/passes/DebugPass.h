/**
 * @file DebugPass.h
 * @brief Overlay raster pass that draws every viewport debug primitive.
 *
 * DebugPass is the single consumer of DebugDrawList. It runs last in the shading
 * tail — after ComposePass, and after FXAAPass when anti-aliasing is on — drawing
 * into whichever image ends that chain with LOAD_OP_LOAD so the picture underneath
 * survives, and reads (never writes) the G-Buffer depth so debug geometry is
 * occluded by solid objects unless it asks not to be.
 *
 * Drawing after post-AA rather than before it is deliberate. Every debug primitive
 * that can be antialiased already is, analytically and from real coverage:
 * debug_line.frag fades one pixel in from the quad edge using the true line width,
 * debug_point.frag fades an exact p-norm distance through fwidth(). FXAA has only
 * luma to work from, and a thin high-contrast line is its worst input — it re-blurs
 * a gradient that was already correct, drags overlay color into neighbouring scene
 * pixels, and softens the hard dash ends that DebugDrawBuilder deliberately leaves
 * un-Smoothed on a stippled line. Handing FXAA the scene alone and compositing the
 * overlay on top keeps both correct. The one thing given up is wireframe, whose
 * PolygonMode::eLine edges have no distance-to-edge to fade and so were the only
 * primitives FXAA genuinely helped.
 *
 * The color target is therefore configurable rather than fixed: FXAAPass is a
 * ping-pong compute pass (samples ComposedOutput, writes FXAAOutput), so the last
 * image in the chain is FXAAOutput when it runs and ComposedOutput when it does
 * not. Both attachments are created with the same format and usage, so one set of
 * pipelines covers either. SetTarget() is called by
 * DeferredRenderer::RebuildMainGraph() before the pass is handed to the graph,
 * which leaves the config → topology decision in PipelineSignature, the one place
 * that already owns it, and lets GetIO() stay a plain declaration of concrete
 * attachment names the graph can validate.
 *
 * Three pipelines, one per primitive kind:
 *   [0] lines — eTriangleList, six vertices per segment expanded into a
 *       screen-space quad by debug_line.vert (Metal caps lineWidth at 1.0, so
 *       wide-line rasterization is not an option; see DebugSegment).
 *   [1] points — ePointList with gl_PointSize, shape masked from gl_PointCoord.
 *   [2] wireframe — eTriangleList with PolygonMode::eLine over the mesh's
 *       existing MeshGPU buffers, so no geometry is duplicated or re-uploaded.
 *
 * X-ray is a dynamic state, not a fourth pipeline: DebugDrawList partitions its
 * segments and points so all depth-tested primitives precede all x-ray ones, and
 * this pass flips vk::DynamicState::eDepthTestEnable between the two halves. That
 * covers the whole frame in six draws at most, independent of primitive count.
 *
 * Upload strategy: the pass uploads nothing. The geometry lives in
 * RenderCache's DebugCache and the camera in its CameraGPU, both written once per
 * frame by DeferredRenderer::recordFrame() before any pass records; this pass
 * reads them. DebugCache grows its buffers with the geometry, which swaps the
 * VkBuffer handle, so the descriptor set for the frame being recorded is
 * re-written from the cache each frame — the same thing LightingPass does for the
 * light SSBOs, and legal for the same reason: a frame waits on its own fence
 * before recording, so the slot being rewritten is not in flight.
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
 * Mirrors the `PushConstants` block in debug_line.vert and debug_point.vert.
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
 * Mirrors the `PushConstants` block in debug_wire.vert. Wireframes keep a matrix
 * here instead of baking world positions like segments and points do, because
 * their geometry is never copied — it is drawn straight from MeshGPU.
 */
struct DebugWirePushConstants
{
	glm::mat4 model{1.0f};       ///< Local-to-world transform.
	uint32_t rgba{0xFFFFFFFFu};  ///< Packed color (see PackDebugColor).
	uint32_t flags{0u};          ///< DebugFlag bits (only XRay is meaningful).
};

static_assert(sizeof(DebugWirePushConstants) == 72, "must match the GLSL PushConstants block");

/**
 * @brief Draws DebugDrawList over the final shaded image.
 *
 * Owns pipelines, a descriptor layout, a pool and one set per frame in flight —
 * and no buffers at all: the camera UBO and the segment/point SSBOs belong to
 * RenderCache (CameraGPU, DebugCache), which every pass that needs them shares.
 *
 * Descriptor set 0:
 *   binding 0  CameraUBO      (uniform buffer, vertex)  → RenderCache::GetCameraGPU()
 *   binding 1  SegmentBuffer  (storage buffer, vertex)  → RenderCache::GetDebugCache()
 *   binding 2  PointBuffer    (storage buffer, vertex)  → RenderCache::GetDebugCache()
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
	 * ComposedOutput (the default) when the overlay is the last thing to touch the
	 * frame, FXAAOutput when FXAAPass runs in between. Must be called before
	 * RenderGraph::AddPass(), which caches GetIO() at registration — changing the
	 * target afterwards would leave the graph wired to the previous attachment.
	 * Only the two post-chain color attachments are valid: they share
	 * ComposedOutput's format and usage, which the pipelines are built against.
	 */
	void SetTarget(AttachmentName target) { p_target = target; }

	/**
	 * @brief The attachment this pass draws into, i.e. the last image of the frame.
	 *
	 * DeferredRenderer uses it as the swapchain blit source, which is correct
	 * whether or not the overlay drew anything: Record() either leaves the target in
	 * ColorAttachment or does not touch it at all, and the blit transitions it
	 * either way.
	 */
	AttachmentName GetTarget() const { return p_target; }

	/**
	 * @brief Records the debug overlay for this frame.
	 *
	 *   1. Returns immediately when there is nothing to draw, leaving every image
	 *      in the state the previous pass left it in.
	 *   2. Points this frame's descriptor set at the cache's current camera UBO and
	 *      debug buffers (their handles change whenever the geometry outgrows them).
	 *   3. Transitions the target to ColorAttachment and Depth to DepthAttachment,
	 *      then begins rendering with LOAD_OP_LOAD on both.
	 *   4. Draws lines, points and wireframes, each as a depth-tested range
	 *      followed by an x-ray range (eDepthTestEnable toggled between them).
	 *   5. Leaves the target in ColorAttachment: the swapchain blit is the consumer
	 *      and transitions it itself, which is what gives its barrier a source scope
	 *      that actually covers these draws.
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/**
	 * @brief Declares the target (read + written in place) and Depth (read).
	 *
	 * Binding metadata is unused: like the other raster passes, this one manages
	 * its own attachments. Only the resource identities matter, so RenderGraph can
	 * order it after whichever pass last wrote the target.
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
	 * The post-chain color format and alpha-over blending, depth read without depth
	 * write against the G-Buffer depth, no culling (debug geometry is two-sided by
	 * nature), and eDepthTestEnable added to the dynamic state so x-ray needs no
	 * second pipeline. The format is shared by ComposedOutput and FXAAOutput, so the
	 * same pipelines serve either target.
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

	/// @brief Color target: ComposedOutput, or FXAAOutput when FXAA runs (SetTarget).
	AttachmentName p_target = AttachmentName::ComposedOutput;
};

} // namespace neurus


