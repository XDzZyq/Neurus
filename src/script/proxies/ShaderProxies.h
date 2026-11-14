/**
 * @file ShaderProxies.h
 * @brief Python-facing shader proxies: the mesh slot, a pooled shader, a unit.
 *
 * These are Python-free value types. Each stores only the run's ScriptView and
 * a UID, and resolves the live ShaderData through the resource pool on every
 * access. A ShaderUnit is a stage view inside a pooled ShaderData; it is never
 * pooled itself.
 */

#pragma once

#include <stdexcept>
#include <string>

#include "asset/data/shader/ShaderData.h"
#include "asset/data/shader/ShaderUnit.h"
#include "core/IResourceLookup.h"
#include "scene/Mesh.h"
#include "script/ScriptContext.h"
#include "script/ScriptView.h"
#include "script/proxies/ProxyBase.h"

namespace neurus
{
namespace script
{

/// @brief The run's resource pool, or nullptr when the run has no document.
inline const IResourceLookup* ShaderPool(const ScriptView* view)
{
	return (view != nullptr && view->ctx != nullptr) ? view->ctx->resources : nullptr;
}

/**
 * @brief Resolves a pooled ShaderData, or throws a clear error.
 * @param view The proxy's run view.
 * @param uid Pooled shader UID.
 * @param what Lower-case proxy name used in the error message.
 */
inline const ShaderData* ResolveShader(const ScriptView* view, int uid, const char* what)
{
	const IResourceLookup* pool = ShaderPool(view);
	if (pool == nullptr)
	{
		throw std::runtime_error(std::string("neurus: ") + what + " proxy has no resource lookup");
	}

	const std::shared_ptr<ShaderData> shader = pool->Get<ShaderData>(uid);
	if (!shader)
	{
		throw std::runtime_error(std::string("neurus: ") + what + " proxy refers to a missing shader");
	}
	return shader.get();
}

/// @brief The mesh's shader slot: create/remove and read the attached stages.
struct ShaderSlotProxy : ProxyBase
{
	ShaderSlotProxy() = default;
	ShaderSlotProxy(int meshUid, const ScriptView* view) : ProxyBase{meshUid, view} {}

	/// @brief Resolves the live mesh or raises a clear error.
	const Mesh* ResolveMesh() const
	{
		const IResourceLookup* pool = ShaderPool(view);
		if (pool == nullptr)
		{
			throw std::runtime_error("neurus: shader slot has no resource lookup");
		}
		const std::shared_ptr<Mesh> mesh = pool->Get<Mesh>(uid);
		if (!mesh)
		{
			throw std::runtime_error("neurus: shader slot refers to a missing mesh");
		}
		return mesh.get();
	}

	/// @brief True when the mesh currently has a shader.
	bool Attached() const { return ResolveMesh()->o_shaderId != 0; }

	/// @brief The attached shader's pooled UID, or 0.
	int ShaderUid() const { return ResolveMesh()->o_shaderId; }
};

/// @brief A pooled RenderShader or ComputeShader bundle.
struct ShaderProxy : ProxyBase
{
	ShaderProxy() = default;
	ShaderProxy(int shaderUid, const ScriptView* view) : ProxyBase{shaderUid, view} {}

	/// @brief Resolves the live shader or raises a clear error.
	const ShaderData* Resolve() const { return ResolveShader(view, uid, "shader"); }
};

/// @brief A view into one ShaderUnit (stage) of a pooled ShaderData.
struct ShaderUnitProxy : ProxyBase
{
	int stage = 0;  ///< ShaderType as int.

	ShaderUnitProxy() = default;
	ShaderUnitProxy(int shaderUid, const ScriptView* view, int shaderStage)
		: ProxyBase{shaderUid, view}
		, stage(shaderStage)
	{}

	/// @brief Resolves the live stage unit or raises a clear error.
	const ShaderUnit* ResolveUnit() const
	{
		const ShaderData* shader = ResolveShader(view, uid, "shader unit");
		const ShaderUnit* unit = shader->FindStage(static_cast<ShaderType>(stage));
		if (!unit)
		{
			throw std::runtime_error("neurus: shader stage is not present");
		}
		return unit;
	}
};

}  // namespace script
}  // namespace neurus
