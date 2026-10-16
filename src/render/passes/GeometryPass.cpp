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

	// The camera UBO lives in the cache, so binding 0 is written in Record()
	// rather than here. With one frame in flight the handle never actually
	// changes, but writing it per frame keeps this pass correct if the UBO
	// ever becomes a ring.

#ifdef _DEBUG
	p_cameraDescriptorSet.SetDebugName("GeometryPass_CameraSet");
#endif

	p_vertexLayout.AddAttribute(0, vk::Format::eR32G32B32Sfloat, 0);   // pos @ 0
	p_vertexLayout.AddAttribute(1, vk::Format::eR32G32B32Sfloat, 12);  // normal @ 12
	p_vertexLayout.AddAttribute(2, vk::Format::eR32G32Sfloat, 24);      // uv @ 24

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
	if (!p_shader)
	{
		throw std::runtime_error("GeometryPass: Render shader not loaded or invalid");
	}

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

Pipeline GeometryPass::CreatePerMeshPipeline(const Shader& shader)
{
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

	PipelineBuilder builder;
	builder.SetDebugName("PerMeshPipeline");
	for (auto& s : stages)
		builder.AddShaderStage(s.ci);
	ConfigureGBufferPipeline(builder);
	return builder.BuildGraphicsPipeline(*p_device);
}

// ---------------------------------------------------------------------------
// Per-mesh pipeline pre-warm (Editor thread, off the frame path)
// ---------------------------------------------------------------------------

bool GeometryPass::EnsureMeshPipeline(RenderCache& cache, const Shader& shader)
{
	// Id and version must come from the same Shader: keying on the mesh's id
	// with the shader's version cannot discriminate a mesh relinked between two
	// shaders that share a version.
	const int      shaderId  = shader.GetObjectID();
	const int      version   = shader.GetVersion();
	PipelineCache& pipelines = cache.GetPipelineCache();

	if (pipelines.GetPipeline(shaderId, version)) return true;

	Pipeline pipeline = CreatePerMeshPipeline(shader);

	// No SPIR-V for any supported stage is an ordinary state while a shader is
	// being edited, so it is neither cached nor reported here - Record() logs
	// the resulting miss, latched.
	if (!*pipeline.pipeline) return false;

	pipelines.UsePipeline(shaderId, std::move(pipeline), version);
	p_reportedMissingPipelines.erase(shaderId);
	NEURUS_LOG("[GeometryPass] Pre-warmed per-mesh pipeline for shaderId="
	           << shaderId << " version=" << version);
	return true;
}

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

PassStats GeometryPass::Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx)
{
	PassStats stats{};

	const Scene* scene = ctx.scene;

	const vk::Extent2D renderExtent{ctx.width, ctx.height};

	// Point set 0 at the cache's shared camera UBO. Bailing out when no frame
	// has published a camera yet keeps the descriptor from naming an unwritten
	// UBO.
	const CameraGPU& camera = cache.GetCameraGPU();
	if (!camera.IsValid())
	{
		return stats;
	}
	p_cameraDescriptorSet.WriteBuffer(0, camera.GetDescriptorInfo(),
	                                  vk::DescriptorType::eUniformBuffer);

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

	const auto clearValues = Pass::PresetClearValues(Pass::PassType::G_BUFFER);

	BeginPass(cmdBuf, colorViews, &depthView, clearValues, renderExtent);

	const vk::Viewport viewport(0.0f, 0.0f,
	                            static_cast<float>(renderExtent.width),
	                            static_cast<float>(renderExtent.height),
	                            0.0f, 1.0f);
	cmdBuf.setViewport(0, viewport);

	const vk::Rect2D scissor({0, 0}, renderExtent);
	cmdBuf.setScissor(0, scissor);

	if (ctx.HasScene())
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

			bool usingCustomPipeline = false;
			vk::PipelineLayout activeLayout = *p_pipelines[0].pipelineLayout;
			if (mesh->o_shader)
			{
				const Shader& shader = *mesh->o_shader;
				const int shaderId = shader.GetObjectID();
				const int version  = shader.GetVersion();

				// Lookup only: compiling reads the live Shader's SPIR-V, which
				// is the pre-warm's job on the Editor thread. Not yet
				// render-thread-safe either - the two ints above come off the
				// live Shader and m_version is unsynchronised; taking them from
				// the frame snapshot would close that. A miss means the shader
				// has not compiled, and the mesh draws with the pass default.
				Pipeline* customPipeline =
				    cache.GetPipelineCache().GetPipeline(shaderId, version);
				if (!customPipeline)
				{
					if (p_reportedMissingPipelines.insert(shaderId).second)
					{
						NEURUS_LOG("[GeometryPass] No per-mesh pipeline for shaderId="
						           << shaderId << " version=" << version
						           << " (objectId=" << mesh->GetObjectID()
						           << ") - falling back to default pipeline");
					}
				}
				else
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

			if (!usingCustomPipeline)
			{
				cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *p_pipelines[0].pipeline);
				cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
				                          *p_pipelines[0].pipelineLayout,
				                          0,
				                          {p_cameraDescriptorSet.handle()},
				                          {});
			}

			const glm::mat4 model = mesh->GetModelMatrix();
			const glm::mat4 normalMat = glm::mat4(mesh->GetNormalMatrix());

			MeshPushConstants pushConstants{model, normalMat,
			                               static_cast<uint32_t>(mesh->GetObjectID())};

			// Push constants must use the layout of the currently bound pipeline
			// (default layout, or the per-mesh custom pipeline's layout when set).
			cmdBuf.pushConstants<MeshPushConstants>(activeLayout,
			    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
			    0, pushConstants);

			const vk::DeviceSize vbOffset = 0;
			cmdBuf.bindVertexBuffers(0, gpuPtr->vertexBuffer->buffer(), vbOffset);
			cmdBuf.bindIndexBuffer(gpuPtr->indexBuffer->buffer(), 0, vk::IndexType::eUint32);
			++stats.drawCalls;
			cmdBuf.drawIndexed(gpuPtr->indexCount, 1, 0, 0, 0);
		}
	}

	EndPass(cmdBuf);

	return stats;
}

// ---------------------------------------------------------------------------
// BeginPass / EndPass
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
	// It has no image reads, and binding metadata is unused because the pass
	// self-manages its attachments; only the resource identities drive the
	// RenderGraph edges.
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
