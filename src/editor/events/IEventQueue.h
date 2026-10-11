/**
 * @file IEventQueue.h
 * @brief Controller-facing interface for the typed event dispatcher.
 *
 * Controllers depend only on this narrow interface, never on the concrete queue:
 * subscribe handlers, enqueue for deferred dispatch (the frame-synchronization
 * boundary), and emitNow for synchronous replay. The queue-management surface
 * stays out of the interface so controllers cannot drive dispatch.
 */

#pragma once

#include <functional>
#include <typeindex>
#include <utility>

namespace neurus {

/**
 * @brief Minimal event dispatch surface exposed to controllers.
 */
class IEventQueue
{
public:
	virtual ~IEventQueue() = default;

	/**
	 * @brief Subscribes a handler to events of type TEvent.
	 */
	template<typename TEvent>
	void subscribe(std::function<void(const TEvent&)> handler)
	{
		subscribeImpl(std::type_index(typeid(TEvent)),
			[h = std::move(handler)](const void* e) {
				h(*static_cast<const TEvent*>(e));
			}
		);
	}

	/**
	 * @brief Enqueues an event for DEFERRED dispatch (frame-synchronization
	 *        boundary). Enqueued events are drained by Process() at one point
	 *        per frame, so all scene mutation happens at a predictable point.
	 *        Use this for everything EXCEPT operation replay.
	 */
	template<typename TEvent>
	void enqueue(const TEvent& event)
	{
		std::type_index ti(typeid(TEvent));
		enqueueImpl([this, ti, ev = event]() {
			dispatch(ti, &ev);
		});
	}

	/**
	 * @brief Dispatches an event SYNCHRONOUSLY, bypassing the deferred queue.
	 *
	 * Reserved for operation replay: the synthesized inverse event must apply
	 * in-place so it cannot be reordered against the user's still-queued events.
	 * Do NOT use this for real-time input.
	 *
	 * @note Named emitNow because Qt reserves `emit` as a keyword macro.
	 */
	template<typename TEvent>
	void emitNow(const TEvent& event)
	{
		dispatch(std::type_index(typeid(TEvent)), &event);
	}

protected:
	/** @brief Registers a type-erased handler for a TEvent type. */
	virtual void subscribeImpl(std::type_index type, std::function<void(const void*)> handler) = 0;

	/** @brief Queues a deferred dispatch thunk. */
	virtual void enqueueImpl(std::function<void()> thunk) = 0;

	/** @brief Synchronously invokes every handler registered for @p type. */
	virtual void dispatch(std::type_index type, const void* event) = 0;
};

} // namespace neurus
