/**
 * @file test_scene_wiring.cpp
 * @brief TDD tests for Scene-driven rendering — DeferredRenderer::DrawFrame(const Scene&).
 *
 * Validates:
 *   - DrawFrame with empty scene does not crash
 *   - DrawFrame with camera but no meshes does not crash
 *   - DrawFrame with camera + mesh completes a full frame
 *
 * @note Requires a Vulkan 1.4-capable GPU with presentation support.
 *       Skipped in CI without GPU or on platforms without window surface support.
 */

#include <gtest/gtest.h>

#include "shared/TestVulkanShared.h"

#include "asset/data/MeshData.h"
#include "render/DeferredRenderer.h"
#include "scene/Camera.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "scene/Scene.h"

#include <vulkan/vulkan_raii.hpp>

#include "platform/PlatformSurface.h"

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include <memory>
#include <string>
#include <vector>
#include <cstring>

using namespace neurus;

// ---------------------------------------------------------------------------
// Test OBJ string: simple 3-vertex triangle
// ---------------------------------------------------------------------------

static const char* kTriangleObj =
	"# Simple triangle with normals and UVs\n"
	"v 0.0 0.5 0.0\n"
	"v -0.5 -0.5 0.0\n"
	"v 0.5 -0.5 0.0\n"
	"vn 0.0 0.0 1.0\n"
	"vt 0.5 1.0\n"
	"vt 0.0 0.0\n"
	"vt 1.0 0.0\n"
	"f 1/1/1 2/2/1 3/3/1\n";

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

/**
 * @brief GPU test fixture for Scene-driven DeferredRenderer rendering.
 *
 * Creates a headless Vulkan device and uses the PlatformSurface abstraction
 * for surface creation. On Windows, a hidden Win32 window is created for
 * the presentation surface. On macOS, headless surface creation is not
 * possible without a Cocoa event loop — those tests are skipped.
 */
class SceneWiringTest : public VulkanTestShared
{
protected:
	void SetUp() override
	{
		try
		{
			// --- Platform surface abstraction ---
			m_platform = CreatePlatformSurface();

			// --- Instance ---
			vk::ApplicationInfo appInfo("NeurusTest_SceneWiring",
			                            VK_MAKE_VERSION(0, 4, 5),
			                            "NeurusTest_SceneWiring",
			                            VK_MAKE_VERSION(0, 4, 5),
			                            VK_API_VERSION_1_4);

			std::vector<const char*> instanceExts = m_platform->requiredInstanceExtensions();
#ifdef _DEBUG
			instanceExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif

			vk::InstanceCreateInfo instanceCI(m_platform->instanceCreateFlags(), &appInfo, {}, instanceExts);
			m_instance = std::make_unique<vk::raii::Instance>(m_context, instanceCI);

			// --- Physical device ---
			m_physicalDevices = vk::raii::PhysicalDevices(*m_instance);
			if (m_physicalDevices.empty())
			{
				m_hasVulkan = false;
				return;
			}

			// Pick discrete GPU if available
			m_selectedPdIndex = 0;
			for (uint32_t i = 0; i < static_cast<uint32_t>(m_physicalDevices.size()); ++i)
			{
				const auto props = m_physicalDevices[i].getProperties();
				if (props.deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
				{
					m_selectedPdIndex = i;
					break;
				}
			}
			auto& pd = m_physicalDevices[m_selectedPdIndex];

			// --- Queue family ---
			auto qfProps = pd.getQueueFamilyProperties();
			m_graphicsQueueFamily = UINT32_MAX;
			for (uint32_t i = 0; i < static_cast<uint32_t>(qfProps.size()); ++i)
			{
				if (qfProps[i].queueFlags & vk::QueueFlagBits::eGraphics)
				{
					m_graphicsQueueFamily = i;
					break;
				}
			}
			if (m_graphicsQueueFamily == UINT32_MAX)
			{
				m_hasVulkan = false;
				return;
			}

			// --- Device (must enable VK_KHR_swapchain for DeferredRenderer) ---
			std::vector<const char*> devExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

			// VK_KHR_portability_subset is required when running on portability layers
			// (e.g. MoltenVK compatibility profile). Check if supported and add.
			for (const auto& ext : pd.enumerateDeviceExtensionProperties())
			{
				if (strcmp(ext.extensionName, "VK_KHR_portability_subset") == 0)
				{
					devExts.push_back("VK_KHR_portability_subset");
					break;
				}
			}

			float prio = 1.0f;
			vk::DeviceQueueCreateInfo qCI({}, m_graphicsQueueFamily, 1, &prio);

			// Mirror VulkanContext::InitDevice: DeferredRenderer's passes need
			// dynamic rendering, synchronization2, multiview (shadow cubemaps) and
			// shaderDemoteToHelperInvocation (`discard` under SPIR-V 1.6).
			vk::PhysicalDeviceMultiviewFeatures multiviewFeature;
			multiviewFeature.multiview = VK_TRUE;
			vk::PhysicalDeviceDynamicRenderingFeatures dynRendering;
			dynRendering.dynamicRendering = VK_TRUE;
			dynRendering.pNext = &multiviewFeature;
			vk::PhysicalDeviceShaderDemoteToHelperInvocationFeatures demote;
			demote.shaderDemoteToHelperInvocation = VK_TRUE;
			demote.pNext = &dynRendering;
			vk::PhysicalDeviceSynchronization2Features sync2;
			sync2.synchronization2 = VK_TRUE;
			sync2.pNext = &demote;
			vk::PhysicalDeviceDescriptorIndexingFeatures descriptorIndexing;
			descriptorIndexing.descriptorBindingPartiallyBound = VK_TRUE;
			descriptorIndexing.pNext = &sync2;

			const vk::PhysicalDeviceFeatures supported = pd.getFeatures();
			vk::PhysicalDeviceFeatures features;
			features.fillModeNonSolid = supported.fillModeNonSolid;
			features.largePoints      = supported.largePoints;

			vk::DeviceCreateInfo devCI({}, qCI, {}, devExts, &features, &descriptorIndexing);
			m_device = std::make_unique<vk::raii::Device>(pd, devCI);
			m_queue = m_device->getQueue(m_graphicsQueueFamily, 0);

			// --- Native window + surface ---
#ifdef _WIN32
			HINSTANCE hInst = GetModuleHandle(nullptr);
			WNDCLASSEX wc = {};
			wc.cbSize = sizeof(WNDCLASSEX);
			wc.lpfnWndProc = DefWindowProc;
			wc.hInstance = hInst;
			wc.lpszClassName = L"NeurusTestSceneWiring";
			RegisterClassEx(&wc);

			m_hwnd = CreateWindowEx(0, L"NeurusTestSceneWiring", L"Test",
			                        WS_POPUP, 0, 0,
			                        static_cast<int>(kRenderWidth),
			                        static_cast<int>(kRenderHeight),
			                        nullptr, nullptr, hInst, nullptr);
			if (!m_hwnd)
			{
				m_hasVulkan = false;
				return;
			}

			m_surface = std::make_unique<vk::raii::SurfaceKHR>(
				m_platform->createSurface(*m_instance, m_hwnd));
#else
			// macOS: headless surface creation requires a Cocoa event loop.
			// Without Qt / NSApplication running, we cannot create a hidden
			// NSWindow+NSView for the presentation surface.
			m_hasVulkan = false;
			GTEST_SKIP() << "SceneWiringTest requires a window surface (not available headless on this platform).";
#endif

			m_hasVulkan = true;
		}
		catch (...)
		{
			m_hasVulkan = false;
		}
	}

	void TearDown() override
	{
		// Destroy DeferredRenderer before device
		m_renderer.reset();
		m_surface.reset();

#ifdef _WIN32
		if (m_hwnd)
		{
			DestroyWindow(m_hwnd);
			m_hwnd = nullptr;
		}
#endif

		m_platform.reset();
		VulkanTestShared::TearDown();
	}

	void CreateRenderer(uint32_t width, uint32_t height)
	{
		m_renderer = std::make_unique<DeferredRenderer>(
			*m_device,
			m_physicalDevices[m_selectedPdIndex],
			m_queue,
			m_graphicsQueueFamily,
			*m_surface,
			width, height);
	}

	std::shared_ptr<Mesh> CreateAndUploadTriangleMesh()
	{
		auto md = std::make_shared<MeshData>();
		EXPECT_TRUE(md->LoadObjFromString(kTriangleObj));

		auto mesh = std::make_shared<Mesh>();
		mesh->o_mesh = md;
		return mesh;
	}

	std::shared_ptr<Camera> CreateDefaultCamera()
	{
		auto cam = std::make_shared<Camera>();
		cam->SetPosition(glm::vec3(0.0f, -5.0f, 2.0f));
		cam->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
		cam->ChangeCamRatio(static_cast<float>(kRenderWidth), static_cast<float>(kRenderHeight));
		return cam;
	}

	static constexpr uint32_t kRenderWidth = 800;
	static constexpr uint32_t kRenderHeight = 600;

	std::unique_ptr<PlatformSurface> m_platform;
	std::unique_ptr<vk::raii::SurfaceKHR> m_surface;
	std::unique_ptr<DeferredRenderer> m_renderer;

#ifdef _WIN32
	HWND m_hwnd = nullptr;
#endif
};

// ---------------------------------------------------------------------------
// 1. DrawFrame with scene containing only a camera — no crash
// ---------------------------------------------------------------------------

TEST_F(SceneWiringTest, DrawFrame_EmptyScene_NoCrash)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NO_FATAL_FAILURE(CreateRenderer(kRenderWidth, kRenderHeight));
	ASSERT_NE(m_renderer, nullptr);

	Scene scene;
	auto cam = CreateDefaultCamera();
	scene.UseCamera(cam);

	EXPECT_NO_THROW(m_renderer->DrawFrame(RenderContext{.editor = {.scene = &scene, .camera = scene.GetActiveCamera()}}));
	m_renderer->WaitIdle();
}

// ---------------------------------------------------------------------------
// 2. DrawFrame with camera but no meshes — no crash
// ---------------------------------------------------------------------------

TEST_F(SceneWiringTest, DrawFrame_SceneWithOnlyCamera_NoCrash)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NO_FATAL_FAILURE(CreateRenderer(kRenderWidth, kRenderHeight));

	Scene scene;
	auto cam = CreateDefaultCamera();
	scene.UseCamera(cam);

	ASSERT_NE(scene.GetActiveCamera(), nullptr);

	EXPECT_NO_THROW(m_renderer->DrawFrame(RenderContext{.editor = {.scene = &scene, .camera = scene.GetActiveCamera()}}));
	m_renderer->WaitIdle();
}

// ---------------------------------------------------------------------------
// 3. DrawFrame with camera + mesh — frame completes
// ---------------------------------------------------------------------------

TEST_F(SceneWiringTest, DrawFrame_SceneWithCameraAndMesh_RendersFrame)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NO_FATAL_FAILURE(CreateRenderer(kRenderWidth, kRenderHeight));

	Scene scene;

	auto cam = CreateDefaultCamera();
	scene.UseCamera(cam);

	auto mesh = CreateAndUploadTriangleMesh();
	ASSERT_NE(mesh, nullptr);
	scene.UseMesh(mesh);

	auto light = std::make_shared<Light>(LightType::POINTLIGHT, 30.0f, glm::vec3(1.0f));
	light->SetPosition(glm::vec3(2.0f, 2.0f, 5.0f));
	scene.UseLight(light);

	EXPECT_NO_THROW(m_renderer->DrawFrame(RenderContext{.editor = {.scene = &scene, .camera = scene.GetActiveCamera()}}));
	EXPECT_NO_THROW(m_renderer->DrawFrame(RenderContext{.editor = {.scene = &scene, .camera = scene.GetActiveCamera()}}));

	m_renderer->WaitIdle();
}

// ---------------------------------------------------------------------------
// 4. Profiling enabled: DrawFrame returns a populated per-frame profile;
//    toggling profiling off leaves the profile untouched (empty).
// ---------------------------------------------------------------------------

TEST_F(SceneWiringTest, ProfilingEnabled_PopulatesFrameProfile)
{
	if (!m_hasVulkan)
	{
		GTEST_SKIP() << "No Vulkan-capable GPU found.";
	}

	ASSERT_NO_FATAL_FAILURE(CreateRenderer(kRenderWidth, kRenderHeight));

	Scene scene;
	auto cam = CreateDefaultCamera();
	scene.UseCamera(cam);
	auto mesh = CreateAndUploadTriangleMesh();
	ASSERT_NE(mesh, nullptr);
	scene.UseMesh(mesh);
	auto light = std::make_shared<Light>(LightType::POINTLIGHT, 30.0f, glm::vec3(1.0f));
	light->SetPosition(glm::vec3(2.0f, 2.0f, 5.0f));
	scene.UseLight(light);

	// Profiling is always on; every frame produces per-pass CPU timers + counters.

	// Frame 1: per-pass CPU timers + counters are recorded; GPU timestamp
	// readback for this slot happens at the start of frame 2 (fence-based).
	{
		const FrameProfile& profile = m_renderer->DrawFrame(RenderContext{.editor = {.scene = &scene, .camera = scene.GetActiveCamera()}});
		EXPECT_GT(profile.passCount, 0u);
		EXPECT_FALSE(profile.passes.empty());
		for (const auto& pass : profile.passes)
		{
			EXPECT_FALSE(pass.name.empty());
			EXPECT_GE(pass.cpuMs, 0.0);
			EXPECT_GE(pass.drawCalls, 0u);
			EXPECT_GE(pass.dispatches, 0u);
		}
		EXPECT_GE(profile.cpuRecordMs, 0.0);
		EXPECT_GE(profile.drawCalls, 0u);
	}

	// Frame 2: exercises the GPU timestamp readback path for frame 1's slot.
	{
		const FrameProfile& profile = m_renderer->DrawFrame(RenderContext{.editor = {.scene = &scene, .camera = scene.GetActiveCamera()}});
		EXPECT_GT(profile.passCount, 0u);
	}

	// Frame 3: the slot recorded in frame 1 is read back. The readback is
	// guarded so pre-signaled, never-written slots are skipped on startup
	// (VUID-vkGetQueryPoolResults-None-09401).
	{
		const FrameProfile& profile = m_renderer->DrawFrame(RenderContext{.editor = {.scene = &scene, .camera = scene.GetActiveCamera()}});
		EXPECT_GT(profile.passCount, 0u);
		if (profile.gpuTimingAvailable)
		{
			EXPECT_TRUE(profile.gpuReady);
			EXPECT_GE(profile.gpuTotalMs, 0.0);
			for (const auto& pass : profile.passes)
				EXPECT_GE(pass.gpuMs, 0.0);
		}
	}

	m_renderer->WaitIdle();
}
