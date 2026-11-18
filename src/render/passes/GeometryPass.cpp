/**
 * @file GeometryPass.cpp
 * @brief G-Buffer geometry pass implementation.
 */

#include "passes/GeometryPass.hpp"

#include "passes/Pass.hpp"
#include "RenderCache.hpp"
#include "RenderContext.hpp"
#include "render/Barrier.hpp"
#include "PipelineBuilder.hpp"
#include "asset/data/render_graph/PassKinds.hpp"
#include "DescriptorLayout.hpp"
#include "shaders/ShaderLibrary.hpp"
#include "shaders/RenderShader.hpp"
#include "core/Log.hpp"

#include "../resources/MeshGPU.hpp"
#include "RenderFrameSnapshot.hpp"
#include "requests/RenderRequests.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace neurus {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

GeometryPass::GeometryPass(const vk::raii::Device& device,
                           const vk::raii::PhysicalDevice& physicalDevice)
	: GeometryPass(device, physicalDevice,
	               ShaderLibrary::LoadRenderShader(
	                   "GeometryPass",
	                   "res/shaders/render/gbuffer.vert.glsl",
	                   "res/shaders/render/gbuffer.frag.glsl"))
{
}

GeometryPass::GeometryPass(const vk::raii::Device& device,
                           const vk::raii::PhysicalDevice& physicalDevice,
                           std::unique_ptr<RenderShader> shader)
	// --- Descriptor set layout ---
	: p_cameraLayout(MakeDescriptorLayout(device, *shader))
	// --- Descriptor pool (1 set, 1 UBO) ---
	, p_descriptorPool(device,
	                   1,
	                   DescriptorPool::CalculatePoolSizes({&p_cameraLayout}, 1))
	// --- Camera descriptor set (allocated from pool) ---
	, p_cameraDescriptorSet(std::move(
	      p_descriptorPool.Allocate(p_cameraLayout, 1).front()))
	// --- Self-load shaders via ShaderLibrary ---
	, p_shader(std::move(shader))
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

	vk::PushConstantRange pushRange = MakePushConstantRange(
		*p_shader, vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment);
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

Pipeline GeometryPass::CreatePerMeshPipeline(const std::vector<uint32_t>& vertexSpv,
                                             const std::vector<uint32_t>& fragmentSpv)
{
	struct StageEntry
	{
		vk::PipelineShaderStageCreateInfo ci;
		vk::raii::ShaderModule module;
	};
	std::vector<StageEntry> stages;

	const std::array<std::pair<vk::ShaderStageFlagBits, const std::vector<uint32_t>*>, 2> inputs = {{
		{vk::ShaderStageFlagBits::eVertex,   &vertexSpv},
		{vk::ShaderStageFlagBits::eFragment, &fragmentSpv},
	}};

	for (const auto& [flag, spv] : inputs)
	{
		if (spv->empty()) continue;

		vk::ShaderModuleCreateInfo smCI({}, *spv);
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
// Per-mesh pipeline warm (Editor thread, off the frame path)
// ---------------------------------------------------------------------------

bool GeometryPass::EnsureMeshPipeline(RenderCache& cache, const ShaderData& shader)
{
	// One build path: snapshot the live Shader into a request and warm from it,
	// the same code the renderer's request drain runs.
	return EnsureMeshPipeline(cache, MakeEnsurePipelineRequest(shader));
}

bool GeometryPass::EnsureMeshPipeline(RenderCache& cache, const EnsurePipelineRequest& request)
{
	// Id and version come from the same Shader snapshot (MakeEnsurePipelineRequest):
	// keying a mesh's id with a shader's version cannot discriminate a mesh
	// relinked between two shaders that share a version.
	const int      shaderId  = request.shaderId;
	const int      version   = request.version;
	PipelineCache& pipelines = cache.GetPipelineCache();

	if (pipelines.GetPipeline(shaderId, version)) return true;

	// A G-buffer pipeline needs both a pre-raster and a fragment stage. A shader
	// compiles one unit at a time, so an in-between state carries SPIR-V for the
	// fragment only; building from it creates an invalid pipeline
	// (VUID-VkGraphicsPipelineCreateInfo-pStages-06896). Wait for the compile
	// that completes the set - Record() draws the pass default until then.
	if (request.vertexSpv.empty() || request.fragmentSpv.empty()) return false;

	Pipeline pipeline = CreatePerMeshPipeline(request.vertexSpv, request.fragmentSpv);

	// No SPIR-V for any supported stage is an ordinary state while a shader is
	// being edited, so it is neither cached nor reported here - Record() logs
	// the resulting miss, latched.
	if (!*pipeline.pipeline) return false;

	pipelines.UsePipeline(shaderId, std::move(pipeline), version);
	p_reportedMissingPipelines.erase(shaderId);
	NEURUS_LOG("[GeometryPass] Warmed per-mesh pipeline for shaderId="
	           << shaderId << " version=" << version);
	return true;
}

// ---------------------------------------------------------------------------
// Record
// ---------------------------------------------------------------------------

PassStats GeometryPass::Record(vk::CommandBuffer cmdBuf, const PassFrame& frame)
{
	PassStats stats{};

	const RenderFrameSnapshot& snapshot = frame.ctx.Frame();

	const vk::Extent2D renderExtent{frame.ctx.width, frame.ctx.height};

	Image* gPosition          = frame.resources.Output("gPosition");
	Image* gNormal            = frame.resources.Output("gNormal");
	Image* gAlbedo            = frame.resources.Output("gAlbedo");
	Image* gMetallicRoughness = frame.resources.Output("gMetallicRoughness");
	Image* gObjectID          = frame.resources.Output("gObjectID");
	Image* depth              = frame.resources.Output("depth");
	if (!gPosition || !gNormal || !gAlbedo || !gMetallicRoughness || !gObjectID || !depth)
	{
		NEURUS_ERR("[GeometryPass] graph did not provide the G-Buffer output set");
		return stats;
	}

	// The graph writes the camera UBO into set 0 before this call. Bailing out
	// when no frame has published a camera keeps a draw from reading an
	// unwritten buffer.
	const CameraGPU& camera = frame.cache.GetCameraGPU();
	if (!camera.IsValid())
	{
		return stats;
	}

	const std::array<Image*, 5> gBufferColorAttachments = {
		gPosition,
		gNormal,
		gAlbedo,
		gMetallicRoughness,
		gObjectID,
	};

	std::vector<vk::ImageView> colorViews;
	colorViews.reserve(5);
	for (Image* attachment : gBufferColorAttachments)
	{
		Barrier::Transition(cmdBuf, *attachment, ImageState::ColorAttachment);
		colorViews.push_back(*attachment->ImageViewHandle());
	}

	Barrier::Transition(cmdBuf, *depth, ImageState::DepthAttachment);
	vk::ImageView depthView = *depth->ImageViewHandle();

	const auto clearValues = Pass::PresetClearValues(Pass::PassType::G_BUFFER);

	BeginPass(cmdBuf, colorViews, &depthView, clearValues, renderExtent);

	const vk::Viewport viewport(0.0f, 0.0f,
	                            static_cast<float>(renderExtent.width),
	                            static_cast<float>(renderExtent.height),
	                            0.0f, 1.0f);
	cmdBuf.setViewport(0, viewport);

	const vk::Rect2D scissor({0, 0}, renderExtent);
	cmdBuf.setScissor(0, scissor);

	if (snapshot.HasScene())
	{
		for (const auto& inst : snapshot.meshes)
		{
			if (!inst.hasGeometry) continue;
			if (!inst.visibleInViewport || !inst.rendered) continue;

			const MeshGPU* gpuPtr = frame.cache.GetMeshGPU(inst.objectId);
			if (!gpuPtr)
			{
				NEURUS_ERR("[GeometryPass] GetMeshGPU returned nullptr for objectId=" << inst.objectId);
				continue;
			}

			bool usingCustomPipeline = false;
			vk::PipelineLayout activeLayout = *p_pipelines[0].pipelineLayout;
			if (inst.shaderObjectId != 0)
			{
				const int shaderId = inst.shaderObjectId;
				const int version  = inst.shaderVersion;

				// Lookup only: compiling reads the live Shader's SPIR-V, which is
				// the warm's job on the Editor thread. The id and version come
				// from the owned frame snapshot, so this read touches nothing the
				// Editor may be mutating. A miss means the shader has not
				// compiled, and the mesh draws with the pass default.
				Pipeline* customPipeline =
				    frame.cache.GetPipelineCache().GetPipeline(shaderId, version);
				if (!customPipeline)
				{
					if (p_reportedMissingPipelines.insert(shaderId).second)
					{
						NEURUS_LOG("[GeometryPass] No per-mesh pipeline for shaderId="
						           << shaderId << " version=" << version
						           << " (objectId=" << inst.objectId
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

			const glm::mat4 model = inst.model;
			const glm::mat4 normalMat = glm::mat4(inst.normalMatrix);

			MeshPushConstants pushConstants{model, normalMat,
			                               static_cast<uint32_t>(inst.objectId)};

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
	PassIO io;
	io.name = "GeometryPass";
	return io;
}

std::vector<const ShaderData*> GeometryPass::GetShaders() const
{
	return {p_shader.get()};
}

void GeometryPass::CollectGraphBuffers(RenderCache& cache, uint32_t frameIndex,
                                        std::vector<GraphBufferBinding>& out) const
{
	(void)frameIndex;
	const CameraGPU& camera = cache.GetCameraGPU();
	if (camera.IsValid())
	{
		out.push_back({0, camera.GetDescriptorInfo(),
		               vk::DescriptorType::eUniformBuffer});
	}
}

std::vector<ShaderPort> GeometryPass::DeclaredOutputs() const
{
	std::vector<ShaderPort> ports;
	for (const DeclaredPort& port : FindPassKind("GeometryPass")->declaredOutputs)
	{
		ports.push_back({std::string(port.name), PortDirection::Output,
		                 std::string(port.type), 0, 0});
	}
	return ports;
}

} // namespace neurus
