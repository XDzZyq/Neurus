/**
 * @file Pass.h
 * @brief Abstract base class for all render passes + pass type queries.
 *
 * Every render pass in the deferred pipeline (GeometryPass, LightingPass,
 * etc.) inherits from this interface.  It enforces a common entry point
 * via Record() and ensures non-copyable RAII semantics for all GPU-
 * owning passes.
 *
 * Also hosts the PassType enum and associated static query helpers that
 * were previously on RenderPassManager.
 */

#pragma once

#include "../Pipeline.h"
#include "../RenderCache.h"
#include "../RenderContext.h"

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace neurus {

/**
 * @brief One declared image resource used by a pass, plus its optional
 *        descriptor-binding slot (consumed by the DescriptorBinder in Wave 2).
 *
 * @c resource is an AttachmentName. Most name a single RenderCache attachment;
 * ShadowDepth / ShadowIntensity are logical resources resolved by their owning
 * pass (see RenderCache.h) but still identified here as one object.
 * @c descriptorType and @c imageLayout describe how the pass sees the resource
 * through its descriptor set:
 *   - Sampled input:  eCombinedImageSampler + eShaderReadOnlyOptimal (default)
 *   - Compute write:  eStorageImage         + eGeneral
 *
 * The fields exist on both read and write bindings because compute shaders
 * bind writable attachments via storage-image descriptors just like reads.
 */
struct AttachmentBinding
{
	AttachmentName     resource;
	uint32_t           binding        = 0;
	vk::DescriptorType descriptorType = vk::DescriptorType::eCombinedImageSampler;
	vk::ImageLayout    imageLayout    = vk::ImageLayout::eShaderReadOnlyOptimal;
};

/**
 * @brief Plain-data description of a pass's image-attachment I/O, returned by
 *        Pass::GetIO() and consumed by RenderGraph to build the DAG.
 *
 * `name` is used for topological error messages and debug labels.
 * `reads`/`writes` list the image attachments the pass consumes/produces.
 * Wave 1 scope: images only; UBO/SSBO/sampler bindings stay hand-written
 * on the pass and are not modeled here.
 *
 * Kept inline with Pass so passes remain unaware of RenderGraph types while
 * exposing enough for the graph to wire itself.
 */
struct PassIO
{
	std::string                    name;
	std::vector<AttachmentBinding> reads;
	std::vector<AttachmentBinding> writes;
};

/**
 * @brief CPU-side command counters returned by Pass::Record().
 *
 * Passes tally their own draw/dispatch calls while recording and hand them
 * back so the caller (RenderGraph) can aggregate per-pass metrics. Keeps
 * passes stateless with respect to profiling — no members, no reset step.
 */
struct PassStats
{
	uint32_t drawCalls  = 0;
	uint32_t dispatches = 0;
};

/**
 * @brief Base class for a single render pass in the pipeline.
 *
 * Derived classes implement Record() to write commands into the provided
 * command buffer.  Each pass owns its own GPU resources (pipelines,
 * descriptor sets, buffers) and exposes no Init()/Terminate() methods.
 *
 * @note Non-copyable, movable (copy = delete, move = default).
 */
class Pass
{
public:
	/**
	 * @brief Identifies a rendering pass with preset attachment configurations.
	 */
	enum class PassType
	{
		G_BUFFER,   ///< 5 color attachments + depth (Position, Normal, Albedo, MetallicRoughness, IDBuffer + Depth)
		LIGHTING,   ///< 1 color attachment, no depth
		SHADOW,     ///< Depth-only (0 color attachments)
		COMPOSITE,  ///< 1 color attachment (DONT_CARE load), no depth
		POST_FX     ///< 1 color attachment (DONT_CARE load), no depth
	};

	virtual ~Pass() = default;

	// --- Non-copyable, movable ---
	Pass(const Pass&) = delete;
	Pass& operator=(const Pass&) = delete;
	Pass(Pass&&) noexcept = default;
	Pass& operator=(Pass&&) noexcept = default;

	/**
	 * @brief Records the pass's commands into a command buffer.
	 *
	 * Each derived pass receives the per-frame context (immutable) and a
	 * mutable cache reference so it can lazily create or retrieve GPU
	 * resources (pipelines, descriptor sets, buffers) during recording.
	 *
	 * @param cmdBuf   Command buffer in the recording state.
	 * @param cache    Mutable render cache for lazy GPU resource creation.
	 * @param ctx      Per-frame context (attachments, viewport, frame index, etc.).
	 * @return Draw/dispatch counts recorded by this pass (for profiling).
	 */
	virtual PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) = 0;

	/**
	 * @brief Declares this pass's read/write attachment dependencies.
	 *
	 * Returns a plain POD describing which RenderCache attachments the pass
	 * consumes and produces.  The RenderGraph consumes this to build the
	 * DAG; passes remain unaware of RenderGraph types.
	 *
	 * Default returns an empty PassIO — passes that don't participate in the
	 * graph (or haven't been migrated yet) simply omit the override.
	 */
	virtual PassIO GetIO() const { return {}; }

	// --- Pass type queries (moved from RenderPassManager) ---

	/**
	 * @brief Returns the expected number of color attachments for a pass type.
	 * @param passType Pass type to query.
	 * @return Number of color attachments (G_BUFFER=5, LIGHTING/COMPOSITE/POST_FX=1, SHADOW=0).
	 */
	static uint32_t ColorAttachmentCount(PassType passType);

	/**
	 * @brief Returns whether the pass type expects a depth attachment.
	 * @param passType Pass type to query.
	 * @return true for G_BUFFER and SHADOW.
	 */
	static bool HasDepth(PassType passType);

	/**
	 * @brief Returns preset clear values for a given pass type.
	 *
	 * Color clear values come first, depth/stencil clear value last.
	 *
	 * @param passType Pass type to query.
	 * @return Vector of clear values sized to match the pass type attachments.
	 */
	static std::vector<vk::ClearValue> PresetClearValues(PassType passType);

	// --- Attachment load/store helpers ---

	static vk::AttachmentLoadOp  ColorLoadOpFor(PassType passType);
	static vk::AttachmentStoreOp ColorStoreOpFor(PassType passType);
	static vk::AttachmentLoadOp  DepthLoadOpFor(PassType passType);
	static vk::AttachmentStoreOp DepthStoreOpFor(PassType passType);

protected:
	Pass() = default;

	// --- Device / physical device references (non-owning) ---
	const vk::raii::Device*         p_device         = nullptr;
	const vk::raii::PhysicalDevice* p_physicalDevice = nullptr;

	/**
	 * @brief All pipelines owned by this pass.
	 *
	 * Subclasses push each Pipeline (graphics or compute) into this vector
	 * during their constructor via BuildPipeline().  Single-pipeline passes
	 * access p_pipelines[0]; multi-pipeline passes access p_pipelines[i].
	 */
	std::vector<Pipeline> p_pipelines;

	/**
	 * @brief Builds all pipelines for this pass and pushes them into
	 *        p_pipelines.
	 *
	 * Override in each subclass to create graphics / compute pipelines from
	 * the subclass's specific shaders, descriptor layouts, and push-constant
	 * ranges.  Each pipeline is push_back()'d into p_pipelines in order.
	 *
	 * @param device     Logical device.
	 * @param debugName  Human-readable name for the pass (e.g. "SSAOPass").
	 */
	virtual void BuildPipeline(const vk::raii::Device& device,
	                           const std::string& debugName)
	{}
};

} // namespace neurus
