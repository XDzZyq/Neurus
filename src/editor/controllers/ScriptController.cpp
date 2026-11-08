/**
 * @file ScriptController.cpp
 * @brief Run-boundary handlers that open and close the one-step script scope.
 */

#include "editor/controllers/ScriptController.h"

#include "editor/events/ScriptEvents.h"

namespace {

/**
 * @brief Handles ScriptRunStarted: opens the buffering scope with the run's label.
 * @param e Run boundary carrying the host-chosen history label.
 * @param ctx Controller context carrying the operation sink.
 */
void OnScriptRunStarted(const neurus::ScriptRunStarted& e, const neurus::ControllerContext& ctx)
{
	ctx.ops.BeginScriptScope(e.name);
}

/**
 * @brief Handles ScriptRunFinished: commits everything the run recorded.
 * @param ctx Controller context carrying the operation sink.
 * @note Emitted even when the run raised, so the scope always closes and the
 *       preceding writes are still one undoable step.
 */
void OnScriptRunFinished(const neurus::ScriptRunFinished&, const neurus::ControllerContext& ctx)
{
	ctx.ops.EndScriptScope();
}

} // anonymous namespace

namespace neurus {

void ScriptController::Init(ControllerContext& ctx)
{
	// The context is captured BY VALUE so the stored handler lambdas are fully
	// self-contained, like the other controllers.
	ctx.events.Subscribe<ScriptRunStarted>(
		[ctx](const ScriptRunStarted& e) { OnScriptRunStarted(e, ctx); });
	ctx.events.Subscribe<ScriptRunFinished>(
		[ctx](const ScriptRunFinished& e) { OnScriptRunFinished(e, ctx); });
}

} // namespace neurus
