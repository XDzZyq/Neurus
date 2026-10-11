#pragma once

#include "editor/controllers/ControllerContext.h"

namespace neurus {

class Controllers
{
public:
	virtual ~Controllers() = default;

	/**
	 * @brief Subscribes the controller to its events.
	 * @param ctx Controller context: event dispatch, pooled-object lookup and
	 *            the operation sink. Controllers must NOT retain the context or
	 *            any of its members; handler lambdas may capture it by reference.
	 */
	virtual void Init(ControllerContext& ctx) = 0;
};

} // namespace neurus
