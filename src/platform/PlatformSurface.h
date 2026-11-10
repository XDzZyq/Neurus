#pragma once

/**
 * @file PlatformSurface.h
 * @brief Platform-native Vulkan surface creation without OS ifdefs.
 *
 * Each platform supplies one implementation of these functions; CMake compiles
 * only the matching source. Win32 uses VK_KHR_win32_surface, macOS uses
 * VK_EXT_metal_surface.
 */

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <vector>

namespace neurus
{

/// Opaque native window handle. HWND on Win32, NSView* on macOS.
using NativeWindowHandle = void*;

namespace platform
{

/// Vulkan instance extensions required by this platform's surface type.
std::vector<const char*> RequiredInstanceExtensions();

/// Instance create flags (e.g. eEnumeratePortabilityKHR on macOS).
vk::InstanceCreateFlags InstanceCreateFlags();

/// Creates a VkSurfaceKHR from a native window handle.
vk::raii::SurfaceKHR CreateSurface(const vk::raii::Instance& instance,
                                   NativeWindowHandle windowHandle);

/// Notifies the platform of a resize. No-op on platforms without a layer.
void OnResize(NativeWindowHandle windowHandle, uint32_t width, uint32_t height);

} // namespace platform
} // namespace neurus
