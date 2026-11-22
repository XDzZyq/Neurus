/**
 * @file OverlayEngine.hpp
 * @brief The overlay graph: everything drawn on top of the converged scene image.
 *
 * The scene graph (DeferredRenderer::m_mainGraph) ends at the composed, tone
 * mapped image. The overlays - selection outline, debug primitives and the
 * transform gizmo - live in their own graph here, so they can be redrawn
 * without re-running a single scene pass once accumulation has converged, and so
 * they never smear into FXAA.
 *
 * The two graphs exchange data only through RenderCache attachments:
 *
 * @code
 * scene tail (ComposedOutput | FXAAOutput)
 *     v
 * SelectionOutlinePass  (IDBuffer -> SelectionOutline mask)
 *     v
 * OverlayCopyPass       (scene tail + mask -> OverlayOutput)   <- the copy
 *     v
 * DebugPass             (OverlayOutput in place, Depth read)
 *     v
 * GizmoPass             (OverlayOutput in place)
 * @endcode
 *
 * The copy is what makes an overlay frame possible: the converged scene image is
 * never written in place, so redrawing the overlays always starts from a clean
 * copy rather than from whatever the previous overlay frame left behind.
 *
 * Owned by DeferredRenderer and driven on the render thread; it borrows nothing
 * beyond the device it was built with.
 */

#pragma once

#include "../RenderCache.hpp"
#include "../passes/ComputePass.hpp"
#include "../render_graph/RenderGraph.hpp"
#include "shader/ComputeShader.hpp"

#include <vulkan/vulkan_raii.hpp>

#include <memory>

namespace neurus {

class DebugPass;
class GizmoPass;
class SelectionOutlinePass;
struct RenderContext;
class GPUProfiler;
struct FrameProfile;

// ---------------------------------------------------------------------------
// OverlayCopyPass
// ---------------------------------------------------------------------------

/**
 * @brief Copies the scene tail into OverlayOutput and blends the outline mask.
 *
 * An implementation detail of OverlayEngine, public only so a test can record it
 * against a primed RenderCache without building the whole overlay graph.
 *
 * Bindings:
 *   0: scene tail  (combined image sampler)
 *   1: outline mask (combined image sampler, R8)
 *   2: OverlayOutput (storage image, RGBA16F)
 *
 * Copy and blend are one dispatch on purpose: pixel-wise, blending over the copy
 * is what "copy, then read-modify-write" would compute, and splitting it would
 * mean sampling OverlayOutput while storing to it - a feedback loop Vulkan
 * leaves undefined.
 */
class OverlayCopyPass : public ComputePass
{
public:
	/**
	 * @brief Constructs the pass and its pipeline.
	 * @param device          Logical device (retained reference).
	 * @param physicalDevice  Physical device (sampler creation).
	 * @param numSets         Descriptor sets, one per in-flight frame.
	 */
	OverlayCopyPass(const vk::raii::Device& device,
	                const vk::raii::PhysicalDevice& physicalDevice,
	                uint32_t numSets);

	/**
	 * @brief Out-of-line: p_shader's deleter needs ComputeShader's definition,
	 *        which this header does not pull in.
	 */
	~OverlayCopyPass() override;

	/** @brief Selects a legacy attachment or an authored scene output, respectively. */
	void SetSource(AttachmentName source) { p_source = source; p_sourceKey.clear(); }
	void SetResourceKey(const std::string& key) { p_sourceKey = key; }

	/** @brief The scene image this pass copies from. */
	AttachmentName GetSource() const { return p_source; }

	/**
	 * @brief Records the copy + highlight blend dispatch.
	 *
	 * Transitions the source and the mask to ColorShaderRead and OverlayOutput to
	 * ShaderWrite, then dispatches ceil(w/16) x ceil(h/16) groups.
	 */
	PassStats Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx) override;

	/** @brief Declares the source + mask reads and the OverlayOutput write. */
	PassIO GetIO() const override;

	/** @brief Writes the source, mask and OverlayOutput descriptors. */
	void WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache) override;

private:
	/** @brief Creates the set layout (source, mask, output). */
	static DescriptorSetLayout CreateDescriptorSetLayout(const vk::raii::Device& device);

	/** @brief Builds the compute pipeline from the self-loaded shader. */
	void BuildPipeline(const vk::raii::Device& device, const std::string& debugName) override;

	/// @brief Self-loaded compute shader (via ShaderLibrary).
	std::unique_ptr<ComputeShader> p_shader;

	/// @brief Scene image copied from - the scene graph's tail.
	Image& Source(RenderCache& cache, vk::Extent2D extent) const;
	std::string p_sourceKey;
	AttachmentName p_source = AttachmentName::ComposedOutput;
};

// ---------------------------------------------------------------------------
// OverlayEngine
// ---------------------------------------------------------------------------

/**
 * @brief Owns the overlay passes and the graph that orders them.
 *
 * Non-copyable, non-movable: it owns GPU resources and a graph whose nodes point
 * at them.
 */
class OverlayEngine
{
public:
	/**
	 * @brief Builds the overlay passes and compiles the overlay graph.
	 *
	 * @param device          Logical device (retained reference).
	 * @param physicalDevice  Physical device (retained reference).
	 * @param numSets         Descriptor sets per pass, one per in-flight frame.
	 */
	OverlayEngine(const vk::raii::Device& device,
	              const vk::raii::PhysicalDevice& physicalDevice,
	              uint32_t numSets);

	~OverlayEngine();

	// --- Non-copyable, non-movable (owns GPU resources) ---
	OverlayEngine(const OverlayEngine&)            = delete;
	OverlayEngine& operator=(const OverlayEngine&) = delete;
	OverlayEngine(OverlayEngine&&)                 = delete;
	OverlayEngine& operator=(OverlayEngine&&)      = delete;

	/** @brief Selects the scene image and enables consumers of the published depth and object ID. */
	void SetSceneOutputs(const std::string& color, bool depth, bool objectId);
	bool HasObjectId() const { return m_hasObjectId; }

	/** @brief The attachment the swapchain blit reads: OverlayOutput. */
	static constexpr AttachmentName OutputAttachment() { return AttachmentName::OverlayOutput; }

	/**
	 * @brief Executes the overlay graph, appending its passes to the profile.
	 *
	 * Uses the caller's profiler so the overlay passes appear in the same frame
	 * profile as the scene passes, with GPU timings resolved by the same
	 * back-fill: overlay sections follow the scene sections in query order and in
	 * the profile's pass list.
	 *
	 * @param cmd          Recording command buffer.
	 * @param cache        Render cache holding every attachment.
	 * @param ctx          Per-frame context (extent, frame index, camera).
	 * @param profiler     The renderer's GPU profiler (already inside BeginFrame).
	 * @param frameProfile Profile to append to; its scene passes are preserved.
	 */
	void Execute(vk::CommandBuffer cmd,
	             RenderCache& cache,
	             const RenderContext& ctx,
	             GPUProfiler& profiler,
	             FrameProfile& frameProfile);

	/** @brief The selection-outline mask pass, for tests and diagnostics. */
	SelectionOutlinePass* GetSelectionOutlinePass() { return m_selectionOutline.get(); }

	/** @brief The debug overlay pass. */
	DebugPass* GetDebugPass() { return m_debug.get(); }

	/** @brief The gizmo overlay pass. */
	GizmoPass* GetGizmoPass() { return m_gizmo.get(); }

	/** @brief The copy pass that seeds OverlayOutput. */
	OverlayCopyPass* GetCopyPass() { return m_copy.get(); }

	/** @brief Number of passes in the compiled overlay graph. */
	size_t PassCount() const { return m_graph.PassCount(); }

private:
	/** @brief Re-registers every pass and rewires the graph's edges. */
	void RebuildGraph();

	// --- Passes (owned; the graph holds non-owning pointers) ---
	std::unique_ptr<SelectionOutlinePass> m_selectionOutline;
	std::unique_ptr<OverlayCopyPass>       m_copy;
	std::unique_ptr<DebugPass>             m_debug;
	std::unique_ptr<GizmoPass>             m_gizmo;

	// --- The overlay DAG ---
	RenderGraph m_graph;

	std::string m_sceneColor;
	bool m_hasDepth = true;
	bool m_hasObjectId = true;
};

} // namespace neurus
