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
 */

#include <pybind11/embed.h>

#include "script/bindings/Bindings.h"

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

}  // namespace script
}  // namespace neurus

PYBIND11_EMBEDDED_MODULE(neurus, m)
{
	neurus::script::RegisterNeurusModule(m);
}
