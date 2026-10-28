/**
 * @file ScriptDispatch.h
 * @brief The single gated funnel a script setter uses to request a mutation.
 *
 * A Python setter never writes a C++ property. It constructs the exact typed
 * editor event and hands it to `ScriptDispatch::Emit`, which dispatches it
 * synchronously through the editor's `IEventQueue`; the controller handling that
 * event performs the write before the setter returns, so a script observes its
 * own mutation on its next statement. Every writable property is declared
 * together with the event it emits, so this funnel stays the only route from
 * Python into editor state (design section 4.4).
 */

#pragma once

#include <stdexcept>

#include "editor/events/IEventQueue.h"

namespace neurus
{

/// @brief Turns a scripted property set into a typed editor event.
class ScriptDispatch
{
public:
	/// @brief Supplies the queue events are emitted to.
	/// @param queue Borrowed event queue, or nullptr when no editor is attached.
	void SetEventQueue(IEventQueue* queue) { m_events = queue; }

	/// @brief The attached queue, or nullptr when scripting has no editor.
	IEventQueue* EventQueue() const { return m_events; }

	/**
	 * @brief Emits @p event synchronously through the attached queue.
	 *
	 * The throw is left to propagate to Python, which either handles it or lets
	 * the run's error handler log it - so a script that catches the error does
	 * not flood the log (design section 7).
	 * @tparam TEvent A typed editor event.
	 * @throws std::runtime_error when no event queue is attached.
	 */
	template <typename TEvent>
	void Emit(const TEvent& event) const
	{
		if (m_events == nullptr)
		{
			throw std::runtime_error("scripting is not attached to an editor");
		}
		m_events->Emit(event);
	}

private:
	IEventQueue* m_events = nullptr;
};

}  // namespace neurus
