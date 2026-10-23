/**
 * @file IFrameRenderer.h
 * @brief What the render thread needs from a renderer, and nothing more.
 *
 * RenderThread drives a renderer through this interface rather than through
 * DeferredRenderer directly, so its loop - the ordering of fence waits, control
 * messages, requests and draws - can be tested headless against a fake.
 *
 * Every method here is called from ONE thread at a time: the render thread once
 * it is running, or the main thread while no render thread exists. None of them
 * is thread-safe on its own; RenderThread is what serialises them.
 *
 * Call-order contract RenderThread honours, and an implementation may rely on:
 * HandleResize, HandleSurfaceChange, ReadIDBufferPixel, CaptureScreenshot and
 * ProcessRequests are only called after WaitForFrameSlot() returned true, i.e.
 * when the GPU no longer uses anything the previous frame submitted.
 */

#pragma once

#include "ProfilingData.h"
#include "RenderRequestSink.h"

#include <vulkan/vulkan_raii.hpp>

#include <cstdint>

namespace neurus {

struct RenderContext;

/**
 * @brief Renderer operations the render thread sequences.
 *
 * Post() (inherited) is the one method that may be called from another thread
 * concurrently with the rest: implementations back it with a thread-safe queue.
 */
class IFrameRenderer : public RenderRequestSink
{
public:
	~IFrameRenderer() override = default;

	/**
	 * @brief Block until the frame slot about to be reused is free on the GPU.
	 * @return False on timeout (a hung GPU); nothing may touch GPU resources then.
	 */
	virtual bool WaitForFrameSlot() = 0;

	/** @brief Execute every posted request, FIFO, as one batch. */
	virtual void ProcessRequests() = 0;

	/**
	 * @brief Draw one frame: acquire, record, submit, present.
	 * @param ctx  Context whose snapshot points at the frame to draw.
	 * @return The latest profile, valid until the next call.
	 */
	virtual const FrameProfile& DrawFrame(const RenderContext& ctx) = 0;

	/** @brief Recreate the swapchain for a new window size (physical pixels). */
	virtual void HandleResize(uint32_t width, uint32_t height) = 0;

	/** @brief Rebuild the swapchain on a new surface the caller keeps alive. */
	virtual void HandleSurfaceChange(const vk::raii::SurfaceKHR& newSurface) = 0;

	/**
	 * @brief Read the object id under one pixel of the ID buffer.
	 * @return The id, or 0 for background, out of bounds or failure.
	 */
	virtual uint32_t ReadIDBufferPixel(uint32_t x, uint32_t y) = 0;

	/** @brief Current swapchain extent in physical pixels. */
	virtual vk::Extent2D GetExtent() const = 0;

	/**
	 * @brief Write the last presented image, or every attachment, to disk.
	 * @param allAttachments  False: the swapchain image. True: every attachment.
	 */
	virtual void CaptureScreenshot(bool allAttachments) = 0;

	/** @brief Block until all GPU work completes; called once on shutdown. */
	virtual void WaitIdle() = 0;

protected:
	IFrameRenderer() = default;
};

} // namespace neurus
