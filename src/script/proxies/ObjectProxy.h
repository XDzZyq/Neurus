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

#include "script/proxies/ProxyBase.h"

namespace neurus
{
namespace script
{

/// @brief "Object identity" facet: name, type, visibility, transform.
struct ObjectProxy : virtual ProxyBase
{
	ObjectProxy() = default;

	/// @brief Binds the facet to an object and the run that constructed it.
	ObjectProxy(int id, const ScriptView* scriptView)
		: ProxyBase{id, scriptView}
	{}
};

}  // namespace script
}  // namespace neurus
