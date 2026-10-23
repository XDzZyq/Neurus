/**
 * @file ThreadPool.h
 * @brief Minimal fixed-size worker pool — the Executor for Graph::ParallelTraverse.
 *
 * Hand-rolled from standard C++ only (std::thread, std::mutex,
 * std::condition_variable) — no third-party library and no Qt, so it is usable
 * from the Vulkan-free core and the Qt-free renderer alike. Satisfies the
 * Executor concept consumed by Graph::ParallelTraverse: a single
 * `submit(std::function<void()>)` entry point.
 *
 * @note std::jthread / std::stop_token (C++20) would express cooperative
 *       shutdown more directly, but AppleClang's libc++ does not ship
 *       <stop_token>, so this uses std::thread plus an explicit stop flag and
 *       join. Behaviour is identical and the implementation stays standard and
 *       portable.
 */

#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace neurus {

/**
 * @brief Fixed-size pool of worker threads running submitted tasks.
 *
 * Tasks are dispatched FIFO to whichever worker becomes free. Shutdown is
 * cooperative: the destructor signals stop, lets the queued tasks drain, then
 * joins every worker. The pool is non-copyable and non-movable; it owns its
 * workers for its whole lifetime.
 *
 * Not a dependency scheduler itself — it only runs opaque tasks. Dependency
 * ordering is Graph::ParallelTraverse's job; this pool is just its Executor.
 */
class ThreadPool
{
public:
	/**
	 * @brief Construct a pool with the given worker count.
	 * @param workers  Number of threads; clamped to at least 1. Defaults to
	 *                 the hardware concurrency (also floored at 1).
	 */
	explicit ThreadPool(unsigned workers = std::thread::hardware_concurrency());

	/** @brief Signal stop, drain queued tasks, and join all workers. */
	~ThreadPool();

	ThreadPool(const ThreadPool&)            = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;
	ThreadPool(ThreadPool&&)                 = delete;
	ThreadPool& operator=(ThreadPool&&)      = delete;

	/**
	 * @brief Enqueue a task to be run by some worker.
	 * @param task  Callable with no arguments; must not be empty.
	 *
	 * Thread-safe; may be called from inside a running task (as
	 * Graph::ParallelTraverse does when a node unlocks its successors).
	 */
	void submit(std::function<void()> task);

	/** @return Number of worker threads in the pool. */
	size_t WorkerCount() const { return m_workers.size(); }

private:
	void workerLoop();

	std::vector<std::thread>          m_workers;
	std::queue<std::function<void()>> m_tasks;
	std::mutex                        m_mutex;
	std::condition_variable           m_cv;
	bool                              m_stop = false;  ///< Guarded by m_mutex.
};

} // namespace neurus
