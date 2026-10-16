/**
 * @file ProxyBase.h
 * @brief Shared state of every Python proxy class.
 *
 * A proxy is a thin, copyable handle: the UID it stands for plus a pointer to
 * the per-run `ScriptExecutor` that built it. It stores no context of its own -
 * values are read live through the executor on every attribute access and are
 * never cached. A proxy is valid only while its executor lives, i.e. for the run.
 */

#pragma once

#include "script/ScriptExecutor.h"

namespace neurus
{
namespace script
{

/**
 * @brief Common members of every proxy.
 *
 * A UID is a generic pooled-object identity, not a scene-only concept.
 */
struct ProxyBase
{
	const ScriptExecutor* exec = nullptr;  ///< Executor of the run that built this proxy.
	int uid = 0;                           ///< Pooled object UID; 0 when unused.
};

/**
 * @brief Emits a scripted mutation through @p exec's dispatch.
 *
 * Refuses with `ReadOnlyError` when there is no run (or the dispatch has no
 * queue). @tparam TEvent A typed editor event.
 */
template <typename TEvent>
void EmitEvent(const ScriptExecutor* exec, const TEvent& event)
{
	if (exec == nullptr)
	{
		throw ReadOnlyError("Neurus scripting is read-only in this phase");
	}
	exec->dispatch().Emit(event);
}

/**
 * @brief Emits a scripted mutation through the proxy's run dispatch.
 *
 * Setters call this with the exact typed event they own.
 * @tparam TEvent A typed editor event.
 */
template <typename TEvent>
void EmitEvent(const ProxyBase& proxy, const TEvent& event)
{
	EmitEvent(proxy.exec, event);
}

}  // namespace script
}  // namespace neurus
