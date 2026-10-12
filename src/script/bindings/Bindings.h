/**
 * @file Bindings.h
 * @brief Entry points of the embedded `neurus` pybind11 module.
 *
 * This header intentionally exposes nothing of pybind11 beyond a forward
 * declaration, so it can be included without pulling Python into a translation
 * unit that does not already own a `pybind11::module_`.
 */

#pragma once

namespace pybind11
{
class module_;
}  // namespace pybind11

namespace neurus
{
namespace script
{

/**
 * @brief Populates the embedded `neurus` module with proxies and submodules.
 * @param m The module object created by `PYBIND11_EMBEDDED_MODULE(neurus, m)`.
 */
void RegisterNeurusModule(pybind11::module_& m);

/**
 * @brief Link anchor for the embedded-module translation unit.
 *
 * `Module.cpp` registers `neurus` through a static initializer that must run
 * before the interpreter starts. Because `neurus_script` is a static library,
 * the linker would otherwise drop the translation unit - and with it the
 * registration - since nothing references it. Calling this no-op from
 * `ScriptEngine::Impl::Init` before interpreter initialization pins it in.
 */
void ForceLinkBindings();

}  // namespace script
}  // namespace neurus
