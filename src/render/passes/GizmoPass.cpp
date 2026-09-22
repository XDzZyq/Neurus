/**
 * @file GizmoPass.cpp
 * @brief Transform-gizmo overlay raster pass implementation.
 *
 * DebugPass.cpp's body with everything depth-related and everything mesh-related
 * removed; see GizmoPass.h for why each omission is a decision rather than a gap.
 */

#include "passes/GizmoPass.h"

#include "RenderCache.h"
#include "RenderContext.h"
#include "render/Barrier.h"
#include "shaders/ShaderLibrary.h"
#include "core/Log.h"

#include "scene/GizmoDrawList.h"

#include <array>
#include <cmath>
#include <stdexcept>

namespace neurus {

namespace {

/// @brief Vertices emitted per segment by overlay_line.vert (two triangles).
constexpr uint32_t kVerticesPerSegment = 6;

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GizmoPass::GizmoPass(const vk::raii::Device& device,
                     const vk::raii::PhysicalDevice& physicalDevice,
                     uint32_t framesInFlight)
	: p_layout(CreateLayout(device))
	, p_descriptorPool(device,
	                   framesInFlight,
	                   DescriptorPool::CalculatePoolSizes({&p_layout}, framesInFlight))
	, p_descriptorSets(p_descriptorPool.Allocate(p_layout, framesInFlight))
	// The same two shader pairs DebugPass loads. ShaderLibrary's first argument is a
	// log tag, not a cache key, so "GizmoLine" and "DebugLine" name the same files
	// without colliding — and the tag names the consumer, which is what makes a
	// compile error point at the right pass.
	, p_lineShader(ShaderLibrary::LoadRenderShader("GizmoLine",
	                                               NEURUS_SHADER_DIR "render/overlay_line.vert",
	                                               NEURUS_SHADER_DIR "render/overlay_line.frag"))
	, p_pointShader(ShaderLibrary::LoadRenderShader("GizmoPoint",
	                                                NEURUS_SHADER_DIR "render/overlay_point.vert",
	                                                NEURUS_SHADER_DIR "render/overlay_point.frag"))
{
	p_device = &device;
	p_physicalDevice = &physicalDevice;

	// gl_PointSize above this is undefined behaviour, not a soft clamp, so the real
	// limit travels to the shader instead of a hardcoded guess.
	p_maxPointSizePx = physicalDevice.getProperties().limits.pointSizeRange[1];

	// No buffers here: bindings 0-2 name RenderCache's camera UBO and the gizmo
	// SSBOs, whose handles change when the geometry grows, so Record() writes the
	// sets every frame instead.
#ifdef _DEBUG
	for (auto& set : p_descriptorSets)
	{
		set.SetDebugName("GizmoPass_Set");
	}
#endif

	BuildPipeline(device, "GizmoPass");

	NEURUS_LOG("[GizmoPass] framesInFlight=" << framesInFlight
	           << " maxPointSizePx=" << p_maxPointSizePx);
}

// ---------------------------------------------------------------------------
// Descriptor set layout factory
// ---------------------------------------------------------------------------

DescriptorSetLayout GizmoPass::CreateLayout(const vk::raii::Device& device)
{
	// Copied unchanged from DebugPass: the shaders are the same files, and
	// overlay_line.vert / overlay_point.vert declare all three bindings in the
	// vertex stage only.
	return BuildLayout()
		.AddBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eVertex)
		.AddBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex)
		.AddBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex)
		.Build(device);
}

// ---------------------------------------------------------------------------
// Shared pipeline state
// ---------------------------------------------------------------------------

void GizmoPass::ConfigureCommonState(PipelineBuilder& builder)
{
	// One color target: the post-AA image, already tonemapped by ComposePass.
	builder.SetColorFormats({vk::Format::eR16G16B16A16Sfloat});
	builder.SetColorBlendAttachment();   // straight alpha-over

	// No SetDepthFormat, no SetDepthStencil, no eDepthTestEnable. A modal guide the
	// object occludes is a guide that cannot be read, so depth is off for the whole
	// pass and beginRendering binds no depth attachment at all — which makes
	// enabling the test invalid here rather than merely useless.

	builder.SetRasterization(vk::PolygonMode::eFill,
	                         vk::CullModeFlagBits::eNone,
	                         vk::FrontFace::eClockwise);
	builder.SetMultisampling();
	builder.AddDescriptorSetLayout(*p_layout.layout());
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

void GizmoPass::BuildPipeline(const vk::raii::Device& device, const std::string& debugName)
{
	if (!p_lineShader || !p_pointShader)
	{
		throw std::runtime_error("GizmoPass: overlay shaders not loaded or invalid");
	}

	// Modules are temporaries that must outlive BuildGraphicsPipeline, so each
	// pipeline is built in its own scope with its own pair.
	struct Modules
	{
		vk::raii::ShaderModule vert;
		vk::raii::ShaderModule frag;
	};

	const auto compile = [&device](const RenderShader& shader, const std::string& name)
	{
		const auto vertSpv = ShaderLibrary::Compile(shader.GetStage(ShaderType::VERTEX),
		                                            ShaderType::VERTEX, name + "_vert");
		const auto fragSpv = ShaderLibrary::Compile(shader.GetStage(ShaderType::FRAGMENT),
		                                            ShaderType::FRAGMENT, name + "_frag");
		return Modules{
			vk::raii::ShaderModule(device, vk::ShaderModuleCreateInfo({}, vertSpv)),
			vk::raii::ShaderModule(device, vk::ShaderModuleCreateInfo({}, fragSpv))};
	};

	const auto addStages = [](PipelineBuilder& builder, const Modules& m, const char* name)
	{
		builder.SetDebugName(name)
		       .AddShaderStage(vk::PipelineShaderStageCreateInfo(
			       {}, vk::ShaderStageFlagBits::eVertex, *m.vert, "main"))
		       .AddShaderStage(vk::PipelineShaderStageCreateInfo(
			       {}, vk::ShaderStageFlagBits::eFragment, *m.frag, "main"));
	};

	// Both pipelines take the identical range, so their layouts are mutually
	// compatible — unlike DebugPass, which also carries a 72 B wire range.
	const vk::PushConstantRange kOverlayRange(vk::ShaderStageFlagBits::eVertex,
	                                          0, sizeof(GizmoPushConstants));

	// --- [0] Lines: no vertex buffer; geometry comes from the SSBO via gl_VertexIndex ---
	{
		const Modules m = compile(*p_lineShader, debugName + "_Line");
		PipelineBuilder builder;
		addStages(builder, m, "GizmoPass_Line");
		ConfigureCommonState(builder);
		builder.SetVertexInput();
		builder.SetInputAssembly(vk::PrimitiveTopology::eTriangleList);
		builder.SetPushConstantRanges({kOverlayRange});
		p_pipelines.push_back(builder.BuildGraphicsPipeline(device));
	}

	// --- [1] Points: one native point per sprite, sized via gl_PointSize ---
	{
		const Modules m = compile(*p_pointShader, debugName + "_Point");
		PipelineBuilder builder;
		addStages(builder, m, "GizmoPass_Point");
		ConfigureCommonState(builder);
		builder.SetVertexInput();
		builder.SetInputAssembly(vk::PrimitiveTopology::ePointList);
		builder.SetPushConstantRanges({kOverlayRange});
		p_pipelines.push_back(builder.BuildGraphicsPipeline(device));
	}
}

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

PassStats GizmoPass::Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx)
{
	PassStats stats{};

	// --- 1. No gesture: touch no image, so every state the upstream pass left behind
	//        stays valid, including the layout the swapchain blit expects. This is
	//        also what lets the pass sit in the graph unconditionally ---
	const GizmoDrawList* list = ctx.editor.gizmoDraw;
	if (!list || list->Empty())
	{
		return stats;
	}

	const CameraGPU& camera = cache.GetCameraGPU();
	if (!camera.IsValid())
	{
		return stats;   // no frame has published a camera yet
	}

	// The descriptor index wraps over the allocated sets; the cache is queried with
	// the raw frame index, because GizmoCache grows its own slot ring to match.
	// Collapsing the two into one value looks tidier and is wrong.
	const size_t frameIdx = ctx.frameIndex % p_descriptorSets.size();
	const GizmoCache::FrameView frame = cache.GetGizmoCache().GetFrame(ctx.frameIndex);
	if (!frame.segments || !frame.points)
	{
		return stats;   // RenderCache::UpdateGizmoDraw() has not run for this frame
	}

	// --- 2. Point this frame's set at what the cache currently holds. Rewritten
	//        every frame because GizmoCache replaces a buffer when the geometry
	//        outgrows it, which changes the handle the descriptor names ---
	p_descriptorSets[frameIdx].WriteBuffer(0, camera.GetDescriptorInfo(),
	                                       vk::DescriptorType::eUniformBuffer);
	p_descriptorSets[frameIdx].WriteBuffer(1, frame.segments->GetDescriptorInfo(),
	                                       vk::DescriptorType::eStorageBuffer);
	p_descriptorSets[frameIdx].WriteBuffer(2, frame.points->GetDescriptorInfo(),
	                                       vk::DescriptorType::eStorageBuffer);

	const vk::Extent2D extent{ctx.width, ctx.height};

	// proj[1][1] is negative (Camera flips Y for Vulkan NDC) and the shaders want a
	// magnitude, so the sign is dropped. Read from the projection, never viewProj:
	// the view rotation mixes into viewProj[1][1] and the y scale is no longer
	// isolated there.
	const glm::mat4& proj = camera.GetProjection();
	const GizmoPushConstants overlayPush{
		{static_cast<float>(extent.width), static_cast<float>(extent.height)},
		std::abs(proj[1][1]) * static_cast<float>(extent.height) * 0.5f,
		p_maxPointSizePx};

	// --- 3. One attachment, loaded rather than cleared: the shaded image must
	//        survive underneath the guides ---
	auto& targetAtt = cache.GetAttachment(p_target, extent);

	// Needed even when DebugPass just left the target in ColorAttachment:
	// Barrier::Transition has no before == after early-out, so this still emits the
	// (ColorAttachmentOutput, Write|Read) → (ColorAttachmentOutput, Write|Read)
	// write-after-write dependency two chained LOAD_OP_LOAD passes require. When
	// DebugPass early-returned instead, the target is still in ShaderWrite from the
	// compute pass that produced it and the same call covers that too — the
	// before-state is read from the tracked Image, not assumed.
	Barrier::Transition(cmdBuf, targetAtt, ImageState::ColorAttachment);

	const vk::RenderingAttachmentInfo colorInfo(
		*targetAtt.ImageViewHandle(),
		vk::ImageLayout::eColorAttachmentOptimal,
		vk::ResolveModeFlagBits::eNone, nullptr, vk::ImageLayout::eUndefined,
		vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore, vk::ClearValue{});

	const std::array<vk::RenderingAttachmentInfo, 1> colorInfos{colorInfo};
	const vk::Rect2D renderArea({0, 0}, extent);

	// Null depth attachment: the pipelines declare no depth format, so binding one
	// would be a format mismatch rather than a harmless extra.
	cmdBuf.beginRendering(vk::RenderingInfo({}, renderArea, 1, 0, colorInfos, nullptr, nullptr));

	cmdBuf.setViewport(0, vk::Viewport(0.0f, 0.0f,
	                                   static_cast<float>(extent.width),
	                                   static_cast<float>(extent.height),
	                                   0.0f, 1.0f));
	cmdBuf.setScissor(0, renderArea);

	// --- 4. Exactly two draws, no depth toggle anywhere ---
	const auto bindFor = [&](size_t pipelineIndex) -> vk::PipelineLayout
	{
		const Pipeline& pipe = p_pipelines[pipelineIndex];
		cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipe.pipeline);
		cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
		                          *pipe.pipelineLayout, 0,
		                          {p_descriptorSets[frameIdx].handle()}, {});
		return *pipe.pipelineLayout;
	};

	if (frame.segmentCount > 0)
	{
		const vk::PipelineLayout layout = bindFor(kLinePipeline);
		cmdBuf.pushConstants<GizmoPushConstants>(
			layout, vk::ShaderStageFlagBits::eVertex, 0, overlayPush);
		cmdBuf.draw(kVerticesPerSegment * frame.segmentCount, 1, 0, 0);
		++stats.drawCalls;
	}

	// Points last on purpose: depth is off and blending is alpha-over, so submission
	// order decides what lands on top and the pivot dot must sit above the axis line
	// (GizmoDrawList.h records the same contract on the producing side).
	if (frame.pointCount > 0)
	{
		const vk::PipelineLayout layout = bindFor(kPointPipeline);
		cmdBuf.pushConstants<GizmoPushConstants>(
			layout, vk::ShaderStageFlagBits::eVertex, 0, overlayPush);
		cmdBuf.draw(frame.pointCount, 1, 0, 0);
		++stats.drawCalls;
	}

	cmdBuf.endRendering();

	// The target is left in ColorAttachment. Its consumer — the swapchain blit —
	// transitions it and thereby picks up a src scope that actually covers the draws
	// above; handing it over pre-transitioned would hide them.

	return stats;
}

// ---------------------------------------------------------------------------
// Graph IO
// ---------------------------------------------------------------------------

PassIO GizmoPass::GetIO() const
{
	// The target is both read (LOAD_OP_LOAD) and written, which is how the graph
	// learns this pass must follow whoever wrote it last. A node's own name never
	// produces a self-edge, so listing it twice is safe. It must be p_target rather
	// than a fixed alias: Connect() matches sockets by AttachmentName, and an unwired
	// input is treated as external rather than as an error.
	PassIO io;
	io.name = "GizmoPass";
	io.reads = {
		{p_target},
	};
	io.writes = {
		{p_target},
	};
	return io;
}

} // namespace neurus
