/**
 * @file ProxyBase.h
 * @brief Shared state of every Python proxy.
 *
 * A proxy carries the UID of the object it stands for and the `ScriptView` of
 * the run that constructed it. It owns nothing: the view outlives the proxy, and
 * each proxy decides for itself how to read the context - there is no shared
 * accessor and no process-wide current view.
 */

#pragma once

#include "script/ScriptDispatch.h"
#include "script/ScriptView.h"

namespace neurus
{
namespace script
{

/// @brief Common members of every proxy: identity plus the constructing run's view.
struct ProxyBase
{
	int               uid = 0;      ///< Pooled object UID; 0 when unused.
	const ScriptView* view = nullptr;  ///< View injected when the proxy was constructed.
};

/**
 * @brief Emits a scripted mutation through the proxy's run dispatch.
 *
 * The dispatch refuses while the read-only phase has no event queue attached;
 * the throw propagates to Python so a script can handle `ReadOnlyError`.
 *
 * @tparam TEvent A typed editor event.
 */
template <typename TEvent>
void EmitEvent(const ProxyBase& proxy, const TEvent& event)
{
	const ScriptDispatch* dispatch = proxy.view != nullptr ? proxy.view->dispatch : nullptr;
	if (dispatch == nullptr)
	{
		throw ReadOnlyError("Neurus scripting is read-only in this phase");
	}
	dispatch->Emit(event);
}

}  // namespace script
}  // namespace neurus
