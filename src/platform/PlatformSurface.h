#pragma once

/**
 * @file PlatformSurface.h
 * @brief Abstract platform surface: Vulkan surface creation without OS ifdefs.
 *
 * Each platform (Win32, macOS, X11, ...) supplies a subclass that knows how
 * to create a VkSurfaceKHR from that platform's native window handle.
 */

#include <vulkan/vulkan_raii.hpp>
#include <vector>
#include <memory>

namespace neurus {

/// Opaque native window handle. HWND on Win32, NSView* on macOS.
using NativeWindowHandle = void*;

/// Abstract platform surface factory.
class PlatformSurface {
public:
	virtual ~PlatformSurface() = default;

	/// Vulkan instance extensions required by this platform's surface type.
	virtual std::vector<const char*> requiredInstanceExtensions() const = 0;

	/// Instance create flags (e.g., eEnumeratePortabilityKHR on macOS).
	virtual vk::InstanceCreateFlags instanceCreateFlags() const { return {}; }

	/// Create a VkSurfaceKHR from a native window handle.
	virtual vk::raii::SurfaceKHR createSurface(
		const vk::raii::Instance& instance,
		NativeWindowHandle windowHandle) const = 0;

	/// Notify the platform of a resize. Default: no-op.
	virtual void onResize(NativeWindowHandle windowHandle,
	                      uint32_t width, uint32_t height) const {}
};

/// Compile-time factory — returns the platform-appropriate PlatformSurface.
std::unique_ptr<PlatformSurface> CreatePlatformSurface();

} // namespace neurus
