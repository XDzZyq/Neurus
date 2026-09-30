/**
 * @file TestVulkanShared.cpp
 * @brief Implementation of the shared GPU test fixture base class.
 */

#include "TestVulkanShared.h"

#include "render/Image.h"
#include "render/Barrier.h"
#include "render/shaders/ShaderLibrary.h"

#include <iostream>

using namespace neurus;

// ===========================================================================
// SetUp — standard Vulkan bootstrap
// ===========================================================================

void VulkanTestShared::SetUp()
{
	try
	{
		// --- Instance ---
		vk::ApplicationInfo appInfo("NeurusTest",
		                            VK_MAKE_VERSION(0, 5, 0),
		                            "NeurusTest",
		                            VK_MAKE_VERSION(0, 5, 0),
		                            VK_API_VERSION_1_4);
		std::vector<const char*> instanceExts = {
			VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _DEBUG
			VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#endif
		};

		// Add platform-specific extensions via PlatformSurface
		m_platform = neurus::CreatePlatformSurface();
		auto platformExts = m_platform->requiredInstanceExtensions();
		for (const auto* ext : platformExts) {
			// Skip VK_KHR_SURFACE (already added above)
			if (std::strcmp(ext, VK_KHR_SURFACE_EXTENSION_NAME) != 0)
				instanceExts.push_back(ext);
		}

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

		// --- Device ---
		float prio = 1.0f;
		vk::DeviceQueueCreateInfo qCI({}, m_graphicsQueueFamily, 1, &prio);

		// Enable multiview for shadow cubemap rendering tests
		// (single-pass 6-face cubemap via VK_KHR_multiview).
		vk::PhysicalDeviceMultiviewFeatures multiviewFeature;
		multiviewFeature.multiview = VK_TRUE;
		vk::PhysicalDeviceDynamicRenderingFeatures dynRendering;
		dynRendering.dynamicRendering = VK_TRUE;
		dynRendering.pNext = &multiviewFeature;

		// Mirrors VulkanContext: `discard` compiles to OpDemoteToHelperInvocation
		// under SPIR-V 1.6, which needs this bit (see VulkanContext::InitDevice).
		vk::PhysicalDeviceShaderDemoteToHelperInvocationFeatures demote;
		demote.shaderDemoteToHelperInvocation = VK_TRUE;
		demote.pNext = &dynRendering;

		vk::PhysicalDeviceSynchronization2Features sync2;
		sync2.synchronization2 = VK_TRUE;
		sync2.pNext = &demote;
		vk::PhysicalDeviceDescriptorIndexingFeatures descriptorIndexing;
		descriptorIndexing.descriptorBindingPartiallyBound = VK_TRUE;
		descriptorIndexing.pNext = &sync2;

		// Mirror VulkanContext::selectOptionalFeatures so DebugPass's
		// wireframe (VK_POLYGON_MODE_LINE) and point-sprite pipelines are
		// legal in GPU tests too. Gated on device support: requesting an
		// unsupported feature fails device creation outright.
		const vk::PhysicalDeviceFeatures supported = pd.getFeatures();
		vk::PhysicalDeviceFeatures features;
		features.fillModeNonSolid = supported.fillModeNonSolid;
		features.largePoints      = supported.largePoints;

		vk::DeviceCreateInfo devCI({}, qCI, {}, {}, &features, &descriptorIndexing);
		m_device = std::make_unique<vk::raii::Device>(pd, devCI);
		m_queue = m_device->getQueue(m_graphicsQueueFamily, 0);

		// --- Command pool ---
		vk::CommandPoolCreateInfo poolCI(
			vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
			m_graphicsQueueFamily);
		m_commandPool = std::make_unique<vk::raii::CommandPool>(*m_device, poolCI);

		// --- Command buffers ---
		vk::CommandBufferAllocateInfo allocInfo(
			*m_commandPool, vk::CommandBufferLevel::ePrimary, 1);
		m_commandBuffers = vk::raii::CommandBuffers(*m_device, allocInfo);

		// --- Reusable upload manager for tests (graphics + transfer share the same queue) ---
		m_uploadManager = std::make_unique<UploadManager>(
			*m_device, PhysicalDevice(), m_queue, m_graphicsQueueFamily);

		m_hasVulkan = true;
	}
	catch (const std::exception& e)
	{
		std::cerr << "[VulkanTestShared::SetUp] " << e.what() << std::endl;
		m_hasVulkan = false;
	}
	catch (...)
	{
		m_hasVulkan = false;
	}
}

// ===========================================================================
// TearDown
// ===========================================================================

void VulkanTestShared::TearDown()
{
	if (m_device)
	{
		m_device->waitIdle();
	}
	// ShaderLibrary no longer caches shaders — no explicit cleanup needed.
	// Each pass owns unique_ptr<Shader>; vk::raii::ShaderModule objects are
	// temporary and destroyed before BuildPipeline returns.
}

// ===========================================================================
// Command buffer helpers
// ===========================================================================

vk::raii::CommandBuffer& VulkanTestShared::BeginCmd()
{
	auto& cmd = m_commandBuffers[0];
	cmd.begin(
		vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
	return cmd;
}

void VulkanTestShared::EndSubmitWait(vk::raii::CommandBuffer& cmd)
{
	cmd.end();
	vk::SubmitInfo submitInfo({}, {}, {}, 1, &(*cmd));
	m_queue.submit(submitInfo, nullptr);
	m_device->waitIdle();
}

// ===========================================================================
// Asset path resolution
// ===========================================================================

std::string VulkanTestShared::ResolveAssetPath(const char* assetRelative)
{
	// res/ is copied to CMAKE_BINARY_DIR/res by a POST_BUILD step on neurus_test.
	// CTest runs from CMAKE_BINARY_DIR, so assets resolve directly.
	std::ifstream f(assetRelative);
	if (f.good()) return assetRelative;
	return assetRelative;  // return the path; caller handles missing files
}

// ===========================================================================
// HalfToFloat — IEEE 754 half-float conversion
// ===========================================================================

float VulkanTestShared::HalfToFloat(uint16_t half)
{
	const uint32_t h = half;
	const uint32_t sign     = (h >> 15) & 0x0001;
	const uint32_t exp      = (h >> 10) & 0x001F;
	const uint32_t mantissa =  h        & 0x03FF;

	uint32_t f32;

	if (exp == 0)
	{
		if (mantissa == 0)
		{
			f32 = sign << 31;
		}
		else
		{
			int e = -14;
			uint32_t m = mantissa;
			while ((m & 0x0400) == 0)
			{
				m <<= 1;
				--e;
			}
			m &= 0x03FF;
			f32 = (sign << 31) | ((uint32_t)(e + 127) << 23) | (m << 13);
		}
	}
	else if (exp == 0x1F)
	{
		f32 = (sign << 31) | (0xFFu << 23) | (mantissa << 13);
	}
	else
	{
		f32 = (sign << 31) | ((uint32_t)(exp + 112) << 23) | (mantissa << 13);
	}

	float result;
	std::memcpy(&result, &f32, sizeof(float));
	return result;
}

// ===========================================================================
// ComputeCameraUBO
// ===========================================================================

CameraUBOData VulkanTestShared::ComputeCameraUBO(Camera& cam)
{
	CameraUBOData ubo;
	ubo.view = cam.GetViewMatrix();
	ubo.viewProj = cam.GetProjectionMatrix() * ubo.view;
	return ubo;
}

// ===========================================================================
// MakeTestCamera
// ===========================================================================

CameraUBOData VulkanTestShared::MakeTestCamera(uint32_t width, uint32_t height)
{
	auto cam = CreateTestCamera(width, height);
	return ComputeCameraUBO(*cam);
}

// ===========================================================================
// CreateTestCamera
// ===========================================================================

std::shared_ptr<Camera> VulkanTestShared::CreateTestCamera(uint32_t width, uint32_t height)
{
	auto cam = std::make_shared<Camera>(
		static_cast<float>(width), static_cast<float>(height), 60.0f, 0.1f, 100.0f);
	cam->SetPosition(glm::vec3(0.0f, -2.0f, 0.0f));
	cam->SetTarPos(glm::vec3(0.0f, 0.0f, 0.0f));
	return cam;
}

// ===========================================================================
// TestTriangle
// ===========================================================================

std::pair<std::vector<TestVertex>, std::vector<uint32_t>> VulkanTestShared::TestTriangle()
{
	std::vector<TestVertex> verts = {
		//  posX  posY posZ    nrmX nrmY nrmZ    uvX  uvY
		{   0.0f, 0.0f,-0.5f,  0.0f, 1.0f, 0.0f, 0.5f, 1.0f },
		{   0.5f, 0.0f, 0.5f,  0.0f, 1.0f, 0.0f, 1.0f, 0.0f },
		{  -0.5f, 0.0f, 0.5f,  0.0f, 1.0f, 0.0f, 0.0f, 0.0f },
	};
	std::vector<uint32_t> indices = { 0, 1, 2 };
	return { verts, indices };
}

// ===========================================================================
// ReadbackHdrOutput
// ===========================================================================

std::vector<float> VulkanTestShared::ReadbackHdrOutput(
	const vk::raii::Device& device,
	const vk::raii::PhysicalDevice& pd,
	vk::Queue queue,
	uint32_t qfi,
	RenderCache& am,
	uint32_t renderWidth,
	uint32_t renderHeight)
{
	auto& hdrColor = am.GetAttachment(AttachmentName::HDRColor,
	                                  vk::Extent2D{renderWidth, renderHeight});

	auto imgData = hdrColor.ReadImageData(device, pd, queue, qfi);
	if (!imgData) return {};

	// Convert half-float → float
	const uint32_t pixelCount = renderWidth * renderHeight;
	std::vector<float> result(pixelCount * 4);
	const auto* src = reinterpret_cast<const uint16_t*>(imgData->GetPixelData().data());
	for (size_t i = 0; i < pixelCount * 4; ++i)
	{
		result[i] = HalfToFloat(src[i]);
	}

	return result;
}
