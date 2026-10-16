/**
 * @file Bindings.h
 * @brief Entry points of the embedded `neurus` pybind11 module.
 *
 * This header exposes nothing of pybind11 beyond a forward declaration, so it
 * can be included without pulling Python into a translation unit that does not
 * already own a `pybind11::module_`. The per-run views are re-bound at the start
 * of each run through `InstallNeurusViews`, which is why a proxy never stores a
 * context of its own.
 */

#pragma once

namespace pybind11
{
class module_;
}  // namespace pybind11

namespace neurus
{
class ScriptExecutor;
}

namespace neurus
{
namespace script
{

/**
 * @brief Registers the `neurus` module's classes and submodules. Called once.
 * @param m The module object created by `PYBIND11_EMBEDDED_MODULE(neurus, m)`.
 */
void RegisterNeurusModule(pybind11::module_& m);

/**
 * @brief Binds the per-run views (collections, context, config) to @p exec.
 *
 * Called at the start of every run; the previous run's views are replaced, so
 * nothing reads a finished run's executor.
 *
 * @param m The `neurus` module.
 * @param exec Executor of the run about to execute; must outlive the run.
 */
void InstallNeurusViews(pybind11::module_& m, const ScriptExecutor* exec);

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

// --- Per-file registration / view installation -----------------------------

void RegisterObjectBindings(pybind11::module_& data);
void InstallObjectViews(pybind11::module_& data, const ScriptExecutor* exec);

void RegisterSceneBindings(pybind11::module_& root, pybind11::module_& data);
void InstallSceneViews(pybind11::module_& root, pybind11::module_& data, const ScriptExecutor* exec);

void RegisterConfigBindings(pybind11::module_& data);
void InstallConfigViews(pybind11::module_& data, const ScriptExecutor* exec);

}  // namespace script
}  // namespace neurus
