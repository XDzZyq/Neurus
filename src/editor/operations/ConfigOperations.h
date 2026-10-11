/**
 * @file ConfigOperations.h
 * @brief Undoable operation for whole-RenderConfig edits.
 *
 * RenderConfig is scene-level (not per-object) state, so it does not fit the
 * UID-based TransitionOp: SetRenderConfigOp stores the absolute before/after
 * values and replays by dispatching the same event the UI emits.
 *
 * Config edits are deliberately NOT mergeable: continuous slider drags are
 * bounded by the controller's edit gesture instead, collapsing one drag to a
 * single op recorded on release.
 */

#pragma once

#include <memory>
#include <string>
#include <utility>

#include "editor/events/ConfigEvents.h"
#include "editor/operations/Operation.h"
#include "core/RenderConfig.h"

namespace neurus {

/**
 * @brief Absolute whole-RenderConfig edit (before -> after), non-mergeable.
 */
class SetRenderConfigOp : public Operation
{
public:
	SetRenderConfigOp() = default;

	SetRenderConfigOp(RenderConfig before, RenderConfig after)
		: m_before(std::move(before))
		, m_after(std::move(after))
	{}

	void Apply(OperationContext& ctx) override
	{
		ctx.events.emitNow(RenderConfigChangedEvent{ m_after });
	}

	std::unique_ptr<Operation> Inverse() const override
	{
		return std::make_unique<SetRenderConfigOp>(m_after, m_before);
	}

	std::string Label() const override { return "Render Config"; }

	/** @brief Serializes the before/after RenderConfig endpoints. */
	template<class Archive>
	void serialize(Archive& ar)
	{
		ar(cereal::make_nvp("before", m_before),
		   cereal::make_nvp("after", m_after));
	}

private:
	RenderConfig m_before;
	RenderConfig m_after;
};

} // namespace neurus
