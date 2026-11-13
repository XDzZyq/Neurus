#include "platform/PlatformSurface.h"

#include <stdexcept>

namespace neurus::platform
{

std::vector<const char*> RequiredInstanceExtensions()
{
	// A headless session presents no surface, so no WSI extension is needed.
	return {};
}

vk::InstanceCreateFlags InstanceCreateFlags()
{
	return {};
}

vk::raii::SurfaceKHR CreateSurface(
	const vk::raii::Instance& instance,
	NativeWindowHandle windowHandle)
{
	// The headless renderer draws into its own images, so a window surface is a
	// configuration error rather than a silently black viewport.
	(void)instance;
	(void)windowHandle;
	throw std::runtime_error("Neurus on Linux is headless: no window surface is available");
}

void OnResize(NativeWindowHandle windowHandle, uint32_t width, uint32_t height)
{
	(void)windowHandle;
	(void)width;
	(void)height;
}

} // namespace neurus::platform
