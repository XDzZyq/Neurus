/**
 * @file DeferredRenderer.cpp
 * @brief Deferred rendering pipeline implementation.
 */

#include "DeferredRenderer.h"

#include "Barrier.h"
#include "Image.h"
#include "RenderCache.h"
#include "RenderContext.h"
#include "Swapchain.h"

#include "resources/LightingCache.h"

#include "passes/GeometryPass.h"
#include "passes/LightingPass.h"
#include "passes/SSAOPass.h"
#include "passes/ShadowDepthPass.h"
#include "passes/ShadowIntensityPass.h"
#include "passes/GizmoPass.h"
#include "passes/ComposePass.h"
#include "passes/DebugPass.h"
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
                                    vk::Queue graphicsQueue,
                                    uint32_t queueFamilyIndex,
                                    const vk::raii::SurfaceKHR& surface,
                                    uint32_t width,
                                    uint32_t height)
	: r_device(device)
	, r_physicalDevice(physicalDevice)
	, r_graphicsQueue(graphicsQueue)
	, r_queueFamilyIndex(queueFamilyIndex)
	, r_commandPool(createCommandPool(device, queueFamilyIndex))
{
	// --- 0. Create the optional GPU timestamp query pool (no-op if unsupported) ---
	m_profiler.Initialize(device, physicalDevice, queueFamilyIndex, kMaxFramesInFlight);

	// --- 1. Create swapchain ---
	r_swapchain = std::make_unique<Swapchain>(physicalDevice, device, surface, width, height);
	const auto extent = r_swapchain->extent();

	// --- 2. Create G-Buffer attachment cache (lazy creation on first access) ---
	r_renderCache = std::make_unique<RenderCache>(device, physicalDevice);
	// LightingCache is set via Application after construction (from UploadManager).

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

	// --- 8c. Create gizmo highlight pass (3×3 IDBuffer edge detection) ---
	{
		auto gizmoPass = std::make_unique<GizmoPass>(
			device, physicalDevice,
			kMaxFramesInFlight);
		r_gizmoPass = gizmoPass.get();
		r_passes.push_back(std::move(gizmoPass));
		NEURUS_LOG("[DeferredRenderer] GizmoPass created");
	}

	// --- 8d. Create compose pass (highlight blend + gamma correction) ---
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

	// --- 8f. Create debug overlay pass (lines / points / wireframes over the
	//         composed image; a no-op on frames with no debug geometry) ---
	{
		auto debugPass = std::make_unique<DebugPass>(
			device, physicalDevice,
			kMaxFramesInFlight);
		r_debugPass = debugPass.get();
		r_passes.push_back(std::move(debugPass));
		NEURUS_LOG("[DeferredRenderer] DebugPass created");
	}

	// --- 8g. Build the Wave 3 shading-tail RenderGraph ---
	// Initial build uses a default signature (no FXAA); recordFrame rebuilds
	// it whenever the pipeline signature derived from RenderConfig changes.
	RebuildMainGraph(PipelineSignature{});

	// --- 9. Allocate command buffers (one per swapchain image, reused) ---
	uint32_t imageCount = r_swapchain->imageCount();

	// Verify the swapchain supports TRANSFER_DST for the blit composite path
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
}

DeferredRenderer::~DeferredRenderer()
{
	WaitIdle();
	// vk::raii destructors clean up automatically in reverse declaration order.
	// r_passes vector destroys all passes in construction order (GeometryPass → LightingPass →
	//   SSAOPass → IBLPass), before RenderCache.
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
// DrawFrame - main render loop entry point
// ---------------------------------------------------------------------------

const FrameProfile& DeferredRenderer::DrawFrame(const RenderContext& ctx)
{
	// --- Precondition: the scene must have an active camera ---
	// Every pass dereferences Scene::GetActiveCamera() unconditionally to build
	// its view-projection, so a camera-less scene faults deep inside whichever
	// pass the graph happens to run first (a stack that says nothing about the
	// cause). The Editor holds the invariant on both ends - NewScene()/
	// CreateDefaultScene() seed a camera, SceneController refuses to delete the
	// last one - so getting here means a scene-mutation path broke it. Name it
	// once and skip the frame.
	const auto* frameScene = static_cast<const Scene*>(ctx.editor.scene);
	if (!frameScene || !frameScene->GetActiveCamera())
	{
		if (!m_reportedNoCamera)
		{
			m_reportedNoCamera = true;
			NEURUS_ERR("[DeferredRenderer] Scene has no active camera - skipping frames "
			           "until one exists");
		}
		return m_frameProfile;
	}
	m_reportedNoCamera = false;

	auto& fence = r_inFlightFences[r_currentFrame];
	auto& imageAvailable = r_imageAvailableSemaphores[r_currentFrame];

	// --- Wait for this frame slot's fence ---
	if (r_device.waitForFences(*fence, VK_TRUE, kFenceTimeoutNs) != vk::Result::eSuccess)
	{
		return m_frameProfile;  // Timeout - skip this frame
	}

	// --- Profiling: read back this slot's GPU timestamps (fence signaled) ---
	// The profiler's slot cursor stays in lockstep with r_currentFrame because
	// BeginFrame/EndFrame run exactly once per recorded frame. Resolve() reads
	// the slot about to be reused; the resolved GPU ms are back-filled into
	// m_frameProfile *after* recordFrame repopulates the pass list below (GPU
	// data lags the CPU/counter data by 1-2 frames, by design).
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
		return m_frameProfile;
	}

	// Only reset fence after successful acquire
	r_device.resetFences(*fence);

	// --- Record and submit (reuse pre-allocated command buffer) ---
	vk::CommandBuffer cmdBufRaw = *r_commandBuffers[imageIndex];

	recordFrame(r_commandBuffers[imageIndex], imageIndex, ctx);

	// --- Profiling: back-fill resolved GPU ms now that passes are repopulated ---
	// recordFrame just rewrote m_frameProfile.passes (CPU + counters, gpuMs=0).
	// Match resolved sections to passes by index (topology is stable frame to
	// frame; the size guard covers the rare rebuild-in-flight case).
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
	r_graphicsQueue.submit(submitInfo, *fence);

	// --- Present ---
	bool presentFailed = false;
	try
	{
		r_swapchain->Present(renderFinished, imageIndex, r_graphicsQueue);
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
		return m_frameProfile;
	}

	r_currentFrame = (r_currentFrame + 1) % kMaxFramesInFlight;
	return m_frameProfile;
}

void DeferredRenderer::ResetShadowAccumulation()
{
	m_iteration = 0;
}

void DeferredRenderer::RebuildMainGraph(const PipelineSignature& sig)
{
	// Rebuild the whole-pipeline DAG so it contains exactly the passes that
	// will run. Edges follow image data-flow; camera/light SSBOs and the
	// per-light shadow maps are addressed by the passes internally.
	m_mainGraph.Clear();

	auto* geometryNode  = m_mainGraph.AddPass(r_geometryPass);
	auto* shadowDepth   = m_mainGraph.AddPass(r_shadowDepthPass);
	auto* shadowInt     = m_mainGraph.AddPass(r_shadowIntensityPass);
	auto* ssaoNode      = m_mainGraph.AddPass(r_ssaoPass);
	auto* lightingNode  = m_mainGraph.AddPass(r_lightingPass);
	auto* gizmoNode     = m_mainGraph.AddPass(r_gizmoPass);
	auto* composeNode   = m_mainGraph.AddPass(r_composePass);
	auto* debugNode     = m_mainGraph.AddPass(r_debugPass);

	// Geometry (G-Buffer) → consumers
	m_mainGraph.Connect(geometryNode, AttachmentName::Position,          ssaoNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::Normal,            ssaoNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::Albedo,            ssaoNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::Position,          shadowInt);
	m_mainGraph.Connect(geometryNode, AttachmentName::Position,          lightingNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::Normal,            lightingNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::Albedo,            lightingNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::MetallicRoughness, lightingNode);
	m_mainGraph.Connect(geometryNode, AttachmentName::IDBuffer,          gizmoNode);

	// Shadow depth bundle → shadow intensity → lighting
	m_mainGraph.Connect(shadowDepth, AttachmentName::ShadowDepth,     shadowInt);
	m_mainGraph.Connect(shadowInt,   AttachmentName::ShadowIntensity, lightingNode);

	// SSAO → lighting; lighting + gizmo → compose
	m_mainGraph.Connect(ssaoNode,     AttachmentName::SSAO,           lightingNode);
	m_mainGraph.Connect(lightingNode, AttachmentName::HDRColor,       composeNode);
	m_mainGraph.Connect(gizmoNode,    AttachmentName::GizmoHighlight, composeNode);

	// Compose → debug overlay. DebugPass edits ComposedOutput in place, so it also
	// needs the G-Buffer depth it tests against, and it — not ComposePass — is now
	// the last writer that FXAA (or the final blit) consumes.
	m_mainGraph.Connect(composeNode,   AttachmentName::ComposedOutput, debugNode);
	m_mainGraph.Connect(geometryNode,  AttachmentName::Depth,          debugNode);

	if (sig.fxaa)
	{
		auto* fxaaNode = m_mainGraph.AddPass(r_fxaaPass);
		m_mainGraph.Connect(debugNode, AttachmentName::ComposedOutput, fxaaNode);
	}

	m_mainGraph.Compile();
	m_builtSignature = sig;

	NEURUS_LOG("[DeferredRenderer] RenderGraph rebuilt ("
	           << m_mainGraph.PassCount() << " passes, FXAA="
	           << (sig.fxaa ? "on" : "off") << ")");
}

void DeferredRenderer::WaitIdle()
{
	r_device.waitIdle();
}

uint32_t DeferredRenderer::ReadIDBufferPixel(uint32_t x, uint32_t y)
{
	const vk::Extent2D extent = GetExtent();

	// Clamp or reject out-of-bounds clicks
	if (x >= extent.width || y >= extent.height)
	{
		return 0;
	}

	auto& idBuffer = r_renderCache->GetAttachment(AttachmentName::IDBuffer, extent);

	// PickPixel handles its own transient command buffer, readback, and state
	// restoration — the image is left in its original state on return.
	auto bytes = idBuffer.PickPixel(r_device, r_physicalDevice,
	                                 r_graphicsQueue, r_queueFamilyIndex,
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
	r_device.waitIdle();

	// Preserve current extent to pass to the new swapchain
	uint32_t width = r_swapchain ? r_swapchain->extent().width : 800;
	uint32_t height = r_swapchain ? r_swapchain->extent().height : 600;

	// Destroy old swapchain (which held a reference to the old surface)
	r_swapchain.reset();

	// Create new swapchain bound to the new surface
	r_swapchain = std::make_unique<Swapchain>(r_physicalDevice, r_device, newSurface, width, height);

	// Rebuild dependent resources for the new swapchain
	recreateSwapchain();

	NEURUS_LOG("[DeferredRenderer] Surface changed — swapchain rebuilt "
	           << r_swapchain->extent().width << "x" << r_swapchain->extent().height);
}

// ---------------------------------------------------------------------------
// Swapchain recreation
// ---------------------------------------------------------------------------

void DeferredRenderer::recreateSwapchain()
{
	r_device.waitIdle();

	uint32_t newImageCount = r_swapchain->imageCount();
	vk::Extent2D newExtent = r_swapchain->extent();
	// Clear screen-space attachments (G-Buffer + shadow intensities).
	// Shadow cubemaps survive resize.  Attachments are re-created lazily
	// on first GetAttachment() call in the next frame.
	r_renderCache->CleanScreenSpace();

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
                                   const RenderContext& editorCtx)
{
	const vk::Extent2D extent = r_swapchain->extent();

	// --- Reset command buffer (ensures it's not in a bad state) then begin ---
	cmdBuf.reset();
	vk::CommandBufferBeginInfo beginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
	cmdBuf.begin(beginInfo);

	// --- Build per-frame render context ---
	// Copy the editor-produced context (scene) and fill in render-specific fields.
	RenderContext ctx = editorCtx;
	ctx.width = extent.width;
	ctx.height = extent.height;
	ctx.frameIndex = r_currentFrame;

	// --- Temporal shadow accumulation: compute 3D random jitter direction ---
	{
		float x = 2.0f * HaltonSequence::Halton2(m_haltonIndex) - 1.0f;
		float y = 2.0f * HaltonSequence::Halton3(m_haltonIndex) - 1.0f;
		float z = 2.0f * HaltonSequence::Halton5(m_haltonIndex) - 1.0f;
		float len = std::sqrt(x * x + y * y + z * z);
		ctx.jitter = (len > 1e-6f) ? glm::vec3(x / len, y / len, z / len) : glm::vec3(0.0f, 0.0f, 1.0f);
	}

	// Set global iteration counter from DeferredRenderer's counter
	ctx.iteration = m_iteration;
	m_iteration++;

	// Advance Halton index (cycles through all Halton(2,3,5) triples)
	m_haltonIndex++;

	// --- Pipeline: Geometry → Shadows → SSAO → Lighting → Gizmo → Compose → [FXAA] ---
	// The whole deferred pipeline runs through one RenderGraph. FXAA is
	// optional; useFXAA also selects the blit source below.
	const bool useFXAA = ctx.editor.config &&
		static_cast<const RenderConfig*>(ctx.editor.config)->RequiresFXAA();

	// Rebuild the graph only when the config-derived signature changes (single
	// source of truth is RenderConfig; the graph is its projection — currently
	// just FXAA presence).
	if (const auto* cfg = static_cast<const RenderConfig*>(ctx.editor.config))
	{
		const PipelineSignature sig = PipelineSignature::From(*cfg);
		if (!(sig == m_builtSignature))
			RebuildMainGraph(sig);
	}

	// --- Profiling: bracket the whole pipeline with GPU frame timestamps ---
	// The RenderGraph drives per-pass GPU timestamps + CPU timing and folds the
	// PassStats each pass returns into m_frameProfile (passes stay unaware of
	// profiling). GPU per-pass ms are back-filled in DrawFrame after Resolve().
	m_profiler.BeginFrame(cmdBuf);
	m_mainGraph.Execute(cmdBuf, *r_renderCache, ctx, m_profiler, m_frameProfile);

	// --- Phase 4: Blit output → swapchain image ---
	auto& blitSource = r_renderCache->GetAttachment(
		useFXAA ? AttachmentName::FXAAOutput : AttachmentName::ComposedOutput, extent);
	const vk::Image composedImage = *blitSource.ImageHandle();
	const vk::Image swapchainImage = r_swapchain->images()[imageIndex];

	// Barrier 1: Blit source → TRANSFER_SRC_OPTIMAL.
	//
	// The transition is issued *here*, by the consumer, and not left to whichever
	// pass wrote the image last. A producer that pre-transitions its output to the
	// state it guesses the next consumer wants makes the following consumer's own
	// barrier a no-op: its src scope becomes the guessed access (TransferRead),
	// which names no writes, so the real producer writes are never made visible.
	// That is exactly how DebugPass's overlay used to race ComposePass's compute
	// writes into ComposedOutput — see the barrier note in DebugPass::Record.
	//
	// Barrier 2: Swapchain image UNDEFINED → TRANSFER_DST_OPTIMAL
	Barrier::Transition(cmdBuf, blitSource, ImageState::TransferSrc);
	{
		Barrier::Transition(*cmdBuf, swapchainImage,
			ImageState::Undefined, ImageState::TransferDst,
			vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	}

	// --- Blit: ComposedOutput → swapchain image ---
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

		cmdBuf.blitImage(composedImage,
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
	// with r_currentFrame (both advance exactly once per recorded frame).
	m_profiler.EndFrame(cmdBuf);

	// --- End command buffer ---
	cmdBuf.end();

	// --- Profiling: finalize CPU-side aggregate fields ---
	// RenderGraph::Execute already populated passes[], cpuRecordMs, drawCalls,
	// dispatches, and passCount; only gpuTimingAvailable is renderer-owned.
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
