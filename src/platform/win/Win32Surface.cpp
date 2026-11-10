#include "platform/PlatformSurface.h"

#include <Windows.h>

namespace neurus::platform
{

std::vector<const char*> RequiredInstanceExtensions()
{
	return {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME
	};
}

vk::InstanceCreateFlags InstanceCreateFlags()
{
	return {};
}

vk::raii::SurfaceKHR CreateSurface(
	const vk::raii::Instance& instance,
	NativeWindowHandle windowHandle)
{
	HINSTANCE hinstance = GetModuleHandle(nullptr);
	vk::Win32SurfaceCreateInfoKHR createInfo({}, hinstance, static_cast<HWND>(windowHandle));
	return vk::raii::SurfaceKHR(instance, createInfo);
}

void OnResize(NativeWindowHandle /*windowHandle*/, uint32_t /*width*/, uint32_t /*height*/)
{
}

} // namespace neurus::platform
