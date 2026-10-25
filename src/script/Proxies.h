/**
 * @file Proxies.h
 * @brief Shared read-only proxy handle types.
 *
 * Every proxy is an identity handle carrying the UID it stands for and the
 * `ScriptView` of the run that constructed it. Resolution is per proxy: each
 * type reads its own `view->ctx` and looks the object up live. Nothing is
 * cached, and no proxy reads a finished run.
 */

#pragma once

#include <stdexcept>

#include "scene/ObjectID.h"
#include "scene/Scene.h"
#include "scene/Transform.h"
#include "script/ScriptContext.h"
#include "script/ProxyBase.h"

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
