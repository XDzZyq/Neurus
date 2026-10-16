/**
 * @file Module.cpp
 * @brief Registration of the embedded `neurus` module and its link anchor.
 *
 * `PYBIND11_EMBEDDED_MODULE` installs a static registrar that calls
 * `PyImport_AppendInittab`; pybind11 fails the registration when the
 * interpreter is already running, so it must run before `Py_Initialize*`.
 * A static initializer in a static library is dropped together with its
 * translation unit, so `ScriptEngine::Impl::Init` references
 * `ForceLinkBindings()` to keep this unit in the link.
 *
 * Class registration happens once here; the per-run views (collections,
 * context, config) are re-bound every run by `InstallNeurusViews`.
 */

#include <pybind11/embed.h>

#include "script/ScriptExecutor.h"
#include "script/bindings/Bindings.h"

namespace py = pybind11;

namespace neurus
{
namespace script
{

void ForceLinkBindings()
{
	// Intentionally empty: existing is the point. Referencing this symbol from
	// the engine forces the linker to keep this translation unit - and with it
	// the static embedded-module registrar below - in the binary.
}

void RegisterNeurusModule(py::module_& m)
{
	m.doc() = "Neurus scripting API (read-only phase 1).";

	py::module_ data = m.def_submodule("data", "Read-only scene data collections.");
	RegisterObjectBindings(data);
	RegisterSceneBindings(m, data);
	RegisterConfigBindings(data);

	// Scripted mutations are refused in this phase.
	py::register_exception<ReadOnlyError>(m, "ReadOnlyError", PyExc_RuntimeError);
}

void InstallNeurusViews(py::module_& m, const ScriptExecutor* exec)
{
	py::module_ data = m.attr("data");
	InstallObjectViews(data, exec);
	InstallSceneViews(m, data, exec);
	InstallConfigViews(data, exec);
}

}  // namespace script
}  // namespace neurus

PYBIND11_EMBEDDED_MODULE(neurus, m)
{
	neurus::script::RegisterNeurusModule(m);
}
