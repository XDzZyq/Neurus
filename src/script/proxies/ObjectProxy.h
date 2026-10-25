/**
 * @file ObjectProxy.h
 * @brief The `Object` facet every scene-object proxy shares.
 *
 * `ObjectProxy` is one component of a proxy, not the whole proxy: it virtually
 * inherits `ProxyBase` so a proxy that also derives from another facet (for
 * example `Transform3DProxy`) still owns exactly one `{uid, view}` pair.
 */

#pragma once

#include <stdexcept>

#include "scene/ObjectID.h"
#include "scene/Scene.h"
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

	/// @brief Resolves the live object or raises a clear error.
	const ObjectID* Resolve() const
	{
		const Scene* scene = (view != nullptr && view->ctx != nullptr) ? view->ctx->scene : nullptr;
		if (scene == nullptr)
		{
			throw std::runtime_error("neurus: object proxy has no run context");
		}

		const ObjectID* obj = scene->GetObjectID(uid);
		if (obj == nullptr)
		{
			throw std::runtime_error("neurus: object proxy refers to a missing object");
		}
		return obj;
	}
};

}  // namespace script
}  // namespace neurus
