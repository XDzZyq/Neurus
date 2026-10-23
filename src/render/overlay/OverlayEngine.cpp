/**
 * @file OverlayEngine.cpp
 * @brief The overlay graph and the copy pass that seeds it.
 */

#include "overlay/OverlayEngine.h"

#include "Barrier.h"
#include "GPUProfiler.h"
#include "Image.h"
#include "PipelineBuilder.h"
#include "ProfilingData.h"
#include "RenderContext.h"

#include "passes/DebugPass.h"
#include "passes/GizmoPass.h"
#include "passes/SelectionOutlinePass.h"

#include "shaders/ComputeShader.h"
#include "shaders/ShaderLibrary.h"

#include "core/Log.h"

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
		                                  NEURUS_SHADER_DIR "compute/overlay_copy.comp"))
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

void OverlayCopyPass::WriteDescriptors(uint32_t setIndex, vk::Extent2D extent, RenderCache& cache)
{
	DescriptorSet& dstSet = p_descriptorSets[setIndex];

	{
		const auto& tailAtt = cache.GetAttachment(p_source, extent);
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
		auto& tailAtt = cache.GetAttachment(p_source, renderExtent);
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
	           << " tail=" << AttachmentNameToString(m_sceneTail)
	           << " passes=" << m_graph.PassCount());
}

OverlayEngine::~OverlayEngine() = default;

void OverlayEngine::SetSceneTail(const AttachmentName tail)
{
	if (tail == m_sceneTail && m_compiled)
		return;

	m_sceneTail = tail;
	m_copy->SetSource(tail);
	RebuildGraph();

	NEURUS_LOG("[OverlayEngine] tail=" << AttachmentNameToString(tail)
	           << " passes=" << m_graph.PassCount());
}

void OverlayEngine::RebuildGraph()
{
	// Every socket name is part of a pass's cached PassIO, so a rebuild
	// re-registers all four passes rather than patching one node: OverlayCopyPass
	// names the scene tail, and after a rebuild that name may have changed.
	m_graph.Clear();

	auto* maskNode  = m_graph.AddPass(m_selectionOutline.get());
	auto* copyNode  = m_graph.AddPass(m_copy.get());
	auto* debugNode = m_graph.AddPass(m_debug.get());
	auto* gizmoNode = m_graph.AddPass(m_gizmo.get());

	// Mask -> copy: the copy blends the mask it just produced.
	m_graph.Connect(maskNode, AttachmentName::SelectionOutline, copyNode);

	// Copy -> debug -> gizmo, all on OverlayOutput. Debug and gizmo declare the
	// same attachment as read and written (LOAD_OP_LOAD), so one edge each is
	// what orders them; the explicit debug -> gizmo edge is what keeps the guides
	// above the debug geometry instead of at an unconstrained position in the
	// topological order.
	m_graph.Connect(copyNode,  AttachmentName::OverlayOutput, debugNode);
	m_graph.Connect(debugNode, AttachmentName::OverlayOutput, gizmoNode);

	// IDBuffer and Depth are written by the scene graph, which this graph does
	// not know about: they stay external inputs, ordered by each pass's own
	// barrier. On an overlay frame they hold the converged scene's contents.

	m_graph.Compile();
	m_compiled = true;
}

void OverlayEngine::Execute(vk::CommandBuffer cmd,
                            RenderCache& cache,
                            const RenderContext& ctx,
                            GPUProfiler& profiler,
                            FrameProfile& frameProfile)
{
	// The graph clears the profile it writes, so it writes to a scratch one and
	// this appends: the frame keeps the scene passes already recorded in it.
	FrameProfile overlayProfile;
	m_graph.Execute(cmd, cache, ctx, profiler, overlayProfile);
	AppendProfile(frameProfile, overlayProfile);
}

} // namespace neurus
