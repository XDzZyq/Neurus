/**
 * @file DeferredRenderer.h
 * @brief Deferred rendering pipeline - GeometryPass → LightingPass → composite to swapchain.
 *
 * DeferredRenderer owns the full deferred pipeline and all scene GPU resources:
 *   - Swapchain management (reuses neurus::Swapchain)
 *   - RenderCache (G-Buffer, HDRColor)
 *   - GeometryPass (writes mesh geometry to G-Buffer MRT)
 *   - LightingPass (compute PBR lighting from G-Buffer into HDRColor)
 *   - Camera UBO data, mesh vertex/index buffers, light SSBO
 *   - Blit HDRColor to swapchain image for presentation
 *
 * Construction creates all Vulkan resources. DrawFrame() is the per-frame entry
 * point called from a QTimer in main.cpp.
 *
 * Non-copyable, non-movable (holds references to borrowed Vulkan objects).
 */

#pragma once

#include "GPUProfiler.h"
#include "ProfilingData.h"
#include "passes/Pass.h"
#include "RenderConfig.h"
#include "RenderContext.h"
#include "Swapchain.h"
#include "render_graph/RenderGraph.h"

#include <vulkan/vulkan_raii.hpp>

#include <glm/glm.hpp>
#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

namespace neurus {

// --- Forward declarations ---
class RenderCache;
class VertexBuffer;
class IndexBuffer;
class GPUBuffer;
class Camera;
class Scene;
class Mesh;
class Image;
class Environment;
class GeometryPass;
class LightingPass;
class SSAOPass;
class ShadowDepthPass;
class ShadowIntensityPass;
class SelectionOutlinePass;
class ComposePass;
class FXAAPass;
class DebugPass;
struct CameraUBOData;

/**
 * @brief Deferred renderer orchestrating GeometryPass → LightingPass → composite.
 *
 * Usage:
 *   DeferredRenderer renderer(device, physDev, queue, qfi, surface,
 *                              w, h);
 *   // SPIR-V shaders are embedded in DeferredRenderer.cpp - no params needed.
 *   // Each frame:
 *   renderer.DrawFrame();
 */
class DeferredRenderer
{
public:
	/**
	 * @brief Creates the full deferred pipeline.
	 *
	 * Construction order: Swapchain → RenderCache →
	 * GeometryPass → LightingPass → sync objects. Mesh buffers are uploaded
	 * directly to GPU by Mesh objects. LightingPass owns its own light SSBO.
	 * SPIR-V shaders are embedded at compile time (no constructor parameters needed).
	 *
	 * @param device           Logical device (borrowed, must outlive this object).
	 * @param physicalDevice   Physical device (borrowed).
	 * @param graphicsQueue    Graphics queue for submits and staging uploads.
	 * @param queueFamilyIndex Queue family for command pool creation.
	 * @param surface          Presentation surface (borrowed, must outlive swapchain).
	 * @param width            Initial window width.
	 * @param height           Initial window height.
	 */
	DeferredRenderer(const vk::raii::Device& device,
	                 const vk::raii::PhysicalDevice& physicalDevice,
	                 vk::Queue graphicsQueue,
	                 uint32_t queueFamilyIndex,
	                 const vk::raii::SurfaceKHR& surface,
	                 uint32_t width,
	                 uint32_t height);

	~DeferredRenderer();

	// --- Non-copyable, non-movable ---
	DeferredRenderer(const DeferredRenderer&) = delete;
	DeferredRenderer& operator=(const DeferredRenderer&) = delete;
	DeferredRenderer(DeferredRenderer&&) = delete;
	DeferredRenderer& operator=(DeferredRenderer&&) = delete;

	/**
	 * @brief Draws a single frame: acquire → record → submit → present.
	 *
	 * Receives the editor-produced RenderContext (scene pointer, render
	 * extent and frameIndex blank), fills in the render-specific fields
	 * from the swapchain, and dispatches to all render passes.
	 *
	 * Handles swapchain recreation on VK_ERROR_OUT_OF_DATE_KHR. Safe to
	 * call repeatedly (fence-guarded, max kMaxFramesInFlight in flight).
	 *
	 * Profiling is always on: the returned profile carries per-pass CPU
	 * times/counters from the frame just recorded and GPU times read back from
	 * the fence that just signaled (1-2 frame lag by design).
	 *
	 * @param editorCtx Editor-produced context with scene set.
	 * @return The latest frame profile (owned by the renderer; valid until the
	 *         next DrawFrame call).
	 */
	const FrameProfile& DrawFrame(const RenderContext& ctx);

	/** @brief Blocks until all GPU work completes. */
	void WaitIdle();

	/** @brief Returns the graphics queue. */
	vk::Queue GetGraphicsQueue() const { return r_graphicsQueue; }

	/** @brief Returns the graphics queue family index. */
	uint32_t GetGraphicsQueueFamily() const { return r_queueFamilyIndex; }

	/** @brief Returns the current swapchain extent. */
	vk::Extent2D GetExtent() const { return r_swapchain ? r_swapchain->extent() : vk::Extent2D{800, 600}; }

	/**
	 * @brief Returns the last acquired swapchain image for screenshot capture.
	 *
	 * The returned image is in PRESENT_SRC layout from the most recent frame.
	 * Caller must wait for GPU idle before using the image for readback.
	 *
	 * @return vk::Image handle (VK_NULL_HANDLE if swapchain not available or
	 *         image index is invalid).
	 */
	vk::Image GetLastSwapchainImage() const;

	/** @brief Returns the current swapchain image format. */
	vk::Format GetSwapchainFormat() const;

	/**
	 * @brief Returns a reference to the RenderCache for external access.
	 *
	 * Used by the Screenshot class to enumerate and capture attachments,
	 * shadow maps, and shadow intensity arrays.
	 */
	RenderCache& GetRenderCache() { return *r_renderCache; }

	const vk::raii::Device& GetDevice() const { return r_device; }

	/**
	 * @brief Reads a single pixel from the IDBuffer attachment and returns the object ID.
	 *
	 * Performs a GPU→CPU readback via Image::PickPixel() using a transient
	 * command buffer. The GPU is drained (queue.waitIdle) before the readback
	 * to ensure the IDBuffer from the most recent frame is available.
	 *
	 * The IDBuffer image state is restored to ColorAttachment afterward so
	 * the next GeometryPass can write to it.
	 *
	 * @param x Pixel x-coordinate (0 = left edge).
	 * @param y Pixel y-coordinate (0 = top edge, Vulkan convention).
	 * @return Object ID at the pixel (0 = background / no object), or 0 on OOB/failure.
	 */
	uint32_t ReadIDBufferPixel(uint32_t x, uint32_t y);

	/**
	 * @brief Handles viewport resize by proactively recreating swapchain and
	 *        dependent resources.
	 *
	 * Calls Swapchain::Recreate() with the new dimensions, then rebuilds
	 * attachments, semaphores, and command buffers. This avoids waiting for
	 * VK_ERROR_OUT_OF_DATE_KHR on the next AcquireNextImage, preventing a
	 * one-frame rendering glitch at the wrong size.
	 *
	 * Safe to call multiple times (idempotent for same dimensions).
	 * No-op if the window is minimized (zero-area surface).
	 *
	 * @param width  New window width in pixels.
	 * @param height New window height in pixels.
	 */
	void HandleResize(uint32_t width, uint32_t height);

	/**
	 * @brief Handles viewport recreation by rebuilding the swapchain with a new surface.
	 *
	 * Destroy the old swapchain and create a new one bound to the given surface.
	 * Rebuilds all dependent resources (attachments, semaphores, command buffers).
	 * Used when the Viewport widget is recreated with a new native HWND.
	 *
	 * @param newSurface New presentation surface (from the recreated Viewport).
	 */
	void HandleSurfaceChange(const vk::raii::SurfaceKHR& newSurface);

	/**
	 * @brief Resets shadow accumulation. Called by Editor when camera/light/scene changes.
	 * Sets the global shadow frame counter to 0, causing the next frame to overwrite (alpha=1).
	 */
	void ResetShadowAccumulation();

private:
	/**
	 * @brief Records the full deferred pipeline into a command buffer.
	 *
	 * Sequence:
	 *   1. GeometryPass::Record() → G-Buffer MRT (reads scene.mesh_list via MeshGPU)
	 *   2. ShadowDepthPass::Record() → cubemap depth (reads scene.mesh_list via MeshGPU)
	 *   3. LightingPass::Record() → compute PBR → HDRColor (uses own light SSBO)
	 *   4. Blit HDRColor → swapchain image
	 *   5. Transition swapchain image to present layout
	 *
	 * @param editorCtx Editor-produced context (scene set; width/height/frameIndex blank).
	 */
	void recordFrame(const vk::raii::CommandBuffer& cmdBuf, uint32_t imageIndex,
	                 const RenderContext& editorCtx);

	/** @brief Destroys and re-creates sync objects after swapchain resize. */
	void recreateSwapchain();

	/** @brief Creates the command pool (static helper for init-list use). */
	static vk::raii::CommandPool createCommandPool(const vk::raii::Device& device,
	                                               uint32_t queueFamilyIndex);

	// --- Borrowed objects ---
	const vk::raii::Device& r_device;
	const vk::raii::PhysicalDevice& r_physicalDevice;
	vk::Queue r_graphicsQueue;
	uint32_t r_queueFamilyIndex;

	// --- Swapchain ---
	std::unique_ptr<Swapchain> r_swapchain;

	// --- Deferred pipeline ---
	std::unique_ptr<RenderCache> r_renderCache;
	// --- Polymorphic pass container (owning) ---
	std::vector<std::unique_ptr<Pass>> r_passes;

	// --- Cached raw pointers for zero-cost access (non-owning) ---
	GeometryPass* r_geometryPass = nullptr;
	LightingPass* r_lightingPass = nullptr;
	SSAOPass*   r_ssaoPass     = nullptr;
	ShadowDepthPass* r_shadowDepthPass = nullptr;
	ShadowIntensityPass* r_shadowIntensityPass = nullptr;
	SelectionOutlinePass* r_selectionOutlinePass = nullptr;
	ComposePass*  r_composePass  = nullptr;
	FXAAPass*     r_fxaaPass     = nullptr;
	/// Debug/gizmo overlay; always in the graph, runs last, and a no-op when the
	/// list is empty. Its target doubles as the swapchain blit source.
	DebugPass*    r_debugPass    = nullptr;

	// --- RenderGraph (the active pipeline) ---
	// Compile-once-per-topology DAG holding the whole deferred pipeline:
	// Geometry → Shadows → SSAO → Lighting → {SelectionOutline} → Compose → [FXAA] → Debug.
	// recordFrame dispatches the entire pipeline via m_mainGraph.Execute(),
	// rebuilding it (RebuildMainGraph) only when the pipeline signature
	// derived from RenderConfig changes.
	RenderGraph m_mainGraph;

	/**
	 * @brief Topology-affecting projection of RenderConfig.
	 *
	 * RenderConfig is the single source of truth for render settings. The
	 * graph is a *derived* view of it: this signature captures exactly the
	 * config choices that change which passes are in the graph. recordFrame
	 * compares the current signature against the last-built one and rebuilds
	 * the graph only when they differ — so there is no independent per-node
	 * "enabled" state to fall out of sync with RenderConfig.
	 *
	 * Add a field here whenever a new pass becomes conditional (e.g. AO/shadow
	 * once those passes are graph-managed). This struct is the one place that
	 * maps config → graph topology.
	 */
	struct PipelineSignature
	{
		bool fxaa = false; ///< FXAAPass present (RenderConfig::RequiresFXAA()).

		static PipelineSignature From(const RenderConfig& cfg)
		{
			return PipelineSignature{ cfg.RequiresFXAA() };
		}

		bool operator==(const PipelineSignature&) const = default;
	};

	/// Signature the current m_mainGraph was built for (dirty-key, not truth).
	PipelineSignature m_builtSignature;

	/**
	 * @brief Rebuild m_mainGraph to match a pipeline signature.
	 * @param sig  Projection of RenderConfig deciding which passes participate.
	 */
	void RebuildMainGraph(const PipelineSignature& sig);

	// --- Command pool ---
	vk::raii::CommandPool r_commandPool;

	// --- Command buffers (one per swapchain image, reused each frame) ---
	std::vector<vk::raii::CommandBuffer> r_commandBuffers;

	// --- Synchronization ---
	// One frame in flight. RenderCache::GetAttachment() returns a single shared
	// Image per AttachmentName, so a second in-flight frame would write
	// ComposedOutput/Depth while the previous frame's blit still reads them —
	// two submits with no dependency between them. The symptom was a presented
	// image built from two different frames (tile-shaped holes, geometry from
	// frame N under an overlay from frame N+1). Raising this above 1 requires
	// per-frame-slot attachments first.
	static constexpr uint32_t kMaxFramesInFlight = 1;
	/// Deadlock guard, not a frame budget. A Debug build with validation layers on
	/// spends ~600 ms per frame here, so the old 100 ms made waitForFences return
	/// eTimeout routinely and DrawFrame abandon a frame it had already acquired an
	/// image for. Only a genuinely hung GPU should trip this.
	static constexpr uint64_t kFenceTimeoutNs = 5'000'000'000;

	std::vector<vk::raii::Fence> r_inFlightFences;
	std::vector<vk::raii::Semaphore> r_imageAvailableSemaphores;
	std::vector<vk::raii::Semaphore> r_renderFinishedSemaphores;
	uint32_t r_currentFrame = 0;
	uint32_t r_swapchainGeneration = 0;

	// --- Last acquired swapchain image index ---
	uint32_t r_lastImageIndex = 0;

	// --- Temporal shadow accumulation state ---
	uint32_t m_haltonIndex = 0;
	uint32_t m_iteration = 0;

	// --- Per-frame profiling (always on; RenderGraph drives GPU sections) ---
	// m_profiler owns the GPU timestamp query pool and per-pass sections;
	// RenderGraph::Execute brackets each pass with BeginPass/EndPass and folds
	// PassStats into m_frameProfile. DrawFrame calls Resolve() after the frame
	// slot's fence signals and back-fills the resolved GPU ms into m_frameProfile
	// once recordFrame has repopulated the pass list (1-2 frame lag by design).
	GPUProfiler m_profiler;
	FrameProfile m_frameProfile;
	bool m_gpuResolved = false; ///< Last Resolve() produced fresh GPU sections.

	/// @brief Latches the "no active camera" report so it is logged once, not
	///        once per frame, while the unrenderable scene is on screen.
	bool m_reportedNoCamera = false;

};

} // namespace neurus
