/**
 * @file TestVulkanShared.h
 * @brief Shared test fixture base class for GPU-dependent tests.
 *
 * Consolidates ~60 lines of duplicated Vulkan bootstrap (Instance → PhysicalDevice
 * → Queue → Device → CommandPool → CommandBuffers) from 6 test files into one
 * reusable base class, plus common helpers (BeginCmd, EndSubmitWait, ResolveAssetPath).
 */

#pragma once

#include <gtest/gtest.h>

#include <vulkan/vulkan_raii.hpp>

#include "platform/PlatformSurface.h"
#include "render/Barrier.h"
#include "render/RenderCache.h"
#include "render/UploadManager.h"
#include "render/passes/GeometryPass.h"
#include "scene/Camera.h"
#include "scene/EditorContext.h"
#include "scene/Scene.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

// ---------------------------------------------------------------------------
// Test vertex structure (matches BufferLayout: pos(3) + normal(3) + uv(2))
// ---------------------------------------------------------------------------

/**
 * @brief Simple vertex with position, normal, and UV coordinates.
 *
 * Layout: posX/Y/Z (3 float), nrmX/Y/Z (3 float), uvX/uvY (2 float) = 32 bytes.
 */
struct TestVertex
{
	float posX, posY, posZ;
	float nrmX, nrmY, nrmZ;
	float uvX,  uvY;
};

// ---------------------------------------------------------------------------
// Shared GPU test fixture — override SetUp/TearDown for non-standard setups
// ---------------------------------------------------------------------------

/**
 * @brief Base test fixture for GPU-dependent Neurus tests.
 *
 * SetUp creates:
 *   - vk::Instance (with surface + win32-surface extensions, debug-utils in Debug)
 *   - Enumerates PhysicalDevices, picks discrete GPU when available
 *   - Finds a graphics queue family
 *   - Creates a Device + Queue
 *   - Creates CommandPool + CommandBuffers (one-shot, reset-capable)
 *
 * TearDown calls m_device->waitIdle().
 *
 * Inheriting test fixtures SHOULD call VulkanTestShared::SetUp() in their
 * own SetUp when they want the standard bootstrap, and
 * VulkanTestShared::TearDown() in their TearDown.
 */
class VulkanTestShared : public ::testing::Test
{
protected:
	void SetUp() override;
	void TearDown() override;

	// --- Vulkan availability ---
	bool HasVulkan() const { return m_hasVulkan; }

	// --- Physical device accessor (most common operation) ---
	vk::raii::PhysicalDevice& PhysicalDevice()
	{
		return m_physicalDevices[m_selectedPdIndex];
	}

	// --- Command buffer helpers ---
	vk::raii::CommandBuffer& BeginCmd();
	void EndSubmitWait(vk::raii::CommandBuffer& cmd);

	// --- Asset path resolution ---
	/**
	 * @brief Resolve an asset path relative to the project root.
	 *
	 * CTest runs from build/debug/Debug/ (MSVC) or build/debug/ (single-config).
	 * This tries multiple relative paths, falling back to the first candidate.
	 */
	static std::string ResolveAssetPath(const char* assetRelative);

	// --- Shared test helpers ---

	/**
	 * @brief Computes CameraUBOData from a Camera object.
	 */
	static neurus::CameraUBOData ComputeCameraUBO(neurus::Camera& cam);

	/**
	 * @brief Creates a default camera UBO (60° FOV, looking at origin from (0,0,2)).
	 * @param width  Render target width (for aspect ratio).
	 * @param height Render target height (for aspect ratio).
	 */
	static neurus::CameraUBOData MakeTestCamera(uint32_t width, uint32_t height);

	/**
	 * @brief Creates a test Camera object with 60° FOV, positioned at (0,-2,0)
	 *        looking at origin. Ready to register via scene.UseCamera().
	 * @param width  Render target width (for aspect ratio).
	 * @param height Render target height (for aspect ratio).
	 * @return Non-null shared_ptr to a configured Camera.
	 */
	static std::shared_ptr<neurus::Camera> CreateTestCamera(uint32_t width, uint32_t height);

	/**
	 * @brief Creates a test triangle in the XY plane facing +Z.
	 * @return Pair of (vertices, indices).
	 */
	static std::pair<std::vector<TestVertex>, std::vector<uint32_t>> TestTriangle();

	/**
	 * @brief Reads back HDRColor RGBA16F attachment into float array.
	 *
	 * Transitions HDRColor from GENERAL to TRANSFER_SRC_OPTIMAL, copies to
	 * a staging buffer, converts half-floats to floats.
	 *
	 * @param device       Logical device.
	 * @param pd           Physical device (for memory queries).
	 * @param queue        Graphics queue.
	 * @param qfi          Queue family index.
	 * @param am           Attachment manager containing HDRColor attachment.
	 * @param renderWidth  Render target width.
	 * @param renderHeight Render target height.
	 * @return Vector of RGBA float values (pixelCount * 4).
	 */
	static std::vector<float> ReadbackHdrOutput(
		const vk::raii::Device& device,
		const vk::raii::PhysicalDevice& pd,
		vk::Queue queue,
		uint32_t qfi,
		neurus::RenderCache& am,
		uint32_t renderWidth,
		uint32_t renderHeight);

	// --- Shared utility helpers ---

	/**
	 * @brief Converts an IEEE 754 half-float (16-bit) to a 32-bit float.
	 */
	static float HalfToFloat(uint16_t half);

	/**
	 * @brief Find a memory type matching the given type bits and property flags.
	 * @param pd Physical device to query.
	 * @param typeBits Bitmask of allowed memory types.
	 * @param required Required memory property flags.
	 * @return Index of the first matching memory type.
	 * @throws std::runtime_error if no suitable memory type is found.
	 */
	static uint32_t FindMemoryType(const vk::raii::PhysicalDevice& pd,
	                               uint32_t typeBits,
	                               vk::MemoryPropertyFlags required)
	{
		auto memProps = pd.getMemoryProperties();
		for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
		{
			if ((typeBits & (1u << i)) &&
			    (memProps.memoryTypes[i].propertyFlags & required) == required)
			{
				return i;
			}
		}
		throw std::runtime_error("No suitable memory type found");
	}

	/**
	 * @brief Transition G-Buffer attachments to renderable layouts.
	 *
	 * Transitions Position, Normal, Albedo, MetallicRoughness to
	 * eColorAttachmentOptimal and Depth to eDepthStencilAttachmentOptimal.
	 *
	 * @param am      RenderCache owning the G-Buffer attachments.
	 * @param fixture Test fixture providing BeginCmd/EndSubmitWait.
	 */
	static void TransitionGbufferToColorAttachment(
		neurus::RenderCache& am,
		vk::Extent2D extent,
		VulkanTestShared& fixture)
	{
		auto& cmd = fixture.BeginCmd();

		const std::array<neurus::AttachmentName, 4> colorAtts = {
			neurus::AttachmentName::Position,
			neurus::AttachmentName::Normal,
			neurus::AttachmentName::Albedo,
			neurus::AttachmentName::MetallicRoughness,
		};

		for (const auto& att : colorAtts)
		{
			neurus::Barrier::Transition(*cmd,
				am.GetAttachment(att, extent),
				neurus::ImageState::ColorAttachment);
		}

		neurus::Barrier::Transition(*cmd,
			am.GetAttachment(neurus::AttachmentName::Depth, extent),
			neurus::ImageState::DepthAttachment);

		fixture.EndSubmitWait(cmd);
	}

	/**
	 * @brief Publishes the scene's active camera into the RenderCache's shared UBO.
	 *
	 * DeferredRenderer::recordFrame() does this once per frame, before any pass
	 * records; a test that drives a pass directly is standing in for that frame
	 * driver and must do the same. GeometryPass and DebugPass only *bind*
	 * RenderCache::GetCameraGPU() — without this call it is still invalid and
	 * both passes record nothing.
	 *
	 * No-op when the scene has no active camera.
	 */
	static void PublishSceneCamera(neurus::RenderCache& cache,
	                               const neurus::Scene& scene)
	{
		if (const neurus::Camera* cam = scene.GetActiveCamera())
		{
			cache.UpdateCamera(cam->GetProjectionMatrix(), cam->GetViewMatrix());
		}
	}

	/**
	 * @brief PublishSceneCamera overload taking the context a pass will receive.
	 *
	 * Convenience for the common shape `PublishSceneCamera(cache, ctx.editor)`:
	 * casts `editor.scene` back to `const Scene*` the way every pass does.
	 * No-op when no scene is attached.
	 */
	static void PublishSceneCamera(neurus::RenderCache& cache,
	                               const neurus::EditorContext& editor)
	{
		if (const auto* scene = static_cast<const neurus::Scene*>(editor.scene))
		{
			PublishSceneCamera(cache, *scene);
		}
	}

	/**
	 * @brief Pre-register all meshes in the scene into the RenderCache.
	 *
	 * Skips meshes already cached. Uses UploadManager for GPU-resident
	 * buffer construction.
	 */
	static void EnsureMeshesUploaded(
		neurus::RenderCache& cache,
		const neurus::Scene& scene,
		const vk::raii::Device& device,
		const vk::raii::PhysicalDevice& pd,
		vk::Queue queue,
		uint32_t qfi)
	{
		neurus::UploadManager upload(device, pd, queue, qfi);
		for (const auto& [id, mesh] : scene.mesh_list)
		{
			if (!mesh || !mesh->o_mesh) continue;
			if (cache.GetMeshGPU(id)) continue;
			auto meshGPU = upload.UploadMesh(*mesh);
			cache.UseMeshGPU(id, std::move(meshGPU));
		}
	}

	/**
	 * @brief Pre-register all shadow-casting lights in the scene into the RenderCache.
	 *
	 * Skips lights already cached or without shadow enabled. Uses
	 * UploadManager for shadow depth map construction.
	 */
	static void EnsureLightShadowsUploaded(
		neurus::RenderCache& cache,
		const neurus::Scene& scene,
		const vk::raii::Device& device,
		const vk::raii::PhysicalDevice& pd,
		vk::Queue queue,
		uint32_t qfi)
	{
		neurus::UploadManager upload(device, pd, queue, qfi);
		for (const auto& [uid, light] : scene.light_list)
		{
			if (!light || !light->use_shadow) continue;
			if (cache.GetLightGPU(uid)) continue;
			auto lightGPU = upload.UploadLight(*light);
			cache.UseLightGPU(uid, std::move(lightGPU));
		}
	}

	// --- Vulkan state (destructor order: reverse declaration) ---
	bool m_hasVulkan = false;
	std::unique_ptr<neurus::PlatformSurface> m_platform;
	vk::raii::Context m_context;
	std::unique_ptr<vk::raii::Instance> m_instance;
	vk::raii::PhysicalDevices m_physicalDevices = nullptr;
	uint32_t m_selectedPdIndex = 0;
	std::unique_ptr<vk::raii::Device> m_device;
	uint32_t m_graphicsQueueFamily = 0;
	vk::Queue m_queue = nullptr;
	std::unique_ptr<vk::raii::CommandPool> m_commandPool;
	vk::raii::CommandBuffers m_commandBuffers = nullptr;

	/// @brief Reusable upload manager for CPU→GPU struct conversion.
	std::unique_ptr<neurus::UploadManager> m_uploadManager;
};
