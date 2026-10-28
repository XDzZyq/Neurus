#include "VulkanContext.h"

#include "core/Log.h"
#include "platform/PlatformSurface.h"

#include <stdexcept>
#include <set>
#include <cstring>
#include <iostream>

namespace neurus {

#ifdef _DEBUG
	constexpr bool kEnableValidation = true;
#else
	constexpr bool kEnableValidation = false;
#endif

const std::vector<const char*> kValidationLayers = {
	"VK_LAYER_KHRONOS_validation",
};

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void*)
{
	if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
		std::cerr << "[Vulkan Validation] " << pCallbackData->pMessage << "\n";
	return VK_FALSE;
}


vk::raii::Instance VulkanContext::CreateInstance(const PlatformSurface& platform)
{
	vk::ApplicationInfo appInfo("Neurus", VK_MAKE_VERSION(0,1,0), "NeurusRenderer", VK_MAKE_VERSION(0,1,0), VK_API_VERSION_1_4);
	auto extensions = platform.requiredInstanceExtensions();
	if constexpr (kEnableValidation)
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	vk::InstanceCreateInfo createInfo(platform.instanceCreateFlags(), &appInfo, {}, extensions);

	if constexpr (kEnableValidation)
	{
		auto layers = vk::enumerateInstanceLayerProperties();
		bool ok = true;
		for (auto* name : kValidationLayers) {
			bool found = false;
			for (auto& l : layers) { if (!strcmp(name, l.layerName)) { found = true; break; } }
			if (!found) { ok = false; break; }
		}
		if (ok) {
			createInfo.setPEnabledLayerNames(kValidationLayers);
			vk::DebugUtilsMessengerCreateInfoEXT dbg({},
				vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning | vk::DebugUtilsMessageSeverityFlagBitsEXT::eError,
				vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation | vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance,
				debugCallback);
			createInfo.setPNext(&dbg);
		} else {
			std::cerr << "[Neurus] Validation layers not available.\n";
		}
	}

	vk::raii::Context context;
	return vk::raii::Instance(context, createInfo);
}

VulkanContext::VulkanContext(vk::raii::Instance&& instance)
{
	ctx_instance = std::make_unique<vk::raii::Instance>(std::move(instance));
}

/**
 * @brief Enables the optional core features this application uses, when supported.
 *
 * vkCreateDevice fails with VK_ERROR_FEATURE_NOT_PRESENT if a requested
 * feature is unavailable, so every field is gated on the device's own report.
 * Currently fillModeNonSolid (line polygon mode) and largePoints (point
 * sprites larger than one pixel) are requested. wideLines is deliberately
 * NOT requested - it is unsupported on some backends.
 */
static vk::PhysicalDeviceFeatures selectOptionalFeatures(const vk::raii::PhysicalDevice& pd)
{
	const vk::PhysicalDeviceFeatures supported = pd.getFeatures();

	vk::PhysicalDeviceFeatures enabled;
	enabled.fillModeNonSolid = supported.fillModeNonSolid;
	enabled.largePoints      = supported.largePoints;

	NEURUS_LOG("[VulkanContext] optional features: fillModeNonSolid="
	           << (enabled.fillModeNonSolid ? "on" : "OFF")
	           << ", largePoints=" << (enabled.largePoints ? "on" : "OFF"));

	return enabled;
}

void VulkanContext::InitDevice()
{
	ctx_physicalDevices = vk::raii::PhysicalDevices(*ctx_instance);
	ctx_selectedDeviceIndex = selectPhysicalDeviceIndex();
	auto& pd = ctx_physicalDevices[ctx_selectedDeviceIndex];
	auto props = pd.getProperties();
	ctx_gpuName = props.deviceName.data();

	NEURUS_LOG("[VulkanContext] " << ctx_gpuName
	           << " | limits.maxPushConstantsSize = "
	           << props.limits.maxPushConstantsSize);

	ctx_graphicsQueueFamily = findGraphicsQueueFamily();
	ctx_transferQueueFamily = findTransferQueueFamily();

	float graphicsPriority = 1.0f;
	std::vector<vk::DeviceQueueCreateInfo> queueCIs;
	queueCIs.push_back(vk::DeviceQueueCreateInfo({}, ctx_graphicsQueueFamily, 1, &graphicsPriority));

	if (ctx_transferQueueFamily != ctx_graphicsQueueFamily)
	{
		float transferPriority = 0.5f;
		queueCIs.push_back(vk::DeviceQueueCreateInfo({}, ctx_transferQueueFamily, 1, &transferPriority));
	}

	vk::PhysicalDeviceMultiviewFeatures multiviewFeature;
	multiviewFeature.multiview = VK_TRUE;
	vk::PhysicalDeviceDynamicRenderingFeatures dynRendering;
	dynRendering.dynamicRendering = VK_TRUE;
	dynRendering.pNext = &multiviewFeature;

	// Required by any fragment shader that uses `discard`: targeting Vulkan 1.3
	// (SPIR-V 1.6) lowers `discard` to OpDemoteToHelperInvocation, and the bit
	// must still be asked for explicitly - without it vkCreateShaderModule
	// reports VUID-VkShaderModuleCreateInfo-pCode-08740.
	vk::PhysicalDeviceShaderDemoteToHelperInvocationFeatures demote;
	demote.shaderDemoteToHelperInvocation = VK_TRUE;
	demote.pNext = &dynRendering;

	vk::PhysicalDeviceSynchronization2Features sync2;
	sync2.synchronization2 = VK_TRUE;
	sync2.pNext = &demote;

	vk::PhysicalDeviceDescriptorIndexingFeatures descriptorIndexing;
	descriptorIndexing.descriptorBindingPartiallyBound = VK_TRUE;
	descriptorIndexing.pNext = &sync2;

	ctx_enabledFeatures = selectOptionalFeatures(pd);
	std::vector<const char*> devExts = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };

	// VK_KHR_portability_subset is required when a profile layer simulates
	// portability (e.g. VP_LUNARG_desktop_baseline): add it when advertised.
	auto availableDevExtensions = pd.enumerateDeviceExtensionProperties();
	for (const auto& ext : availableDevExtensions)
	{
		if (strcmp(ext.extensionName, "VK_KHR_portability_subset") == 0)
		{
			devExts.push_back("VK_KHR_portability_subset");
			break;
		}
	}

	vk::DeviceCreateInfo devCI({}, queueCIs, {}, devExts, &ctx_enabledFeatures, &descriptorIndexing);

	ctx_device = std::make_unique<vk::raii::Device>(pd, devCI);

	// Get transfer queue handle immediately (does not need present check)
	ctx_transferQueueOwned = std::make_unique<SharedQueue>(
		*ctx_device, ctx_device->getQueue(ctx_transferQueueFamily, 0),
		ctx_transferQueueFamily);
	ctx_transferQueue = ctx_transferQueueOwned.get();
}

void VulkanContext::InitQueue(const vk::raii::SurfaceKHR& surface)
{
	ctx_graphicsQueueFamily = findGraphicsQueueFamilyWithPresent(surface);
	const vk::Queue graphicsHandle = ctx_device->getQueue(ctx_graphicsQueueFamily, 0);

	// One wrapper per distinct handle: a single-family device (MoltenVK, a
	// desktop-baseline profile) returns the same VkQueue for both families, and
	// a second wrapper would guard that queue with a second mutex - which is the
	// same as no mutex at all. Aliasing keeps one lock for both roles.
	if (graphicsHandle == ctx_transferQueue->Handle())
	{
		ctx_graphicsQueue = ctx_transferQueue;
		NEURUS_LOG("[VulkanContext] graphics and transfer share one VkQueue (one lock)");
		return;
	}

	ctx_graphicsQueueOwned = std::make_unique<SharedQueue>(
		*ctx_device, graphicsHandle, ctx_graphicsQueueFamily);
	ctx_graphicsQueue = ctx_graphicsQueueOwned.get();
	NEURUS_LOG("[VulkanContext] graphics and transfer use separate VkQueues (two locks)");
}

VulkanContext::~VulkanContext() {}

uint32_t VulkanContext::selectPhysicalDeviceIndex()
{
	if (ctx_physicalDevices.empty())
		throw std::runtime_error("No Vulkan-capable GPUs found.");
	for (uint32_t i = 0; i < (uint32_t)ctx_physicalDevices.size(); ++i)
		if (ctx_physicalDevices[i].getProperties().deviceType == vk::PhysicalDeviceType::eDiscreteGpu)
			return i;
	return 0;
}

uint32_t VulkanContext::findGraphicsQueueFamily()
{
	auto& pd = ctx_physicalDevices[ctx_selectedDeviceIndex];
	auto qf = pd.getQueueFamilyProperties();
	for (uint32_t i = 0; i < (uint32_t)qf.size(); ++i)
	{
		if (qf[i].queueFlags & vk::QueueFlagBits::eGraphics)
			return i;
	}
	throw std::runtime_error("No graphics-capable queue family found.");
}

uint32_t VulkanContext::findGraphicsQueueFamilyWithPresent(const vk::raii::SurfaceKHR& surface)
{
	auto& pd = ctx_physicalDevices[ctx_selectedDeviceIndex];
	auto qf = pd.getQueueFamilyProperties();
	for (uint32_t i = 0; i < (uint32_t)qf.size(); ++i)
	{
		if (!(qf[i].queueFlags & vk::QueueFlagBits::eGraphics)) continue;
		if (!pd.getSurfaceSupportKHR(i, *surface)) continue;
		return i;
	}
	throw std::runtime_error("No queue family with graphics + present.");
}

// ---------------------------------------------------------------------------
// findTransferQueueFamily
// ---------------------------------------------------------------------------

uint32_t VulkanContext::findTransferQueueFamily()
{
	auto& pd = ctx_physicalDevices[ctx_selectedDeviceIndex];
	auto qf = pd.getQueueFamilyProperties();

	// This queue carries UploadManager's staging copies AND the IBL convolution
	// dispatches, so it must support compute: a transfer-only family would fail
	// vkCmdDispatch. Prefer an async-compute family (compute + transfer, no
	// graphics) so uploads stay off the graphics queue.
	//
	// Known gap: with a separate family, the images it writes are eExclusive
	// and no queue-family ownership transfer is recorded, and the upload
	// barriers name fragment-shader stages this queue does not support. Until
	// both are fixed, only a device whose fallback below is taken (e.g.
	// MoltenVK, a single family) is validation-clean.
	const auto computeTransfer = vk::QueueFlagBits::eCompute | vk::QueueFlagBits::eTransfer;
	for (uint32_t i = 0; i < (uint32_t)qf.size(); ++i)
	{
		if ((qf[i].queueFlags & computeTransfer) == computeTransfer &&
		    !(qf[i].queueFlags & vk::QueueFlagBits::eGraphics))
		{
			return i;
		}
	}

	// Fall back to the graphics family: a graphics family always supports
	// compute and transfer.
	return ctx_graphicsQueueFamily;
}

} // namespace neurus
