/**
 * @file Runtime.cpp
 * @brief Definition of the run-scoped context slot declared in `Runtime.h`.
 */

#include "script/bindings/Runtime.h"

namespace neurus
{
namespace script
{
namespace
{
/// The context of the run in flight, or nullptr outside a run.
const ScriptContext* g_currentContext = nullptr;
}  // namespace

void SetCurrentContext(const ScriptContext* context)
{
	g_currentContext = context;
}

const ScriptContext* CurrentContext()
{
	return g_currentContext;
}

}  // namespace script
}  // namespace neurus
