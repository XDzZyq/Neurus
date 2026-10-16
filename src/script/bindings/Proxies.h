/**
 * @file Proxies.h
 * @brief Shared read-only proxy handle types.
 *
 * Every proxy is an empty handle over a `ProxyBase` (a per-run `ScriptExecutor`
 * plus a UID); values are resolved live on attribute access, never cached.
 * Proxies that several binding translation units must materialise (an `Object`,
 * a `Transform`) live here so `neurus.data.objects` and `neurus.context.selection`
 * hand out the same Python classes.
 */

#pragma once

#include <stdexcept>

#include "scene/ObjectID.h"
#include "scene/Scene.h"
#include "scene/Transform.h"
#include "script/bindings/ProxyBase.h"

namespace neurus
{
namespace script
{

/// @brief Read-only proxy over one `ObjectID` in the master scene pool.
struct ObjectProxy : ProxyBase
{
	/// @brief Resolves the live object or raises a clear error.
	const ObjectID* Resolve() const
	{
		if (exec == nullptr || exec->context().scene == nullptr)
		{
			throw std::runtime_error("neurus: object proxy has no run context");
		}

		const ObjectID* obj = exec->context().scene->GetObjectID(uid);
		if (obj == nullptr)
		{
			throw std::runtime_error("neurus: object proxy refers to a missing object");
		}
		return obj;
	}
};

/**
 * @brief Read-only proxy over a scene object's `Transform3D`.
 *
 * `Scene::GetObjectID(int) const` hands out a const object while the component
 * accessor predates const-correctness; the cast below only ever feeds read-only
 * getters and mirrors the cast `Scene::UpdateObjTransforms` already performs.
 */
struct TransformProxy : ProxyBase
{
	/// @brief Resolves the live `Transform3D` or raises a clear error.
	const Transform3D* Resolve() const
	{
		if (exec == nullptr || exec->context().scene == nullptr)
		{
			throw std::runtime_error("neurus: transform proxy has no run context");
		}

		ObjectID* obj = const_cast<ObjectID*>(exec->context().scene->GetObjectID(uid));
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
