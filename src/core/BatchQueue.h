/**
 * @file BatchQueue.h
 * @brief Mutex-guarded FIFO handed between threads one whole batch at a time.
 *
 * Producers Push() single items; the consumer TakeAll()s everything pending in
 * one swap. Taking the whole batch, rather than popping one item at a time, is
 * the guarantee callers rely on: an item pushed while a taken batch is being
 * executed lands in the next batch, never mid-iteration, so a handler may push a
 * follow-up without the consumer tripping over the container it is walking.
 * Within a batch the order is strict FIFO - push order is execution order.
 *
 * Items are moved in and moved out, never copied, so move-only payloads such as
 * std::promise travel through it unchanged.
 *
 * Neither copyable nor movable: it is the rendezvous point both threads refer
 * to, so it keeps one identity and one address for its lifetime.
 *
 * Standard C++ only (no Qt), so every layer may use it.
 */

#pragma once

#include <mutex>
#include <utility>
#include <vector>

namespace neurus {

/**
 * @brief Thread-safe FIFO drained a batch at a time.
 * @tparam T  Item type; must be move-constructible.
 */
template <typename T>
class BatchQueue
{
public:
	BatchQueue()  = default;
	~BatchQueue() = default;

	BatchQueue(const BatchQueue&)            = delete;
	BatchQueue& operator=(const BatchQueue&) = delete;
	BatchQueue(BatchQueue&&)                 = delete;
	BatchQueue& operator=(BatchQueue&&)      = delete;

	/**
	 * @brief Append one item to the back of the queue.
	 * @param item  Moved in, never copied.
	 */
	void Push(T&& item)
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_items.push_back(std::move(item));
	}

	/**
	 * @brief Remove and return every pending item, oldest first.
	 *
	 * Swaps the storage out rather than copying, so a Push() that arrives while
	 * the caller executes the returned batch grows a fresh vector.
	 */
	std::vector<T> TakeAll()
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		std::vector<T> batch;
		batch.swap(m_items);
		return batch;
	}

	/** @return True when nothing is pending. */
	bool Empty() const
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		return m_items.empty();
	}

private:
	mutable std::mutex m_mutex;
	std::vector<T>     m_items;
};

} // namespace neurus
