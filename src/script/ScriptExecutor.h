/**
 * @file ScriptExecutor.h
 * @brief Per-run execution state handed explicitly to the Python bindings.
 *
 * One `ScriptExecutor` is created for each script run and destroyed when the
 * run returns. It carries the read-only `ScriptContext` and the
 * `ScriptDispatch` that run may use. The bindings pass it to the proxies they
 * build, so there is no process-wide context or dispatcher, and a proxy never
 * stores the context itself - it reaches it through the executor on each access.
 *
 * @note Run-scoped: a proxy outlives its usefulness when the executor dies.
 */

#pragma once

#include "script/ScriptContext.h"
#include "script/ScriptDispatch.h"

namespace neurus
{

/**
 * @brief The context and dispatch of one script run.
 *
 * Non-owning: the caller keeps the referenced context and dispatch alive for
 * the run's duration. Never stored beyond the run.
 */
class ScriptExecutor
{
public:
	ScriptExecutor(const ScriptContext& context, const ScriptDispatch& dispatch)
		: m_context(&context)
		, m_dispatch(&dispatch)
	{}

	/// @brief The run's read-only scene/config view.
	const ScriptContext& context() const { return *m_context; }

	/// @brief The run's mutation funnel (refuses in this read-only phase).
	const ScriptDispatch& dispatch() const { return *m_dispatch; }

private:
	const ScriptContext*  m_context;
	const ScriptDispatch* m_dispatch;
};

}  // namespace neurus
