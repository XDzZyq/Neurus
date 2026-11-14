/**
 * @file ShaderBindings.h
 * @brief Bindings that attach shader properties to the existing Mesh class.
 *
 * The shader module itself is registered by Bindings.h's RegisterShaderBindings
 * entry point; this header only exposes the Mesh-side property registration so
 * DataBindings.cpp can add it while it defines the Mesh class.
 */

#pragma once

#include <pybind11/pybind11.h>

#include "script/proxies/SceneProxies.h"

namespace neurus
{
namespace script
{

/**
 * @brief Adds the `shader` slot property to the Mesh proxy class.
 * @param cls The Mesh class registered by RegisterDataBindings.
 */
void RegisterMeshShaderBindings(
	pybind11::class_<MeshProxy, ObjectProxy, Transform3DProxy>& cls);

}  // namespace script
}  // namespace neurus
