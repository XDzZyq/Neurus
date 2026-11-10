#include "Win32Surface.h"

#include <Windows.h>

namespace neurus {

std::vector<const char*> Win32Surface::requiredInstanceExtensions() const
{
	return {
		VK_KHR_SURFACE_EXTENSION_NAME,
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME
	};
}

vk::raii::SurfaceKHR Win32Surface::createSurface(
	const vk::raii::Instance& instance,
	NativeWindowHandle windowHandle) const
{
	HINSTANCE hinstance = GetModuleHandle(nullptr);
	vk::Win32SurfaceCreateInfoKHR createInfo({}, hinstance, static_cast<HWND>(windowHandle));
	return vk::raii::SurfaceKHR(instance, createInfo);
}

std::unique_ptr<PlatformSurface> CreatePlatformSurface()
{
	return std::make_unique<Win32Surface>();
}

} // namespace neurus
