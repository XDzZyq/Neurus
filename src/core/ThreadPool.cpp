/**
 * @file ThreadPool.cpp
 * @brief Implementation of ThreadPool.
 */

#include "ThreadPool.h"

#include <algorithm>
#include <utility>

namespace neurus {

ThreadPool::ThreadPool(unsigned workers)
{
	const unsigned count = std::max(1u, workers);
	m_workers.reserve(count);
	for (unsigned i = 0; i < count; ++i)
		m_workers.emplace_back([this] { workerLoop(); });
}

ThreadPool::~ThreadPool()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stop = true;
	}
	m_cv.notify_all();
	for (auto& w : m_workers)
		if (w.joinable())
			w.join();
}

void ThreadPool::submit(std::function<void()> task)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_tasks.push(std::move(task));
	}
	m_cv.notify_one();
}

void ThreadPool::workerLoop()
{
	while (true)
	{
		std::function<void()> task;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_cv.wait(lock, [this] { return m_stop || !m_tasks.empty(); });
			// Drain outstanding tasks even after stop is requested; only exit
			// once the queue is empty, so no submitted work is dropped.
			if (m_stop && m_tasks.empty())
				return;
			task = std::move(m_tasks.front());
			m_tasks.pop();
		}
		task();
	}
}

} // namespace neurus
