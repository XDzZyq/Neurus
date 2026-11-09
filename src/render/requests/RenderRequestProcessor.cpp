#include "RenderRequestProcessor.h"

#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace neurus {

RenderRequestProcessor::RenderRequestProcessor(std::function<void()> drainFrames,
                                               RenderCache& cache,
                                               GeometryPass* geometryPass,
                                               std::function<void()> restartAccumulation,
                                               std::function<void()> markOverlaysDirty)
	: m_executor(std::move(drainFrames), cache, /*uploader=*/nullptr, geometryPass)
	, m_restartAccumulation(std::move(restartAccumulation))
	, m_markOverlaysDirty(std::move(markOverlaysDirty))
{
}

void RenderRequestProcessor::Post(RenderRequest&& request)
{
	m_queue.Push(std::move(request));
}

void RenderRequestProcessor::Drain()
{
	// Whole batch at once: a request a handler posts lands in the next TakeAll(),
	// never mid-iteration over this one.
	std::vector<RenderRequest> batch = m_queue.TakeAll();
	for (const RenderRequest& request : batch)
	{
		std::visit(
			[this](const auto& typed)
			{
				using T = std::decay_t<decltype(typed)>;

				// Overlays only: keep the converged scene image and redraw on top
				// of it. This is the one request that must NOT restart
				// accumulation - that would throw away the image the redraw
				// exists for.
				if constexpr (std::is_same_v<T, RedrawOverlaysRequest>)
				{
					if (m_markOverlaysDirty) m_markOverlaysDirty();
				}
				else
				{
					// Every other request changes scene content, GPU resources or
					// the view, which invalidates the accumulated average. The
					// restart happens before the handler runs, as it always has.
					if (m_restartAccumulation) m_restartAccumulation();

					// ResetAccumulationRequest exists to ask for exactly that, so
					// it stops here instead of reaching the executor.
					if constexpr (!std::is_same_v<T, ResetAccumulationRequest>)
					{
						m_executor.Execute(typed);
					}
				}
			},
			request);
	}
}

void RenderRequestProcessor::SetUploadManager(UploadManager* uploader)
{
	m_executor.SetUploadManager(uploader);
}

} // namespace neurus
