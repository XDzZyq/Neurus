/**
 * @file IEventQueue.h
 * @brief Controller-facing interface for the typed event dispatcher.
 *
 * Controllers depend only on this narrow interface, never on the concrete queue:
 * Subscribe handlers and Emit for synchronous dispatch. Deferral is deliberately
 * absent here - it exists only on the concrete EventQueue and only for the UI
 * boundary - so anything a controller or a script emits has been applied before
 * the emitting call returns.
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
	void Subscribe(std::function<void(const TEvent&)> handler)
	{
		SubscribeImpl(std::type_index(typeid(TEvent)),
			[h = std::move(handler)](const void* e) {
				h(*static_cast<const TEvent*>(e));
			}
		);
	}

	/**
	 * @brief Dispatches an event SYNCHRONOUSLY: every handler has run by return.
	 *
	 * The one way to emit through this interface. A handler's mutation is
	 * therefore complete when the emitting call returns, which is what lets a
	 * script observe its own write and a composite path observe the event it just
	 * emitted. Deferral is not offered here; only the UI boundary queues.
	 *
	 * @note Named Emit because Qt reserves `emit` as a keyword macro.
	 */
	template<typename TEvent>
	void Emit(const TEvent& event)
	{
		Dispatch(std::type_index(typeid(TEvent)), &event);
	}

protected:
	/** @brief Registers a type-erased handler for a TEvent type. */
	virtual void SubscribeImpl(std::type_index type, std::function<void(const void*)> handler) = 0;

	/** @brief Synchronously invokes every handler registered for @p type. */
	virtual void Dispatch(std::type_index type, const void* event) = 0;
};

} // namespace neurus
