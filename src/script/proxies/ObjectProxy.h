/**
 * @file ObjectProxy.h
 * @brief The `Object` facet every scene-object proxy shares.
 *
 * `ObjectProxy` is one component of a proxy, not the whole proxy: it virtually
 * inherits `ProxyBase` so a proxy that also derives from another facet (for
 * example `Transform3DProxy`) still owns exactly one `{uid, view}` pair. The
 * facet resolves through the run's resource pool, not scene membership, so a
 * pooled object stays readable while detached from the scene.
 */

#pragma once

#include <memory>
#include <stdexcept>

#include "core/IResourceLookup.h"
#include "scene/ObjectID.h"
#include "script/proxies/ProxyBase.h"
#include "script/ScriptContext.h"

namespace neurus
{
namespace script
{

/// @brief Read-only "object identity" facet: name, type, visibility, transform.
struct ObjectProxy : virtual ProxyBase
{
	ObjectProxy() = default;

	/// @brief Binds the facet to an object and the run that constructed it.
	ObjectProxy(int id, const ScriptView* scriptView)
		: ProxyBase{id, scriptView}
	{}

	/**
	 * @brief Resolves the pooled object or raises a clear error.
	 *
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

}  // namespace script
}  // namespace neurus
