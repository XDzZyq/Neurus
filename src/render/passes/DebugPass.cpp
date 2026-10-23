/**
 * @file DebugPass.cpp
 * @brief Debug overlay raster pass implementation.
 */

#include "passes/DebugPass.h"

#include "RenderCache.h"
#include "RenderContext.h"
#include "render/Barrier.h"
#include "shaders/ShaderLibrary.h"
#include "core/Log.h"

#include "../resources/MeshGPU.h"
#include "RenderFrameSnapshot.h"
#include "render/overlay/DebugDrawList.h"

#include <algorithm>
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

DebugPass::DebugPass(const vk::raii::Device& device,
                     const vk::raii::PhysicalDevice& physicalDevice,
                     uint32_t framesInFlight)
	: p_layout(CreateLayout(device))
	, p_descriptorPool(device,
	                   framesInFlight,
	                   DescriptorPool::CalculatePoolSizes({&p_layout}, framesInFlight))
	, p_descriptorSets(p_descriptorPool.Allocate(p_layout, framesInFlight))
	// The label is only a log tag - ShaderLibrary does not cache by it - so it
	// names the consumer, not the file.
	, p_lineShader(ShaderLibrary::LoadRenderShader("DebugLine",
	                                               NEURUS_SHADER_DIR "render/overlay_line.vert",
	                                               NEURUS_SHADER_DIR "render/overlay_line.frag"))
	, p_pointShader(ShaderLibrary::LoadRenderShader("DebugPoint",
	                                                NEURUS_SHADER_DIR "render/overlay_point.vert",
	                                                NEURUS_SHADER_DIR "render/overlay_point.frag"))
	, p_wireShader(ShaderLibrary::LoadRenderShader("DebugWire",
	                                               NEURUS_SHADER_DIR "render/debug_wire.vert",
	                                               NEURUS_SHADER_DIR "render/debug_wire.frag"))
{
	p_device = &device;
	p_physicalDevice = &physicalDevice;

	// gl_PointSize above this is undefined behaviour, not a soft clamp, so the
	// real limit travels to the shader instead of a hardcoded guess.
	p_maxPointSizePx = physicalDevice.getProperties().limits.pointSizeRange[1];

	// No buffers are created here: bindings 0-2 name the cache's camera UBO and
	// debug SSBOs, whose handles change when the geometry grows, so Record()
	// writes the sets each frame instead.
#ifdef _DEBUG
	for (auto& set : p_descriptorSets)
	{
		set.SetDebugName("DebugPass_Set");
	}
#endif

	// --- Wireframe vertex input layout ---
	p_meshVertexLayout.AddAttribute(0, vk::Format::eR32G32B32Sfloat, 0);   // pos    @ 0
	p_meshVertexLayout.AddAttribute(1, vk::Format::eR32G32B32Sfloat, 12);  // normal @ 12
	p_meshVertexLayout.AddAttribute(2, vk::Format::eR32G32Sfloat, 24);      // uv     @ 24

	BuildPipeline(device, "DebugPass");

	NEURUS_LOG("[DebugPass] framesInFlight=" << framesInFlight
	           << " maxPointSizePx=" << p_maxPointSizePx);
}

// ---------------------------------------------------------------------------
// Descriptor set layout factory
// ---------------------------------------------------------------------------

DescriptorSetLayout DebugPass::CreateLayout(const vk::raii::Device& device)
{
	// All three bindings are vertex-stage only: the fragment shaders work purely
	// from interpolated inputs, which is what keeps them branch-light.
	return BuildLayout()
		.AddBinding(0, vk::DescriptorType::eUniformBuffer, vk::ShaderStageFlagBits::eVertex)
		.AddBinding(1, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex)
		.AddBinding(2, vk::DescriptorType::eStorageBuffer, vk::ShaderStageFlagBits::eVertex)
		.Build(device);
}

// ---------------------------------------------------------------------------
// Shared pipeline state
// ---------------------------------------------------------------------------

void DebugPass::ConfigureCommonState(PipelineBuilder& builder)
{
	// One color target: the post-AA image.
	builder.SetColorFormats({vk::Format::eR16G16B16A16Sfloat});
	builder.SetColorBlendAttachment();   // straight alpha-over

	// Depth is read but never written: an overlay must not stop the next debug
	// primitive at the same depth from drawing, and it must not corrupt the
	// G-Buffer depth that later frames' accumulation still reads.
	builder.SetDepthFormat(vk::Format::eD32Sfloat);
	builder.SetDepthStencil(true, false, vk::CompareOp::eLess);

	// X-ray flips this per draw range instead of needing a second pipeline.
	// Core since Vulkan 1.3 (VK_EXT_extended_dynamic_state was promoted with no
	// feature bit), and the instance already requires 1.3-core dynamic rendering.
	builder.AddDynamicState(vk::DynamicState::eDepthTestEnable);

	builder.SetRasterization(vk::PolygonMode::eFill,
	                         vk::CullModeFlagBits::eNone,
	                         vk::FrontFace::eClockwise);
	builder.SetMultisampling();
	builder.AddDescriptorSetLayout(*p_layout.layout());
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

void DebugPass::BuildPipeline(const vk::raii::Device& device, const std::string& debugName)
{
	if (!p_lineShader || !p_pointShader || !p_wireShader)
	{
		throw std::runtime_error("DebugPass: debug shaders not loaded or invalid");
	}

	// Modules are temporaries that must outlive BuildGraphicsPipeline, so each
	// pipeline is built inside its own scope with its own pair. Compiling and
	// wiring them is identical for all three, hence the helper; only the
	// primitive-specific state below differs.
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

	const vk::PushConstantRange kOverlayRange(vk::ShaderStageFlagBits::eVertex,
	                                          0, sizeof(DebugPushConstants));
	const vk::PushConstantRange kWireRange(vk::ShaderStageFlagBits::eVertex,
	                                       0, sizeof(DebugWirePushConstants));

	// --- [0] Lines: no vertex buffer; geometry comes from the SSBO ---
	{
		const Modules m = compile(*p_lineShader, debugName + "_Line");
		PipelineBuilder builder;
		addStages(builder, m, "DebugPass_Line");
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
		addStages(builder, m, "DebugPass_Point");
		ConfigureCommonState(builder);
		builder.SetVertexInput();
		builder.SetInputAssembly(vk::PrimitiveTopology::ePointList);
		builder.SetPushConstantRanges({kOverlayRange});
		p_pipelines.push_back(builder.BuildGraphicsPipeline(device));
	}

	// --- [2] Wireframe: MeshGPU geometry rasterized as lines ---
	{
		const Modules m = compile(*p_wireShader, debugName + "_Wire");
		PipelineBuilder builder;
		addStages(builder, m, "DebugPass_Wire");
		ConfigureCommonState(builder);
		builder.SetVertexInput(p_meshVertexLayout);
		builder.SetInputAssembly(vk::PrimitiveTopology::eTriangleList);
		// Overrides the eFill from ConfigureCommonState. Line width stays 1 px:
		// wideLines is unavailable on MoltenVK.
		builder.SetRasterization(vk::PolygonMode::eLine,
		                         vk::CullModeFlagBits::eNone,
		                         vk::FrontFace::eClockwise);
		builder.SetPushConstantRanges({kWireRange});
		p_pipelines.push_back(builder.BuildGraphicsPipeline(device));
	}
}

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

PassStats DebugPass::Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx)
{
	PassStats stats{};

	// Nothing to draw: touch no image, so every state the upstream pass left
	// behind stays valid, including the layout the blit expects. The gate lives
	// in the producer - r_debug_draw decides whether the overlay is published at
	// all - so hasDebugDraw is false for a disabled overlay and the pass still
	// does no work without the graph topology ever changing.
	const RenderFrameSnapshot& snapshot = ctx.Frame();
	if (!snapshot.HasDebugDraw() || snapshot.debugDraw.Empty())
	{
		return stats;
	}

	const CameraGPU& camera = cache.GetCameraGPU();
	if (!camera.IsValid())
	{
		return stats;   // no frame has published a camera yet
	}

	const size_t frameIdx = ctx.frameIndex % p_descriptorSets.size();
	const DebugCache::FrameView frame = cache.GetDebugCache().GetFrame(ctx.frameIndex);
	if (!frame.segments || !frame.points)
	{
		return stats;   // the debug buffers have not been uploaded for this frame
	}

	// Point this frame's set at what the cache currently holds; the handles
	// change whenever the debug geometry outgrows its buffers.
	p_descriptorSets[frameIdx].WriteBuffer(0, camera.GetDescriptorInfo(),
	                                       vk::DescriptorType::eUniformBuffer);
	p_descriptorSets[frameIdx].WriteBuffer(1, frame.segments->GetDescriptorInfo(),
	                                       vk::DescriptorType::eStorageBuffer);
	p_descriptorSets[frameIdx].WriteBuffer(2, frame.points->GetDescriptorInfo(),
	                                       vk::DescriptorType::eStorageBuffer);

	const vk::Extent2D extent{ctx.width, ctx.height};

	// proj[1][1] is negative here (Camera flips Y for Vulkan NDC), and the shaders
	// want a magnitude, so the sign is dropped rather than propagated. It is read
	// from the projection CameraGPU kept on the CPU, not from viewProj: the view
	// rotation mixes into viewProj[1][1], so the y scale is no longer isolated
	// there.
	const glm::mat4& proj = camera.GetProjection();
	const DebugPushConstants overlayPush{
		{static_cast<float>(extent.width), static_cast<float>(extent.height)},
		std::abs(proj[1][1]) * static_cast<float>(extent.height) * 0.5f,
		p_maxPointSizePx};
	// --- 3. Attachments. Both are loaded, not cleared: the shaded image and the
	//        G-Buffer depth must survive underneath the overlay ---
	auto& targetAtt = cache.GetAttachment(p_target, extent);
	auto& depthAtt  = cache.GetAttachment(AttachmentName::Depth, extent);

	// The target arrives in ShaderWrite straight from a compute dispatch -
	// OverlayCopyPass's in production - and this transition is what makes those
	// writes visible to the raster stage: src becomes (ComputeShader, ShaderWrite),
	// dst (ColorAttachmentOutput, ColorAttachmentWrite|Read). The producer must not
	// pre-transition it to TransferSrc for the blit: a src scope of TransferRead
	// names no writes. LOAD_OP_LOAD also needs the read bit, which ColorAttachment
	// carries.
	Barrier::Transition(cmdBuf, targetAtt, ImageState::ColorAttachment);
	Barrier::Transition(cmdBuf, depthAtt, ImageState::DepthAttachment);

	// Built by hand rather than through Pass::PresetClearValues: no PassType
	// maps to load-and-store on both attachments, which is what an in-place
	// overlay needs.
	const vk::RenderingAttachmentInfo colorInfo(
		*targetAtt.ImageViewHandle(),
		vk::ImageLayout::eColorAttachmentOptimal,
		vk::ResolveModeFlagBits::eNone, nullptr, vk::ImageLayout::eUndefined,
		vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore, vk::ClearValue{});

	// eStore even though depthWriteEnable is false, so the depth image keeps the
	// contents later passes and the next frame's accumulation still read.
	const vk::RenderingAttachmentInfo depthInfo(
		*depthAtt.ImageViewHandle(),
		vk::ImageLayout::eDepthStencilAttachmentOptimal,
		vk::ResolveModeFlagBits::eNone, nullptr, vk::ImageLayout::eUndefined,
		vk::AttachmentLoadOp::eLoad, vk::AttachmentStoreOp::eStore, vk::ClearValue{});

	const std::array<vk::RenderingAttachmentInfo, 1> colorInfos{colorInfo};
	const vk::Rect2D renderArea({0, 0}, extent);

	cmdBuf.beginRendering(vk::RenderingInfo({}, renderArea, 1, 0, colorInfos, &depthInfo, nullptr));

	cmdBuf.setViewport(0, vk::Viewport(0.0f, 0.0f,
	                                   static_cast<float>(extent.width),
	                                   static_cast<float>(extent.height),
	                                   0.0f, 1.0f));
	cmdBuf.setScissor(0, renderArea);
	const auto bindFor = [&](size_t pipelineIndex) -> vk::PipelineLayout
	{
		const Pipeline& pipe = p_pipelines[pipelineIndex];
		cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipe.pipeline);

		// Re-bound per pipeline rather than once per frame: the line/point layouts
		// declare a 16 B push range and the wire layout a 72 B one, and Vulkan
		// layout compatibility requires identical push-constant ranges as well as
		// identical set layouts - so binding across that boundary disturbs set 0.
		cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
		                          *pipe.pipelineLayout, 0,
		                          {p_descriptorSets[frameIdx].handle()}, {});
		return *pipe.pipelineLayout;
	};

	if (frame.segmentCount > 0)
	{
		const vk::PipelineLayout layout = bindFor(kLinePipeline);
		cmdBuf.pushConstants<DebugPushConstants>(
			layout, vk::ShaderStageFlagBits::eVertex, 0, overlayPush);

		if (frame.xraySegmentStart > 0)
		{
			cmdBuf.setDepthTestEnable(VK_TRUE);
			cmdBuf.draw(kVerticesPerSegment * frame.xraySegmentStart, 1, 0, 0);
			++stats.drawCalls;
		}
		if (frame.segmentCount > frame.xraySegmentStart)
		{
			const uint32_t count = frame.segmentCount - frame.xraySegmentStart;
			cmdBuf.setDepthTestEnable(VK_FALSE);
			cmdBuf.draw(kVerticesPerSegment * count, 1,
			            kVerticesPerSegment * frame.xraySegmentStart, 0);
			++stats.drawCalls;
		}
	}
	if (frame.pointCount > 0)
	{
		const vk::PipelineLayout layout = bindFor(kPointPipeline);
		cmdBuf.pushConstants<DebugPushConstants>(
			layout, vk::ShaderStageFlagBits::eVertex, 0, overlayPush);

		if (frame.xrayPointStart > 0)
		{
			cmdBuf.setDepthTestEnable(VK_TRUE);
			cmdBuf.draw(frame.xrayPointStart, 1, 0, 0);
			++stats.drawCalls;
		}
		if (frame.pointCount > frame.xrayPointStart)
		{
			cmdBuf.setDepthTestEnable(VK_FALSE);
			cmdBuf.draw(frame.pointCount - frame.xrayPointStart, 1, frame.xrayPointStart, 0);
			++stats.drawCalls;
		}
	}

	// Wireframes are not pre-partitioned - they draw from their own mesh
	// buffers, so a range cannot merge them - and the depth toggle is only
	// re-issued when it differs from the current state.
	if (!snapshot.debugDraw.wireMeshes.empty())
	{
		const vk::PipelineLayout layout = bindFor(kWirePipeline);
		bool depthTest = true;
		cmdBuf.setDepthTestEnable(VK_TRUE);

		for (const auto& wire : snapshot.debugDraw.wireMeshes)
		{
			MeshGPU* gpu = cache.GetMeshGPU(wire.meshObjectId);
			if (!gpu || !gpu->vertexBuffer || !gpu->indexBuffer)
			{
				continue;   // geometry not uploaded yet; it appears next frame
			}

			const bool wantDepth = (wire.flags & DebugFlag::XRay) == 0u;
			if (wantDepth != depthTest)
			{
				depthTest = wantDepth;
				cmdBuf.setDepthTestEnable(wantDepth ? VK_TRUE : VK_FALSE);
			}

			cmdBuf.pushConstants<DebugWirePushConstants>(
				layout, vk::ShaderStageFlagBits::eVertex, 0,
				DebugWirePushConstants{wire.model, wire.rgba, wire.flags});

			cmdBuf.bindVertexBuffers(0, gpu->vertexBuffer->buffer(), vk::DeviceSize{0});
			cmdBuf.bindIndexBuffer(gpu->indexBuffer->buffer(), 0, vk::IndexType::eUint32);
			cmdBuf.drawIndexed(gpu->indexCount, 1, 0, 0, 0);
			++stats.drawCalls;
		}
	}

	cmdBuf.endRendering();

	// The target is left in ColorAttachment; the swapchain blit transitions it
	// and thereby picks up a src scope that covers the draws above.

	return stats;
}

// ---------------------------------------------------------------------------
// Graph IO
// ---------------------------------------------------------------------------

PassIO DebugPass::GetIO() const
{
	// The target appears on both sides: the overlay loads the shaded image and
	// stores it back in place, which orders this pass after whichever pass last
	// wrote it. The name must be p_target, not a fixed alias: Connect() matches
	// sockets by AttachmentName, and an unwired input is treated as external,
	// which would leave this pass's order unconstrained.
	PassIO io;
	io.name = "DebugPass";
	io.reads = {
		{p_target},
		{AttachmentName::Depth},
	};
	io.writes = {
		{p_target},
	};
	return io;
}

} // namespace neurus
