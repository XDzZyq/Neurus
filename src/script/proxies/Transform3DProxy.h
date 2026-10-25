/**
 * @file Transform3DProxy.h
 * @brief The `Transform3D` facet shared by every spatial scene-object proxy.
 *
 * Virtually inherits `ProxyBase`, so a proxy composed of this facet and another
 * facet still owns exactly one `{uid, view}` pair. The facet resolves only the
 * transform; object identity lives in `ObjectProxy`.
 */

#pragma once

#include <stdexcept>

#include "scene/ObjectID.h"
#include "scene/Scene.h"
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

	/// @brief Resolves the live `Transform3D` or raises a clear error.
	const Transform3D* Resolve() const
	{
		const Scene* scene = (view != nullptr && view->ctx != nullptr) ? view->ctx->scene : nullptr;
		if (scene == nullptr)
		{
			throw std::runtime_error("neurus: transform proxy has no run context");
		}

		ObjectID* obj = const_cast<ObjectID*>(scene->GetObjectID(uid));
		if (obj == nullptr)
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
