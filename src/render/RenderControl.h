/**
 * @file RenderControl.h
 * @brief Control messages the main thread sends the render thread.
 *
 * Distinct from RenderRequest on purpose. A RenderRequest is Editor-owned GPU
 * state (uploads, resets) that the Editor posts and never waits for. A control
 * message is application plumbing - window size, surface, picking, screenshots -
 * that RenderThread executes itself between frames, after the frame slot's
 * fence, and some of which the caller waits on through a promise.
 *
 * Every alternative is a value; the variant is move-only because some carry a
 * std::promise. A message that is never executed (posted after the thread
 * stopped, or still pending when it stops) is destroyed, which breaks its
 * promise, so a waiting caller sees std::future_error instead of hanging.
 */

#pragma once

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>
#include <filesystem>
#include <future>
#include <variant>

namespace neurus {

/// @brief New window size in physical pixels. Only the last of a batch runs.
struct ResizeControl
{
	uint32_t width  = 0;
	uint32_t height = 0;
};

/**
 * @brief Rebuild the swapchain on a new surface.
 *
 * The surface stays owned by the UI side. The caller must keep it, and the old
 * one, alive until @c done is satisfied: the old swapchain still refers to the
 * old surface until then.
 */
struct SurfaceChangeControl
{
	const vk::raii::SurfaceKHR* surface = nullptr;
	std::promise<void>          done;
};

/**
 * @brief Read the object id under a viewport position.
 *
 * Coordinates are normalised to [0, 1] over the viewport, so the caller never
 * needs the swapchain extent; the render thread maps them onto its own extent.
 */
struct PickControl
{
	float                  u = 0.0f;
	float                  v = 0.0f;
	std::promise<uint32_t> objectId;
};

/**
 * @brief Write the last presented image, or every attachment, to disk.
 *
 * The directory is carried here as a value because only the Application knows
 * where the executable lives; the renderer must stay Qt-free and must not
 * resolve paths against the process working directory.
 */
struct ScreenshotControl
{
	bool                  allAttachments = false;
	std::filesystem::path directory;  ///< Destination root (created on demand).
};

using RenderControl = std::variant<ResizeControl,
                                   SurfaceChangeControl,
                                   PickControl,
                                   ScreenshotControl>;

} // namespace neurus
