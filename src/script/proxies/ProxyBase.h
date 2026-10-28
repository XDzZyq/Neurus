/**
 * @file ProxyBase.h
 * @brief Shared state and target resolution of every Python proxy.
 *
 * A proxy carries the UID of the object it stands for and the `ScriptView` of
 * the run that constructed it. It owns nothing: the view outlives the proxy, and
 * each proxy decides for itself how to read the context - there is no shared
 * accessor and no process-wide current view.
 *
 * Resolution lives here rather than in `ObjectProxy` so every proxy, including a
 * bare `Transform3DProxy`, can resolve the object a setter addresses.
 */

#pragma once

#include <memory>
#include <stdexcept>

#include "core/IResourceLookup.h"
#include "scene/ObjectID.h"
#include "script/ScriptContext.h"
#include "script/ScriptDispatch.h"
#include "script/ScriptView.h"

namespace neurus
{
namespace script
{

/// @brief Common members of every proxy: identity, the run's view and target lookup.
struct ProxyBase
{
	int               uid = 0;      ///< Pooled object UID; 0 when unused.
	const ScriptView* view = nullptr;  ///< View injected when the proxy was constructed.

	/**
	 * @brief Resolves the pooled object or raises a clear error.
	 *
	 * A setter calls this before constructing its event, so a write to an object
	 * that has left the pool raises in Python instead of vanishing silently.
	 * The pool owns the object, so the raw pointer stays valid after the
	 * temporary shared_ptr below is destroyed.
	 */
	const ObjectID* Resolve() const
	{
		const IResourceLookup* pool = (view != nullptr && view->ctx != nullptr) ? view->ctx->resources : nullptr;
		if (pool == nullptr)
		{
			throw std::runtime_error("neurus: object proxy has no resource lookup");
		}

		const std::shared_ptr<ObjectID> obj = pool->Get<ObjectID>(uid);
		if (!obj)
		{
			throw std::runtime_error("neurus: object proxy refers to a missing object");
		}
		return obj.get();
	}
};

/**
 * @brief Emits a scripted mutation through the proxy's run dispatch.
 *
 * The dispatch throws when scripting has no editor attached; the throw
 * propagates to Python so a script can handle it.
 *
 * @tparam TEvent A typed editor event.
 */
template <typename TEvent>
void EmitEvent(const ProxyBase& proxy, const TEvent& event)
{
	const ScriptDispatch* dispatch = proxy.view != nullptr ? proxy.view->dispatch : nullptr;
	if (dispatch == nullptr)
	{
		throw std::runtime_error("scripting is not attached to an editor");
	}
	dispatch->Emit(event);
}

}  // namespace script
}  // namespace neurus
