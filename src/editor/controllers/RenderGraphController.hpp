#pragma once

#include "editor/controllers/Controllers.hpp"

namespace neurus {

/** Applies graph edits and submits them to the current operation scope. */
class RenderGraphController : public Controllers
{
public:
	void Init(ControllerContext& ctx) override;
};

} // namespace neurus
