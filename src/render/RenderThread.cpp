/**
 * @file RenderThread.cpp
 * @brief RenderThread implementation: the loop, the control queue and shutdown.
 *
 * The design and the loop itself live in RenderThread.h; this file only spells
 * out the ordering the header promises - fence-first, resize coalescing, the
 * invalidate-after-Post(), and the broken-promise shutdown path.
 */

#include "RenderThread.h"

#include "RenderContext.h"

#include "core/Log.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace neurus
{

RenderThread::RenderThread(IFrameRenderer& renderer)
	: m_renderer(renderer)
{
}

RenderThread::~RenderThread()
{
	Stop();
}

void RenderThread::Start()
{
	// At most once: a second launch would leave two threads driving one
	// renderer, the exact single-owner rule this type exists to enforce.
	if (m_started)
	{
		NEURUS_ERR("[RenderThread] Start() called more than once - ignoring");
		return;
	}

	m_started = true;
	m_running = true;
	m_thread  = std::thread([this] { Run(); });
}

void RenderThread::Stop()
{
	// Never launched: no loop will ever drain the control queue, so stop
	// accepting messages and destroy any already posted. Their promises break
	// here rather than leaving a caller waiting on a thread that never runs.
	if (!m_started)
	{
		std::lock_guard<std::mutex> lock(m_acceptMutex);
		m_accepting = false;
		m_control.TakeAll();  // destroyed at scope exit -> broken promises
		return;
	}

	// Idempotent: RequestStop() is sticky and join() is skipped once the thread
	// has already been joined, so a second Stop() (e.g. from the destructor
	// after an explicit Stop()) is harmless.
	m_mailbox.RequestStop();
	if (m_thread.joinable())
		m_thread.join();
}

bool RenderThread::Running() const
{
	return m_running.load();
}

void RenderThread::Publish(RenderFrameSnapshot&& snapshot)
{
	m_mailbox.Publish(std::move(snapshot));
}

void RenderThread::Post(RenderRequest&& request)
{
	m_renderer.Post(std::move(request));

	// Invalidate rather than just wake: a request restarts accumulation or
	// marks overlays stale, so a frame snapshotted before it describes state the
	// request replaced. Invalidate raises the wake (the request still has to run
	// before any later frame is drawn, the ordering guarantee in the header) and
	// drops the held frame, so the loop waits for a fresh Publish instead of
	// redrawing the one whose camera predates the request.
	m_mailbox.Invalidate();
}

void RenderThread::PostResize(uint32_t width, uint32_t height)
{
	// A resize restarts accumulation: the old frame's extent and camera are both
	// stale, so the held frame must not be redrawn after it.
	PostControl(ResizeControl{width, height}, true);
}

std::future<void> RenderThread::PostSurfaceChange(const vk::raii::SurfaceKHR& surface)
{
	SurfaceChangeControl control;
	control.surface = &surface;

	// Take the future before the message is moved away: once posted, the
	// message belongs to the control queue (or is already destroyed).
	std::future<void> future = control.done.get_future();
	PostControl(std::move(control), true);  // a new swapchain restarts accumulation
	return future;
}

std::future<uint32_t> RenderThread::PostPick(float u, float v)
{
	PickControl control;
	control.u = u;
	control.v = v;

	// Plain wake: a pick reads the ID buffer, so it must neither discard a
	// pending frame nor force a fresh one.
	std::future<uint32_t> future = control.objectId.get_future();
	PostControl(std::move(control), false);
	return future;
}

void RenderThread::PostScreenshot(bool allAttachments)
{
	// Plain wake: the screenshot reads whatever was drawn last.
	PostControl(ScreenshotControl{allAttachments}, false);
}

RendererStatus RenderThread::Status() const
{
	std::lock_guard<std::mutex> lock(m_statusMutex);
	return m_status;
}

void RenderThread::PostControl(RenderControl&& message, bool restartAccumulation)
{
	{
		std::lock_guard<std::mutex> lock(m_acceptMutex);

		// Rejected after the loop has ended: let the message (and any promise it
		// carries) be destroyed as the argument goes out of scope, so a waiting
		// caller sees a broken promise instead of hanging on a dead thread.
		if (!m_accepting)
			return;

		m_control.Push(std::move(message));
	}

	// Enqueue first, THEN signal: the mailbox's ordering contract makes the
	// message visible to the consumer draining on Interrupted. Resizes and
	// surface changes also drop the held frame, since they restart accumulation.
	if (restartAccumulation)
		m_mailbox.Invalidate();
	else
		m_mailbox.Wake();
}

void RenderThread::Run()
{
	// Report the starting extent before the first frame, so a Status() query
	// between Start() and the first draw already reflects the swapchain size.
	PublishExtent();

	// One try around the whole loop: a renderer exception is a fault that keeps
	// recurring, so fail loud once and leave rather than spam it every frame.
	try
	{
		for (;;)
		{
			// Fence first: a control message or request may replace or read a
			// resource the previous frame's submission still uses, so wait for
			// the slot before draining either. An Interrupted iteration does not
			// draw, so the loop waits here again before the next drain.
			if (m_renderer.WaitForFrameSlot())
			{
				DrainControl();
				m_renderer.ProcessRequests();
			}

			// Ask the renderer how much work the next draw is worth before
			// deciding how long to wait. Sample can redraw the frame already
			// held; Overlay waits for the fresh frame a RedrawOverlaysRequest
			// always arrives with; Idle parks until a control message and never
			// consumes a published frame; Retry waits because the last draw
			// produced nothing (no camera, acquire failure) and must not spin.
			AcquireMode mode = AcquireMode::Wait;
			switch (m_renderer.NextWork())
			{
			case FrameWork::Sample:  mode = AcquireMode::Reuse; break;
			case FrameWork::Overlay: mode = AcquireMode::Wait;  break;
			case FrameWork::Idle:    mode = AcquireMode::Idle;  break;
			case FrameWork::Retry:   mode = AcquireMode::Wait;  break;
			}

			const AcquireStatus status = m_mailbox.Acquire(m_frame, mode);
			if (status == AcquireStatus::Stopped)
				break;
			if (status == AcquireStatus::Interrupted)
				continue;

			// Frame or Repeat. On Repeat nothing was moved into m_frame, so it
			// still holds the last snapshot - exactly what a Sample draw wants.
			RenderContext ctx;
			ctx.snapshot = &m_frame;
			const FrameProfile& profile = m_renderer.DrawFrame(ctx);

			std::lock_guard<std::mutex> lock(m_statusMutex);
			m_status.profile = profile;
		}
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("[RenderThread] render loop aborted: " << e.what());
	}
	catch (...)
	{
		NEURUS_ERR("[RenderThread] render loop aborted: unknown exception");
	}

	// Both exits (stop and escaped exception) land here. Stop accepting and
	// destroy whatever is still queued so every pending promise breaks and no
	// caller is left waiting.
	{
		std::lock_guard<std::mutex> lock(m_acceptMutex);
		m_accepting = false;
		m_control.TakeAll();  // destroyed at scope exit -> broken promises
	}

	// Drain the device on the way out: this thread owns every GPU call, so the
	// idle must happen here, not on the thread that joins us.
	try
	{
		m_renderer.WaitIdle();
	}
	catch (const std::exception& e)
	{
		NEURUS_ERR("[RenderThread] WaitIdle on shutdown failed: " << e.what());
	}
	catch (...)
	{
		NEURUS_ERR("[RenderThread] WaitIdle on shutdown failed: unknown exception");
	}

	m_running = false;
}

void RenderThread::DrainControl()
{
	std::vector<RenderControl> batch = m_control.TakeAll();

	// Coalesce resizes: rebuilding the swapchain is expensive and only the final
	// size matters, so only the LAST ResizeControl in the batch runs. Every
	// other message keeps its FIFO position, so a pick posted after the resize
	// still reads the extent that resize produced.
	size_t lastResize = batch.size();
	for (size_t i = 0; i < batch.size(); ++i)
	{
		if (std::holds_alternative<ResizeControl>(batch[i]))
			lastResize = i;
	}

	for (size_t i = 0; i < batch.size(); ++i)
	{
		RenderControl& message = batch[i];

		if (std::holds_alternative<ResizeControl>(message) && i != lastResize)
			continue;  // superseded by a later resize in this same batch

		if (auto* resize = std::get_if<ResizeControl>(&message))
		{
			// No promise to carry the failure, so report it and keep going.
			try
			{
				m_renderer.HandleResize(resize->width, resize->height);
				PublishExtent();
			}
			catch (const std::exception& e)
			{
				NEURUS_ERR("[RenderThread] resize failed: " << e.what());
			}
		}
		else if (auto* surface = std::get_if<SurfaceChangeControl>(&message))
		{
			// A null surface is a caller error; surface it through the promise
			// rather than dereferencing null, so the waiting caller sees it.
			if (!surface->surface)
			{
				surface->done.set_exception(std::make_exception_ptr(
					std::invalid_argument("[RenderThread] SurfaceChangeControl with null surface")));
				continue;
			}

			try
			{
				m_renderer.HandleSurfaceChange(*surface->surface);
				PublishExtent();
				surface->done.set_value();
			}
			catch (...)
			{
				surface->done.set_exception(std::current_exception());
			}
		}
		else if (auto* pick = std::get_if<PickControl>(&message))
		{
			try
			{
				const vk::Extent2D extent = m_renderer.GetExtent();
				if (extent.width == 0 || extent.height == 0)
				{
					// No swapchain to read: 0 is the same "background" an
					// out-of-bounds read returns, so the caller needs no branch.
					pick->objectId.set_value(0);
				}
				else
				{
					// Map the normalised position onto the extent and clamp to
					// the last valid pixel, so u/v of exactly 1.0 stays in range.
					const float u = std::clamp(pick->u, 0.0f, 1.0f);
					const float v = std::clamp(pick->v, 0.0f, 1.0f);
					const uint32_t x =
						std::min(static_cast<uint32_t>(u * extent.width), extent.width - 1);
					const uint32_t y =
						std::min(static_cast<uint32_t>(v * extent.height), extent.height - 1);
					pick->objectId.set_value(m_renderer.ReadIDBufferPixel(x, y));
				}
			}
			catch (...)
			{
				pick->objectId.set_exception(std::current_exception());
			}
		}
		else if (auto* screenshot = std::get_if<ScreenshotControl>(&message))
		{
			try
			{
				m_renderer.CaptureScreenshot(screenshot->allAttachments);
			}
			catch (const std::exception& e)
			{
				NEURUS_ERR("[RenderThread] screenshot failed: " << e.what());
			}
		}
	}
}

void RenderThread::PublishExtent()
{
	// Query outside the status lock: GetExtent() is a renderer call, and the
	// status mutex only guards the slot it lands in.
	const vk::Extent2D extent = m_renderer.GetExtent();

	std::lock_guard<std::mutex> lock(m_statusMutex);
	m_status.extent = extent;
}

} // namespace neurus
