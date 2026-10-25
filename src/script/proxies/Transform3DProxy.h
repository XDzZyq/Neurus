/**
 * @file Transform3DProxy.h
 * @brief The `Transform3D` facet shared by every spatial scene-object proxy.
 *
 * Virtually inherits `ProxyBase`, so a proxy composed of this facet and another
 * facet still owns exactly one `{uid, view}` pair. The facet resolves only the
 * transform; object identity lives in `ObjectProxy`. Resolution goes through
 * the run's resource pool, not scene membership.
 */

#pragma once

#include <memory>
#include <stdexcept>

#include "core/IResourceLookup.h"
#include "scene/ObjectID.h"
#include "scene/Transform.h"
#include "script/proxies/ProxyBase.h"
#include "script/ScriptContext.h"

namespace neurus
{
namespace script
{

/// @brief Read-only 3D transform facet: position, rotation and scale.
struct Transform3DProxy : virtual ProxyBase
{
	Transform3DProxy() = default;

	/// @brief Binds the facet to an object and the run that constructed it.
	Transform3DProxy(int id, const ScriptView* scriptView)
		: ProxyBase{id, scriptView}
	{}

	/**
	 * @brief Resolves the live `Transform3D` or raises a clear error.
	 *
	 * The pool owns the object, so the returned pointer stays valid after the
	 * temporary shared_ptr below is destroyed.
	 */
	const Transform3D* Resolve() const
	{
		const IResourceLookup* pool = (view != nullptr && view->ctx != nullptr) ? view->ctx->resources : nullptr;
		if (pool == nullptr)
		{
			throw std::runtime_error("neurus: transform proxy has no resource lookup");
		}

		const std::shared_ptr<ObjectID> obj = pool->Get<ObjectID>(uid);
		if (!obj)
		{
			throw std::runtime_error("neurus: transform proxy refers to a missing object");
		}

		auto* transform = static_cast<Transform3D*>(obj->GetTransform());
		if (transform == nullptr)
		{
			throw std::runtime_error("neurus: object has no 3D transform");
		}
		return transform;
	}
};

}  // namespace script
}  // namespace neurus
