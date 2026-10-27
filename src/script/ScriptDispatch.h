/**
 * @file ScriptDispatch.h
 * @brief The single gated funnel a script setter uses to request a mutation.
 *
 * A Python setter never writes a C++ property. It constructs the exact typed
 * editor event and hands it to `ScriptDispatch::Emit`, which either emits it
 * synchronously (write phase) or refuses (this read-only phase). Every writable
 * property is declared together with the event it will emit, so enabling writes
 * later means supplying an `IEventQueue` and nothing else changes (design
 * section 4.4).
 */

#pragma once

#include <stdexcept>

#include "editor/events/IEventQueue.h"

namespace neurus
{

/// @brief Thrown when a script attempts a mutation in the read-only phase.
struct ReadOnlyError : std::runtime_error
{
	using std::runtime_error::runtime_error;
};

/// @brief Turns a scripted property set into a typed editor event.
class ScriptDispatch
{
public:
	/// @brief Supplies the queue events are emitted to (write phase only).
	/// @param queue Borrowed event queue, or nullptr for the read-only phase.
	void SetEventQueue(IEventQueue* queue) { m_events = queue; }

	/**
	 * @brief Emits @p event, or refuses in the read-only phase.
	 *
	 * The throw is left to propagate to Python, which either handles it or lets
	 * the run's error handler log it - so a script that catches `ReadOnlyError`
	 * does not flood the log (design section 7).
	 * @tparam TEvent A typed editor event.
	 * @throws ReadOnlyError when no event queue is attached.
	 */
	template <typename TEvent>
	void Emit(const TEvent& event) const
	{
		if (m_events == nullptr)
		{
			throw ReadOnlyError("Neurus scripting is read-only in this phase");
		}
		m_events->emitNow(event);
	}

private:
	IEventQueue* m_events = nullptr;
};

}  // namespace neurus
