/**
 * @file SceneProxies.h
 * @brief Typed read-only proxies over the pooled scene objects.
 *
 * `CameraProxy`, `LightProxy`, `MeshProxy` and `EnvironmentProxy` each compose
 * the shared `ObjectProxy` and `Transform3DProxy` facets with a typed resolve
 * through the run's resource pool. A proxy stores only its UID and the
 * `ScriptView` of the run that constructed it: nothing is cached, nothing is
 * owned, and a proxy outlives its run without dangling. Like every header in
 * `proxies/`, this one is Python-free.
 */

#pragma once

#include <memory>
#include <stdexcept>
#include <string>

#include "core/IResourceLookup.h"
#include "scene/Camera.h"
#include "scene/Environment.h"
#include "scene/Light.h"
#include "scene/Mesh.h"
#include "script/ScriptContext.h"
#include "script/ScriptView.h"
#include "script/proxies/ObjectProxy.h"
#include "script/proxies/ProxyBase.h"
#include "script/proxies/Transform3DProxy.h"

namespace neurus
{
namespace script
{

/// @brief The run's resource pool, or nullptr when the run has no document.
inline const IResourceLookup* PoolOf(const ScriptView* view)
{
	return (view != nullptr && view->ctx != nullptr) ? view->ctx->resources : nullptr;
}

/**
 * @brief Resolves a pooled object of dynamic type @p T for a typed proxy.
 *
 * The pool owns the object, so the returned raw pointer stays valid after the
 * temporary shared_ptr is destroyed. Throws a per-proxy error when the run has
 * no pool, or the UID is absent from it or of another type.
 *
 * @tparam T Concrete pooled type (Camera, Light, Mesh, Environment).
 * @param view The proxy's run view.
 * @param uid Pooled object UID.
 * @param what Lower-case proxy type name used in the error message.
 */
template <typename T>
const T* ResolvePooled(const ScriptView* view, int uid, const char* what)
{
	const IResourceLookup* pool = PoolOf(view);
	if (pool == nullptr)
	{
		throw std::runtime_error(std::string("neurus: ") + what + " proxy has no resource lookup");
	}

	const std::shared_ptr<T> obj = pool->Get<T>(uid);
	if (!obj)
	{
		throw std::runtime_error(std::string("neurus: ") + what + " proxy refers to a missing or mistyped object");
	}
	return obj.get();
}

struct CameraProxy : ObjectProxy, Transform3DProxy
{
	CameraProxy() = default;

	CameraProxy(int id, const ScriptView* scriptView)
		: ProxyBase{id, scriptView}
	{}

	/// @brief Resolves the live camera or raises a clear error.
	const Camera* Resolve() const
	{
		return ResolvePooled<Camera>(view, uid, "camera");
	}
};

struct LightProxy : ObjectProxy, Transform3DProxy
{
	LightProxy() = default;

	LightProxy(int id, const ScriptView* scriptView)
		: ProxyBase{id, scriptView}
	{}

	/// @brief Resolves the live light or raises a clear error.
	const Light* Resolve() const
	{
		return ResolvePooled<Light>(view, uid, "light");
	}
};

struct MeshProxy : ObjectProxy, Transform3DProxy
{
	MeshProxy() = default;

	MeshProxy(int id, const ScriptView* scriptView)
		: ProxyBase{id, scriptView}
	{}

	/// @brief Resolves the live mesh or raises a clear error.
	const Mesh* Resolve() const
	{
		return ResolvePooled<Mesh>(view, uid, "mesh");
	}
};

struct EnvironmentProxy : ObjectProxy, Transform3DProxy
{
	EnvironmentProxy() = default;

	EnvironmentProxy(int id, const ScriptView* scriptView)
		: ProxyBase{id, scriptView}
	{}

	/// @brief Resolves the live environment or raises a clear error.
	const Environment* Resolve() const
	{
		return ResolvePooled<Environment>(view, uid, "environment");
	}
};

}  // namespace script
}  // namespace neurus
