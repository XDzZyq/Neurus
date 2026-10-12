/**
 * @file Runtime.h
 * @brief Process-wide slot for the context the Python bindings read.
 *
 * A proxy handed to Python stores only a UID and reaches back into live scene
 * data through a `ScriptContext`. That context is owned by `ScriptEngine`, but
 * the embedded module has no reference to the engine instance, so the engine
 * publishes the context here for the duration of a run and clears it
 * afterwards. All runs happen on the Qt main thread (design section 8), so a
 * plain pointer is sufficient - no synchronization is required.
 */

#pragma once

#include "script/ScriptContext.h"

namespace neurus
{
namespace script
{

/**
 * @brief Publishes the context the bindings read for the current run.
 * @param context Borrowed context, or nullptr to clear.
 * @note Passed pointers are borrowed: the caller must keep the referent alive
 *       until the next call. Every run clears the slot on exit.
 */
void SetCurrentContext(const ScriptContext* context);

/**
 * @brief The context of the run currently executing, or nullptr outside a run.
 * @return Borrowed pointer, valid only while a run is in flight.
 */
const ScriptContext* CurrentContext();

}  // namespace script
}  // namespace neurus
