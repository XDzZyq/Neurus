/**
 * @file DeferredRenderer.cpp
 * @brief Deferred rendering pipeline implementation.
 */

#include "DeferredRenderer.h"

#include "Barrier.h"
#include "Image.h"
#include "RenderCache.h"
#include "RenderContext.h"
#include "RenderFrameSnapshot.h"
#include "Swapchain.h"

#include "resources/LightingCache.h"
#include "resources/PipelineCache.h"

#include "shaders/Shader.h"

#include "RenderRequestExecutor.h"
#include "Screenshot.h"
#include "SharedQueue.h"
#include "UploadManager.h"

#include "overlay/OverlayEngine.h"

#include "passes/GeometryPass.h"
#include "passes/LightingPass.h"
#include "passes/SSAOPass.h"
#include "passes/ShadowDepthPass.h"
#include "passes/ShadowIntensityPass.h"
#include "passes/ComposePass.h"
#include "passes/FXAAPass.h"

#include "render/HaltonSequence.h"

#include "scene/Light.h"
#include "scene/Scene.h"

#include "core/Log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace neurus {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

DeferredRenderer::DeferredRenderer(const vk::raii::Device& device,
                                    const vk::raii::PhysicalDevice& physicalDevice,
                                    SharedQueue& graphicsQueue,
                                    uint32_t queueFamilyIndex,
                                    const vk::raii::SurfaceKHR& surface,
                                    uint32_t width,
                                    uint32_t height)
	: r_device(device)
	, r_physicalDevice(physicalDevice)
	, r_graphicsQueue(&graphicsQueue)
	, r_queueFamilyIndex(queueFamilyIndex)
	, r_commandPool(createCommandPool(device, queueFamilyIndex))
{
	// --- 0. Create the optional GPU timestamp query pool (no-op if unsupported) ---
	m_profiler.Initialize(device, physicalDevice, queueFamilyIndex, kMaxFramesInFlight);

	// --- 1. Create swapchain ---
	r_swapchain = std::make_unique<Swapchain>(physicalDevice, device, graphicsQueue, surface, width, height);
	const auto extent = r_swapchain->extent();

	// --- 2. Create G-Buffer attachment cache (lazy creation on first access) ---
	r_renderCache = std::make_unique<RenderCache>(device, physicalDevice);

	// --- 2b. Create the screenshot capture service ---
	// Same Vulkan handles the renderer draws with; its capture methods sync the
	// GPU themselves. Built after r_renderCache (an all-attachments capture reads
	// it) and destroyed before it, per the member declaration order.
	m_screenshot = std::make_unique<Screenshot>(
		r_device, r_physicalDevice, graphicsQueue, r_queueFamilyIndex);

	// --- 3. Create geometry pass ---
	{
		auto geoPass = std::make_unique<GeometryPass>(
			device, physicalDevice);
		r_geometryPass = geoPass.get();
		r_passes.push_back(std::move(geoPass));
	}

	// --- 5. Create lighting pass ---
	{
		auto lightPass = std::make_unique<LightingPass>(
			device, physicalDevice,
			kMaxFramesInFlight);
		r_lightingPass = lightPass.get();
		r_passes.push_back(std::move(lightPass));
	}

	// --- 6. Create SSAO pass ---
	{
		auto ssaoPass = std::make_unique<SSAOPass>(
			device, physicalDevice,
			kMaxFramesInFlight,
			graphicsQueue, queueFamilyIndex);
		r_ssaoPass = ssaoPass.get();
		r_passes.push_back(std::move(ssaoPass));
	}

	// --- 7. Create shadow depth pass (cubemap depth from light's POV) ---
	{
		auto shadowDepth = std::make_unique<ShadowDepthPass>(
			device, physicalDevice,
			graphicsQueue, queueFamilyIndex,
			ShadowDepthPass::kDefaultResolution);
		r_shadowDepthPass = shadowDepth.get();
		r_passes.push_back(std::move(shadowDepth));
		NEURUS_LOG("[DeferredRenderer] ShadowDepthPass created");
	}

	// --- 8b. Create shadow intensity pass (per-pixel shadow evaluation from cubemap) ---
	{
		auto shadowIntensity = std::make_unique<ShadowIntensityPass>(
			device, physicalDevice,
			kMaxFramesInFlight);
		r_shadowIntensityPass = shadowIntensity.get();
		r_passes.push_back(std::move(shadowIntensity));
		NEURUS_LOG("[DeferredRenderer] ShadowIntensityPass created");
	}

	// --- 8c. Build the overlay graph (selection outline, debug overlay, gizmo) ---
	// Separate from the scene graph so overlays can be redrawn over a converged
	// scene image without re-running a single scene pass. RebuildMainGraph below
	// tells it which scene image to draw over.
	m_overlayEngine = std::make_unique<OverlayEngine>(
		device, physicalDevice, kMaxFramesInFlight);
	NEURUS_LOG("[DeferredRenderer] OverlayEngine created ("
	           << m_overlayEngine->PassCount() << " overlay passes)");

	// --- 8d. Create compose pass (tone map + gamma correction) ---
	{
		auto composePass = std::make_unique<ComposePass>(
			device, physicalDevice,
			kMaxFramesInFlight);
		r_composePass = composePass.get();
		r_passes.push_back(std::move(composePass));
		NEURUS_LOG("[DeferredRenderer] ComposePass created");
	}

	// --- 8e. Create FXAA pass (luma-based anti-aliasing) ---
	{
		auto fxaaPass = std::make_unique<FXAAPass>(
			device, physicalDevice,
			kMaxFramesInFlight);
		r_fxaaPass = fxaaPass.get();
		r_passes.push_back(std::move(fxaaPass));
		NEURUS_LOG("[DeferredRenderer] FXAAPass created");
	}

	// --- 8f. Build the scene RenderGraph ---
	// Initial build uses a default signature (no FXAA); recordFrame rebuilds
	// it whenever the pipeline signature derived from RenderConfig changes.
	RebuildMainGraph(PipelineSignature{});

	// --- 9. Allocate command buffers (one per swapchain image, reused) ---
	uint32_t imageCount = r_swapchain->imageCount();

	const bool hasTransferDst = (r_swapchain->actualImageUsage() & vk::ImageUsageFlagBits::eTransferDst) != vk::ImageUsageFlags{};
	if (!hasTransferDst)
	{
		throw std::runtime_error(
			"Swapchain surface does not support VK_IMAGE_USAGE_TRANSFER_DST_BIT.\n"
			"DeferredRenderer requires TRANSFER_DST for the HDRColor-to-swapchain blit composite.\n"
			"Try running on a GPU/driver that supports this usage flag for the surface format.");
	}

	vk::CommandBufferAllocateInfo cmdBufAlloc(*r_commandPool, vk::CommandBufferLevel::ePrimary, imageCount);
	r_commandBuffers = vk::raii::CommandBuffers(device, cmdBufAlloc);

	// --- Set debug names for command buffers ---
#ifdef _DEBUG
	for (uint32_t i = 0; i < imageCount; ++i)
	{
		char nameBuf[32];
		snprintf(nameBuf, sizeof(nameBuf), "DeferredRenderer::FrameCmd[%u]", i);
		const vk::DebugUtilsObjectNameInfoEXT nameInfo(
			vk::ObjectType::eCommandBuffer,
			reinterpret_cast<uint64_t>(static_cast<VkCommandBuffer>(*r_commandBuffers[i])),
			nameBuf);
		device.setDebugUtilsObjectNameEXT(nameInfo);
		NEURUS_LOG("[DeferredRenderer] CmdBuf[" << i << "] handle=0x"
		          << std::hex << reinterpret_cast<uint64_t>(
		                 static_cast<VkCommandBuffer>(*r_commandBuffers[i]))
		          << std::dec << " name='" << nameBuf << "'");
	}
#endif

	// --- 10. Create sync objects ---
	for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
	{
		r_inFlightFences.emplace_back(device, vk::FenceCreateInfo(vk::FenceCreateFlagBits::eSignaled));
		r_imageAvailableSemaphores.emplace_back(device, vk::SemaphoreCreateInfo());
	}
	for (uint32_t i = 0; i < imageCount; ++i)
	{
		r_renderFinishedSemaphores.emplace_back(device, vk::SemaphoreCreateInfo());
	}

	NEURUS_LOG("[DeferredRenderer] " << extent.width << "x" << extent.height
	          << " swapchainImages=" << imageCount
	          << " transferDst=" << (hasTransferDst ? "yes" : "no"));

	// --- Request executor ---
	// Built last: it borrows the geometry pass and the cache, both live by now.
	// The UploadManager does not exist yet (the Application creates it after the
	// renderer), so the executor starts without one; SetUploadManager() wires it
	// in before the first scene upload. Its frame drain is this object's fence
	// wait, so a destroying handler never idles the device.
	m_executor = std::make_unique<RenderRequestExecutor>(
		[this] { DrainInFlightFrames(); }, *r_renderCache, /*uploader=*/nullptr, r_geometryPass);
}

DeferredRenderer::~DeferredRenderer()
{
	WaitIdle();
	// vk::raii destructors clean up automatically in reverse declaration order;
	// r_passes destroys all passes before RenderCache.
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

vk::raii::CommandPool DeferredRenderer::createCommandPool(const vk::raii::Device& device,
                                                           uint32_t queueFamilyIndex)
{
	vk::CommandPoolCreateInfo poolCI(vk::CommandPoolCreateFlagBits::eResetCommandBuffer, queueFamilyIndex);
	return vk::raii::CommandPool(device, poolCI);
}

// ---------------------------------------------------------------------------
// NextWork / DrawFrame - main render loop entry point
// ---------------------------------------------------------------------------

bool DeferredRenderer::WaitForFrameSlot()
{
	auto& fence = r_inFlightFences[r_currentFrame];
	// The fence is reset only on DrawFrame's submit path, never here, so a
	// return after a successful wait leaves it signaled and the next wait does
	// not deadlock. A timeout means a hung GPU: the caller must not touch GPU
	// resources, so it skips the frame and the requests stay queued.
	return r_device.waitForFences(*fence, VK_TRUE, kFenceTimeoutNs) == vk::Result::eSuccess;
}

void DeferredRenderer::DrainInFlightFrames()
{
	// Every fence, because a handler replaces or frees a resource any recorded
	// frame may still read - not just the slot the next frame would reuse. The
	// fences are created signaled, so a slot never submitted waits trivially.
	std::vector<vk::Fence> fences;
	fences.reserve(r_inFlightFences.size());
	for (const auto& fence : r_inFlightFences)
		fences.push_back(*fence);

	vk::Result result = vk::Result::eSuccess;
	try
	{
		result = r_device.waitForFences(fences, VK_TRUE, kFenceTimeoutNs);
	}
	catch (const std::exception& e)
	{
		// A device-loss wait throws instead of returning; the fallback below is
		// the only thing that can still make the caller's release safe.
		NEURUS_ERR("[DeferredRenderer] Frame fence drain threw (" << e.what()
		          << ") - falling back to an idle queue");
		r_graphicsQueue->WaitIdle();
		return;
	}

	if (result != vk::Result::eSuccess)
	{
		// The fences did not prove the GPU is done, but the caller is about to
		// destroy what a recorded frame may be reading: idle the queue so the
		// release is safe even though the drain failed.
		NEURUS_ERR("[DeferredRenderer] Frame fence drain returned "
		          << vk::to_string(result) << " - falling back to an idle queue");
		r_graphicsQueue->WaitIdle();
	}
}

FrameWork DeferredRenderer::NextWork() const
{
	// A frame that drew nothing must not be treated as drawn: the caller has to
	// wait for a new one (another sample would either draw the same nothing or
	// reuse a snapshot the failed pass never consumed).
	if (m_drewNothing)
		return FrameWork::Retry;

	// Accumulation not converged: sample. This also covers iteration 0, which is
	// what guarantees an Overlay frame never precedes a sample after a restart -
	// the image it would draw over only exists because a sample produced it.
	if (m_iteration < m_samplingCount)
		return FrameWork::Sample;

	if (m_overlaysDirty)
		return FrameWork::Overlay;

	return FrameWork::Idle;
}

const FrameProfile& DeferredRenderer::DrawFrame(const RenderContext& ctx)
{
	// --- Wait for this frame slot's fence ---
	// Extracted into WaitForFrameSlot() so the render thread can gate its own
	// drains on the same wait. On TIMEOUT return WITHOUT draining: requests stay
	// queued for the next frame, when the GPU is idle again.
	if (!WaitForFrameSlot())
	{
		m_drewNothing = true;
		return m_frameProfile;  // Timeout - requests stay queued for the next frame
	}

	auto& fence = r_inFlightFences[r_currentFrame];
	auto& imageAvailable = r_imageAvailableSemaphores[r_currentFrame];

	// --- Drain Editor-posted GPU requests ---
	// Placed after the fence wait and before swapchain acquire. The handlers that
	// replace or free a resource drain every in-flight frame fence themselves
	// (DrainInFlightFrames), so no recorded frame still references what they
	// release - independent of how many frames are in flight. Uploads and
	// screenshots wait their own submits, so nothing else can be running against
	// the resource being replaced.
	//
	// The single-threaded path relies on this drain. On the render thread the
	// loop already drained after its own WaitForFrameSlot(), so this second
	// drain is usually empty - but kept, because it is the only drain the
	// single-threaded (QTimer) path has.
	//
	// The drain runs before the no-camera early-out below, so a camera-less
	// session still executes its queued requests instead of queueing forever.
	ProcessRequests();

	// What this call performs, decided after the drain: a request handled above
	// may have restarted accumulation or marked the overlays stale.
	FrameWork work = NextWork();

	// Retry means the previous DrawFrame drew nothing; it says nothing about
	// which work to perform. What it was going to do never advanced - iteration
	// and the overlay flag are only written by a frame that drew - so dropping
	// the latch and asking again yields exactly the interrupted work.
	if (work == FrameWork::Retry)
	{
		m_drewNothing = false;
		work = NextWork();
	}

	// --- Idle: nothing to draw, so nothing to acquire ---
	// Acquiring an image would burn a present slot (and, with a real swapchain,
	// block) to draw exactly what is already on screen. Deliberately before the
	// camera check: an idle frame draws nothing, so it cannot have failed to
	// draw, and flagging it would make NextWork() report Retry on every idle
	// frame of a camera-less session.
	if (work == FrameWork::Idle)
	{
		return m_frameProfile;
	}

	// --- Precondition: we must have a camera to look through ---
	// The frame carries the camera the Editor chose, and every pass that needs a
	// viewing camera reads it from the same snapshot, so they cannot disagree. A
	// frame with no camera is one nobody published a camera into (a scene-mutation
	// path that broke the contract, or an unpublished frame), so name it once and
	// skip the frame.
	if (!ctx.Frame().HasCamera())
	{
		if (!m_reportedNoCamera)
		{
			m_reportedNoCamera = true;
			NEURUS_ERR("[DeferredRenderer] No camera to render through - skipping frames "
			           "until one exists");
		}
		m_drewNothing = true;
		return m_frameProfile;
	}
	m_reportedNoCamera = false;

	// --- Profiling: read back this slot's GPU timestamps (fence signaled) ---
	// Resolve() reads the slot about to be reused; the resolved GPU ms are
	// back-filled into m_frameProfile after recordFrame repopulates the pass
	// list below (GPU data lags the CPU/counter data by 1-2 frames, by design).
	m_gpuResolved = m_profiler.Resolve();

	// --- Acquire next swapchain image ---
	uint32_t imageIndex = 0;
	bool skipFrame = false;
	try
	{
		imageIndex = r_swapchain->AcquireNextImage(imageAvailable);
		r_lastImageIndex = imageIndex;
	}
	catch (const vk::OutOfDateKHRError&)
	{
		NEURUS_ERR("AcquireNextImage: swapchain out of date");
		skipFrame = true;
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("AcquireNextImage failed: " << e.what());
		skipFrame = true;
	}

	// --- Handle swapchain recreation (from out-of-date acquire or external resize) ---
	if (r_swapchain->generation() != r_swapchainGeneration)
	{
		recreateSwapchain();
		skipFrame = true;
	}

	if (skipFrame)
	{
		// Nothing was acquired, so nothing was drawn: NextWork() must report Retry
		// rather than let the caller believe this frame consumed its work.
		m_drewNothing = true;
		return m_frameProfile;
	}

	// Only reset fence after successful acquire
	r_device.resetFences(*fence);

	// --- Record and submit (reuse pre-allocated command buffer) ---
	vk::CommandBuffer cmdBufRaw = *r_commandBuffers[imageIndex];

	recordFrame(r_commandBuffers[imageIndex], imageIndex, ctx, work);

	// --- Profiling: back-fill resolved GPU ms now that passes are repopulated ---
	// recordFrame just rewrote m_frameProfile.passes with gpuMs=0; match the
	// resolved sections to passes by index (the size guard covers a rare
	// rebuild-in-flight case).
	if (m_gpuResolved)
	{
		const auto& sections = m_profiler.Sections();
		const size_t n = std::min(sections.size(), m_frameProfile.passes.size());
		for (size_t i = 0; i < n; ++i)
			m_frameProfile.passes[i].gpuMs = sections[i].gpuMs;
		m_frameProfile.gpuTotalMs = m_profiler.FrameGpuMs();
		m_frameProfile.gpuReady = true;
	}

	auto& renderFinished = r_renderFinishedSemaphores[imageIndex];
	// The swapchain image's first use is a layout transition + blit at the transfer
	// stage, not a color-attachment write, so the acquire semaphore must be waited
	// on before transfer as well: waiting only at eColorAttachmentOutput lets the
	// blit overwrite an image the presentation engine is still reading.
	vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eColorAttachmentOutput |
	                                   vk::PipelineStageFlagBits::eTransfer;

	vk::SubmitInfo submitInfo(*imageAvailable, waitStage, cmdBufRaw, *renderFinished);
	r_graphicsQueue->Submit(submitInfo, *fence);

	// --- Present ---
	bool presentFailed = false;
	try
	{
		r_swapchain->Present(renderFinished, imageIndex);
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("Present failed: " << e.what());
		presentFailed = true;
	}

	// --- Handle swapchain recreation after present ---
	if (r_swapchain->generation() != r_swapchainGeneration)
	{
		recreateSwapchain();
		presentFailed = true;
	}

	if (presentFailed)
	{
		// Drawn but not put on screen. Treat it like any other failed frame: the
		// work is reported as Retry, and because the pending-work flags are only
		// cleared below, the retry redraws the same thing (an Overlay frame
		// redraws the overlays; a Sample frame samples the next iteration).
		m_drewNothing = true;
		return m_frameProfile;
	}

	// --- Drawn and presented: this frame consumed its work ---
	m_drewNothing = false;
	m_overlaysDirty = false;
	r_currentFrame = (r_currentFrame + 1) % kMaxFramesInFlight;
	return m_frameProfile;
}

void DeferredRenderer::ResetShadowAccumulation()
{
	m_iteration = 0;
}

void DeferredRenderer::Post(RenderRequest&& request)
{
	m_requestQueue.Push(std::move(request));
	// Single-threaded: DrawFrame drains the queue directly, so there is nothing
	// to wake here. The threaded caller (RenderThread::Post) wakes the render
	// thread after enqueuing, so a request posted while no frame is coming is
	// still serviced.
}

void DeferredRenderer::ProcessRequests()
{
	// Whole batch at once: a request a handler posts lands in the next TakeAll(),
	// never mid-iteration over this one.
	std::vector<RenderRequest> batch = m_requestQueue.TakeAll();
	for (const RenderRequest& request : batch)
	{
		// Two requests write renderer state the executor does not own, so they are
		// handled here. Routing them through the queue rather than a direct call
		// keeps the rule simple once the renderer runs on its own thread: the
		// Editor never calls in except to Post.

		// Overlays only: keep the converged scene image and redraw on top of it.
		// This is the one request that must NOT restart accumulation - doing so
		// would throw away the converged image the overlay redraw exists for.
		if (std::holds_alternative<RedrawOverlaysRequest>(request))
		{
			m_overlaysDirty = true;
			continue;
		}

		// Everything else changes scene content, GPU resources or the view, which
		// invalidates the accumulated average: restart from frame 0. That includes
		// ResetAccumulationRequest, which needs no further handling (it exists to
		// ask for exactly this), so it stops here instead of reaching the executor.
		m_iteration = 0;

		if (std::holds_alternative<ResetAccumulationRequest>(request))
			continue;

		m_executor->Execute(request);
	}
}

void DeferredRenderer::CaptureScreenshot(bool allAttachments)
{
	// Mirrors the capture the Application used to drive: an all-attachments dump
	// reads the whole render cache, a plain screenshot reads back the last
	// presented swapchain image. The Screenshot methods sync the GPU themselves.
	if (allAttachments)
	{
		m_screenshot->TakeScreenshotAllAttachments(*r_renderCache, GetExtent(), 1024);
	}
	else
	{
		m_screenshot->TakeScreenshot(GetLastSwapchainImage(), GetSwapchainFormat(), GetExtent());
	}
}

void DeferredRenderer::SetUploadManager(UploadManager* uploader)
{
	m_executor->SetUploadManager(uploader);
}

void DeferredRenderer::RebuildMainGraph(const PipelineSignature& sig)
{
	// Rebuild the whole-pipeline DAG so it contains exactly the passes that
	// will run. Edges follow image data-flow; camera/light SSBOs and the
	// per-light shadow maps are addressed by the passes internally.
	//
	// Overlays are NOT in this graph. They live in OverlayEngine, which draws over
	// whatever this graph ends with, so the graph stops at the scene image (the
	// tone mapped/gamma corrected one, optionally FXAA'd). That split is what lets
	// an overlay-only change redraw without re-running a single pass below.
	m_mainGraph.Clear();

	auto* geometryNode  = m_mainGraph.AddPass(r_geometryPass);
	auto* shadowDepth   = m_mainGraph.AddPass(r_shadowDepthPass);
	auto* shadowInt     = m_mainGraph.AddPass(r_shadowIntensityPass);
	auto* ssaoNode      = m_mainGraph.AddPass(r_ssaoPass);
	auto* lightingNode  = m_mainGraph.AddPass(r_lightingPass);
	auto* composeNode   = m_mainGraph.AddPass(r_composePass);

	// Geometry (G-Buffer) → consumers
	m_mainGraph.Connect(geometryNode, AttachmentName::Position,          ssaoNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::Normal,            ssaoNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::Albedo,            ssaoNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::Position,          shadowInt);
	m_mainGraph.Connect(geometryNode, AttachmentName::Position,          lightingNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::Normal,            lightingNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::Albedo,            lightingNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::MetallicRoughness, lightingNode);

	// Shadow depth bundle → shadow intensity → lighting
	m_mainGraph.Connect(shadowDepth, AttachmentName::ShadowDepth,     shadowInt);
	m_mainGraph.Connect(shadowInt,   AttachmentName::ShadowIntensity, lightingNode);

	// SSAO → lighting → compose (tone map + gamma only; the selection highlight is
	// an overlay and is blended in the overlay graph, after this pass).
	m_mainGraph.Connect(ssaoNode,     AttachmentName::SSAO,             lightingNode);
	m_mainGraph.Connect(lightingNode, AttachmentName::HDRColor,         composeNode);

	// Compose -> [FXAA]. The scene tail is the last image this graph writes, and
	// the overlay graph copies it - FXAA is the only topology variation, so the
	// tail choice belongs here, with the config projection that produced it.
	const AttachmentName tail = sig.fxaa ? AttachmentName::FXAAOutput
	                                     : AttachmentName::ComposedOutput;
	if (sig.fxaa)
	{
		auto* fxaaNode = m_mainGraph.AddPass(r_fxaaPass);
		m_mainGraph.Connect(composeNode, AttachmentName::ComposedOutput, fxaaNode);
	}

	// Tell the overlay graph which image to copy. Its copy source is part of its
	// socket names, so it re-wires itself when the tail changes.
	m_overlayEngine->SetSceneTail(tail);

	m_mainGraph.Compile();
	m_builtSignature = sig;

	NEURUS_LOG("[DeferredRenderer] RenderGraph rebuilt ("
	           << m_mainGraph.PassCount() << " passes, FXAA="
	           << (sig.fxaa ? "on" : "off") << ", tail="
	           << AttachmentNameToString(tail) << ")");
}

void DeferredRenderer::WaitIdle()
{
	// Rare event (shutdown, tests), so holding the queue lock across the wait is
	// accepted. On a device with a separate transfer queue that queue is idle
	// here too: every upload waits its own submit before returning.
	r_graphicsQueue->WaitIdle();
}

uint32_t DeferredRenderer::ReadIDBufferPixel(uint32_t x, uint32_t y)
{
	const vk::Extent2D extent = GetExtent();

	if (x >= extent.width || y >= extent.height)
	{
		return 0;
	}

	// A pick is a query, never a producer: it must not create the ID buffer, nor
	// stand in for the geometry pass that writes it. Creating it here (the cache
	// creates lazily on GetAttachment) handed the pick an image no pass had drawn
	// into, and reading that recorded a transition back to UNDEFINED on restore
	// (VUID-VkImageMemoryBarrier2-newLayout-01198) - once per pick, for as
	// long as the renderer stayed idle. This happens for real: a pick that lands
	// right after a resize, before a frame has drawn at the new extent, has no
	// ID buffer to read because the resize dropped the screen-space attachments.
	// The honest answer is "background", i.e. object 0.
	if (!r_renderCache->HasAttachment(AttachmentName::IDBuffer))
	{
		NEURUS_LOG("[DeferredRenderer] ReadIDBufferPixel: no IDBuffer at "
		           << extent.width << "x" << extent.height
		           << " (no frame drawn yet) - returning background");
		return 0;
	}

	const Image& idBuffer = r_renderCache->GetAttachment(AttachmentName::IDBuffer);

	// Stale extent or never written: same answer, for the same reason.
	if (idBuffer.Extent() != extent || idBuffer.State() == ImageState::Undefined)
	{
		NEURUS_LOG("[DeferredRenderer] ReadIDBufferPixel: IDBuffer holds no "
		           << "content (extent " << idBuffer.Extent().width << "x"
		           << idBuffer.Extent().height << ", state "
		           << static_cast<int>(idBuffer.State()) << ") - returning background");
		return 0;
	}

	// The image is left in its original state on return.
	auto bytes = idBuffer.PickPixel(r_device, r_physicalDevice,
	                                 *r_graphicsQueue, r_queueFamilyIndex,
	                                 x, y);

	if (bytes.size() < sizeof(uint32_t))
	{
		return 0;
	}

	uint32_t objectID;
	std::memcpy(&objectID, bytes.data(), sizeof(objectID));
	return objectID;
}

void DeferredRenderer::HandleResize(uint32_t width, uint32_t height)
{
	uint32_t oldGen = r_swapchain->generation();
	r_swapchain->Recreate(width, height);
	if (r_swapchain->generation() != oldGen)
	{
		recreateSwapchain();
	}
}

void DeferredRenderer::HandleSurfaceChange(const vk::raii::SurfaceKHR& newSurface)
{
	// The old swapchain's images must not be in use while it is destroyed, and
	// this is a rare event (a window/monitor move), so holding the queue lock
	// across the wait is accepted. A separate transfer queue is idle here
	// because every upload waits its own submit.
	r_graphicsQueue->WaitIdle();

	// Preserve current extent to pass to the new swapchain
	uint32_t width = r_swapchain ? r_swapchain->extent().width : 800;
	uint32_t height = r_swapchain ? r_swapchain->extent().height : 600;

	// Destroy old swapchain (which held a reference to the old surface)
	r_swapchain.reset();

	r_swapchain = std::make_unique<Swapchain>(r_physicalDevice, r_device, *r_graphicsQueue, newSurface, width, height);

	recreateSwapchain();

	NEURUS_LOG("[DeferredRenderer] Surface changed — swapchain rebuilt "
	           << r_swapchain->extent().width << "x" << r_swapchain->extent().height);
}

// ---------------------------------------------------------------------------
// Swapchain recreation
// ---------------------------------------------------------------------------

void DeferredRenderer::recreateSwapchain()
{
	// The swapchain and the screen-space attachments it sizes are about to be
	// replaced; a resize or an out-of-date present is rare, so holding the queue
	// lock across the wait is accepted. A separate transfer queue is idle here
	// because every upload waits its own submit.
	r_graphicsQueue->WaitIdle();

	uint32_t newImageCount = r_swapchain->imageCount();
	vk::Extent2D newExtent = r_swapchain->extent();
	// Clear screen-space attachments (G-Buffer + shadow intensities).
	// Shadow cubemaps survive resize.  Attachments are re-created lazily
	// on first GetAttachment() call in the next frame.
	r_renderCache->CleanScreenSpace();

	// The shadow intensity history just went with the screen-space attachments,
	// so accumulation must restart from frame 0 with it. Nothing else resets it
	// on a resize (the Editor posts ResetAccumulationRequest only when scene
	// content changes), and a rebuild can also come from an out-of-date acquire
	// the Editor never sees. A running average continued over a cleared history
	// fades in slowly with EMA and never recovers in moving-average mode.
	m_iteration = 0;

	// Rebuild render-finished semaphores (one per swapchain image)
	r_renderFinishedSemaphores.clear();
	for (uint32_t i = 0; i < newImageCount; ++i)
	{
		r_renderFinishedSemaphores.emplace_back(r_device, vk::SemaphoreCreateInfo());
	}

	// Rebuild command buffers if image count changed (swapchain image count
	// may differ across surfaces/drivers).
	if (r_commandBuffers.size() != newImageCount)
	{
		r_commandBuffers.clear();
		vk::CommandBufferAllocateInfo cmdBufAlloc(
			*r_commandPool, vk::CommandBufferLevel::ePrimary, newImageCount);
		r_commandBuffers = vk::raii::CommandBuffers(r_device, cmdBufAlloc);

#ifdef _DEBUG
		for (uint32_t i = 0; i < newImageCount; ++i)
		{
			char nameBuf[32];
			snprintf(nameBuf, sizeof(nameBuf), "DeferredRenderer::FrameCmd[%u]", i);
			const vk::DebugUtilsObjectNameInfoEXT nameInfo(
				vk::ObjectType::eCommandBuffer,
				reinterpret_cast<uint64_t>(static_cast<VkCommandBuffer>(*r_commandBuffers[i])),
				nameBuf);
			r_device.setDebugUtilsObjectNameEXT(nameInfo);
			NEURUS_LOG("[DeferredRenderer] CmdBuf[" << i << "] handle=0x"
			          << std::hex << reinterpret_cast<uint64_t>(
			                 static_cast<VkCommandBuffer>(*r_commandBuffers[i]))
			          << std::dec << " name='" << nameBuf << "'");
		}
#endif
	}

	r_swapchainGeneration = r_swapchain->generation();
}

// ---------------------------------------------------------------------------
// Frame recording
// ---------------------------------------------------------------------------

void DeferredRenderer::recordFrame(const vk::raii::CommandBuffer& cmdBuf, uint32_t imageIndex,
                                   const RenderContext& editorCtx, const FrameWork work)
{
	if (work != FrameWork::Sample && work != FrameWork::Overlay)
	{
		throw std::logic_error(
			"DeferredRenderer::recordFrame called with work that draws nothing");
	}

	// An Overlay frame draws over the image a Sample frame produced. At iteration
	// 0 no sample has run since the last restart, so there is nothing to draw
	// over: NextWork() never asks for it, but recordFrame is also reachable
	// directly, so the invariant is checked here rather than assumed.
	if (work == FrameWork::Overlay && m_iteration == 0)
	{
		throw std::logic_error(
			"DeferredRenderer::recordFrame: Overlay frame before any Sample frame");
	}

	const vk::Extent2D extent = r_swapchain->extent();

	// --- Reset the command buffer, then begin recording ---
	cmdBuf.reset();
	vk::CommandBufferBeginInfo beginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
	cmdBuf.begin(beginInfo);

	// --- Build per-frame render context ---
	// Fill the render-specific fields into a copy of the caller's context.
	RenderContext ctx = editorCtx;
	ctx.width = extent.width;
	ctx.height = extent.height;
	ctx.frameIndex = r_currentFrame;

	if (work == FrameWork::Sample)
	{
		// --- Refresh the sampling budget ---
		// Read on every sample frame, the only place it is compared against
		// m_iteration. A new value always arrives with an accumulation restart, so
		// this read is what makes the cached copy authoritative for the run.
		m_samplingCount = static_cast<uint32_t>(
			std::max(1, ctx.Frame().Settings().r_sampling_count));

		// --- Temporal shadow accumulation: compute 3D random jitter direction ---
		{
			float x = 2.0f * HaltonSequence::Halton2(m_haltonIndex) - 1.0f;
			float y = 2.0f * HaltonSequence::Halton3(m_haltonIndex) - 1.0f;
			float z = 2.0f * HaltonSequence::Halton5(m_haltonIndex) - 1.0f;
			float len = std::sqrt(x * x + y * y + z * z);
			ctx.jitter = (len > 1e-6f) ? glm::vec3(x / len, y / len, z / len) : glm::vec3(0.0f, 0.0f, 1.0f);
		}

		ctx.iteration = m_iteration;

		// Advance the temporal counters for the next sample. An Overlay frame
		// draws no accumulation sample, so it must not touch either counter.
		m_iteration++;
		m_haltonIndex++;
	}
	else
	{
		// Overlay: no jitter, no accumulation sample. iteration is reported so the
		// passes that read it (and the profile) see how converged the image is;
		// jitter stays zero, which is what the unused field means.
		ctx.iteration = m_iteration;
	}

	// --- Per-frame cache updates: one writer, before any pass records ---
	// Both resources live in RenderCache because more than one pass reads them,
	// and a pass that wrote them would make the result depend on pass order.
	// This is the one place that defines what the GPU believes the camera is;
	// DrawFrame's precondition has already proved the frame carries one.
	const RenderFrameSnapshot& frame = ctx.Frame();
	r_renderCache->UpdateCamera(frame.camera.projection, frame.camera.view);

	if (frame.HasDebugDraw())
	{
		// Revision-gated: an unchanged list copies nothing.
		r_renderCache->UpdateDebugDraw(ctx.frameIndex, frame.debugDraw);
	}

	// Unconditional: production always published a gizmo list (empty when nothing
	// is armed), and GizmoCache::Update tolerates an empty list - it writes zero
	// counts, which is what makes a finished gesture's guide vanish.
	r_renderCache->UpdateGizmoDraw(ctx.frameIndex, frame.gizmoDraw);

	// --- Pipeline ---
	// Sample: Geometry -> Shadows -> SSAO -> Lighting -> Compose -> [FXAA], then
	//         the overlay graph over that image.
	// Overlay: the overlay graph alone, over the image the last sample produced.
	// The graph is only rebuilt here (Sample only) when the config-derived
	// signature changes: an Overlay frame runs the same scene topology the
	// converged image came from, so re-evaluating it would be pointless - and
	// rebuilding it mid-convergence is exactly what a signature change implies
	// was already handled by the restart that came with it.
	if (work == FrameWork::Sample)
	{
		const PipelineSignature sig = PipelineSignature::From(frame.Settings());
		if (!(sig == m_builtSignature))
			RebuildMainGraph(sig);
	}

	// --- Profiling: bracket the whole pipeline with GPU frame timestamps ---
	// One BeginFrame/EndFrame pair covers both graphs, so the overlay passes are
	// timed by the same query pool and land after the scene passes in query and
	// profile order (OverlayEngine appends them to m_frameProfile). GPU per-pass
	// ms are back-filled in DrawFrame after Resolve().
	m_profiler.BeginFrame(cmdBuf);
	if (work == FrameWork::Sample)
		m_mainGraph.Execute(cmdBuf, *r_renderCache, ctx, m_profiler, m_frameProfile);
	else
		m_frameProfile = FrameProfile{};  // An overlay frame records only overlay passes.
	m_overlayEngine->Execute(cmdBuf, *r_renderCache, ctx, m_profiler, m_frameProfile);

	// --- Blit the final image to the swapchain ---
	// The overlay graph's output is always the end of the chain: the scene tail is
	// copied into OverlayOutput, the overlays draw into it, and nothing presents
	// the scene image directly. That is also why the copy is mandatory - on an
	// overlay frame the scene tail must survive so the next overlay redraw can
	// start from it again.
	auto& blitSource = r_renderCache->GetAttachment(OverlayEngine::OutputAttachment(), extent);
	const vk::Image overlayImage = *blitSource.ImageHandle();
	const vk::Image swapchainImage = r_swapchain->images()[imageIndex];

	// Barrier 1: Blit source -> TRANSFER_SRC_OPTIMAL.
	//
	// The transition is issued here, by the consumer, not left to whichever pass
	// wrote the image last. A producer that pre-transitions its output to the
	// state it guesses the next consumer wants makes the following consumer's
	// own barrier a no-op: its src scope becomes the guessed access (TransferRead),
	// which names no writes, so the real producer writes are never made visible.
	//
	// Barrier 2: Swapchain image UNDEFINED -> TRANSFER_DST_OPTIMAL
	Barrier::Transition(cmdBuf, blitSource, ImageState::TransferSrc);
	{
		Barrier::Transition(*cmdBuf, swapchainImage,
			ImageState::Undefined, ImageState::TransferDst,
			vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	}

	// --- Blit the final image into the swapchain image ---
	{
		vk::ImageBlit blitRegion;
		blitRegion.srcSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
		blitRegion.srcOffsets[0] = vk::Offset3D(0, 0, 0);
		blitRegion.srcOffsets[1] = vk::Offset3D(static_cast<int32_t>(extent.width),
		                                         static_cast<int32_t>(extent.height), 1);
		blitRegion.dstSubresource = vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
		blitRegion.dstOffsets[0] = vk::Offset3D(0, 0, 0);
		blitRegion.dstOffsets[1] = vk::Offset3D(static_cast<int32_t>(extent.width),
		                                         static_cast<int32_t>(extent.height), 1);

		cmdBuf.blitImage(overlayImage,
		                 vk::ImageLayout::eTransferSrcOptimal,
		                 swapchainImage,
		                 vk::ImageLayout::eTransferDstOptimal,
		                 blitRegion,
		                 vk::Filter::eLinear);
	}

	// --- Transition swapchain image to PRESENT_SRC_KHR ---
	Barrier::Transition(*cmdBuf, swapchainImage,
		ImageState::TransferDst, ImageState::Present,
		vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));

	// --- Profiling: write the GPU frame-end timestamp (covers the final blit) ---
	// EndFrame also advances the profiler's slot cursor, keeping it in lockstep
	// with r_currentFrame.
	m_profiler.EndFrame(cmdBuf);

	// --- End command buffer ---
	cmdBuf.end();

	// --- Profiling: finalize CPU-side aggregate fields ---
	// The graphs populated passes[], cpuRecordMs, drawCalls, dispatches and
	// passCount (the scene graph's from m_mainGraph, the overlay passes appended by
	// OverlayEngine); only gpuTimingAvailable is renderer-owned.
	m_frameProfile.gpuTimingAvailable = m_profiler.Available();
}

// ---------------------------------------------------------------------------
// Swapchain accessors
// ---------------------------------------------------------------------------

vk::Image DeferredRenderer::GetLastSwapchainImage() const
{
	if (!r_swapchain || r_lastImageIndex >= r_swapchain->images().size())
	{
		return vk::Image{};
	}
	return r_swapchain->images()[r_lastImageIndex];
}

vk::Format DeferredRenderer::GetSwapchainFormat() const
{
	if (!r_swapchain)
	{
		return vk::Format::eUndefined;
	}
	return r_swapchain->format();
}

} // namespace neurus
