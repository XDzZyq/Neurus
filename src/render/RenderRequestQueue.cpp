/**
 * @file RenderRequestQueue.cpp
 * @brief RenderRequestQueue implementation.
 */

#include "RenderRequestQueue.h"

namespace neurus {

void RenderRequestQueue::Post(RenderRequest&& request)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_requests.push_back(std::move(request));
}

std::vector<RenderRequest> RenderRequestQueue::TakeAll()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	std::vector<RenderRequest> batch;
	// Swap rather than copy-then-clear: the batch leaves with the storage, so a
	// Post() arriving while the caller executes this batch grows a fresh vector.
	batch.swap(m_requests);
	return batch;
}

bool RenderRequestQueue::Empty() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_requests.empty();
}

} // namespace neurus
