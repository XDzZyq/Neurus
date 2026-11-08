/**
 * @file ScriptController.h
 * @brief Run-boundary controller: bounds a script run to one undo step.
 *
 * A script run performs ordinary scene writes, and each write is recorded by its
 * controller as its own operation. This controller translates the run-boundary
 * events into the operation sink's script scope, so every write the run makes is
 * buffered and committed as a single history entry.
 *
 * Fully stateless: the two handlers are free functions in the .cpp, and the
 * scope state lives in the operation sink, not here.
 */

#pragma once

#include "editor/controllers/Controllers.h"

namespace neurus {

class ScriptController : public Controllers
{
public:
	/**
	 * @brief Constructs the controller. No stored references: everything is
	 *        reached through the ControllerContext passed to Init().
	 */
	ScriptController() = default;

	/**
	 * @brief Subscribes to the run-boundary events on the given context.
	 * @param ctx Controller context (event dispatch and the operation sink).
	 */
	void Init(ControllerContext& ctx) override;
};

} // namespace neurus
