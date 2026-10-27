#pragma once

#include "core/Log.h"
#include "editor/events/IEventQueue.h"

#include <functional>
#include <queue>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace neurus {

class EventQueue : public IEventQueue
{
public:
	/**
	 * @brief Hard limit on nested synchronous dispatch depth.
	 *
	 * A handler that synchronously re-emits an event type recurses instead of
	 * reaching the deferred queue, so dispatch stops at this depth, logs once
	 * and returns.
	 */
	static constexpr int kMaxDispatchDepth = 64;

	EventQueue() = default;

	// Non-copyable - moved via Application ownership
	EventQueue(const EventQueue&) = delete;
	EventQueue& operator=(const EventQueue&) = delete;

	/**
	 * @brief Dispatches all enqueued events in FIFO order.
	 * @note The drain starts from a clean depth so one runaway chain cannot
	 *       poison later frames; a nested caller's depth is restored after.
	 */
	void Process(int maxEvents = 1000)
	{
		struct DepthReset
		{
			int& depth;
			int saved;
			explicit DepthReset(int& d) : depth(d), saved(d) { depth = 0; }
			~DepthReset() { depth = saved; }
		} reset(m_depth);

		int count = 0;
		while (!evt_eventQueue.empty() && count < maxEvents)
		{
			auto fn = std::move(evt_eventQueue.front());
			evt_eventQueue.pop();
			fn();
			++count;
		}
	}

	/**
	 * @brief Returns the number of pending events in the queue.
	 */
	size_t PendingCount() const { return evt_eventQueue.size(); }

	/**
	 * @brief Queues an event for deferred dispatch at the next Process().
	 *
	 * Only the UI boundary uses this: Editor::OnUIEvent. Everything inside the
	 * editor emits synchronously through Emit, so a mutation is visible to its
	 * caller.
	 */
	template<typename TEvent>
	void Enqueue(const TEvent& event)
	{
		std::type_index ti(typeid(TEvent));
		EnqueueImpl([this, ti, ev = event]() {
			Dispatch(ti, &ev);
		});
	}

protected:
	void SubscribeImpl(std::type_index type, std::function<void(const void*)> handler) override
	{
		evt_handlers[type].push_back(std::move(handler));
	}

	void Dispatch(std::type_index type, const void* event) override
	{
		if (m_depth >= kMaxDispatchDepth)
		{
			NEURUS_ERR("[EventQueue] dispatch depth limit reached at " << type.name());
			return;
		}

		// RAII so a throwing handler cannot leave the counter elevated.
		struct DepthGuard
		{
			int& depth;
			explicit DepthGuard(int& d) : depth(d) { ++depth; }
			~DepthGuard() { --depth; }
		} guard(m_depth);

		auto it = evt_handlers.find(type);
		if (it == evt_handlers.end()) return;

		for (auto& fn : it->second)
		{
			fn(event);
		}
	}

private:
	/**
	 * @brief Pushes a deferred dispatch thunk; the only writer of the queue.
	 *
	 * Not virtual: deferral is a property of this concrete queue, not of the
	 * controller-facing interface.
	 */
	void EnqueueImpl(std::function<void()> thunk)
	{
		evt_eventQueue.push(std::move(thunk));
	}

	std::unordered_map<
		std::type_index,
		std::vector<std::function<void(const void*)> >
	> evt_handlers;

	std::queue<std::function<void()> > evt_eventQueue;

	// Current nested dispatch depth; guards against self-re-emitting handlers.
	int m_depth = 0;
};

} // namespace neurus
