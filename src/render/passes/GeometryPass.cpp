/**
 * @file GeometryPass.cpp
 * @brief G-Buffer geometry pass implementation.
 */

#include "passes/GeometryPass.h"

#include "passes/Pass.h"
#include "RenderCache.h"
#include "RenderContext.h"
#include "render/Barrier.h"
#include "PipelineBuilder.h"
#include "shaders/ShaderLibrary.h"
#include "shaders/RenderShader.h"
#include "core/Log.h"

#include "../resources/MeshGPU.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace neurus {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GeometryPass::GeometryPass(const vk::raii::Device& device,
                           const vk::raii::PhysicalDevice& physicalDevice)
	// --- Descriptor set layout ---
	: p_cameraLayout(CreateCameraLayout(device))
	// --- Descriptor pool (1 set, 1 UBO) ---
	, p_descriptorPool(device,
	                   1,
	                   DescriptorPool::CalculatePoolSizes({&p_cameraLayout}, 1))
	// --- Camera descriptor set (allocated from pool) ---
	, p_cameraDescriptorSet(std::move(
	      p_descriptorPool.Allocate(p_cameraLayout, 1).front()))
	// --- Self-load shaders via ShaderLibrary ---
	, p_shader(
		ShaderLibrary::LoadRenderShader("GeometryPass",
		                                 "res/shaders/render/gbuffer.vert",
		                                 "res/shaders/render/gbuffer.frag"))
{
	p_device = &device;
	p_physicalDevice = &physicalDevice;

	// The camera UBO is RenderCache's (see CameraGPU), so binding 0 is written in
	// Record() from the cache rather than here. With one frame in flight the handle
	// never actually changes, but writing it per frame keeps this pass correct if
	// CameraGPU ever becomes a ring.

#ifdef _DEBUG
	p_cameraDescriptorSet.SetDebugName("GeometryPass_CameraSet");
#endif

	// --- Build vertex input layout ---
	p_vertexLayout.AddAttribute(0, vk::Format::eR32G32B32Sfloat, 0);   // pos @ 0
	p_vertexLayout.AddAttribute(1, vk::Format::eR32G32B32Sfloat, 12);  // normal @ 12
	p_vertexLayout.AddAttribute(2, vk::Format::eR32G32Sfloat, 24);      // uv @ 24

	// --- Create graphics pipeline from self-loaded shader ---
	BuildPipeline(device, "GeometryPass");

	NEURUS_LOG("[GeometryPass] shader=" << (p_shader ? "OK" : "FAIL")
	          << " colorAttachments=5"
	          << " depthAttachments=1"
	          << " vertexStride=" << p_vertexLayout.GetStride());
}

// ---------------------------------------------------------------------------
// Descriptor set layout factory
// ---------------------------------------------------------------------------

DescriptorSetLayout GeometryPass::CreateCameraLayout(const vk::raii::Device& device)
{
	return BuildLayout()
		.AddBinding(0,
		            vk::DescriptorType::eUniformBuffer,
		            vk::ShaderStageFlagBits::eVertex)
		.Build(device);
}

// ---------------------------------------------------------------------------
// Pipeline creation
// ---------------------------------------------------------------------------

void GeometryPass::BuildPipeline(const vk::raii::Device& device,
                                  const std::string& debugName)
{
	// --- Guard: shader must be valid ---
	if (!p_shader)
	{
		throw std::runtime_error("GeometryPass: Render shader not loaded or invalid");
	}

	// --- Compile and create temporary shader modules ---
	auto vertSpv = ShaderLibrary::Compile(p_shader->GetStage(ShaderType::VERTEX),
	                                      ShaderType::VERTEX, debugName + "_vert");
	auto fragSpv = ShaderLibrary::Compile(p_shader->GetStage(ShaderType::FRAGMENT),
	                                      ShaderType::FRAGMENT, debugName + "_frag");
	vk::ShaderModuleCreateInfo vertSmCI({}, vertSpv);
	vk::raii::ShaderModule vertModule(device, vertSmCI);
	vk::ShaderModuleCreateInfo fragSmCI({}, fragSpv);
	vk::raii::ShaderModule fragModule(device, fragSmCI);
	vk::PipelineShaderStageCreateInfo vertStageCI({}, vk::ShaderStageFlagBits::eVertex, *vertModule, "main");
	vk::PipelineShaderStageCreateInfo fragStageCI({}, vk::ShaderStageFlagBits::eFragment, *fragModule, "main");

	// --- Build pipeline via PipelineBuilder ---
	PipelineBuilder builder;
	builder.SetDebugName(debugName.c_str())
	       .AddShaderStage(vertStageCI)
	       .AddShaderStage(fragStageCI);
	ConfigureGBufferPipeline(builder);
	p_pipelines.push_back(builder.BuildGraphicsPipeline(device));
}

// ---------------------------------------------------------------------------
// Shared G-Buffer pipeline configuration
// ---------------------------------------------------------------------------

void GeometryPass::ConfigureGBufferPipeline(PipelineBuilder& builder)
{
	static const vk::PipelineColorBlendAttachmentState kBlendNone(
		VK_FALSE, vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
		vk::BlendFactor::eOne, vk::BlendFactor::eZero, vk::BlendOp::eAdd,
		vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
		vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);

	builder.SetColorFormats({
		vk::Format::eR16G16B16A16Sfloat,
		vk::Format::eR16G16B16A16Sfloat,
		vk::Format::eR8G8B8A8Srgb,
		vk::Format::eR8G8B8A8Unorm,
		vk::Format::eR32Uint
	});
	builder.SetDepthFormat(vk::Format::eD32Sfloat);
	builder.SetDepthStencil(true, true, vk::CompareOp::eLess);
	builder.SetVertexInput(p_vertexLayout);
	builder.AddDescriptorSetLayout(*p_cameraLayout.layout());

	vk::PushConstantRange pushRange(vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
	                                0, sizeof(MeshPushConstants));
	builder.SetPushConstantRanges({pushRange});

	builder.SetInputAssembly(vk::PrimitiveTopology::eTriangleList);
	builder.SetRasterization(vk::PolygonMode::eFill,
	                         vk::CullModeFlagBits::eNone,
	                         vk::FrontFace::eClockwise);
	builder.SetMultisampling();

	for (int i = 0; i < 5; ++i)
		builder.AddColorBlendAttachment(kBlendNone);
}

// ---------------------------------------------------------------------------
// Per-mesh custom pipeline creation
// ---------------------------------------------------------------------------

Pipeline GeometryPass::CreatePerMeshPipeline(const Shader& shader, ShaderType /*stageType*/)
{
	// Create shader modules for all available stages
	struct StageEntry
	{
		vk::PipelineShaderStageCreateInfo ci;
		vk::raii::ShaderModule module;
	};
	std::vector<StageEntry> stages;

	for (auto t : {ShaderType::VERTEX, ShaderType::FRAGMENT})
	{
		if (!shader.HasStage(t)) continue;
		const auto& unit = shader.GetStage(t);
		if (unit.spv.empty()) continue;

		vk::ShaderStageFlagBits flag = vk::ShaderStageFlagBits::eVertex;
		if (t == ShaderType::FRAGMENT) flag = vk::ShaderStageFlagBits::eFragment;
		else if (t == ShaderType::COMPUTE) flag = vk::ShaderStageFlagBits::eCompute;

		vk::ShaderModuleCreateInfo smCI({}, unit.spv);
		vk::raii::ShaderModule module(*p_device, smCI);
		vk::PipelineShaderStageCreateInfo stageCI({}, flag, *module, "main");

		stages.push_back({stageCI, std::move(module)});
	}

	if (stages.empty()) return Pipeline{};

	// Build pipeline using shared G-Buffer configuration
	PipelineBuilder builder;
	builder.SetDebugName("PerMeshPipeline");
	for (auto& s : stages)
		builder.AddShaderStage(s.ci);
	ConfigureGBufferPipeline(builder);
	return builder.BuildGraphicsPipeline(*p_device);
}

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

PassStats GeometryPass::Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx)
{
	PassStats stats{};

	// --- Cast scene UID to Scene* for access to Scene-specific members ---
	const auto* scene = static_cast<const Scene*>(ctx.editor.scene);

	// --- Extract per-frame context ---
	const vk::Extent2D renderExtent{ctx.width, ctx.height};

	// --- 1. Point set 0 at RenderCache's shared camera UBO ---
	// DeferredRenderer::recordFrame() uploads it once per frame, before any pass
	// records, so this pass only names the buffer. Bailing out when no frame has
	// published a camera yet keeps the descriptor from naming an unwritten UBO.
	const CameraGPU& camera = cache.GetCameraGPU();
	if (!camera.IsValid())
	{
		return stats;
	}
	p_cameraDescriptorSet.WriteBuffer(0, camera.GetDescriptorInfo(),
	                                  vk::DescriptorType::eUniformBuffer);

	// --- 2. Collect G-Buffer attachment image views ---
	const std::array<AttachmentName, 5> gBufferColorAttachments = {
		AttachmentName::Position,
		AttachmentName::Normal,
		AttachmentName::Albedo,
		AttachmentName::MetallicRoughness,
		AttachmentName::IDBuffer,
	};

	std::vector<vk::ImageView> colorViews;
	colorViews.reserve(5);
	for (const auto& attName : gBufferColorAttachments)
	{
		auto& att = cache.GetAttachment(attName, renderExtent);
		Barrier::Transition(cmdBuf, att, ImageState::ColorAttachment);
		colorViews.push_back(*att.ImageViewHandle());
	}

	auto& depthAtt = cache.GetAttachment(AttachmentName::Depth, renderExtent);
	Barrier::Transition(cmdBuf, depthAtt, ImageState::DepthAttachment);
	vk::ImageView depthView = *depthAtt.ImageViewHandle();

	// --- 3. Begin G-Buffer dynamic rendering pass ---
	const auto clearValues = Pass::PresetClearValues(Pass::PassType::G_BUFFER);

	BeginPass(cmdBuf, colorViews, &depthView, clearValues, renderExtent);

	// --- 4. Set viewport and scissor (dynamic state) ---
	const vk::Viewport viewport(0.0f, 0.0f,
	                            static_cast<float>(renderExtent.width),
	                            static_cast<float>(renderExtent.height),
	                            0.0f, 1.0f);
	cmdBuf.setViewport(0, viewport);

	const vk::Rect2D scissor({0, 0}, renderExtent);
	cmdBuf.setScissor(0, scissor);

	// --- 5. Draw each mesh with optional per-mesh shader pipeline ---
	if (scene)
	{
		for (const auto& [id, mesh] : scene->mesh_list)
		{
			if (!mesh || !mesh->o_mesh) continue;
			if (!mesh->is_viewport || !mesh->is_rendered) continue;

			MeshGPU* gpuPtr = cache.GetMeshGPU(mesh->GetObjectID());
			if (!gpuPtr)
			{
				NEURUS_ERR("[GeometryPass] GetMeshGPU returned nullptr for objectId=" << mesh->GetObjectID());
				continue;
			}

			// --- Per-mesh shader override (version-aware pipeline lookup) ---
			bool usingCustomPipeline = false;
			vk::PipelineLayout activeLayout = *p_pipelines[0].pipelineLayout;
			if (mesh->o_shader)
			{
				int version = mesh->o_shader->GetVersion();

				Pipeline* customPipeline = cache.GetPipeline(mesh->GetObjectID(), version);
				if (!customPipeline)
				{
					Pipeline newPipeline = CreatePerMeshPipeline(*mesh->o_shader, ShaderType::VERTEX);
					// Do not cache/bind a failed (empty) pipeline — fall back to the default one.
					if (*newPipeline.pipeline)
					{
						cache.UsePipeline(mesh->GetObjectID(), std::move(newPipeline), version);
						customPipeline = cache.GetPipeline(mesh->GetObjectID(), version);
					}
					else
					{
						NEURUS_ERR("[GeometryPass] Per-mesh pipeline creation failed for objectId="
						           << mesh->GetObjectID() << " — falling back to default pipeline");
					}
				}

				if (customPipeline)
				{
					cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
					                    *customPipeline->pipeline);
					// Bind camera descriptor set with the custom pipeline's layout
					cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
					    *customPipeline->pipelineLayout,
					    0,
					    {p_cameraDescriptorSet.handle()},
					    {});
					activeLayout = *customPipeline->pipelineLayout;
					usingCustomPipeline = true;
				}
			}

			// Fall back to default pipeline if no custom shader or pipeline not cached
			if (!usingCustomPipeline)
			{
				cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *p_pipelines[0].pipeline);
				cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
				                          *p_pipelines[0].pipelineLayout,
				                          0,
				                          {p_cameraDescriptorSet.handle()},
				                          {});
			}

			// --- Push constants ---
			const glm::mat4 model = mesh->GetModelMatrix();
			const glm::mat4 normalMat = glm::mat4(mesh->GetNormalMatrix());

			MeshPushConstants pushConstants{model, normalMat,
			                               static_cast<uint32_t>(mesh->GetObjectID())};

			// Push constants must use the layout of the currently bound pipeline
			// (default layout, or the per-mesh custom pipeline's layout when set).
			cmdBuf.pushConstants<MeshPushConstants>(activeLayout,
			    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			    0, pushConstants);

			// --- Draw ---
			const vk::DeviceSize vbOffset = 0;
			cmdBuf.bindVertexBuffers(0, gpuPtr->vertexBuffer->buffer(), vbOffset);
			cmdBuf.bindIndexBuffer(gpuPtr->indexBuffer->buffer(), 0, vk::IndexType::eUint32);
			++stats.drawCalls;
			cmdBuf.drawIndexed(gpuPtr->indexCount, 1, 0, 0, 0);
		}
	}

	// --- 6. End dynamic rendering pass ---
	EndPass(cmdBuf);

	return stats;
}

// ---------------------------------------------------------------------------
// BeginPass / EndPass (G_BUFFER-specific, moved from RenderPassManager)
// ---------------------------------------------------------------------------

void GeometryPass::BeginPass(vk::CommandBuffer cmdBuf,
                              std::span<const vk::ImageView> colorImageViews,
                              const vk::ImageView* pDepthImageView,
                              std::span<const vk::ClearValue> clearValues,
                              vk::Extent2D renderExtent)
{
	const uint32_t colorCount = static_cast<uint32_t>(colorImageViews.size());

	if (colorCount != Pass::ColorAttachmentCount(Pass::PassType::G_BUFFER))
	{
		throw std::invalid_argument(
			"GeometryPass::BeginPass: expected " + std::to_string(Pass::ColorAttachmentCount(Pass::PassType::G_BUFFER)) + " color attachments, got "
			+ std::to_string(colorCount));
	}

	const bool depthProvided = (pDepthImageView != nullptr);
	if (!depthProvided)
	{
		throw std::invalid_argument(
			"GeometryPass::BeginPass: depth attachment required for G_BUFFER pass");
	}

	// --- Build color attachment infos ---
	const auto colorLoadOp  = Pass::ColorLoadOpFor(Pass::PassType::G_BUFFER);
	const auto colorStoreOp = Pass::ColorStoreOpFor(Pass::PassType::G_BUFFER);

	std::vector<vk::RenderingAttachmentInfo> colorAttachmentInfos;
	colorAttachmentInfos.reserve(colorCount);

	for (uint32_t i = 0; i < colorCount; ++i)
	{
		colorAttachmentInfos.push_back(vk::RenderingAttachmentInfo(
			colorImageViews[i],
			vk::ImageLayout::eColorAttachmentOptimal,
			vk::ResolveModeFlagBits::eNone,
			nullptr,
			vk::ImageLayout::eUndefined,
			colorLoadOp,
			colorStoreOp,
			(i < static_cast<uint32_t>(clearValues.size())) ? clearValues[i] : vk::ClearValue{}));
	}

	// --- Build depth attachment info ---
	const auto depthLoadOp  = Pass::DepthLoadOpFor(Pass::PassType::G_BUFFER);
	const auto depthStoreOp = Pass::DepthStoreOpFor(Pass::PassType::G_BUFFER);

	const size_t depthClearIndex = colorCount;

	vk::RenderingAttachmentInfo depthAttachmentInfo(
		*pDepthImageView,
		vk::ImageLayout::eDepthStencilAttachmentOptimal,
		vk::ResolveModeFlagBits::eNone,
		nullptr,
		vk::ImageLayout::eUndefined,
		depthLoadOp,
		depthStoreOp,
		(depthClearIndex < clearValues.size()) ? clearValues[depthClearIndex] : vk::ClearValue{});

	// --- Build rendering info ---
	vk::RenderingInfo renderingInfo(
		{},
		vk::Rect2D({0, 0}, renderExtent),
		1,
		0,
		colorAttachmentInfos,
		&depthAttachmentInfo,
		nullptr);

	cmdBuf.beginRendering(renderingInfo);
}

void GeometryPass::EndPass(vk::CommandBuffer cmdBuf)
{
	cmdBuf.endRendering();
}

PassIO GeometryPass::GetIO() const
{
	// GeometryPass is a rasterization pass writing the G-Buffer MRT + depth.
	// It has no image reads (camera data comes via a UBO). Binding metadata is
	// unused — the pass self-manages its render pass and attachments; only the
	// resource identities drive RenderGraph edges to SSAO / Lighting / SelectionOutline.
	PassIO io;
	io.name  = "GeometryPass";
	io.writes = {
		{AttachmentName::Position},
		{AttachmentName::Normal},
		{AttachmentName::Albedo},
		{AttachmentName::MetallicRoughness},
		{AttachmentName::IDBuffer},
		{AttachmentName::Depth},
	};
	return io;
}

} // namespace neurus
