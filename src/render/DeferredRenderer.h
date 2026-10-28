/**
 * @file DeferredRenderer.h
 * @brief Deferred rendering pipeline - GeometryPass → LightingPass → composite to swapchain.
 *
 * DeferredRenderer owns the full deferred pipeline and all scene GPU resources:
 *   - Swapchain management (reuses neurus::Swapchain)
 *   - RenderCache (G-Buffer, HDRColor, overlay output)
 *   - GeometryPass (writes mesh geometry to G-Buffer MRT)
 *   - LightingPass (compute PBR lighting from G-Buffer into HDRColor)
 *   - Camera UBO data, mesh vertex/index buffers, light SSBO
 *   - OverlayEngine (selection outline, debug overlay, gizmo) - a separate graph
 *     over the scene graph's tail image
 *   - Blit of the overlay output to the swapchain image for presentation
 *
 * Progressive accumulation: DrawFrame() draws at most r_sampling_count scene
 * frames after every restart, then stops. What is left to do is published by
 * NextWork() so the render thread can wait for the right thing: another sample,
 * a redraw of the overlays only, or nothing at all.
 *
 * Construction creates all Vulkan resources. DrawFrame() is the per-frame entry
 * point, driven either by a QTimer in single-threaded mode or by RenderThread
 * once the renderer runs on its own thread.
 *
 * Implements IFrameRenderer, the interface RenderThread sequences; it is also
 * the single-threaded RenderRequestSink the Editor posts to directly.
 *
 * Non-copyable, non-movable (holds references to borrowed Vulkan objects).
 */

#pragma once

#include "GPUProfiler.h"
#include "IFrameRenderer.h"
#include "ProfilingData.h"
#include "passes/Pass.h"
#include "core/RenderConfig.h"
#include "RenderContext.h"
#include "RenderRequestQueue.h"
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
class ComposePass;
class FXAAPass;
class OverlayEngine;
struct CameraUBOData;
class Shader;
class UploadManager;
class RenderRequestExecutor;
class Screenshot;
class SharedQueue;

/**
 * @brief Deferred renderer orchestrating GeometryPass → LightingPass → composite.
 */
class DeferredRenderer : public IFrameRenderer
{
public:
	/**
	 * @brief Creates the full deferred pipeline.
	 *
	 * @param device           Logical device (borrowed, must outlive this object).
	 * @param physicalDevice   Physical device (borrowed).
	 * @param graphicsQueue    Serialised graphics queue (borrowed, must outlive
	 *                         this object) for submits, presents and readbacks.
	 * @param queueFamilyIndex Queue family for command pool creation.
	 * @param surface          Presentation surface (borrowed, must outlive swapchain).
	 * @param width            Initial window width.
	 * @param height           Initial window height.
	 */
	DeferredRenderer(const vk::raii::Device& device,
	                 const vk::raii::PhysicalDevice& physicalDevice,
	                 SharedQueue& graphicsQueue,
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
	 * @brief Draws one frame: acquire, record, submit, present.
	 *
	 * Performs exactly NextWork(): a Sample frame runs the scene graph and then
	 * the overlay graph, an Overlay frame runs the overlay graph only, an Idle
	 * frame returns without acquiring a swapchain image at all. Fills in the
	 * render-specific fields of @p ctx from the swapchain and handles swapchain
	 * recreation on VK_ERROR_OUT_OF_DATE_KHR. Safe to call repeatedly
	 * (fence-guarded, kMaxFramesInFlight frames in flight).
	 *
	 * @param ctx RenderContext with the scene set.
	 * @return The latest frame profile (owned by the renderer; valid until the
	 *         next DrawFrame call).
	 */
	const FrameProfile& DrawFrame(const RenderContext& ctx) override;

	/**
	 * @brief What the next DrawFrame() will do.
	 *
	 * Order, and why:
	 * - Retry when the last DrawFrame drew nothing (no camera, a failed acquire,
	 *   a swapchain that went out of date). The caller must wait for a new frame
	 *   rather than spin, and must not treat the frame it holds as drawn.
	 * - Sample while fewer than N scene frames have been accumulated since the
	 *   last restart, where N = max(1, RenderConfig::r_sampling_count) as read on
	 *   the last sample frame. A change to it always arrives with a restart, so
	 *   the cached value is never stale by the time it is compared.
	 * - Overlay when accumulation has converged but an overlay-only change is
	 *   pending: redraw the overlays over the converged scene image.
	 * - Idle otherwise: nothing to draw until a request or control arrives.
	 */
	FrameWork NextWork() const override;

	/**
	 * @brief Blocks until the frame slot about to be reused is free on the GPU.
	 *
	 * Extracted from the top of DrawFrame so RenderThread can gate control
	 * messages and request drains on the same fence wait a draw would do. The
	 * fence is reset only on the submit path in DrawFrame, never here, so a
	 * caller may wait, skip the frame, and wait again without deadlocking.
	 *
	 * @return False on timeout (a hung GPU); nothing may touch GPU resources then.
	 */
	bool WaitForFrameSlot() override;

	/** @brief Blocks until the graphics queue completes its work. */
	void WaitIdle() override;

	/** @brief Returns the graphics queue. */
	SharedQueue& GetGraphicsQueue() const { return *r_graphicsQueue; }

	/** @brief Returns the graphics queue family index. */
	uint32_t GetGraphicsQueueFamily() const { return r_queueFamilyIndex; }

	/** @brief Returns the current swapchain extent. */
	vk::Extent2D GetExtent() const override { return r_swapchain ? r_swapchain->extent() : vk::Extent2D{800, 600}; }

	/**
	 * @brief Returns the last acquired swapchain image for screenshot capture.
	 *
	 * The returned image is in PRESENT_SRC layout from the most recent frame.
	 * A caller's readback is submitted after the frame on the same queue, so its
	 * barrier orders it after the frame's writes, and it waits its own submit -
	 * no idle wait is needed.
	 *
	 * @return vk::Image handle (VK_NULL_HANDLE if swapchain not available or
	 *         image index is invalid).
	 */
	vk::Image GetLastSwapchainImage() const;

	/** @brief Returns the current swapchain image format. */
	vk::Format GetSwapchainFormat() const;

	/**
	 * @brief Returns a reference to the RenderCache for external access.
	 */
	RenderCache& GetRenderCache() { return *r_renderCache; }

	const vk::raii::Device& GetDevice() const { return r_device; }

	/**
	 * @brief Reads a single pixel from the IDBuffer attachment and returns the object ID.
	 *
	 * The readback is its own submission through the shared queue and waits that
	 * submission before mapping the result, so the IDBuffer from the most recent
	 * frame is what gets read without idling the device. The readback restores
	 * the image to the state it entered in, within the same submission.
	 *
	 * @param x Pixel x-coordinate (0 = left edge).
	 * @param y Pixel y-coordinate (0 = top edge, Vulkan convention).
	 * @return Object ID at the pixel (0 = background / no object), or 0 on OOB/failure.
	 *         Also 0 when no ID buffer exists at the current extent or it has
	 *         never been drawn into - a pick is a query, it never creates one.
	 */
	uint32_t ReadIDBufferPixel(uint32_t x, uint32_t y) override;

	/**
	 * @brief Handles viewport resize by proactively recreating swapchain and
	 *        dependent resources.
	 *
	 * Recreates the swapchain with the new dimensions and rebuilds attachments,
	 * semaphores and command buffers, avoiding a one-frame rendering glitch at
	 * the wrong size.
	 *
	 * Safe to call multiple times (idempotent for same dimensions).
	 * No-op if the window is minimized (zero-area surface).
	 *
	 * @param width  New window width in pixels.
	 * @param height New window height in pixels.
	 */
	void HandleResize(uint32_t width, uint32_t height) override;

	/**
	 * @brief Handles viewport recreation by rebuilding the swapchain with a new surface.
	 *
	 * Rebuilds all dependent resources (attachments, semaphores, command buffers).
	 *
	 * @param newSurface New presentation surface.
	 */
	void HandleSurfaceChange(const vk::raii::SurfaceKHR& newSurface) override;

	/**
	 * @brief Resets shadow accumulation, so the next frame overwrites (alpha = 1).
	 */
	void ResetShadowAccumulation();

	/**
	 * @brief Post one GPU request for the renderer to execute at the next drain.
	 *
	 * The only entry point the Editor uses to drive the renderer's GPU state:
	 * upload a mesh, allocate a light's shadow maps, regenerate IBL, rewrite the
	 * lighting SSBOs, warm a pipeline, drop the outgoing scene, reset
	 * accumulation. Requests are executed FIFO at the top of DrawFrame (see
	 * RenderRequests.h). Returns at once; nothing blocks the caller.
	 *
	 * @param request  The request; moved into the queue, never copied.
	 */
	void Post(RenderRequest&& request) override;

	/**
	 * @brief Drain the request queue and execute every pending request, FIFO.
	 *
	 * Called by DrawFrame after the frame-slot fence wait, but public so a test
	 * or a non-drawing caller can flush the queue without a frame. Two kinds are
	 * handled here because they write renderer state the executor does not own:
	 * ResetAccumulationRequest and RedrawOverlaysRequest. Every other request
	 * restarts accumulation (m_iteration = 0) before it is delegated to the
	 * executor, so the next frame samples from scratch; that is the convergence
	 * contract IFrameRenderer::NextWork() documents.
	 */
	void ProcessRequests() override;

	/**
	 * @brief Write the last presented image, or every attachment, to disk.
	 *
	 * Owns a Screenshot built in the constructor from the same device, physical
	 * device, queue and family the renderer draws with; the Screenshot's own
	 * capture methods do the GPU sync (a blocking submit per readback) internally.
	 *
	 * @param allAttachments  False: the swapchain image. True: every attachment.
	 */
	void CaptureScreenshot(bool allAttachments) override;

	/**
	 * @brief Sets the upload service the request executor uses (non-owning).
	 *
	 * Called by the Application once the UploadManager exists. The executor is
	 * built in this object's constructor, before the UploadManager, so it starts
	 * without one and a mesh/light/environment request posted before this call
	 * is logged once and skipped.
	 *
	 * @param uploader  Upload service, borrowed; must outlive this renderer.
	 */
	void SetUploadManager(UploadManager* uploader);

private:
	/**
	 * @brief Records one frame's work into a command buffer.
	 *
	 * Rebuilds the scene graph when the pipeline signature changes, then records
	 * exactly @p work: Sample runs the scene graph followed by the overlay graph,
	 * Overlay runs the overlay graph only. Both end with the OverlayOutput ->
	 * swapchain blit, because the scene image is never presented directly.
	 *
	 * @param cmdBuf     Command buffer to record into.
	 * @param imageIndex Swapchain image index being rendered into.
	 * @param editorCtx  Render context (scene set; width/height/frameIndex blank).
	 * @param work       Sample or Overlay - never Idle/Retry, which draw nothing.
	 */
	void recordFrame(const vk::raii::CommandBuffer& cmdBuf, uint32_t imageIndex,
	                 const RenderContext& editorCtx, FrameWork work);

	/** @brief Destroys and re-creates sync objects after swapchain resize. */
	void recreateSwapchain();

	/**
	 * @brief Waits every in-flight frame fence, so no recorded frame is still
	 *        reading the resources a handler is about to replace or free.
	 *
	 * On a failed wait (timeout, device loss) it logs the result and falls back
	 * to an idle queue: the caller must be able to release what it replaces, and
	 * a fence that never signals cannot prove the GPU is done.
	 */
	void DrainInFlightFrames();

	/** @brief Creates the command pool (static helper for init-list use). */
	static vk::raii::CommandPool createCommandPool(const vk::raii::Device& device,
	                                               uint32_t queueFamilyIndex);

	// --- Borrowed objects ---
	const vk::raii::Device& r_device;
	const vk::raii::PhysicalDevice& r_physicalDevice;
	SharedQueue* r_graphicsQueue = nullptr;
	uint32_t r_queueFamilyIndex;

	// --- Swapchain ---
	std::unique_ptr<Swapchain> r_swapchain;

	// --- Deferred pipeline ---
	std::unique_ptr<RenderCache> r_renderCache;
	// Screenshot capture service; declared right after r_renderCache so it is
	// destroyed BEFORE the cache it reads during an all-attachments capture.
	std::unique_ptr<Screenshot> m_screenshot;
	// --- Polymorphic pass container (owning) ---
	std::vector<std::unique_ptr<Pass>> r_passes;

	// --- Cached raw pointers for zero-cost access (non-owning) ---
	GeometryPass* r_geometryPass = nullptr;
	LightingPass* r_lightingPass = nullptr;
	SSAOPass*   r_ssaoPass     = nullptr;
	ShadowDepthPass* r_shadowDepthPass = nullptr;
	ShadowIntensityPass* r_shadowIntensityPass = nullptr;
	ComposePass*  r_composePass  = nullptr;
	FXAAPass*     r_fxaaPass     = nullptr;

	// --- Overlay graph (owned) ---
	// Selection outline, debug overlay and gizmo, in their own RenderGraph over
	// the scene graph's tail image. Separate so an overlay-only change redraws
	// over the converged image without re-running a single scene pass; see
	// OverlayEngine.h for the graph shape.
	std::unique_ptr<OverlayEngine> m_overlayEngine;

	// --- RenderGraph (the active pipeline) ---
	// Compile-once-per-topology DAG holding the whole deferred pipeline; recordFrame
	// dispatches it and rebuilds it (RebuildMainGraph) only when the pipeline
	// signature derived from RenderConfig changes.
	RenderGraph m_mainGraph;

	/**
	 * @brief Topology-affecting projection of RenderConfig.
	 *
	 * The graph is a derived view of RenderConfig: this signature captures
	 * exactly the config choices that change which passes are in the graph,
	 * so the graph is rebuilt only when they differ - there is no independent
	 * per-node "enabled" state to fall out of sync with RenderConfig.
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
	// One frame in flight: the render cache returns one shared attachment per
	// name, so a second in-flight frame would write an attachment the previous
	// frame's blit still reads. Raising this above 1 requires per-frame-slot
	// attachments first.
	static constexpr uint32_t kMaxFramesInFlight = 1;
	/// Deadlock guard, not a frame budget: a Debug build with validation layers
	/// spends ~600 ms per frame here, so a short timeout would fire routinely.
	/// Only a genuinely hung GPU should trip this.
	static constexpr uint64_t kFenceTimeoutNs = 5'000'000'000;

	std::vector<vk::raii::Fence> r_inFlightFences;
	std::vector<vk::raii::Semaphore> r_imageAvailableSemaphores;
	std::vector<vk::raii::Semaphore> r_renderFinishedSemaphores;
	uint32_t r_currentFrame = 0;
	uint32_t r_swapchainGeneration = 0;

	// --- Last acquired swapchain image index ---
	uint32_t r_lastImageIndex = 0;

	// --- Temporal accumulation state ---
	uint32_t m_haltonIndex = 0;
	uint32_t m_iteration = 0;

	/// @brief Scene frames to accumulate after a restart; max(1, r_sampling_count).
	///
	/// Refreshed on every sample frame, which is the only place it is compared
	/// against m_iteration and the only place a change can matter: a new
	/// r_sampling_count always arrives with an accumulation restart, so the next
	/// comparison uses the fresh value.
	uint32_t m_samplingCount = 64;

	/// @brief An overlay-only redraw is pending (a RedrawOverlaysRequest arrived).
	bool m_overlaysDirty = false;

	/// @brief The last DrawFrame drew nothing (no camera, failed acquire).
	///        NextWork() reports Retry while set, so the caller waits for a new
	///        frame instead of redrawing a stale one.
	bool m_drewNothing = false;

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

	// --- Editor-posted GPU requests (drained at the top of DrawFrame) ---
	// The queue is the handoff from the Editor; the executor runs each request
	// against the device, the cache and the passes. Both single-threaded today;
	// the queue carries the mutex a future render thread needs.
	RenderRequestQueue                     m_requestQueue;
	std::unique_ptr<RenderRequestExecutor> m_executor;

};

} // namespace neurus
