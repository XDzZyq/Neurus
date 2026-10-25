/**
 * @file Bindings.h
 * @brief Entry points of the embedded `neurus` pybind11 module.
 *
 * This header exposes nothing of pybind11 beyond a forward declaration, so it
 * can be included without pulling Python into a translation unit that does not
 * already own a `pybind11::module_`. Classes are registered once; the per-run
 * views are installed afterwards with the run's `ScriptView`, which each proxy
 * copies and resolves through itself.
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

struct ScriptView;

/**
 * @brief Registers the `neurus` module's classes and submodules. Called once.
 * @param m The module object created by `PYBIND11_EMBEDDED_MODULE(neurus, m)`.
 */
void RegisterNeurusModule(pybind11::module_& m);

/**
 * @brief Creates the module's view instances for the interpreter's lifetime.
 *
 * Called once from `ScriptEngine::Impl::Init` after the classes are registered.
 * The proxies these views hand out copy @p view, so the engine owns the only
 * stable reference and can publish/clear the document per run.
 *
 * @param m The `neurus` module.
 * @param view Engine-owned view; must outlive the interpreter.
 */
void InstallNeurusViews(pybind11::module_& m, const ScriptView* view);

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

// --- Per-file registration and view installation ----------------------------

void RegisterObjectBindings(pybind11::module_& data);
void InstallObjectViews(pybind11::module_& data, const ScriptView* view);

void RegisterSceneBindings(pybind11::module_& root, pybind11::module_& data);
void InstallSceneViews(pybind11::module_& root, pybind11::module_& data, const ScriptView* view);

void RegisterConfigBindings(pybind11::module_& data);
void InstallConfigViews(pybind11::module_& data, const ScriptView* view);

}  // namespace script
}  // namespace neurus
