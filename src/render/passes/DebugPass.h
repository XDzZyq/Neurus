/**
 * @file DebugPass.h
 * @brief Overlay raster pass that draws every viewport debug primitive.
 *
 * DebugPass is the single consumer of DebugDrawList. It runs after ComposePass,
 * drawing directly into ComposedOutput with LOAD_OP_LOAD so the tonemapped image
 * survives underneath, and reads (never writes) the G-Buffer depth so debug
 * geometry is occluded by solid objects unless it asks not to be.
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
 * Upload strategy: debug producers are stateful, so the list is usually identical
 * to last frame's. Each frame-in-flight slot owns its own HostBuffer pair and
 * remembers the DebugDrawList::revision it last copied; a matching revision skips
 * the memcpy entirely. Buffer capacity is FIXED (see kMaxSegments / kMaxPoints)
 * so the descriptor sets can be written once at construction and never touched
 * again — a growing buffer would swap the VkBuffer handle out from under them.
 */

#pragma once

#include "../DescriptorManager.h"
#include "../PipelineBuilder.h"
#include "../buffers/BufferLayout.h"
#include "../buffers/HostBuffer.h"
#include "../buffers/UniformBuffer.h"
#include "../shaders/RenderShader.h"
#include "GeometryPass.h"   // CameraUBOData — same {viewProj, view} block as gbuffer.vert
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
 * @brief Draws DebugDrawList over the composed image.
 *
 * Owns its own camera UBO, descriptor layout, pool and sets — there is no shared
 * camera buffer in the renderer, so this mirrors what GeometryPass does.
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
	 * @brief Segment capacity per frame-in-flight (48 B each → 3 MB per slot).
	 *
	 * Fixed, not a hint: overflow is clamped and logged once rather than growing
	 * the buffer, which would invalidate the descriptor written at construction.
	 * Issue #22 budgets 10,000 segments; this leaves 6x headroom.
	 */
	static constexpr uint32_t kMaxSegments = 65536;

	/// @brief Point-sprite capacity per frame-in-flight (32 B each → 0.5 MB per slot).
	static constexpr uint32_t kMaxPoints = 16384;

	/**
	 * @brief Constructs the pass and all its GPU resources.
	 *
	 * @param device          Logical device (retained reference).
	 * @param physicalDevice  Physical device (memory types + pointSizeRange).
	 * @param framesInFlight  Ring size; must equal the renderer's frames in flight,
	 *                        since each slot's buffers are written while the
	 *                        previous frame may still be reading its own.
	 * @throws std::runtime_error if a shader fails to load or a pipeline to build.
	 */
	DebugPass(const vk::raii::Device& device,
	          const vk::raii::PhysicalDevice& physicalDevice,
	          uint32_t framesInFlight);

	/**
	 * @brief Records the debug overlay for this frame.
	 *
	 *   1. Returns immediately when there is nothing to draw, leaving every image
	 *      in the state ComposePass left it in.
	 *   2. Uploads the camera UBO, and the segment/point buffers only when
	 *      DebugDrawList::revision differs from what this slot last copied.
	 *   3. Transitions ComposedOutput to ColorAttachment and Depth to
	 *      DepthAttachment, then begins rendering with LOAD_OP_LOAD on both.
	 *   4. Draws lines, points and wireframes, each as a depth-tested range
	 *      followed by an x-ray range (eDepthTestEnable toggled between them).
	 *   5. Leaves ComposedOutput in TransferSrc — the state the final blit
	 *      assumes when FXAA is off, and the one ComposePass would have left.
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/**
	 * @brief Declares ComposedOutput (read + written in place) and Depth (read).
	 *
	 * Binding metadata is unused: like the other raster passes, this one manages
	 * its own attachments. Only the resource identities matter, so RenderGraph can
	 * order it after ComposePass and before FXAAPass.
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
	 * ComposedOutput's format and alpha-over blending, depth read without depth
	 * write against the G-Buffer depth, no culling (debug geometry is two-sided by
	 * nature), and eDepthTestEnable added to the dynamic state so x-ray needs no
	 * second pipeline.
	 */
	void ConfigureCommonState(PipelineBuilder& builder);

	/**
	 * @brief One ring slot: the buffers a single in-flight frame reads from.
	 *
	 * A slot's buffers are refilled while the *other* slot may still be in the
	 * GPU's hands, which is the whole reason the ring exists. The cached counts
	 * live here too: when the revision matches, Record() draws from them without
	 * looking at the list again.
	 */
	struct FrameSlot
	{
		std::unique_ptr<UniformBuffer<CameraUBOData>> camera;
		std::unique_ptr<HostBuffer> segments;
		std::unique_ptr<HostBuffer> points;

		/**
		 * @brief DebugDrawList::revision this slot's buffers currently hold.
		 *
		 * UINT64_MAX means "never uploaded". Zero would be wrong: a list that
		 * legitimately sits at revision 0 would be skipped forever.
		 */
		uint64_t uploadedRevision = UINT64_MAX;

		uint32_t segmentCount = 0;      ///< Segments actually resident (post-clamp).
		uint32_t pointCount = 0;        ///< Points actually resident (post-clamp).
		uint32_t xraySegmentStart = 0;  ///< First x-ray segment within segmentCount.
		uint32_t xrayPointStart = 0;    ///< First x-ray point within pointCount.
	};

	/**
	 * @brief Copies the list into a slot when its revision has moved on.
	 * @param slot Ring slot to fill (its cached counts are updated either way).
	 * @param list The frame's debug geometry.
	 * @note Counts above capacity are clamped and reported once per pass lifetime.
	 */
	void UploadIfChanged(FrameSlot& slot, const DebugDrawList& list);

	// --- Pipeline slots within Pass::p_pipelines ---
	static constexpr size_t kLinePipeline  = 0;
	static constexpr size_t kPointPipeline = 1;
	static constexpr size_t kWirePipeline  = 2;

	// --- Descriptor resources (pool must outlive the sets: declare it first) ---
	DescriptorSetLayout p_layout;
	DescriptorPool p_descriptorPool;
	std::vector<DescriptorSet> p_descriptorSets;  ///< One per frame in flight.

	std::vector<FrameSlot> p_frames;

	// --- Self-loaded shaders (via ShaderLibrary) ---
	std::unique_ptr<RenderShader> p_lineShader;
	std::unique_ptr<RenderShader> p_pointShader;
	std::unique_ptr<RenderShader> p_wireShader;

	/// @brief MeshData's vertex layout, shared with gbuffer.vert (pos/normal/uv).
	BufferLayout p_meshVertexLayout;

	/// @brief Device pointSizeRange[1], queried once and pushed to the point shader.
	float p_maxPointSizePx = 1.0f;

	/// @brief Latches the capacity-overflow warning so it is logged once, not per frame.
	bool p_warnedOverflow = false;
};

} // namespace neurus


