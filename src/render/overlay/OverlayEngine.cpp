/**
 * @file OverlayEngine.cpp
 * @brief The overlay graph and the copy pass that seeds it.
 */

#include "overlay/OverlayEngine.hpp"

#include "Barrier.hpp"
#include "GPUProfiler.hpp"
#include "Image.hpp"
#include "PipelineBuilder.hpp"
#include "render_api/ProfilingData.hpp"
#include "render_api/RenderContext.hpp"

#include "passes/DebugPass.hpp"
#include "passes/GizmoPass.hpp"
#include "passes/SelectionOutlinePass.hpp"

#include "shader/ComputeShader.hpp"
#include "shader/ShaderLibrary.hpp"

#include "core/Log.hpp"

#include <array>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace neurus {

namespace {

/**
 * @brief Folds one graph execution's profile into the frame's profile.
 *
 * RenderGraph::Execute() clears the profile it writes, because it owns the
 * passes it recorded. Two graphs record one frame, so the overlay graph is
 * recorded into a scratch profile and appended here: the scene passes keep their
 * order, the overlay passes follow them, and the per-pass GPU back-fill in
 * DrawFrame still matches by index because the profiler saw the same order.
 */
void AppendProfile(FrameProfile& dst, FrameProfile& src)
{
	dst.passes.insert(dst.passes.end(),
	                  std::make_move_iterator(src.passes.begin()),
	                  std::make_move_iterator(src.passes.end()));
	dst.cpuRecordMs += src.cpuRecordMs;
	dst.drawCalls   += src.drawCalls;
	dst.dispatches  += src.dispatches;
	dst.passCount    = static_cast<uint32_t>(dst.passes.size());
}

} // namespace

// ---------------------------------------------------------------------------
// OverlayCopyPass
// ---------------------------------------------------------------------------

OverlayCopyPass::OverlayCopyPass(const vk::raii::Device& device,
                                 const vk::raii::PhysicalDevice& physicalDevice,
                                 uint32_t numSets)
	: ComputePass(device, physicalDevice,
	              OverlayCopyPass::CreateDescriptorSetLayout(device), numSets)
	, p_shader(
		ShaderLibrary::LoadComputeShader("overlay_copy",
		                                  NEURUS_SHADER_DIR "compute/overlay_copy.comp.glsl"))
{
	BuildPipeline(device, "OverlayCopyPass");

	NEURUS_LOG("[OverlayCopyPass] numSets=" << numSets
	           << " shader=" << (p_shader ? "OK" : "FAIL"));

#ifdef _DEBUG
	for (uint32_t i = 0; i < numSets; ++i)
	{
		const std::string dsName = "OverlayCopyPass_Set" + std::to_string(i);
		p_descriptorSets[i].SetDebugName(dsName.c_str());
	}
#endif
}

OverlayCopyPass::~OverlayCopyPass() = default;

DescriptorSetLayout OverlayCopyPass::CreateDescriptorSetLayout(const vk::raii::Device& device)
{
	return BuildLayout()
		// Scene tail input (combined image sampler)
		.AddBinding(0,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// Selection outline mask input (combined image sampler, R8)
		.AddBinding(1,
		            vk::DescriptorType::eCombinedImageSampler,
		            vk::ShaderStageFlagBits::eCompute)
		// OverlayOutput (storage image, RGBA16F)
		.AddBinding(2,
		            vk::DescriptorType::eStorageImage,
		            vk::ShaderStageFlagBits::eCompute)
		.Build(device);
}

void OverlayCopyPass::BuildPipeline(const vk::raii::Device& device,
                                    const std::string& debugName)
{
	if (!p_shader)
	{
		throw std::runtime_error("OverlayCopyPass: Compute shader not loaded or invalid");
	}

	auto spv = ShaderLibrary::Compile(p_shader->GetStage(ShaderType::COMPUTE),
	                                  ShaderType::COMPUTE, debugName);
	vk::ShaderModuleCreateInfo smCI({}, spv);
	vk::raii::ShaderModule module(device, smCI);
	vk::PipelineShaderStageCreateInfo stageCI({}, vk::ShaderStageFlagBits::eCompute, *module, "main");

	PipelineBuilder builder;
	p_pipelines.push_back(
		builder.AddShaderStage(stageCI)
			.SetDebugName(debugName.c_str())
			.AddDescriptorSetLayout(*p_descriptorSetLayout.layout())
			.BuildComputePipeline(device));
}

Image& OverlayCopyPass::Source(RenderCache& cache, vk::Extent2D extent) const
{
	return p_sourceKey.empty() ? cache.GetAttachment(p_source, extent)
	                           : cache.GetResource(p_sourceKey, extent);
}

void OverlayCopyPass::WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache)
{
	DescriptorSet& dstSet = p_descriptorSets[setIndex];

	{
		const auto& tailAtt = Source(cache, extent);
		dstSet.WriteImage(0,
		                  vk::DescriptorImageInfo(*p_sampler,
		                                          *tailAtt.ImageViewHandle(),
		                                          vk::ImageLayout::eShaderReadOnlyOptimal),
		                  vk::DescriptorType::eCombinedImageSampler);
	}

	{
		const auto& maskAtt = cache.GetAttachment(AttachmentName::SelectionOutline, extent);
		dstSet.WriteImage(1,
		                  vk::DescriptorImageInfo(*p_sampler,
		                                          *maskAtt.ImageViewHandle(),
		                                          vk::ImageLayout::eShaderReadOnlyOptimal),
		                  vk::DescriptorType::eCombinedImageSampler);
	}

	{
		auto& overlayAtt = cache.GetAttachment(AttachmentName::OverlayOutput, extent);
		dstSet.WriteImage(2,
		                  vk::DescriptorImageInfo(nullptr,
		                                          *overlayAtt.ImageViewHandle(),
		                                          vk::ImageLayout::eGeneral),
		                  vk::DescriptorType::eStorageImage);
	}
}

PassStats OverlayCopyPass::Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx)
{
	PassStats stats{};

	const vk::Extent2D renderExtent{ctx.width, ctx.height};
	const uint32_t    frameIndex   = ctx.frameIndex;

	WriteDescriptors(frameIndex, renderExtent, cache);

	{
		auto& tailAtt = Source(cache, renderExtent);
		Barrier::Transition(cmdBuf, tailAtt, ImageState::ColorShaderRead);

		auto& maskAtt = cache.GetAttachment(AttachmentName::SelectionOutline, renderExtent);
		Barrier::Transition(cmdBuf, maskAtt, ImageState::ColorShaderRead);

		auto& overlayAtt = cache.GetAttachment(AttachmentName::OverlayOutput, renderExtent);
		Barrier::Transition(cmdBuf, overlayAtt, ImageState::ShaderWrite);
	}

	cmdBuf.bindPipeline(vk::PipelineBindPoint::eCompute, *p_pipelines[0].pipeline);
	cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eCompute,
	                          *p_pipelines[0].pipelineLayout,
	                          0,
	                          {p_descriptorSets[frameIndex].handle()},
	                          {});

	const uint32_t groupCountX = (renderExtent.width  + 15) / 16;
	const uint32_t groupCountY = (renderExtent.height + 15) / 16;
	++stats.dispatches;
	cmdBuf.dispatch(groupCountX, groupCountY, 1);

	// OverlayOutput is deliberately left in ShaderWrite: the first overlay raster
	// (DebugPass) transitions it itself, which is what gives its LOAD_OP_LOAD read
	// a (ComputeShader, ShaderWrite) source dependency. Pre-transitioning here
	// would swallow this dispatch's writes.

	return stats;
}

PassIO OverlayCopyPass::GetIO() const
{
	PassIO io;
	io.name  = "OverlayCopyPass";
	io.reads = {
		{p_source,                         0, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
		{AttachmentName::SelectionOutline, 1, vk::DescriptorType::eCombinedImageSampler, vk::ImageLayout::eShaderReadOnlyOptimal},
	};
	io.writes = {
		{AttachmentName::OverlayOutput,    2, vk::DescriptorType::eStorageImage, vk::ImageLayout::eGeneral},
	};
	return io;
}

// ---------------------------------------------------------------------------
// OverlayEngine
// ---------------------------------------------------------------------------

OverlayEngine::OverlayEngine(const vk::raii::Device& device,
                             const vk::raii::PhysicalDevice& physicalDevice,
                             uint32_t numSets)
{
	m_selectionOutline = std::make_unique<SelectionOutlinePass>(device, physicalDevice, numSets);
	m_copy             = std::make_unique<OverlayCopyPass>(device, physicalDevice, numSets);
	m_debug            = std::make_unique<DebugPass>(device, physicalDevice, numSets);
	m_gizmo            = std::make_unique<GizmoPass>(device, physicalDevice, numSets);

	// Fixed, not config-driven: every overlay pass draws into the overlay image,
	// never into the scene tail. Both share ComposedOutput's format and usage, so
	// the pipelines are the same ones these passes build for the post-chain image.
	m_debug->SetTarget(AttachmentName::OverlayOutput);
	m_gizmo->SetTarget(AttachmentName::OverlayOutput);

	RebuildGraph();

	NEURUS_LOG("[OverlayEngine] numSets=" << numSets
	           << " passes=" << m_graph.PassCount());
}

OverlayEngine::~OverlayEngine() = default;

void OverlayEngine::SetSceneOutputs(const std::string& color, bool depth, bool objectId)
{
	m_sceneColor = color;
	m_copy->SetResourceKey(color);
	if (depth != m_hasDepth || objectId != m_hasObjectId)
	{
		m_hasDepth = depth;
		m_hasObjectId = objectId;
		RebuildGraph();
	}
}

void OverlayEngine::RebuildGraph()
{
	m_graph.Clear();
	auto* copy = m_graph.AddPass(m_copy.get());
	if (m_hasObjectId)
	{
		auto* mask = m_graph.AddPass(m_selectionOutline.get());
		m_graph.Connect(mask, AttachmentName::SelectionOutline, copy);
	}
	auto* color = copy;
	if (m_hasDepth)
	{
		auto* debug = m_graph.AddPass(m_debug.get());
		m_graph.Connect(copy, AttachmentName::OverlayOutput, debug);
		color = debug;
	}
	m_graph.Connect(color, AttachmentName::OverlayOutput, m_graph.AddPass(m_gizmo.get()));
	m_graph.Compile();
}

void OverlayEngine::Execute(vk::CommandBuffer cmd,
                            RenderCache& cache,
                            const RenderContext& ctx,
                            GPUProfiler& profiler,
                            FrameProfile& frameProfile)
{
	// Without a scene producer, the copy pass samples a black image or an empty mask.
	const auto clearFallback = [&](AttachmentName attachment)
	{
		Image& image = cache.GetAttachment(attachment, {ctx.width, ctx.height});
		if (image.State() != ImageState::Undefined)
			return;
		Barrier::Transition(cmd, image, ImageState::TransferDst);
		cmd.clearColorImage(*image.ImageHandle(), vk::ImageLayout::eTransferDstOptimal,
		                    vk::ClearColorValue(std::array<float, 4>{}),
		                    vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1));
	};
	if (m_sceneColor.empty())
		clearFallback(AttachmentName::ComposedOutput);
	if (!m_hasObjectId)
		clearFallback(AttachmentName::SelectionOutline);
	// The graph clears the profile it writes, so it writes to a scratch one and
	// this appends: the frame keeps the scene passes already recorded in it.
	FrameProfile overlayProfile;
	m_graph.Execute(cmd, cache, ctx, profiler, overlayProfile);
	AppendProfile(frameProfile, overlayProfile);
}

} // namespace neurus
