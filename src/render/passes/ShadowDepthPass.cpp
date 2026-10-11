/**
 * @file ShadowDepthPass.cpp
 * @brief Point-light shadow depth cubemap pass and sun-light orthographic shadow pass.
 *
 * Point lights render all 6 cubemap faces in one multiview pass: static face
 * view-projection matrices come from an SSBO, per-light and per-draw data from
 * push constants. Sun lights take a separate depth-only orthographic pass with
 * a single lightViewProj push constant.
 */

#include "passes/ShadowDepthPass.h"
#include "RenderContext.h"
#include "RenderCache.h"
#include "../resources/MeshGPU.h"
#include "../PipelineBuilder.h"
#include "render/Barrier.h"
#include "../shaders/ShaderLibrary.h"
#include "../shaders/RenderShader.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include "core/Log.h"

#include <glm/gtc/matrix_transform.hpp>

namespace neurus {

namespace {
	constexpr float kNearPlane = 0.1f;
	constexpr vk::Format kDepthFmt = vk::Format::eD32Sfloat;
} // anon

// ===========================================================================
// Static helpers - 6 cubemap face view-projection matrices from origin
// ===========================================================================

namespace {

/**
 * @brief Computes the 6 view-projection matrices for a cubemap centred on the
 *        origin.
 *
 * The vertex shader offsets each vertex by the light position, so one set of
 * matrices serves every point light.
 */
std::array<glm::mat4, 6> MakeFaceVPs(const glm::mat4& proj)
{
	// Six cubemap face directions (+X, -X, +Y, -Y, +Z, -Z) with matching up vectors
	return {{
		proj * glm::lookAt(glm::vec3(0.0f), glm::vec3( 1, 0, 0), glm::vec3( 0,-1, 0)),  // +X
		proj * glm::lookAt(glm::vec3(0.0f), glm::vec3(-1, 0, 0), glm::vec3( 0,-1, 0)),  // -X
		proj * glm::lookAt(glm::vec3(0.0f), glm::vec3( 0, 1, 0), glm::vec3( 0, 0, 1)),  // +Y
		proj * glm::lookAt(glm::vec3(0.0f), glm::vec3( 0,-1, 0), glm::vec3( 0, 0,-1)),  // -Y
		proj * glm::lookAt(glm::vec3(0.0f), glm::vec3( 0, 0, 1), glm::vec3( 0,-1, 0)),  // +Z
		proj * glm::lookAt(glm::vec3(0.0f), glm::vec3( 0, 0,-1), glm::vec3( 0,-1, 0)),  // -Z
	}};
}

} // anon

// ===========================================================================
// Descriptor set layout (SSBO for face VP matrices)
// ===========================================================================

DescriptorSetLayout ShadowDepthPass::CreateSSBOLayout(const vk::raii::Device& device)
{
	return BuildLayout()
		.AddBinding(0, vk::DescriptorType::eStorageBuffer,
		            vk::ShaderStageFlagBits::eVertex |
		                vk::ShaderStageFlagBits::eFragment)
		.Build(device);
}

// ===========================================================================
// Constructor
// ===========================================================================

ShadowDepthPass::ShadowDepthPass(const vk::raii::Device& device,
                                    const vk::raii::PhysicalDevice& physicalDevice,
                                    vk::Queue graphicsQueue,
                                    uint32_t queueFamilyIndex,
                                    uint32_t resolution)
	: Pass()
	, p_resolution(resolution)
	, p_queue(graphicsQueue)
	, p_queueFamilyIndex(queueFamilyIndex)
	// --- Self-load shaders via ShaderLibrary ---
	, p_multiviewShader(
		ShaderLibrary::LoadRenderShader("ShadowDepthMultiview",
		                                 NEURUS_SHADER_DIR "render/shadow_depth_multiview.vert",
		                                 NEURUS_SHADER_DIR "render/shadow_depth.frag"))
	, p_sunShader(
		ShaderLibrary::LoadRenderShader("ShadowDepthSun",
		                                 NEURUS_SHADER_DIR "render/sun_shadow_depth.vert",
		                                 NEURUS_SHADER_DIR "render/sun_shadow_depth.frag"))
{
	p_device = &device;
	p_physicalDevice = &physicalDevice;

	p_vtxLayout.AddAttribute(0, vk::Format::eR32G32B32Sfloat, 0);
	p_vtxLayout.AddAttribute(1, vk::Format::eR32G32B32Sfloat, 12);
	p_vtxLayout.AddAttribute(2, vk::Format::eR32G32Sfloat, 24);

	createSSBOResources(device, physicalDevice, graphicsQueue, queueFamilyIndex);
	BuildPipeline(device, "ShadowDepthPass");

		NEURUS_LOG("[ShadowDepthPass] resolution=" << resolution
	           << " faceVPSize=" << kFaceVPSize
	           << " staticFarPlane=" << kStaticFarPlane
	           << " multiviewShader=" << (p_multiviewShader ? "OK" : "FAIL")
	           << " sunShader=" << (p_sunShader ? "OK" : "FAIL"));
}

// ===========================================================================
// createSSBOResources - SSBO, descriptor pool & set
// ===========================================================================

void ShadowDepthPass::createSSBOResources(const vk::raii::Device& device,
                                           const vk::raii::PhysicalDevice& physicalDevice,
                                           vk::Queue queue, uint32_t qfi)
{
	// --- Compute static face VP matrices once ---
	const glm::mat4 proj = glm::perspective(glm::radians(90.0f), 1.0f,
	                                        kNearPlane, kStaticFarPlane);
	const auto faceVPs = MakeFaceVPs(proj);

	// --- Upload to device-local storage buffer ---
	p_faceVPs = std::make_unique<GPUBuffer>(
		device, physicalDevice, queue, qfi,
		kFaceVPSize,
		vk::BufferUsageFlagBits::eStorageBuffer,
		"ShadowDepthFaceVPs");
	p_faceVPs->Upload(faceVPs.data(), kFaceVPSize);

	// --- Descriptor layout, pool, and set ---
	p_ssboLayout = CreateSSBOLayout(device);
	p_ssboPool = DescriptorPool(device, 1,
	                            DescriptorPool::CalculatePoolSizes({&p_ssboLayout}, 1));
	p_ssboSet = std::make_unique<DescriptorSet>(
		std::move(p_ssboPool.Allocate(p_ssboLayout, 1).front()));
	p_ssboSet->WriteBuffer(0, p_faceVPs->GetDescriptorInfo(),
	                       vk::DescriptorType::eStorageBuffer);
#ifdef _DEBUG
	p_ssboSet->SetDebugName("ShadowDepth_FaceVPSSBO");
#endif

	NEURUS_LOG("[ShadowDepthPass] SSBO with 6 faceVP matrices uploaded");
}

// ===========================================================================
// createPipeline - multiview colour+depth pipeline (all 6 faces in single pass)
// ===========================================================================

void ShadowDepthPass::BuildPipeline(const vk::raii::Device& device,
                                     const std::string& debugName)
{
	// --- Multiview cubemap depth pipeline ---
	{
		if (!p_multiviewShader)
		{
			throw std::runtime_error("ShadowDepthPass: Multiview shader not loaded or invalid");
		}

		auto vertSpv = ShaderLibrary::Compile(p_multiviewShader->GetStage(ShaderType::VERTEX),
		                                      ShaderType::VERTEX, debugName + "_multiview_vert");
		auto fragSpv = ShaderLibrary::Compile(p_multiviewShader->GetStage(ShaderType::FRAGMENT),
		                                      ShaderType::FRAGMENT, debugName + "_multiview_frag");
		vk::ShaderModuleCreateInfo vertSmCI({}, vertSpv);
		vk::raii::ShaderModule vertModule(device, vertSmCI);
		vk::ShaderModuleCreateInfo fragSmCI({}, fragSpv);
		vk::raii::ShaderModule fragModule(device, fragSmCI);
		vk::PipelineShaderStageCreateInfo vertStageCI({}, vk::ShaderStageFlagBits::eVertex, *vertModule, "main");
		vk::PipelineShaderStageCreateInfo fragStageCI({}, vk::ShaderStageFlagBits::eFragment, *fragModule, "main");

		std::vector<vk::PushConstantRange> pushRanges = {
			vk::PushConstantRange(vk::ShaderStageFlagBits::eVertex |
			                      vk::ShaderStageFlagBits::eFragment,
			                      0, kTotalPushSize)
		};

		std::vector<vk::DescriptorSetLayout> dslayouts = { *p_ssboLayout.layout() };

		PipelineBuilder builder;
		p_pipelines.push_back(
			builder
				.SetDebugName((debugName + "::Multiview").c_str())
				.AddShaderStage(vertStageCI)
				.AddShaderStage(fragStageCI)
				.SetVertexInput(p_vtxLayout)
				.SetInputAssembly(vk::PrimitiveTopology::eTriangleList)
				.SetViewMask(0x3f)  // 6 faces of cubemap
				.SetRasterization(vk::PolygonMode::eFill,
				                  vk::CullModeFlagBits::eNone,
				                  vk::FrontFace::eClockwise)
				.SetMultisampling()
				.SetDepthStencil(true, true, vk::CompareOp::eLessOrEqual)
				.AddColorBlendAttachment(vk::PipelineColorBlendAttachmentState(
					VK_FALSE,
					vk::BlendFactor::eOne, vk::BlendFactor::eZero,
					vk::BlendOp::eAdd,
					vk::BlendFactor::eOne, vk::BlendFactor::eZero,
					vk::BlendOp::eAdd,
					vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
					vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA))
				.SetColorFormats({vk::Format::eR32G32B32A32Sfloat})
				.SetDepthFormat(kDepthFmt)
				.SetDescriptorSetLayouts(dslayouts)
				.SetPushConstantRanges(pushRanges)
				.BuildGraphicsPipeline(device));
	}

	// --- Sun orthographic depth-only pipeline ---
	{
		if (!p_sunShader)
		{
			throw std::runtime_error("ShadowDepthPass: Sun shader not loaded or invalid");
		}

		auto vertSpv = ShaderLibrary::Compile(p_sunShader->GetStage(ShaderType::VERTEX),
		                                      ShaderType::VERTEX, debugName + "_sun_vert");
		auto fragSpv = ShaderLibrary::Compile(p_sunShader->GetStage(ShaderType::FRAGMENT),
		                                      ShaderType::FRAGMENT, debugName + "_sun_frag");
		vk::ShaderModuleCreateInfo vertSmCI({}, vertSpv);
		vk::raii::ShaderModule vertModule(device, vertSmCI);
		vk::ShaderModuleCreateInfo fragSmCI({}, fragSpv);
		vk::raii::ShaderModule fragModule(device, fragSmCI);
		vk::PipelineShaderStageCreateInfo vertStageCI({}, vk::ShaderStageFlagBits::eVertex, *vertModule, "main");
		vk::PipelineShaderStageCreateInfo fragStageCI({}, vk::ShaderStageFlagBits::eFragment, *fragModule, "main");

		std::vector<vk::PushConstantRange> pushRanges = {
			vk::PushConstantRange(vk::ShaderStageFlagBits::eVertex,
			                      0, sizeof(glm::mat4))
		};

		PipelineBuilder builder;
		p_pipelines.push_back(
			builder
				.SetDebugName((debugName + "::Sun").c_str())
				.AddShaderStage(vertStageCI)
				.AddShaderStage(fragStageCI)
				.SetVertexInput(p_vtxLayout)
				.SetInputAssembly(vk::PrimitiveTopology::eTriangleList)
				.SetRasterization(vk::PolygonMode::eFill,
				                  vk::CullModeFlagBits::eNone,
				                  vk::FrontFace::eClockwise)
				.SetMultisampling()
				.SetDepthStencil(true, true, vk::CompareOp::eLessOrEqual)
				.SetDepthFormat(kDepthFmt)
				.SetPushConstantRanges(pushRanges)
				.BuildGraphicsPipeline(device));
	}
}

// ===========================================================================
// Record
// ===========================================================================

PassStats ShadowDepthPass::Record(vk::CommandBuffer cmdBuf, RenderCache& cache, const RenderContext& ctx)
{
	PassStats stats{};

	// A frame recorded before a scene exists is legal.
	if (!ctx.snapshot.HasScene()) { NEURUS_LOG("[ShadowDepthPass] No scene, skipping"); return stats; }

	const Scene* scene = ctx.snapshot.scene;

	const vk::Viewport viewport(0.f, 0.f,
	                            static_cast<float>(p_resolution),
	                            static_cast<float>(p_resolution),
	                            0.f, 1.f);
	const vk::Rect2D scissor({0, 0}, {p_resolution, p_resolution});

	for (const auto& [uid, lightPtr] : scene->light_list)
	{
		// Skip lights that do not cast shadows or are invisible.
		if (!lightPtr || !lightPtr->use_shadow) continue;
		if (!lightPtr->is_viewport || !lightPtr->is_rendered) continue;

		if (lightPtr->light_type != LightType::POINTLIGHT &&
		    lightPtr->light_type != LightType::SPOTLIGHT) continue;

		const glm::vec3 lightPos = lightPtr->GetPosition();
		const float farPlane = (lightPtr->light_type == LightType::SPOTLIGHT)
			? Light::spot_shadow_far
			: Light::point_shadow_far;

		// --- Per-light data: lightWorldPos + farPlane at offset 0 ---
		{
			LightPushData lp = {};
			lp.lpx = lightPos.x;
			lp.lpy = lightPos.y;
			lp.lpz = lightPos.z;
			lp.farPlane = farPlane;

			cmdBuf.pushConstants<LightPushData>(*p_pipelines[0].pipelineLayout,
			    vk::ShaderStageFlagBits::eVertex |
			    vk::ShaderStageFlagBits::eFragment,
			    0, lp);
		}

		cmdBuf.setViewport(0, viewport);
		cmdBuf.setScissor(0, scissor);

		// Transition cubemap to depth attachment layout (all faces/layers)
		{
			LightGPU* lgpu = cache.GetLightGPU(uid);
			if (!lgpu || !lgpu->shadowDepthMap) continue;
			auto& cubemap = *lgpu->shadowDepthMap;
			Barrier::Transition(cmdBuf, cubemap, ImageState::DepthAttachment);
		}

		// --- Render all 6 faces in a single multiview pass ---
	cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *p_pipelines[0].pipeline);
	cmdBuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics,
	                          *p_pipelines[0].pipelineLayout, 0,
		                          vk::ArrayProxy<const vk::DescriptorSet>(p_ssboSet->handle()), {});

		// --- Transition colour cubemap to ColorAttachment ---
		{
			LightGPU* lgpu = cache.GetLightGPU(uid);
			if (!lgpu || !lgpu->shadowColorMap) continue;
			auto& colorCube = *lgpu->shadowColorMap;
			Barrier::Transition(cmdBuf, colorCube, ImageState::ColorAttachment);
		}

		{
			LightGPU* lgpu = cache.GetLightGPU(uid);
			vk::RenderingAttachmentInfo depthAtt(
				lgpu->shadowDepthMap->ArrayView(),
				vk::ImageLayout::eDepthStencilAttachmentOptimal,
				vk::ResolveModeFlagBits::eNone, nullptr,
				vk::ImageLayout::eUndefined,
				vk::AttachmentLoadOp::eClear,
				vk::AttachmentStoreOp::eStore,
				vk::ClearDepthStencilValue(1.0f, 0));

		// Colour attachment: the cache's colour cubemap
		vk::ImageView colorView = lgpu->shadowColorMap->ArrayView();

		vk::RenderingAttachmentInfo colorAtt(
			colorView,
			vk::ImageLayout::eColorAttachmentOptimal,
			vk::ResolveModeFlagBits::eNone, nullptr,
			vk::ImageLayout::eUndefined,
			vk::AttachmentLoadOp::eClear,
			vk::AttachmentStoreOp::eStore,
			vk::ClearColorValue(std::array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}));

		vk::RenderingInfo renderInfo(
			{}, {{0, 0}, {p_resolution, p_resolution}},
			1u, 0x3Fu, colorAtt, &depthAtt, nullptr);

		cmdBuf.beginRendering(renderInfo);
		}

		if (scene)
		{
			for (const auto& [id, mesh] : scene->mesh_list)
			{
				if (!mesh || !mesh->o_mesh) continue;
				if (!mesh->is_viewport || !mesh->is_rendered) continue;


				MeshGPU* gpuPtr = cache.GetMeshGPU(mesh->GetObjectID());
				if (!gpuPtr)
				{
					NEURUS_ERR("[ShadowDepthPass] GetMeshGPU returned nullptr for objectId=" << mesh->GetObjectID());
					continue;
				}

				const glm::mat4 model = mesh->GetModelMatrix();

				// Push model matrix at offset 16 (light data at offset 0
				// persists from the per-light push above).
				cmdBuf.pushConstants<glm::mat4>(*p_pipelines[0].pipelineLayout,
				    vk::ShaderStageFlagBits::eVertex |
				    vk::ShaderStageFlagBits::eFragment,
				    kModelPushOffset, model);
				cmdBuf.bindVertexBuffers(0, gpuPtr->vertexBuffer->buffer(), {vk::DeviceSize{0}});
				cmdBuf.bindIndexBuffer(gpuPtr->indexBuffer->buffer(), 0, vk::IndexType::eUint32);
				++stats.drawCalls;
				cmdBuf.drawIndexed(gpuPtr->indexCount, 1, 0, 0, 0);
			}
		}

		cmdBuf.endRendering();

		// Return colour cubemap to ColorShaderRead for later sampling.
		{
			LightGPU* lgpu = cache.GetLightGPU(uid);
			if (lgpu && lgpu->shadowColorMap)
			{
				auto& colorCube = *lgpu->shadowColorMap;
				Barrier::Transition(cmdBuf, colorCube, ImageState::ColorShaderRead);
			}
		}

	}

	// =========================================================================
	// Sun light pass - orthographic depth-only (non-multiview)
	// =========================================================================

	{
		const vk::Viewport sunViewport(0.f, 0.f,
		                               static_cast<float>(kSunResolution),
		                               static_cast<float>(kSunResolution),
		                               0.f, 1.f);
		const vk::Rect2D sunScissor({0, 0}, {kSunResolution, kSunResolution});

		// The camera comes from the context, not the Scene: the editor injects
		// the frame's camera, and the sun shadow must centre on that one or the
		// shadow slides off the visible part of the world. Nothing follows this
		// block, so returning here skips only the sun path - the point-light
		// work above is already recorded.
		const Camera* activeCam = ctx.snapshot.camera;
		if (!activeCam)
		{
			NEURUS_LOG("[ShadowDepthPass] No camera in SceneContext, skipping sun path");
			return stats;
		}
		const glm::vec3 center = activeCam->cam_tar;

		const float field = Light::sun_shadow_field;
		const float nearPlane = Light::sun_shadow_near;
		const float farPlane = Light::sun_shadow_far;
		const glm::mat4 orthoProj = glm::ortho(-field, field, -field, field,
		                                       nearPlane, farPlane);

		constexpr glm::vec3 kWorldUp(0.0f, 0.0f, 1.0f);
		constexpr glm::vec3 kAltUp(1.0f, 0.0f, 0.0f);

		for (const auto& [uid, lightPtr] : scene->light_list)
		{
			if (!lightPtr) continue;
			if (lightPtr->light_type != LightType::SUNLIGHT) continue;
			if (!lightPtr->use_shadow) continue;
			if (!lightPtr->is_viewport || !lightPtr->is_rendered) continue;

			const glm::vec3 sunDir = glm::normalize(lightPtr->GetDirection());
			const glm::vec3 eye = center - sunDir * Light::sun_depth_range;

			// --- Degenerate up-vector check ---
			const glm::vec3 up = (glm::abs(glm::dot(sunDir, kWorldUp)) > 0.999f)
				? kAltUp : kWorldUp;

			const glm::mat4 lightView = glm::lookAt(eye, center, up);
			const glm::mat4 lightViewProj = orthoProj * lightView;

			cmdBuf.pushConstants<glm::mat4>(*p_pipelines[1].pipelineLayout,
			                                vk::ShaderStageFlagBits::eVertex,
			                                0, lightViewProj);

			LightGPU* slgpu = cache.GetLightGPU(uid);
			if (!slgpu || !slgpu->shadowDepthMap) continue;
			auto& sunImage = *slgpu->shadowDepthMap;
			Barrier::Transition(cmdBuf, sunImage, ImageState::DepthAttachment);

			// --- Depth attachment (2D, not cubemap) ---
			vk::RenderingAttachmentInfo depthAtt(
				sunImage.ImageViewHandle(),
				vk::ImageLayout::eDepthStencilAttachmentOptimal,
				vk::ResolveModeFlagBits::eNone, nullptr,
				vk::ImageLayout::eUndefined,
				vk::AttachmentLoadOp::eClear,
				vk::AttachmentStoreOp::eStore,
				vk::ClearDepthStencilValue(1.0f, 0));

			// Depth-only dynamic rendering (layerCount=1 required when viewMask=0)
			vk::RenderingInfo renderInfo(
				{}, {{0, 0}, {kSunResolution, kSunResolution}},
				1u, 0u, nullptr, &depthAtt, nullptr);

			cmdBuf.setViewport(0, sunViewport);
			cmdBuf.setScissor(0, sunScissor);
			cmdBuf.bindPipeline(vk::PipelineBindPoint::eGraphics, *p_pipelines[1].pipeline);

			cmdBuf.beginRendering(renderInfo);

			if (scene)
			{
				for (const auto& [id, mesh] : scene->mesh_list)
				{
					if (!mesh || !mesh->o_mesh) continue;

					MeshGPU* gpuPtr = cache.GetMeshGPU(mesh->GetObjectID());
					if (!gpuPtr || !gpuPtr->vertexBuffer || !gpuPtr->indexBuffer) continue;

					const glm::mat4 model = mesh->GetModelMatrix();
					const glm::mat4 mvp = lightViewProj * model;

					cmdBuf.pushConstants<glm::mat4>(*p_pipelines[1].pipelineLayout,
					    vk::ShaderStageFlagBits::eVertex,
					    0, mvp);

					cmdBuf.bindVertexBuffers(0, gpuPtr->vertexBuffer->buffer(), {vk::DeviceSize{0}});
					cmdBuf.bindIndexBuffer(gpuPtr->indexBuffer->buffer(), 0, vk::IndexType::eUint32);
					++stats.drawCalls;
					cmdBuf.drawIndexed(gpuPtr->indexCount, 1, 0, 0, 0);
				}
			}

			cmdBuf.endRendering();

			Barrier::Transition(cmdBuf, sunImage, ImageState::DepthShaderRead);
		}
	}

	return stats;
}

PassIO ShadowDepthPass::GetIO() const
{
	// The per-light depth maps are addressed internally; the graph models the
	// whole set as one ShadowDepthBundle write token. No image reads.
	PassIO io;
	io.name   = "ShadowDepthPass";
	io.writes = { { AttachmentName::ShadowDepth } };
	return io;
}

} // namespace neurus
